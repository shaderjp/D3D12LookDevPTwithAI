#include "stdafx.h"
#include "D3D12PathTracingBackend.h"
#include "DenoiseSettingsJson.h"
#include "PbrtSceneImporter.h"
#include "ProjectPath.h"

#include "TextureLoader.h"
#include "TransientResourceAllocator.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <commdlg.h>
#include <wincred.h>
#include <cstddef>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
    constexpr const wchar_t* McpTokenCredentialTarget = L"D3D12LookDevPTWinUI/MCPBearerToken";

    bool WriteMcpTokenCredential(const std::string& token)
    {
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(McpTokenCredentialTarget);
        credential.CredentialBlobSize = static_cast<DWORD>(token.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.UserName = const_cast<LPWSTR>(L"D3D12LookDevPTWinUI");
        return CredWriteW(&credential, 0) != FALSE;
    }

    std::string ReadMcpTokenCredential()
    {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(McpTokenCredentialTarget, CRED_TYPE_GENERIC, 0, &credential) || !credential) return {};
        std::string token;
        if (credential->CredentialBlob && credential->CredentialBlobSize > 0)
            token.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        CredFree(credential);
        return token;
    }

    std::wstring ExecutableDirectory()
    {
        std::array<wchar_t, 32768> path = {};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return std::filesystem::current_path().wstring();
        }
        return std::filesystem::path(path.data()).parent_path().wstring();
    }

    constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT ReferenceAccumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr DXGI_FORMAT SignalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT RestirReservoirStride = 24u; // sizeof(RTXDI_PackedDIReservoir)
    constexpr UINT RestirGiReservoirStride = 32u; // sizeof(RTXDI_PackedGIReservoir)
    constexpr UINT RestirPtReservoirStride = 64u; // sizeof(RTXDI_PackedPTReservoir)

    // Mirrors RayPayload/ShadowPayload in PathTracingABI.hlsli. These host-side
    // definitions turn accidental HLSL ABI growth into an explicit state-object
    // edit instead of silently restoring the old 192-byte allocation.
    struct PathPayloadAbi
    {
        XMFLOAT2 barycentrics;
        float hitT;
        float rayConeWidth;
        float rayConeSpread;
        UINT instanceIndex;
        UINT geometryIndex;
        UINT primitiveIndex;
    };
    struct ShadowPayloadAbi
    {
        UINT occluded;
        float rayConeWidth;
        float rayConeSpread;
        XMFLOAT3 transmittance;
    };
    static_assert(sizeof(PathPayloadAbi) == 32u);
    static_assert(offsetof(PathPayloadAbi, hitT) == 8u);
    static_assert(offsetof(PathPayloadAbi, primitiveIndex) == 28u);
    static_assert(sizeof(ShadowPayloadAbi) == 24u);
    static_assert(offsetof(ShadowPayloadAbi, rayConeSpread) == 8u);
    static_assert(offsetof(ShadowPayloadAbi, transmittance) == 12u);

    const wchar_t* RayGenShaderName = L"RayGen";
    const wchar_t* PrimaryRayGenShaderName = L"PrimaryRayGen";
    const wchar_t* SecondaryRayGenShaderName = L"SecondaryRayGen";
    const wchar_t* ReviewProbeRayGenShaderName = L"ReviewProbeRayGen";
    const wchar_t* MissShaderName = L"Miss";
    const wchar_t* ShadowMissShaderName = L"ShadowMiss";
    const wchar_t* ClosestHitShaderName = L"ClosestHit";
    const wchar_t* AnyHitShaderName = L"AnyHit";
    const wchar_t* ShadowAnyHitShaderName = L"ShadowAnyHit";
    const wchar_t* HitGroupName = L"HitGroup";
    const wchar_t* ShadowHitGroupName = L"ShadowHitGroup";

    XMFLOAT3 NormalizeFloat3(const float values[3])
    {
        XMVECTOR v = XMVector3Normalize(XMVectorSet(values[0], values[1], values[2], 0.0f));
        XMFLOAT3 result;
        XMStoreFloat3(&result, v);
        return result;
    }

    std::wstring PathtracingTierName(D3D12_RAYTRACING_TIER tier)
    {
        switch (static_cast<int>(tier))
        {
        case 10: return L"1.0";
        case 11: return L"1.1";
        case 12: return L"1.2";
        default:
            return tier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? L"Not supported" : L"Unknown";
        }
    }

    bool UsesRestirReuse(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIR || mode == PathTracingMode::ReSTIRDI ||
            mode == PathTracingMode::ReSTIRCombined || mode == PathTracingMode::ReSTIRPT ||
            mode == PathTracingMode::ReSTIRPTCombined;
    }

    bool UsesRestirGI(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIR || mode == PathTracingMode::ReSTIRCombined;
    }

    bool UsesRestirPT(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIRPT || mode == PathTracingMode::ReSTIRPTCombined;
    }

    bool UsesRestirDI(PathTracingMode mode)
    {
        return mode == PathTracingMode::ReSTIRDI || mode == PathTracingMode::ReSTIRCombined ||
            mode == PathTracingMode::ReSTIRPTCombined;
    }

    constexpr const char* RenderModeLabels[] =
    {
        "Baseline PT", "ReSTIR GI", "ReSTIR DI", "ReSTIR GI + DI", "ReSTIR PT", "ReSTIR PT + DI"
    };
    constexpr const char* RenderModeValues[] =
    {
        "baseline", "restir_gi", "restir_di", "restir_gi_di", "restir_pt", "restir_pt_di"
    };
    constexpr const char* TextureSlotLabels[] = {
        "Base Color", "Normal", "Roughness", "Metallic", "Occlusion", "Emissive", "Alpha",
        "Specular Color", "Specular Factor", "Transmission", "Thickness", "Clearcoat",
        "Clearcoat Roughness", "Clearcoat Normal" };
    constexpr const char* TextureSlotKeys[] = {
        "baseColor", "normal", "roughness", "metallic", "occlusion", "emissive", "alpha",
        "specularColor", "specularFactor", "transmission", "thickness", "clearcoat",
        "clearcoatRoughness", "clearcoatNormal" };
    static_assert(_countof(TextureSlotLabels) == Bistro::TextureSlotCount);
    static_assert(_countof(TextureSlotKeys) == Bistro::TextureSlotCount);
    std::string WideToUtf8(const std::wstring& text);
    constexpr const char* MaterialFocusLabels[] = { "Normal", "Isolate", "Dim" };
    constexpr const char* ToneMapperLabels[] = { "None", "Reinhard", "ACES" };
    constexpr const char* DebugViewLabels[] =
    {
        "Final", "Base Color", "World Normal", "Normal Texture", "Roughness", "Metallic", "Emissive",
        "Hit Distance", "Direct NEE", "Indirect", "Bounce Count", "Accumulation Samples", "Sky",
        "Reservoir Weight", "Temporal Reuse", "Spatial Reuse", "Current Radiance", "Temporal Output",
        "History Length", "Variance", "Motion Vector", "Disocclusion Mask", "Denoised Indirect",
        "Denoise Delta", "Reservoir Age", "Reservoir Validity", "GI Reservoir Weight",
        "DI Reservoir Weight", "GI Temporal Reuse", "DI Temporal Reuse", "GI Spatial Reuse",
        "DI Spatial Reuse", "Diffuse Signal", "Specular Signal", "Emission / Sky", "Temporal Input",
        "Temporal Output Detail", "A-Trous Output", "Reactive Mask", "History Match", "History Confidence",
        "NRD World Normal", "NRD Roughness", "NRD Linear View-Z", "NRD 2.5D Motion",
        "NRD Disocclusion / History", "NRD Input Validation", "TAA History Length",
        "TAA History Acceptance", "GI Weight", "GI Age", "GI Validity",
        "GI Temporal Acceptance", "GI Initial vs Resampled", "Specular F0", "Transmission",
        "Thickness / Attenuation", "Clearcoat", "UV Set"
    };
    constexpr int DebugViewMax = static_cast<int>(_countof(DebugViewLabels)) - 1;

    std::string KeyFromLabel(const std::string& label)
    {
        std::string key;
        key.reserve(label.size());
        bool lastUnderscore = false;
        for (const unsigned char ch : label)
        {
            if (std::isalnum(ch))
            {
                key.push_back(static_cast<char>(std::tolower(ch)));
                lastUnderscore = false;
            }
            else if (!lastUnderscore)
            {
                key.push_back('_');
                lastUnderscore = true;
            }
        }
        while (!key.empty() && key.back() == '_')
        {
            key.pop_back();
        }
        return key;
    }

    std::string BuildDebugViewsJson()
    {
        std::ostringstream out;
        out << "{\"debugViews\":[";
        for (int i = 0; i < static_cast<int>(_countof(DebugViewLabels)); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            const std::string label = DebugViewLabels[i];
            out << "{\"id\":" << i << ",\"label\":\"" << cld::EscapeJson(label) << "\",\"key\":\"" << KeyFromLabel(label) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    std::string BuildRenderModesJson()
    {
        std::ostringstream out;
        out << "{\"renderModes\":[";
        for (int i = 0; i < static_cast<int>(_countof(RenderModeLabels)); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            out << "{\"id\":" << i << ",\"label\":\"" << RenderModeLabels[i] << "\",\"value\":\"" << RenderModeValues[i] << "\"}";
        }
        out << "]}";
        return out.str();
    }

    const char* TextureSamplerName(uint32_t sampler)
    {
        static constexpr const char* Names[] = {
            "linearRepeat", "linearClamp", "linearMirror",
            "nearestRepeat", "nearestClamp", "nearestMirror" };
        return Names[(std::min)(sampler, static_cast<uint32_t>(_countof(Names) - 1))];
    }

    const char* TextureResolutionPolicyName(uint32_t policy)
    {
        static constexpr const char* Names[] = { "auto", "source", "4k", "2k", "1k", "512" };
        return Names[(std::min)(policy, static_cast<uint32_t>(_countof(Names) - 1))];
    }

    uint32_t TextureSamplerFromName(const std::string& name, uint32_t fallback)
    {
        static constexpr const char* Names[] = {
            "linearRepeat", "linearClamp", "linearMirror",
            "nearestRepeat", "nearestClamp", "nearestMirror" };
        for (uint32_t i = 0; i < _countof(Names); ++i)
        {
            if (name == Names[i]) return i;
        }
        return fallback;
    }

    uint32_t TextureResolutionPolicyFromName(const std::string& name, uint32_t fallback)
    {
        static constexpr const char* Names[] = { "auto", "source", "4k", "2k", "1k", "512" };
        for (uint32_t i = 0; i < _countof(Names); ++i)
        {
            if (name == Names[i]) return i;
        }
        return fallback;
    }

    template<typename MaterialLike>
    void WriteGltfExtensionsJson(std::ostream& out, const MaterialLike& material)
    {
        out << "{\"featureMask\":" << material.extensionFeatureMask
            << ",\"specularFactor\":" << material.specularFactor
            << ",\"specularColorFactor\":[" << material.specularColorFactor.x << "," << material.specularColorFactor.y << "," << material.specularColorFactor.z << "]"
            << ",\"ior\":" << material.indexOfRefraction
            << ",\"transmissionFactor\":" << material.transmissionFactor
            << ",\"thicknessFactor\":" << material.thicknessFactor
            << ",\"attenuationColor\":[" << material.attenuationColor.x << "," << material.attenuationColor.y << "," << material.attenuationColor.z << "]"
            << ",\"attenuationDistance\":" << material.attenuationDistance
            << ",\"clearcoatFactor\":" << material.clearcoatFactor
            << ",\"clearcoatRoughnessFactor\":" << material.clearcoatRoughnessFactor
            << ",\"clearcoatNormalScale\":" << material.clearcoatNormalScale << "}";
    }

    template<typename MaterialLike>
    void WriteTextureBindingsJson(std::ostream& out, const MaterialLike& material,
        const std::array<bool, Bistro::TextureSlotCount>& overrideEnabled)
    {
        out << "{";
        for (UINT slot = 0; slot < Bistro::TextureSlotCount; ++slot)
        {
            if (slot > 0) out << ",";
            const Bistro::TextureBinding& binding = material.textureBindings[slot];
            out << "\"" << TextureSlotKeys[slot] << "\":{" 
                << "\"path\":\"" << cld::EscapeJson(WideToUtf8(material.textures[slot])) << "\""
                << ",\"uvSet\":" << binding.texCoord
                << ",\"offset\":[" << binding.offset.x << "," << binding.offset.y << "]"
                << ",\"scale\":[" << binding.scale.x << "," << binding.scale.y << "]"
                << ",\"rotation\":" << binding.rotation
                << ",\"sampler\":\"" << TextureSamplerName(binding.samplerIndex) << "\""
                << ",\"resolutionPolicy\":\"" << TextureResolutionPolicyName(binding.resolutionPolicy) << "\""
                << ",\"overridden\":" << (overrideEnabled[slot] ? "true" : "false") << "}";
        }
        out << "}";
    }

    template<typename MaterialLike>
    void ReadGltfExtensionsJson(const cld::JsonValue& owner, MaterialLike& material)
    {
        const cld::JsonValue* gltf = cld::FindMember(owner, "gltfExtensions");
        if (!gltf || gltf->type != cld::JsonValue::Type::Object) return;
        const double featureMask = cld::JsonNumberOr(*gltf, "featureMask", material.extensionFeatureMask);
        material.extensionFeatureMask = static_cast<uint32_t>(std::clamp(
            featureMask,
            0.0,
            static_cast<double>(std::numeric_limits<uint32_t>::max())));
        material.specularFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "specularFactor", material.specularFactor)), 0.0f, 2.0f);
        const std::array<float, 3> specular = cld::JsonFloat3Or(*gltf, "specularColorFactor", { material.specularColorFactor.x, material.specularColorFactor.y, material.specularColorFactor.z });
        material.specularColorFactor = XMFLOAT3(specular[0], specular[1], specular[2]);
        material.indexOfRefraction = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "ior", material.indexOfRefraction)), 1.0f, 3.0f);
        material.transmissionFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "transmissionFactor", material.transmissionFactor)), 0.0f, 1.0f);
        material.thicknessFactor = (std::max)(0.0f, static_cast<float>(cld::JsonNumberOr(*gltf, "thicknessFactor", material.thicknessFactor)));
        const std::array<float, 3> attenuation = cld::JsonFloat3Or(*gltf, "attenuationColor", { material.attenuationColor.x, material.attenuationColor.y, material.attenuationColor.z });
        material.attenuationColor = XMFLOAT3(attenuation[0], attenuation[1], attenuation[2]);
        material.attenuationDistance = (std::max)(0.0f, static_cast<float>(cld::JsonNumberOr(*gltf, "attenuationDistance", material.attenuationDistance)));
        material.clearcoatFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "clearcoatFactor", material.clearcoatFactor)), 0.0f, 1.0f);
        material.clearcoatRoughnessFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "clearcoatRoughnessFactor", material.clearcoatRoughnessFactor)), 0.0f, 1.0f);
        material.clearcoatNormalScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*gltf, "clearcoatNormalScale", material.clearcoatNormalScale)), 0.0f, 2.0f);
    }

    XMFLOAT2 JsonFloat2Or(const cld::JsonValue& owner, const char* name, const XMFLOAT2& fallback)
    {
        const cld::JsonValue* value = cld::FindMember(owner, name);
        if (!value || value->type != cld::JsonValue::Type::Array || value->array.size() != 2 ||
            value->array[0].type != cld::JsonValue::Type::Number || value->array[1].type != cld::JsonValue::Type::Number)
        {
            return fallback;
        }
        return XMFLOAT2(static_cast<float>(value->array[0].number), static_cast<float>(value->array[1].number));
    }

    bool TryParseDebugView(const cld::JsonValue& value, int& debugView)
    {
        if (value.type == cld::JsonValue::Type::Number)
        {
            const int index = static_cast<int>(value.number);
            if (index >= 0 && index < static_cast<int>(_countof(DebugViewLabels)))
            {
                debugView = index;
                return true;
            }
            return false;
        }
        if (value.type != cld::JsonValue::Type::String)
        {
            return false;
        }
        const std::string key = KeyFromLabel(value.string);
        for (int i = 0; i < static_cast<int>(_countof(DebugViewLabels)); ++i)
        {
            if (value.string == DebugViewLabels[i] || key == KeyFromLabel(DebugViewLabels[i]))
            {
                debugView = i;
                return true;
            }
        }
        return false;
    }

    float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f;
        float result = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }

    const char* PathtracingModeName(PathTracingMode mode)
    {
        switch (mode)
        {
        case PathTracingMode::ReSTIR:
            return "ReSTIR GI";
        case PathTracingMode::ReSTIRDI:
            return "ReSTIR DI";
        case PathTracingMode::ReSTIRCombined:
            return "ReSTIR GI + DI";
        case PathTracingMode::ReSTIRPT:
            return "ReSTIR PT";
        case PathTracingMode::ReSTIRPTCombined:
            return "ReSTIR PT + DI";
        default:
            return "Path Tracing";
        }
    }

    PathTracingMode PathtracingModeFromName(const std::string& name, PathTracingMode fallback)
    {
        if (name == "Baseline PT" || name == "Path Tracing" || name == "path_tracing" || name == "baseline")
        {
            return PathTracingMode::Pathtracing;
        }
        if (name == "ReSTIR GI" || name == "restir_gi" || name == "restir")
        {
            return PathTracingMode::ReSTIR;
        }
        if (name == "ReSTIR DI" || name == "restir_di")
        {
            return PathTracingMode::ReSTIRDI;
        }
        if (name == "ReSTIR GI + DI" || name == "restir_gi_di" || name == "combined")
        {
            return PathTracingMode::ReSTIRCombined;
        }
        if (name == "ReSTIR PT" || name == "restir_pt")
        {
            return PathTracingMode::ReSTIRPT;
        }
        if (name == "ReSTIR PT + DI" || name == "restir_pt_di")
        {
            return PathTracingMode::ReSTIRPTCombined;
        }
        return fallback;
    }

    const char* ToneMapperName(D3D12PathTracingBackend::ToneMapper toneMapper)
    {
        switch (toneMapper)
        {
        case D3D12PathTracingBackend::ToneMapper::None:
            return "none";
        case D3D12PathTracingBackend::ToneMapper::Reinhard:
            return "reinhard";
        default:
            return "aces";
        }
    }

    bool TryParseToneMapper(const std::string& name, D3D12PathTracingBackend::ToneMapper& toneMapper)
    {
        if (name.empty() || name == "aces" || name == "ACES" || name == "Aces")
        {
            toneMapper = D3D12PathTracingBackend::ToneMapper::Aces;
            return true;
        }
        if (name == "none" || name == "None" || name == "raw")
        {
            toneMapper = D3D12PathTracingBackend::ToneMapper::None;
            return true;
        }
        if (name == "reinhard" || name == "Reinhard")
        {
            toneMapper = D3D12PathTracingBackend::ToneMapper::Reinhard;
            return true;
        }
        return false;
    }

    const char* MaterialFocusModeName(D3D12PathTracingBackend::MaterialFocusMode mode)
    {
        switch (mode)
        {
        case D3D12PathTracingBackend::MaterialFocusMode::Isolate:
            return "isolate";
        case D3D12PathTracingBackend::MaterialFocusMode::Dim:
            return "dim";
        default:
            return "normal";
        }
    }

    bool TryParseMaterialFocusMode(const std::string& name, D3D12PathTracingBackend::MaterialFocusMode& mode)
    {
        if (name.empty() || name == "normal" || name == "Normal")
        {
            mode = D3D12PathTracingBackend::MaterialFocusMode::Normal;
            return true;
        }
        if (name == "isolate" || name == "Isolate")
        {
            mode = D3D12PathTracingBackend::MaterialFocusMode::Isolate;
            return true;
        }
        if (name == "dim" || name == "Dim")
        {
            mode = D3D12PathTracingBackend::MaterialFocusMode::Dim;
            return true;
        }
        return false;
    }

    bool IsSupportedMaterialTextureExtension(const std::filesystem::path& path)
    {
        std::wstring ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".tga" ||
            ext == L".dds" || ext == L".hdr" || ext == L".exr" || ext == L".bmp";
    }

    std::wstring MaterialPresetDirectory()
    {
        std::array<wchar_t, 32768> appData = {};
        size_t length = 0;
        if (_wgetenv_s(&length, appData.data(), appData.size(), L"APPDATA") != 0 || length == 0)
        {
            std::array<wchar_t, MAX_PATH> tempPath = {};
            const DWORD tempLength = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
            if (tempLength > 0 && tempLength < tempPath.size())
            {
                return (std::filesystem::path(tempPath.data()) / L"D3D12LookDevPTWinUI" / L"materials").wstring();
            }
            return L"materials";
        }
        return (std::filesystem::path(appData.data()) / L"D3D12LookDevPTWinUI" / L"materials").wstring();
    }

    std::string SafeFileName(std::string text)
    {
        for (char& ch : text)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!std::isalnum(c) && ch != '_' && ch != '-')
            {
                ch = '_';
            }
        }
        if (text.empty())
        {
            text = "preset";
        }
        return text;
    }

    const char* NoisePresetName(D3D12PathTracingBackend::NoisePreset preset)
    {
        switch (preset)
        {
        case D3D12PathTracingBackend::NoisePreset::SharpPreview:
            return "sharp_preview";
        case D3D12PathTracingBackend::NoisePreset::StillCapture:
            return "still_capture";
        default:
            return "interactive_stable";
        }
    }

    const char* NoisePresetDisplayName(D3D12PathTracingBackend::NoisePreset preset)
    {
        switch (preset)
        {
        case D3D12PathTracingBackend::NoisePreset::SharpPreview:
            return "Sharp Preview";
        case D3D12PathTracingBackend::NoisePreset::StillCapture:
            return "Still Capture";
        default:
            return "Interactive Stable";
        }
    }

    bool TryParseNoisePreset(const std::string& name, D3D12PathTracingBackend::NoisePreset& preset)
    {
        if (name.empty() || name == "interactive_stable" || name == "Interactive Stable")
        {
            preset = D3D12PathTracingBackend::NoisePreset::InteractiveStable;
            return true;
        }
        if (name == "sharp_preview" || name == "Sharp Preview")
        {
            preset = D3D12PathTracingBackend::NoisePreset::SharpPreview;
            return true;
        }
        if (name == "still_capture" || name == "Still Capture")
        {
            preset = D3D12PathTracingBackend::NoisePreset::StillCapture;
            return true;
        }
        return false;
    }

    const char* JitterModeName(D3D12PathTracingBackend::JitterMode mode)
    {
        switch (mode)
        {
        case D3D12PathTracingBackend::JitterMode::Halton:
            return "halton";
        case D3D12PathTracingBackend::JitterMode::Off:
            return "off";
        default:
            return "stable32";
        }
    }

    const char* JitterModeDisplayName(D3D12PathTracingBackend::JitterMode mode)
    {
        switch (mode)
        {
        case D3D12PathTracingBackend::JitterMode::Halton:
            return "Halton";
        case D3D12PathTracingBackend::JitterMode::Off:
            return "Off";
        default:
            return "Stable32";
        }
    }

    bool TryParseJitterMode(const std::string& name, D3D12PathTracingBackend::JitterMode& mode)
    {
        if (name.empty() || name == "stable32" || name == "Stable32" || name == "stable16" || name == "Stable16")
        {
            mode = D3D12PathTracingBackend::JitterMode::Stable32;
            return true;
        }
        if (name == "halton" || name == "Halton")
        {
            mode = D3D12PathTracingBackend::JitterMode::Halton;
            return true;
        }
        if (name == "off" || name == "Off" || name == "none")
        {
            mode = D3D12PathTracingBackend::JitterMode::Off;
            return true;
        }
        return false;
    }

    const char* DenoiseBackendName(D3D12PathTracingBackend::DenoiseBackend backend)
    {
        switch (backend)
        {
        case D3D12PathTracingBackend::DenoiseBackend::NrdReblur:
            return "nrd_reblur";
        case D3D12PathTracingBackend::DenoiseBackend::NrdRelax:
            return "nrd_relax";
        case D3D12PathTracingBackend::DenoiseBackend::DlssRayReconstruction:
            return "dlss_rr";
        case D3D12PathTracingBackend::DenoiseBackend::Off:
            return "off";
        default:
            return "internal";
        }
    }

    const char* DenoiseBackendDisplayName(D3D12PathTracingBackend::DenoiseBackend backend)
    {
        switch (backend)
        {
        case D3D12PathTracingBackend::DenoiseBackend::NrdReblur:
            return "NRD REBLUR";
        case D3D12PathTracingBackend::DenoiseBackend::NrdRelax:
            return "NRD RELAX";
        case D3D12PathTracingBackend::DenoiseBackend::DlssRayReconstruction:
            return "DLSS Ray Reconstruction";
        case D3D12PathTracingBackend::DenoiseBackend::Off:
            return "Off";
        default:
            return "Internal";
        }
    }

    bool TryParseDenoiseBackend(const std::string& name, D3D12PathTracingBackend::DenoiseBackend& backend)
    {
        if (name.empty() || name == "internal" || name == "Internal")
        {
            backend = D3D12PathTracingBackend::DenoiseBackend::Internal;
            return true;
        }
        if (name == "nrd_reblur" || name == "nrd" || name == "reblur" || name == "NRD REBLUR")
        {
            backend = D3D12PathTracingBackend::DenoiseBackend::NrdReblur;
            return true;
        }
        if (name == "nrd_relax" || name == "relax" || name == "NRD RELAX")
        {
            backend = D3D12PathTracingBackend::DenoiseBackend::NrdRelax;
            return true;
        }
        if (name == "dlss_rr" || name == "dlss" || name == "DLSS Ray Reconstruction")
        {
            backend = D3D12PathTracingBackend::DenoiseBackend::DlssRayReconstruction;
            return true;
        }
        if (name == "off" || name == "none" || name == "Off")
        {
            backend = D3D12PathTracingBackend::DenoiseBackend::Off;
            return true;
        }
        return false;
    }

    const char* DlssModeName(DlssMode mode)
    {
        switch (mode)
        {
        case DlssMode::Balanced:
            return "balanced";
        case DlssMode::Performance:
            return "performance";
        case DlssMode::UltraPerformance:
            return "ultra_performance";
        default:
            return "quality";
        }
    }

    const char* DlssModeDisplayName(DlssMode mode)
    {
        switch (mode)
        {
        case DlssMode::Balanced:
            return "Balanced";
        case DlssMode::Performance:
            return "Performance";
        case DlssMode::UltraPerformance:
            return "Ultra Performance";
        default:
            return "Quality";
        }
    }

    bool TryParseDlssMode(const std::string& name, DlssMode& mode)
    {
        if (name.empty() || name == "quality" || name == "Quality")
        {
            mode = DlssMode::Quality;
            return true;
        }
        if (name == "balanced" || name == "Balanced")
        {
            mode = DlssMode::Balanced;
            return true;
        }
        if (name == "performance" || name == "Performance")
        {
            mode = DlssMode::Performance;
            return true;
        }
        if (name == "ultra_performance" || name == "Ultra Performance" || name == "ultraPerformance")
        {
            mode = DlssMode::UltraPerformance;
            return true;
        }
        return false;
    }

    std::array<float, 4> Float4ToArray(const XMFLOAT4& value)
    {
        return { value.x, value.y, value.z, value.w };
    }

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::string BenchmarkFormatName(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32G32_UINT: return "R32G32_UINT";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        default: return "DXGI_" + std::to_string(static_cast<unsigned>(format));
        }
    }

    uint32_t BenchmarkFormatChannelCount(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_FLOAT:
            return 1;
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_FLOAT:
            return 2;
        default:
            return 4;
        }
    }

    void InitializeArtifactStatistics(
        lookdevpt::benchmark::ArtifactStatistics& statistics,
        uint32_t channelCount)
    {
        statistics = {};
        statistics.available = true;
        statistics.channelCount = channelCount;
        statistics.channelMin.fill((std::numeric_limits<double>::max)());
        statistics.channelMax.fill((std::numeric_limits<double>::lowest)());
    }

    void AccumulateArtifactPixel(
        lookdevpt::benchmark::ArtifactStatistics& statistics,
        const double values[4])
    {
        bool nonFinitePixel = false;
        for (uint32_t channel = 0; channel < statistics.channelCount; ++channel)
        {
            if (!std::isfinite(values[channel]))
            {
                ++statistics.nonFiniteValueCount;
                nonFinitePixel = true;
                continue;
            }
            statistics.channelMin[channel] = (std::min)(statistics.channelMin[channel], values[channel]);
            statistics.channelMax[channel] = (std::max)(statistics.channelMax[channel], values[channel]);
        }
        if (nonFinitePixel)
        {
            ++statistics.nonFinitePixelCount;
        }
    }

    void FinalizeArtifactStatistics(lookdevpt::benchmark::ArtifactStatistics& statistics)
    {
        for (uint32_t channel = 0; channel < statistics.channelCount; ++channel)
        {
            if (statistics.channelMin[channel] == (std::numeric_limits<double>::max)())
            {
                // An entirely non-finite channel still has a valid count. Keep
                // min/max JSON finite while the non-finite gate fails.
                statistics.channelMin[channel] = 0.0;
                statistics.channelMax[channel] = 0.0;
            }
        }
    }

    bool ComputeArtifactStatistics(
        const DirectX::Image& image,
        lookdevpt::benchmark::ArtifactStatistics& statistics,
        std::string& diagnostics)
    {
        const uint32_t channelCount = BenchmarkFormatChannelCount(image.format);
        InitializeArtifactStatistics(statistics, channelCount);

        if (image.format == DXGI_FORMAT_R32_UINT)
        {
            for (size_t y = 0; y < image.height; ++y)
            {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(image.pixels + y * image.rowPitch);
                for (size_t x = 0; x < image.width; ++x)
                {
                    const double values[4] = { static_cast<double>(row[x]), 0.0, 0.0, 0.0 };
                    AccumulateArtifactPixel(statistics, values);
                }
            }
            FinalizeArtifactStatistics(statistics);
            return true;
        }
        if (image.format == DXGI_FORMAT_R32G32_UINT)
        {
            for (size_t y = 0; y < image.height; ++y)
            {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(image.pixels + y * image.rowPitch);
                for (size_t x = 0; x < image.width; ++x)
                {
                    const double values[4] = { static_cast<double>(row[x * 2]), static_cast<double>(row[x * 2 + 1]), 0.0, 0.0 };
                    AccumulateArtifactPixel(statistics, values);
                }
            }
            FinalizeArtifactStatistics(statistics);
            return true;
        }
        if (image.format == DXGI_FORMAT_R8_UNORM)
        {
            for (size_t y = 0; y < image.height; ++y)
            {
                const uint8_t* row = image.pixels + y * image.rowPitch;
                for (size_t x = 0; x < image.width; ++x)
                {
                    const double values[4] = { static_cast<double>(row[x]) / 255.0, 0.0, 0.0, 0.0 };
                    AccumulateArtifactPixel(statistics, values);
                }
            }
            FinalizeArtifactStatistics(statistics);
            return true;
        }

        DirectX::ScratchImage converted;
        const DirectX::Image* floatImage = &image;
        if (image.format != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            const HRESULT convertResult = DirectX::Convert(
                image,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                DirectX::TEX_FILTER_DEFAULT,
                0.0f,
                converted);
            if (FAILED(convertResult))
            {
                diagnostics = "Could not convert benchmark artifact for numeric validation.";
                return false;
            }
            floatImage = converted.GetImage(0, 0, 0);
        }
        if (!floatImage)
        {
            diagnostics = "Benchmark artifact numeric validation image is missing.";
            return false;
        }

        for (size_t y = 0; y < floatImage->height; ++y)
        {
            const float* row = reinterpret_cast<const float*>(floatImage->pixels + y * floatImage->rowPitch);
            for (size_t x = 0; x < floatImage->width; ++x)
            {
                const double values[4] =
                {
                    static_cast<double>(row[x * 4]),
                    static_cast<double>(row[x * 4 + 1]),
                    static_cast<double>(row[x * 4 + 2]),
                    static_cast<double>(row[x * 4 + 3]),
                };
                AccumulateArtifactPixel(statistics, values);
            }
        }
        FinalizeArtifactStatistics(statistics);
        return true;
    }

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return std::wstring(text.begin(), text.end());
        }
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    std::string Base64Encode(const uint8_t* data, size_t size)
    {
        static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve(((size + 2) / 3) * 4);
        for (size_t i = 0; i < size; i += 3)
        {
            const uint32_t a = data[i];
            const uint32_t b = (i + 1 < size) ? data[i + 1] : 0;
            const uint32_t c = (i + 2 < size) ? data[i + 2] : 0;
            const uint32_t triple = (a << 16) | (b << 8) | c;
            output.push_back(Alphabet[(triple >> 18) & 0x3f]);
            output.push_back(Alphabet[(triple >> 12) & 0x3f]);
            output.push_back(i + 1 < size ? Alphabet[(triple >> 6) & 0x3f] : '=');
            output.push_back(i + 2 < size ? Alphabet[triple & 0x3f] : '=');
        }
        return output;
    }

    bool Base64Decode(const std::string& input, std::vector<uint8_t>& output)
    {
        static constexpr int8_t Invalid = -1;
        std::array<int8_t, 256> table{};
        table.fill(Invalid);
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
        {
            table[static_cast<uint8_t>(alphabet[i])] = static_cast<int8_t>(i);
        }
        output.clear();
        output.reserve(input.size() * 3u / 4u);
        uint32_t accumulator = 0;
        int bits = 0;
        for (const unsigned char character : input)
        {
            if (character == '=') break;
            const int8_t value = table[character];
            if (value == Invalid) return false;
            accumulator = (accumulator << 6u) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xffu));
            }
        }
        return true;
    }

    bool DecodePngRgba8(const std::string& base64Png, lookdevpt::review::Rgba8Image& output)
    {
        std::vector<uint8_t> bytes;
        if (!Base64Decode(base64Png, bytes) || bytes.empty()) return false;
        DirectX::ScratchImage source;
        DirectX::TexMetadata metadata{};
        if (FAILED(DirectX::LoadFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_NONE, &metadata, source)))
        {
            return false;
        }
        DirectX::ScratchImage converted;
        const DirectX::Image* image = source.GetImage(0, 0, 0);
        if (!image) return false;
        if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            if (FAILED(DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, converted)))
            {
                return false;
            }
            image = converted.GetImage(0, 0, 0);
        }
        if (!image || image->width > UINT32_MAX || image->height > UINT32_MAX) return false;
        output.width = static_cast<uint32_t>(image->width);
        output.height = static_cast<uint32_t>(image->height);
        output.pixels.resize(image->width * image->height * 4u);
        for (size_t row = 0; row < image->height; ++row)
        {
            memcpy(output.pixels.data() + row * image->width * 4u,
                image->pixels + row * image->rowPitch, image->width * 4u);
        }
        return true;
    }

    bool EncodePngRgba8(const lookdevpt::review::Rgba8Image& image, std::string& base64Png)
    {
        if (image.width == 0 || image.height == 0 ||
            image.pixels.size() != static_cast<size_t>(image.width) * image.height * 4u)
        {
            return false;
        }
        DirectX::Image source{};
        source.width = image.width;
        source.height = image.height;
        source.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source.rowPitch = static_cast<size_t>(image.width) * 4u;
        source.slicePitch = image.pixels.size();
        source.pixels = const_cast<uint8_t*>(image.pixels.data());
        DirectX::Blob blob;
        if (FAILED(DirectX::SaveToWICMemory(source, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, blob)))
        {
            return false;
        }
        base64Png = Base64Encode(blob.GetConstBufferPointer(), blob.GetBufferSize());
        return true;
    }

    std::wstring UserSettingsDirectory()
    {
        std::array<wchar_t, 32768> appData = {};
        size_t length = 0;
        if (_wgetenv_s(&length, appData.data(), appData.size(), L"APPDATA") != 0 || length == 0)
        {
            std::array<wchar_t, MAX_PATH> tempPath = {};
            const DWORD tempLength = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
            if (tempLength > 0 && tempLength < tempPath.size())
            {
                return (std::filesystem::path(tempPath.data()) / L"D3D12LookDevPTWinUI").wstring();
            }
            return L".";
        }
        return (std::filesystem::path(appData.data()) / L"D3D12LookDevPTWinUI").wstring();
    }

    std::wstring McpSettingsPath()
    {
        return (std::filesystem::path(UserSettingsDirectory()) / L"settings.json").wstring();
    }

    std::filesystem::path McpCheckpointDirectory()
    {
        return std::filesystem::path(UserSettingsDirectory()) / L"Checkpoints";
    }

    std::filesystem::path McpBenchmarkDirectory()
    {
        return std::filesystem::path(UserSettingsDirectory()) / L"Benchmarks";
    }

    std::wstring DefaultStartupSettingsPath()
    {
        return (std::filesystem::path(UserSettingsDirectory()) / L"startup.json").wstring();
    }

    std::wstring ResolveStartupPath(const std::string& text, const std::filesystem::path& baseDirectory)
    {
        const std::wstring wide = Utf8ToWide(text);
        if (wide.empty())
        {
            return {};
        }

        std::filesystem::path path(wide);
        if (path.is_relative() && !baseDirectory.empty())
        {
            path = baseDirectory / path;
        }
        return std::filesystem::absolute(path).wstring();
    }

    bool AllFinite(std::initializer_list<float> values)
    {
        for (float value : values)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }
        return true;
    }

    void AppendJsonFloat3(std::ostringstream& out, const float values[3])
    {
        out << "[" << values[0] << "," << values[1] << "," << values[2] << "]";
    }

    void AppendJsonFloat3(std::ostringstream& out, const XMFLOAT3& value)
    {
        out << "[" << value.x << "," << value.y << "," << value.z << "]";
    }

    void AppendJsonFloat4(std::ostringstream& out, const XMFLOAT4& value)
    {
        out << "[" << value.x << "," << value.y << "," << value.z << "," << value.w << "]";
    }

    void LogDiagnostic(const std::string& message)
    {
        OutputDebugStringA((message + "\n").c_str());

        std::array<wchar_t, MAX_PATH> tempPath = {};
        const DWORD length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
        if (length == 0 || length >= tempPath.size())
        {
            return;
        }

        std::ofstream file(std::filesystem::path(tempPath.data()) / L"D3D12LookDevPTWinUI.log", std::ios::app | std::ios::binary);
        if (file)
        {
            file << message << "\n";
        }
    }

    void LogDiagnostic(const std::wstring& message)
    {
        LogDiagnostic(WideToUtf8(message));
    }

    void ExpandBounds(Bistro::Scene& scene, const XMFLOAT3& p)
    {
        scene.boundsMin.x = (std::min)(scene.boundsMin.x, p.x);
        scene.boundsMin.y = (std::min)(scene.boundsMin.y, p.y);
        scene.boundsMin.z = (std::min)(scene.boundsMin.z, p.z);
        scene.boundsMax.x = (std::max)(scene.boundsMax.x, p.x);
        scene.boundsMax.y = (std::max)(scene.boundsMax.y, p.y);
        scene.boundsMax.z = (std::max)(scene.boundsMax.z, p.z);
    }

    Bistro::Scene MakePreviewScene()
    {
        Bistro::Scene scene;
        scene.assetRoot = L".";
        scene.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        scene.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        Bistro::Material material;
        material.name = L"Preview Material";
        material.baseColorFactor = XMFLOAT4(0.72f, 0.76f, 0.82f, 1.0f);
        material.roughnessFactor = 0.42f;
        material.metallicFactor = 0.0f;
        scene.materials.push_back(material);

        struct Face
        {
            XMFLOAT3 normal;
            XMFLOAT3 a;
            XMFLOAT3 b;
            XMFLOAT3 c;
            XMFLOAT3 d;
        };
        const Face faces[] =
        {
            { XMFLOAT3(0, 0, -1), XMFLOAT3(-1, -1, -1), XMFLOAT3(1, -1, -1), XMFLOAT3(1, 1, -1), XMFLOAT3(-1, 1, -1) },
            { XMFLOAT3(0, 0, 1), XMFLOAT3(1, -1, 1), XMFLOAT3(-1, -1, 1), XMFLOAT3(-1, 1, 1), XMFLOAT3(1, 1, 1) },
            { XMFLOAT3(-1, 0, 0), XMFLOAT3(-1, -1, 1), XMFLOAT3(-1, -1, -1), XMFLOAT3(-1, 1, -1), XMFLOAT3(-1, 1, 1) },
            { XMFLOAT3(1, 0, 0), XMFLOAT3(1, -1, -1), XMFLOAT3(1, -1, 1), XMFLOAT3(1, 1, 1), XMFLOAT3(1, 1, -1) },
            { XMFLOAT3(0, 1, 0), XMFLOAT3(-1, 1, -1), XMFLOAT3(1, 1, -1), XMFLOAT3(1, 1, 1), XMFLOAT3(-1, 1, 1) },
            { XMFLOAT3(0, -1, 0), XMFLOAT3(-1, -1, 1), XMFLOAT3(1, -1, 1), XMFLOAT3(1, -1, -1), XMFLOAT3(-1, -1, -1) },
        };

        for (const Face& face : faces)
        {
            const uint32_t base = static_cast<uint32_t>(scene.vertices.size());
            const XMFLOAT2 uv[] = { XMFLOAT2(0, 1), XMFLOAT2(1, 1), XMFLOAT2(1, 0), XMFLOAT2(0, 0) };
            const XMFLOAT3 positions[] = { face.a, face.b, face.c, face.d };
            for (uint32_t i = 0; i < 4; ++i)
            {
                Bistro::Vertex vertex;
                vertex.position = positions[i];
                vertex.normal = face.normal;
                vertex.tangent = XMFLOAT4(1, 0, 0, 1);
                vertex.texcoord = uv[i];
                scene.vertices.push_back(vertex);
                ExpandBounds(scene, vertex.position);
            }
            scene.indices.insert(scene.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }

        Bistro::DrawItem draw;
        draw.indexCount = static_cast<uint32_t>(scene.indices.size());
        draw.startIndex = 0;
        draw.materialIndex = 0;
        scene.draws.push_back(draw);
        return scene;
    }

    Bistro::Scene ConvertImportedScene(const rb::ImportedScene& imported)
    {
        Bistro::Scene scene;
        scene.assetRoot = std::filesystem::path(imported.path).parent_path().wstring();
        scene.boundsMin = imported.boundsMin;
        scene.boundsMax = imported.boundsMax;
        if (!imported.meshes.empty())
        {
            std::size_t totalVertices = 0;
            std::size_t totalIndices = 0;
            std::size_t totalDraws = 0;
            for (const rb::SceneMesh& mesh : imported.meshes)
            {
                if (mesh.vertices.size() > UINT32_MAX || mesh.indices.size() > UINT32_MAX || mesh.draws.size() > UINT32_MAX ||
                    mesh.vertices.size() > std::numeric_limits<std::size_t>::max() - totalVertices ||
                    mesh.indices.size() > std::numeric_limits<std::size_t>::max() - totalIndices ||
                    mesh.draws.size() > std::numeric_limits<std::size_t>::max() - totalDraws)
                {
                    throw std::runtime_error("Imported scene exceeds addressable CPU mesh-buffer size.");
                }
                totalVertices += mesh.vertices.size();
                totalIndices += mesh.indices.size();
                totalDraws += mesh.draws.size();
            }
            if (totalVertices > UINT32_MAX || totalIndices > UINT32_MAX)
            {
                throw std::runtime_error("Imported scene exceeds 32-bit GPU mesh-buffer addressing.");
            }
            if (totalDraws >= (1u << 24u))
            {
                throw std::runtime_error("Imported scene exceeds the DXR 24-bit global geometry limit.");
            }
            if (totalVertices > std::numeric_limits<UINT64>::max() / sizeof(Bistro::Vertex) ||
                totalIndices > std::numeric_limits<UINT64>::max() / sizeof(uint32_t))
            {
                throw std::runtime_error("Imported scene GPU allocation size overflowed.");
            }
            scene.vertices.reserve(totalVertices);
            scene.indices.reserve(totalIndices);
            scene.draws.reserve(totalDraws);
            scene.meshes.reserve(imported.meshes.size());
            for (const rb::SceneMesh& sourceMesh : imported.meshes)
            {
                if (scene.vertices.size() > UINT32_MAX || scene.indices.size() > UINT32_MAX || scene.draws.size() > UINT32_MAX)
                {
                    throw std::runtime_error("Imported scene exceeds 32-bit mesh-buffer addressing.");
                }
                Bistro::MeshRange mesh;
                mesh.vertexOffset = static_cast<uint32_t>(scene.vertices.size());
                mesh.vertexCount = static_cast<uint32_t>(sourceMesh.vertices.size());
                mesh.indexOffset = static_cast<uint32_t>(scene.indices.size());
                mesh.indexCount = static_cast<uint32_t>(sourceMesh.indices.size());
                mesh.drawOffset = static_cast<uint32_t>(scene.draws.size());
                mesh.drawCount = static_cast<uint32_t>(sourceMesh.draws.size());
                mesh.boundsMin = sourceMesh.boundsMin;
                mesh.boundsMax = sourceMesh.boundsMax;
                for (const rb::SceneVertex& src : sourceMesh.vertices)
                {
                    scene.vertices.push_back({ src.position, src.normal, src.tangent, src.texcoord, src.texcoord1 });
                }
                for (const uint32_t localIndex : sourceMesh.indices)
                {
                    if (localIndex >= sourceMesh.vertices.size()) throw std::runtime_error("Imported mesh index is out of bounds.");
                    scene.indices.push_back(mesh.vertexOffset + localIndex);
                }
                for (const rb::SceneDraw& src : sourceMesh.draws)
                {
                    scene.draws.push_back({ src.indexCount, mesh.indexOffset + src.startIndex, 0, src.materialIndex });
                }
                scene.meshes.push_back(mesh);
            }
            scene.instances.reserve(imported.instances.size());
            for (const rb::SceneInstance& src : imported.instances)
            {
                scene.instances.push_back({ src.meshIndex, src.transform, src.normalTransform });
            }
        }
        else
        {
            scene.vertices.reserve(imported.vertices.size());
            for (const rb::SceneVertex& src : imported.vertices)
            {
                scene.vertices.push_back({ src.position, src.normal, src.tangent, src.texcoord, src.texcoord1 });
            }
            scene.indices = imported.indices;
            for (const rb::SceneDraw& src : imported.draws)
            {
                scene.draws.push_back({ src.indexCount, src.startIndex, src.baseVertex, src.materialIndex });
            }
        }
        scene.hasAuthoredLighting = imported.hasAuthoredLighting;
        scene.extensionsUsed = imported.extensionsUsed;
        scene.extensionsRequired = imported.extensionsRequired;
        scene.unsupportedExtensions = imported.unsupportedExtensions;
        scene.materialFeatureMask = imported.materialFeatureMask;
        scene.analyticLights.reserve(imported.lights.size());
        for (const rb::SceneLight& sourceLight : imported.lights)
        {
            Bistro::AnalyticLight light;
            light.type = sourceLight.type == rb::SceneLightType::Spot
                ? Bistro::AnalyticLightType::Spot
                : sourceLight.type == rb::SceneLightType::Distant
                    ? Bistro::AnalyticLightType::Distant
                    : Bistro::AnalyticLightType::Point;
            light.position = sourceLight.position;
            light.direction = sourceLight.direction;
            light.radiance = sourceLight.radiance;
            light.coneAngleDegrees = sourceLight.coneAngleDegrees;
            light.coneDeltaDegrees = sourceLight.coneDeltaDegrees;
            scene.analyticLights.push_back(light);
        }
        scene.materials.reserve(imported.materials.size());
        for (const rb::SceneMaterial& src : imported.materials)
        {
            Bistro::Material material;
            material.name = Utf8ToWide(src.assignment.materialName);
            material.sourceMaterialId = src.sourceMaterialId;
            material.textures[Bistro::TextureSlotBaseColor] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::BaseColor)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::BaseColor)] : src.baseColorTexturePath;
            material.textures[Bistro::TextureSlotNormal] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Normal)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Normal)] : src.normalTexturePath;
            material.textures[Bistro::TextureSlotRoughness] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Roughness)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Roughness)] : src.roughnessTexturePath;
            material.textures[Bistro::TextureSlotMetallic] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Metallic)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Metallic)] : src.metallicTexturePath;
            material.textures[Bistro::TextureSlotOcclusion] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Occlusion)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Occlusion)] : src.occlusionTexturePath;
            material.textures[Bistro::TextureSlotEmissive] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Emissive)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Emissive)] : src.emissiveTexturePath;
            material.textures[Bistro::TextureSlotAlpha] = src.assignment.textureOverrideEnabled[static_cast<size_t>(rb::TextureSlot::Alpha)] ? src.assignment.textureOverrides[static_cast<size_t>(rb::TextureSlot::Alpha)] : src.alphaTexturePath;
            for (UINT slot = Bistro::TextureSlotSpecularColor; slot < Bistro::TextureSlotCount; ++slot)
            {
                const size_t editorSlot = static_cast<size_t>(slot);
                material.textures[slot] = src.assignment.textureOverrideEnabled[editorSlot]
                    ? src.assignment.textureOverrides[editorSlot]
                    : src.textureBindings[editorSlot].path;
            }
            for (UINT slot = 0; slot < Bistro::TextureSlotCount; ++slot)
            {
                const rb::TextureBinding& sourceBinding = src.assignment.textureBindingOverrideEnabled[slot]
                    ? src.assignment.textureBindings[slot]
                    : src.textureBindings[slot];
                Bistro::TextureBinding& targetBinding = material.textureBindings[slot];
                targetBinding.offset = XMFLOAT2(sourceBinding.transform.offset[0], sourceBinding.transform.offset[1]);
                targetBinding.scale = XMFLOAT2(sourceBinding.transform.scale[0], sourceBinding.transform.scale[1]);
                targetBinding.rotation = sourceBinding.transform.rotation;
                targetBinding.texCoord = std::min(sourceBinding.transform.texCoord, 1u);
                targetBinding.samplerIndex = static_cast<uint32_t>(sourceBinding.sampler);
                targetBinding.resolutionPolicy = static_cast<uint32_t>(sourceBinding.resolutionPolicy);
            }
            material.baseColorFactor = XMFLOAT4(src.assignment.baseColorFactor[0], src.assignment.baseColorFactor[1], src.assignment.baseColorFactor[2], src.assignment.baseColorFactor[3]);
            material.emissiveFactor = XMFLOAT4(src.assignment.emissiveFactor[0], src.assignment.emissiveFactor[1], src.assignment.emissiveFactor[2], src.assignment.emissiveFactor[3]);
            material.roughnessFactor = src.assignment.roughnessFactor;
            material.metallicFactor = src.assignment.metallicFactor;
            material.occlusionStrength = src.assignment.occlusionStrength;
            material.normalStrength = src.assignment.normalStrength;
            material.alphaCutoff = src.assignment.alphaCutoff;
            material.alphaMasked = src.assignment.alphaMode == rb::AlphaMode::Mask || src.assignment.baseColorFactor[3] < 0.99f;
            material.twoSidedEmission = src.twoSidedEmission;
            material.packedOcclusionRoughnessMetallic = src.assignment.packedOcclusionRoughnessMetallic;
            material.gltfMetallicRoughness = !src.sourceMaterialId.empty();
            material.transmissionFactor = src.transmissionFactor;
            material.indexOfRefraction = src.indexOfRefraction;
            material.thinDielectric = src.thinDielectric;
            material.uvScaleOffset = src.uvScaleOffset;
            const rb::GltfMaterialExtensions& gltf = src.assignment.gltfExtensions.featureMask != 0
                ? src.assignment.gltfExtensions
                : src.gltfExtensions;
            material.extensionFeatureMask = gltf.featureMask;
            material.specularFactor = gltf.specularFactor;
            material.specularColorFactor = XMFLOAT3(gltf.specularColorFactor[0], gltf.specularColorFactor[1], gltf.specularColorFactor[2]);
            material.indexOfRefraction = gltf.ior;
            material.transmissionFactor = gltf.transmissionFactor;
            material.thicknessFactor = gltf.thicknessFactor;
            material.attenuationColor = XMFLOAT3(gltf.attenuationColor[0], gltf.attenuationColor[1], gltf.attenuationColor[2]);
            material.attenuationDistance = gltf.attenuationDistance;
            material.clearcoatFactor = gltf.clearcoatFactor;
            material.clearcoatRoughnessFactor = gltf.clearcoatRoughnessFactor;
            material.clearcoatNormalScale = gltf.clearcoatNormalScale;
            scene.materials.push_back(material);
        }
        if (scene.materials.empty())
        {
            scene.materials.push_back(Bistro::Material{});
        }
        return scene;
    }
}

D3D12PathTracingBackend::D3D12PathTracingBackend(UINT width, UINT height, std::wstring name, PathTracingMode mode) :
    DXSample(width, height, name),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_renderWidth(width),
    m_renderHeight(height),
    m_mode(mode)
{
}

void D3D12PathTracingBackend::OnInit()
{
    if (!m_benchmarkCommandLineValid)
    {
        throw std::invalid_argument(m_benchmarkDiagnostics.empty() ? "Invalid benchmark command line." : m_benchmarkDiagnostics);
    }

    m_rtxdiBackendRuntime.RefreshStatus();
    m_rtxdiAvailable = m_rtxdiBackendRuntime.IsDiEvaluationReady();
    LoadStartupSettings();
    if (m_benchmarkOptions.enabled)
    {
        m_benchmarkHarness = std::make_unique<lookdevpt::benchmark::Harness>(m_benchmarkOptions);
        if (!m_benchmarkHarness->Initialize(m_benchmarkDiagnostics))
        {
            throw std::runtime_error(m_benchmarkDiagnostics);
        }
        const uint64_t seed = m_benchmarkOptions.seed;
        m_samplingSeed = static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(seed >> 32u);
        m_vsyncEnabled = false;
        m_benchmarkFrameIndex = 0;
        m_benchmarkRecordedFrameCount = 0;
        m_completedBenchmarkMetrics.clear();
        m_benchmarkFinished = false;
        ResetRenderingHistory();
        LogDiagnostic("Benchmark initialized: " + std::to_string(m_benchmarkHarness->TotalFrames()) + " total frames, fixed 1/60 timestep.");
    }
    // Benchmark mode affects the resource contract: Reference Still normally
    // uses 1x1 guide placeholders, while benchmark artifact capture requires
    // full-resolution AOVs. Initialize the harness before scene resources.
    LoadPipeline();
    LoadAssets();
    LoadMcpUserSettings();
    if (m_startupMcpPort > 0 || !m_startupMcpToken.empty() ||
        m_hasStartupMcpAuthenticationMode || m_hasStartupMcpAccessMode)
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        if (m_startupMcpPort > 0)
        {
            m_mcpSettings.port = static_cast<uint16_t>(std::clamp<UINT>(m_startupMcpPort, 1, 65535));
        }
        if (!m_startupMcpToken.empty())
        {
            m_mcpSettings.token = m_startupMcpToken;
        }
        if (m_hasStartupMcpAuthenticationMode)
        {
            m_mcpSettings.authenticationMode =
                m_startupMcpAuthenticationMode;
        }
        if (m_hasStartupMcpAccessMode)
        {
            m_mcpSettings.accessMode = m_startupMcpAccessMode;
        }
    }
    if (m_startupMcpServer)
    {
        StartMcpServer();
        if (m_startupMcpPairing && m_mcpServer.IsRunning())
        {
            m_mcpServer.BeginPairing();
            m_mcpUiDiagnostics = "MCP server started with a 90-second pairing code.";
        }
    }
    UpdateMcpSnapshots();
    m_lastUpdate = std::chrono::steady_clock::now();
}

void D3D12PathTracingBackend::LoadPipeline()
{
    m_dlssBackendRuntime.InitializeBeforeDevice(ExecutableDirectory());

    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    // Composition swap chains do not support DXGI_PRESENT_ALLOW_TEARING.
    m_tearingSupported = false;

    ComPtr<IDXGIAdapter1> hardwareAdapter;
    GetHardwareAdapter(factory.Get(), &hardwareAdapter);
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if (hardwareAdapter && SUCCEEDED(hardwareAdapter->GetDesc1(&adapterDesc)))
    {
        m_adapterDescription = adapterDesc.Description;
        m_adapterDedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        const UINT64 lower = 512ull * 1024ull * 1024ull;
        const UINT64 upper = 4ull * 1024ull * 1024ull * 1024ull;
        m_textureBudgetBytes = std::clamp(m_adapterDedicatedVideoMemory / 4ull, lower, upper);
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(hardwareAdapter.As(&adapter3)))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo)) && memoryInfo.Budget > memoryInfo.CurrentUsage)
            {
                m_textureBudgetBytes = std::min(m_textureBudgetBytes, memoryInfo.Budget - memoryInfo.CurrentUsage);
            }
        }
    }
    ComPtr<ID3D12Device2> baseDevice;
    ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&baseDevice)));
    ThrowIfFailed(baseDevice.As(&m_device));
    m_dlssBackendRuntime.SetD3DDevice(m_device.Get(), hardwareAdapter.Get(), m_width, m_height, m_dlssMode);
    m_nrdBackendRuntime.Initialize(m_device.Get(), m_width, m_height, SelectedNrdMethod(), FrameCount);

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    m_raytracingTier = options5.RaytracingTier;
    if (m_raytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    {
        throw std::runtime_error("This GPU/driver does not support DirectX Pathtracing. D3D12PathTracingBackend has no raster fallback.");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    CreateGpuTimingResources();

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = BackBufferFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForComposition(
        m_commandQueue.Get(),
        &swapChainDesc,
        nullptr,
        &swapChain));
    ThrowIfFailed(swapChain.As(&m_swapChain));
    ComPtr<IDXGISwapChain2> swapChain2;
    ThrowIfFailed(m_swapChain.As(&swapChain2));
    ThrowIfFailed(swapChain2->SetMaximumFrameLatency(2));
    m_frameLatencyWaitableObject =
        swapChain2->GetFrameLatencyWaitableObject();
    if (!m_frameLatencyWaitableObject)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CreateRenderTargetViews();

    for (FrameContext& frameContext : m_frameContexts)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameContext.commandAllocator)));
    }
    ThrowIfFailed(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_frameContexts[m_frameIndex].commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_mcpReviewCommandAllocator)));
    ThrowIfFailed(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_mcpReviewCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_mcpReviewCommandList)));
    ThrowIfFailed(m_mcpReviewCommandList->Close());

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

void D3D12PathTracingBackend::CreateRenderTargetViews()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, m_rtvDescriptorSize);
    }
}

void D3D12PathTracingBackend::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
    {
        return;
    }
    if (width == m_width && height == m_height)
    {
        return;
    }

    LogDiagnostic("Resize: " + std::to_string(width) + "x" + std::to_string(height));
    InvalidateHistory(rb::FrameChangeMask::Resolution);
    WaitForPreviousFrame();

    for (UINT n = 0; n < FrameCount; ++n)
    {
        m_renderTargets[n].Reset();
    }

    m_width = width;
    m_height = height;
    m_dlssBackendRuntime.UpdateMode(m_width, m_height, m_dlssMode);
    ApplyConfiguredRenderScale(false);
    m_aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    m_scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height));

    constexpr UINT flags =
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, m_width, m_height, BackBufferFormat, flags));
    m_nrdBackendRuntime.Resize(m_device.Get(), m_renderWidth, m_renderHeight);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargetViews();
    CreateGpuResourcesForCurrentScene();
}

bool D3D12PathTracingBackend::UsesTemporalUpscale() const
{
    return m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        (m_renderWidth != m_width || m_renderHeight != m_height);
}

bool D3D12PathTracingBackend::UsesCompactSecondaryWorkList() const
{
    const bool qualityDiagnostics = m_benchmarkHarness &&
        lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind);
    return m_qualitySettings.qualityProfile == rb::QualityProfile::InteractiveGame &&
        m_raytracingTier >= D3D12_RAYTRACING_TIER_1_1 &&
        !UsesRestirGI(m_mode) &&
        !UsesRestirPT(m_mode) &&
        m_maxPathBounces > 1 &&
        m_debugViewMode == 0 &&
        !qualityDiagnostics;
}

void D3D12PathTracingBackend::ApplyConfiguredRenderScale(bool resetDynamicScale)
{
    const DlssStatus& dlssStatus = m_dlssBackendRuntime.Status();
    if (m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        IsDlssSelected() &&
        dlssStatus.evaluationReady &&
        dlssStatus.recommendedRenderWidth > 0u &&
        dlssStatus.recommendedRenderHeight > 0u)
    {
        m_renderWidth = (std::min)(dlssStatus.recommendedRenderWidth, m_width);
        m_renderHeight = (std::min)(dlssStatus.recommendedRenderHeight, m_height);
        m_activeRenderScale = (std::min)(
            static_cast<float>(m_renderWidth) /
                static_cast<float>((std::max)(m_width, 1u)),
            static_cast<float>(m_renderHeight) /
                static_cast<float>((std::max)(m_height, 1u)));
        return;
    }

    float requestedScale = 1.0f;
    if (m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill)
    {
        switch (m_qualitySettings.resolutionMode)
        {
        case rb::ResolutionMode::Fixed:
            requestedScale = m_qualitySettings.fixedRenderScale;
            break;
        case rb::ResolutionMode::Dynamic:
            if (resetDynamicScale || m_activeRenderScale <= 0.0f)
            {
                m_activeRenderScale = m_qualitySettings.maxRenderScale;
            }
            requestedScale = std::clamp(
                m_activeRenderScale,
                m_qualitySettings.minRenderScale,
                m_qualitySettings.maxRenderScale);
            break;
        case rb::ResolutionMode::Native:
        default:
            requestedScale = 1.0f;
            break;
        }
    }

    const rb::RenderExtent extent =
        rb::ResolveRenderExtent(m_width, m_height, requestedScale);
    m_activeRenderScale = extent.scale;
    m_renderWidth = extent.width;
    m_renderHeight = extent.height;
}

void D3D12PathTracingBackend::RecreateResolutionDependentResources()
{
    if (!m_device || !m_descriptorHeap)
    {
        return;
    }

    WaitForPreviousFrame();
    m_nrdBackendRuntime.Resize(m_device.Get(), m_renderWidth, m_renderHeight);
    CreateOutputResources();
    InvalidateHistory(rb::FrameChangeMask::Resolution);
    ResetRenderingHistory();
    LogDiagnostic(
        "Internal render resolution changed to " +
        std::to_string(m_renderWidth) + "x" + std::to_string(m_renderHeight) +
        " (" + std::to_string(m_activeRenderScale) + "x)." );
}

void D3D12PathTracingBackend::UpdateDynamicResolution()
{
    if (m_qualitySettings.resolutionMode != rb::ResolutionMode::Dynamic ||
        m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        (IsDlssSelected() &&
            m_dlssBackendRuntime.CanEvaluateRayReconstruction()) ||
        !m_gpuTimingValid)
    {
        m_dynamicResolutionOverBudgetFrames = 0;
        m_dynamicResolutionUnderBudgetFrames = 0;
        return;
    }

    const float targetMs = m_qualitySettings.rayBudget.targetGpuMs;
    const uint32_t settleFrames =
        (std::max)(m_qualitySettings.rayBudget.settleFrames, 1u);
    float nextScale = m_activeRenderScale;
    if (m_gpuFrameMs > static_cast<double>(targetMs))
    {
        ++m_dynamicResolutionOverBudgetFrames;
        m_dynamicResolutionUnderBudgetFrames = 0;
        if (m_dynamicResolutionOverBudgetFrames >= settleFrames)
        {
            nextScale -= 1.0f / 16.0f;
            m_dynamicResolutionOverBudgetFrames = 0;
        }
    }
    else if (m_gpuFrameMs <
        static_cast<double>((std::max)(targetMs - 1.5f, 1.0f)))
    {
        ++m_dynamicResolutionUnderBudgetFrames;
        m_dynamicResolutionOverBudgetFrames = 0;
        if (m_dynamicResolutionUnderBudgetFrames >= settleFrames * 4u)
        {
            nextScale += 1.0f / 16.0f;
            m_dynamicResolutionUnderBudgetFrames = 0;
        }
    }
    else
    {
        m_dynamicResolutionOverBudgetFrames = 0;
        m_dynamicResolutionUnderBudgetFrames = 0;
    }

    nextScale = rb::QuantizeRenderScale(std::clamp(
        nextScale,
        m_qualitySettings.minRenderScale,
        m_qualitySettings.maxRenderScale));
    if (std::abs(nextScale - m_activeRenderScale) < 1.0e-6f)
    {
        return;
    }

    const UINT previousWidth = m_renderWidth;
    const UINT previousHeight = m_renderHeight;
    m_activeRenderScale = nextScale;
    ApplyConfiguredRenderScale(false);
    if (m_renderWidth != previousWidth || m_renderHeight != previousHeight)
    {
        RecreateResolutionDependentResources();
    }
}

void D3D12PathTracingBackend::LoadAssets()
{
    LogDiagnostic("LoadAssets: creating preview scene.");
    m_scene = MakePreviewScene();
    m_mcpSceneAuditSummary = lookdevpt::review::AnalyzeScene(m_scene);
    m_mcpSceneAuditFresh = true;
    if (m_scene.draws.empty())
    {
        throw std::runtime_error("Preview scene did not produce Path Tracing geometries.");
    }

    m_defaultCameraPosition = XMFLOAT3(0.0f, 0.4f, -5.0f);
    m_defaultCameraYaw = 0.0f;
    m_defaultCameraPitch = XMConvertToRadians(4.0f);
    ResetCameraView();
    ResetCameraSpeeds();
    InitializeMaterialLookDevState(true);

    CreateGpuResourcesForCurrentScene();
    ApplyStartupSettings();
}

void D3D12PathTracingBackend::CreateGpuResourcesForCurrentScene()
{
    LogDiagnostic("CreateGpuResourcesForCurrentScene: begin.");
    WaitForPreviousFrame();
    ApplyConfiguredRenderScale(false);
    m_geometryRecords.clear();
    m_rtMaterials.clear();
    m_rtMaterialExtensions.clear();
    m_rtTextureBindings.clear();
    m_rtInstances.clear();
    m_materialTextureIndices.clear();
    m_textures.clear();
    m_textureResidentBytes = 0;
    m_uploadBuffers.clear();

    LogDiagnostic("CreateGpuResourcesForCurrentScene: building light list.");
    RebuildLightList();

    m_geometryRecords.reserve(m_scene.draws.size());
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        Bistro::RtGeometryRecord record{};
        record.indexOffset = draw.startIndex;
        record.indexCount = draw.indexCount;
        record.baseVertex = draw.baseVertex;
        record.materialIndex = draw.materialIndex;
        m_geometryRecords.push_back(record);
    }

    auto appendInstanceRecord = [&](const XMFLOAT4X4& objectToWorld, const XMFLOAT4X4& normalToWorld)
    {
        Bistro::RtInstance record{};
        record.objectToWorldColumn0 = XMFLOAT4(objectToWorld._11, objectToWorld._21, objectToWorld._31, objectToWorld._41);
        record.objectToWorldColumn1 = XMFLOAT4(objectToWorld._12, objectToWorld._22, objectToWorld._32, objectToWorld._42);
        record.objectToWorldColumn2 = XMFLOAT4(objectToWorld._13, objectToWorld._23, objectToWorld._33, objectToWorld._43);
        record.objectToWorldColumn3 = XMFLOAT4(objectToWorld._14, objectToWorld._24, objectToWorld._34, objectToWorld._44);
        record.normalToWorldColumn0 = XMFLOAT4(normalToWorld._11, normalToWorld._21, normalToWorld._31, normalToWorld._41);
        record.normalToWorldColumn1 = XMFLOAT4(normalToWorld._12, normalToWorld._22, normalToWorld._32, normalToWorld._42);
        record.normalToWorldColumn2 = XMFLOAT4(normalToWorld._13, normalToWorld._23, normalToWorld._33, normalToWorld._43);
        record.normalToWorldColumn3 = XMFLOAT4(normalToWorld._14, normalToWorld._24, normalToWorld._34, normalToWorld._44);
        m_rtInstances.push_back(record);
    };
    if (m_scene.instances.empty())
    {
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        appendInstanceRecord(identity, identity);
    }
    else
    {
        for (const Bistro::SceneInstance& instance : m_scene.instances)
        {
            appendInstanceRecord(instance.transform, instance.normalTransform);
        }
    }

    for (FrameContext& frameContext : m_frameContexts)
    {
        if (frameContext.sceneConstantBuffer && frameContext.mappedSceneConstants)
        {
            frameContext.sceneConstantBuffer->Unmap(0, nullptr);
        }
        frameContext.mappedSceneConstants = nullptr;
        frameContext.sceneConstantBuffer.Reset();
    }

    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    ThrowIfFailed(frameContext.commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));

    LogDiagnostic("CreateGpuResourcesForCurrentScene: descriptors/output/root signature.");
    CreateDescriptorHeap();
    CreateOutputResources();
    CreateGlobalRootSignature();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: scene buffers.");
    CreateSceneBuffers();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: textures.");
    CreateTextures();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: pipelines.");
    CreatePathtracingStateObject();
    CreateRestirReusePipeline();
    CreateDenoisePipeline();
    CreateSecondaryWorkPipelines();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: acceleration structures.");
    BuildAccelerationStructures();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: shader tables.");
    CreateShaderTables();

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
    WaitForPreviousFrame();
    m_uploadBuffers.clear();
    ResetRenderingHistory();
    LogDiagnostic("CreateGpuResourcesForCurrentScene: complete.");
}

void D3D12PathTracingBackend::RequestGpuResourceRefresh(PendingGpuResourceRefresh refresh)
{
    if (static_cast<std::uint8_t>(refresh) > static_cast<std::uint8_t>(m_pendingGpuResourceRefresh))
    {
        m_pendingGpuResourceRefresh = refresh;
    }
}

void D3D12PathTracingBackend::RebuildLightList()
{
    Bistro::LightBuildResult lightBuild = Bistro::BuildLightList(m_scene);
    m_lights = std::move(lightBuild.lights);
    m_activeLightCount = lightBuild.activeLightCount;
    m_emissiveTriangleLightCount = lightBuild.emissiveTriangleCount;
    m_proceduralAreaLightCount = lightBuild.proceduralAreaCount;
}

void D3D12PathTracingBackend::RefreshEditableGpuResources(bool reloadTextures, bool rebuildLights)
{
    LogDiagnostic(std::string("Selective GPU refresh: ") +
        (reloadTextures ? "textures/material buffer" : "material buffer") +
        (rebuildLights ? " + light list/buffer." : "."));

    // Descriptor contents and resource ComPtrs can only be replaced once all
    // in-flight frames have finished using them. Output/history textures,
    // descriptor heap, root signatures, pipelines, constant buffers and ASes
    // deliberately remain untouched.
    WaitForPreviousFrame();
    m_uploadBuffers.clear();

    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    ThrowIfFailed(frameContext.commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));

    if (reloadTextures)
    {
        m_rtMaterials.clear();
        m_rtMaterialExtensions.clear();
        m_rtTextureBindings.clear();
        m_materialTextureIndices.clear();
        m_textures.clear();
        m_textureResidentBytes = 0;
        CreateTextures();
    }
    else
    {
        CreateMaterialBuffer();
    }

    if (rebuildLights)
    {
        RebuildLightList();
        CreateLightBuffer();
    }

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
    WaitForPreviousFrame();
    m_uploadBuffers.clear();
}

void D3D12PathTracingBackend::BeginAsyncSceneLoad(const std::wstring& path)
{
    if (path.empty()) return;
    if (m_sceneLoadFuture.valid() || m_sceneLoadCpuResult)
    {
        m_sceneLoadCancelRequested.store(true, std::memory_order_relaxed);
        m_queuedSceneLoadPath = path;
        m_sceneDiagnostics = "Cancelling the current scene load before starting the queued scene.";
        return;
    }

    m_sceneLoadCancelRequested.store(false, std::memory_order_relaxed);
    m_sceneLoadCompleted.store(0, std::memory_order_relaxed);
    m_sceneLoadTotal.store(0, std::memory_order_relaxed);
    m_sceneLoadStage.store(SceneLoadStage::Parsing, std::memory_order_relaxed);
    {
        std::scoped_lock lock(m_sceneLoadProgressMutex);
        m_sceneLoadCurrentAsset = path;
    }
    m_sceneDiagnostics = "Parsing scene...";
    m_sceneLoadFuture = std::async(std::launch::async, [this, path]()
    {
        const std::filesystem::path scenePath(path);
        if (rb::IsPbrtScenePath(scenePath))
        {
            return rb::ImportPbrtScene(path, &m_sceneLoadCancelRequested,
                [this](const rb::SceneImportProgress& progress)
                {
                    SceneLoadStage stage = SceneLoadStage::Parsing;
                    if (progress.stage == rb::SceneImportProgress::Stage::LoadingAssets)
                        stage = SceneLoadStage::LoadingAssets;
                    m_sceneLoadStage.store(stage, std::memory_order_relaxed);
                    m_sceneLoadCompleted.store(progress.completed, std::memory_order_relaxed);
                    m_sceneLoadTotal.store(progress.total, std::memory_order_relaxed);
                    std::scoped_lock lock(m_sceneLoadProgressMutex);
                    m_sceneLoadCurrentAsset = progress.currentAsset;
                });
        }
        m_sceneLoadStage.store(SceneLoadStage::LoadingAssets, std::memory_order_relaxed);
        return rb::SceneImporter{}.ImportScene(path);
    });
}

void D3D12PathTracingBackend::CancelAsyncSceneLoad()
{
    if (m_sceneLoadFuture.valid() || m_sceneLoadCpuResult)
    {
        m_sceneLoadCancelRequested.store(true, std::memory_order_relaxed);
        m_sceneDiagnostics = "Cancelling scene load...";
    }
}

void D3D12PathTracingBackend::PollAsyncSceneLoad()
{
    if (m_sceneLoadCpuResult)
    {
        if (m_sceneLoadCancelRequested.load(std::memory_order_relaxed))
        {
            m_sceneLoadCpuResult.reset();
            m_sceneLoadStage.store(SceneLoadStage::Cancelled, std::memory_order_relaxed);
            m_sceneDiagnostics = "Scene load cancelled; the previous scene is unchanged.";
        }
        else
        {
            rb::SceneImportResult imported = std::move(*m_sceneLoadCpuResult);
            m_sceneLoadCpuResult.reset();
            std::string diagnostics;
            const std::wstring path = imported.scene.path;
            m_sceneLoadStage.store(SceneLoadStage::BuildingBLAS, std::memory_order_relaxed);
            if (CommitImportedScene(path, std::move(imported), diagnostics))
                m_sceneLoadStage.store(SceneLoadStage::Completed, std::memory_order_relaxed);
            else
            {
                m_sceneLoadStage.store(SceneLoadStage::Failed, std::memory_order_relaxed);
                m_sceneDiagnostics = diagnostics;
            }
        }
    }
    else if (m_sceneLoadFuture.valid() &&
        m_sceneLoadFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        rb::SceneImportResult imported;
        try
        {
            imported = m_sceneLoadFuture.get();
        }
        catch (const std::exception& exception)
        {
            imported.diagnostics = exception.what();
        }
        if (m_sceneLoadCancelRequested.load(std::memory_order_relaxed))
        {
            m_sceneLoadStage.store(SceneLoadStage::Cancelled, std::memory_order_relaxed);
            m_sceneDiagnostics = "Scene load cancelled; the previous scene is unchanged.";
        }
        else if (!imported.succeeded)
        {
            m_sceneLoadStage.store(SceneLoadStage::Failed, std::memory_order_relaxed);
            m_sceneDiagnostics = imported.diagnostics;
        }
        else
        {
            m_sceneLoadStage.store(SceneLoadStage::BuildingBLAS, std::memory_order_relaxed);
            m_sceneDiagnostics = "CPU scene data is ready; building BLAS...";
            m_sceneLoadCpuResult = std::move(imported);
        }
    }

    if (!m_sceneLoadFuture.valid() && !m_sceneLoadCpuResult && !m_queuedSceneLoadPath.empty())
    {
        const std::wstring queued = std::move(m_queuedSceneLoadPath);
        m_queuedSceneLoadPath.clear();
        BeginAsyncSceneLoad(queued);
    }
}

bool D3D12PathTracingBackend::LoadScenePath(const std::wstring& path, std::string& diagnostics)
{
    LogDiagnostic(L"LoadScenePath: importing " + path);
    rb::SceneImportResult imported = m_sceneImporter.ImportScene(path);
    if (!imported.succeeded)
    {
        diagnostics = imported.diagnostics;
        LogDiagnostic("LoadScenePath: import failed: " + diagnostics);
        return false;
    }

    return CommitImportedScene(path, std::move(imported), diagnostics);
}

bool D3D12PathTracingBackend::CommitImportedScene(
    const std::wstring& path,
    rb::SceneImportResult&& imported,
    std::string& diagnostics)
{
    Bistro::Scene previousScene = m_scene;
    const std::wstring previousScenePath = m_scenePath;
    const std::string previousSceneDiagnostics = m_sceneDiagnostics;
    const Bistro::FpsCamera previousCamera = m_camera;
    const XMFLOAT3 previousDefaultCameraPosition = m_defaultCameraPosition;
    const float previousDefaultCameraYaw = m_defaultCameraYaw;
    const float previousDefaultCameraPitch = m_defaultCameraPitch;
    const float previousDefaultCameraRoll = m_defaultCameraRoll;
    const float previousDefaultCameraFov = m_defaultCameraFovDegrees;
    const float previousCameraFov = m_cameraFovDegrees;
    const std::wstring previousEnvironmentPath = m_environmentTexturePath;
    const bool previousEnvironmentEnabled = m_environmentMapEnabled;
    const bool previousEnvironmentEqualAreaMapping = m_environmentEqualAreaMapping;
    const float previousEnvironmentIntensity = m_environmentIntensity;
    const float previousEnvironmentRotation = m_environmentRotation;
    const XMFLOAT4X4 previousEnvironmentLightToWorld = m_environmentLightToWorld;
    const XMFLOAT4X4 previousEnvironmentWorldToLight = m_environmentWorldToLight;
    const XMFLOAT3 previousEnvironmentTint = m_environmentTint;
    const float previousLightIntensity = m_lightIntensity;
    const float previousSunIntensity = m_sunIntensity;
    const bool previousSkyEnabled = m_skyEnabled;
    const float previousProceduralLightIntensity = m_proceduralLightIntensity;
    const bool previousProjectDirty = m_projectDirty;
    const auto previousSourceMaterials = m_sourceMaterials;
    const auto previousTextureOverrides = m_textureOverrideEnabled;
    const auto previousTextureBindingOverrides = m_textureBindingOverrideEnabled;
    const auto previousMaterialUsage = m_materialUsage;
    const auto previousSceneAuditSummary = m_mcpSceneAuditSummary;

    try
    {
        LogDiagnostic("LoadScenePath: import succeeded. Waiting for GPU.");
        WaitForPreviousFrame();
        LogDiagnostic("LoadScenePath: converting scene.");
        m_scene = ConvertImportedScene(imported.scene);
        InvalidateHistory(rb::FrameChangeMask::Geometry);
        m_mcpSceneAuditSummary = lookdevpt::review::AnalyzeScene(m_scene);
        m_mcpSceneAuditFresh = true;
        m_scenePath = path;
        m_sceneDiagnostics = imported.diagnostics;
        InitializeMaterialLookDevState(true);

        const float sx = m_scene.boundsMax.x - m_scene.boundsMin.x;
        const float sy = m_scene.boundsMax.y - m_scene.boundsMin.y;
        const float sz = m_scene.boundsMax.z - m_scene.boundsMin.z;
        const float radius = (std::max)(1.0f, 0.5f * std::sqrt(sx * sx + sy * sy + sz * sz));
        const XMFLOAT3 center(
            (m_scene.boundsMin.x + m_scene.boundsMax.x) * 0.5f,
            (m_scene.boundsMin.y + m_scene.boundsMax.y) * 0.5f,
            (m_scene.boundsMin.z + m_scene.boundsMax.z) * 0.5f);
        m_defaultCameraPosition = XMFLOAT3(center.x, center.y + radius * 0.25f, center.z - radius * 2.5f);
        m_defaultCameraYaw = 0.0f;
        m_defaultCameraPitch = XMConvertToRadians(5.0f);
        m_defaultCameraRoll = 0.0f;
        m_defaultCameraFovDegrees = 60.0f;
        if (imported.scene.camera)
        {
            const rb::SceneCamera& camera = *imported.scene.camera;
            XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&camera.forward));
            const float forwardY = std::clamp(XMVectorGetY(forward), -1.0f, 1.0f);
            m_defaultCameraPosition = camera.position;
            m_defaultCameraYaw = std::atan2(XMVectorGetX(forward), XMVectorGetZ(forward));
            m_defaultCameraPitch = std::asin(forwardY);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
            if (XMVectorGetX(XMVector3LengthSq(right)) < 1.0e-8f) right = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
            const XMVECTOR baseUp = XMVector3Normalize(XMVector3Cross(forward, right));
            const XMVECTOR authoredUp = XMVector3Normalize(XMLoadFloat3(&camera.up));
            m_defaultCameraRoll = std::atan2(
                XMVectorGetX(XMVector3Dot(authoredUp, right)),
                XMVectorGetX(XMVector3Dot(authoredUp, baseUp)));
            m_defaultCameraFovDegrees = std::clamp(camera.fovDegrees, 1.0f, 179.0f);
        }
        if (imported.scene.environment)
        {
            const rb::SceneEnvironment& environment = *imported.scene.environment;
            m_environmentTexturePath = environment.texturePath;
            // An infinite light with no filename is a valid constant PBRT
            // environment, represented by the white fallback texture.
            m_environmentMapEnabled = true;
            m_environmentEqualAreaMapping = environment.equalAreaMapping;
            m_environmentIntensity = (std::max)({ environment.scale.x, environment.scale.y, environment.scale.z, 0.0f });
            const float inverseIntensity = m_environmentIntensity > 0.0f ? 1.0f / m_environmentIntensity : 0.0f;
            m_environmentTint = XMFLOAT3(
                environment.scale.x * inverseIntensity,
                environment.scale.y * inverseIntensity,
                environment.scale.z * inverseIntensity);
            XMMATRIX lightToWorld = XMLoadFloat4x4(&environment.lightToWorld);
            const XMVECTOR determinant = XMMatrixDeterminant(lightToWorld);
            if (!std::isfinite(XMVectorGetX(determinant)) || std::abs(XMVectorGetX(determinant)) < 1.0e-8f)
            {
                throw std::runtime_error("PBRT infinite-light transform is singular.");
            }
            const XMMATRIX worldToLight = XMMatrixInverse(nullptr, lightToWorld);
            XMStoreFloat4x4(&m_environmentLightToWorld, lightToWorld);
            XMStoreFloat4x4(&m_environmentWorldToLight, worldToLight);
            // The PBRT CTM is represented by the full matrices above. The
            // legacy yaw control remains an additional user rotation.
            m_environmentRotation = 0.0f;
        }
        else if (rb::IsPbrtScenePath(std::filesystem::path(path)))
        {
            m_environmentTexturePath.clear();
            m_environmentMapEnabled = false;
            m_environmentEqualAreaMapping = false;
            m_environmentTint = XMFLOAT3(1.0f, 1.0f, 1.0f);
            XMStoreFloat4x4(&m_environmentLightToWorld, XMMatrixIdentity());
            XMStoreFloat4x4(&m_environmentWorldToLight, XMMatrixIdentity());
            m_environmentRotation = 0.0f;
        }
        if (imported.scene.hasAuthoredLighting)
        {
            m_lightIntensity = 0.0f;
            m_sunIntensity = 0.0f;
            m_skyEnabled = false;
            m_proceduralLightIntensity = 1.0f;
        }
        ResetCameraView();
        LogDiagnostic("LoadScenePath: creating GPU resources for imported scene.");
        CreateGpuResourcesForCurrentScene();
        diagnostics = imported.diagnostics;
        m_projectDirty = true;
        LogDiagnostic("LoadScenePath: complete.");
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        LogDiagnostic("LoadScenePath: exception: " + diagnostics);
        m_scene = std::move(previousScene);
        m_scenePath = previousScenePath;
        m_sceneDiagnostics = previousSceneDiagnostics;
        m_camera = previousCamera;
        m_defaultCameraPosition = previousDefaultCameraPosition;
        m_defaultCameraYaw = previousDefaultCameraYaw;
        m_defaultCameraPitch = previousDefaultCameraPitch;
        m_defaultCameraRoll = previousDefaultCameraRoll;
        m_defaultCameraFovDegrees = previousDefaultCameraFov;
        m_cameraFovDegrees = previousCameraFov;
        m_environmentTexturePath = previousEnvironmentPath;
        m_environmentMapEnabled = previousEnvironmentEnabled;
        m_environmentEqualAreaMapping = previousEnvironmentEqualAreaMapping;
        m_environmentIntensity = previousEnvironmentIntensity;
        m_environmentRotation = previousEnvironmentRotation;
        m_environmentLightToWorld = previousEnvironmentLightToWorld;
        m_environmentWorldToLight = previousEnvironmentWorldToLight;
        m_environmentTint = previousEnvironmentTint;
        m_lightIntensity = previousLightIntensity;
        m_sunIntensity = previousSunIntensity;
        m_skyEnabled = previousSkyEnabled;
        m_proceduralLightIntensity = previousProceduralLightIntensity;
        m_projectDirty = previousProjectDirty;
        m_sourceMaterials = previousSourceMaterials;
        m_textureOverrideEnabled = previousTextureOverrides;
        m_textureBindingOverrideEnabled = previousTextureBindingOverrides;
        m_materialUsage = previousMaterialUsage;
        m_mcpSceneAuditSummary = previousSceneAuditSummary;
        m_mcpSceneAuditFresh = true;
        try
        {
            CreateGpuResourcesForCurrentScene();
            diagnostics += " The previous scene was restored.";
        }
        catch (const std::exception& rollbackException)
        {
            diagnostics += std::string(" Previous-scene GPU rollback failed: ") + rollbackException.what();
        }
        return false;
    }
}

bool D3D12PathTracingBackend::LoadEnvironmentPath(const std::wstring& path, std::string& diagnostics)
{
    if (!path.empty() && !std::filesystem::exists(path))
    {
        diagnostics = "Environment texture was not found.";
        return false;
    }
    const std::wstring previousPath = m_environmentTexturePath;
    const bool previousEnabled = m_environmentMapEnabled;
    const bool previousEqualAreaMapping = m_environmentEqualAreaMapping;
    const XMFLOAT4X4 previousLightToWorld = m_environmentLightToWorld;
    const XMFLOAT4X4 previousWorldToLight = m_environmentWorldToLight;
    const XMFLOAT3 previousTint = m_environmentTint;
    try
    {
        m_environmentTexturePath = path;
        m_environmentMapEnabled = !path.empty();
        m_environmentEqualAreaMapping = false;
        m_environmentTint = XMFLOAT3(1.0f, 1.0f, 1.0f);
        XMStoreFloat4x4(&m_environmentLightToWorld, XMMatrixIdentity());
        XMStoreFloat4x4(&m_environmentWorldToLight, XMMatrixIdentity());
        InvalidateHistory(rb::FrameChangeMask::Hdri);
        RefreshEditableGpuResources(true, false);
        diagnostics = path.empty() ? "Environment texture cleared." : "Environment texture loaded.";
        m_projectDirty = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        m_environmentTexturePath = previousPath;
        m_environmentMapEnabled = previousEnabled;
        m_environmentEqualAreaMapping = previousEqualAreaMapping;
        m_environmentLightToWorld = previousLightToWorld;
        m_environmentWorldToLight = previousWorldToLight;
        m_environmentTint = previousTint;
        try
        {
            RefreshEditableGpuResources(true, false);
        }
        catch (const std::exception& rollbackException)
        {
            diagnostics = std::string(ex.what()) + " Environment rollback also failed: " + rollbackException.what();
            return false;
        }
        diagnostics = ex.what();
        return false;
    }
}

void D3D12PathTracingBackend::InitializeMaterialLookDevState(bool clearVariants)
{
    m_sourceMaterials = m_scene.materials;
    m_textureOverrideEnabled.assign(m_scene.materials.size(), {});
    m_textureBindingOverrideEnabled.assign(m_scene.materials.size(), {});
    m_selectedMaterial = std::clamp(m_selectedMaterial, 0, (std::max)(0, static_cast<int>(m_scene.materials.size()) - 1));
    m_hasMaterialCompareA = false;
    m_hasMaterialCompareB = false;
    if (clearVariants)
    {
        m_materialVariants.clear();
    }
    RebuildMaterialUsage();
    LoadMaterialPresets();
}

void D3D12PathTracingBackend::RebuildMaterialUsage()
{
    m_materialUsage.assign(m_scene.materials.size(), {});
    m_sceneSubmittedIndexCount = 0;
    m_scenePrimitiveCount = 0;
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        m_sceneSubmittedIndexCount += draw.indexCount;
        m_scenePrimitiveCount += draw.indexCount / 3u;
        if (draw.materialIndex >= m_materialUsage.size())
        {
            continue;
        }
        MaterialUsage& usage = m_materialUsage[draw.materialIndex];
        ++usage.meshCount;
        usage.triangleCount += draw.indexCount / 3u;
    }
}

int D3D12PathTracingBackend::ResolveMaterialIndex(const cld::JsonValue& params) const
{
    int materialIndex = static_cast<int>(cld::JsonNumberOr(params, "index", -1.0));
    const std::string materialNameUtf8 = cld::JsonStringOr(params, "name");
    if (materialIndex < 0 && !materialNameUtf8.empty())
    {
        const std::wstring materialName = Utf8ToWide(materialNameUtf8);
        for (size_t i = 0; i < m_scene.materials.size(); ++i)
        {
            if (m_scene.materials[i].name == materialName)
            {
                materialIndex = static_cast<int>(i);
                break;
            }
        }
    }
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
    {
        return -1;
    }
    return materialIndex;
}

D3D12PathTracingBackend::MaterialSnapshot D3D12PathTracingBackend::CaptureMaterialSnapshot(int materialIndex) const
{
    MaterialSnapshot snapshot;
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
    {
        return snapshot;
    }
    const Bistro::Material& material = m_scene.materials[materialIndex];
    snapshot.sourceMaterialId = material.sourceMaterialId;
    snapshot.baseColorFactor = material.baseColorFactor;
    snapshot.emissiveFactor = material.emissiveFactor;
    snapshot.roughnessFactor = material.roughnessFactor;
    snapshot.metallicFactor = material.metallicFactor;
    snapshot.occlusionStrength = material.occlusionStrength;
    snapshot.normalStrength = material.normalStrength;
    snapshot.alphaCutoff = material.alphaCutoff;
    snapshot.alphaMasked = material.alphaMasked;
    snapshot.packedOcclusionRoughnessMetallic = material.packedOcclusionRoughnessMetallic;
    snapshot.extensionFeatureMask = material.extensionFeatureMask;
    snapshot.specularFactor = material.specularFactor;
    snapshot.specularColorFactor = material.specularColorFactor;
    snapshot.indexOfRefraction = material.indexOfRefraction;
    snapshot.transmissionFactor = material.transmissionFactor;
    snapshot.thicknessFactor = material.thicknessFactor;
    snapshot.attenuationColor = material.attenuationColor;
    snapshot.attenuationDistance = material.attenuationDistance;
    snapshot.clearcoatFactor = material.clearcoatFactor;
    snapshot.clearcoatRoughnessFactor = material.clearcoatRoughnessFactor;
    snapshot.clearcoatNormalScale = material.clearcoatNormalScale;
    snapshot.textures = material.textures;
    snapshot.textureBindings = material.textureBindings;
    if (static_cast<size_t>(materialIndex) < m_textureOverrideEnabled.size())
    {
        snapshot.textureOverrideEnabled = m_textureOverrideEnabled[materialIndex];
    }
    if (static_cast<size_t>(materialIndex) < m_textureBindingOverrideEnabled.size())
    {
        snapshot.textureBindingOverrideEnabled = m_textureBindingOverrideEnabled[materialIndex];
    }
    return snapshot;
}

bool D3D12PathTracingBackend::RequiresMaterialTextureReload(
    const MaterialSnapshot& before,
    const MaterialSnapshot& after)
{
    // Alpha coverage is baked into the base-color mip chain. Changing any
    // part of its effective threshold therefore requires regenerating the
    // texture resource even when the source path itself is unchanged.
    bool bindingChanged = false;
    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
    {
        const Bistro::TextureBinding& a = before.textureBindings[slot];
        const Bistro::TextureBinding& b = after.textureBindings[slot];
        bindingChanged = bindingChanged || a.offset.x != b.offset.x || a.offset.y != b.offset.y ||
            a.scale.x != b.scale.x || a.scale.y != b.scale.y || a.rotation != b.rotation ||
            a.texCoord != b.texCoord || a.samplerIndex != b.samplerIndex ||
            a.resolutionPolicy != b.resolutionPolicy;
    }
    return before.textures != after.textures
        || bindingChanged
        || before.alphaMasked != after.alphaMasked
        || before.alphaCutoff != after.alphaCutoff
        || before.baseColorFactor.w != after.baseColorFactor.w;
}

bool D3D12PathTracingBackend::RequiresMaterialBlasRebuild(
    const MaterialSnapshot& before,
    const MaterialSnapshot& after)
{
    // Opaque geometry bypasses AnyHit. The BLAS geometry flag therefore only
    // changes when the alpha-mask mode itself changes; cutoff/factor edits
    // merely regenerate coverage-preserving mips.
    return before.alphaMasked != after.alphaMasked;
}

void D3D12PathTracingBackend::ApplyMaterialSnapshot(int materialIndex, const MaterialSnapshot& snapshot, bool useSnapshotTextureFlags)
{
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
    {
        return;
    }
    Bistro::Material& material = m_scene.materials[materialIndex];
    const bool alphaModeChanged = material.alphaMasked != snapshot.alphaMasked;
    material.sourceMaterialId = snapshot.sourceMaterialId;
    material.baseColorFactor = snapshot.baseColorFactor;
    material.emissiveFactor = snapshot.emissiveFactor;
    material.roughnessFactor = snapshot.roughnessFactor;
    material.metallicFactor = snapshot.metallicFactor;
    material.occlusionStrength = snapshot.occlusionStrength;
    material.normalStrength = snapshot.normalStrength;
    material.alphaCutoff = snapshot.alphaCutoff;
    material.alphaMasked = snapshot.alphaMasked;
    material.packedOcclusionRoughnessMetallic = snapshot.packedOcclusionRoughnessMetallic;
    material.extensionFeatureMask = snapshot.extensionFeatureMask;
    material.specularFactor = snapshot.specularFactor;
    material.specularColorFactor = snapshot.specularColorFactor;
    material.indexOfRefraction = snapshot.indexOfRefraction;
    material.transmissionFactor = snapshot.transmissionFactor;
    material.thicknessFactor = snapshot.thicknessFactor;
    material.attenuationColor = snapshot.attenuationColor;
    material.attenuationDistance = snapshot.attenuationDistance;
    material.clearcoatFactor = snapshot.clearcoatFactor;
    material.clearcoatRoughnessFactor = snapshot.clearcoatRoughnessFactor;
    material.clearcoatNormalScale = snapshot.clearcoatNormalScale;
    material.textures = snapshot.textures;
    material.textureBindings = snapshot.textureBindings;
    if (useSnapshotTextureFlags && static_cast<size_t>(materialIndex) < m_textureOverrideEnabled.size())
    {
        m_textureOverrideEnabled[materialIndex] = snapshot.textureOverrideEnabled;
    }
    if (useSnapshotTextureFlags && static_cast<size_t>(materialIndex) < m_textureBindingOverrideEnabled.size())
    {
        m_textureBindingOverrideEnabled[materialIndex] = snapshot.textureBindingOverrideEnabled;
    }
    InvalidateHistory(alphaModeChanged ? rb::FrameChangeMask::Geometry : rb::FrameChangeMask::Material);
}

void D3D12PathTracingBackend::ResetMaterialToSource(int materialIndex)
{
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size() ||
        static_cast<size_t>(materialIndex) >= m_sourceMaterials.size())
    {
        return;
    }
    m_scene.materials[materialIndex] = m_sourceMaterials[materialIndex];
    if (static_cast<size_t>(materialIndex) < m_textureOverrideEnabled.size())
    {
        m_textureOverrideEnabled[materialIndex].fill(false);
    }
    if (static_cast<size_t>(materialIndex) < m_textureBindingOverrideEnabled.size())
    {
        m_textureBindingOverrideEnabled[materialIndex].fill(false);
    }
    InvalidateHistory(rb::FrameChangeMask::Material);
}

bool D3D12PathTracingBackend::ValidateMaterialTexturePath(const std::wstring& path, std::string& diagnostics) const
{
    if (path.empty())
    {
        return true;
    }
    if (!std::filesystem::exists(path))
    {
        diagnostics = "Texture path does not exist.";
        return false;
    }
    if (!IsSupportedMaterialTextureExtension(path))
    {
        diagnostics = "Texture extension is not supported.";
        return false;
    }

    const uint8_t fallback[] = { 255, 255, 255, 255 };
    const Bistro::TextureData texture = Bistro::LoadTextureD3D12(path, false, fallback);
    if (texture.fallback)
    {
        diagnostics = "Texture could not be decoded.";
        return false;
    }
    return true;
}

bool D3D12PathTracingBackend::TryParseTextureSlot(const cld::JsonValue& value, UINT& slot) const
{
    if (value.type == cld::JsonValue::Type::Number)
    {
        const int index = static_cast<int>(value.number);
        if (index >= 0 && index < static_cast<int>(TextureSlotCount))
        {
            slot = static_cast<UINT>(index);
            return true;
        }
        return false;
    }
    if (value.type != cld::JsonValue::Type::String)
    {
        return false;
    }

    const std::string key = KeyFromLabel(value.string);
    for (UINT i = 0; i < TextureSlotCount; ++i)
    {
        if (value.string == TextureSlotKeys[i] || value.string == TextureSlotLabels[i] || key == KeyFromLabel(TextureSlotLabels[i]))
        {
            slot = i;
            return true;
        }
    }
    return false;
}

bool D3D12PathTracingBackend::ApplyMaterialTextureOverride(int materialIndex, UINT slot, const std::wstring& path, bool enableOverride, std::string& diagnostics)
{
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size() || slot >= TextureSlotCount)
    {
        diagnostics = "Material index or texture slot is invalid.";
        return false;
    }
    if (enableOverride && !ValidateMaterialTexturePath(path, diagnostics))
    {
        return false;
    }

    m_scene.materials[materialIndex].textures[slot] = enableOverride
        ? path
        : (static_cast<size_t>(materialIndex) < m_sourceMaterials.size() ? m_sourceMaterials[materialIndex].textures[slot] : std::wstring());
    if (static_cast<size_t>(materialIndex) < m_textureOverrideEnabled.size())
    {
        m_textureOverrideEnabled[materialIndex][slot] = enableOverride;
    }
    InvalidateHistory(rb::FrameChangeMask::Material);
    diagnostics = "Material texture accepted.";
    return true;
}

void D3D12PathTracingBackend::LoadMaterialPresets()
{
    ++m_mcpMaterialCatalogRevision;
    m_materialPresets.clear();

    auto addPreset = [&](const char* name, const char* category, const XMFLOAT4& baseColor, float roughness, float metallic)
    {
        MaterialPreset preset;
        preset.name = name;
        preset.category = category;
        preset.snapshot.baseColorFactor = baseColor;
        preset.snapshot.roughnessFactor = roughness;
        preset.snapshot.metallicFactor = metallic;
        m_materialPresets.push_back(preset);
    };

    addPreset("Neutral Clay", "Built-in", XMFLOAT4(0.70f, 0.68f, 0.62f, 1.0f), 0.55f, 0.0f);
    addPreset("Matte Plastic", "Built-in", XMFLOAT4(0.18f, 0.22f, 0.28f, 1.0f), 0.72f, 0.0f);
    addPreset("Brushed Dark Metal", "Built-in", XMFLOAT4(0.58f, 0.60f, 0.62f, 1.0f), 0.34f, 1.0f);
    addPreset("Warm Emissive", "Built-in", XMFLOAT4(1.0f, 0.78f, 0.42f, 1.0f), 0.48f, 0.0f);
    m_materialPresets.back().snapshot.emissiveFactor = XMFLOAT4(1.0f, 0.62f, 0.22f, 1.8f);

    const std::filesystem::path presetDir(MaterialPresetDirectory());
    if (!std::filesystem::exists(presetDir))
    {
        return;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(presetDir))
    {
        if (!entry.is_regular_file() || entry.path().extension() != L".json")
        {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        if (!file)
        {
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        try
        {
            const cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
            if (root.type != cld::JsonValue::Type::Object)
            {
                continue;
            }
            MaterialPreset preset;
            preset.name = cld::JsonStringOr(root, "name", entry.path().stem().string());
            preset.category = cld::JsonStringOr(root, "category", "User");
            preset.sourcePath = entry.path().wstring();
            const std::array<float, 4> baseColor = cld::JsonFloat4Or(root, "baseColor", Float4ToArray(preset.snapshot.baseColorFactor));
            const std::array<float, 4> emissive = cld::JsonFloat4Or(root, "emissive", Float4ToArray(preset.snapshot.emissiveFactor));
            preset.snapshot.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
            preset.snapshot.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
            preset.snapshot.roughnessFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(root, "roughness", preset.snapshot.roughnessFactor)), 0.02f, 1.0f);
            preset.snapshot.metallicFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(root, "metallic", preset.snapshot.metallicFactor)), 0.0f, 1.0f);
            preset.snapshot.occlusionStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(root, "occlusionStrength", preset.snapshot.occlusionStrength)), 0.0f, 2.0f);
            preset.snapshot.normalStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(root, "normalStrength", preset.snapshot.normalStrength)), 0.0f, 2.0f);
            preset.snapshot.alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(root, "alphaCutoff", preset.snapshot.alphaCutoff)), 0.0f, 1.0f);
            preset.snapshot.alphaMasked = cld::JsonBoolOr(root, "alphaMasked", preset.snapshot.alphaMasked);
            preset.snapshot.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(root, "packedORM", preset.snapshot.packedOcclusionRoughnessMetallic);
            if (const cld::JsonValue* textures = cld::FindMember(root, "textures"); textures && textures->type == cld::JsonValue::Type::Object)
            {
                for (UINT slot = 0; slot < TextureSlotCount; ++slot)
                {
                    const std::string pathText = cld::JsonStringOr(*textures, TextureSlotKeys[slot]);
                    if (pathText.empty())
                    {
                        continue;
                    }
                    std::filesystem::path texturePath = Utf8ToWide(pathText);
                    if (texturePath.is_relative())
                    {
                        texturePath = entry.path().parent_path() / texturePath;
                    }
                    std::string textureDiagnostics;
                    if (ValidateMaterialTexturePath(texturePath.wstring(), textureDiagnostics))
                    {
                        preset.snapshot.textures[slot] = texturePath.wstring();
                        preset.snapshot.textureOverrideEnabled[slot] = true;
                    }
                }
            }
            m_materialPresets.push_back(std::move(preset));
        }
        catch (const std::exception&)
        {
        }
    }
}

bool D3D12PathTracingBackend::SaveUserMaterialPreset(const std::string& name, int materialIndex, std::string& diagnostics)
{
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
    {
        diagnostics = "Material index was not found.";
        return false;
    }
    const std::filesystem::path presetDir(MaterialPresetDirectory());
    std::error_code ec;
    std::filesystem::create_directories(presetDir, ec);
    if (ec)
    {
        diagnostics = "Could not create material preset directory.";
        return false;
    }

    const std::string safeName = SafeFileName(name.empty() ? "preset" : name);
    const std::filesystem::path presetPath = presetDir / Utf8ToWide(safeName + ".json");
    std::ofstream file(presetPath, std::ios::binary);
    if (!file)
    {
        diagnostics = "Could not write material preset.";
        return false;
    }

    const Bistro::Material& material = m_scene.materials[materialIndex];
    file << "{\n";
    file << "  \"name\": \"" << cld::EscapeJson(name.empty() ? safeName : name) << "\",\n";
    file << "  \"category\": \"User\",\n";
    file << "  \"baseColor\": [" << material.baseColorFactor.x << ", " << material.baseColorFactor.y << ", " << material.baseColorFactor.z << ", " << material.baseColorFactor.w << "],\n";
    file << "  \"emissive\": [" << material.emissiveFactor.x << ", " << material.emissiveFactor.y << ", " << material.emissiveFactor.z << ", " << material.emissiveFactor.w << "],\n";
    file << "  \"roughness\": " << material.roughnessFactor << ",\n";
    file << "  \"metallic\": " << material.metallicFactor << ",\n";
    file << "  \"occlusionStrength\": " << material.occlusionStrength << ",\n";
    file << "  \"normalStrength\": " << material.normalStrength << ",\n";
    file << "  \"alphaCutoff\": " << material.alphaCutoff << ",\n";
    file << "  \"alphaMasked\": " << (material.alphaMasked ? "true" : "false") << ",\n";
    file << "  \"packedORM\": " << (material.packedOcclusionRoughnessMetallic ? "true" : "false") << ",\n";
    file << "  \"textures\": {";
    bool wroteTexture = false;
    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
    {
        if (static_cast<size_t>(materialIndex) >= m_textureOverrideEnabled.size() || !m_textureOverrideEnabled[materialIndex][slot])
        {
            continue;
        }
        if (wroteTexture)
        {
            file << ", ";
        }
        file << "\"" << TextureSlotKeys[slot] << "\": \"" << cld::EscapeJson(WideToUtf8(material.textures[slot])) << "\"";
        wroteTexture = true;
    }
    file << "}\n";
    file << "}\n";
    diagnostics = "Material preset saved.";
    LoadMaterialPresets();
    return true;
}

bool D3D12PathTracingBackend::ApplyMaterialPreset(int materialIndex, size_t presetIndex, std::string& diagnostics)
{
    if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size() || presetIndex >= m_materialPresets.size())
    {
        diagnostics = "Material preset or material index was not found.";
        return false;
    }

    const MaterialSnapshot before = CaptureMaterialSnapshot(materialIndex);
    MaterialSnapshot snapshot = CaptureMaterialSnapshot(materialIndex);
    const MaterialSnapshot& preset = m_materialPresets[presetIndex].snapshot;
    snapshot.baseColorFactor = preset.baseColorFactor;
    snapshot.emissiveFactor = preset.emissiveFactor;
    snapshot.roughnessFactor = preset.roughnessFactor;
    snapshot.metallicFactor = preset.metallicFactor;
    snapshot.occlusionStrength = preset.occlusionStrength;
    snapshot.normalStrength = preset.normalStrength;
    snapshot.alphaCutoff = preset.alphaCutoff;
    snapshot.alphaMasked = preset.alphaMasked;
    snapshot.packedOcclusionRoughnessMetallic = preset.packedOcclusionRoughnessMetallic;
    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
    {
        if (preset.textureOverrideEnabled[slot])
        {
            snapshot.textures[slot] = preset.textures[slot];
            snapshot.textureOverrideEnabled[slot] = true;
        }
    }

    const bool reloadTextures = RequiresMaterialTextureReload(before, snapshot);
    const bool rebuildBlas = RequiresMaterialBlasRebuild(before, snapshot);
    ApplyMaterialSnapshot(materialIndex, snapshot, true);
    RequestGpuResourceRefresh(rebuildBlas
        ? PendingGpuResourceRefresh::FullScene
        : (reloadTextures ? PendingGpuResourceRefresh::MaterialTextures : PendingGpuResourceRefresh::MaterialData));
    diagnostics = "Material preset applied.";
    return true;
}

bool D3D12PathTracingBackend::SaveProjectToDisk(const std::wstring& path)
{
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        return false;
    }

    XMFLOAT3 camera = m_camera.GetPosition();
    file << "{\n";
    file << "  \"schemaVersion\": " << rb::DenoiseSettingsSchemaVersion << ",\n";
    file << "  \"scenePath\": \"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",\n";
    file << "  \"environmentPath\": \"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\",\n";
    file << "  \"mode\": \"" << PathtracingModeName(m_mode) << "\",\n";
    file << "  \"quality\": " << rb::QualitySettingsToJson(m_qualitySettings) << ",\n";
    file << "  \"camera\": {\"position\": [" << camera.x << ", " << camera.y << ", " << camera.z << "], \"yaw\": " << m_camera.GetYawRadians() << ", \"pitch\": " << m_camera.GetPitchRadians() << ", \"roll\": " << m_camera.GetRollRadians() << ", \"fovDegrees\": " << m_cameraFovDegrees << "},\n";
    file << "  \"lighting\": {\"direction\": [" << m_lightDirection[0] << ", " << m_lightDirection[1] << ", " << m_lightDirection[2] << "], \"intensity\": " << m_lightIntensity << "},\n";
    file << "  \"materials\": [\n";
    for (size_t i = 0; i < m_scene.materials.size(); ++i)
    {
        const Bistro::Material& material = m_scene.materials[i];
        const std::array<bool, TextureSlotCount> bindingOverrides = i < m_textureBindingOverrideEnabled.size()
            ? m_textureBindingOverrideEnabled[i]
            : std::array<bool, TextureSlotCount>{};
        file << "    {\"index\": " << i
             << ", \"name\": \"" << cld::EscapeJson(WideToUtf8(material.name)) << "\""
             << ", \"sourceMaterialId\": \"" << cld::EscapeJson(material.sourceMaterialId) << "\""
             << ", \"baseColor\": [" << material.baseColorFactor.x << ", " << material.baseColorFactor.y << ", " << material.baseColorFactor.z << ", " << material.baseColorFactor.w << "]"
             << ", \"emissive\": [" << material.emissiveFactor.x << ", " << material.emissiveFactor.y << ", " << material.emissiveFactor.z << ", " << material.emissiveFactor.w << "]"
             << ", \"roughness\": " << material.roughnessFactor
             << ", \"metallic\": " << material.metallicFactor
             << ", \"occlusionStrength\": " << material.occlusionStrength
             << ", \"normalStrength\": " << material.normalStrength
             << ", \"alphaCutoff\": " << material.alphaCutoff
             << ", \"alphaMasked\": " << (material.alphaMasked ? "true" : "false")
             << ", \"packedORM\": " << (material.packedOcclusionRoughnessMetallic ? "true" : "false")
             << ", \"variant\": \"\""
             << ", \"textures\": {";
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (slot > 0)
            {
                file << ", ";
            }
            file << "\"" << TextureSlotKeys[slot] << "\": \"" << cld::EscapeJson(WideToUtf8(material.textures[slot])) << "\"";
        }
        file << "}, \"textureOverridesEnabled\": {";
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (slot > 0)
            {
                file << ", ";
            }
            const bool enabled = i < m_textureOverrideEnabled.size() && m_textureOverrideEnabled[i][slot];
            file << "\"" << TextureSlotKeys[slot] << "\": " << (enabled ? "true" : "false");
        }
        file << "}, \"gltfExtensions\": ";
        WriteGltfExtensionsJson(file, material);
        file << ", \"textureBindings\": ";
        WriteTextureBindingsJson(file, material, bindingOverrides);
        file << ""
             << "}" << (i + 1 < m_scene.materials.size() ? "," : "") << "\n";
    }
    file << "  ],\n";
    file << "  \"materialVariants\": [\n";
    for (size_t i = 0; i < m_materialVariants.size(); ++i)
    {
        const MaterialVariant& variant = m_materialVariants[i];
        const MaterialSnapshot& snapshot = variant.snapshot;
        file << "    {\"name\": \"" << cld::EscapeJson(variant.name) << "\""
             << ", \"materialIndex\": " << variant.materialIndex
             << ", \"materialName\": \"" << cld::EscapeJson(WideToUtf8(variant.materialName)) << "\""
             << ", \"sourceMaterialId\": \"" << cld::EscapeJson(snapshot.sourceMaterialId) << "\""
             << ", \"baseColor\": [" << snapshot.baseColorFactor.x << ", " << snapshot.baseColorFactor.y << ", " << snapshot.baseColorFactor.z << ", " << snapshot.baseColorFactor.w << "]"
             << ", \"emissive\": [" << snapshot.emissiveFactor.x << ", " << snapshot.emissiveFactor.y << ", " << snapshot.emissiveFactor.z << ", " << snapshot.emissiveFactor.w << "]"
             << ", \"roughness\": " << snapshot.roughnessFactor
             << ", \"metallic\": " << snapshot.metallicFactor
             << ", \"occlusionStrength\": " << snapshot.occlusionStrength
             << ", \"normalStrength\": " << snapshot.normalStrength
             << ", \"alphaCutoff\": " << snapshot.alphaCutoff
             << ", \"alphaMasked\": " << (snapshot.alphaMasked ? "true" : "false")
             << ", \"packedORM\": " << (snapshot.packedOcclusionRoughnessMetallic ? "true" : "false")
             << ", \"textures\": {";
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (slot > 0) file << ", ";
            file << "\"" << TextureSlotKeys[slot] << "\": \"" << cld::EscapeJson(WideToUtf8(snapshot.textures[slot])) << "\"";
        }
        file << "}, \"textureOverridesEnabled\": {";
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (slot > 0) file << ", ";
            file << "\"" << TextureSlotKeys[slot] << "\": " << (snapshot.textureOverrideEnabled[slot] ? "true" : "false");
        }
        file << "}, \"gltfExtensions\": ";
        WriteGltfExtensionsJson(file, snapshot);
        file << ", \"textureBindings\": ";
        WriteTextureBindingsJson(file, snapshot, snapshot.textureBindingOverrideEnabled);
        file << "}" << (i + 1 < m_materialVariants.size() ? "," : "") << "\n";
    }
    file << "  ],\n";
    file << "  \"pathTracing\": {\"samplesPerFrame\": " << m_giSamplesPerFrame << ", \"maxBounces\": " << m_maxPathBounces << ", \"minBounces\": " << m_minPathBounces
         << ", \"radianceClamp\": " << m_giRadianceClamp << ", \"temporalClamp\": " << m_giTemporalClampScale
         << ", \"maxAccumSamples\": " << m_maxAccumulatedFrames << ", \"adaptiveSampling\": " << (m_adaptiveSamplingEnabled ? "true" : "false")
         << ", \"maxAdaptiveSPP\": " << m_maxAdaptiveSamplesPerPixel << ", \"varianceThreshold\": " << m_adaptiveVarianceThreshold
         << ", \"disocclusionBoost\": " << m_adaptiveDisocclusionBoost << "},\n";
    file << "  \"restir\": {\"temporalReuse\": " << (m_restirTemporalReuse ? "true" : "false") << ", \"spatialReusePasses\": " << m_restirSpatialReusePasses
         << ", \"spatialRadius\": " << m_restirSpatialRadius << ", \"candidateSamples\": " << m_restirCandidateSamples << ", \"mClamp\": " << m_restirMClamp
         << ", \"diTemporalReuse\": " << (m_restirDiTemporalReuse ? "true" : "false") << ", \"diSpatialReusePasses\": " << m_restirDiSpatialReusePasses
         << ", \"diCandidateSamples\": " << m_restirDiCandidateSamples << ", \"diMClamp\": " << m_restirDiMClamp
         << ", \"reservoirReprojection\": " << (m_reservoirReprojection ? "true" : "false")
         << ", \"reservoirValidation\": " << (m_reservoirValidation ? "true" : "false")
         << ", \"giValidationRay\": " << (m_restirGiValidationRay ? "true" : "false")
         << ", \"reservoirMaxAge\": " << m_reservoirMaxAge << "},\n";
    file << "  \"denoise\": {\"preset\": \"" << NoisePresetName(m_noisePreset) << "\", \"enabled\": " << (m_denoiserEnabled ? "true" : "false")
         << ", \"backend\": \"" << DenoiseBackendName(m_denoiseBackend) << "\""
         << ", \"dlssMode\": \"" << DlssModeName(m_dlssMode) << "\""
         << ", \"dlssEnabledWhenAvailable\": " << (m_dlssEnabledWhenAvailable ? "true" : "false")
         << ", \"splitSignalDenoise\": " << (m_splitSignalDenoise ? "true" : "false")
         << ", \"realtimeReconstruction\": " << (m_realtimeReconstruction ? "true" : "false")
         << ", \"cameraJitter\": " << (m_cameraJitter ? "true" : "false")
         << ", \"temporalStability\": " << (m_temporalStabilityEnabled ? "true" : "false")
         << ", \"jitterMode\": \"" << JitterModeName(m_jitterMode) << "\""
         << ", \"movingJitterScale\": " << m_movingJitterScale
         << ", \"maxHistoryFrames\": " << m_reconstructionMaxHistoryFrames
         << ", \"temporalAlphaMin\": " << m_temporalAlphaMin << ", \"temporalAlphaMax\": " << m_temporalAlphaMax
         << ", \"historyClampSigma\": " << m_historyClampSigma << ", \"reactiveThreshold\": " << m_reactiveThreshold
         << ", \"specularHistoryScale\": " << m_specularHistoryScale
         << ", \"atrousPasses\": " << m_atrousPassCount
         << ", \"diffuseFilterStrength\": " << m_atrousDiffuseStrength << ", \"specularFilterStrength\": " << m_atrousSpecularStrength
         << ", \"varianceScale\": " << m_atrousVarianceScale
         << ", \"normalSigma\": " << m_denoiserNormalSigma << ", \"depthSigma\": " << m_denoiserDepthSigma
         << ", \"luminanceSigma\": " << m_denoiserLuminanceSigma << ", \"albedoSigma\": " << m_denoiserAlbedoSigma
         << ", \"strength\": " << m_denoiserStrength << "},\n";
    file << "  \"view\": {\"debugView\": " << m_debugViewMode << ", \"environmentEnabled\": " << (m_environmentMapEnabled ? "true" : "false")
         << ", \"exposure\": " << m_exposure << ", \"gamma\": " << m_gamma
         << ", \"toneMapper\": \"" << ToneMapperName(m_toneMapper) << "\""
         << ", \"materialFocusMode\": \"" << MaterialFocusModeName(m_materialFocusMode) << "\""
         << ", \"selectedMaterial\": " << m_selectedMaterial << "}\n";
    file << "}\n";
    m_projectPath = path;
    m_projectDirty = false;
    m_projectDiagnostics = "Project saved.";
    return true;
}

bool D3D12PathTracingBackend::LoadProjectFromDisk(const std::wstring& path, std::string& diagnostics)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        diagnostics = "Project file was not found.";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    try
    {
        cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
        if (root.type != cld::JsonValue::Type::Object)
        {
            diagnostics = "Project root must be an object.";
            return false;
        }

        // Projects written before schemaVersion existed are v1.  Keep exactly
        // one migration window for denoise.spatialIterations, then require the
        // v2 atrousPasses spelling in newly written projects.
        const double schemaVersionValue = cld::JsonNumberOr(
            root,
            "schemaVersion",
            static_cast<double>(rb::LegacySpatialIterationsSchemaVersion));
        constexpr unsigned int BundleProjectSchemaVersion = 3;
        if (!std::isfinite(schemaVersionValue) || schemaVersionValue < 1.0 ||
            schemaVersionValue > static_cast<double>(BundleProjectSchemaVersion) ||
            std::floor(schemaVersionValue) != schemaVersionValue)
        {
            diagnostics = "Project schemaVersion is unsupported.";
            return false;
        }
        const unsigned int projectSchemaVersion = static_cast<unsigned int>(schemaVersionValue);
        const bool bundleProject = projectSchemaVersion == BundleProjectSchemaVersion;
        const std::filesystem::path projectFilePath(path);
        std::filesystem::path assetRoot;
        if (bundleProject)
        {
            const std::string assetRootText = cld::JsonStringOr(root, "assetRoot");
            if (assetRootText.empty())
            {
                diagnostics = "Bundle project schema v3 requires assetRoot.";
                return false;
            }
            assetRoot = Utf8ToWide(assetRootText);
        }
        auto resolveAssetPath = [&](const std::string& pathText) -> std::filesystem::path
        {
            const std::filesystem::path relativeOrAbsolute = Utf8ToWide(pathText);
            return bundleProject
                ? rb::ResolveProjectAssetPath(projectFilePath, relativeOrAbsolute, assetRoot)
                : rb::ResolveProjectAssetPath(projectFilePath, relativeOrAbsolute);
        };

        std::string denoiseCompatibilityWarning;
        const DenoiseBackend initialDenoiseBackend = m_denoiseBackend;
        const bool initialDenoiserEnabled = m_denoiserEnabled;
        const int initialDebugView = m_debugViewMode;
        bool explicitDenoiseBackend = false;
        DenoiseBackend projectDenoiseBackend = m_denoiseBackend;
        bool explicitDenoiserEnabled = false;
        bool projectDenoiserEnabled = m_denoiserEnabled;

        const PathTracingMode loadedMode = PathtracingModeFromName(cld::JsonStringOr(root, "mode"), m_mode);
        const bool modeChanged = loadedMode != m_mode;
        bool qualityResourcesChanged = false;
        m_mode = loadedMode;

        const std::string scenePathText = cld::JsonStringOr(root, "scenePath");
        const std::filesystem::path scenePath = resolveAssetPath(scenePathText);
        if (bundleProject && !scenePathText.empty() && scenePath.empty())
        {
            diagnostics = "Bundle scenePath is absolute or escapes assetRoot.";
            return false;
        }
        if (!scenePath.empty())
        {
            if (!LoadScenePath(scenePath.wstring(), diagnostics))
            {
                return false;
            }
        }
        if (cld::FindMember(root, "environmentPath"))
        {
            const std::string environmentPathText = cld::JsonStringOr(root, "environmentPath");
            const std::filesystem::path environmentPath = resolveAssetPath(environmentPathText);
            if (bundleProject && !environmentPathText.empty() && environmentPath.empty())
            {
                diagnostics = "Bundle environmentPath is absolute or escapes assetRoot.";
                return false;
            }
            std::string envDiagnostics;
            if (!LoadEnvironmentPath(environmentPath.wstring(), envDiagnostics))
            {
                diagnostics = envDiagnostics;
                return false;
            }
        }

        bool materialOverridesChanged = false;
        if (const cld::JsonValue* materials = cld::FindMember(root, "materials"); materials && materials->type == cld::JsonValue::Type::Array)
        {
            for (const cld::JsonValue& overrideValue : materials->array)
            {
                if (overrideValue.type != cld::JsonValue::Type::Object)
                {
                    continue;
                }

                const int storedMaterialIndex = static_cast<int>(cld::JsonNumberOr(overrideValue, "index", -1.0));
                int materialIndex = -1;
                const std::string sourceMaterialId = cld::JsonStringOr(overrideValue, "sourceMaterialId");
                const std::string name = cld::JsonStringOr(overrideValue, "name");
                if (!sourceMaterialId.empty())
                {
                    for (size_t i = 0; i < m_scene.materials.size(); ++i)
                    {
                        if (m_scene.materials[i].sourceMaterialId == sourceMaterialId)
                        {
                            materialIndex = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (materialIndex < 0 && !name.empty())
                {
                    const std::wstring materialName = Utf8ToWide(name);
                    for (size_t i = 0; i < m_scene.materials.size(); ++i)
                    {
                        if (m_scene.materials[i].name == materialName)
                        {
                            materialIndex = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (materialIndex < 0)
                {
                    materialIndex = storedMaterialIndex;
                }

                if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
                {
                    continue;
                }

                Bistro::Material& material = m_scene.materials[materialIndex];
                const std::array<float, 4> baseColor = cld::JsonFloat4Or(overrideValue, "baseColor", Float4ToArray(material.baseColorFactor));
                const std::array<float, 4> emissive = cld::JsonFloat4Or(overrideValue, "emissive", Float4ToArray(material.emissiveFactor));
                material.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
                material.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
                material.roughnessFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "roughness", material.roughnessFactor)), 0.02f, 1.0f);
                material.metallicFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "metallic", material.metallicFactor)), 0.0f, 1.0f);
                material.occlusionStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "occlusionStrength", material.occlusionStrength)), 0.0f, 2.0f);
                material.normalStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "normalStrength", material.normalStrength)), 0.0f, 2.0f);
                material.alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(overrideValue, "alphaCutoff", material.alphaCutoff)), 0.0f, 1.0f);
                material.alphaMasked = cld::JsonBoolOr(overrideValue, "alphaMasked", material.alphaMasked);
                material.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(overrideValue, "packedORM", material.packedOcclusionRoughnessMetallic);
                ReadGltfExtensionsJson(overrideValue, material);
                if (const cld::JsonValue* textures = cld::FindMember(overrideValue, "textures"); textures && textures->type == cld::JsonValue::Type::Object)
                {
                    const cld::JsonValue* enabled = cld::FindMember(overrideValue, "textureOverridesEnabled");
                    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
                    {
                        const bool overrideEnabled = enabled && enabled->type == cld::JsonValue::Type::Object
                            ? cld::JsonBoolOr(*enabled, TextureSlotKeys[slot], false)
                            : false;
                        if (!overrideEnabled)
                        {
                            continue;
                        }
                        const std::string texturePathUtf8 = cld::JsonStringOr(*textures, TextureSlotKeys[slot]);
                        const std::filesystem::path resolvedTexturePath = resolveAssetPath(texturePathUtf8);
                        if (bundleProject && !texturePathUtf8.empty() && resolvedTexturePath.empty())
                        {
                            diagnostics = "Bundle material texture path is absolute or escapes assetRoot.";
                            return false;
                        }
                        const std::wstring texturePath = resolvedTexturePath.wstring();
                        std::string textureDiagnostics;
                        if (ValidateMaterialTexturePath(texturePath, textureDiagnostics))
                        {
                            material.textures[slot] = texturePath;
                            if (static_cast<size_t>(materialIndex) < m_textureOverrideEnabled.size())
                            {
                                m_textureOverrideEnabled[materialIndex][slot] = true;
                            }
                        }
                    }
                }
                if (const cld::JsonValue* bindings = cld::FindMember(overrideValue, "textureBindings"); bindings && bindings->type == cld::JsonValue::Type::Object)
                {
                    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
                    {
                        const cld::JsonValue* value = cld::FindMember(*bindings, TextureSlotKeys[slot]);
                        if (!value || value->type != cld::JsonValue::Type::Object) continue;
                        Bistro::TextureBinding& binding = material.textureBindings[slot];
                        binding.texCoord = static_cast<uint32_t>(std::clamp(
                            static_cast<int>(cld::JsonNumberOr(*value, "uvSet", binding.texCoord)), 0, 1));
                        binding.offset = JsonFloat2Or(*value, "offset", binding.offset);
                        binding.scale = JsonFloat2Or(*value, "scale", binding.scale);
                        binding.rotation = static_cast<float>(cld::JsonNumberOr(*value, "rotation", binding.rotation));
                        binding.samplerIndex = TextureSamplerFromName(cld::JsonStringOr(*value, "sampler"), binding.samplerIndex);
                        binding.resolutionPolicy = TextureResolutionPolicyFromName(cld::JsonStringOr(*value, "resolutionPolicy"), binding.resolutionPolicy);
                        const bool bindingOverridden = cld::JsonBoolOr(*value, "overridden", false);
                        if (static_cast<size_t>(materialIndex) < m_textureBindingOverrideEnabled.size())
                        {
                            m_textureBindingOverrideEnabled[materialIndex][slot] = bindingOverridden;
                        }
                    }
                }
                materialOverridesChanged = true;
            }
        }

        m_materialVariants.clear();
        if (const cld::JsonValue* variants = cld::FindMember(root, "materialVariants"); variants && variants->type == cld::JsonValue::Type::Array)
        {
            for (const cld::JsonValue& variantValue : variants->array)
            {
                if (variantValue.type != cld::JsonValue::Type::Object)
                {
                    continue;
                }
                MaterialVariant variant;
                variant.name = cld::JsonStringOr(variantValue, "name", "Variant");
                variant.materialIndex = std::clamp(static_cast<int>(cld::JsonNumberOr(variantValue, "materialIndex", 0.0)), 0, (std::max)(0, static_cast<int>(m_scene.materials.size()) - 1));
                variant.materialName = Utf8ToWide(cld::JsonStringOr(variantValue, "materialName"));
                if (variant.materialName.empty() && static_cast<size_t>(variant.materialIndex) < m_scene.materials.size())
                {
                    variant.materialName = m_scene.materials[variant.materialIndex].name;
                }
                variant.snapshot = CaptureMaterialSnapshot(variant.materialIndex);
                variant.snapshot.sourceMaterialId = cld::JsonStringOr(variantValue, "sourceMaterialId", variant.snapshot.sourceMaterialId);
                const std::array<float, 4> baseColor = cld::JsonFloat4Or(variantValue, "baseColor", Float4ToArray(variant.snapshot.baseColorFactor));
                const std::array<float, 4> emissive = cld::JsonFloat4Or(variantValue, "emissive", Float4ToArray(variant.snapshot.emissiveFactor));
                variant.snapshot.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
                variant.snapshot.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
                variant.snapshot.roughnessFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(variantValue, "roughness", variant.snapshot.roughnessFactor)), 0.02f, 1.0f);
                variant.snapshot.metallicFactor = std::clamp(static_cast<float>(cld::JsonNumberOr(variantValue, "metallic", variant.snapshot.metallicFactor)), 0.0f, 1.0f);
                variant.snapshot.occlusionStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(variantValue, "occlusionStrength", variant.snapshot.occlusionStrength)), 0.0f, 2.0f);
                variant.snapshot.normalStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(variantValue, "normalStrength", variant.snapshot.normalStrength)), 0.0f, 2.0f);
                variant.snapshot.alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(variantValue, "alphaCutoff", variant.snapshot.alphaCutoff)), 0.0f, 1.0f);
                variant.snapshot.alphaMasked = cld::JsonBoolOr(variantValue, "alphaMasked", variant.snapshot.alphaMasked);
                variant.snapshot.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(variantValue, "packedORM", variant.snapshot.packedOcclusionRoughnessMetallic);
                ReadGltfExtensionsJson(variantValue, variant.snapshot);
                if (const cld::JsonValue* textures = cld::FindMember(variantValue, "textures"); textures && textures->type == cld::JsonValue::Type::Object)
                {
                    const cld::JsonValue* enabled = cld::FindMember(variantValue, "textureOverridesEnabled");
                    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
                    {
                        variant.snapshot.textureOverrideEnabled[slot] = enabled && enabled->type == cld::JsonValue::Type::Object
                            ? cld::JsonBoolOr(*enabled, TextureSlotKeys[slot], false)
                            : false;
                        const std::string texturePathUtf8 = cld::JsonStringOr(*textures, TextureSlotKeys[slot]);
                        const std::filesystem::path resolvedTexturePath = resolveAssetPath(texturePathUtf8);
                        if (bundleProject && !texturePathUtf8.empty() && resolvedTexturePath.empty())
                        {
                            diagnostics = "Bundle material variant texture path is absolute or escapes assetRoot.";
                            return false;
                        }
                        variant.snapshot.textures[slot] = resolvedTexturePath.wstring();
                    }
                }
                if (const cld::JsonValue* bindings = cld::FindMember(variantValue, "textureBindings"); bindings && bindings->type == cld::JsonValue::Type::Object)
                {
                    for (UINT slot = 0; slot < TextureSlotCount; ++slot)
                    {
                        const cld::JsonValue* value = cld::FindMember(*bindings, TextureSlotKeys[slot]);
                        if (!value || value->type != cld::JsonValue::Type::Object) continue;
                        Bistro::TextureBinding& binding = variant.snapshot.textureBindings[slot];
                        binding.texCoord = static_cast<uint32_t>(std::clamp(
                            static_cast<int>(cld::JsonNumberOr(*value, "uvSet", binding.texCoord)), 0, 1));
                        binding.offset = JsonFloat2Or(*value, "offset", binding.offset);
                        binding.scale = JsonFloat2Or(*value, "scale", binding.scale);
                        binding.rotation = static_cast<float>(cld::JsonNumberOr(*value, "rotation", binding.rotation));
                        binding.samplerIndex = TextureSamplerFromName(cld::JsonStringOr(*value, "sampler"), binding.samplerIndex);
                        binding.resolutionPolicy = TextureResolutionPolicyFromName(cld::JsonStringOr(*value, "resolutionPolicy"), binding.resolutionPolicy);
                        variant.snapshot.textureBindingOverrideEnabled[slot] = cld::JsonBoolOr(*value, "overridden", false);
                    }
                }
                m_materialVariants.push_back(std::move(variant));
            }
        }
        ++m_mcpMaterialCatalogRevision;

        if (const cld::JsonValue* camera = cld::FindMember(root, "camera"))
        {
            const std::array<float, 3> position = cld::JsonFloat3Or(*camera, "position", { m_defaultCameraPosition.x, m_defaultCameraPosition.y, m_defaultCameraPosition.z });
            m_camera.Reset(
                XMFLOAT3(position[0], position[1], position[2]),
                static_cast<float>(cld::JsonNumberOr(*camera, "yaw", m_defaultCameraYaw)),
                static_cast<float>(cld::JsonNumberOr(*camera, "pitch", m_defaultCameraPitch)),
                static_cast<float>(cld::JsonNumberOr(*camera, "roll", m_defaultCameraRoll)));
            m_cameraFovDegrees = std::clamp(static_cast<float>(cld::JsonNumberOr(*camera, "fovDegrees", m_defaultCameraFovDegrees)), 1.0f, 179.0f);
        }
        if (const cld::JsonValue* lighting = cld::FindMember(root, "lighting"))
        {
            const std::array<float, 3> direction = cld::JsonFloat3Or(*lighting, "direction", { m_lightDirection[0], m_lightDirection[1], m_lightDirection[2] });
            m_lightDirection[0] = direction[0];
            m_lightDirection[1] = direction[1];
            m_lightDirection[2] = direction[2];
            m_lightIntensity = static_cast<float>(cld::JsonNumberOr(*lighting, "intensity", m_lightIntensity));
        }
        if (const cld::JsonValue* pathTracing = cld::FindMember(root, "pathTracing"))
        {
            m_giSamplesPerFrame = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "samplesPerFrame", m_giSamplesPerFrame)), 1, 8);
            m_maxPathBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxBounces", m_maxPathBounces)), 1, 8);
            m_minPathBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "minBounces", m_minPathBounces)), 0, m_maxPathBounces);
            m_giRadianceClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "radianceClamp", m_giRadianceClamp)), 1.0f, 100.0f);
            m_giTemporalClampScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "temporalClamp", m_giTemporalClampScale)), 0.25f, 4.0f);
            m_maxAccumulatedFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxAccumSamples", m_maxAccumulatedFrames)), 1, 4096);
            m_adaptiveSamplingEnabled = cld::JsonBoolOr(*pathTracing, "adaptiveSampling", m_adaptiveSamplingEnabled);
            m_maxAdaptiveSamplesPerPixel = std::clamp(static_cast<int>(cld::JsonNumberOr(*pathTracing, "maxAdaptiveSPP", m_maxAdaptiveSamplesPerPixel)), 1, 4);
            m_adaptiveVarianceThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "varianceThreshold", m_adaptiveVarianceThreshold)), 0.02f, 1.0f);
            m_adaptiveDisocclusionBoost = std::clamp(static_cast<float>(cld::JsonNumberOr(*pathTracing, "disocclusionBoost", m_adaptiveDisocclusionBoost)), 0.0f, 4.0f);
        }
        if (const cld::JsonValue* restir = cld::FindMember(root, "restir"))
        {
            m_restirTemporalReuse = cld::JsonBoolOr(*restir, "temporalReuse", m_restirTemporalReuse);
            m_restirSpatialReusePasses = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "spatialReusePasses", m_restirSpatialReusePasses)), 0, 4);
            m_restirSpatialRadius = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "spatialRadius", m_restirSpatialRadius)), 1, 64);
            m_restirCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "candidateSamples", m_restirCandidateSamples)), 1, 4);
            m_restirMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*restir, "mClamp", m_restirMClamp)), 1.0f, 64.0f);
            m_restirDiTemporalReuse = cld::JsonBoolOr(*restir, "diTemporalReuse", m_restirDiTemporalReuse);
            m_restirDiSpatialReusePasses = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "diSpatialReusePasses", m_restirDiSpatialReusePasses)), 0, 4);
            m_restirDiCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "diCandidateSamples", m_restirDiCandidateSamples)), 1, 4);
            m_restirDiMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(*restir, "diMClamp", m_restirDiMClamp)), 1.0f, 64.0f);
            m_reservoirReprojection = cld::JsonBoolOr(*restir, "reservoirReprojection", m_reservoirReprojection);
            m_reservoirValidation = cld::JsonBoolOr(*restir, "reservoirValidation", m_reservoirValidation);
            m_restirGiValidationRay = cld::JsonBoolOr(*restir, "giValidationRay", m_restirGiValidationRay);
            m_reservoirMaxAge = std::clamp(static_cast<int>(cld::JsonNumberOr(*restir, "reservoirMaxAge", m_reservoirMaxAge)), 1, 32);
        }
        if (const cld::JsonValue* denoise = cld::FindMember(root, "denoise"))
        {
            if (const cld::JsonValue* presetValue = cld::FindMember(*denoise, "preset"))
            {
                NoisePreset preset = m_noisePreset;
                if (presetValue->type != cld::JsonValue::Type::String || !TryParseNoisePreset(presetValue->string, preset))
                {
                    diagnostics = "Project denoise preset is invalid.";
                    return false;
                }
                ApplyNoisePreset(preset);
            }
            if (const cld::JsonValue* jitterValue = cld::FindMember(*denoise, "jitterMode"))
            {
                JitterMode jitterMode = m_jitterMode;
                if (jitterValue->type != cld::JsonValue::Type::String || !TryParseJitterMode(jitterValue->string, jitterMode))
                {
                    diagnostics = "Project denoise jitterMode is invalid.";
                    return false;
                }
                m_jitterMode = jitterMode;
            }
            if (const cld::JsonValue* backendValue = cld::FindMember(*denoise, "backend"))
            {
                DenoiseBackend backend = m_denoiseBackend;
                if (backendValue->type != cld::JsonValue::Type::String || !TryParseDenoiseBackend(backendValue->string, backend))
                {
                    diagnostics = "Project denoise backend is invalid.";
                    return false;
                }
                m_denoiseBackend = backend;
                explicitDenoiseBackend = true;
                projectDenoiseBackend = backend;
                WaitForPreviousFrame();
                m_nrdBackendRuntime.UpdateMethod(m_device.Get(), SelectedNrdMethod());
            }
            if (const cld::JsonValue* dlssModeValue = cld::FindMember(*denoise, "dlssMode"))
            {
                DlssMode dlssMode = m_dlssMode;
                if (dlssModeValue->type != cld::JsonValue::Type::String || !TryParseDlssMode(dlssModeValue->string, dlssMode))
                {
                    diagnostics = "Project denoise dlssMode is invalid.";
                    return false;
                }
                m_dlssMode = dlssMode;
                m_dlssBackendRuntime.UpdateMode(m_width, m_height, m_dlssMode);
            }
            m_denoiserEnabled = cld::JsonBoolOr(*denoise, "enabled", m_denoiserEnabled);
            if (cld::FindMember(*denoise, "enabled"))
            {
                explicitDenoiserEnabled = true;
                projectDenoiserEnabled = m_denoiserEnabled;
            }
            m_dlssEnabledWhenAvailable = cld::JsonBoolOr(*denoise, "dlssEnabledWhenAvailable", m_dlssEnabledWhenAvailable);
            m_splitSignalDenoise = cld::JsonBoolOr(*denoise, "splitSignalDenoise", m_splitSignalDenoise);
            m_realtimeReconstruction = cld::JsonBoolOr(*denoise, "realtimeReconstruction", m_realtimeReconstruction);
            m_cameraJitter = cld::JsonBoolOr(*denoise, "cameraJitter", m_cameraJitter);
            m_temporalStabilityEnabled = cld::JsonBoolOr(*denoise, "temporalStability", m_temporalStabilityEnabled);
            m_movingJitterScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "movingJitterScale", m_movingJitterScale)), 0.0f, 1.0f);
            m_reconstructionMaxHistoryFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(*denoise, "maxHistoryFrames", m_reconstructionMaxHistoryFrames)), 1, 128);
            m_temporalAlphaMin = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "temporalAlphaMin", m_temporalAlphaMin)), 0.01f, 0.5f);
            m_temporalAlphaMax = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "temporalAlphaMax", m_temporalAlphaMax)), m_temporalAlphaMin, 0.8f);
            m_historyClampSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "historyClampSigma", m_historyClampSigma)), 0.5f, 4.0f);
            m_reactiveThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "reactiveThreshold", m_reactiveThreshold)), 0.05f, 1.0f);
            m_specularHistoryScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "specularHistoryScale", m_specularHistoryScale)), 0.0f, 1.0f);
            const rb::DenoiseAtrousPassesCompatibility atrousCompatibility =
                rb::ResolveDenoiseAtrousPasses(
                    *denoise,
                    m_atrousPassCount,
                    projectSchemaVersion <= rb::LegacySpatialIterationsSchemaVersion);
            m_atrousPassCount = atrousCompatibility.atrousPasses;
            if (atrousCompatibility.usedLegacySpatialIterations)
            {
                m_denoiserSpatialIterations = (std::min)(m_atrousPassCount, 4);
            }
            denoiseCompatibilityWarning = atrousCompatibility.warning;
            m_atrousDiffuseStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "diffuseFilterStrength", m_atrousDiffuseStrength)), 0.0f, 1.0f);
            m_atrousSpecularStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "specularFilterStrength", m_atrousSpecularStrength)), 0.0f, 1.0f);
            m_atrousVarianceScale = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "varianceScale", m_atrousVarianceScale)), 0.25f, 4.0f);
            m_denoiserNormalSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "normalSigma", m_denoiserNormalSigma)), 0.05f, 1.0f);
            m_denoiserDepthSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "depthSigma", m_denoiserDepthSigma)), 0.002f, 0.10f);
            m_denoiserLuminanceSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "luminanceSigma", m_denoiserLuminanceSigma)), 0.1f, 8.0f);
            m_denoiserAlbedoSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "albedoSigma", m_denoiserAlbedoSigma)), 0.05f, 1.0f);
            m_denoiserStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(*denoise, "strength", m_denoiserStrength)), 0.0f, 1.0f);
        }
        if (const cld::JsonValue* quality = cld::FindMember(root, "quality"))
        {
            rb::QualitySettings parsedQuality;
            if (!rb::TryParseQualitySettings(*quality, m_qualitySettings, parsedQuality, diagnostics))
            {
                return false;
            }
            qualityResourcesChanged =
                parsedQuality.qualityProfile != m_qualitySettings.qualityProfile ||
                parsedQuality.restirBackend != m_qualitySettings.restirBackend ||
                parsedQuality.finalTaa != m_qualitySettings.finalTaa ||
                parsedQuality.resolutionMode != m_qualitySettings.resolutionMode ||
                parsedQuality.fixedRenderScale != m_qualitySettings.fixedRenderScale ||
                parsedQuality.minRenderScale != m_qualitySettings.minRenderScale ||
                parsedQuality.maxRenderScale != m_qualitySettings.maxRenderScale;
            m_qualitySettings = parsedQuality;
            ApplyQualitySettingsToRenderer();
            if (explicitDenoiseBackend)
            {
                m_denoiseBackend = projectDenoiseBackend;
            }
            if (explicitDenoiserEnabled)
            {
                m_denoiserEnabled = projectDenoiserEnabled;
            }
            if (explicitDenoiseBackend)
            {
                WaitForPreviousFrame();
                m_nrdBackendRuntime.UpdateMethod(m_device.Get(), SelectedNrdMethod());
            }
        }
        if (const cld::JsonValue* view = cld::FindMember(root, "view"))
        {
            m_debugViewMode = std::clamp(static_cast<int>(cld::JsonNumberOr(*view, "debugView", m_debugViewMode)), 0, DebugViewMax);
            m_environmentMapEnabled = cld::JsonBoolOr(*view, "environmentEnabled", m_environmentMapEnabled);
            m_exposure = std::clamp(static_cast<float>(cld::JsonNumberOr(*view, "exposure", m_exposure)), -12.0f, 12.0f);
            m_gamma = std::clamp(static_cast<float>(cld::JsonNumberOr(*view, "gamma", m_gamma)), 0.8f, 4.0f);
            ToneMapper toneMapper = m_toneMapper;
            if (TryParseToneMapper(cld::JsonStringOr(*view, "toneMapper"), toneMapper))
            {
                m_toneMapper = toneMapper;
            }
            MaterialFocusMode focusMode = m_materialFocusMode;
            if (TryParseMaterialFocusMode(cld::JsonStringOr(*view, "materialFocusMode"), focusMode))
            {
                m_materialFocusMode = focusMode;
            }
            m_selectedMaterial = std::clamp(static_cast<int>(cld::JsonNumberOr(*view, "selectedMaterial", m_selectedMaterial)), 0, (std::max)(0, static_cast<int>(m_scene.materials.size()) - 1));
        }

        const bool fusedNrdContractChanged =
            IsNrdSelected() &&
            (initialDenoiserEnabled != m_denoiserEnabled ||
                (initialDebugView == 0) != (m_debugViewMode == 0));
        if (modeChanged || qualityResourcesChanged || initialDenoiseBackend != m_denoiseBackend ||
            fusedNrdContractChanged)
        {
            CreateGpuResourcesForCurrentScene();
        }
        else if (materialOverridesChanged)
        {
            InvalidateHistory(rb::FrameChangeMask::Material);
            RefreshEditableGpuResources(true, true);
        }
        m_projectPath = path;
        m_projectDirty = false;
        ResetRenderingHistory();
        diagnostics = "Project loaded.";
        if (!denoiseCompatibilityWarning.empty())
        {
            diagnostics += " " + denoiseCompatibilityWarning;
            LogDiagnostic(denoiseCompatibilityWarning);
        }
        m_projectDiagnostics = diagnostics;
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

void D3D12PathTracingBackend::LoadStartupSettings()
{
    if (m_startupSettingsPath.empty())
    {
        m_startupSettingsPath = DefaultStartupSettingsPath();
    }
    else
    {
        m_startupSettingsPath = std::filesystem::absolute(std::filesystem::path(m_startupSettingsPath)).wstring();
    }

    const std::filesystem::path settingsPath(m_startupSettingsPath);
    std::ifstream file(settingsPath, std::ios::binary);
    if (!file)
    {
        if (m_hasStartupSettingsPath)
        {
            m_startupDiagnostics = "Startup settings file was not found: " + WideToUtf8(m_startupSettingsPath);
            LogDiagnostic(m_startupDiagnostics);
        }
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    try
    {
        const cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
        if (root.type != cld::JsonValue::Type::Object)
        {
            m_startupDiagnostics = "Startup settings root must be an object.";
            LogDiagnostic(m_startupDiagnostics);
            return;
        }

        if (!cld::JsonBoolOr(root, "enabled", true))
        {
            m_startupDiagnostics = "Startup settings disabled: " + WideToUtf8(m_startupSettingsPath);
            LogDiagnostic(m_startupDiagnostics);
            return;
        }

        std::filesystem::path baseDirectory = settingsPath.parent_path();
        const std::string baseDirectoryUtf8 = cld::JsonStringOr(root, "baseDirectory");
        if (!baseDirectoryUtf8.empty())
        {
            std::filesystem::path configuredBase(Utf8ToWide(baseDirectoryUtf8));
            if (configuredBase.is_relative())
            {
                configuredBase = baseDirectory / configuredBase;
            }
            baseDirectory = std::filesystem::absolute(configuredBase);
        }

        const std::wstring configuredProjectPath = ResolveStartupPath(cld::JsonStringOr(root, "projectPath"), baseDirectory);
        const std::wstring configuredScenePath = ResolveStartupPath(cld::JsonStringOr(root, "scenePath"), baseDirectory);
        const std::wstring configuredEnvironmentPath = ResolveStartupPath(cld::JsonStringOr(root, "environmentPath"), baseDirectory);
        const bool configuredEnvironmentEnabled = cld::JsonBoolOr(root, "environmentEnabled", !configuredEnvironmentPath.empty());

        if (!m_hasCommandLineProjectPath && !m_hasCommandLineScenePath)
        {
            m_startupProjectPath = configuredProjectPath;
            m_startupScenePath = configuredScenePath;
        }
        if (!m_hasCommandLineEnvironmentPath)
        {
            m_startupEnvironmentPath = configuredEnvironmentEnabled ? configuredEnvironmentPath : std::wstring();
        }

        m_startupDiagnostics = "Startup settings loaded: " + WideToUtf8(m_startupSettingsPath);
        LogDiagnostic(m_startupDiagnostics);
    }
    catch (const std::exception& ex)
    {
        m_startupDiagnostics = std::string("Startup settings ignored: ") + ex.what();
        LogDiagnostic(m_startupDiagnostics);
    }
}

void D3D12PathTracingBackend::ApplyStartupSettings()
{
    bool startupProjectLoaded = false;
    if (!m_startupProjectPath.empty())
    {
        std::string diagnostics;
        LogDiagnostic(L"Startup project load: " + m_startupProjectPath);
        if (!LoadProjectFromDisk(m_startupProjectPath, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            m_startupDiagnostics = "Startup project load failed: " + diagnostics;
            LogDiagnostic(m_startupDiagnostics);
        }
        else
        {
            startupProjectLoaded = true;
            m_startupDiagnostics = "Startup project loaded.";
            LogDiagnostic("Startup project load succeeded: " + diagnostics);
        }
    }

    if (!startupProjectLoaded && !m_startupScenePath.empty())
    {
        std::string diagnostics;
        LogDiagnostic(L"Startup scene load: " + m_startupScenePath);
        if (!LoadScenePath(m_startupScenePath, diagnostics))
        {
            m_sceneDiagnostics = diagnostics;
            m_startupDiagnostics = "Startup scene load failed: " + diagnostics;
            LogDiagnostic(m_startupDiagnostics);
        }
        else
        {
            m_startupDiagnostics = "Startup scene loaded.";
            LogDiagnostic("Startup scene load succeeded: " + diagnostics);
        }
    }

    if (!m_startupEnvironmentPath.empty())
    {
        std::string diagnostics;
        LogDiagnostic(L"Startup environment load: " + m_startupEnvironmentPath);
        if (!LoadEnvironmentPath(m_startupEnvironmentPath, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            m_startupDiagnostics = "Startup environment load failed: " + diagnostics;
            LogDiagnostic(m_startupDiagnostics);
        }
        else
        {
            LogDiagnostic("Startup environment load succeeded: " + diagnostics);
        }
    }
}

bool D3D12PathTracingBackend::SaveStartupSettingsToDisk()
{
    if (m_startupSettingsPath.empty())
    {
        m_startupSettingsPath = DefaultStartupSettingsPath();
    }

    const std::filesystem::path settingsPath(m_startupSettingsPath);
    std::error_code ec;
    std::filesystem::create_directories(settingsPath.parent_path(), ec);
    if (ec)
    {
        m_startupDiagnostics = "Failed to create startup settings directory.";
        m_projectDiagnostics = m_startupDiagnostics;
        return false;
    }

    std::ofstream file(settingsPath, std::ios::binary);
    if (!file)
    {
        m_startupDiagnostics = "Failed to save startup settings.";
        m_projectDiagnostics = m_startupDiagnostics;
        return false;
    }

    const std::wstring startupProjectPath = !m_projectDirty ? m_projectPath : std::wstring();
    const std::wstring startupEnvironmentPath = m_environmentMapEnabled ? m_environmentTexturePath : std::wstring();
    file << "{\n";
    file << "  \"version\": 1,\n";
    file << "  \"enabled\": true,\n";
    file << "  \"projectPath\": \"" << cld::EscapeJson(WideToUtf8(startupProjectPath)) << "\",\n";
    file << "  \"scenePath\": \"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",\n";
    file << "  \"environmentPath\": \"" << cld::EscapeJson(WideToUtf8(startupEnvironmentPath)) << "\",\n";
    file << "  \"environmentEnabled\": " << (m_environmentMapEnabled ? "true" : "false") << "\n";
    file << "}\n";

    m_startupDiagnostics = "Startup settings saved: " + WideToUtf8(m_startupSettingsPath);
    m_projectDiagnostics = m_startupDiagnostics;
    return true;
}

bool D3D12PathTracingBackend::DeleteStartupSettings()
{
    if (m_startupSettingsPath.empty())
    {
        m_startupSettingsPath = DefaultStartupSettingsPath();
    }

    const std::filesystem::path settingsPath(m_startupSettingsPath);
    std::error_code ec;
    const bool settingsFileExists = std::filesystem::exists(settingsPath, ec);
    if (ec)
    {
        m_startupDiagnostics = "Failed to inspect startup settings.";
        m_projectDiagnostics = m_startupDiagnostics;
        return false;
    }
    if (!settingsFileExists)
    {
        m_startupDiagnostics = "No startup settings file to clear.";
        m_projectDiagnostics = m_startupDiagnostics;
        return true;
    }

    const bool removed = std::filesystem::remove(settingsPath, ec);
    if (ec || !removed)
    {
        m_startupDiagnostics = "Failed to clear startup settings.";
        m_projectDiagnostics = m_startupDiagnostics;
        return false;
    }

    m_startupProjectPath.clear();
    m_startupScenePath.clear();
    m_startupEnvironmentPath.clear();
    m_startupDiagnostics = "Startup settings cleared.";
    m_projectDiagnostics = m_startupDiagnostics;
    return true;
}

bool D3D12PathTracingBackend::ApplyAction(const std::string& method, const cld::JsonValue& params, std::string& diagnostics, bool validateOnly)
{
    if (method == "set_scene")
    {
        std::string scenePathUtf8 = cld::JsonStringOr(params, "path");
        if (scenePathUtf8.empty())
        {
            scenePathUtf8 = cld::JsonStringOr(params, "scenePath");
        }
        if (scenePathUtf8.empty())
        {
            diagnostics = "set_scene requires path or scenePath.";
            return false;
        }

        const std::wstring scenePath = Utf8ToWide(scenePathUtf8);
        if (!std::filesystem::exists(scenePath))
        {
            diagnostics = "Scene path does not exist.";
            return false;
        }

        const bool hasEnvironmentPath = cld::FindMember(params, "environmentPath") != nullptr;
        const std::string environmentPathUtf8 = cld::JsonStringOr(params, "environmentPath");
        const std::wstring environmentPath = Utf8ToWide(environmentPathUtf8);
        if (!environmentPath.empty() && !std::filesystem::exists(environmentPath))
        {
            diagnostics = "Environment path does not exist.";
            return false;
        }

        if (!validateOnly)
        {
            if (!LoadScenePath(scenePath, diagnostics))
            {
                return false;
            }
            if (hasEnvironmentPath && !LoadEnvironmentPath(environmentPath, diagnostics))
            {
                return false;
            }
            m_projectDirty = true;
        }
        diagnostics = "Scene settings accepted.";
        return true;
    }
    if (method == "set_camera")
    {
        const XMFLOAT3 current = m_camera.GetPosition();
        const std::array<float, 3> position = cld::JsonFloat3Or(params, "position", { current.x, current.y, current.z });
        const float yaw = static_cast<float>(cld::JsonNumberOr(params, "yaw", m_camera.GetYawRadians()));
        const float pitch = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "pitch", m_camera.GetPitchRadians())), XMConvertToRadians(-83.0f), XMConvertToRadians(83.0f));
        const float roll = static_cast<float>(cld::JsonNumberOr(params, "roll", m_camera.GetRollRadians()));
        const float fovDegrees = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "fovDegrees", m_cameraFovDegrees)), 1.0f, 179.0f);
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]) ||
            !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll) || !std::isfinite(fovDegrees))
        {
            diagnostics = "Camera contains non-finite values.";
            return false;
        }

        rb::CameraHistoryMode historyMode = rb::CameraHistoryMode::Auto;
        if (const cld::JsonValue* historyModeValue = cld::FindMember(params, "historyMode"))
        {
            if (historyModeValue->type != cld::JsonValue::Type::String ||
                !rb::TryParseCameraHistoryMode(historyModeValue->string, historyMode))
            {
                diagnostics = "Camera historyMode is invalid.";
                return false;
            }
        }

        if (!validateOnly)
        {
            m_camera.Reset(XMFLOAT3(position[0], position[1], position[2]), yaw, pitch, roll);
            m_cameraFovDegrees = fovDegrees;
            if (historyMode == rb::CameraHistoryMode::Reset)
            {
                InvalidateHistory(rb::FrameChangeMask::CameraCut);
            }
            else
            {
                m_forcePreserveCameraHistoryOnce = historyMode == rb::CameraHistoryMode::Preserve;
                InvalidateHistory(rb::FrameChangeMask::CameraMotion);
            }
            m_projectDirty = true;
        }
        diagnostics = "Camera settings accepted.";
        return true;
    }
    if (method == "set_quality")
    {
        rb::QualitySettings parsedQuality;
        if (!rb::TryParseQualitySettings(params, m_qualitySettings, parsedQuality, diagnostics))
        {
            return false;
        }
        if (!validateOnly)
        {
            const bool restirBackendChanged = parsedQuality.restirBackend != m_qualitySettings.restirBackend;
            const bool qualityProfileChanged = parsedQuality.qualityProfile != m_qualitySettings.qualityProfile;
            const bool finalTaaResourcesChanged = parsedQuality.finalTaa != m_qualitySettings.finalTaa;
            const bool resolutionResourcesChanged =
                parsedQuality.resolutionMode != m_qualitySettings.resolutionMode ||
                parsedQuality.fixedRenderScale != m_qualitySettings.fixedRenderScale ||
                parsedQuality.minRenderScale != m_qualitySettings.minRenderScale ||
                parsedQuality.maxRenderScale != m_qualitySettings.maxRenderScale;
            const DenoiseBackend previousDenoiseBackend = m_denoiseBackend;
            m_qualitySettings = parsedQuality;
            ApplyQualitySettingsToRenderer();
            const bool denoiseBackendChanged = previousDenoiseBackend != m_denoiseBackend;
            rb::FrameChangeMask qualityChanges = rb::FrameChangeMask::Backend;
            if (qualityProfileChanged)
            {
                qualityChanges |= rb::FrameChangeMask::QualityProfile;
            }
            InvalidateHistory(qualityChanges);
            if (restirBackendChanged || qualityProfileChanged || denoiseBackendChanged ||
                finalTaaResourcesChanged || resolutionResourcesChanged)
            {
                CreateGpuResourcesForCurrentScene();
            }
            m_projectDirty = true;
        }
        diagnostics = "Quality settings accepted.";
        return true;
    }
    if (method == "set_material")
    {
        const int materialIndex = ResolveMaterialIndex(params);
        if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= m_scene.materials.size())
        {
            diagnostics = "Material index or name was not found.";
            return false;
        }

        const Bistro::Material& current = m_scene.materials[materialIndex];
        const std::array<float, 4> baseColor = cld::JsonFloat4Or(params, "baseColor", Float4ToArray(current.baseColorFactor));
        const std::array<float, 4> emissive = cld::JsonFloat4Or(params, "emissive", Float4ToArray(current.emissiveFactor));
        const float roughness = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "roughness", current.roughnessFactor)), 0.02f, 1.0f);
        const float metallic = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "metallic", current.metallicFactor)), 0.0f, 1.0f);
        const float occlusion = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "occlusionStrength", current.occlusionStrength)), 0.0f, 2.0f);
        const float normal = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "normalStrength", current.normalStrength)), 0.0f, 2.0f);
        const float alphaCutoff = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "alphaCutoff", current.alphaCutoff)), 0.0f, 1.0f);
        if (!AllFinite({ baseColor[0], baseColor[1], baseColor[2], baseColor[3], emissive[0], emissive[1], emissive[2], emissive[3],
            roughness, metallic, occlusion, normal, alphaCutoff }))
        {
            diagnostics = "Material contains non-finite values.";
            return false;
        }

        MaterialSnapshot next = CaptureMaterialSnapshot(materialIndex);
        if (cld::JsonBoolOr(params, "resetToSource", false))
        {
            next = {};
            if (static_cast<size_t>(materialIndex) < m_sourceMaterials.size())
            {
                const Bistro::Material& source = m_sourceMaterials[materialIndex];
                next.baseColorFactor = source.baseColorFactor;
                next.emissiveFactor = source.emissiveFactor;
                next.roughnessFactor = source.roughnessFactor;
                next.metallicFactor = source.metallicFactor;
                next.occlusionStrength = source.occlusionStrength;
                next.normalStrength = source.normalStrength;
                next.alphaCutoff = source.alphaCutoff;
                next.alphaMasked = source.alphaMasked;
                next.packedOcclusionRoughnessMetallic = source.packedOcclusionRoughnessMetallic;
                next.sourceMaterialId = source.sourceMaterialId;
                next.extensionFeatureMask = source.extensionFeatureMask;
                next.specularFactor = source.specularFactor;
                next.specularColorFactor = source.specularColorFactor;
                next.indexOfRefraction = source.indexOfRefraction;
                next.transmissionFactor = source.transmissionFactor;
                next.thicknessFactor = source.thicknessFactor;
                next.attenuationColor = source.attenuationColor;
                next.attenuationDistance = source.attenuationDistance;
                next.clearcoatFactor = source.clearcoatFactor;
                next.clearcoatRoughnessFactor = source.clearcoatRoughnessFactor;
                next.clearcoatNormalScale = source.clearcoatNormalScale;
                next.textures = source.textures;
                next.textureBindings = source.textureBindings;
                next.textureOverrideEnabled.fill(false);
                next.textureBindingOverrideEnabled.fill(false);
            }
        }
        if (cld::FindMember(params, "baseColor")) next.baseColorFactor = XMFLOAT4(baseColor[0], baseColor[1], baseColor[2], baseColor[3]);
        if (cld::FindMember(params, "emissive")) next.emissiveFactor = XMFLOAT4(emissive[0], emissive[1], emissive[2], emissive[3]);
        if (cld::FindMember(params, "roughness")) next.roughnessFactor = roughness;
        if (cld::FindMember(params, "metallic")) next.metallicFactor = metallic;
        if (cld::FindMember(params, "occlusionStrength")) next.occlusionStrength = occlusion;
        if (cld::FindMember(params, "normalStrength")) next.normalStrength = normal;
        if (cld::FindMember(params, "alphaCutoff")) next.alphaCutoff = alphaCutoff;
        if (cld::FindMember(params, "alphaMasked")) next.alphaMasked = cld::JsonBoolOr(params, "alphaMasked", next.alphaMasked);
        if (cld::FindMember(params, "packedORM")) next.packedOcclusionRoughnessMetallic = cld::JsonBoolOr(params, "packedORM", next.packedOcclusionRoughnessMetallic);
        ReadGltfExtensionsJson(params, next);
        if (!AllFinite({ next.specularFactor, next.specularColorFactor.x, next.specularColorFactor.y,
            next.specularColorFactor.z, next.indexOfRefraction, next.transmissionFactor,
            next.thicknessFactor, next.attenuationColor.x, next.attenuationColor.y,
            next.attenuationColor.z, next.attenuationDistance, next.clearcoatFactor,
            next.clearcoatRoughnessFactor, next.clearcoatNormalScale }))
        {
            diagnostics = "glTF material extensions contain non-finite values.";
            return false;
        }

        if (const cld::JsonValue* textures = cld::FindMember(params, "textures"); textures && textures->type == cld::JsonValue::Type::Object)
        {
            for (UINT slot = 0; slot < TextureSlotCount; ++slot)
            {
                const std::string texturePathUtf8 = cld::JsonStringOr(*textures, TextureSlotKeys[slot]);
                if (texturePathUtf8.empty())
                {
                    continue;
                }
                const std::wstring texturePath = Utf8ToWide(texturePathUtf8);
                if (!ValidateMaterialTexturePath(texturePath, diagnostics))
                {
                    return false;
                }
                next.textures[slot] = texturePath;
                next.textureOverrideEnabled[slot] = true;
            }
        }
        if (const cld::JsonValue* clearTextures = cld::FindMember(params, "clearTextures"); clearTextures && clearTextures->type == cld::JsonValue::Type::Array)
        {
            for (const cld::JsonValue& slotValue : clearTextures->array)
            {
                UINT slot = 0;
                if (!TryParseTextureSlot(slotValue, slot))
                {
                    diagnostics = "clearTextures contains an invalid texture slot.";
                    return false;
                }
                next.textures[slot].clear();
                next.textureOverrideEnabled[slot] = true;
            }
        }

        if (!validateOnly)
        {
            const MaterialSnapshot before = CaptureMaterialSnapshot(materialIndex);
            const bool reloadTextures = RequiresMaterialTextureReload(before, next);
            const bool rebuildBlas = RequiresMaterialBlasRebuild(before, next);
            ApplyMaterialSnapshot(materialIndex, next, true);
            try
            {
                if (rebuildBlas) CreateGpuResourcesForCurrentScene();
                else RefreshEditableGpuResources(reloadTextures, true);
            }
            catch (const std::exception& ex)
            {
                ApplyMaterialSnapshot(materialIndex, before, true);
                if (rebuildBlas) CreateGpuResourcesForCurrentScene();
                else RefreshEditableGpuResources(reloadTextures, true);
                diagnostics = ex.what();
                return false;
            }
            m_projectDirty = true;
        }
        diagnostics = "Material settings accepted.";
        return true;
    }
    if (method == "set_material_texture")
    {
        const int materialIndex = ResolveMaterialIndex(params);
        UINT slot = 0;
        const cld::JsonValue* slotValue = cld::FindMember(params, "slot");
        if (materialIndex < 0 || !slotValue || !TryParseTextureSlot(*slotValue, slot))
        {
            diagnostics = "set_material_texture requires a valid material and slot.";
            return false;
        }
        const bool resetToSource = cld::JsonBoolOr(params, "resetToSource", false);
        const bool clearTexture = cld::JsonBoolOr(params, "clear", false);
        const std::wstring texturePath = clearTexture || resetToSource ? std::wstring() : Utf8ToWide(cld::JsonStringOr(params, "path"));
        const bool hasBindingEdit = cld::FindMember(params, "uvSet") || cld::FindMember(params, "offset") ||
            cld::FindMember(params, "scale") || cld::FindMember(params, "rotation") ||
            cld::FindMember(params, "sampler") || cld::FindMember(params, "resolutionPolicy");
        if (!resetToSource && !clearTexture && texturePath.empty() && !hasBindingEdit)
        {
            diagnostics = "set_material_texture requires a path, binding edit, clear, or resetToSource.";
            return false;
        }
        if (!resetToSource && !clearTexture && !ValidateMaterialTexturePath(texturePath, diagnostics))
        {
            return false;
        }
        const MaterialSnapshot before = CaptureMaterialSnapshot(materialIndex);
        MaterialSnapshot next = before;
        if (resetToSource)
        {
            if (static_cast<size_t>(materialIndex) >= m_sourceMaterials.size())
            {
                diagnostics = "The source material is unavailable.";
                return false;
            }
            next.textures[slot] = m_sourceMaterials[materialIndex].textures[slot];
            next.textureBindings[slot] = m_sourceMaterials[materialIndex].textureBindings[slot];
            next.textureOverrideEnabled[slot] = false;
            next.textureBindingOverrideEnabled[slot] = false;
        }
        else
        {
            if (clearTexture || !texturePath.empty())
            {
                next.textures[slot] = texturePath;
                next.textureOverrideEnabled[slot] = true;
            }
            Bistro::TextureBinding& binding = next.textureBindings[slot];
            if (cld::FindMember(params, "uvSet"))
            {
                binding.texCoord = static_cast<uint32_t>(std::clamp(
                    static_cast<int>(cld::JsonNumberOr(params, "uvSet", binding.texCoord)), 0, 1));
            }
            if (cld::FindMember(params, "offset")) binding.offset = JsonFloat2Or(params, "offset", binding.offset);
            if (cld::FindMember(params, "scale")) binding.scale = JsonFloat2Or(params, "scale", binding.scale);
            if (cld::FindMember(params, "rotation")) binding.rotation = static_cast<float>(cld::JsonNumberOr(params, "rotation", binding.rotation));
            if (cld::FindMember(params, "sampler")) binding.samplerIndex = TextureSamplerFromName(cld::JsonStringOr(params, "sampler"), binding.samplerIndex);
            if (cld::FindMember(params, "resolutionPolicy")) binding.resolutionPolicy = TextureResolutionPolicyFromName(cld::JsonStringOr(params, "resolutionPolicy"), binding.resolutionPolicy);
            if (hasBindingEdit) next.textureBindingOverrideEnabled[slot] = true;
        }
        if (!std::isfinite(next.textureBindings[slot].offset.x) || !std::isfinite(next.textureBindings[slot].offset.y) ||
            !std::isfinite(next.textureBindings[slot].scale.x) || !std::isfinite(next.textureBindings[slot].scale.y) ||
            !std::isfinite(next.textureBindings[slot].rotation))
        {
            diagnostics = "Texture binding contains non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            ApplyMaterialSnapshot(materialIndex, next, true);
            try
            {
                RefreshEditableGpuResources(true, true);
            }
            catch (const std::exception& ex)
            {
                ApplyMaterialSnapshot(materialIndex, before, true);
                RefreshEditableGpuResources(true, true);
                diagnostics = ex.what();
                return false;
            }
            m_projectDirty = true;
        }
        diagnostics = "Material texture settings accepted.";
        return true;
    }
    if (method == "reset_material")
    {
        const int materialIndex = ResolveMaterialIndex(params);
        if (materialIndex < 0)
        {
            diagnostics = "Material index or name was not found.";
            return false;
        }
        if (!validateOnly)
        {
            const MaterialSnapshot before = CaptureMaterialSnapshot(materialIndex);
            ResetMaterialToSource(materialIndex);
            const MaterialSnapshot after = CaptureMaterialSnapshot(materialIndex);
            if (RequiresMaterialBlasRebuild(before, after)) CreateGpuResourcesForCurrentScene();
            else RefreshEditableGpuResources(true, true);
            m_projectDirty = true;
        }
        diagnostics = "Material reset accepted.";
        return true;
    }
    if (method == "save_material_variant")
    {
        const int materialIndex = ResolveMaterialIndex(params);
        const std::string variantName = cld::JsonStringOr(params, "variant", cld::JsonStringOr(params, "variantName", "Variant"));
        if (materialIndex < 0 || variantName.empty())
        {
            diagnostics = "save_material_variant requires a valid material and variant name.";
            return false;
        }
        if (!validateOnly)
        {
            auto it = std::find_if(m_materialVariants.begin(), m_materialVariants.end(), [&](const MaterialVariant& variant)
            {
                return variant.materialIndex == materialIndex && variant.name == variantName;
            });
            MaterialVariant variant;
            variant.name = variantName;
            variant.materialIndex = materialIndex;
            variant.materialName = m_scene.materials[materialIndex].name;
            variant.snapshot = CaptureMaterialSnapshot(materialIndex);
            if (it == m_materialVariants.end())
            {
                m_materialVariants.push_back(std::move(variant));
            }
            else
            {
                *it = std::move(variant);
            }
            ++m_mcpMaterialCatalogRevision;
            m_projectDirty = true;
        }
        diagnostics = "Material variant saved.";
        return true;
    }
    if (method == "apply_material_variant" || method == "delete_material_variant")
    {
        const int materialIndex = ResolveMaterialIndex(params);
        const int variantIndex = static_cast<int>(cld::JsonNumberOr(params, "variantIndex", -1.0));
        const std::string variantName = cld::JsonStringOr(params, "variant", cld::JsonStringOr(params, "variantName"));
        auto it = m_materialVariants.end();
        if (variantIndex >= 0 && static_cast<size_t>(variantIndex) < m_materialVariants.size())
        {
            it = m_materialVariants.begin() + variantIndex;
        }
        else if (!variantName.empty())
        {
            it = std::find_if(m_materialVariants.begin(), m_materialVariants.end(), [&](const MaterialVariant& variant)
            {
                return variant.name == variantName && (materialIndex < 0 || variant.materialIndex == materialIndex || variant.materialName == Utf8ToWide(cld::JsonStringOr(params, "name")));
            });
        }
        if (it == m_materialVariants.end())
        {
            diagnostics = "Material variant was not found.";
            return false;
        }
        if (method == "delete_material_variant")
        {
            if (!validateOnly)
            {
                m_materialVariants.erase(it);
                ++m_mcpMaterialCatalogRevision;
                m_projectDirty = true;
            }
            diagnostics = "Material variant deleted.";
            return true;
        }

        int applyIndex = materialIndex >= 0 ? materialIndex : it->materialIndex;
        if (applyIndex < 0 || static_cast<size_t>(applyIndex) >= m_scene.materials.size())
        {
            diagnostics = "Material variant target was not found.";
            return false;
        }
        if (!validateOnly)
        {
            const MaterialSnapshot before = CaptureMaterialSnapshot(applyIndex);
            const bool reloadTextures = RequiresMaterialTextureReload(before, it->snapshot);
            const bool rebuildBlas = RequiresMaterialBlasRebuild(before, it->snapshot);
            ApplyMaterialSnapshot(applyIndex, it->snapshot, true);
            try
            {
                if (rebuildBlas) CreateGpuResourcesForCurrentScene();
                else RefreshEditableGpuResources(reloadTextures, true);
            }
            catch (const std::exception& ex)
            {
                ApplyMaterialSnapshot(applyIndex, before, true);
                if (rebuildBlas) CreateGpuResourcesForCurrentScene();
                else RefreshEditableGpuResources(reloadTextures, true);
                diagnostics = ex.what();
                return false;
            }
            m_projectDirty = true;
        }
        diagnostics = "Material variant applied.";
        return true;
    }
    if (method == "set_material_view")
    {
        const int selected = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "selectedMaterial", m_selectedMaterial)), 0, (std::max)(0, static_cast<int>(m_scene.materials.size()) - 1));
        MaterialFocusMode focusMode = m_materialFocusMode;
        if (const cld::JsonValue* focus = cld::FindMember(params, "focusMode"))
        {
            if (focus->type != cld::JsonValue::Type::String || !TryParseMaterialFocusMode(focus->string, focusMode))
            {
                diagnostics = "Material focus mode is invalid.";
                return false;
            }
        }
        if (!validateOnly)
        {
            const bool renderedViewChanged = focusMode != m_materialFocusMode ||
                (focusMode != MaterialFocusMode::Normal && selected != m_selectedMaterial);
            m_selectedMaterial = selected;
            m_materialFocusMode = focusMode;
            if (renderedViewChanged)
            {
                InvalidateHistory(rb::FrameChangeMask::View);
            }
            m_projectDirty = true;
        }
        diagnostics = "Material view settings accepted.";
        return true;
    }
    if (method == "set_color_management")
    {
        ToneMapper toneMapper = m_toneMapper;
        if (const cld::JsonValue* toneMapperValue = cld::FindMember(params, "toneMapper"))
        {
            if (toneMapperValue->type != cld::JsonValue::Type::String || !TryParseToneMapper(toneMapperValue->string, toneMapper))
            {
                diagnostics = "Tone mapper is invalid.";
                return false;
            }
        }
        const float exposure = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "exposure", m_exposure)), -12.0f, 12.0f);
        const float gamma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "gamma", m_gamma)), 0.8f, 4.0f);
        if (!AllFinite({ exposure, gamma }))
        {
            diagnostics = "Color management settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            m_toneMapper = toneMapper;
            m_exposure = exposure;
            m_gamma = gamma;
            ResetAccumulation();
            m_projectDirty = true;
        }
        diagnostics = "Color management settings accepted.";
        return true;
    }
    if (method == "set_lighting")
    {
        const std::array<float, 3> direction = cld::JsonFloat3Or(params, "direction", { m_lightDirection[0], m_lightDirection[1], m_lightDirection[2] });
        const std::array<float, 3> color = cld::JsonFloat3Or(params, "color", { m_lightColor[0], m_lightColor[1], m_lightColor[2] });
        const std::array<float, 3> skyColor = cld::JsonFloat3Or(params, "skyColor", { m_skyColor[0], m_skyColor[1], m_skyColor[2] });
        const std::array<float, 3> skyHorizonColor = cld::JsonFloat3Or(params, "skyHorizonColor", { m_skyHorizonColor[0], m_skyHorizonColor[1], m_skyHorizonColor[2] });
        const std::array<float, 3> skyZenithColor = cld::JsonFloat3Or(params, "skyZenithColor", { m_skyZenithColor[0], m_skyZenithColor[1], m_skyZenithColor[2] });
        const std::array<float, 3> skyGroundColor = cld::JsonFloat3Or(params, "skyGroundColor", { m_skyGroundColor[0], m_skyGroundColor[1], m_skyGroundColor[2] });
        const float intensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "intensity", m_lightIntensity)), 0.0f, 100.0f);
        const float rayTMin = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "rayTMin", m_rayTMin)), 0.001f, 0.25f);
        const float skyIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "skyIntensity", m_skyIntensity)), 0.0f, 10.0f);
        const float sunIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "sunIntensity", m_sunIntensity)), 0.0f, 50.0f);
        const float sunSize = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "sunSize", m_sunAngularRadius)), 0.001f, 0.08f);
        const float environmentIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "environmentIntensity", m_environmentIntensity)), 0.0f, 10.0f);
        const float environmentRotation = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "environmentRotation", m_environmentRotation)), -3.14159f, 3.14159f);
        const float emissiveIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "emissiveIntensity", m_emissiveLightIntensity)), 0.0f, 30.0f);
        const float areaLightIntensity = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "areaLightIntensity", m_proceduralLightIntensity)), 0.0f, 50.0f);
        if (!AllFinite({ direction[0], direction[1], direction[2], color[0], color[1], color[2], skyColor[0], skyColor[1], skyColor[2],
            skyHorizonColor[0], skyHorizonColor[1], skyHorizonColor[2], skyZenithColor[0], skyZenithColor[1], skyZenithColor[2],
            skyGroundColor[0], skyGroundColor[1], skyGroundColor[2], intensity, rayTMin, skyIntensity, sunIntensity, sunSize,
            environmentIntensity, environmentRotation, emissiveIntensity, areaLightIntensity }))
        {
            diagnostics = "Lighting contains non-finite values.";
            return false;
        }

        if (!validateOnly)
        {
            m_lightDirection[0] = direction[0];
            m_lightDirection[1] = direction[1];
            m_lightDirection[2] = direction[2];
            m_lightColor[0] = color[0];
            m_lightColor[1] = color[1];
            m_lightColor[2] = color[2];
            m_lightIntensity = intensity;
            m_rayTMin = rayTMin;
            m_skyEnabled = cld::JsonBoolOr(params, "skyEnabled", m_skyEnabled);
            m_skyColor[0] = skyColor[0]; m_skyColor[1] = skyColor[1]; m_skyColor[2] = skyColor[2];
            m_skyHorizonColor[0] = skyHorizonColor[0]; m_skyHorizonColor[1] = skyHorizonColor[1]; m_skyHorizonColor[2] = skyHorizonColor[2];
            m_skyZenithColor[0] = skyZenithColor[0]; m_skyZenithColor[1] = skyZenithColor[1]; m_skyZenithColor[2] = skyZenithColor[2];
            m_skyGroundColor[0] = skyGroundColor[0]; m_skyGroundColor[1] = skyGroundColor[1]; m_skyGroundColor[2] = skyGroundColor[2];
            m_skyIntensity = skyIntensity;
            m_sunIntensity = sunIntensity;
            m_sunAngularRadius = sunSize;
            m_environmentMapEnabled = cld::JsonBoolOr(params, "environmentEnabled", m_environmentMapEnabled);
            m_environmentIntensity = environmentIntensity;
            m_environmentRotation = environmentRotation;
            m_shadowEnabled = cld::JsonBoolOr(params, "sunNEE", m_shadowEnabled);
            m_skyNeeEnabled = cld::JsonBoolOr(params, "skyNEE", m_skyNeeEnabled);
            m_emissiveLightsEnabled = cld::JsonBoolOr(params, "emissiveTriangleLights", m_emissiveLightsEnabled);
            m_emissiveLightIntensity = emissiveIntensity;
            m_proceduralLightsEnabled = cld::JsonBoolOr(params, "proceduralAreaLights", m_proceduralLightsEnabled);
            m_proceduralLightIntensity = areaLightIntensity;
            InvalidateHistory(rb::FrameChangeMask::Light);
            m_projectDirty = true;
        }
        diagnostics = "Lighting settings accepted.";
        return true;
    }
    if (method == "set_path_tracing")
    {
        const int samples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "samplesPerFrame", m_giSamplesPerFrame)), 1, 8);
        const int maxBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxBounces", m_maxPathBounces)), 1, 8);
        const int minBounces = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "minBounces", m_minPathBounces)), 0, maxBounces);
        const PathTracingMode mode = PathtracingModeFromName(cld::JsonStringOr(params, "mode"), m_mode);
        const float radianceClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "radianceClamp", m_giRadianceClamp)), 1.0f, 100.0f);
        const float temporalClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalClamp", m_giTemporalClampScale)), 0.25f, 4.0f);
        const int maxAccumSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxAccumSamples", m_maxAccumulatedFrames)), 1, 4096);
        const int maxAdaptiveSpp = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxAdaptiveSPP", m_maxAdaptiveSamplesPerPixel)), 1, 4);
        const float varianceThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "varianceThreshold", m_adaptiveVarianceThreshold)), 0.02f, 1.0f);
        const float disocclusionBoost = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "disocclusionBoost", m_adaptiveDisocclusionBoost)), 0.0f, 4.0f);
        if (!AllFinite({ radianceClamp, temporalClamp, varianceThreshold, disocclusionBoost }))
        {
            diagnostics = "Path tracing settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            const bool modeChanged = mode != m_mode;
            m_mode = mode;
            m_giSamplesPerFrame = samples;
            m_maxPathBounces = maxBounces;
            m_minPathBounces = minBounces;
            m_giRadianceClamp = radianceClamp;
            m_giTemporalClampScale = temporalClamp;
            m_maxAccumulatedFrames = maxAccumSamples;
            m_freezeAccumulation = cld::JsonBoolOr(params, "freezeAccumulation", m_freezeAccumulation);
            m_adaptiveSamplingEnabled = cld::JsonBoolOr(params, "adaptiveSampling", m_adaptiveSamplingEnabled);
            m_maxAdaptiveSamplesPerPixel = maxAdaptiveSpp;
            m_adaptiveVarianceThreshold = varianceThreshold;
            m_adaptiveDisocclusionBoost = disocclusionBoost;
            InvalidateHistory(rb::FrameChangeMask::Backend);
            if (modeChanged)
            {
                CreateGpuResourcesForCurrentScene();
            }
            m_projectDirty = true;
        }
        diagnostics = "Path tracing settings accepted.";
        return true;
    }
    if (method == "set_restir")
    {
        const int spatialPasses = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "spatialReusePasses", m_restirSpatialReusePasses)), 0, 4);
        const int spatialRadius = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "spatialRadius", m_restirSpatialRadius)), 1, 64);
        const int candidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "candidateSamples", m_restirCandidateSamples)), 1, 4);
        const float mClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "mClamp", m_restirMClamp)), 1.0f, 64.0f);
        const int diSpatialPasses = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "diSpatialReusePasses", m_restirDiSpatialReusePasses)), 0, 4);
        const int diCandidateSamples = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "diCandidateSamples", m_restirDiCandidateSamples)), 1, 4);
        const float diMClamp = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "diMClamp", m_restirDiMClamp)), 1.0f, 64.0f);
        const int reservoirMaxAge = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "reservoirMaxAge", m_reservoirMaxAge)), 1, 32);
        if (!AllFinite({ mClamp, diMClamp }))
        {
            diagnostics = "ReSTIR settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            m_restirTemporalReuse = cld::JsonBoolOr(params, "temporalReuse", m_restirTemporalReuse);
            m_restirSpatialReusePasses = spatialPasses;
            m_restirSpatialRadius = spatialRadius;
            m_restirCandidateSamples = candidateSamples;
            m_restirMClamp = mClamp;
            m_restirDiTemporalReuse = cld::JsonBoolOr(params, "diTemporalReuse", m_restirDiTemporalReuse);
            m_restirDiSpatialReusePasses = diSpatialPasses;
            m_restirDiCandidateSamples = diCandidateSamples;
            m_restirDiMClamp = diMClamp;
            m_reservoirReprojection = cld::JsonBoolOr(params, "reservoirReprojection", m_reservoirReprojection);
            m_reservoirValidation = cld::JsonBoolOr(params, "reservoirValidation", m_reservoirValidation);
            m_restirGiValidationRay = cld::JsonBoolOr(params, "giValidationRay", m_restirGiValidationRay);
            m_reservoirMaxAge = reservoirMaxAge;
            InvalidateHistory(rb::FrameChangeMask::Backend);
            m_projectDirty = true;
        }
        diagnostics = "ReSTIR settings accepted.";
        return true;
    }
    if (method == "set_denoise")
    {
        NoisePreset preset = m_noisePreset;
        bool hasPreset = false;
        if (const cld::JsonValue* presetValue = cld::FindMember(params, "preset"))
        {
            if (presetValue->type != cld::JsonValue::Type::String || !TryParseNoisePreset(presetValue->string, preset))
            {
                diagnostics = "Denoise preset is invalid.";
                return false;
            }
            hasPreset = true;
        }

        JitterMode jitterMode = m_jitterMode;
        if (const cld::JsonValue* jitterValue = cld::FindMember(params, "jitterMode"))
        {
            if (jitterValue->type != cld::JsonValue::Type::String || !TryParseJitterMode(jitterValue->string, jitterMode))
            {
                diagnostics = "Denoise jitterMode is invalid.";
                return false;
            }
        }

        DenoiseBackend denoiseBackend = m_denoiseBackend;
        if (const cld::JsonValue* backendValue = cld::FindMember(params, "backend"))
        {
            if (backendValue->type != cld::JsonValue::Type::String || !TryParseDenoiseBackend(backendValue->string, denoiseBackend))
            {
                diagnostics = "Denoise backend is invalid.";
                return false;
            }
        }

        DlssMode dlssMode = m_dlssMode;
        if (const cld::JsonValue* dlssModeValue = cld::FindMember(params, "dlssMode"))
        {
            if (dlssModeValue->type != cld::JsonValue::Type::String || !TryParseDlssMode(dlssModeValue->string, dlssMode))
            {
                diagnostics = "Denoise dlssMode is invalid.";
                return false;
            }
        }

        const float strength = static_cast<float>(cld::JsonNumberOr(params, "strength", m_denoiserStrength));
        const float temporalAlphaMin = static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMin", m_temporalAlphaMin));
        const float temporalAlphaMax = static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMax", m_temporalAlphaMax));
        const float historyClampSigma = static_cast<float>(cld::JsonNumberOr(params, "historyClampSigma", m_historyClampSigma));
        const float reactiveThreshold = static_cast<float>(cld::JsonNumberOr(params, "reactiveThreshold", m_reactiveThreshold));
        const float specularHistoryScale = static_cast<float>(cld::JsonNumberOr(params, "specularHistoryScale", m_specularHistoryScale));
        const float diffuseFilterStrength = static_cast<float>(cld::JsonNumberOr(params, "diffuseFilterStrength", m_atrousDiffuseStrength));
        const float specularFilterStrength = static_cast<float>(cld::JsonNumberOr(params, "specularFilterStrength", m_atrousSpecularStrength));
        const float varianceScale = static_cast<float>(cld::JsonNumberOr(params, "varianceScale", m_atrousVarianceScale));
        const float normalSigma = static_cast<float>(cld::JsonNumberOr(params, "normalSigma", m_denoiserNormalSigma));
        const float depthSigma = static_cast<float>(cld::JsonNumberOr(params, "depthSigma", m_denoiserDepthSigma));
        const float luminanceSigma = static_cast<float>(cld::JsonNumberOr(params, "luminanceSigma", m_denoiserLuminanceSigma));
        const float albedoSigma = static_cast<float>(cld::JsonNumberOr(params, "albedoSigma", m_denoiserAlbedoSigma));
        const float movingJitterScale = static_cast<float>(cld::JsonNumberOr(params, "movingJitterScale", m_movingJitterScale));
        const float maxHistoryFrames = static_cast<float>(cld::JsonNumberOr(params, "maxHistoryFrames", m_reconstructionMaxHistoryFrames));
        const rb::DenoiseAtrousPassesCompatibility atrousCompatibility =
            rb::ResolveDenoiseAtrousPasses(params, m_atrousPassCount);
        if (!AllFinite({ strength, temporalAlphaMin, temporalAlphaMax, historyClampSigma, reactiveThreshold, specularHistoryScale,
            diffuseFilterStrength, specularFilterStrength, varianceScale, normalSigma, depthSigma, luminanceSigma, albedoSigma,
            movingJitterScale, maxHistoryFrames }))
        {
            diagnostics = "Denoise settings contain non-finite values.";
            return false;
        }
        if (!validateOnly)
        {
            const bool denoiseBackendChanged = denoiseBackend != m_denoiseBackend;
            const bool previousDenoiserEnabled = m_denoiserEnabled;
            if (hasPreset)
            {
                ApplyNoisePreset(preset);
            }
            else
            {
                m_noisePreset = preset;
            }

            m_denoiserEnabled = cld::JsonBoolOr(params, "enabled", m_denoiserEnabled);
            m_denoiseBackend = denoiseBackend;
            m_dlssMode = dlssMode;
            m_dlssEnabledWhenAvailable = cld::JsonBoolOr(params, "dlssEnabledWhenAvailable", m_dlssEnabledWhenAvailable);
            m_dlssBackendRuntime.UpdateMode(m_width, m_height, m_dlssMode);
            WaitForPreviousFrame();
            m_nrdBackendRuntime.UpdateMethod(m_device.Get(), SelectedNrdMethod());
            m_splitSignalDenoise = cld::JsonBoolOr(params, "splitSignalDenoise", m_splitSignalDenoise);
            m_realtimeReconstruction = cld::JsonBoolOr(params, "realtimeReconstruction", m_realtimeReconstruction);
            m_cameraJitter = cld::JsonBoolOr(params, "cameraJitter", m_cameraJitter);
            m_temporalStabilityEnabled = cld::JsonBoolOr(params, "temporalStability", m_temporalStabilityEnabled);
            m_jitterMode = jitterMode;
            m_movingJitterScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "movingJitterScale", m_movingJitterScale)), 0.0f, 1.0f);
            m_reconstructionMaxHistoryFrames = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "maxHistoryFrames", m_reconstructionMaxHistoryFrames)), 1, 128);
            m_temporalAlphaMin = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMin", m_temporalAlphaMin)), 0.01f, 0.5f);
            m_temporalAlphaMax = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "temporalAlphaMax", m_temporalAlphaMax)), m_temporalAlphaMin, 0.8f);
            m_historyClampSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "historyClampSigma", m_historyClampSigma)), 0.5f, 4.0f);
            m_reactiveThreshold = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "reactiveThreshold", m_reactiveThreshold)), 0.05f, 1.0f);
            m_specularHistoryScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "specularHistoryScale", m_specularHistoryScale)), 0.0f, 1.0f);
            m_atrousPassCount = atrousCompatibility.atrousPasses;
            if (atrousCompatibility.usedLegacySpatialIterations)
            {
                m_denoiserSpatialIterations = (std::min)(m_atrousPassCount, 4);
            }
            m_atrousDiffuseStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "diffuseFilterStrength", m_atrousDiffuseStrength)), 0.0f, 1.0f);
            m_atrousSpecularStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "specularFilterStrength", m_atrousSpecularStrength)), 0.0f, 1.0f);
            m_atrousVarianceScale = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "varianceScale", m_atrousVarianceScale)), 0.25f, 4.0f);
            m_denoiserNormalSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "normalSigma", m_denoiserNormalSigma)), 0.05f, 1.0f);
            m_denoiserDepthSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "depthSigma", m_denoiserDepthSigma)), 0.002f, 0.10f);
            m_denoiserLuminanceSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "luminanceSigma", m_denoiserLuminanceSigma)), 0.1f, 8.0f);
            m_denoiserAlbedoSigma = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "albedoSigma", m_denoiserAlbedoSigma)), 0.05f, 1.0f);
            m_denoiserStrength = std::clamp(static_cast<float>(cld::JsonNumberOr(params, "strength", m_denoiserStrength)), 0.0f, 1.0f);
            if (cld::JsonBoolOr(params, "resetHistory", true))
            {
                InvalidateHistory(denoiseBackendChanged
                    ? rb::FrameChangeMask::Backend
                    : rb::FrameChangeMask::DenoiserSettings);
            }
            else
            {
                ResetAccumulation();
            }
            if (cld::JsonBoolOr(params, "resetDlss", false))
            {
                m_dlssBackendRuntime.ResetHistory();
            }
            if (cld::JsonBoolOr(params, "resetNrd", false))
            {
                m_nrdBackendRuntime.ResetHistory();
            }
            const bool fusedNrdContractChanged =
                IsNrdSelected() && previousDenoiserEnabled != m_denoiserEnabled;
            if (denoiseBackendChanged || fusedNrdContractChanged)
            {
                // Backend-specific histories are allocated exclusively. Rebuild
                // the resource graph only when the selected backend changes.
                CreateGpuResourcesForCurrentScene();
            }
            m_projectDirty = true;
        }
        diagnostics = "Denoise settings accepted.";
        if (!atrousCompatibility.warning.empty())
        {
            diagnostics += " " + atrousCompatibility.warning;
            if (!validateOnly)
            {
                LogDiagnostic(atrousCompatibility.warning);
            }
        }
        return true;
    }
    if (method == "set_view")
    {
        const int debugView = std::clamp(static_cast<int>(cld::JsonNumberOr(params, "debugView", m_debugViewMode)), 0, DebugViewMax);
        const bool changesEnvironment = cld::FindMember(params, "environmentEnabled") != nullptr;
        if (!validateOnly)
        {
            const bool debugContractChanged = debugView != m_debugViewMode ||
                cld::JsonBoolOr(params, "normalMapYFlip", m_debugNormalMapYFlip) != m_debugNormalMapYFlip;
            const bool fusedNrdContractChanged =
                IsNrdSelected() && ((debugView == 0) != (m_debugViewMode == 0));
            m_debugViewMode = debugView;
            m_debugNormalMapYFlip = cld::JsonBoolOr(params, "normalMapYFlip", m_debugNormalMapYFlip);
            m_environmentMapEnabled = cld::JsonBoolOr(params, "environmentEnabled", m_environmentMapEnabled);
            if (changesEnvironment)
            {
                InvalidateHistory(rb::FrameChangeMask::Light);
            }
            else if (debugContractChanged)
            {
                InvalidateHistory(rb::FrameChangeMask::DenoiserSettings);
            }
            if (fusedNrdContractChanged)
            {
                CreateGpuResourcesForCurrentScene();
            }
            m_projectDirty = true;
        }
        diagnostics = "View settings accepted.";
        return true;
    }
    diagnostics = "Unsupported action method.";
    return false;
}

mcp::ToolResult D3D12PathTracingBackend::CallMcpTool(const std::string& name, const cld::JsonValue& arguments, int timeoutMs)
{
    if (name == "lookdevpt.get_stats")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Renderer stats returned.", m_mcpStatsJson);
    }
    if (name == "lookdevpt.get_state")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Renderer state returned.", m_mcpStateJson);
    }
    if (name == "lookdevpt.list_materials")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Material list returned.", m_mcpMaterialsJson);
    }
    if (name == "lookdevpt.list_debug_views")
    {
        return MakeMcpJsonToolResult(true, "Debug view list returned.", BuildDebugViewsJson());
    }
    if (name == "lookdevpt.list_render_modes")
    {
        return MakeMcpJsonToolResult(true, "Render mode list returned.", BuildRenderModesJson());
    }
    if (name == "lookdevpt.get_diagnostics")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Diagnostics returned.", m_mcpDiagnosticsJson);
    }
    if (name == "lookdevpt.get_benchmark")
    {
        uint64_t id = 0;
        if (const cld::JsonValue* value = cld::FindMember(arguments, "benchmarkId"))
        {
            if (value->type == cld::JsonValue::Type::Number) id = static_cast<uint64_t>((std::max)(value->number, 0.0));
            else if (value->type == cld::JsonValue::Type::String) id = std::strtoull(value->string.c_str(), nullptr, 10);
        }
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        const std::string json = BuildMcpBenchmarkJson(id);
        const bool found = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
            [&](const McpBenchmark& benchmark) { return benchmark.id == id; }) != m_mcpBenchmarks.end();
        return MakeMcpJsonToolResult(found, found ? "Benchmark returned." : "Benchmark id was not found.", json);
    }
    if (auto reviewTool = CallMcpReviewTool(name, arguments, timeoutMs))
    {
        return *reviewTool;
    }
    if (name == "lookdevpt.capture_viewport")
    {
        return SubmitMcpCommandTool(name, "__capture_viewport", arguments, false, timeoutMs);
    }
    if (name == "lookdevpt.capture_debug_pack")
    {
        return SubmitMcpCommandTool(name, "__capture_debug_pack", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.validate_action")
    {
        const std::string method = cld::JsonStringOr(arguments, "method");
        const cld::JsonValue* params = cld::FindMember(arguments, "params");
        if (method.empty() || !params || params->type != cld::JsonValue::Type::Object)
        {
            return MakeMcpJsonToolResult(false, "validate_action requires method and object params.", "{\"ok\":false,\"diagnostics\":\"validate_action requires method and object params.\"}");
        }
        return SubmitMcpActionTool(name, method, *params, true, timeoutMs);
    }
    if (name == "lookdevpt.run_actions")
    {
        const bool validateOnly = cld::JsonBoolOr(arguments, "validateOnly", false);
        return SubmitMcpCommandTool(name, "__run_actions", arguments, !validateOnly, timeoutMs);
    }
    if (name == "lookdevpt.create_checkpoint")
    {
        return SubmitMcpCommandTool(name, "__create_checkpoint", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.restore_checkpoint")
    {
        return SubmitMcpCommandTool(name, "__restore_checkpoint", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.delete_checkpoint")
    {
        return SubmitMcpCommandTool(name, "__delete_checkpoint", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.start_benchmark")
    {
        return SubmitMcpCommandTool(name, "__start_benchmark", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.cancel_benchmark")
    {
        return SubmitMcpCommandTool(name, "__cancel_benchmark", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.reset_accumulation")
    {
        return SubmitMcpCommandTool(name, "__reset_accumulation", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.reset_denoise_history")
    {
        return SubmitMcpCommandTool(name, "__reset_denoise_history", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.reset_reservoirs")
    {
        return SubmitMcpCommandTool(name, "__reset_reservoirs", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.reset_camera_view")
    {
        return SubmitMcpCommandTool(name, "__reset_camera_view", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.set_camera_speed")
    {
        return SubmitMcpCommandTool(name, "__set_camera_speed", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.fit_camera_to_scene")
    {
        return SubmitMcpCommandTool(name, "__fit_camera_to_scene", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.set_display_resolution")
    {
        return SubmitMcpCommandTool(name, "__set_display_resolution", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.load_project")
    {
        return SubmitMcpCommandTool(name, "__load_project", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.save_project")
    {
        return SubmitMcpCommandTool(name, "__save_project", arguments, true, timeoutMs);
    }
    if (name == "lookdevpt.save_project_as")
    {
        return SubmitMcpCommandTool(name, "__save_project_as", arguments, true, timeoutMs);
    }

    constexpr const char* prefix = "lookdevpt.";
    if (name.rfind(prefix, 0) == 0)
    {
        const std::string actionMethod = name.substr(std::char_traits<char>::length(prefix));
        if (actionMethod == "set_scene" || actionMethod == "set_camera" || actionMethod == "set_material" ||
            actionMethod == "set_lighting" || actionMethod == "set_path_tracing" || actionMethod == "set_restir" ||
            actionMethod == "set_denoise" || actionMethod == "set_quality" || actionMethod == "set_view" || actionMethod == "set_material_texture" ||
            actionMethod == "reset_material" || actionMethod == "save_material_variant" || actionMethod == "apply_material_variant" ||
            actionMethod == "delete_material_variant" || actionMethod == "set_material_view" || actionMethod == "set_color_management")
        {
            return SubmitMcpActionTool(name, actionMethod, arguments, false, timeoutMs);
        }
    }

    return MakeMcpJsonToolResult(false, "Unknown MCP tool.", "{\"ok\":false,\"diagnostics\":\"Unknown MCP tool.\"}");
}

std::optional<mcp::ToolResult> D3D12PathTracingBackend::CallMcpReviewTool(
    const std::string& name,
    const cld::JsonValue& arguments,
    int timeoutMs)
{
    if (name == "lookdevpt.audit_scene")
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        return MakeMcpJsonToolResult(true, "Scene audit returned.", m_mcpAuditJson);
    }
    if (name == "lookdevpt.probe_surfaces")
    {
        return SubmitMcpCommandTool(name, "__probe_surfaces", arguments, false, timeoutMs);
    }
    if (name == "lookdevpt.compare_captures")
    {
        auto parseId = [&](const char* member) -> uint64_t
        {
            const cld::JsonValue* value = cld::FindMember(arguments, member);
            if (!value) return 0;
            if (value->type == cld::JsonValue::Type::Number && value->number > 0.0)
                return static_cast<uint64_t>(value->number);
            if (value->type != cld::JsonValue::Type::String) return 0;
            char* end = nullptr;
            const uint64_t parsed = std::strtoull(value->string.c_str(), &end, 10);
            return end && *end == '\0' ? parsed : 0;
        };
        const uint64_t beforeId = parseId("beforeCaptureId");
        const uint64_t afterId = parseId("afterCaptureId");
        std::string comparisonJson;
        std::string comparisonUri;
        {
            std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
            auto findCapture = [&](uint64_t id) -> const McpCapture*
            {
                for (const auto& capture : m_mcpCaptures) if (capture.id == id) return &capture;
                return nullptr;
            };
            const McpCapture* before = findCapture(beforeId);
            const McpCapture* after = findCapture(afterId);
            if (!before || !after)
                return MakeMcpJsonToolResult(false, "Capture id was not found.",
                    "{\"ok\":false,\"code\":\"capture_not_found\",\"diagnostics\":\"Capture id was not found.\"}");
            lookdevpt::review::ComparisonMetrics metrics;
            lookdevpt::review::Rgba8Image heatmap;
            std::string diagnostics;
            if (!lookdevpt::review::CompareImages(before->rgba, after->rgba, metrics, heatmap, diagnostics))
            {
                const std::string code = diagnostics == "resolution_mismatch" ? diagnostics : "comparison_failed";
                return MakeMcpJsonToolResult(false, diagnostics,
                    "{\"ok\":false,\"code\":\"" + code + "\",\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\"}");
            }
            McpComparison comparison;
            comparison.id = m_nextMcpComparisonId++;
            comparison.beforeCaptureId = beforeId;
            comparison.afterCaptureId = afterId;
            const std::string id = std::to_string(comparison.id);
            comparison.json = lookdevpt::review::BuildComparisonJson(id, std::to_string(beforeId),
                std::to_string(afterId), metrics, before->sceneFingerprint == after->sceneFingerprint,
                before->cameraFingerprint == after->cameraFingerprint);
            EncodePngRgba8(heatmap, comparison.heatmapBase64);
            comparison.artifactBytes = comparison.heatmapBase64.size() * 3u / 4u;
            comparison.accessSerial = m_mcpArtifactAccessSerial++;
            comparisonJson = comparison.json;
            comparisonUri = "lookdevpt://comparisons/" + id;
            m_mcpComparisons.push_back(std::move(comparison));
            EnforceMcpArtifactBudget();
        }
        m_mcpServer.PublishResourceUpdates({ comparisonUri, comparisonUri + "/heatmap.png" });
        return MakeMcpJsonToolResult(true, "Captures compared.", comparisonJson);
    }
    if (name == "lookdevpt.start_review")
    {
        const std::string preset = cld::JsonStringOr(arguments, "preset", "quick");
        std::vector<int> views;
        if (preset == "quick") views = { 0, 1, 2, 4, 32, 33, 40 };
        else if (preset == "material") views = { 0, 1, 4, 54, 55, 56, 57, 58 };
        else if (preset == "lighting") views = { 0, 32, 33, 12, 8, 9, 34, 23 };
        else if (preset == "temporal") views = { 0, 19, 20, 21, 35, 36, 46, 40 };
        else return MakeMcpJsonToolResult(false, "Unknown review preset.",
            "{\"ok\":false,\"code\":\"invalid_preset\",\"diagnostics\":\"Unknown review preset.\"}");
        const uint64_t settleFrames = static_cast<uint64_t>(std::clamp(
            static_cast<int>(cld::JsonNumberOr(arguments, "settleFrames", 8.0)), 0, 600));
        const int timeoutSeconds = std::clamp(
            static_cast<int>(cld::JsonNumberOr(arguments, "timeoutSeconds", 120.0)), 1, 600);
        uint64_t baseline = 0;
        if (const cld::JsonValue* value = cld::FindMember(arguments, "baselineCaptureId"))
        {
            if (value->type == cld::JsonValue::Type::String) baseline = std::strtoull(value->string.c_str(), nullptr, 10);
            else if (value->type == cld::JsonValue::Type::Number) baseline = static_cast<uint64_t>(value->number);
        }
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
            if (m_activeMcpReviewId != 0)
                return MakeMcpJsonToolResult(false, "A review is already running.",
                    "{\"ok\":false,\"code\":\"review_busy\",\"currentReviewId\":\"" +
                    std::to_string(m_activeMcpReviewId) + "\"}");
            auto baselineIterator = std::find_if(m_mcpCaptures.begin(), m_mcpCaptures.end(),
                [&](const McpCapture& capture) { return capture.id == baseline; });
            if (baseline != 0 && baselineIterator == m_mcpCaptures.end())
                return MakeMcpJsonToolResult(false, "Baseline capture was not found.",
                    "{\"ok\":false,\"code\":\"capture_not_found\",\"diagnostics\":\"Baseline capture was not found.\"}");
            McpReview review;
            review.id = m_nextMcpReviewId++;
            review.preset = preset;
            review.views = std::move(views);
            review.baselineCaptureId = baseline;
            review.settleUntilFrame = settleFrames;
            review.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
            review.auditJson = m_mcpAuditJson;
            if (baseline != 0) ++baselineIterator->pinCount;
            id = review.id;
            m_activeMcpReviewId = id;
            m_mcpReviews.push_back(std::move(review));
            while (m_mcpReviews.size() > 32u) m_mcpReviews.pop_front();
        }
        const std::string idText = std::to_string(id);
        m_mcpServer.PublishResourceUpdates({ "lookdevpt://reviews/index", "lookdevpt://reviews/" + idText });
        return MakeMcpJsonToolResult(true, "Review queued.",
            "{\"ok\":true,\"reviewId\":\"" + idText + "\",\"state\":\"queued\",\"resource\":\"lookdevpt://reviews/" + idText + "\"}");
    }
    if (name == "lookdevpt.get_review" || name == "lookdevpt.cancel_review")
    {
        const uint64_t id = std::strtoull(cld::JsonStringOr(arguments, "reviewId").c_str(), nullptr, 10);
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
            [&](const McpReview& review) { return review.id == id; });
        if (iterator == m_mcpReviews.end()) return MakeMcpJsonToolResult(false, "Review id was not found.",
            "{\"ok\":false,\"code\":\"review_not_found\"}");
        if (name == "lookdevpt.cancel_review" && m_activeMcpReviewId == id) iterator->cancelRequested = true;
        const std::string json = BuildMcpReviewJson(id);
        return MakeMcpJsonToolResult(!iterator->artifactEvicted,
            iterator->artifactEvicted ? "A review artifact was evicted." :
                (name == "lookdevpt.cancel_review" ? "Review cancellation requested." : "Review returned."), json);
    }
    return std::nullopt;
}

mcp::ResourceResult D3D12PathTracingBackend::ReadMcpResource(const std::string& uri)
{
    mcp::ResourceResult result;
    result.uri = uri;
    if (uri == "lookdevpt://integration")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = "{\"application\":{\"name\":\"D3D12LookDevPTWinUI\",\"version\":\"" +
            std::string(mcp::ApplicationVersion) + "\"},\"contractVersion\":\"" + mcp::ContractVersion +
            R"json(","features":{"imageArtifacts":true,"structuredContent":true,"resourceTemplates":true,"prompts":true,"resourceSubscriptions":true,"sceneAudit":true,"viewportCapture":true,"asyncReview":true,"comparisonHeatmap":true,"surfaceProbe":true,"pairing":true,"checkpoints":true,"benchmarks":true,"gltfMaterialExtensionsV1":true,"textureResidencyV1":true},"artifactLimits":{"maxImageBytes":16777216,"maxImagesPerToolCall":8,"maxTurnBytes":67108864,"maxDecodedPixels":64000000}})json";
        return result;
    }
    if (uri == "lookdevpt://actions/schema")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = mcp::BuildActionsSchemaJson();
        return result;
    }
    if (uri == "lookdevpt://debug-views")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildDebugViewsJson();
        return result;
    }
    if (uri == "lookdevpt://render-modes")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildRenderModesJson();
        return result;
    }
    constexpr const char* capturePrefix = "lookdevpt://captures/";
    if (uri.rfind(capturePrefix, 0) == 0 && uri != "lookdevpt://captures/latest.png" && uri != "lookdevpt://captures/index")
    {
        const std::string suffix = uri.substr(std::char_traits<char>::length(capturePrefix));
        if (suffix.size() > 4 && suffix.compare(suffix.size() - 4, 4, ".png") == 0)
        {
            const std::string idText = suffix.substr(0, suffix.size() - 4);
            char* end = nullptr;
            const unsigned long long id = std::strtoull(idText.c_str(), &end, 10);
            if (end && *end == '\0' && id > 0)
            {
                std::string label;
                if (FindMcpCapture(static_cast<uint64_t>(id), result.blob, label))
                {
                    result.ok = true;
                    result.mimeType = "image/png";
                    return result;
                }
            }
        }
        result.error = "Capture id was not found.";
        return result;
    }

    std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
    if (uri == "lookdevpt://state")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpStateJson;
        return result;
    }
    if (uri == "lookdevpt://stats")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpStatsJson;
        return result;
    }
    if (uri == "lookdevpt://diagnostics")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpDiagnosticsJson;
        return result;
    }
    if (uri == "lookdevpt://materials")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpMaterialsJson;
        return result;
    }
    if (uri == "lookdevpt://material-variants")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpMaterialVariantsJson;
        return result;
    }
    if (uri == "lookdevpt://material-presets")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpMaterialPresetsJson;
        return result;
    }
    constexpr const char* materialPrefix = "lookdevpt://materials/";
    if (uri.rfind(materialPrefix, 0) == 0)
    {
        const std::string indexText = uri.substr(std::char_traits<char>::length(materialPrefix));
        constexpr const char* textureSuffix = "/textures";
        if (indexText.size() > std::char_traits<char>::length(textureSuffix) &&
            indexText.compare(indexText.size() - std::char_traits<char>::length(textureSuffix), std::char_traits<char>::length(textureSuffix), textureSuffix) == 0)
        {
            const std::string materialIndexText = indexText.substr(0, indexText.size() - std::char_traits<char>::length(textureSuffix));
            char* textureEnd = nullptr;
            const unsigned long textureIndex = std::strtoul(materialIndexText.c_str(), &textureEnd, 10);
            if (textureEnd && *textureEnd == '\0' && textureIndex < m_scene.materials.size())
            {
                result.ok = true;
                result.mimeType = "application/json";
                result.text = "{\"materialIndex\":" + std::to_string(textureIndex) + ",\"textures\":" + BuildMaterialTexturesJson(textureIndex) + "}";
                return result;
            }
            result.error = "Material texture resource index was not found.";
            return result;
        }
        char* end = nullptr;
        const unsigned long index = std::strtoul(indexText.c_str(), &end, 10);
        if (!end || *end != '\0')
        {
            result.error = "Material index resource is invalid.";
            return result;
        }
        try
        {
            const cld::JsonValue root = cld::JsonParser(m_mcpMaterialsJson).Parse();
            const cld::JsonValue* materials = cld::FindMember(root, "materials");
            if (materials && materials->type == cld::JsonValue::Type::Array && index < materials->array.size())
            {
                result.ok = true;
                result.mimeType = "application/json";
                result.text = cld::JsonValueToJson(materials->array[index]);
                return result;
            }
        }
        catch (const std::exception&)
        {
        }
        result.error = "Material index was not found.";
        return result;
    }
    if (uri == "lookdevpt://project")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpProjectJson;
        return result;
    }
    if (uri == "lookdevpt://scene/summary")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpSceneSummaryJson;
        return result;
    }
    if (uri == "lookdevpt://scene/audit")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpAuditJson;
        return result;
    }
    if (uri == "lookdevpt://probes/latest")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = m_mcpLatestProbesJson;
        return result;
    }
    if (uri == "lookdevpt://checkpoints/index")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildMcpCheckpointIndexJson();
        return result;
    }
    constexpr const char* checkpointPrefix = "lookdevpt://checkpoints/";
    if (uri.rfind(checkpointPrefix, 0) == 0)
    {
        const uint64_t id = std::strtoull(
            uri.c_str() + std::char_traits<char>::length(checkpointPrefix), nullptr, 10);
        const auto iterator = std::find_if(m_mcpCheckpoints.begin(), m_mcpCheckpoints.end(),
            [&](const McpCheckpoint& checkpoint) { return checkpoint.id == id; });
        if (iterator == m_mcpCheckpoints.end())
        {
            result.error = "Checkpoint id was not found.";
            return result;
        }
        result.ok = true;
        result.mimeType = "application/json";
        result.text = "{\"id\":\"" + std::to_string(iterator->id) + "\",\"label\":\"" +
            cld::EscapeJson(iterator->label) + "\",\"sceneFingerprint\":\"" +
            cld::EscapeJson(iterator->sceneFingerprint) + "\",\"cameraFingerprint\":\"" +
            cld::EscapeJson(iterator->cameraFingerprint) + "\"}";
        return result;
    }
    if (uri == "lookdevpt://benchmarks/index")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildMcpBenchmarkIndexJson();
        return result;
    }
    constexpr const char* benchmarkPrefix = "lookdevpt://benchmarks/";
    if (uri.rfind(benchmarkPrefix, 0) == 0)
    {
        const std::string suffix = uri.substr(std::char_traits<char>::length(benchmarkPrefix));
        const size_t separator = suffix.find('/');
        const std::string idText = separator == std::string::npos ? suffix : suffix.substr(0, separator);
        const uint64_t id = std::strtoull(idText.c_str(), nullptr, 10);
        const auto iterator = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
            [&](const McpBenchmark& benchmark) { return benchmark.id == id; });
        if (iterator == m_mcpBenchmarks.end())
        {
            result.error = "Benchmark id was not found.";
            return result;
        }
        if (separator == std::string::npos)
        {
            result.ok = true;
            result.mimeType = "application/json";
            result.text = BuildMcpBenchmarkJson(id);
            return result;
        }
        const std::string artifactName = suffix.substr(separator + 1);
        const bool jsonArtifact = artifactName == "summary.json" || artifactName == "artifacts.json" ||
            artifactName == "quality_analysis.json";
        const bool csvArtifact = artifactName == "frames.csv";
        if ((!jsonArtifact && !csvArtifact) || iterator->state != "completed")
        {
            result.error = "Benchmark artifact is unavailable.";
            return result;
        }
        const std::filesystem::path artifactPath = iterator->outputDirectory / Utf8ToWide(artifactName);
        std::ifstream artifactFile(artifactPath, std::ios::binary);
        if (!artifactFile)
        {
            result.error = "Benchmark artifact file was not found.";
            return result;
        }
        std::stringstream artifactBuffer;
        artifactBuffer << artifactFile.rdbuf();
        result.ok = true;
        result.mimeType = jsonArtifact ? "application/json" : "text/csv";
        result.text = artifactBuffer.str();
        return result;
    }
    if (uri == "lookdevpt://reviews/index")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildMcpReviewIndexJson();
        return result;
    }
    constexpr const char* reviewPrefix = "lookdevpt://reviews/";
    if (uri.rfind(reviewPrefix, 0) == 0)
    {
        const uint64_t id = std::strtoull(uri.c_str() + std::char_traits<char>::length(reviewPrefix), nullptr, 10);
        const auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
            [&](const McpReview& review) { return review.id == id; });
        if (iterator == m_mcpReviews.end())
        {
            result.error = "Review id was not found.";
            return result;
        }
        result.ok = !iterator->artifactEvicted;
        result.mimeType = "application/json";
        result.text = BuildMcpReviewJson(id);
        if (iterator->artifactEvicted) result.error = "artifact_evicted";
        return result;
    }
    constexpr const char* comparisonPrefix = "lookdevpt://comparisons/";
    if (uri.rfind(comparisonPrefix, 0) == 0)
    {
        std::string suffix = uri.substr(std::char_traits<char>::length(comparisonPrefix));
        constexpr const char* heatmapSuffix = "/heatmap.png";
        const bool heatmap = suffix.size() > std::char_traits<char>::length(heatmapSuffix) &&
            suffix.compare(suffix.size() - std::char_traits<char>::length(heatmapSuffix),
                std::char_traits<char>::length(heatmapSuffix), heatmapSuffix) == 0;
        if (heatmap) suffix.resize(suffix.size() - std::char_traits<char>::length(heatmapSuffix));
        const uint64_t id = std::strtoull(suffix.c_str(), nullptr, 10);
        const auto iterator = std::find_if(m_mcpComparisons.begin(), m_mcpComparisons.end(),
            [&](const McpComparison& comparison) { return comparison.id == id; });
        if (iterator == m_mcpComparisons.end())
        {
            result.error = "Comparison id was not found.";
            return result;
        }
        result.ok = true;
        if (heatmap)
        {
            result.mimeType = "image/png";
            result.blob = iterator->heatmapBase64;
        }
        else
        {
            result.mimeType = "application/json";
            result.text = iterator->json;
        }
        return result;
    }
    if (uri == "lookdevpt://captures/index")
    {
        result.ok = true;
        result.mimeType = "application/json";
        result.text = BuildMcpCaptureIndexJson();
        return result;
    }
    if (uri == "lookdevpt://captures/latest.png")
    {
        if (m_mcpLatestCaptureBase64.empty())
        {
            result.error = m_mcpLastCaptureDiagnostics;
            return result;
        }
        result.ok = true;
        result.mimeType = "image/png";
        result.blob = m_mcpLatestCaptureBase64;
        return result;
    }

    result.error = "Unknown MCP resource URI.";
    return result;
}

size_t D3D12PathTracingBackend::PendingMcpCommandCount() const
{
    return m_mcpDispatcher.PendingCount();
}

void D3D12PathTracingBackend::LoadMcpUserSettings()
{
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        m_mcpSettings = mcp::ServerSettings{};
    }
    mcp::ServerSettings loadedSettings;
    loadedSettings.pairedClientsPath = (std::filesystem::path(UserSettingsDirectory()) / L"mcp-paired-clients.json").string();
    bool shouldSaveSettings = false;
    const std::wstring path = McpSettingsPath();
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        shouldSaveSettings = true;
    }
    if (file)
    {
        std::stringstream buffer;
        buffer << file.rdbuf();
        try
        {
            const cld::JsonValue root = cld::JsonParser(buffer.str()).Parse();
            if (root.type == cld::JsonValue::Type::Object)
            {
                loadedSettings.port = static_cast<uint16_t>(std::clamp(static_cast<int>(cld::JsonNumberOr(root, "port", loadedSettings.port)), 1, 65535));
                loadedSettings.requestTimeoutSeconds = std::clamp(static_cast<int>(cld::JsonNumberOr(root, "requestTimeoutSeconds", loadedSettings.requestTimeoutSeconds)), 5, 300);
                loadedSettings.accessMode = mcp::AccessModeFromName(cld::JsonStringOr(root, "accessMode"), loadedSettings.accessMode);
                loadedSettings.authenticationMode =
                    mcp::AuthenticationModeFromName(
                        cld::JsonStringOr(root, "authenticationMode"),
                        loadedSettings.authenticationMode);
                loadedSettings.token = cld::JsonStringOr(root, "token", loadedSettings.token);
                if (!loadedSettings.token.empty())
                {
                    if (!WriteMcpTokenCredential(loadedSettings.token))
                        m_mcpUiDiagnostics = "Could not migrate the MCP token to Windows Credential Manager.";
                    shouldSaveSettings = true;
                }
            }
        }
        catch (const std::exception& ex)
        {
            m_mcpUiDiagnostics = std::string("MCP settings ignored: ") + ex.what();
        }
    }
    if (loadedSettings.token.empty()) loadedSettings.token = ReadMcpTokenCredential();
    if (loadedSettings.token.empty())
    {
        loadedSettings.token = mcp::GenerateToken();
        shouldSaveSettings = true;
    }
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        m_mcpSettings = loadedSettings;
    }
    if (shouldSaveSettings)
    {
        SaveMcpUserSettings();
    }
}

void D3D12PathTracingBackend::SaveMcpUserSettings()
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }
    const std::filesystem::path path(McpSettingsPath());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        m_mcpUiDiagnostics = "Failed to save MCP settings.";
        return;
    }

    file << "{\n";
    file << "  \"port\": " << settings.port << ",\n";
    file << "  \"requestTimeoutSeconds\": " << settings.requestTimeoutSeconds << ",\n";
    file << "  \"accessMode\": \"" << mcp::AccessModeName(settings.accessMode) << "\",\n";
    file << "  \"authenticationMode\": \""
         << mcp::AuthenticationModeName(settings.authenticationMode)
         << "\",\n";
    if (!WriteMcpTokenCredential(settings.token))
    {
        m_mcpUiDiagnostics = "Failed to save the MCP bearer token to Windows Credential Manager.";
    }
    file << "  \"tokenSecretRef\": \"windows-credential-manager\"\n";
    file << "}\n";
}

void D3D12PathTracingBackend::StartMcpServer()
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        if (m_mcpSettings.token.empty())
        {
            m_mcpSettings.token = mcp::GenerateToken();
        }
        settings = m_mcpSettings;
    }
    SaveMcpUserSettings();
    if (m_mcpServer.Start(settings, this))
    {
        m_mcpUiDiagnostics = "MCP server started.";
        // Populate every resource once before the server handles its first
        // read, then let the normal 30/10 Hz and revision throttles take over.
        m_mcpStaticSnapshotsValid = false;
        m_mcpNextStateSnapshot = {};
        m_mcpNextStatsSnapshot = {};
        UpdateMcpSnapshots();
    }
    else
    {
        m_mcpUiDiagnostics = m_mcpServer.GetStatus().lastError;
    }
}

void D3D12PathTracingBackend::StopMcpServer()
{
    m_mcpDispatcher.CancelAll("MCP server stopped.");
    bool cancelBenchmark = false;
    McpCheckpoint restoreCheckpoint;
    std::vector<std::filesystem::path> checkpointPaths;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        if (m_activeMcpReviewId != 0)
        {
            for (auto& review : m_mcpReviews)
            {
                if (review.id == m_activeMcpReviewId)
                {
                    review.state = "cancelled";
                    review.stage = "cancelled";
                    review.errorCode.clear();
                    review.diagnostics = "Review cancelled because the MCP server stopped.";
                    for (const uint64_t id : review.captureIds)
                        for (auto& capture : m_mcpCaptures)
                            if (capture.id == id && capture.pinCount != 0u) --capture.pinCount;
                    if (review.baselineCaptureId != 0)
                        for (auto& capture : m_mcpCaptures)
                            if (capture.id == review.baselineCaptureId && capture.pinCount != 0u) --capture.pinCount;
                    break;
                }
            }
            m_activeMcpReviewId = 0;
            EnforceMcpArtifactBudget();
        }
        if (m_activeMcpBenchmarkId != 0)
        {
            const auto benchmark = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
                [&](const McpBenchmark& value) { return value.id == m_activeMcpBenchmarkId; });
            if (benchmark != m_mcpBenchmarks.end())
            {
                benchmark->cancelRequested = true;
                benchmark->diagnostics = "Benchmark cancelled because the MCP server stopped.";
                cancelBenchmark = true;
            }
        }
        if (!m_mcpCheckpoints.empty()) restoreCheckpoint = m_mcpCheckpoints.back();
        for (const McpCheckpoint& checkpoint : m_mcpCheckpoints) checkpointPaths.push_back(checkpoint.snapshotPath);
    }
    if (cancelBenchmark) ProcessMcpBenchmark();
    if (restoreCheckpoint.id != 0 && SceneFingerprint() == restoreCheckpoint.sceneFingerprint)
    {
        std::string diagnostics;
        if (LoadProjectFromDisk(restoreCheckpoint.snapshotPath.wstring(), diagnostics))
        {
            m_projectPath = restoreCheckpoint.projectPath;
            m_projectDiagnostics = restoreCheckpoint.projectDiagnostics;
            m_projectDirty = restoreCheckpoint.projectDirty;
            m_mcpUiDiagnostics = "Unfinished MCP change restored from its checkpoint.";
        }
        else
        {
            m_mcpUiDiagnostics = "MCP checkpoint restoration failed: " + diagnostics;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        m_mcpCheckpoints.clear();
    }
    for (const auto& path : checkpointPaths)
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    if (m_mcpServer.IsRunning())
    {
        m_mcpServer.Stop();
        m_mcpUiDiagnostics = "MCP server stopped.";
    }
}

mcp::CommandResult D3D12PathTracingBackend::ExecuteMcpCommand(const mcp::CommandRequest& request)
{
    mcp::CommandResult result;
    auto finish = [&](bool ok, const std::string& diagnostics, const std::string& structuredJson)
    {
        UpdateMcpSnapshots();
        result.ok = ok;
        result.diagnostics = diagnostics;
        result.structuredJson = structuredJson.empty() ? ("{\"ok\":" + std::string(ok ? "true" : "false") + ",\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\"}") : structuredJson;
        return result;
    };

    try
    {
        if (request.actionMethod == "__capture_viewport")
        {
            std::string base64Png;
            std::string diagnostics;
            const bool ok = CaptureViewportPng(base64Png, diagnostics);
            if (!ok)
            {
                return finish(false, diagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\"}");
            }

            uint64_t captureId = 0;
            {
                const std::string requestedLabel = cld::JsonStringOr(request.params, "label");
                const std::string label = requestedLabel.empty()
                    ? DebugViewLabels[std::clamp(m_debugViewMode, 0, static_cast<int>(_countof(DebugViewLabels)) - 1)]
                    : requestedLabel.substr(0, 128);
                captureId = StoreMcpCapture(base64Png, m_debugViewMode, label);
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                m_mcpLatestCaptureBase64 = std::move(base64Png);
                m_mcpLastCaptureDiagnostics = diagnostics;
            }
            m_mcpServer.PublishResourceUpdates({ "lookdevpt://captures/latest.png" });
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"" << cld::EscapeJson(diagnostics) << "\",\"captureId\":" << captureId
                 << ",\"resource\":\"lookdevpt://captures/latest.png\",\"idResource\":\"lookdevpt://captures/" << captureId
                 << ".png\",\"mimeType\":\"image/png\"}";
            mcp::CommandResult captureResult = finish(true, diagnostics, json.str());
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                captureResult.contentJson = "[{\"type\":\"text\",\"text\":\"" + cld::EscapeJson(diagnostics) +
                    "\"},{\"type\":\"image\",\"data\":\"" + m_mcpLatestCaptureBase64 +
                    "\",\"mimeType\":\"image/png\"},{\"type\":\"resource_link\",\"uri\":\"lookdevpt://captures/latest.png\",\"name\":\"latest_capture\",\"mimeType\":\"image/png\"},{\"type\":\"resource_link\",\"uri\":\"lookdevpt://captures/" +
                    std::to_string(captureId) + ".png\",\"name\":\"capture_" + std::to_string(captureId) + "\",\"mimeType\":\"image/png\"}]";
            }
            return captureResult;
        }

        if (request.actionMethod == "__probe_surfaces")
        {
            const cld::JsonValue* points = cld::FindMember(request.params, "points");
            if (!points || points->type != cld::JsonValue::Type::Array || points->array.empty() || points->array.size() > 16u)
                return finish(false, "probe_surfaces requires 1 to 16 points.",
                    "{\"ok\":false,\"code\":\"invalid_points\",\"diagnostics\":\"probe_surfaces requires 1 to 16 points.\"}");
            const std::string coordinateSpace = cld::JsonStringOr(request.params, "coordinateSpace", "normalized");
            if (coordinateSpace != "normalized" && coordinateSpace != "output_pixels")
                return finish(false, "Unknown probe coordinate space.",
                    "{\"ok\":false,\"code\":\"invalid_coordinate_space\"}");
            std::vector<XMFLOAT2> normalized;
            normalized.reserve(points->array.size());
            std::array<uint32_t, 16u * 64u> records{};
            for (size_t i = 0; i < points->array.size(); ++i)
            {
                const cld::JsonValue& point = points->array[i];
                if (point.type != cld::JsonValue::Type::Object)
                    return finish(false, "Each probe point must be an object.", "{\"ok\":false,\"code\":\"invalid_point\"}");
                float x = static_cast<float>(cld::JsonNumberOr(point, "x", std::numeric_limits<double>::quiet_NaN()));
                float y = static_cast<float>(cld::JsonNumberOr(point, "y", std::numeric_limits<double>::quiet_NaN()));
                if (!std::isfinite(x) || !std::isfinite(y))
                    return finish(false, "Probe coordinates must be finite.", "{\"ok\":false,\"code\":\"invalid_point\"}");
                if (coordinateSpace == "output_pixels")
                {
                    x = m_width > 1u ? x / static_cast<float>(m_width) : 0.0f;
                    y = m_height > 1u ? y / static_cast<float>(m_height) : 0.0f;
                }
                if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
                    return finish(false, "Probe coordinates are outside the viewport.", "{\"ok\":false,\"code\":\"point_out_of_range\"}");
                normalized.emplace_back(x, y);
                memcpy(&records[i * 64u + 0u], &x, sizeof(float));
                memcpy(&records[i * 64u + 1u], &y, sizeof(float));
            }
            if (!m_reviewProbeBuffer || !m_reviewProbeRayGenTable.resource || !m_topLevelAs.result)
                return finish(false, "Surface probe GPU path is not ready.", "{\"ok\":false,\"code\":\"probe_not_ready\"}");

            WaitForPreviousFrame();
            ComPtr<ID3D12Resource> upload = CreateUploadBuffer(records.data(), sizeof(records), L"MCP Review Probe Upload");
            ComPtr<ID3D12Resource> readback;
            const CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(records));
            ThrowIfFailed(m_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
            FrameContext& frame = m_frameContexts[m_frameIndex];
            ThrowIfFailed(frame.commandAllocator->Reset());
            ThrowIfFailed(m_commandList->Reset(frame.commandAllocator.Get(), nullptr));
            auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(m_reviewProbeBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            m_commandList->ResourceBarrier(1, &toCopy);
            m_commandList->CopyBufferRegion(m_reviewProbeBuffer.Get(), 0, upload.Get(), 0, sizeof(records));
            auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(m_reviewProbeBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_commandList->ResourceBarrier(1, &toUav);
            ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
            m_commandList->SetDescriptorHeaps(1, heaps);
            m_commandList->SetPipelineState1(m_stateObject.Get());
            m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
            const UINT guideParity = LastSubmittedSurfaceGuideParity();
            m_commandList->SetComputeRootDescriptorTable(RootOutputTable,
                GpuDescriptor(guideParity == 0u ? static_cast<UINT>(DescriptorOutputUav) : m_alternateOutputTableBase));
            m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
            const UINT constantFrame = (m_frameIndex + FrameCount - 1u) % FrameCount;
            m_commandList->SetComputeRootConstantBufferView(RootSceneConstants,
                m_frameContexts[constantFrame].sceneConstantBuffer->GetGPUVirtualAddress());
            m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
            m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
            D3D12_DISPATCH_RAYS_DESC dispatch{};
            dispatch.RayGenerationShaderRecord.StartAddress = m_reviewProbeRayGenTable.resource->GetGPUVirtualAddress();
            dispatch.RayGenerationShaderRecord.SizeInBytes = m_reviewProbeRayGenTable.recordSize;
            dispatch.MissShaderTable.StartAddress = m_missTable.resource->GetGPUVirtualAddress();
            dispatch.MissShaderTable.SizeInBytes = m_missTable.recordSize * m_missTable.recordCount;
            dispatch.MissShaderTable.StrideInBytes = m_missTable.recordSize;
            dispatch.HitGroupTable.StartAddress = m_hitGroupTable.resource->GetGPUVirtualAddress();
            dispatch.HitGroupTable.SizeInBytes = m_hitGroupTable.recordSize * m_hitGroupTable.recordCount;
            dispatch.HitGroupTable.StrideInBytes = m_hitGroupTable.recordSize;
            dispatch.Width = static_cast<UINT>(normalized.size());
            dispatch.Height = 1u;
            dispatch.Depth = 1u;
            m_commandList->DispatchRays(&dispatch);
            auto probeBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_reviewProbeBuffer.Get());
            m_commandList->ResourceBarrier(1, &probeBarrier);
            auto toReadback = CD3DX12_RESOURCE_BARRIER::Transition(m_reviewProbeBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_commandList->ResourceBarrier(1, &toReadback);
            m_commandList->CopyBufferRegion(readback.Get(), 0, m_reviewProbeBuffer.Get(), 0, sizeof(records));
            auto restoreUav = CD3DX12_RESOURCE_BARRIER::Transition(m_reviewProbeBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_commandList->ResourceBarrier(1, &restoreUav);
            ThrowIfFailed(m_commandList->Close());
            ID3D12CommandList* lists[] = { m_commandList.Get() };
            m_commandQueue->ExecuteCommandLists(1, lists);
            WaitForPreviousFrame();
            void* mapped = nullptr;
            const D3D12_RANGE range{ 0, sizeof(records) };
            ThrowIfFailed(readback->Map(0, &range, &mapped));
            memcpy(records.data(), mapped, sizeof(records));
            readback->Unmap(0, nullptr);
            auto readFloat = [&](size_t record, size_t byteOffset)
            {
                float value = 0.0f;
                const uint32_t bits = records[record * 64u + byteOffset / 4u];
                memcpy(&value, &bits, sizeof(value));
                return value;
            };
            auto appendFloat3 = [&](std::ostringstream& json, size_t record, size_t offset)
            {
                json << '[' << readFloat(record, offset) << ',' << readFloat(record, offset + 4u) << ','
                    << readFloat(record, offset + 8u) << ']';
            };
            std::ostringstream json;
            json << "{\"ok\":true,\"coordinateSpace\":\"" << coordinateSpace << "\",\"sceneFingerprint\":\""
                << cld::EscapeJson(SceneFingerprint()) << "\",\"cameraFingerprint\":\""
                << cld::EscapeJson(CameraFingerprint()) << "\",\"probes\":[";
            for (size_t i = 0; i < normalized.size(); ++i)
            {
                if (i != 0) json << ',';
                const size_t base = i * 64u;
                const bool hit = records[base + 3u] != 0u;
                const uint32_t outputX = std::min(static_cast<uint32_t>(normalized[i].x * m_width), m_width - 1u);
                const uint32_t outputY = std::min(static_cast<uint32_t>(normalized[i].y * m_height), m_height - 1u);
                json << "{\"index\":" << i << ",\"normalized\":[" << normalized[i].x << ',' << normalized[i].y
                    << "],\"outputPixel\":[" << outputX << ',' << outputY << "],\"renderPixel\":["
                    << records[base + 4u] << ',' << records[base + 5u] << "],\"hit\":" << (hit ? "true" : "false");
                if (hit)
                {
                    json << ",\"instanceId\":" << records[base + 8u] << ",\"geometryId\":" << records[base + 9u]
                        << ",\"primitiveId\":" << records[base + 10u] << ",\"materialId\":" << records[base + 11u]
                        << ",\"hitDistance\":" << readFloat(i, 48u) << ",\"depth\":" << readFloat(i, 52u)
                        << ",\"worldPosition\":";
                    appendFloat3(json, i, 56u);
                    json << ",\"geometricNormal\":"; appendFloat3(json, i, 68u);
                    json << ",\"shadingNormal\":"; appendFloat3(json, i, 80u);
                    json << ",\"uv\":[" << readFloat(i, 92u) << ',' << readFloat(i, 96u) << "]"
                        << ",\"baseColor\":"; appendFloat3(json, i, 100u);
                    json << ",\"roughness\":" << readFloat(i, 112u) << ",\"metallic\":" << readFloat(i, 116u)
                        << ",\"emissive\":"; appendFloat3(json, i, 120u);
                    json << ",\"ao\":" << readFloat(i, 132u) << ",\"directRadiance\":"; appendFloat3(json, i, 136u);
                    json << ",\"indirectRadiance\":"; appendFloat3(json, i, 148u);
                    json << ",\"radiance\":"; appendFloat3(json, i, 160u);
                    json << ",\"historyConfidence\":" << readFloat(i, 180u);
                }
                else
                {
                    json << ",\"radiance\":"; appendFloat3(json, i, 160u);
                }
                json << '}';
            }
            json << "]}";
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                m_mcpLatestProbesJson = json.str();
            }
            m_mcpServer.PublishResourceUpdates({ "lookdevpt://probes/latest" });
            return finish(true, "Surface probes completed without changing renderer state.", json.str());
        }

        if (request.actionMethod == "__reset_accumulation")
        {
            ResetAccumulation();
            return finish(true, "Accumulation reset.", "{\"ok\":true,\"diagnostics\":\"Accumulation reset.\",\"stats\":" + BuildMcpStatsJson() + "}");
        }
        if (request.actionMethod == "__reset_denoise_history")
        {
            ResetDenoiseHistory();
            return finish(true, "Denoise history reset.", "{\"ok\":true,\"diagnostics\":\"Denoise history reset.\",\"stats\":" + BuildMcpStatsJson() + "}");
        }
        if (request.actionMethod == "__reset_reservoirs")
        {
            InvalidateHistory(rb::FrameChangeMask::Backend);
            return finish(true, "Reservoirs and rendering history reset.", "{\"ok\":true,\"diagnostics\":\"Reservoirs and rendering history reset.\",\"stats\":" + BuildMcpStatsJson() + "}");
        }
        if (request.actionMethod == "__reset_camera_view")
        {
            ResetCameraView();
            InvalidateHistory(rb::FrameChangeMask::CameraCut);
            m_projectDirty = true;
            return finish(true, "Camera view reset.", "{\"ok\":true,\"diagnostics\":\"Camera view reset.\",\"state\":" + BuildMcpStateJson() + "}");
        }
        if (request.actionMethod == "__set_camera_speed")
        {
            const float baseSpeed = static_cast<float>(cld::JsonNumberOr(request.params, "baseMoveSpeed", m_baseMoveSpeed));
            const float fastSpeed = static_cast<float>(cld::JsonNumberOr(request.params, "fastMoveSpeed", m_fastMoveSpeed));
            if (!AllFinite({ baseSpeed, fastSpeed }))
            {
                return finish(false, "Camera speed contains non-finite values.", "{\"ok\":false,\"diagnostics\":\"Camera speed contains non-finite values.\"}");
            }
            m_baseMoveSpeed = std::clamp(baseSpeed, 0.1f, 100.0f);
            m_fastMoveSpeed = std::clamp(fastSpeed, m_baseMoveSpeed, 200.0f);
            m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"Camera speed accepted.\",\"baseMoveSpeed\":" << m_baseMoveSpeed
                 << ",\"fastMoveSpeed\":" << m_fastMoveSpeed << "}";
            return finish(true, "Camera speed accepted.", json.str());
        }
        if (request.actionMethod == "__fit_camera_to_scene")
        {
            const float padding = std::clamp(static_cast<float>(cld::JsonNumberOr(request.params, "padding", 1.2)), 1.0f, 4.0f);
            const bool preserveOrientation = cld::JsonBoolOr(request.params, "preserveOrientation", true);
            const float yaw = static_cast<float>(cld::JsonNumberOr(request.params, "yaw", preserveOrientation ? m_camera.GetYawRadians() : m_defaultCameraYaw));
            const float pitch = std::clamp(static_cast<float>(cld::JsonNumberOr(request.params, "pitch", preserveOrientation ? m_camera.GetPitchRadians() : m_defaultCameraPitch)), XMConvertToRadians(-83.0f), XMConvertToRadians(83.0f));
            if (!AllFinite({ padding, yaw, pitch }))
            {
                return finish(false, "Fit camera settings contain non-finite values.", "{\"ok\":false,\"diagnostics\":\"Fit camera settings contain non-finite values.\"}");
            }
            const XMFLOAT3 center(
                (m_scene.boundsMin.x + m_scene.boundsMax.x) * 0.5f,
                (m_scene.boundsMin.y + m_scene.boundsMax.y) * 0.5f,
                (m_scene.boundsMin.z + m_scene.boundsMax.z) * 0.5f);
            const XMVECTOR extents = XMVectorSet(
                m_scene.boundsMax.x - m_scene.boundsMin.x,
                m_scene.boundsMax.y - m_scene.boundsMin.y,
                m_scene.boundsMax.z - m_scene.boundsMin.z,
                0.0f);
            const float radius = (std::max)(1.0f, XMVectorGetX(XMVector3Length(extents)) * 0.5f);
            const float distance = radius / tanf(XMConvertToRadians(30.0f)) * padding;
            const XMVECTOR forward = XMVector3Normalize(XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch), 0.0f));
            XMFLOAT3 position;
            XMStoreFloat3(&position, XMLoadFloat3(&center) - forward * distance);
            m_camera.Reset(position, yaw, pitch, preserveOrientation ? m_camera.GetRollRadians() : 0.0f);
            ResetRenderingHistory();
            m_projectDirty = true;
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"Camera fit to scene.\",\"camera\":{\"position\":";
            AppendJsonFloat3(json, position);
            json << ",\"yaw\":" << yaw << ",\"pitch\":" << pitch << "},\"padding\":" << padding << "}";
            return finish(true, "Camera fit to scene.", json.str());
        }
        if (request.actionMethod == "__set_display_resolution")
        {
            int preset = -1;
            const std::string presetName = cld::JsonStringOr(request.params, "preset");
            if (presetName == "720p")
            {
                preset = 0;
            }
            else if (presetName.empty() || presetName == "1080p")
            {
                preset = 1;
            }
            else if (presetName == "4k" || presetName == "4K")
            {
                preset = 2;
            }

            UINT width = 0;
            UINT height = 0;
            if (const cld::JsonValue* widthValue = cld::FindMember(request.params, "width"); widthValue && widthValue->type == cld::JsonValue::Type::Number)
            {
                width = static_cast<UINT>(std::clamp(static_cast<int>(widthValue->number), 320, 7680));
            }
            if (const cld::JsonValue* heightValue = cld::FindMember(request.params, "height"); heightValue && heightValue->type == cld::JsonValue::Type::Number)
            {
                height = static_cast<UINT>(std::clamp(static_cast<int>(heightValue->number), 240, 4320));
            }

            if (width > 0 && height > 0)
            {
                m_pendingResizeWidth = width;
                m_pendingResizeHeight = height;
                m_resizePending = true;
                if (width == 1280u && height == 720u) m_displayResolutionPreset = 0;
                else if (width == 1920u && height == 1080u) m_displayResolutionPreset = 1;
                else if (width == 3840u && height == 2160u) m_displayResolutionPreset = 2;
            }
            else if (preset >= 0)
            {
                m_displayResolutionPreset = preset;
                constexpr UINT widths[] = { 1280u, 1920u, 3840u };
                constexpr UINT heights[] = { 720u, 1080u, 2160u };
                m_pendingResizeWidth = widths[preset];
                m_pendingResizeHeight = heights[preset];
                m_resizePending = true;
            }
            else
            {
                return finish(false, "Display resolution requires preset or width/height.", "{\"ok\":false,\"diagnostics\":\"Display resolution requires preset or width/height.\"}");
            }
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"Display resolution requested.\",\"target\":{\"preset\":" << m_displayResolutionPreset
                 << ",\"width\":" << (width > 0 ? width : (m_displayResolutionPreset == 0 ? 1280u : m_displayResolutionPreset == 2 ? 3840u : 1920u))
                 << ",\"height\":" << (height > 0 ? height : (m_displayResolutionPreset == 0 ? 720u : m_displayResolutionPreset == 2 ? 2160u : 1080u)) << "}}";
            return finish(true, "Display resolution requested.", json.str());
        }
        if (request.actionMethod == "__load_project")
        {
            const std::string pathUtf8 = cld::JsonStringOr(request.params, "path");
            if (pathUtf8.empty())
            {
                return finish(false, "load_project requires path.", "{\"ok\":false,\"diagnostics\":\"load_project requires path.\"}");
            }
            const std::wstring path = Utf8ToWide(pathUtf8);
            if (!std::filesystem::exists(path))
            {
                return finish(false, "Project path does not exist.", "{\"ok\":false,\"diagnostics\":\"Project path does not exist.\"}");
            }
            std::string diagnostics;
            const bool ok = LoadProjectFromDisk(path, diagnostics);
            return finish(ok, diagnostics, "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"diagnostics\":\"" + cld::EscapeJson(diagnostics) + "\",\"projectPath\":\"" + cld::EscapeJson(pathUtf8) + "\"}");
        }
        if (request.actionMethod == "__save_project")
        {
            if (m_projectPath.empty())
            {
                return finish(false, "Current project has no path. Use save_project_as.", "{\"ok\":false,\"diagnostics\":\"Current project has no path. Use save_project_as.\"}");
            }
            const bool ok = SaveProjectToDisk(m_projectPath);
            const std::string diagnostics = ok ? "Project saved." : "Project save failed.";
            return finish(ok, diagnostics, "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"diagnostics\":\"" + diagnostics + "\",\"projectPath\":\"" + cld::EscapeJson(WideToUtf8(m_projectPath)) + "\"}");
        }
        if (request.actionMethod == "__save_project_as")
        {
            const std::string pathUtf8 = cld::JsonStringOr(request.params, "path");
            if (pathUtf8.empty())
            {
                return finish(false, "save_project_as requires path.", "{\"ok\":false,\"diagnostics\":\"save_project_as requires path.\"}");
            }
            const std::filesystem::path path(Utf8ToWide(pathUtf8));
            if (!path.parent_path().empty() && !std::filesystem::exists(path.parent_path()))
            {
                return finish(false, "Project directory does not exist.", "{\"ok\":false,\"diagnostics\":\"Project directory does not exist.\"}");
            }
            const bool ok = SaveProjectToDisk(path.wstring());
            const std::string diagnostics = ok ? "Project saved." : "Project save failed.";
            return finish(ok, diagnostics, "{\"ok\":" + std::string(ok ? "true" : "false") + ",\"diagnostics\":\"" + diagnostics + "\",\"projectPath\":\"" + cld::EscapeJson(pathUtf8) + "\"}");
        }
        if (request.actionMethod == "__start_benchmark")
        {
            if (m_activeMcpBenchmarkId != 0 || (m_benchmarkHarness && !m_benchmarkFinished))
                return finish(false, "A benchmark is already running.",
                    "{\"ok\":false,\"code\":\"benchmark_busy\"}");
            const std::string cameraPathText = cld::JsonStringOr(request.params, "cameraPath");
            const std::filesystem::path cameraPath = Utf8ToWide(cameraPathText);
            if (cameraPath.empty() || !std::filesystem::is_regular_file(cameraPath))
                return finish(false, "start_benchmark requires an existing cameraPath.",
                    "{\"ok\":false,\"code\":\"camera_path_not_found\"}");
            const std::string kind = cld::JsonStringOr(request.params, "kind", "combined");
            lookdevpt::benchmark::BenchmarkKind benchmarkKind = lookdevpt::benchmark::BenchmarkKind::Combined;
            if (kind == "performance") benchmarkKind = lookdevpt::benchmark::BenchmarkKind::Performance;
            else if (kind == "quality") benchmarkKind = lookdevpt::benchmark::BenchmarkKind::Quality;
            else if (kind != "combined")
                return finish(false, "Benchmark kind must be combined, performance, or quality.",
                    "{\"ok\":false,\"code\":\"invalid_benchmark_kind\"}");

            uint64_t id = m_nextMcpBenchmarkId++;
            std::filesystem::path outputDirectory;
            do
            {
                outputDirectory = McpBenchmarkDirectory() / (L"benchmark-" + std::to_wstring(id));
                if (std::filesystem::exists(outputDirectory)) id = m_nextMcpBenchmarkId++;
                else break;
            } while (true);
            std::error_code error;
            std::filesystem::create_directories(McpCheckpointDirectory(), error);
            if (error)
                return finish(false, "Benchmark checkpoint directory could not be created.",
                    "{\"ok\":false,\"code\":\"benchmark_storage_failed\"}");

            McpBenchmark benchmark;
            benchmark.id = id;
            benchmark.kind = kind;
            benchmark.outputDirectory = outputDirectory;
            benchmark.checkpointPath = McpCheckpointDirectory() /
                (L"benchmark-" + std::to_wstring(id) + L".lookdevpt.json");
            benchmark.projectPath = m_projectPath;
            benchmark.projectDiagnostics = m_projectDiagnostics;
            benchmark.projectDirty = m_projectDirty;
            benchmark.vsyncEnabled = m_vsyncEnabled;
            benchmark.samplingSeed = m_samplingSeed;

            const bool checkpointSaved = SaveProjectToDisk(benchmark.checkpointPath.wstring());
            m_projectPath = benchmark.projectPath;
            m_projectDiagnostics = benchmark.projectDiagnostics;
            m_projectDirty = benchmark.projectDirty;
            if (!checkpointSaved)
                return finish(false, "Benchmark checkpoint could not be written.",
                    "{\"ok\":false,\"code\":\"benchmark_storage_failed\"}");

            lookdevpt::benchmark::Options options;
            options.enabled = true;
            options.benchmarkKind = benchmarkKind;
            options.cameraPath = std::filesystem::absolute(cameraPath);
            options.frames = static_cast<uint32_t>(std::clamp(
                static_cast<int>(cld::JsonNumberOr(request.params, "frames", 120.0)), 1, 10000));
            options.warmup = static_cast<uint32_t>(std::clamp(
                static_cast<int>(cld::JsonNumberOr(request.params, "warmup", 30.0)), 0, 2000));
            options.seed = static_cast<uint64_t>((std::max)(cld::JsonNumberOr(request.params, "seed", 1.0), 0.0));
            options.outputDirectory = outputDirectory;
            options.captureEvery = static_cast<uint32_t>(std::clamp(
                static_cast<int>(cld::JsonNumberOr(request.params, "captureEvery", 0.0)), 0, 10000));
            options.captureAovs = cld::JsonBoolOr(request.params, "captureAovs", false);
            auto harness = std::make_unique<lookdevpt::benchmark::Harness>(options);
            std::string diagnostics;
            if (!harness->Initialize(diagnostics))
            {
                std::filesystem::remove(benchmark.checkpointPath, error);
                return finish(false, diagnostics,
                    "{\"ok\":false,\"code\":\"benchmark_initialization_failed\",\"diagnostics\":\"" +
                    cld::EscapeJson(diagnostics) + "\"}");
            }
            benchmark.totalFrames = harness->TotalFrames();
            benchmark.state = "running";
            benchmark.diagnostics = "Benchmark running.";
            m_benchmarkOptions = options;
            m_benchmarkHarness = std::move(harness);
            m_benchmarkFrameIndex = 0;
            m_benchmarkRecordedFrameCount = 0;
            m_completedBenchmarkMetrics.clear();
            m_benchmarkFinished = false;
            m_samplingSeed = static_cast<uint32_t>(options.seed) ^ static_cast<uint32_t>(options.seed >> 32u);
            m_vsyncEnabled = false;
            ResetRenderingHistory();
            try
            {
                CreateGpuResourcesForCurrentScene();
            }
            catch (const std::exception& exception)
            {
                m_benchmarkHarness.reset();
                m_benchmarkOptions = {};
                m_benchmarkFinished = true;
                m_vsyncEnabled = benchmark.vsyncEnabled;
                m_samplingSeed = benchmark.samplingSeed;
                std::filesystem::remove(benchmark.checkpointPath, error);
                return finish(false, exception.what(),
                    "{\"ok\":false,\"code\":\"benchmark_resource_initialization_failed\",\"diagnostics\":\"" +
                    cld::EscapeJson(exception.what()) + "\"}");
            }
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                m_mcpBenchmarks.push_back(benchmark);
                m_activeMcpBenchmarkId = id;
                while (m_mcpBenchmarks.size() > 16u && m_mcpBenchmarks.front().id != id)
                    m_mcpBenchmarks.pop_front();
            }
            const std::string idText = std::to_string(id);
            m_mcpServer.PublishResourceUpdates({ "lookdevpt://benchmarks/index", "lookdevpt://benchmarks/" + idText });
            return finish(true, "Benchmark started.",
                "{\"ok\":true,\"benchmarkId\":\"" + idText + "\",\"state\":\"running\",\"resource\":\"lookdevpt://benchmarks/" +
                idText + "\",\"totalFrames\":" + std::to_string(benchmark.totalFrames) + "}");
        }
        if (request.actionMethod == "__cancel_benchmark")
        {
            uint64_t id = 0;
            if (const cld::JsonValue* value = cld::FindMember(request.params, "benchmarkId"))
            {
                if (value->type == cld::JsonValue::Type::Number) id = static_cast<uint64_t>((std::max)(value->number, 0.0));
                else if (value->type == cld::JsonValue::Type::String) id = std::strtoull(value->string.c_str(), nullptr, 10);
            }
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                const auto iterator = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
                    [&](const McpBenchmark& benchmark) { return benchmark.id == id; });
                if (iterator != m_mcpBenchmarks.end() && iterator->state == "running")
                {
                    iterator->cancelRequested = true;
                    iterator->diagnostics = "Benchmark cancellation requested.";
                    found = true;
                }
            }
            return finish(found, found ? "Benchmark cancellation requested." : "Running benchmark id was not found.",
                found ? "{\"ok\":true,\"benchmarkId\":\"" + std::to_string(id) + "\",\"state\":\"cancelling\"}"
                    : "{\"ok\":false,\"code\":\"benchmark_not_found\"}");
        }
        if (request.actionMethod == "__create_checkpoint")
        {
            const uint64_t id = m_nextMcpCheckpointId++;
            std::error_code error;
            const std::filesystem::path directory = McpCheckpointDirectory();
            std::filesystem::create_directories(directory, error);
            if (error)
            {
                return finish(false, "Checkpoint directory could not be created.",
                    "{\"ok\":false,\"code\":\"checkpoint_storage_failed\"}");
            }
            const std::filesystem::path snapshotPath = directory /
                (L"checkpoint-" + std::to_wstring(id) + L".lookdevpt.json");
            McpCheckpoint checkpoint;
            checkpoint.id = id;
            checkpoint.label = cld::JsonStringOr(request.params, "label", "Checkpoint").substr(0, 128);
            checkpoint.sceneFingerprint = SceneFingerprint();
            checkpoint.cameraFingerprint = CameraFingerprint();
            checkpoint.snapshotPath = snapshotPath;
            checkpoint.projectPath = m_projectPath;
            checkpoint.projectDiagnostics = m_projectDiagnostics;
            checkpoint.projectDirty = m_projectDirty;

            const bool saved = SaveProjectToDisk(snapshotPath.wstring());
            m_projectPath = checkpoint.projectPath;
            m_projectDiagnostics = checkpoint.projectDiagnostics;
            m_projectDirty = checkpoint.projectDirty;
            if (!saved)
            {
                std::filesystem::remove(snapshotPath, error);
                return finish(false, "Checkpoint snapshot could not be written.",
                    "{\"ok\":false,\"code\":\"checkpoint_storage_failed\"}");
            }
            std::filesystem::path evictedPath;
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                m_mcpCheckpoints.push_back(checkpoint);
                if (m_mcpCheckpoints.size() > 16u)
                {
                    evictedPath = m_mcpCheckpoints.front().snapshotPath;
                    m_mcpCheckpoints.pop_front();
                }
            }
            if (!evictedPath.empty()) std::filesystem::remove(evictedPath, error);
            const std::string idText = std::to_string(id);
            m_mcpServer.PublishResourceUpdates({ "lookdevpt://checkpoints/index", "lookdevpt://checkpoints/" + idText });
            return finish(true, "Checkpoint created.",
                "{\"ok\":true,\"checkpointId\":\"" + idText + "\",\"label\":\"" +
                cld::EscapeJson(checkpoint.label) + "\",\"sceneFingerprint\":\"" +
                cld::EscapeJson(checkpoint.sceneFingerprint) + "\",\"resource\":\"lookdevpt://checkpoints/" +
                idText + "\"}");
        }
        if (request.actionMethod == "__restore_checkpoint" || request.actionMethod == "__delete_checkpoint")
        {
            uint64_t id = 0;
            if (const cld::JsonValue* value = cld::FindMember(request.params, "checkpointId"))
            {
                if (value->type == cld::JsonValue::Type::Number && value->number > 0.0)
                    id = static_cast<uint64_t>(value->number);
                else if (value->type == cld::JsonValue::Type::String)
                    id = std::strtoull(value->string.c_str(), nullptr, 10);
            }
            McpCheckpoint checkpoint;
            bool foundCheckpoint = false;
            {
                std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                const auto iterator = std::find_if(m_mcpCheckpoints.begin(), m_mcpCheckpoints.end(),
                    [&](const McpCheckpoint& candidate) { return candidate.id == id; });
                if (iterator != m_mcpCheckpoints.end())
                {
                    checkpoint = *iterator;
                    foundCheckpoint = true;
                    if (request.actionMethod == "__delete_checkpoint") m_mcpCheckpoints.erase(iterator);
                }
            }
            if (!foundCheckpoint)
                return finish(false, "Checkpoint id was not found.",
                    "{\"ok\":false,\"code\":\"checkpoint_not_found\"}");
            if (request.actionMethod == "__delete_checkpoint")
            {
                std::error_code error;
                std::filesystem::remove(checkpoint.snapshotPath, error);
                m_mcpServer.PublishResourceUpdates({ "lookdevpt://checkpoints/index", "lookdevpt://checkpoints/" + std::to_string(id) });
                return finish(true, "Checkpoint deleted.",
                    "{\"ok\":true,\"checkpointId\":\"" + std::to_string(id) + "\"}");
            }
            if (SceneFingerprint() != checkpoint.sceneFingerprint)
            {
                return finish(false, "Checkpoint belongs to a different scene fingerprint.",
                    "{\"ok\":false,\"code\":\"scene_fingerprint_mismatch\",\"checkpointId\":\"" +
                    std::to_string(id) + "\"}");
            }
            std::string diagnostics;
            const bool restored = LoadProjectFromDisk(checkpoint.snapshotPath.wstring(), diagnostics);
            if (restored)
            {
                m_projectPath = checkpoint.projectPath;
                m_projectDiagnostics = checkpoint.projectDiagnostics;
                m_projectDirty = checkpoint.projectDirty;
                diagnostics = "Checkpoint restored.";
            }
            return finish(restored, diagnostics,
                "{\"ok\":" + std::string(restored ? "true" : "false") +
                ",\"checkpointId\":\"" + std::to_string(id) + "\",\"diagnostics\":\"" +
                cld::EscapeJson(diagnostics) + "\"}");
        }
        if (request.actionMethod == "__run_actions")
        {
            const cld::JsonValue* actions = cld::FindMember(request.params, "actions");
            const bool validateOnly = cld::JsonBoolOr(request.params, "validateOnly", false);
            const bool stopOnError = cld::JsonBoolOr(request.params, "stopOnError", true);
            if (!actions || actions->type != cld::JsonValue::Type::Array || actions->array.empty() || actions->array.size() > 16)
            {
                return finish(false, "run_actions requires 1 to 16 actions.", "{\"ok\":false,\"diagnostics\":\"run_actions requires 1 to 16 actions.\"}");
            }

            std::vector<std::string> methods;
            std::vector<const cld::JsonValue*> params;
            std::vector<std::string> diagnostics;
            methods.reserve(actions->array.size());
            params.reserve(actions->array.size());
            diagnostics.reserve(actions->array.size());

            for (size_t i = 0; i < actions->array.size(); ++i)
            {
                const cld::JsonValue& action = actions->array[i];
                if (action.type != cld::JsonValue::Type::Object)
                {
                    return finish(false, "run_actions action must be an object.", "{\"ok\":false,\"diagnostics\":\"run_actions action must be an object.\",\"failedIndex\":" + std::to_string(i) + "}");
                }
                const std::string method = cld::JsonStringOr(action, "method");
                const cld::JsonValue* actionParams = cld::FindMember(action, "params");
                if (method.empty() || !actionParams || actionParams->type != cld::JsonValue::Type::Object)
                {
                    return finish(false, "run_actions action requires method and object params.", "{\"ok\":false,\"diagnostics\":\"run_actions action requires method and object params.\",\"failedIndex\":" + std::to_string(i) + "}");
                }
                std::string validationDiagnostics;
                if (!ApplyAction(method, *actionParams, validationDiagnostics, true))
                {
                    std::ostringstream json;
                    json << "{\"ok\":false,\"diagnostics\":\"Validation failed.\",\"failedIndex\":" << i
                         << ",\"method\":\"" << cld::EscapeJson(method) << "\",\"actionDiagnostics\":\"" << cld::EscapeJson(validationDiagnostics)
                         << "\",\"appliedCount\":0}";
                    return finish(false, "Validation failed.", json.str());
                }
                methods.push_back(method);
                params.push_back(actionParams);
                diagnostics.push_back(validationDiagnostics);
            }

            if (validateOnly)
            {
                std::ostringstream json;
                json << "{\"ok\":true,\"diagnostics\":\"All actions validated.\",\"validateOnly\":true,\"actions\":[";
                for (size_t i = 0; i < methods.size(); ++i)
                {
                    if (i > 0) json << ",";
                    json << "{\"method\":\"" << cld::EscapeJson(methods[i]) << "\",\"diagnostics\":\"" << cld::EscapeJson(diagnostics[i]) << "\"}";
                }
                json << "]}";
                return finish(true, "All actions validated.", json.str());
            }

            size_t appliedCount = 0;
            bool hadExecutionFailure = false;
            size_t firstFailedIndex = 0;
            std::string firstFailureMethod;
            std::string firstFailureDiagnostics;
            for (size_t i = 0; i < methods.size(); ++i)
            {
                std::string actionDiagnostics;
                if (!ApplyAction(methods[i], *params[i], actionDiagnostics, false))
                {
                    if (!hadExecutionFailure)
                    {
                        hadExecutionFailure = true;
                        firstFailedIndex = i;
                        firstFailureMethod = methods[i];
                        firstFailureDiagnostics = actionDiagnostics;
                    }
                    std::ostringstream json;
                    json << "{\"ok\":false,\"diagnostics\":\"Action execution failed.\",\"failedIndex\":" << i
                         << ",\"method\":\"" << cld::EscapeJson(methods[i]) << "\",\"actionDiagnostics\":\"" << cld::EscapeJson(actionDiagnostics)
                         << "\",\"appliedCount\":" << appliedCount << "}";
                    if (stopOnError)
                    {
                        return finish(false, "Action execution failed.", json.str());
                    }
                }
                else
                {
                    ++appliedCount;
                }
            }
            if (hadExecutionFailure)
            {
                std::ostringstream json;
                json << "{\"ok\":false,\"diagnostics\":\"Action execution failed.\",\"failedIndex\":" << firstFailedIndex
                     << ",\"method\":\"" << cld::EscapeJson(firstFailureMethod) << "\",\"actionDiagnostics\":\"" << cld::EscapeJson(firstFailureDiagnostics)
                     << "\",\"appliedCount\":" << appliedCount << "}";
                return finish(false, "Action execution failed.", json.str());
            }
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"Actions applied.\",\"validateOnly\":false,\"appliedCount\":" << appliedCount
                 << ",\"stats\":" << BuildMcpStatsJson() << "}";
            return finish(true, "Actions applied.", json.str());
        }
        if (request.actionMethod == "__capture_debug_pack")
        {
            std::vector<int> views;
            if (const cld::JsonValue* viewArray = cld::FindMember(request.params, "views"); viewArray && viewArray->type == cld::JsonValue::Type::Array)
            {
                if (viewArray->array.size() > 8)
                {
                    return finish(false, "capture_debug_pack supports at most 8 views.", "{\"ok\":false,\"diagnostics\":\"capture_debug_pack supports at most 8 views.\"}");
                }
                for (const cld::JsonValue& viewValue : viewArray->array)
                {
                    int debugView = 0;
                    if (!TryParseDebugView(viewValue, debugView))
                    {
                        return finish(false, "capture_debug_pack contains an unknown debug view.", "{\"ok\":false,\"diagnostics\":\"capture_debug_pack contains an unknown debug view.\"}");
                    }
                    views.push_back(debugView);
                }
            }
            if (views.empty())
            {
                views = { 0, 1, 2, 4, 5, 32, 33, 40 };
            }
            const bool restoreView = cld::JsonBoolOr(request.params, "restoreView", true);
            const int originalView = m_debugViewMode;
            std::vector<uint64_t> ids;
            std::string diagnostics = "Debug pack captured.";
            for (int view : views)
            {
                m_debugViewMode = view;
                InvalidateHistory(rb::FrameChangeMask::DenoiserSettings);
                std::string renderDiagnostics;
                if (!RenderPathTracingOutputForCapture(renderDiagnostics))
                {
                    if (restoreView)
                    {
                        m_debugViewMode = originalView;
                        InvalidateHistory(rb::FrameChangeMask::DenoiserSettings);
                    }
                    return finish(false, renderDiagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(renderDiagnostics) + "\"}");
                }
                std::string base64Png;
                std::string captureDiagnostics;
                if (!CaptureViewportPng(base64Png, captureDiagnostics))
                {
                    if (restoreView)
                    {
                        m_debugViewMode = originalView;
                        InvalidateHistory(rb::FrameChangeMask::DenoiserSettings);
                    }
                    return finish(false, captureDiagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(captureDiagnostics) + "\"}");
                }
                const uint64_t id = StoreMcpCapture(base64Png, view, DebugViewLabels[view]);
                {
                    std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
                    m_mcpLatestCaptureBase64 = std::move(base64Png);
                    m_mcpLastCaptureDiagnostics = captureDiagnostics;
                }
                ids.push_back(id);
            }
            if (!ids.empty())
            {
                m_mcpServer.PublishResourceUpdates({ "lookdevpt://captures/latest.png" });
            }
            if (restoreView)
            {
                m_debugViewMode = originalView;
                InvalidateHistory(rb::FrameChangeMask::DenoiserSettings);
            }
            std::ostringstream json;
            json << "{\"ok\":true,\"diagnostics\":\"" << diagnostics << "\",\"captures\":[";
            std::ostringstream content;
            content << "[{\"type\":\"text\",\"text\":\"" << cld::EscapeJson(diagnostics) << "\"}";
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (i > 0) json << ",";
                const int view = views[i];
                json << "{\"id\":" << ids[i] << ",\"debugView\":" << view << ",\"label\":\"" << DebugViewLabels[view]
                     << "\",\"resource\":\"lookdevpt://captures/" << ids[i] << ".png\"}";
                content << ",{\"type\":\"resource_link\",\"uri\":\"lookdevpt://captures/" << ids[i]
                    << ".png\",\"name\":\"" << cld::EscapeJson(KeyFromLabel(DebugViewLabels[view]))
                    << "\",\"mimeType\":\"image/png\"}";
            }
            json << "],\"restoredDebugView\":" << m_debugViewMode << "}";
            content << "]";
            mcp::CommandResult packResult = finish(true, diagnostics, json.str());
            packResult.contentJson = content.str();
            return packResult;
        }

        std::string diagnostics;
        const bool ok = ApplyAction(request.actionMethod, request.params, diagnostics, request.validateOnly);
        std::string structured = "{\"ok\":" + std::string(ok ? "true" : "false") +
            ",\"action\":\"" + cld::EscapeJson(request.actionMethod) +
            "\",\"validateOnly\":" + std::string(request.validateOnly ? "true" : "false") +
            ",\"diagnostics\":\"" + cld::EscapeJson(diagnostics) +
            "\",\"stats\":" + BuildMcpStatsJson() + "}";
        return finish(ok, diagnostics, structured);
    }
    catch (const std::exception& ex)
    {
        return finish(false, ex.what(), "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(ex.what()) + "\"}");
    }
}

void D3D12PathTracingBackend::ProcessMcpCommands()
{
    m_mcpDispatcher.ProcessOne([this](const mcp::CommandRequest& request)
    {
        return ExecuteMcpCommand(request);
    });
}

void D3D12PathTracingBackend::UpdateMcpSnapshots()
{
    // Snapshot construction is intentionally disabled with the server off.
    // In a Bistro scene the material resource alone is hundreds of KiB and
    // used to perform hundreds of filesystem probes every rendered frame.
    if (!m_mcpServer.IsRunning())
    {
        return;
    }

    RefreshMcpAuditCache();

    const auto now = std::chrono::steady_clock::now();
    const bool force = !m_mcpStaticSnapshotsValid;
    const bool updateState = force || now >= m_mcpNextStateSnapshot;
    const bool updateStats = force || now >= m_mcpNextStatsSnapshot;
    const rb::FrameRevisions& revisions = m_frameState.revisions;
    const bool revisionChanged = force ||
        revisions.scene != m_mcpSnapshotRevisions.scene ||
        revisions.geometry != m_mcpSnapshotRevisions.geometry ||
        revisions.material != m_mcpSnapshotRevisions.material ||
        revisions.light != m_mcpSnapshotRevisions.light ||
        revisions.hdri != m_mcpSnapshotRevisions.hdri ||
        revisions.backend != m_mcpSnapshotRevisions.backend ||
        revisions.qualityProfile != m_mcpSnapshotRevisions.qualityProfile ||
        m_projectDirty != m_mcpSnapshotProjectDirty ||
        m_mcpMaterialCatalogRevision != m_mcpSnapshotMaterialCatalogRevision;

    std::string stateJson;
    std::string statsJson;
    std::string materialsJson;
    std::string diagnosticsJson;
    std::string projectJson;
    std::string sceneSummaryJson;
    std::string variantsJson;
    std::string presetsJson;
    if (updateState)
    {
        stateJson = BuildMcpStateJson();
    }
    if (updateStats)
    {
        statsJson = BuildMcpStatsJson();
        diagnosticsJson = BuildMcpDiagnosticsJson();
    }
    if (revisionChanged)
    {
        materialsJson = BuildMcpMaterialsJson();
        projectJson = BuildMcpProjectJson();
        sceneSummaryJson = BuildMcpSceneSummaryJson();
        variantsJson = BuildMaterialVariantsJson();
        presetsJson = BuildMaterialPresetsJson();
    }

    std::vector<std::string> updatedResources;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        if (updateState)
        {
            if (stateJson != m_mcpStateJson) updatedResources.push_back("lookdevpt://state");
            m_mcpStateJson = std::move(stateJson);
        }
        if (updateStats)
        {
            if (statsJson != m_mcpStatsJson) updatedResources.push_back("lookdevpt://stats");
            if (diagnosticsJson != m_mcpDiagnosticsJson) updatedResources.push_back("lookdevpt://diagnostics");
            m_mcpStatsJson = std::move(statsJson);
            m_mcpDiagnosticsJson = std::move(diagnosticsJson);
        }
        if (revisionChanged)
        {
            if (materialsJson != m_mcpMaterialsJson) updatedResources.push_back("lookdevpt://materials");
            if (projectJson != m_mcpProjectJson) updatedResources.push_back("lookdevpt://project");
            if (sceneSummaryJson != m_mcpSceneSummaryJson) updatedResources.push_back("lookdevpt://scene/summary");
            if (variantsJson != m_mcpMaterialVariantsJson) updatedResources.push_back("lookdevpt://material-variants");
            if (presetsJson != m_mcpMaterialPresetsJson) updatedResources.push_back("lookdevpt://material-presets");
            m_mcpMaterialsJson = std::move(materialsJson);
            m_mcpProjectJson = std::move(projectJson);
            m_mcpSceneSummaryJson = std::move(sceneSummaryJson);
            m_mcpMaterialVariantsJson = std::move(variantsJson);
            m_mcpMaterialPresetsJson = std::move(presetsJson);
            m_mcpSnapshotRevisions = revisions;
            m_mcpSnapshotProjectDirty = m_projectDirty;
            m_mcpSnapshotMaterialCatalogRevision = m_mcpMaterialCatalogRevision;
            m_mcpStaticSnapshotsValid = true;
        }
    }
    m_mcpServer.PublishResourceUpdates(updatedResources);
    if (updateState) m_mcpNextStateSnapshot = now + std::chrono::milliseconds(33);
    if (updateStats) m_mcpNextStatsSnapshot = now + std::chrono::milliseconds(100);
}

std::string D3D12PathTracingBackend::BuildMcpStateJson() const
{
    XMFLOAT3 camera = m_camera.GetPosition();
    std::ostringstream out;
    out << "{";
    out << "\"scenePath\":\"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",";
    out << "\"environmentPath\":\"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\",";
    out << "\"projectPath\":\"" << cld::EscapeJson(WideToUtf8(m_projectPath)) << "\",";
    out << "\"projectDirty\":" << (m_projectDirty ? "true" : "false") << ",";
    out << "\"quality\":" << rb::QualitySettingsToJson(m_qualitySettings) << ",";
    out << "\"resolution\":{\"mode\":\""
        << rb::ResolutionModeName(m_qualitySettings.resolutionMode)
        << "\",\"outputWidth\":" << m_width
        << ",\"outputHeight\":" << m_height
        << ",\"renderWidth\":" << m_renderWidth
        << ",\"renderHeight\":" << m_renderHeight
        << ",\"activeScale\":" << m_activeRenderScale
        << ",\"taauActive\":" << (UsesTemporalUpscale() ? "true" : "false")
        << "},";
    const bool finalTaaActive = (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill && m_finalTaaPipeline;
    out << "\"finalTaaActive\":" << (finalTaaActive ? "true" : "false") << ",";
    out << "\"camera\":{\"position\":";
    AppendJsonFloat3(out, camera);
    out << ",\"yaw\":" << m_camera.GetYawRadians() << ",\"pitch\":" << m_camera.GetPitchRadians()
        << ",\"roll\":" << m_camera.GetRollRadians() << ",\"fovDegrees\":" << m_cameraFovDegrees
        << ",\"baseMoveSpeed\":" << m_baseMoveSpeed << ",\"fastMoveSpeed\":" << m_fastMoveSpeed << "},";
    out << "\"lighting\":{\"direction\":";
    AppendJsonFloat3(out, m_lightDirection);
    out << ",\"color\":";
    AppendJsonFloat3(out, m_lightColor);
    out << ",\"intensity\":" << m_lightIntensity << ",\"rayTMin\":" << m_rayTMin
        << ",\"skyEnabled\":" << (m_skyEnabled ? "true" : "false") << ",\"skyIntensity\":" << m_skyIntensity
        << ",\"sunIntensity\":" << m_sunIntensity << ",\"sunSize\":" << m_sunAngularRadius
        << ",\"environmentEnabled\":" << (m_environmentMapEnabled ? "true" : "false")
        << ",\"environmentIntensity\":" << m_environmentIntensity << ",\"environmentRotation\":" << m_environmentRotation
        << ",\"emissiveTriangleLights\":" << (m_emissiveLightsEnabled ? "true" : "false")
        << ",\"proceduralAreaLights\":" << (m_proceduralLightsEnabled ? "true" : "false") << "},";
    out << "\"pathTracing\":{\"mode\":\"" << PathtracingModeName(m_mode) << "\",\"samplesPerFrame\":" << m_giSamplesPerFrame
        << ",\"maxBounces\":" << m_maxPathBounces << ",\"minBounces\":" << m_minPathBounces
        << ",\"radianceClamp\":" << m_giRadianceClamp << ",\"maxAccumSamples\":" << m_maxAccumulatedFrames
        << ",\"freezeAccumulation\":" << (m_freezeAccumulation ? "true" : "false")
        << ",\"adaptiveSampling\":" << (m_adaptiveSamplingEnabled ? "true" : "false")
        << ",\"requestedSecondaryShadingRate\":\""
        << rb::SecondaryShadingRateName(m_qualitySettings.secondaryShadingRate)
        << "\",\"activeSecondaryRate\":" << m_activeSecondaryShadingRate
        << ",\"autoSecondaryHalfActive\":" << (m_autoSecondaryHalfActive ? "true" : "false") << "},";
    const RtxdiStatus& rtxdiStatus = m_rtxdiBackendRuntime.Status();
    const bool realtimeRtxdiRequested =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        m_rtxdiAvailable &&
        m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi;
    const bool rtxdiDiActive =
        realtimeRtxdiRequested && rtxdiStatus.diEvaluationReady && UsesRestirDI(m_mode);
    const bool rtxdiGiActive =
        realtimeRtxdiRequested && rtxdiStatus.giEvaluationReady && UsesRestirGI(m_mode);
    const bool rtxdiPtActive = rtxdiStatus.ptEvaluationReady &&
        m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi && UsesRestirPT(m_mode);
    const RtxdiReservoirLayout diReservoirLayout =
        m_rtxdiBackendRuntime.CalculateReservoirLayout(m_renderWidth, m_renderHeight);
    const RtxdiReservoirLayout giReservoirLayout =
        m_rtxdiBackendRuntime.CalculateGiReservoirLayout(m_renderWidth, m_renderHeight);
    const RtxdiReservoirLayout ptReservoirLayout =
        m_rtxdiBackendRuntime.CalculatePtReservoirLayout(m_renderWidth, m_renderHeight);
    const std::uint64_t diReservoirBytes = rtxdiDiActive
        ? static_cast<std::uint64_t>(diReservoirLayout.arrayPitch) * diReservoirLayout.elementStride * 2ull
        : 0ull;
    const std::uint64_t giReservoirBytes = rtxdiGiActive
        ? static_cast<std::uint64_t>(giReservoirLayout.arrayPitch) * giReservoirLayout.elementStride * 2ull
        : 0ull;
    const std::uint64_t ptReservoirBytes = rtxdiPtActive
        ? static_cast<std::uint64_t>(ptReservoirLayout.arrayPitch) * ptReservoirLayout.elementStride * 2ull
        : 0ull;
    const char* activeIndirectAlgorithm =
        rtxdiPtActive ? "rtxdi_restir_pt" : (rtxdiGiActive ? "rtxdi_restir_gi" : "baseline_pt");
    const char* effectiveRestir = "baseline";
    if (rtxdiPtActive)
    {
        effectiveRestir = rtxdiDiActive ? "rtxdi_pt+di" : "rtxdi_pt";
    }
    else if (rtxdiGiActive)
    {
        effectiveRestir = rtxdiDiActive ? "rtxdi_gi+di" : "rtxdi_gi";
    }
    else if (rtxdiDiActive)
    {
        effectiveRestir = (UsesRestirGI(m_mode) || UsesRestirPT(m_mode))
            ? "rtxdi_di+baseline_indirect"
            : "rtxdi_di";
    }
    out << "\"restir\":{\"requestedBackend\":\"" << rb::RestirBackendName(m_qualitySettings.restirBackend)
        << "\",\"rtxdiAvailable\":" << (m_rtxdiAvailable ? "true" : "false")
        << ",\"effective\":\"" << effectiveRestir
        << "\",\"activeIndirectAlgorithm\":\"" << activeIndirectAlgorithm
        << "\",\"rtxdiStatus\":{\"requestedAtBuild\":" << (rtxdiStatus.requestedAtBuild ? "true" : "false")
        << ",\"compiled\":" << (rtxdiStatus.compiled ? "true" : "false")
        << ",\"sdkAvailable\":" << (rtxdiStatus.sdkAvailable ? "true" : "false")
        << ",\"runtimeLibraryLinked\":" << (rtxdiStatus.runtimeLibraryLinked ? "true" : "false")
        << ",\"evaluationReady\":" << (rtxdiStatus.evaluationReady ? "true" : "false")
        << ",\"diEvaluationReady\":" << (rtxdiStatus.diEvaluationReady ? "true" : "false")
        << ",\"giEvaluationReady\":" << (rtxdiStatus.giEvaluationReady ? "true" : "false")
        << ",\"ptEvaluationReady\":" << (rtxdiStatus.ptEvaluationReady ? "true" : "false")
        << ",\"diActive\":" << (rtxdiDiActive ? "true" : "false")
        << ",\"giActive\":" << (rtxdiGiActive ? "true" : "false")
        << ",\"ptActive\":" << (rtxdiPtActive ? "true" : "false")
        << ",\"diReservoirBytes\":" << diReservoirBytes
        << ",\"giReservoirBytes\":" << giReservoirBytes
        << ",\"ptReservoirBytes\":" << ptReservoirBytes
        << ",\"version\":\"" << cld::EscapeJson(rtxdiStatus.sdkVersion)
        << "\",\"sdkCommit\":\"" << cld::EscapeJson(rtxdiStatus.sdkCommit)
        << "\",\"runtimeCommit\":\"" << cld::EscapeJson(rtxdiStatus.runtimeCommit)
        << "\",\"fallbackReason\":\"" << cld::EscapeJson(rtxdiStatus.fallbackReason) << "\"}"
        << ",\"temporalReuse\":" << (m_restirTemporalReuse ? "true" : "false")
        << ",\"spatialReusePasses\":" << m_restirSpatialReusePasses << ",\"spatialRadius\":" << m_restirSpatialRadius
        << ",\"candidateSamples\":" << m_restirCandidateSamples << ",\"mClamp\":" << m_restirMClamp
        << ",\"diTemporalReuse\":" << (m_restirDiTemporalReuse ? "true" : "false")
        << ",\"diSpatialReusePasses\":" << m_restirDiSpatialReusePasses
        << ",\"diCandidateSamples\":" << m_restirDiCandidateSamples << ",\"diMClamp\":" << m_restirDiMClamp
        << ",\"reservoirReprojection\":" << (m_reservoirReprojection ? "true" : "false")
        << ",\"reservoirValidation\":" << (m_reservoirValidation ? "true" : "false")
        << ",\"giValidationRay\":" << (m_restirGiValidationRay ? "true" : "false")
        << ",\"reservoirMaxAge\":" << m_reservoirMaxAge << "},";
    out << "\"denoise\":{\"preset\":\"" << NoisePresetName(m_noisePreset) << "\",\"presetLabel\":\"" << NoisePresetDisplayName(m_noisePreset)
        << "\",\"backend\":\"" << DenoiseBackendName(m_denoiseBackend)
        << "\",\"backendLabel\":\"" << DenoiseBackendDisplayName(m_denoiseBackend)
        << "\",\"activeBackend\":\"" << ActiveDenoiseBackendName()
        << "\",\"dlssMode\":\"" << DlssModeName(m_dlssMode)
        << "\",\"dlssEnabledWhenAvailable\":" << (m_dlssEnabledWhenAvailable ? "true" : "false")
        << ",\"enabled\":" << (m_denoiserEnabled ? "true" : "false")
        << ",\"splitSignalDenoise\":" << (m_splitSignalDenoise ? "true" : "false")
        << ",\"realtimeReconstruction\":" << (m_realtimeReconstruction ? "true" : "false")
        << ",\"cameraJitter\":" << (m_cameraJitter ? "true" : "false")
        << ",\"temporalStability\":" << (m_temporalStabilityEnabled ? "true" : "false")
        << ",\"jitterMode\":\"" << JitterModeName(m_jitterMode) << "\""
        << ",\"movingJitterScale\":" << m_movingJitterScale
        << ",\"currentJitterStrength\":" << m_currentJitterStrength
        << ",\"cameraMotionAmount\":" << m_cameraMotionAmount
        << ",\"temporalHistoryValid\":" << (m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? "true" : "false")
        << ",\"maxHistoryFrames\":" << m_reconstructionMaxHistoryFrames
        << ",\"historyClampSigma\":" << m_historyClampSigma << ",\"reactiveThreshold\":" << m_reactiveThreshold
        << ",\"spatialIterations\":" << m_denoiserSpatialIterations << ",\"atrousPasses\":" << m_atrousPassCount
        << ",\"strength\":" << m_denoiserStrength << ",\"dlss\":" << BuildDlssStatusJson()
        << ",\"nrd\":" << BuildNrdStatusJson() << "},";
    out << "\"frameState\":{\"frameNumber\":" << m_frameState.frameNumber
        << ",\"progressiveSampleCount\":" << m_frameState.progressiveSampleCount
        << ",\"changeMask\":" << static_cast<uint32_t>(m_frameState.changes)
        << ",\"validHistoryDomains\":" << static_cast<uint32_t>(m_frameState.validHistoryDomains)
        << ",\"cameraCut\":" << (m_frameState.cameraCut ? "true" : "false")
        << ",\"revisions\":{\"scene\":" << m_frameState.revisions.scene
        << ",\"geometry\":" << m_frameState.revisions.geometry
        << ",\"material\":" << m_frameState.revisions.material
        << ",\"light\":" << m_frameState.revisions.light
        << ",\"hdri\":" << m_frameState.revisions.hdri
        << ",\"backend\":" << m_frameState.revisions.backend
        << ",\"qualityProfile\":" << m_frameState.revisions.qualityProfile << "}},";
    const int debugViewIndex = std::clamp(m_debugViewMode, 0, static_cast<int>(_countof(DebugViewLabels)) - 1);
    out << "\"view\":{\"debugView\":" << m_debugViewMode << ",\"debugViewLabel\":\"" << DebugViewLabels[debugViewIndex]
        << "\",\"normalMapYFlip\":" << (m_debugNormalMapYFlip ? "true" : "false")
        << ",\"exposure\":" << m_exposure << ",\"gamma\":" << m_gamma
        << ",\"toneMapper\":\"" << ToneMapperName(m_toneMapper)
        << "\",\"materialFocusMode\":\"" << MaterialFocusModeName(m_materialFocusMode)
        << "\",\"selectedMaterial\":" << m_selectedMaterial << "}";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpStatsJson() const
{
    std::ostringstream out;
    out << "{";
    out << "\"api\":\"Direct3D 12 DXR\",";
    out << "\"adapter\":\"" << cld::EscapeJson(WideToUtf8(m_adapterDescription)) << "\",";
    out << "\"dxrTier\":\"" << cld::EscapeJson(WideToUtf8(PathtracingTierName(m_raytracingTier))) << "\",";
    out << "\"resolution\":{\"width\":" << m_width << ",\"height\":" << m_height
        << ",\"renderWidth\":" << m_renderWidth << ",\"renderHeight\":" << m_renderHeight << "},";
    out << "\"gpuTiming\":{\"supported\":" << (m_gpuTimingSupported ? "true" : "false")
        << ",\"valid\":" << (m_gpuTimingValid ? "true" : "false")
        << ",\"pipelineMs\":" << m_gpuFrameMs
        << ",\"pathTraceMs\":" << m_gpuPathTraceMs
        << ",\"restirReuseMs\":" << m_gpuRestirMs
        << ",\"restirGiInitialMs\":" << m_gpuRestirGiInitialMs
        << ",\"restirGiFusedMs\":" << m_gpuRestirGiFusedMs
        << ",\"restirPtInitialMs\":" << m_gpuRestirPtInitialMs
        << ",\"restirPtFusedMs\":" << m_gpuRestirPtFusedMs
        << ",\"denoiseMs\":" << m_gpuDenoiseMs
        << ",\"copyMs\":" << m_gpuCopyMs
        << ",\"uiMs\":" << m_gpuUiMs
        << ",\"completedFrameSerial\":" << m_gpuTimingFrameSerial << "},";
    out << "\"materials\":" << m_scene.materials.size() << ",";
    out << "\"textures\":" << m_textures.size() << ",";
    out << "\"vertices\":" << m_scene.vertices.size() << ",";
    out << "\"indices\":" << m_scene.indices.size() << ",";
    out << "\"submittedIndices\":" << m_sceneSubmittedIndexCount << ",";
    out << "\"primitives\":" << m_scenePrimitiveCount << ",";
    out << "\"blasGeometries\":" << m_geometryRecords.size() << ",";
    out << "\"blasOriginalBytes\":" << m_blasOriginalBytes << ",";
    out << "\"blasCompactedBytes\":" << m_blasCompactedBytes << ",";
    out << "\"tlasInstances\":" << (m_scene.instances.empty() ? 1u : m_scene.instances.size()) << ",";
    out << "\"lights\":" << m_activeLightCount << ",";
    out << "\"emissiveTriangleLights\":" << m_emissiveTriangleLightCount << ",";
    out << "\"proceduralAreaLights\":" << m_proceduralAreaLightCount << ",";
    out << "\"samplesAccumulated\":" << m_accumulatedFrames << ",";
    out << "\"historyDomains\":{\"validMask\":" << static_cast<uint32_t>(m_validHistoryDomains)
        << ",\"lastChangeMask\":" << static_cast<uint32_t>(m_frameState.changes) << "},";
    out << "\"activeMode\":\"" << PathtracingModeName(m_mode) << "\",";
    const bool finalTaaActive = (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill && m_finalTaaPipeline;
    out << "\"finalTaaActive\":" << (finalTaaActive ? "true" : "false") << ",";
    out << "\"reservoirCount\":" << m_restirReservoirElementCount << ",";
    const char* resourceProfile = m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill
        ? "reference"
        : ActiveDenoiseBackendName();
    out << "\"resourceMemory\":{\"frameAndHistoryBytes\":" << m_frameHistoryResourceBytes
        << ",\"frameAndHistoryMiB\":" << (static_cast<double>(m_frameHistoryResourceBytes) / (1024.0 * 1024.0))
        << ",\"budgetMiB\":512.0"
        << ",\"headroomMiB\":" << ((512.0 * 1024.0 * 1024.0 - static_cast<double>(m_frameHistoryResourceBytes)) / (1024.0 * 1024.0))
        << ",\"restirAliasHeapBytes\":" << (m_restirAliasHeapSize * m_restirAliasHeaps.size())
        << ",\"taaOutputHistoryAliased\":" << (m_accumulationAliasesTaaHistory ? "true" : "false")
        << ",\"withinBudget\":" << (m_frameHistoryResourceBytes <= 512ull * 1024ull * 1024ull ? "true" : "false")
        << ",\"allocationProfile\":\"" << resourceProfile << "\"},";
    out << "\"textureResidency\":{\"budgetBytes\":" << m_textureBudgetBytes
        << ",\"residentBytes\":" << m_textureResidentBytes
        << ",\"dedicatedVideoMemoryBytes\":" << m_adapterDedicatedVideoMemory
        << ",\"withinBudget\":" << (m_textureResidentBytes <= m_textureBudgetBytes ? "true" : "false") << "},";
    out << "\"secondaryShading\":{\"requested\":\""
        << rb::SecondaryShadingRateName(m_qualitySettings.secondaryShadingRate)
        << "\",\"effective\":\"" << (m_activeSecondaryShadingRate < 0.75f ? "adaptive_half" : "full")
        << "\",\"activeRate\":" << m_activeSecondaryShadingRate
        << ",\"autoHalfActive\":" << (m_autoSecondaryHalfActive ? "true" : "false")
        << ",\"extraSampleQuotaEnabled\":" << (m_rayBudgetExtraSampleEnabled ? "true" : "false")
        << ",\"bouncePenalty\":" << m_rayBudgetBouncePenalty
        << ",\"overBudgetFrames\":" << m_overBudgetFrameCount
        << ",\"underBudgetFrames\":" << m_underBudgetFrameCount << "},";
    out << "\"denoiser\":{\"preset\":\"" << NoisePresetName(m_noisePreset)
        << "\",\"backend\":\"" << DenoiseBackendName(m_denoiseBackend)
        << "\",\"activeBackend\":\"" << ActiveDenoiseBackendName()
        << "\",\"enabled\":" << (m_denoiserEnabled ? "true" : "false")
        << ",\"temporalStability\":" << (m_temporalStabilityEnabled ? "true" : "false")
        << ",\"historyValid\":" << (m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? "true" : "false")
        << ",\"jitterMode\":\"" << JitterModeName(m_jitterMode) << "\",\"jitterStrength\":" << m_currentJitterStrength
        << ",\"spatialIterations\":" << m_denoiserSpatialIterations << ",\"atrousPasses\":" << m_atrousPassCount
        << ",\"dlss\":" << BuildDlssStatusJson() << ",\"nrd\":" << BuildNrdStatusJson() << "},";
    out << "\"mcp\":{\"running\":" << (m_mcpServer.IsRunning() ? "true" : "false") << ",\"pendingCommands\":" << m_mcpDispatcher.PendingCount() << "}";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpMaterialsJson() const
{
    std::ostringstream out;
    out << "{\"materials\":[";
    for (size_t i = 0; i < m_scene.materials.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        const Bistro::Material& material = m_scene.materials[i];
        out << "{\"index\":" << i << ",\"name\":\"" << cld::EscapeJson(WideToUtf8(material.name))
            << "\",\"sourceMaterialId\":\"" << cld::EscapeJson(material.sourceMaterialId) << "\",";
        out << "\"baseColor\":";
        AppendJsonFloat4(out, material.baseColorFactor);
        out << ",\"emissive\":";
        AppendJsonFloat4(out, material.emissiveFactor);
        out << ",\"roughness\":" << material.roughnessFactor << ",\"metallic\":" << material.metallicFactor;
        out << ",\"occlusionStrength\":" << material.occlusionStrength << ",\"normalStrength\":" << material.normalStrength;
        out << ",\"alphaCutoff\":" << material.alphaCutoff << ",\"alphaMasked\":" << (material.alphaMasked ? "true" : "false");
        out << ",\"packedORM\":" << (material.packedOcclusionRoughnessMetallic ? "true" : "false");
        out << ",\"gltfExtensions\":";
        WriteGltfExtensionsJson(out, material);
        if (i < m_materialUsage.size())
        {
            out << ",\"meshCount\":" << m_materialUsage[i].meshCount << ",\"triangleCount\":" << m_materialUsage[i].triangleCount;
        }
        out << ",\"hasEmissive\":" << ((material.emissiveFactor.x > 0.0f || material.emissiveFactor.y > 0.0f || material.emissiveFactor.z > 0.0f || material.emissiveFactor.w > 0.0f) ? "true" : "false");
        out << ",\"hasAlpha\":" << (material.alphaMasked || material.baseColorFactor.w < 0.99f ? "true" : "false");
        out << ",\"textures\":";
        out << BuildMaterialTexturesJson(i);
        out << "}";
    }
    out << "]}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMaterialTexturesJson(size_t materialIndex) const
{
    std::ostringstream out;
    out << "[";
    if (materialIndex < m_scene.materials.size())
    {
        const Bistro::Material& material = m_scene.materials[materialIndex];
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (slot > 0)
            {
                out << ",";
            }
            const bool overrideEnabled = materialIndex < m_textureOverrideEnabled.size() && m_textureOverrideEnabled[materialIndex][slot];
            const bool bindingOverrideEnabled = materialIndex < m_textureBindingOverrideEnabled.size() && m_textureBindingOverrideEnabled[materialIndex][slot];
            const std::wstring sourcePath = materialIndex < m_sourceMaterials.size() ? m_sourceMaterials[materialIndex].textures[slot] : std::wstring();
            out << "{\"slot\":" << slot << ",\"key\":\"" << TextureSlotKeys[slot] << "\",\"label\":\"" << TextureSlotLabels[slot] << "\"";
            out << ",\"sourcePath\":\"" << cld::EscapeJson(WideToUtf8(sourcePath)) << "\"";
            out << ",\"path\":\"" << cld::EscapeJson(WideToUtf8(material.textures[slot])) << "\"";
            out << ",\"overrideEnabled\":" << (overrideEnabled ? "true" : "false");
            const Bistro::TextureBinding& binding = material.textureBindings[slot];
            out << ",\"binding\":{\"uvSet\":" << binding.texCoord
                << ",\"offset\":[" << binding.offset.x << ',' << binding.offset.y << ']'
                << ",\"scale\":[" << binding.scale.x << ',' << binding.scale.y << ']'
                << ",\"rotation\":" << binding.rotation
                << ",\"sampler\":\"" << TextureSamplerName(binding.samplerIndex) << "\""
                << ",\"resolutionPolicy\":\"" << TextureResolutionPolicyName(binding.resolutionPolicy) << "\""
                << ",\"overrideEnabled\":" << (bindingOverrideEnabled ? "true" : "false") << '}';
            const bool exists = materialIndex < m_materialTextureExists.size() &&
                m_materialTextureExists[materialIndex][slot];
            out << ",\"exists\":" << (exists ? "true" : "false");
            if (materialIndex < m_materialTextureIndices.size() && slot < m_materialTextureIndices[materialIndex].size())
            {
                const UINT textureIndex = m_materialTextureIndices[materialIndex][slot];
                if (textureIndex < m_textures.size())
                {
                    const GpuTexture& texture = m_textures[textureIndex];
                    out << ",\"sourceWidth\":" << texture.sourceWidth << ",\"sourceHeight\":" << texture.sourceHeight
                        << ",\"residentWidth\":" << texture.width << ",\"residentHeight\":" << texture.height
                        << ",\"mipLevels\":" << texture.mipLevels << ",\"format\":" << static_cast<int>(texture.format)
                        << ",\"container\":\"" << cld::EscapeJson(texture.container) << "\""
                        << ",\"transcodeFormat\":\"" << cld::EscapeJson(texture.transcodeFormat) << "\""
                        << ",\"residentBytes\":" << texture.residentBytes
                        << ",\"fallback\":" << (texture.fallback ? "true" : "false")
                        << ",\"fallbackReason\":\"" << (texture.fallback ? "decode_or_budget_fallback" : "") << "\"";
                }
            }
            out << "}";
        }
    }
    out << "]";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMaterialVariantsJson() const
{
    std::ostringstream out;
    out << "{\"variants\":[";
    for (size_t i = 0; i < m_materialVariants.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        const MaterialVariant& variant = m_materialVariants[i];
        out << "{\"index\":" << i << ",\"name\":\"" << cld::EscapeJson(variant.name)
            << "\",\"materialIndex\":" << variant.materialIndex
            << ",\"materialName\":\"" << cld::EscapeJson(WideToUtf8(variant.materialName)) << "\"}";
    }
    out << "]}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMaterialPresetsJson() const
{
    std::ostringstream out;
    out << "{\"presets\":[";
    for (size_t i = 0; i < m_materialPresets.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        const MaterialPreset& preset = m_materialPresets[i];
        out << "{\"index\":" << i << ",\"name\":\"" << cld::EscapeJson(preset.name)
            << "\",\"category\":\"" << cld::EscapeJson(preset.category)
            << "\",\"sourcePath\":\"" << cld::EscapeJson(WideToUtf8(preset.sourcePath)) << "\"}";
    }
    out << "]}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpDiagnosticsJson() const
{
    const mcp::ServerStatus status = m_mcpServer.GetStatus();
    std::ostringstream out;
    out << "{";
    out << "\"scene\":\"" << cld::EscapeJson(m_sceneDiagnostics) << "\",";
    out << "\"project\":\"" << cld::EscapeJson(m_projectDiagnostics) << "\",";
    out << "\"mcp\":\"" << cld::EscapeJson(m_mcpUiDiagnostics) << "\",";
    out << "\"lastCapture\":\"" << cld::EscapeJson(m_mcpLastCaptureDiagnostics) << "\",";
    out << "\"server\":{\"running\":" << (status.running ? "true" : "false")
        << ",\"endpoint\":\"" << cld::EscapeJson(status.endpoint)
        << "\",\"lastError\":\"" << cld::EscapeJson(status.lastError)
        << "\",\"activeLegacySessions\":" << status.activeLegacySessions
        << ",\"activeSubscriptions\":" << status.activeSubscriptions
        << ",\"activeRequests\":" << status.activeRequests
        << ",\"pendingCommands\":" << m_mcpDispatcher.PendingCount() << ",\"recentRequests\":[";
    for (size_t i = 0; i < status.recentRequests.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        out << "\"" << cld::EscapeJson(status.recentRequests[i]) << "\"";
    }
    out << "]}}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpProjectJson() const
{
    std::ostringstream out;
    out << "{";
    out << "\"projectPath\":\"" << cld::EscapeJson(WideToUtf8(m_projectPath)) << "\",";
    out << "\"projectDirty\":" << (m_projectDirty ? "true" : "false") << ",";
    out << "\"scenePath\":\"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",";
    out << "\"environmentPath\":\"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\"";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpSceneSummaryJson() const
{
    std::ostringstream out;
    out << "{";
    out << "\"scenePath\":\"" << cld::EscapeJson(WideToUtf8(m_scenePath)) << "\",";
    out << "\"environmentPath\":\"" << cld::EscapeJson(WideToUtf8(m_environmentTexturePath)) << "\",";
    out << "\"meshes\":" << m_scene.draws.size() << ",\"materials\":" << m_scene.materials.size()
        << ",\"textures\":" << m_textures.size() << ",\"vertices\":" << m_scene.vertices.size()
        << ",\"indices\":" << m_scene.indices.size() << ",\"submittedIndices\":" << m_sceneSubmittedIndexCount
        << ",\"triangles\":" << m_scenePrimitiveCount << ",\"bounds\":{\"min\":";
    AppendJsonFloat3(out, m_scene.boundsMin);
    out << ",\"max\":";
    AppendJsonFloat3(out, m_scene.boundsMax);
    out << "},\"lights\":" << m_activeLightCount
        << ",\"emissiveTriangleLights\":" << m_emissiveTriangleLightCount
        << ",\"proceduralAreaLights\":" << m_proceduralAreaLightCount << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildMcpCaptureIndexJson() const
{
    std::ostringstream out;
    out << "{\"latest\":\"" << (m_mcpLatestCaptureBase64.empty() ? "" : "lookdevpt://captures/latest.png") << "\",\"captures\":[";
    for (size_t i = 0; i < m_mcpCaptures.size(); ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        const McpCapture& capture = m_mcpCaptures[i];
        out << "{\"id\":" << capture.id << ",\"debugView\":" << capture.debugView
            << ",\"label\":\"" << cld::EscapeJson(capture.label)
            << "\",\"width\":" << capture.rgba.width << ",\"height\":" << capture.rgba.height
            << ",\"fingerprints\":{\"scene\":\"" << cld::EscapeJson(capture.sceneFingerprint)
            << "\",\"camera\":\"" << cld::EscapeJson(capture.cameraFingerprint)
            << "\",\"material\":\"" << cld::EscapeJson(capture.materialFingerprint)
            << "\",\"lighting\":\"" << cld::EscapeJson(capture.lightingFingerprint)
            << "\",\"backend\":\"" << cld::EscapeJson(capture.backendFingerprint)
            << "\"},\"uri\":\"lookdevpt://captures/" << capture.id << ".png\",\"mimeType\":\"image/png\"}";
    }
    out << "]}";
    return out.str();
}

uint64_t D3D12PathTracingBackend::StoreMcpCapture(std::string base64Png, int debugView, const std::string& label)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        McpCapture capture;
        capture.id = m_nextMcpCaptureId++;
        capture.debugView = debugView;
        capture.label = label;
        capture.base64Png = std::move(base64Png);
        DecodePngRgba8(capture.base64Png, capture.rgba);
        capture.sceneFingerprint = SceneFingerprint();
        capture.cameraFingerprint = CameraFingerprint();
        capture.materialFingerprint = std::to_string(m_frameState.revisions.material);
        capture.lightingFingerprint = std::to_string(m_frameState.revisions.light) + "-" +
            std::to_string(m_frameState.revisions.hdri);
        capture.backendFingerprint = std::to_string(m_frameState.revisions.backend) + "-" +
            std::to_string(m_frameState.revisions.qualityProfile);
        capture.artifactBytes = capture.base64Png.size() * 3u / 4u + capture.rgba.pixels.size();
        capture.accessSerial = m_mcpArtifactAccessSerial++;
        m_mcpCaptures.push_back(std::move(capture));
        EnforceMcpArtifactBudget();
        id = m_mcpCaptures.back().id;
    }
    m_mcpServer.PublishResourceUpdates({ "lookdevpt://captures/index" });
    return id;
}

bool D3D12PathTracingBackend::FindMcpCapture(uint64_t id, std::string& base64Png, std::string& label) const
{
    std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
    for (const McpCapture& capture : m_mcpCaptures)
    {
        if (capture.id == id)
        {
            base64Png = capture.base64Png;
            label = capture.label;
            return true;
        }
    }
    return false;
}

mcp::ToolResult D3D12PathTracingBackend::SubmitMcpCommandTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool mutation, int timeoutMs)
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }

    if (mutation && settings.accessMode == mcp::AccessMode::ReadOnly)
    {
        return MakeMcpJsonToolResult(false, "MCP mutation rejected because access mode is Read Only.", "{\"ok\":false,\"diagnostics\":\"MCP mutation rejected because access mode is Read Only.\"}");
    }

    mcp::CommandRequest request;
    request.toolName = toolName;
    request.actionMethod = actionMethod;
    request.params = params;
    request.validateOnly = !mutation;
    request.mutation = mutation;
    request.summary = (mutation ? "Run " : "Read ") + toolName + " " + cld::JsonValueToJson(params);
    request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    const bool requiresApproval = mutation && settings.accessMode == mcp::AccessMode::ConfirmMutations;
    mcp::SubmitResult submitted = m_mcpDispatcher.Submit(std::move(request), requiresApproval);
    if (!submitted.accepted)
    {
        return MakeMcpJsonToolResult(false, submitted.diagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(submitted.diagnostics) + "\"}");
    }

    if (submitted.future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
    {
        m_mcpDispatcher.Cancel(submitted.id, "MCP request timed out.");
        return MakeMcpJsonToolResult(false, "MCP request timed out.", "{\"ok\":false,\"diagnostics\":\"MCP request timed out.\"}");
    }

    const mcp::CommandResult result = submitted.future.get();
    mcp::ToolResult tool = MakeMcpJsonToolResult(result.ok, result.diagnostics, result.structuredJson);
    tool.contentJson = result.contentJson;
    return tool;
}

mcp::ToolResult D3D12PathTracingBackend::SubmitMcpActionTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool validateOnly, int timeoutMs)
{
    mcp::ServerSettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mcpSettingsMutex);
        settings = m_mcpSettings;
    }

    const bool mutation = !validateOnly;
    if (mutation && settings.accessMode == mcp::AccessMode::ReadOnly)
    {
        return MakeMcpJsonToolResult(false, "MCP mutation rejected because access mode is Read Only.", "{\"ok\":false,\"diagnostics\":\"MCP mutation rejected because access mode is Read Only.\"}");
    }

    mcp::CommandRequest request;
    request.toolName = toolName;
    request.actionMethod = actionMethod;
    request.params = params;
    request.validateOnly = validateOnly;
    request.mutation = mutation;
    request.summary = (validateOnly ? "Validate " : "Apply ") + actionMethod + " " + cld::JsonValueToJson(params);
    request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    const bool requiresApproval = mutation && settings.accessMode == mcp::AccessMode::ConfirmMutations;
    mcp::SubmitResult submitted = m_mcpDispatcher.Submit(std::move(request), requiresApproval);
    if (!submitted.accepted)
    {
        return MakeMcpJsonToolResult(false, submitted.diagnostics, "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(submitted.diagnostics) + "\"}");
    }

    if (submitted.future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
    {
        m_mcpDispatcher.Cancel(submitted.id, "MCP request timed out.");
        return MakeMcpJsonToolResult(false, "MCP request timed out.", "{\"ok\":false,\"diagnostics\":\"MCP request timed out.\"}");
    }

    const mcp::CommandResult result = submitted.future.get();
    return MakeMcpJsonToolResult(result.ok, result.diagnostics, result.structuredJson);
}

mcp::ToolResult D3D12PathTracingBackend::MakeMcpJsonToolResult(bool ok, const std::string& text, const std::string& structuredJson) const
{
    mcp::ToolResult result;
    result.ok = ok;
    result.isError = !ok;
    result.text = text;
    result.structuredJson = structuredJson.empty() ? "{}" : structuredJson;
    return result;
}

bool D3D12PathTracingBackend::CaptureViewportPng(std::string& base64Png, std::string& diagnostics)
{
    return CaptureViewportPng(base64Png, diagnostics, {});
}

bool D3D12PathTracingBackend::CaptureViewportPng(
    std::string& base64Png,
    std::string& diagnostics,
    const std::filesystem::path& outputPath,
    lookdevpt::benchmark::ArtifactStatistics* statistics)
{
    if (!m_PathtracingOutput || !m_commandQueue || !m_device)
    {
        diagnostics = "Path tracing output is not ready.";
        return false;
    }

    try
    {
        WaitForPreviousFrame();
        const D3D12_RESOURCE_DESC sourceDesc = m_PathtracingOutput->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &layout, &rowCount, &rowSizeInBytes, &totalBytes);
        (void)rowCount;
        (void)rowSizeInBytes;

        ComPtr<ID3D12Resource> readback;
        const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readback)));

        FrameContext& frameContext = m_frameContexts[m_frameIndex];
        ThrowIfFailed(frameContext.commandAllocator->Reset());
        ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));
        auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &toCopySource);

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = m_PathtracingOutput.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        auto backToUav = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &backToUav);
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* commandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
        WaitForPreviousFrame();

        void* mapped = nullptr;
        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
        ThrowIfFailed(readback->Map(0, &readRange, &mapped));
        const uint8_t* sourceBytes = static_cast<const uint8_t*>(mapped) + layout.Offset;
        const size_t width = static_cast<size_t>(sourceDesc.Width);
        const size_t height = static_cast<size_t>(sourceDesc.Height);
        std::vector<uint8_t> pixels(width * height * 4);
        if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            readback->Unmap(0, nullptr);
            diagnostics = "Viewport capture expected an R8G8B8A8 output resource.";
            return false;
        }
        for (size_t y = 0; y < height; ++y)
        {
            const uint8_t* sourceRow = sourceBytes + y * layout.Footprint.RowPitch;
            uint8_t* destinationRow = pixels.data() + y * width * 4;
            memcpy(destinationRow, sourceRow, width * 4);
        }
        const D3D12_RANGE writtenRange = { 0, 0 };
        readback->Unmap(0, &writtenRange);

        DirectX::Image pngImage = {};
        pngImage.width = width;
        pngImage.height = height;
        pngImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        pngImage.rowPitch = width * 4;
        pngImage.slicePitch = pixels.size();
        pngImage.pixels = pixels.data();

        if (statistics && !ComputeArtifactStatistics(pngImage, *statistics, diagnostics))
        {
            return false;
        }

        DirectX::Blob pngBlob;
        ThrowIfFailed(DirectX::SaveToWICMemory(pngImage, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, pngBlob));
        if (!outputPath.empty())
        {
            std::error_code directoryError;
            if (!outputPath.parent_path().empty())
            {
                std::filesystem::create_directories(outputPath.parent_path(), directoryError);
            }
            if (directoryError)
            {
                diagnostics = "Could not create viewport capture directory: " + directoryError.message();
                return false;
            }
            std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                diagnostics = "Could not open viewport capture output file.";
                return false;
            }
            file.write(
                reinterpret_cast<const char*>(pngBlob.GetConstBufferPointer()),
                static_cast<std::streamsize>(pngBlob.GetBufferSize()));
            if (!file)
            {
                diagnostics = "Could not write viewport capture output file.";
                return false;
            }
        }
        base64Png = Base64Encode(pngBlob.GetConstBufferPointer(), pngBlob.GetBufferSize());
        diagnostics = "Viewport captured.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

std::string D3D12PathTracingBackend::SceneFingerprint() const
{
    std::ostringstream value;
    value << std::hex << m_frameState.revisions.scene << '-' << m_frameState.revisions.geometry;
    return value.str();
}

std::string D3D12PathTracingBackend::CameraFingerprint() const
{
    const XMFLOAT3 position = m_camera.GetPosition();
    std::ostringstream value;
    value << std::hexfloat << position.x << ',' << position.y << ',' << position.z << ','
        << m_camera.GetYawRadians() << ',' << m_camera.GetPitchRadians() << ','
        << m_camera.GetRollRadians() << ',' << m_cameraFovDegrees;
    return value.str();
}

void D3D12PathTracingBackend::EnforceMcpArtifactBudget()
{
    constexpr size_t MaximumImageCount = 24u;
    constexpr size_t MaximumArtifactBytes = 128u * 1024u * 1024u;
    auto usage = [&]()
    {
        size_t images = m_mcpCaptures.size() + m_mcpComparisons.size();
        size_t bytes = 0;
        for (const auto& capture : m_mcpCaptures) bytes += capture.artifactBytes;
        for (const auto& comparison : m_mcpComparisons) bytes += comparison.artifactBytes;
        return std::pair(images, bytes);
    };
    for (;;)
    {
        const auto [images, bytes] = usage();
        if (images <= MaximumImageCount && bytes <= MaximumArtifactBytes) break;
        auto captureCandidate = m_mcpCaptures.end();
        for (auto iterator = m_mcpCaptures.begin(); iterator != m_mcpCaptures.end(); ++iterator)
        {
            if (iterator->pinCount == 0u && (captureCandidate == m_mcpCaptures.end() ||
                iterator->accessSerial < captureCandidate->accessSerial))
            {
                captureCandidate = iterator;
            }
        }
        auto comparisonCandidate = m_mcpComparisons.end();
        for (auto iterator = m_mcpComparisons.begin(); iterator != m_mcpComparisons.end(); ++iterator)
        {
            if (comparisonCandidate == m_mcpComparisons.end() ||
                iterator->accessSerial < comparisonCandidate->accessSerial)
            {
                comparisonCandidate = iterator;
            }
        }
        const bool evictCapture = captureCandidate != m_mcpCaptures.end() &&
            (comparisonCandidate == m_mcpComparisons.end() ||
                captureCandidate->accessSerial <= comparisonCandidate->accessSerial);
        if (!evictCapture && comparisonCandidate == m_mcpComparisons.end()) break;
        if (evictCapture)
        {
            const uint64_t evictedId = captureCandidate->id;
            for (auto& review : m_mcpReviews)
            {
                if (std::find(review.captureIds.begin(), review.captureIds.end(), evictedId) != review.captureIds.end() ||
                    review.baselineCaptureId == evictedId)
                {
                    review.artifactEvicted = true;
                }
            }
            m_mcpCaptures.erase(captureCandidate);
        }
        else
        {
            const uint64_t evictedId = comparisonCandidate->id;
            for (auto& review : m_mcpReviews)
            {
                if (review.comparisonId == evictedId) review.artifactEvicted = true;
            }
            m_mcpComparisons.erase(comparisonCandidate);
        }
    }
}

void D3D12PathTracingBackend::RefreshMcpAuditCache()
{
    const rb::FrameRevisions& revisions = m_frameState.revisions;
    const bool changed = m_mcpAuditJson == "{}" ||
        revisions.scene != m_mcpAuditRevisions.scene ||
        revisions.geometry != m_mcpAuditRevisions.geometry ||
        revisions.material != m_mcpAuditRevisions.material ||
        revisions.light != m_mcpAuditRevisions.light ||
        revisions.hdri != m_mcpAuditRevisions.hdri ||
        revisions.backend != m_mcpAuditRevisions.backend ||
        revisions.qualityProfile != m_mcpAuditRevisions.qualityProfile;
    if (!changed) return;

    if (!m_mcpSceneAuditFresh && (revisions.scene != m_mcpAuditRevisions.scene ||
        revisions.geometry != m_mcpAuditRevisions.geometry ||
        revisions.material != m_mcpAuditRevisions.material || m_mcpAuditJson == "{}"))
    {
        m_mcpSceneAuditSummary = lookdevpt::review::AnalyzeScene(m_scene);
    }
    lookdevpt::review::AuditRuntimeState runtime;
    runtime.sceneRevision = revisions.scene;
    runtime.geometryRevision = revisions.geometry;
    runtime.materialRevision = revisions.material;
    runtime.lightRevision = revisions.light;
    runtime.hdriRevision = revisions.hdri;
    runtime.backendRevision = revisions.backend;
    runtime.profileRevision = revisions.qualityProfile;
    runtime.activeLightCount = m_activeLightCount;
    runtime.environmentEnabled = m_environmentMapEnabled;
    runtime.environmentAvailable = !m_environmentTexturePath.empty();
    const RtxdiStatus& rtxdi = m_rtxdiBackendRuntime.Status();
    runtime.rtxdiRequested = m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi;
    runtime.rtxdiAvailable = rtxdi.evaluationReady;
    runtime.nrdRequested = IsNrdSelected();
    runtime.nrdAvailable = m_nrdBackendRuntime.Status().evaluationReady;
    runtime.dlssRequested = IsDlssSelected();
    runtime.dlssAvailable = m_dlssBackendRuntime.Status().evaluationReady;
    runtime.textureBudgetBytes = m_textureBudgetBytes;
    runtime.textureResidentBytes = m_textureResidentBytes;
    runtime.dedicatedVideoMemoryBytes = m_adapterDedicatedVideoMemory;
    const lookdevpt::review::AuditReport report =
        lookdevpt::review::BuildAuditReport(m_mcpSceneAuditSummary, runtime);
    const std::string json = lookdevpt::review::BuildAuditJson(report);
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        m_mcpAuditJson = json;
        m_mcpAuditInfoCount = report.infoCount;
        m_mcpAuditWarningCount = report.warningCount;
        m_mcpAuditErrorCount = report.errorCount;
        m_mcpAuditRevisions = revisions;
        m_mcpSceneAuditFresh = false;
    }
    m_mcpServer.PublishResourceUpdates({ "lookdevpt://scene/audit" });
}

std::string D3D12PathTracingBackend::BuildMcpReviewIndexJson() const
{
    std::ostringstream json;
    json << "{\"activeReviewId\":";
    if (m_activeMcpReviewId == 0) json << "null"; else json << "\"" << m_activeMcpReviewId << "\"";
    json << ",\"reviews\":[";
    for (size_t i = 0; i < m_mcpReviews.size(); ++i)
    {
        if (i != 0) json << ',';
        const McpReview& review = m_mcpReviews[i];
        json << "{\"id\":\"" << review.id << "\",\"preset\":\"" << cld::EscapeJson(review.preset)
            << "\",\"state\":\"" << review.state << "\",\"progress\":" << review.progress
            << ",\"resource\":\"lookdevpt://reviews/" << review.id << "\"}";
    }
    json << "]}";
    return json.str();
}

std::string D3D12PathTracingBackend::BuildMcpCheckpointIndexJson() const
{
    std::ostringstream json;
    json << "{\"checkpoints\":[";
    for (size_t i = 0; i < m_mcpCheckpoints.size(); ++i)
    {
        if (i != 0) json << ',';
        const McpCheckpoint& checkpoint = m_mcpCheckpoints[i];
        json << "{\"id\":\"" << checkpoint.id << "\",\"label\":\""
            << cld::EscapeJson(checkpoint.label) << "\",\"sceneFingerprint\":\""
            << cld::EscapeJson(checkpoint.sceneFingerprint) << "\",\"resource\":\"lookdevpt://checkpoints/"
            << checkpoint.id << "\"}";
    }
    json << "]}";
    return json.str();
}

std::string D3D12PathTracingBackend::BuildMcpBenchmarkIndexJson() const
{
    std::ostringstream json;
    json << "{\"activeBenchmarkId\":";
    if (m_activeMcpBenchmarkId == 0) json << "null";
    else json << '\"' << m_activeMcpBenchmarkId << '\"';
    json << ",\"benchmarks\":[";
    for (size_t i = 0; i < m_mcpBenchmarks.size(); ++i)
    {
        if (i != 0) json << ',';
        const McpBenchmark& benchmark = m_mcpBenchmarks[i];
        const double progress = benchmark.totalFrames == 0 ? 0.0 :
            static_cast<double>(benchmark.completedFrames) / benchmark.totalFrames;
        json << "{\"id\":\"" << benchmark.id << "\",\"kind\":\"" << benchmark.kind
            << "\",\"state\":\"" << benchmark.state << "\",\"progress\":" << progress
            << ",\"resource\":\"lookdevpt://benchmarks/" << benchmark.id << "\"}";
    }
    json << "]}";
    return json.str();
}

std::string D3D12PathTracingBackend::BuildMcpBenchmarkJson(uint64_t id) const
{
    const auto iterator = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
        [&](const McpBenchmark& benchmark) { return benchmark.id == id; });
    if (iterator == m_mcpBenchmarks.end()) return "{\"ok\":false,\"code\":\"benchmark_not_found\"}";
    const McpBenchmark& benchmark = *iterator;
    const double progress = benchmark.totalFrames == 0 ? 0.0 :
        static_cast<double>(benchmark.completedFrames) / benchmark.totalFrames;
    std::ostringstream json;
    json << "{\"ok\":true,\"id\":\"" << benchmark.id << "\",\"kind\":\"" << benchmark.kind
        << "\",\"state\":\"" << benchmark.state << "\",\"progress\":" << progress
        << ",\"completedFrames\":" << benchmark.completedFrames << ",\"totalFrames\":" << benchmark.totalFrames
        << ",\"diagnostics\":\"" << cld::EscapeJson(benchmark.diagnostics) << "\"";
    if (benchmark.state == "completed")
    {
        const std::string prefix = "lookdevpt://benchmarks/" + std::to_string(benchmark.id) + "/";
        json << ",\"artifacts\":["
            << "{\"name\":\"summary.json\",\"mimeType\":\"application/json\",\"resource\":\"" << prefix << "summary.json\"},"
            << "{\"name\":\"artifacts.json\",\"mimeType\":\"application/json\",\"resource\":\"" << prefix << "artifacts.json\"},"
            << "{\"name\":\"quality_analysis.json\",\"mimeType\":\"application/json\",\"resource\":\"" << prefix << "quality_analysis.json\"},"
            << "{\"name\":\"frames.csv\",\"mimeType\":\"text/csv\",\"resource\":\"" << prefix << "frames.csv\"}]";
    }
    json << '}';
    return json.str();
}

void D3D12PathTracingBackend::ProcessMcpBenchmark()
{
    if (m_activeMcpBenchmarkId == 0) return;
    McpBenchmark benchmark;
    bool terminal = false;
    bool cancelled = false;
    bool publishProgress = false;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        const auto iterator = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
            [&](const McpBenchmark& candidate) { return candidate.id == m_activeMcpBenchmarkId; });
        if (iterator == m_mcpBenchmarks.end())
        {
            m_activeMcpBenchmarkId = 0;
            return;
        }
        iterator->completedFrames = (std::min)(m_benchmarkFrameIndex, iterator->totalFrames);
        cancelled = iterator->cancelRequested;
        terminal = cancelled || m_benchmarkFinished;
        const uint32_t publishStep = (std::max)(1u, iterator->totalFrames / 20u);
        if (!terminal && iterator->completedFrames >= iterator->lastPublishedFrame + publishStep)
        {
            iterator->lastPublishedFrame = iterator->completedFrames;
            publishProgress = true;
        }
        benchmark = *iterator;
    }
    const std::string idText = std::to_string(benchmark.id);
    if (!terminal)
    {
        if (publishProgress)
            m_mcpServer.PublishResourceUpdates({ "lookdevpt://benchmarks/index", "lookdevpt://benchmarks/" + idText });
        return;
    }

    if (cancelled) WaitForPreviousFrame();
    m_benchmarkHarness.reset();
    m_benchmarkOptions = {};
    m_benchmarkFinished = true;
    m_completedBenchmarkMetrics.clear();
    m_vsyncEnabled = benchmark.vsyncEnabled;
    m_samplingSeed = benchmark.samplingSeed;
    std::string restoreDiagnostics;
    const bool restored = LoadProjectFromDisk(benchmark.checkpointPath.wstring(), restoreDiagnostics);
    if (restored)
    {
        m_projectPath = benchmark.projectPath;
        m_projectDiagnostics = benchmark.projectDiagnostics;
        m_projectDirty = benchmark.projectDirty;
    }
    std::error_code error;
    std::filesystem::remove(benchmark.checkpointPath, error);
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        const auto iterator = std::find_if(m_mcpBenchmarks.begin(), m_mcpBenchmarks.end(),
            [&](const McpBenchmark& candidate) { return candidate.id == benchmark.id; });
        if (iterator != m_mcpBenchmarks.end())
        {
            iterator->completedFrames = (std::min)(m_benchmarkFrameIndex, iterator->totalFrames);
            if (!restored)
            {
                iterator->state = "failed";
                iterator->diagnostics = "Benchmark ended, but checkpoint restoration failed: " + restoreDiagnostics;
            }
            else if (cancelled)
            {
                iterator->state = "cancelled";
                iterator->diagnostics = "Benchmark cancelled; interactive state restored from checkpoint.";
            }
            else
            {
                iterator->state = "completed";
                iterator->completedFrames = iterator->totalFrames;
                iterator->diagnostics = "Benchmark completed; interactive state restored from checkpoint.";
            }
        }
        m_activeMcpBenchmarkId = 0;
    }
    std::vector<std::string> resources =
    {
        "lookdevpt://benchmarks/index",
        "lookdevpt://benchmarks/" + idText,
    };
    if (!cancelled && restored)
    {
        resources.push_back("lookdevpt://benchmarks/" + idText + "/summary.json");
        resources.push_back("lookdevpt://benchmarks/" + idText + "/artifacts.json");
        resources.push_back("lookdevpt://benchmarks/" + idText + "/quality_analysis.json");
        resources.push_back("lookdevpt://benchmarks/" + idText + "/frames.csv");
    }
    m_mcpServer.PublishResourceUpdates(resources);
}

std::string D3D12PathTracingBackend::BuildMcpReviewJson(uint64_t id) const
{
    const auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
        [&](const McpReview& review) { return review.id == id; });
    if (iterator == m_mcpReviews.end()) return "{\"ok\":false,\"code\":\"review_not_found\"}";
    const McpReview& review = *iterator;
    std::ostringstream json;
    json << "{\"ok\":" << (review.artifactEvicted ? "false" : "true") << ",\"id\":\"" << review.id
        << "\",\"preset\":\"" << cld::EscapeJson(review.preset) << "\",\"state\":\"" << review.state
        << "\",\"stage\":\"" << cld::EscapeJson(review.stage) << "\",\"progress\":" << review.progress;
    if (review.artifactEvicted) json << ",\"code\":\"artifact_evicted\"";
    else if (!review.errorCode.empty()) json << ",\"code\":\"" << cld::EscapeJson(review.errorCode) << "\"";
    json << ",\"diagnostics\":\"" << cld::EscapeJson(review.diagnostics) << "\",\"audit\":"
        << (review.auditJson.empty() ? "{}" : review.auditJson) << ",\"captures\":[";
    for (size_t i = 0; i < review.captureIds.size(); ++i)
    {
        if (i != 0) json << ',';
        json << "{\"id\":\"" << review.captureIds[i] << "\",\"debugView\":" << review.views[i]
            << ",\"label\":\"" << cld::EscapeJson(DebugViewLabels[review.views[i]])
            << "\",\"resource\":\"lookdevpt://captures/" << review.captureIds[i] << ".png\"}";
    }
    json << ']';
    if (review.comparisonId != 0)
    {
        json << ",\"comparison\":\"lookdevpt://comparisons/" << review.comparisonId << "\"";
    }
    json << '}';
    return json.str();
}

void D3D12PathTracingBackend::ProcessMcpReview()
{
    PollMcpReviewCapture();
    uint64_t activeId = 0;
    int viewToCapture = -1;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        activeId = m_activeMcpReviewId;
        if (activeId == 0) return;
        auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
            [&](const McpReview& review) { return review.id == activeId; });
        if (iterator == m_mcpReviews.end())
        {
            m_activeMcpReviewId = 0;
            return;
        }
        McpReview& review = *iterator;
        auto finish = [&](const char* state, const char* stage, const char* code, const char* diagnostics)
        {
            review.state = state;
            review.stage = stage;
            review.errorCode = code;
            review.diagnostics = diagnostics;
            review.progress = strcmp(state, "completed") == 0 ? 1.0 : review.progress;
            for (const uint64_t captureId : review.captureIds)
            {
                for (auto& capture : m_mcpCaptures)
                    if (capture.id == captureId && capture.pinCount != 0u) --capture.pinCount;
            }
            if (review.baselineCaptureId != 0)
            {
                for (auto& capture : m_mcpCaptures)
                    if (capture.id == review.baselineCaptureId && capture.pinCount != 0u) --capture.pinCount;
            }
            m_activeMcpReviewId = 0;
            EnforceMcpArtifactBudget();
        };
        if (review.cancelRequested)
        {
            finish("cancelled", "cancelled", "", "Review cancelled.");
        }
        else if (std::chrono::steady_clock::now() >= review.deadline)
        {
            finish("failed", "failed", "review_timeout", "Review timed out.");
        }
        else if (review.state == "queued")
        {
            review.startFrame = m_frameState.frameNumber;
            review.settleUntilFrame = review.startFrame + review.settleUntilFrame;
            review.state = "waiting";
            review.stage = "waiting_for_scene_and_settle";
            review.progress = 0.05;
        }
        else if (review.state == "waiting")
        {
            const SceneLoadStage loadStage = m_sceneLoadStage.load(std::memory_order_relaxed);
            if (loadStage == SceneLoadStage::Failed || loadStage == SceneLoadStage::Cancelled)
            {
                finish("failed", "failed", "scene_load_failed", "Scene loading failed or was cancelled.");
            }
            else if ((loadStage == SceneLoadStage::Idle || loadStage == SceneLoadStage::Completed) &&
                m_frameState.frameNumber >= review.settleUntilFrame)
            {
                review.state = "capturing";
                review.stage = "capturing";
                review.progress = 0.1;
            }
        }
        else if (review.state == "capturing")
        {
            if (m_pendingMcpReviewReadback)
            {
                review.stage = "waiting_for_gpu_readback";
            }
            else if (m_mcpReviewCaptureFuture.valid())
            {
                review.stage = "encoding_capture";
            }
            else if (review.nextView < review.views.size()) viewToCapture = review.views[review.nextView];
            else
            {
                review.state = "analyzing";
                review.stage = "analyzing";
                review.progress = 0.9;
            }
        }
        else if (review.state == "analyzing")
        {
            if (review.baselineCaptureId != 0 && !review.captureIds.empty())
            {
                McpCapture* before = nullptr;
                McpCapture* after = nullptr;
                for (auto& capture : m_mcpCaptures)
                {
                    if (capture.id == review.baselineCaptureId) before = &capture;
                    if (capture.id == review.captureIds.front()) after = &capture;
                }
                if (!before || !after)
                {
                    review.artifactEvicted = true;
                    finish("failed", "failed", "artifact_evicted", "A review artifact was evicted.");
                }
                else
                {
                    lookdevpt::review::ComparisonMetrics metrics;
                    lookdevpt::review::Rgba8Image heatmap;
                    std::string comparisonDiagnostics;
                    if (!lookdevpt::review::CompareImages(before->rgba, after->rgba, metrics, heatmap, comparisonDiagnostics))
                    {
                        finish("failed", "failed", comparisonDiagnostics.c_str(), comparisonDiagnostics.c_str());
                    }
                    else
                    {
                        McpComparison comparison;
                        comparison.id = m_nextMcpComparisonId++;
                        comparison.beforeCaptureId = before->id;
                        comparison.afterCaptureId = after->id;
                        comparison.json = lookdevpt::review::BuildComparisonJson(std::to_string(comparison.id),
                            std::to_string(before->id), std::to_string(after->id), metrics,
                            before->sceneFingerprint == after->sceneFingerprint,
                            before->cameraFingerprint == after->cameraFingerprint);
                        EncodePngRgba8(heatmap, comparison.heatmapBase64);
                        comparison.artifactBytes = comparison.heatmapBase64.size() * 3u / 4u;
                        comparison.accessSerial = m_mcpArtifactAccessSerial++;
                        review.comparisonId = comparison.id;
                        m_mcpComparisons.push_back(std::move(comparison));
                    }
                }
            }
            if (m_activeMcpReviewId != 0)
                finish("completed", "completed", "", "Review completed without changing renderer state.");
        }
    }

    if (viewToCapture >= 0)
    {
        std::string diagnostics;
        const bool captured = BeginMcpReviewCapture(activeId, viewToCapture, diagnostics);
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
            [&](const McpReview& review) { return review.id == activeId; });
        if (iterator != m_mcpReviews.end() && m_activeMcpReviewId == activeId)
        {
            McpReview& review = *iterator;
            if (!captured)
            {
                review.state = "failed";
                review.stage = "failed";
                review.errorCode = "capture_failed";
                review.diagnostics = diagnostics;
                for (const uint64_t captureId : review.captureIds)
                    for (auto& capture : m_mcpCaptures)
                        if (capture.id == captureId && capture.pinCount != 0u) --capture.pinCount;
                if (review.baselineCaptureId != 0)
                    for (auto& capture : m_mcpCaptures)
                        if (capture.id == review.baselineCaptureId && capture.pinCount != 0u) --capture.pinCount;
                m_activeMcpReviewId = 0;
            }
            else
            {
                review.stage = "waiting_for_gpu_readback";
            }
        }
    }
    const std::string reviewUri = "lookdevpt://reviews/" + std::to_string(activeId);
    m_mcpServer.PublishResourceUpdates({ "lookdevpt://reviews/index", reviewUri,
        "lookdevpt://captures/index" });
}

bool D3D12PathTracingBackend::BeginMcpReviewCapture(
    uint64_t reviewId,
    int debugView,
    std::string& diagnostics)
{
    ID3D12Resource* source = m_PathtracingOutput.Get();
    McpReviewVisualization visualization = McpReviewVisualization::Color;
    switch (debugView)
    {
    case 1: source = SurfaceGuideAovResource(1u, LastSubmittedSurfaceGuideParity()); break;
    case 2: case 3: case 41:
        source = SurfaceGuideAovResource(0u, LastSubmittedSurfaceGuideParity());
        visualization = McpReviewVisualization::Normal;
        break;
    case 4: case 42:
        source = SurfaceGuideAovResource(1u, LastSubmittedSurfaceGuideParity());
        visualization = McpReviewVisualization::ScalarW;
        break;
    case 5:
        source = m_signalResidual.Get();
        visualization = McpReviewVisualization::ScalarW;
        break;
    case 6: case 12: case 34:
        source = m_signalResidual.Get();
        visualization = McpReviewVisualization::Radiance;
        break;
    case 7: case 43:
        source = SurfaceGuideAovResource(2u, LastSubmittedSurfaceGuideParity());
        visualization = McpReviewVisualization::ScalarW;
        break;
    case 8: case 32:
        source = m_signalDirect.Get();
        visualization = McpReviewVisualization::Radiance;
        break;
    case 9: case 22: case 33:
        source = m_signalIndirect.Get();
        visualization = McpReviewVisualization::Radiance;
        break;
    case 19:
        source = (LastSubmittedSurfaceGuideParity() == 0u
            ? m_reconstructionHistoryMoments.Get() : m_reconstructionHistoryMomentsB.Get());
        visualization = McpReviewVisualization::Variance;
        break;
    case 20: case 44:
        source = SurfaceGuideAovResource(2u, LastSubmittedSurfaceGuideParity());
        visualization = McpReviewVisualization::Motion;
        break;
    case 21: case 38: case 46:
        source = (LastSubmittedSurfaceGuideParity() == 0u
            ? m_reconstructionHistoryLength.Get() : m_reconstructionHistoryLengthB.Get());
        visualization = McpReviewVisualization::ScalarZ;
        break;
    case 23: case 36:
        source = m_postDenoiseHdr.Get();
        visualization = McpReviewVisualization::Radiance;
        break;
    case 35:
        source = m_accumulationOutput.Get();
        visualization = McpReviewVisualization::Radiance;
        break;
    case 37:
        source = FinalResolvedHdrResource(LastSubmittedSurfaceGuideParity());
        visualization = McpReviewVisualization::Radiance;
        break;
    case 40:
        source = m_nrdDiffuseConfidence.Get();
        visualization = McpReviewVisualization::ScalarX;
        break;
    default:
        break;
    }
    if (!source || !m_commandQueue || !m_device || !m_fence ||
        !m_mcpReviewCommandAllocator || !m_mcpReviewCommandList)
    {
        diagnostics = "Review source is not available.";
        return false;
    }

    try
    {
        if (m_pendingMcpReviewReadback || m_mcpReviewCaptureFuture.valid())
        {
            diagnostics = "Review capture pipeline is busy.";
            return false;
        }
        const D3D12_RESOURCE_DESC desc = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &totalBytes);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rows == 0 || totalBytes == 0)
        {
            diagnostics = "Review source is not a copyable 2D texture.";
            return false;
        }

        ComPtr<ID3D12Resource> readback;
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC buffer = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        ThrowIfFailed(m_mcpReviewCommandAllocator->Reset());
        ThrowIfFailed(m_mcpReviewCommandList->Reset(m_mcpReviewCommandAllocator.Get(), nullptr));
        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(source,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_mcpReviewCommandList->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = source;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        m_mcpReviewCommandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(source,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_mcpReviewCommandList->ResourceBarrier(1, &toUav);
        ThrowIfFailed(m_mcpReviewCommandList->Close());
        ID3D12CommandList* lists[] = { m_mcpReviewCommandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, lists);
        const UINT64 captureFence = m_fenceValue++;
        ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), captureFence));
        PendingMcpReviewReadback pending;
        pending.reviewId = reviewId;
        pending.debugView = debugView;
        pending.visualization = visualization;
        pending.resource = std::move(readback);
        pending.sourceDesc = desc;
        pending.layout = layout;
        pending.rows = rows;
        pending.rowBytes = rowBytes;
        pending.totalBytes = totalBytes;
        pending.fenceValue = captureFence;
        pending.sceneFingerprint = SceneFingerprint();
        pending.cameraFingerprint = CameraFingerprint();
        pending.materialFingerprint = std::to_string(m_frameState.revisions.material);
        pending.lightingFingerprint = std::to_string(m_frameState.revisions.light) + "-" +
            std::to_string(m_frameState.revisions.hdri);
        pending.backendFingerprint = std::to_string(m_frameState.revisions.backend) + "-" +
            std::to_string(m_frameState.revisions.qualityProfile);
        m_pendingMcpReviewReadback = std::move(pending);
        diagnostics = "Review GPU readback queued.";
        return true;
    }
    catch (const std::exception& error)
    {
        diagnostics = error.what();
        return false;
    }
}

D3D12PathTracingBackend::McpReviewCaptureResult D3D12PathTracingBackend::EncodeMcpReviewCapture(
    uint64_t reviewId,
    int debugView,
    McpReviewVisualization visualization,
    D3D12_RESOURCE_DESC sourceDesc,
    UINT64 rowBytes,
    std::vector<uint8_t> sourcePixels)
{
    McpReviewCaptureResult result;
    result.reviewId = reviewId;
    result.debugView = debugView;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    try
    {
        DirectX::Image input{};
        input.width = static_cast<size_t>(sourceDesc.Width);
        input.height = sourceDesc.Height;
        input.format = sourceDesc.Format;
        input.rowPitch = static_cast<size_t>(rowBytes);
        input.slicePitch = sourcePixels.size();
        input.pixels = sourcePixels.data();
        DirectX::ScratchImage floats;
        ThrowIfFailed(DirectX::Convert(input, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DirectX::TEX_FILTER_DEFAULT, 0.0f, floats));
        const DirectX::Image* floatImage = floats.GetImage(0, 0, 0);
        if (!floatImage) throw std::runtime_error("Review source conversion failed.");

        result.rgba.width = static_cast<uint32_t>(floatImage->width);
        result.rgba.height = static_cast<uint32_t>(floatImage->height);
        result.rgba.pixels.resize(floatImage->width * floatImage->height * 4u);
        auto encode = [](float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            const float srgb = value <= 0.0031308f ? 12.92f * value : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::round(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
        };
        for (size_t y = 0; y < floatImage->height; ++y)
        {
            const XMFLOAT4* row = reinterpret_cast<const XMFLOAT4*>(floatImage->pixels + y * floatImage->rowPitch);
            for (size_t x = 0; x < floatImage->width; ++x)
            {
                XMFLOAT3 color(row[x].x, row[x].y, row[x].z);
                bool linearColor = true;
                switch (visualization)
                {
                case McpReviewVisualization::Radiance:
                    color = XMFLOAT3(color.x / (1.0f + std::max(color.x, 0.0f)),
                        color.y / (1.0f + std::max(color.y, 0.0f)),
                        color.z / (1.0f + std::max(color.z, 0.0f)));
                    break;
                case McpReviewVisualization::Normal:
                    linearColor = false;
                    break;
                case McpReviewVisualization::ScalarX: color = XMFLOAT3(row[x].x, row[x].x, row[x].x); break;
                case McpReviewVisualization::ScalarY: color = XMFLOAT3(row[x].y, row[x].y, row[x].y); break;
                case McpReviewVisualization::ScalarZ: color = XMFLOAT3(row[x].z, row[x].z, row[x].z); break;
                case McpReviewVisualization::ScalarW:
                {
                    const float value = debugView == 7 || debugView == 43
                        ? std::clamp(row[x].w / 250.0f, 0.0f, 1.0f) : row[x].w;
                    color = XMFLOAT3(value, value, value);
                    break;
                }
                case McpReviewVisualization::Motion:
                    color = XMFLOAT3(0.5f + row[x].x * 20.0f, 0.5f + row[x].y * 20.0f, 0.5f);
                    linearColor = false;
                    break;
                case McpReviewVisualization::Variance:
                {
                    const float variance = std::max(row[x].y - row[x].x * row[x].x, 0.0f);
                    const float value = variance / (variance + 1.0f);
                    color = XMFLOAT3(value, value, value);
                    break;
                }
                default:
                    linearColor = false;
                    break;
                }
                const size_t offset = (y * floatImage->width + x) * 4u;
                if (linearColor)
                {
                    result.rgba.pixels[offset + 0u] = encode(color.x);
                    result.rgba.pixels[offset + 1u] = encode(color.y);
                    result.rgba.pixels[offset + 2u] = encode(color.z);
                }
                else
                {
                    result.rgba.pixels[offset + 0u] = static_cast<uint8_t>(std::round(std::clamp(color.x, 0.0f, 1.0f) * 255.0f));
                    result.rgba.pixels[offset + 1u] = static_cast<uint8_t>(std::round(std::clamp(color.y, 0.0f, 1.0f) * 255.0f));
                    result.rgba.pixels[offset + 2u] = static_cast<uint8_t>(std::round(std::clamp(color.z, 0.0f, 1.0f) * 255.0f));
                }
                result.rgba.pixels[offset + 3u] = 255u;
            }
        }
        if (!EncodePngRgba8(result.rgba, result.base64Png))
            throw std::runtime_error("Review PNG encoding failed.");
        result.ok = true;
        result.diagnostics = "Non-destructive review view captured.";
    }
    catch (const std::exception& error)
    {
        result.diagnostics = error.what();
    }
    if (uninitializeCom) CoUninitialize();
    return result;
}

void D3D12PathTracingBackend::PollMcpReviewCapture()
{
    std::optional<McpReviewCaptureResult> completed;
    if (m_mcpReviewCaptureFuture.valid() &&
        m_mcpReviewCaptureFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        completed = m_mcpReviewCaptureFuture.get();
    }
    else if (m_pendingMcpReviewReadback && m_fence &&
        m_fence->GetCompletedValue() >= m_pendingMcpReviewReadback->fenceValue)
    {
        PendingMcpReviewReadback pending = std::move(*m_pendingMcpReviewReadback);
        m_pendingMcpReviewReadback.reset();
        try
        {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(pending.totalBytes) };
            ThrowIfFailed(pending.resource->Map(0, &readRange, &mapped));
            const size_t tightPitch = static_cast<size_t>(pending.rowBytes);
            std::vector<uint8_t> sourcePixels(tightPitch * pending.rows);
            const uint8_t* mappedBytes = static_cast<const uint8_t*>(mapped) + pending.layout.Offset;
            for (UINT row = 0; row < pending.rows; ++row)
            {
                memcpy(sourcePixels.data() + row * tightPitch,
                    mappedBytes + row * pending.layout.Footprint.RowPitch, tightPitch);
            }
            const D3D12_RANGE writtenRange{ 0, 0 };
            pending.resource->Unmap(0, &writtenRange);
            m_mcpReviewCaptureFuture = std::async(std::launch::async,
                [reviewId = pending.reviewId,
                    debugView = pending.debugView,
                    visualization = pending.visualization,
                    sourceDesc = pending.sourceDesc,
                    rowBytes = pending.rowBytes,
                    sceneFingerprint = std::move(pending.sceneFingerprint),
                    cameraFingerprint = std::move(pending.cameraFingerprint),
                    materialFingerprint = std::move(pending.materialFingerprint),
                    lightingFingerprint = std::move(pending.lightingFingerprint),
                    backendFingerprint = std::move(pending.backendFingerprint),
                    pixels = std::move(sourcePixels)]() mutable
                {
                    McpReviewCaptureResult result = EncodeMcpReviewCapture(reviewId, debugView, visualization,
                        sourceDesc, rowBytes, std::move(pixels));
                    result.sceneFingerprint = std::move(sceneFingerprint);
                    result.cameraFingerprint = std::move(cameraFingerprint);
                    result.materialFingerprint = std::move(materialFingerprint);
                    result.lightingFingerprint = std::move(lightingFingerprint);
                    result.backendFingerprint = std::move(backendFingerprint);
                    return result;
                });
        }
        catch (const std::exception& error)
        {
            McpReviewCaptureResult failed;
            failed.reviewId = pending.reviewId;
            failed.debugView = pending.debugView;
            failed.diagnostics = error.what();
            completed = std::move(failed);
        }
    }

    if (!completed) return;
    bool publish = false;
    {
        std::lock_guard<std::mutex> lock(m_mcpSnapshotMutex);
        auto iterator = std::find_if(m_mcpReviews.begin(), m_mcpReviews.end(),
            [&](const McpReview& review) { return review.id == completed->reviewId; });
        if (iterator == m_mcpReviews.end() || m_activeMcpReviewId != completed->reviewId) return;
        McpReview& review = *iterator;
        if (!completed->ok)
        {
            review.state = "failed";
            review.stage = "failed";
            review.errorCode = "capture_failed";
            review.diagnostics = completed->diagnostics;
            for (const uint64_t captureId : review.captureIds)
                for (auto& capture : m_mcpCaptures)
                    if (capture.id == captureId && capture.pinCount != 0u) --capture.pinCount;
            if (review.baselineCaptureId != 0)
                for (auto& capture : m_mcpCaptures)
                    if (capture.id == review.baselineCaptureId && capture.pinCount != 0u) --capture.pinCount;
            m_activeMcpReviewId = 0;
        }
        else
        {
            McpCapture capture;
            capture.id = m_nextMcpCaptureId++;
            capture.debugView = completed->debugView;
            capture.label = DebugViewLabels[completed->debugView];
            capture.base64Png = std::move(completed->base64Png);
            capture.rgba = std::move(completed->rgba);
            capture.sceneFingerprint = std::move(completed->sceneFingerprint);
            capture.cameraFingerprint = std::move(completed->cameraFingerprint);
            capture.materialFingerprint = std::move(completed->materialFingerprint);
            capture.lightingFingerprint = std::move(completed->lightingFingerprint);
            capture.backendFingerprint = std::move(completed->backendFingerprint);
            capture.artifactBytes = capture.base64Png.size() * 3u / 4u + capture.rgba.pixels.size();
            capture.accessSerial = m_mcpArtifactAccessSerial++;
            capture.pinCount = 1u;
            review.captureIds.push_back(capture.id);
            ++review.nextView;
            review.stage = "capturing";
            review.progress = 0.1 + 0.8 * static_cast<double>(review.nextView) /
                static_cast<double>(review.views.size());
            m_mcpCaptures.push_back(std::move(capture));
            EnforceMcpArtifactBudget();
        }
        publish = true;
    }
    if (publish)
    {
        const std::string reviewUri = "lookdevpt://reviews/" + std::to_string(completed->reviewId);
        m_mcpServer.PublishResourceUpdates({ "lookdevpt://reviews/index", reviewUri,
            "lookdevpt://captures/index" });
    }
}

bool D3D12PathTracingBackend::CaptureTextureArtifact(
    ID3D12Resource* texture,
    const std::filesystem::path& outputPath,
    bool radianceHdr,
    std::string& diagnostics,
    lookdevpt::benchmark::ArtifactStatistics* statistics)
{
    if (!texture || !m_commandQueue || !m_device || outputPath.empty())
    {
        diagnostics = "Benchmark texture capture input is invalid.";
        return false;
    }

    try
    {
        WaitForPreviousFrame();
        const D3D12_RESOURCE_DESC sourceDesc = texture->GetDesc();
        if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.DepthOrArraySize != 1 || sourceDesc.MipLevels != 1)
        {
            diagnostics = "Benchmark texture capture supports one 2D subresource.";
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        m_device->GetCopyableFootprints(&sourceDesc, 0, 1, 0, &layout, &rowCount, &rowSizeInBytes, &totalBytes);
        if (rowCount == 0 || rowSizeInBytes == 0 || totalBytes == 0)
        {
            diagnostics = "Benchmark texture has no copyable footprint.";
            return false;
        }

        ComPtr<ID3D12Resource> readback;
        const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readback)));

        FrameContext& frameContext = m_frameContexts[m_frameIndex];
        ThrowIfFailed(frameContext.commandAllocator->Reset());
        ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));
        auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &toCopySource);
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        auto backToUav = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &backToUav);
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* commandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
        WaitForPreviousFrame();

        void* mapped = nullptr;
        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
        ThrowIfFailed(readback->Map(0, &readRange, &mapped));
        const uint8_t* sourceBytes = static_cast<const uint8_t*>(mapped) + layout.Offset;
        const size_t width = static_cast<size_t>(sourceDesc.Width);
        const size_t height = static_cast<size_t>(sourceDesc.Height);
        const size_t tightRowPitch = static_cast<size_t>(rowSizeInBytes);
        std::vector<uint8_t> pixels(tightRowPitch * height);
        for (size_t row = 0; row < height; ++row)
        {
            memcpy(pixels.data() + row * tightRowPitch, sourceBytes + row * layout.Footprint.RowPitch, tightRowPitch);
        }
        const D3D12_RANGE writtenRange = { 0, 0 };
        readback->Unmap(0, &writtenRange);

        std::error_code directoryError;
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path(), directoryError);
        }
        if (directoryError)
        {
            diagnostics = "Could not create benchmark artifact directory: " + directoryError.message();
            return false;
        }

        DirectX::Image image = {};
        image.width = width;
        image.height = height;
        image.format = sourceDesc.Format;
        image.rowPitch = tightRowPitch;
        image.slicePitch = pixels.size();
        image.pixels = pixels.data();
        if (statistics && !ComputeArtifactStatistics(image, *statistics, diagnostics))
        {
            return false;
        }
        if (radianceHdr)
        {
            DirectX::ScratchImage converted;
            const DirectX::Image* convertedImage = &image;
            if (image.format != DXGI_FORMAT_R32G32B32A32_FLOAT)
            {
                ThrowIfFailed(DirectX::Convert(
                    image,
                    DXGI_FORMAT_R32G32B32A32_FLOAT,
                    DirectX::TEX_FILTER_DEFAULT,
                    0.0f,
                    converted));
                convertedImage = converted.GetImage(0, 0, 0);
            }
            if (!convertedImage)
            {
                diagnostics = "Could not convert benchmark HDR artifact.";
                return false;
            }

            // Radiance RGBE cannot represent NaN/Inf or negative radiance.
            // Preserve their counts in ArtifactStatistics, but sanitize only
            // the encoded diagnostic file so a failed quality gate still
            // produces inspectable benchmark output.
            DirectX::ScratchImage sanitized;
            ThrowIfFailed(sanitized.Initialize2D(
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                convertedImage->width,
                convertedImage->height,
                1,
                1));
            const DirectX::Image* sanitizedImage = sanitized.GetImage(0, 0, 0);
            if (!sanitizedImage)
            {
                diagnostics = "Could not allocate sanitized benchmark HDR artifact.";
                return false;
            }
            for (size_t y = 0; y < convertedImage->height; ++y)
            {
                const float* sourceRow = reinterpret_cast<const float*>(convertedImage->pixels + y * convertedImage->rowPitch);
                float* destinationRow = reinterpret_cast<float*>(sanitizedImage->pixels + y * sanitizedImage->rowPitch);
                for (size_t x = 0; x < convertedImage->width; ++x)
                {
                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        const float value = sourceRow[x * 4 + channel];
                        destinationRow[x * 4 + channel] = std::isfinite(value) && value > 0.0f ? value : 0.0f;
                    }
                    destinationRow[x * 4 + 3] = 1.0f;
                }
            }
            ThrowIfFailed(DirectX::SaveToHDRFile(*sanitizedImage, outputPath.c_str()));
        }
        else
        {
            ThrowIfFailed(DirectX::SaveToDDSFile(image, DirectX::DDS_FLAGS_NONE, outputPath.c_str()));
        }

        diagnostics = "Benchmark texture artifact saved.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = "Could not capture '" + WideToUtf8(outputPath.wstring()) + "': " + ex.what();
        LogDiagnostic(diagnostics);
        return false;
    }
}

bool D3D12PathTracingBackend::SaveBenchmarkArtifactSet(
    const std::filesystem::path& relativeDirectory,
    const std::filesystem::path& ldrFileName,
    const std::filesystem::path& hdrFileName,
    lookdevpt::benchmark::ArtifactPhase phase,
    std::uint32_t frameIndex,
    bool includeAovs,
    std::string& diagnostics)
{
    if (!m_benchmarkHarness)
    {
        diagnostics = "Benchmark harness is not active.";
        return false;
    }

    const std::filesystem::path outputDirectory = m_benchmarkHarness->GetOptions().outputDirectory;
    const auto registerArtifact = [&](
        const std::filesystem::path& relativePath,
        const char* role,
        const char* encoding,
        ID3D12Resource* resource,
        const lookdevpt::benchmark::ArtifactStatistics& statistics) -> bool
    {
        if (!resource)
        {
            diagnostics = "Benchmark artifact resource is not available.";
            return false;
        }
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        if (desc.Width > std::numeric_limits<std::uint32_t>::max())
        {
            diagnostics = "Benchmark artifact width exceeds the manifest format.";
            return false;
        }
        lookdevpt::benchmark::ArtifactRecord artifact;
        artifact.relativePath = relativePath;
        artifact.role = role;
        artifact.encoding = encoding;
        artifact.sourceFormat = BenchmarkFormatName(desc.Format);
        artifact.width = static_cast<std::uint32_t>(desc.Width);
        artifact.height = desc.Height;
        artifact.phase = phase;
        artifact.frameIndex = phase == lookdevpt::benchmark::ArtifactPhase::Final
            ? lookdevpt::benchmark::InvalidMeasuredFrameIndex
            : frameIndex;
        artifact.measuredFrameIndex = phase == lookdevpt::benchmark::ArtifactPhase::Final
            ? lookdevpt::benchmark::InvalidMeasuredFrameIndex
            : frameIndex - m_benchmarkHarness->GetOptions().warmup;
        artifact.statistics = statistics;
        return m_benchmarkHarness->RegisterArtifact(artifact, diagnostics);
    };

    const bool capturesFinalTaa =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        (m_qualitySettings.finalTaa ||
            UsesTemporalUpscale() ||
            (IsDlssSelected() &&
                m_dlssBackendRuntime.Status().lastEvaluationSucceeded)) &&
        (m_debugViewMode < 41 || m_debugViewMode >= 47);
    ID3D12Resource* finalHdrSource = m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill
        ? m_accumulationOutput.Get()
        : (capturesFinalTaa
            ? FinalResolvedHdrResource(LastSubmittedSurfaceGuideParity())
            : m_postDenoiseHdr.Get());
    if (!finalHdrSource)
    {
        diagnostics = "Benchmark HDR source is not available.";
        return false;
    }

    std::string ignoredBase64;
    const std::filesystem::path ldrRelativePath = relativeDirectory / ldrFileName;
    lookdevpt::benchmark::ArtifactStatistics ldrStatistics;
    if (!CaptureViewportPng(
            ignoredBase64,
            diagnostics,
            outputDirectory / ldrRelativePath,
            &ldrStatistics) ||
        !registerArtifact(ldrRelativePath, "beauty_ldr", "png", m_PathtracingOutput.Get(), ldrStatistics))
    {
        return false;
    }

    const std::filesystem::path hdrRelativePath = relativeDirectory / hdrFileName;
    lookdevpt::benchmark::ArtifactStatistics hdrStatistics;
    if (!CaptureTextureArtifact(
            finalHdrSource,
            outputDirectory / hdrRelativePath,
            true,
            diagnostics,
            &hdrStatistics) ||
        !registerArtifact(hdrRelativePath, "beauty_hdr", "radiance_hdr", finalHdrSource, hdrStatistics))
    {
        return false;
    }

    if (includeAovs)
    {
        const UINT capturedCurrentGuideParity = LastSubmittedSurfaceGuideParity();
        const UINT capturedPreviousGuideParity = capturedCurrentGuideParity ^ 1u;
        struct TextureArtifact
        {
            ID3D12Resource* resource;
            const char* fileName;
            const char* role;
        };
        const TextureArtifact aovs[] =
        {
            { SurfaceGuideAovResource(0u, capturedCurrentGuideParity), "surface_normal_viewz.dds", "surface_normal_viewz" },
            { SurfaceGuideAovResource(1u, capturedCurrentGuideParity), "surface_albedo_roughness.dds", "surface_albedo_roughness" },
            { SurfaceGuideAovResource(2u, capturedCurrentGuideParity), "surface_motion_hit.dds", "surface_motion_hit" },
            { SurfaceGuideIdentityResource(capturedCurrentGuideParity), "surface_identity_coverage.dds", "surface_identity_coverage" },
            { SurfaceGuideIdentityResource(capturedPreviousGuideParity), "surface_identity_coverage_previous.dds", "surface_identity_coverage_previous" },
            { m_signalDirect.Get(), "lighting_diffuse.dds", "lighting_diffuse" },
            { m_signalIndirect.Get(), "lighting_specular.dds", "lighting_specular" },
            { m_signalResidual.Get(), "lighting_emission_sky.dds", "lighting_emission_sky" },
            { m_nrdDiffuseConfidence.Get(), "lighting_diffuse_confidence.dds", "lighting_diffuse_confidence" },
            { m_nrdSpecularConfidence.Get(), "lighting_specular_confidence.dds", "lighting_specular_confidence" },
        };
        for (const TextureArtifact& aov : aovs)
        {
            const std::filesystem::path relativePath = relativeDirectory / aov.fileName;
            lookdevpt::benchmark::ArtifactStatistics statistics;
            if (!CaptureTextureArtifact(
                    aov.resource,
                    outputDirectory / relativePath,
                    false,
                    diagnostics,
                    &statistics) ||
                !registerArtifact(relativePath, aov.role, "dds", aov.resource, statistics))
            {
                return false;
            }
        }
    }

    diagnostics = includeAovs
        ? "Benchmark LDR, HDR, SurfaceGuides, and lighting AOV artifact set saved."
        : "Benchmark LDR and HDR artifact set saved.";
    return true;
}

bool D3D12PathTracingBackend::SaveBenchmarkArtifacts(std::string& diagnostics)
{
    return SaveBenchmarkArtifactSet(
        {},
        "final_ldr.png",
        "final_hdr.hdr",
        lookdevpt::benchmark::ArtifactPhase::Final,
        lookdevpt::benchmark::InvalidMeasuredFrameIndex,
        true,
        diagnostics);
}

bool D3D12PathTracingBackend::SaveBenchmarkFrameArtifacts(std::uint32_t frameIndex, std::string& diagnostics)
{
    if (!m_benchmarkHarness || !m_benchmarkHarness->ShouldCaptureFrame(frameIndex))
    {
        diagnostics = "Benchmark frame is not scheduled for artifact capture.";
        return false;
    }
    const std::uint32_t measuredFrameIndex = frameIndex - m_benchmarkHarness->GetOptions().warmup;
    std::ostringstream frameDirectory;
    frameDirectory << std::setfill('0') << std::setw(6) << measuredFrameIndex;
    return SaveBenchmarkArtifactSet(
        std::filesystem::path("frames") / frameDirectory.str(),
        "beauty_ldr.png",
        "beauty_hdr.hdr",
        lookdevpt::benchmark::ArtifactPhase::MeasuredFrame,
        frameIndex,
        m_benchmarkHarness->GetOptions().captureAovs,
        diagnostics);
}

bool D3D12PathTracingBackend::RenderPathTracingOutputForCapture(std::string& diagnostics)
{
    if (!m_PathtracingOutput || !m_commandQueue || !m_device || !m_frameContexts[m_frameIndex].commandAllocator || !m_commandList)
    {
        diagnostics = "Path tracing output is not ready.";
        return false;
    }

    try
    {
        WaitForPreviousFrame();
        UpdateConstantBuffer(0.0f);
        FrameContext& frameContext = m_frameContexts[m_frameIndex];
        ThrowIfFailed(frameContext.commandAllocator->Reset());
        ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));
        ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
        m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        DispatchRays();
        RunRestirReusePass();
        RunRestirGiPass();
        RunRestirPtPass();
        RunDenoisePass();
        RunFinalTaaPass();
        SealSurfaceGuideFrame();
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* commandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
        WaitForPreviousFrame();
        ++m_frameCounter;
        diagnostics = "Path tracing output rendered for capture.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

void D3D12PathTracingBackend::CreateDescriptorHeap()
{
    // Material textures are followed by the environment radiance texture and
    // its luminance*sin(theta) alias table. The alternate output table is kept
    // after every scene descriptor so all legacy descriptor indices remain
    // stable and no shader-visible descriptor is rewritten while in flight.
    m_alternateOutputTableBase =
        DescriptorTextureBase + static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount + 2u;
    m_descriptorCount = m_alternateOutputTableBase + OutputTableDescriptorCount;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = m_descriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap)));
}

void D3D12PathTracingBackend::CreateGpuTimingResources()
{
    m_gpuTimingSupported = false;
    m_gpuTimingValid = false;
    m_gpuTimestampFrequency = 0;
    m_gpuTimestampQueryHeap.Reset();
    for (FrameContext& frameContext : m_frameContexts)
    {
        frameContext.gpuTimestampReadback.Reset();
        frameContext.qualityCounterReadback.Reset();
        frameContext.gpuTimingPending = false;
        frameContext.qualityCountersPending = false;
    }

    if (!m_device || !m_commandQueue)
    {
        return;
    }

    if (FAILED(m_commandQueue->GetTimestampFrequency(&m_gpuTimestampFrequency)) || m_gpuTimestampFrequency == 0)
    {
        LogDiagnostic("GPU timestamp queries are not supported by this command queue.");
        return;
    }

    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryHeapDesc.Count = GpuTimestampCount * FrameCount;
    ThrowIfFailed(m_device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&m_gpuTimestampQueryHeap)));
    m_gpuTimestampQueryHeap->SetName(L"GpuTimestampQueryHeap");

    auto readbackHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * GpuTimestampCount);
    for (UINT frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameContext& frameContext = m_frameContexts[frameIndex];
        ThrowIfFailed(m_device->CreateCommittedResource(
            &readbackHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&frameContext.gpuTimestampReadback)));
        frameContext.gpuTimestampReadback->SetName((L"GpuTimestampReadback " + std::to_wstring(frameIndex)).c_str());

        if (m_benchmarkHarness &&
            lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind))
        {
            m_qualityCounterTileCountX = (m_width + 15u) / 16u;
            m_qualityCounterTileCountY = (m_height + 15u) / 16u;
            m_qualityCounterTileCount = (std::max)(1u, m_qualityCounterTileCountX * m_qualityCounterTileCountY);
            m_qualityCounterBufferSize = static_cast<UINT64>(m_qualityCounterTileCount) *
                sizeof(lookdevpt::benchmark::QualityCounterTileV1);
            auto qualityReadbackDesc = CD3DX12_RESOURCE_DESC::Buffer(m_qualityCounterBufferSize);
            ThrowIfFailed(m_device->CreateCommittedResource(
                &readbackHeapProps,
                D3D12_HEAP_FLAG_NONE,
                &qualityReadbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&frameContext.qualityCounterReadback)));
            frameContext.qualityCounterReadback->SetName(
                (L"GpuQualityCounterReadback " + std::to_wstring(frameIndex)).c_str());
        }
    }
    m_gpuTimingSupported = true;
}

void D3D12PathTracingBackend::CreateOutputResources()
{
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto createDefaultTexture = [&](const D3D12_RESOURCE_DESC& desc, ID3D12Resource** resource)
    {
        ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(resource)));
    };

    D3D12_RESOURCE_DESC outputDesc = CD3DX12_RESOURCE_DESC::Tex2D(BackBufferFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(outputDesc, &m_PathtracingOutput);
    m_PathtracingOutput->SetName(L"PathtracingOutput");

    const DXGI_FORMAT accumulationFormat = m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill
        ? ReferenceAccumulationFormat
        : SignalFormat;
    D3D12_RESOURCE_DESC accumulationDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        accumulationFormat,
        m_renderWidth,
        m_renderHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const bool referenceOnlyResources = m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill;
    const bool dlssResourceSet =
        !referenceOnlyResources &&
        IsDlssSelected() &&
        m_dlssBackendRuntime.CanEvaluateRayReconstruction();
    const bool finalTaaResourceSet =
        !referenceOnlyResources &&
        (m_qualitySettings.finalTaa || UsesTemporalUpscale() || dlssResourceSet);
    // Reference output does not need temporal guides during ordinary use, but
    // deterministic benchmark captures promise full-resolution AOVs from the
    // same submitted frame. Keep only the guides/signals full in that case;
    // all temporal denoiser histories remain 1x1 placeholders.
    const bool benchmarkAovCapture = m_benchmarkHarness != nullptr;
    const bool fullGuideResources = !referenceOnlyResources || benchmarkAovCapture;
    const UINT guideWidth = fullGuideResources ? m_renderWidth : 1u;
    const UINT guideHeight = fullGuideResources ? m_renderHeight : 1u;
    const bool nrdResourceSet = !referenceOnlyResources && IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate();
    const bool nativeExternalDenoiser = nrdResourceSet ||
        (!referenceOnlyResources && IsDlssSelected() && m_dlssBackendRuntime.CanEvaluateRayReconstruction());
    const bool internalResourceSet = !referenceOnlyResources &&
        m_denoiseBackend != DenoiseBackend::Off && !nativeExternalDenoiser;
    const UINT internalWidth = internalResourceSet ? guideWidth : 1u;
    const UINT internalHeight = internalResourceSet ? guideHeight : 1u;
    const UINT nrdGuideWidth = nrdResourceSet ? guideWidth : 1u;
    const UINT nrdGuideHeight = nrdResourceSet ? guideHeight : 1u;
    // The four NRD radiance resources are also the immutable A/B diffuse and
    // specular histories of the internal fallback. The two backends are
    // mutually exclusive, so this preserves lobe separation without adding
    // another full-resolution allocation set.
    const bool lobeRadianceResourceSet = nrdResourceSet || internalResourceSet;
    const UINT lobeWidth = lobeRadianceResourceSet ? guideWidth : 1u;
    const UINT lobeHeight = lobeRadianceResourceSet ? guideHeight : 1u;
    D3D12_RESOURCE_DESC signalDesc = CD3DX12_RESOURCE_DESC::Tex2D(SignalFormat, guideWidth, guideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // Combined radiance is reconstructed from diffuse + specular + residual.
    // Keep u17 descriptor-valid for shader ABI compatibility without paying
    // for a redundant full-resolution copy of the same estimator.
    D3D12_RESOURCE_DESC combinedSignalPlaceholderDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        SignalFormat, 1u, 1u, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC internalHistoryDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        SignalFormat, internalWidth, internalHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC internalHistoryControlsDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM, internalWidth, internalHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC identityDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_UINT, guideWidth, guideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const bool qualityDiagnosticsEnabled = m_benchmarkHarness &&
        lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind);
    const UINT qualityWidth = qualityDiagnosticsEnabled ? m_renderWidth : 1u;
    const UINT qualityHeight = qualityDiagnosticsEnabled ? m_renderHeight : 1u;
    D3D12_RESOURCE_DESC qualityContributionDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_UINT, qualityWidth, qualityHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_accumulationAliasesTaaHistory =
        finalTaaResourceSet && !UsesTemporalUpscale();
    if (m_accumulationAliasesTaaHistory)
    {
        // The interactive accumulation target is frame-local. ReSTIR and the
        // denoisers consume it before Final TAA, so the next TAA history can
        // occupy the same physical texture and overwrite it at the end of the
        // frame. Descriptor-table parity selects B on even frames and A on odd.
        createDefaultTexture(signalDesc, &m_taaHistoryA);
        m_taaHistoryA->SetName(L"TAA History / Frame Accumulation A");
        createDefaultTexture(signalDesc, &m_taaHistoryB);
        m_taaHistoryB->SetName(L"TAA History / Frame Accumulation B");
        m_accumulationOutput = m_taaHistoryB;
    }
    else
    {
        createDefaultTexture(accumulationDesc, &m_accumulationOutput);
        m_accumulationOutput->SetName(L"PathtracingAccumulation");
        D3D12_RESOURCE_DESC taaDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            SignalFormat,
            finalTaaResourceSet ? m_width : 1u,
            finalTaaResourceSet ? m_height : 1u,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        createDefaultTexture(taaDesc, &m_taaHistoryA);
        m_taaHistoryA->SetName(finalTaaResourceSet
            ? L"TAAU History A"
            : L"TAA History A Placeholder");
        createDefaultTexture(taaDesc, &m_taaHistoryB);
        m_taaHistoryB->SetName(finalTaaResourceSet
            ? L"TAAU History B"
            : L"TAA History B Placeholder");
    }
    createDefaultTexture(signalDesc, &m_denoiseAov0);
    m_denoiseAov0->SetName(L"SurfaceGuide A Normal ViewZ");
    createDefaultTexture(signalDesc, &m_denoiseAov1);
    m_denoiseAov1->SetName(L"SurfaceGuide A Albedo Roughness");
    createDefaultTexture(signalDesc, &m_denoiseAov2);
    m_denoiseAov2->SetName(L"SurfaceGuide A Motion Hit");
    createDefaultTexture(internalHistoryDesc, &m_reconstructionHistoryRadiance);
    m_reconstructionHistoryRadiance->SetName(L"PathtracingReconstructionHistoryRadiance");
    createDefaultTexture(internalHistoryDesc, &m_reconstructionHistoryMoments);
    m_reconstructionHistoryMoments->SetName(L"PathtracingReconstructionHistoryMoments");
    createDefaultTexture(internalHistoryControlsDesc, &m_reconstructionHistoryLength);
    m_reconstructionHistoryLength->SetName(L"PathtracingReconstructionHistoryLength");
    createDefaultTexture(internalHistoryDesc, &m_reconstructionHistoryRadianceB);
    m_reconstructionHistoryRadianceB->SetName(L"PathtracingReconstructionHistoryRadianceB");
    createDefaultTexture(internalHistoryDesc, &m_reconstructionHistoryMomentsB);
    m_reconstructionHistoryMomentsB->SetName(L"PathtracingReconstructionHistoryMomentsB");
    createDefaultTexture(internalHistoryControlsDesc, &m_reconstructionHistoryLengthB);
    m_reconstructionHistoryLengthB->SetName(L"PathtracingReconstructionHistoryLengthB");
    createDefaultTexture(signalDesc, &m_previousDenoiseAov0);
    m_previousDenoiseAov0->SetName(L"SurfaceGuide B Normal ViewZ");
    createDefaultTexture(signalDesc, &m_previousDenoiseAov1);
    m_previousDenoiseAov1->SetName(L"SurfaceGuide B Albedo Roughness");
    createDefaultTexture(signalDesc, &m_previousDenoiseAov2);
    m_previousDenoiseAov2->SetName(L"SurfaceGuide B Motion Hit");
    createDefaultTexture(identityDesc, &m_surfaceIdentity);
    m_surfaceIdentity->SetName(L"SurfaceGuide A Identity Coverage");
    createDefaultTexture(identityDesc, &m_previousSurfaceIdentity);
    m_previousSurfaceIdentity->SetName(L"SurfaceGuide B Identity Coverage");
    createDefaultTexture(qualityContributionDesc, &m_qualityContribution);
    m_qualityContribution->SetName(qualityDiagnosticsEnabled
        ? L"Benchmark Contribution Energy LogHalf2"
        : L"Benchmark Contribution Energy Placeholder");
    createDefaultTexture(combinedSignalPlaceholderDesc, &m_signalCurrentRadiance);
    m_signalCurrentRadiance->SetName(L"Combined Signal ABI Placeholder");
    createDefaultTexture(signalDesc, &m_signalDirect);
    m_signalDirect->SetName(L"PathtracingSignalDiffuse");
    createDefaultTexture(signalDesc, &m_signalIndirect);
    m_signalIndirect->SetName(L"PathtracingSignalSpecular");
    createDefaultTexture(signalDesc, &m_signalResidual);
    m_signalResidual->SetName(L"PathtracingSignalResidual");
    createDefaultTexture(internalHistoryDesc, &m_denoisePing);
    m_denoisePing->SetName(L"PathtracingDenoisePing");
    createDefaultTexture(internalHistoryDesc, &m_denoisePong);
    m_denoisePong->SetName(L"PathtracingDenoisePong");

    D3D12_RESOURCE_DESC nrdMotionDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, nrdGuideWidth, nrdGuideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // NRD is built with NRD_NORMAL_ENCODING=2. Match its official packed
    // normal/roughness contract instead of spending twice the bandwidth on
    // an RGBA16F guide.
    D3D12_RESOURCE_DESC nrdNormalRoughnessDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R10G10B10A2_UNORM, nrdGuideWidth, nrdGuideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC nrdViewZDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, nrdGuideWidth, nrdGuideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC nrdRadianceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, lobeWidth, lobeHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(nrdMotionDesc, &m_nrdMotion);
    m_nrdMotion->SetName(L"NRD Motion");
    createDefaultTexture(nrdNormalRoughnessDesc, &m_nrdNormalRoughness);
    m_nrdNormalRoughness->SetName(L"NRD Normal Roughness");
    createDefaultTexture(nrdViewZDesc, &m_nrdViewZ);
    m_nrdViewZ->SetName(L"NRD ViewZ");
    createDefaultTexture(nrdRadianceDesc, &m_nrdDiffRadianceHitDistance);
    m_nrdDiffRadianceHitDistance->SetName(L"NRD Diffuse Radiance HitDistance");
    createDefaultTexture(nrdRadianceDesc, &m_nrdSpecRadianceHitDistance);
    m_nrdSpecRadianceHitDistance->SetName(L"NRD Specular Radiance HitDistance");
    createDefaultTexture(nrdRadianceDesc, &m_nrdDiffDenoised);
    m_nrdDiffDenoised->SetName(L"NRD Diffuse Denoised");
    createDefaultTexture(nrdRadianceDesc, &m_nrdSpecDenoised);
    m_nrdSpecDenoised->SetName(L"NRD Specular Denoised");
    const bool fusedNrdTaaResourceSet =
        nrdResourceSet && m_denoiserEnabled &&
        (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_debugViewMode == 0 &&
        !qualityDiagnosticsEnabled;
    D3D12_RESOURCE_DESC postDenoiseHdrDesc = fusedNrdTaaResourceSet
        ? combinedSignalPlaceholderDesc
        : signalDesc;
    createDefaultTexture(postDenoiseHdrDesc, &m_postDenoiseHdr);
    m_postDenoiseHdr->SetName(fusedNrdTaaResourceSet
        ? L"Post Denoise HDR Fused Placeholder"
        : L"Post Denoise HDR");
    if (finalTaaResourceSet)
    {
        // Base-table parity uses B; the alternate table overrides both the
        // accumulation and final-resolve UAVs with A below.
        m_finalResolvedHdr = m_taaHistoryB;
    }
    else
    {
        D3D12_RESOURCE_DESC finalResolvedHdrDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            SignalFormat, 1u, 1u, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        createDefaultTexture(finalResolvedHdrDesc, &m_finalResolvedHdr);
        m_finalResolvedHdr->SetName(L"Final Resolved HDR Placeholder");
    }
    D3D12_RESOURCE_DESC nrdConfidenceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8_UNORM, guideWidth, guideHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(nrdConfidenceDesc, &m_nrdDiffuseConfidence);
    m_nrdDiffuseConfidence->SetName(L"NRD Diffuse History Confidence");
    createDefaultTexture(nrdConfidenceDesc, &m_nrdSpecularConfidence);
    m_nrdSpecularConfidence->SetName(L"NRD Specular History Confidence");

    const UINT dlssGuideWidth = dlssResourceSet ? guideWidth : 1u;
    const UINT dlssGuideHeight = dlssResourceSet ? guideHeight : 1u;
    D3D12_RESOURCE_DESC dlssDepthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_FLOAT,
        dlssGuideWidth,
        dlssGuideHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC dlssMotionDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16_FLOAT,
        dlssGuideWidth,
        dlssGuideHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC dlssGuideDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        dlssGuideWidth,
        dlssGuideHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC dlssExposureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32_FLOAT,
        1u,
        1u,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(dlssDepthDesc, &m_dlssDepth);
    m_dlssDepth->SetName(dlssResourceSet ? L"DLSS-RR Linear Depth" : L"DLSS-RR Depth Placeholder");
    createDefaultTexture(dlssMotionDesc, &m_dlssMotion);
    m_dlssMotion->SetName(dlssResourceSet ? L"DLSS-RR Motion" : L"DLSS-RR Motion Placeholder");
    createDefaultTexture(dlssGuideDesc, &m_dlssNormalRoughness);
    m_dlssNormalRoughness->SetName(dlssResourceSet
        ? L"DLSS-RR Normal Roughness"
        : L"DLSS-RR Normal Roughness Placeholder");
    createDefaultTexture(dlssGuideDesc, &m_dlssAlbedo);
    m_dlssAlbedo->SetName(dlssResourceSet ? L"DLSS-RR Albedo" : L"DLSS-RR Albedo Placeholder");
    createDefaultTexture(dlssGuideDesc, &m_dlssSpecularAlbedo);
    m_dlssSpecularAlbedo->SetName(dlssResourceSet
        ? L"DLSS-RR Specular Albedo"
        : L"DLSS-RR Specular Albedo Placeholder");
    createDefaultTexture(dlssExposureDesc, &m_dlssExposure);
    m_dlssExposure->SetName(L"DLSS-RR Exposure");

    const bool compactSecondaryResources = UsesCompactSecondaryWorkList();
    const UINT compactWidth = compactSecondaryResources ? m_renderWidth : 1u;
    const UINT compactHeight = compactSecondaryResources ? m_renderHeight : 1u;
    const UINT64 compactPixelCount = static_cast<UINT64>(compactWidth) * compactHeight;
    const UINT compactMaxSpp = compactSecondaryResources
        ? static_cast<UINT>((std::max)(
            (std::max)(m_giSamplesPerFrame, 1),
            m_adaptiveSamplingEnabled ? (std::max)(m_maxAdaptiveSamplesPerPixel, 1) : 1))
        : 1u;
    const UINT64 requestedTaskCapacity = compactPixelCount * compactMaxSpp;
    if (requestedTaskCapacity > UINT_MAX)
    {
        throw std::runtime_error("Compact secondary task capacity exceeds the DXR dispatch limit.");
    }
    m_secondaryTaskCapacity = static_cast<UINT>((std::max)(requestedTaskCapacity, UINT64{ 1 }));
    m_secondaryGroupCount = static_cast<UINT>((std::max)(
        (compactPixelCount + 255u) / 256u,
        UINT64{ 1 }));

    D3D12_RESOURCE_DESC primaryPositionDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        m_renderWidth,
        m_renderHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC primaryNormalDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        m_renderWidth,
        m_renderHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC primaryIdentityDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32G32B32A32_UINT,
        compactWidth,
        compactHeight,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    createDefaultTexture(primaryPositionDesc, &m_primaryPositionCone);
    m_primaryPositionCone->SetName(L"Primary Surface Position RayConeWidth");
    createDefaultTexture(primaryNormalDesc, &m_primaryGeometricNormal);
    m_primaryGeometricNormal->SetName(L"Primary Surface Incident Direction RayConeSpread");
    createDefaultTexture(primaryIdentityDesc, &m_primaryIdentity);
    m_primaryIdentity->SetName(compactSecondaryResources
        ? L"Primary Visibility Exact Identity"
        : L"Primary Visibility Identity Placeholder");
    m_secondaryTaskOffsets = CreateUavBuffer(
        compactPixelCount * sizeof(UINT),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        compactSecondaryResources
            ? L"Secondary Task Pixel Prefix"
            : L"Secondary Task Prefix Placeholder");
    m_secondaryGroupOffsets = CreateUavBuffer(
        static_cast<UINT64>(m_secondaryGroupCount) * sizeof(UINT),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        compactSecondaryResources
            ? L"Secondary Task Group Prefix"
            : L"Secondary Task Group Prefix Placeholder");
    m_secondaryTasks = CreateUavBuffer(
        static_cast<UINT64>(m_secondaryTaskCapacity) * 8u,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        compactSecondaryResources
            ? L"Compact Secondary Tasks"
            : L"Secondary Task Placeholder");
    m_secondaryResults = CreateUavBuffer(
        static_cast<UINT64>(m_secondaryTaskCapacity) * 32u,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        compactSecondaryResources
            ? L"Compact Secondary Results"
            : L"Secondary Result Placeholder");
    m_secondaryIndirectArgs = CreateUavBuffer(
        Align(sizeof(D3D12_DISPATCH_RAYS_DESC), D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        compactSecondaryResources
            ? L"Compact Secondary DispatchRays Arguments"
            : L"Secondary DispatchRays Arguments Placeholder");
    m_reviewProbeBuffer = CreateUavBuffer(
        16u * 256u,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"MCP Review Surface Probe Records");

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = {};
    outputUav.Format = BackBufferFormat;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_PathtracingOutput.Get(), nullptr, &outputUav, CpuDescriptor(DescriptorOutputUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC accumulationUav = {};
    accumulationUav.Format = accumulationFormat;
    accumulationUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_accumulationOutput.Get(), nullptr, &accumulationUav, CpuDescriptor(DescriptorAccumulationUav));
    D3D12_UNORDERED_ACCESS_VIEW_DESC signalUav = accumulationUav;
    signalUav.Format = SignalFormat;
    m_device->CreateUnorderedAccessView(m_denoiseAov0.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(m_denoiseAov1.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(m_denoiseAov2.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorDenoiseAov2Uav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryRadiance.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorReconstructionHistoryRadianceUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryMoments.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorReconstructionHistoryMomentsUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryRadianceB.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorReconstructionHistoryRadianceBUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryMomentsB.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorReconstructionHistoryMomentsBUav));
    D3D12_UNORDERED_ACCESS_VIEW_DESC historyControlsUav = signalUav;
    historyControlsUav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryLength.Get(), nullptr, &historyControlsUav, CpuDescriptor(DescriptorReconstructionHistoryLengthUav));
    m_device->CreateUnorderedAccessView(m_reconstructionHistoryLengthB.Get(), nullptr, &historyControlsUav, CpuDescriptor(DescriptorReconstructionHistoryLengthBUav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov0.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorPreviousDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov1.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorPreviousDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(m_previousDenoiseAov2.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorPreviousDenoiseAov2Uav));
    m_device->CreateUnorderedAccessView(m_signalCurrentRadiance.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorSignalCurrentRadianceUav));
    m_device->CreateUnorderedAccessView(m_signalDirect.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorSignalDirectUav));
    m_device->CreateUnorderedAccessView(m_signalIndirect.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorSignalIndirectUav));
    m_device->CreateUnorderedAccessView(m_signalResidual.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorSignalResidualUav));
    m_device->CreateUnorderedAccessView(m_denoisePing.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorDenoisePingUav));
    m_device->CreateUnorderedAccessView(m_denoisePong.Get(), nullptr, &signalUav, CpuDescriptor(DescriptorDenoisePongUav));

    auto createTextureUav = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(resource, nullptr, &uav, CpuDescriptor(slot));
    };
    createTextureUav(m_nrdMotion.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorNrdMotionUav);
    createTextureUav(m_nrdNormalRoughness.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, DescriptorNrdNormalRoughnessUav);
    createTextureUav(m_nrdViewZ.Get(), DXGI_FORMAT_R32_FLOAT, DescriptorNrdViewZUav);
    createTextureUav(m_nrdDiffRadianceHitDistance.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorNrdDiffRadianceHitDistanceUav);
    createTextureUav(m_nrdSpecRadianceHitDistance.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorNrdSpecRadianceHitDistanceUav);
    createTextureUav(m_nrdDiffDenoised.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorNrdDiffDenoisedUav);
    createTextureUav(m_nrdSpecDenoised.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorNrdSpecDenoisedUav);
    createTextureUav(m_postDenoiseHdr.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorPostDenoiseHdrUav);
    createTextureUav(m_taaHistoryA.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorTaaHistoryAUav);
    createTextureUav(m_taaHistoryB.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorTaaHistoryBUav);
    createTextureUav(m_nrdDiffuseConfidence.Get(), DXGI_FORMAT_R8_UNORM, DescriptorNrdDiffuseConfidenceUav);
    createTextureUav(m_nrdSpecularConfidence.Get(), DXGI_FORMAT_R8_UNORM, DescriptorNrdSpecularConfidenceUav);
    createTextureUav(m_surfaceIdentity.Get(), DXGI_FORMAT_R32_UINT, DescriptorSurfaceIdentityUav);
    createTextureUav(m_previousSurfaceIdentity.Get(), DXGI_FORMAT_R32_UINT, DescriptorPreviousSurfaceIdentityUav);
    createTextureUav(m_finalResolvedHdr.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorFinalResolvedHdrUav);
    createTextureUav(m_qualityContribution.Get(), DXGI_FORMAT_R32_UINT, DescriptorQualityContributionUav);
    createTextureUav(m_dlssDepth.Get(), DXGI_FORMAT_R32_FLOAT, DescriptorDlssDepthUav);
    createTextureUav(m_dlssMotion.Get(), DXGI_FORMAT_R16G16_FLOAT, DescriptorDlssMotionUav);
    createTextureUav(
        m_dlssNormalRoughness.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DescriptorDlssNormalRoughnessUav);
    createTextureUav(m_dlssAlbedo.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, DescriptorDlssAlbedoUav);
    createTextureUav(
        m_dlssSpecularAlbedo.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DescriptorDlssSpecularAlbedoUav);
    createTextureUav(m_dlssExposure.Get(), DXGI_FORMAT_R32_FLOAT, DescriptorDlssExposureUav);
    createTextureUav(
        m_primaryPositionCone.Get(),
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DescriptorPrimaryPositionConeUav);
    createTextureUav(
        m_primaryGeometricNormal.Get(),
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DescriptorPrimaryGeometricNormalUav);
    createTextureUav(
        m_primaryIdentity.Get(),
        DXGI_FORMAT_R32G32B32A32_UINT,
        DescriptorPrimaryIdentityUav);

    auto createStructuredUav = [&](
        ID3D12Resource* resource,
        UINT elementCount,
        UINT stride,
        UINT slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = elementCount;
        uav.Buffer.StructureByteStride = stride;
        m_device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            CpuDescriptor(slot));
    };
    createStructuredUav(
        m_secondaryTaskOffsets.Get(),
        static_cast<UINT>((std::max)(compactPixelCount, UINT64{ 1 })),
        sizeof(UINT),
        DescriptorSecondaryTaskOffsetsUav);
    createStructuredUav(
        m_secondaryGroupOffsets.Get(),
        m_secondaryGroupCount,
        sizeof(UINT),
        DescriptorSecondaryGroupOffsetsUav);
    createStructuredUav(
        m_secondaryTasks.Get(),
        m_secondaryTaskCapacity,
        8u,
        DescriptorSecondaryTasksUav);
    createStructuredUav(
        m_secondaryResults.Get(),
        m_secondaryTaskCapacity,
        32u,
        DescriptorSecondaryResultsUav);
    D3D12_UNORDERED_ACCESS_VIEW_DESC indirectArgsUav{};
    indirectArgsUav.Format = DXGI_FORMAT_R32_TYPELESS;
    indirectArgsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    indirectArgsUav.Buffer.NumElements = static_cast<UINT>(
        Align(sizeof(D3D12_DISPATCH_RAYS_DESC), sizeof(UINT)) / sizeof(UINT));
    indirectArgsUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device->CreateUnorderedAccessView(
        m_secondaryIndirectArgs.Get(),
        nullptr,
        &indirectArgsUav,
        CpuDescriptor(DescriptorSecondaryIndirectArgsUav));
    D3D12_UNORDERED_ACCESS_VIEW_DESC reviewProbeUav{};
    reviewProbeUav.Format = DXGI_FORMAT_R32_TYPELESS;
    reviewProbeUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    reviewProbeUav.Buffer.NumElements = 16u * 256u / sizeof(UINT);
    reviewProbeUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device->CreateUnorderedAccessView(
        m_reviewProbeBuffer.Get(),
        nullptr,
        &reviewProbeUav,
        CpuDescriptor(DescriptorReviewProbeUav));

    m_qualityCounterTileCountX =
        qualityDiagnosticsEnabled ? (m_renderWidth + 15u) / 16u : 1u;
    m_qualityCounterTileCountY =
        qualityDiagnosticsEnabled ? (m_renderHeight + 15u) / 16u : 1u;
    m_qualityCounterTileCount = (std::max)(1u, m_qualityCounterTileCountX * m_qualityCounterTileCountY);
    m_qualityCounterBufferSize = static_cast<UINT64>(m_qualityCounterTileCount) *
        sizeof(lookdevpt::benchmark::QualityCounterTileV1);
    m_qualityCounterBuffer = CreateUavBuffer(
        m_qualityCounterBufferSize,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        qualityDiagnosticsEnabled ? L"Benchmark Quality Counter Tiles" : L"Benchmark Quality Counter Placeholder");
    D3D12_UNORDERED_ACCESS_VIEW_DESC qualityCounterUav = {};
    qualityCounterUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    qualityCounterUav.Buffer.NumElements = m_qualityCounterTileCount;
    qualityCounterUav.Buffer.StructureByteStride = sizeof(lookdevpt::benchmark::QualityCounterTileV1);
    m_device->CreateUnorderedAccessView(
        m_qualityCounterBuffer.Get(),
        nullptr,
        &qualityCounterUav,
        CpuDescriptor(DescriptorQualityCounterUav));

    // RTXDI DI history is independent from its transient candidate scratch.
    // In GI+DI mode, each parity has a placed-resource heap shared by that DI
    // scratch and the GI write target. DI completes before GI starts, while
    // the opposite heap remains immutable GI history, so their lifetimes
    // never overlap.
    const bool allocateFullRestirReservoirs =
        m_rtxdiAvailable && m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi &&
        UsesRestirDI(m_mode) && m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill;
    const bool allocateFullGiReservoirs =
        m_rtxdiAvailable && m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi &&
        UsesRestirGI(m_mode) && m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill;
    const bool allocateFullPtReservoirs =
        m_rtxdiAvailable && m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi &&
        UsesRestirPT(m_mode) && m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill;
    const bool allocateFullIndirectReservoirs =
        allocateFullGiReservoirs || allocateFullPtReservoirs;
    const bool aliasDiScratchWithIndirect =
        allocateFullRestirReservoirs && allocateFullIndirectReservoirs;
    const RtxdiReservoirLayout reservoirLayout =
        m_rtxdiBackendRuntime.CalculateReservoirLayout(m_renderWidth, m_renderHeight);
    const RtxdiReservoirLayout giReservoirLayout =
        m_rtxdiBackendRuntime.CalculateGiReservoirLayout(m_renderWidth, m_renderHeight);
    const RtxdiReservoirLayout ptReservoirLayout =
        m_rtxdiBackendRuntime.CalculatePtReservoirLayout(m_renderWidth, m_renderHeight);
    if (allocateFullRestirReservoirs && reservoirLayout.elementStride != RestirReservoirStride)
    {
        throw std::runtime_error("RTXDI packed DI reservoir ABI changed.");
    }
    if (allocateFullGiReservoirs && giReservoirLayout.elementStride != RestirGiReservoirStride)
    {
        throw std::runtime_error("RTXDI packed GI reservoir ABI changed.");
    }
    if (allocateFullPtReservoirs && ptReservoirLayout.elementStride != RestirPtReservoirStride)
    {
        throw std::runtime_error("RTXDI packed PT reservoir ABI changed.");
    }
    m_restirReservoirElementCount = allocateFullRestirReservoirs ? (std::max)(1u, reservoirLayout.arrayPitch) : 1u;
    m_restirReservoirBufferSize = static_cast<UINT64>(m_restirReservoirElementCount) * RestirReservoirStride;
    m_restirGiReservoirElementCount = allocateFullGiReservoirs
        ? (std::max)(1u, giReservoirLayout.arrayPitch)
        : 1u;
    m_restirGiReservoirBufferSize =
        static_cast<UINT64>(m_restirGiReservoirElementCount) * RestirGiReservoirStride;
    m_restirPtReservoirElementCount = allocateFullPtReservoirs
        ? (std::max)(1u, ptReservoirLayout.arrayPitch)
        : 1u;
    m_restirPtReservoirBufferSize =
        static_cast<UINT64>(m_restirPtReservoirElementCount) * RestirPtReservoirStride;

    m_restirReservoirCurrent = CreateUavBuffer(m_restirReservoirBufferSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RTXDI DI History Output A");
    m_restirReservoirHistory = m_restirReservoirCurrent;
    m_restirAliasHeapSize = 0;
    for (ComPtr<ID3D12Heap>& heap : m_restirAliasHeaps)
    {
        heap.Reset();
    }
    m_restirReservoirSpatial.Reset();
    m_restirReservoirSpatialB.Reset();
    m_restirGiReservoirA.Reset();
    m_restirGiReservoirB.Reset();
    m_restirPtReservoirA.Reset();
    m_restirPtReservoirB.Reset();

    if (aliasDiScratchWithIndirect)
    {
        const D3D12_RESOURCE_DESC diScratchDesc = CD3DX12_RESOURCE_DESC::Buffer(
            m_restirReservoirBufferSize,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC giDesc = CD3DX12_RESOURCE_DESC::Buffer(
            m_restirGiReservoirBufferSize,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC ptDesc = CD3DX12_RESOURCE_DESC::Buffer(
            m_restirPtReservoirBufferSize,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_ALLOCATION_INFO diInfo =
            m_device->GetResourceAllocationInfo(0, 1, &diScratchDesc);
        const D3D12_RESOURCE_ALLOCATION_INFO giInfo =
            m_device->GetResourceAllocationInfo(0, 1, &giDesc);
        const D3D12_RESOURCE_ALLOCATION_INFO ptInfo =
            m_device->GetResourceAllocationInfo(0, 1, &ptDesc);
        const D3D12_RESOURCE_ALLOCATION_INFO indirectInfo =
            allocateFullGiReservoirs ? giInfo : ptInfo;
        // The DI candidate/spatial passes complete before the current
        // indirect reservoir becomes active. Declare those intervals here so
        // future graph changes cannot silently overlap placed resources.
        const rb::TransientResourcePlan aliasPlan =
            rb::BuildTransientResourcePlan({
                {
                    "diScratch",
                    diInfo.SizeInBytes,
                    diInfo.Alignment,
                    0u,
                    1u,
                },
                {
                    allocateFullGiReservoirs ? "giCurrent" : "ptCurrent",
                    indirectInfo.SizeInBytes,
                    indirectInfo.Alignment,
                    2u,
                    3u,
                },
            });
        const rb::TransientResourcePlacement* diPlacement =
            aliasPlan.Find("diScratch");
        const rb::TransientResourcePlacement* indirectPlacement =
            aliasPlan.Find(
                allocateFullGiReservoirs ? "giCurrent" : "ptCurrent");
        if (!diPlacement || !indirectPlacement ||
            diPlacement->offset != indirectPlacement->offset)
        {
            throw std::runtime_error(
                "DI scratch and current indirect reservoir no longer have disjoint lifetimes.");
        }
        m_restirAliasHeapSize = aliasPlan.heapSize;
        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = m_restirAliasHeapSize;
        heapDesc.Alignment = (std::max)(diInfo.Alignment, indirectInfo.Alignment);
        heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        for (UINT parity = 0u; parity < 2u; ++parity)
        {
            ThrowIfFailed(m_device->CreateHeap(
                &heapDesc,
                IID_PPV_ARGS(&m_restirAliasHeaps[parity])));
            m_restirAliasHeaps[parity]->SetName(
                parity == 0u ? L"RTXDI Alias Heap A" : L"RTXDI Alias Heap B");
        }
        ThrowIfFailed(m_device->CreatePlacedResource(
            m_restirAliasHeaps[0].Get(), diPlacement->offset, &diScratchDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_restirReservoirSpatial)));
        ThrowIfFailed(m_device->CreatePlacedResource(
            m_restirAliasHeaps[1].Get(), diPlacement->offset, &diScratchDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_restirReservoirSpatialB)));
        m_restirReservoirSpatial->SetName(L"RTXDI DI Scratch A (aliases GI A)");
        m_restirReservoirSpatialB->SetName(L"RTXDI DI Scratch B (aliases GI B)");
        if (allocateFullGiReservoirs)
        {
            ThrowIfFailed(m_device->CreatePlacedResource(
                m_restirAliasHeaps[0].Get(), indirectPlacement->offset, &giDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_restirGiReservoirA)));
            ThrowIfFailed(m_device->CreatePlacedResource(
                m_restirAliasHeaps[1].Get(), indirectPlacement->offset, &giDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_restirGiReservoirB)));
            m_restirGiReservoirA->SetName(L"RTXDI GI Reservoir A (aliases DI Scratch A)");
            m_restirGiReservoirB->SetName(L"RTXDI GI Reservoir B (aliases DI Scratch B)");
            m_restirPtReservoirA = CreateUavBuffer(
                m_restirPtReservoirBufferSize,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"RTXDI PT Reservoir Placeholder");
            m_restirPtReservoirB = m_restirPtReservoirA;
        }
        else
        {
            ThrowIfFailed(m_device->CreatePlacedResource(
                m_restirAliasHeaps[0].Get(), indirectPlacement->offset, &ptDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_restirPtReservoirA)));
            ThrowIfFailed(m_device->CreatePlacedResource(
                m_restirAliasHeaps[1].Get(), indirectPlacement->offset, &ptDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&m_restirPtReservoirB)));
            m_restirPtReservoirA->SetName(L"RTXDI PT Reservoir A (aliases DI Scratch A)");
            m_restirPtReservoirB->SetName(L"RTXDI PT Reservoir B (aliases DI Scratch B)");
            m_restirGiReservoirA = CreateUavBuffer(
                m_restirGiReservoirBufferSize,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"RTXDI GI Reservoir Placeholder");
            m_restirGiReservoirB = m_restirGiReservoirA;
        }
    }
    else
    {
        m_restirReservoirSpatial = allocateFullRestirReservoirs
            ? CreateUavBuffer(
                m_restirReservoirBufferSize,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"RTXDI DI Candidate Temporal Scratch")
            : m_restirReservoirCurrent;
        m_restirReservoirSpatialB = m_restirReservoirSpatial;
        m_restirGiReservoirA = CreateUavBuffer(
            m_restirGiReservoirBufferSize,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            allocateFullGiReservoirs
                ? L"RTXDI GI Reservoir A"
                : L"RTXDI GI Reservoir Placeholder");
        m_restirGiReservoirB = allocateFullGiReservoirs
            ? CreateUavBuffer(
                m_restirGiReservoirBufferSize,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"RTXDI GI Reservoir B")
            : m_restirGiReservoirA;
        m_restirPtReservoirA = CreateUavBuffer(
            m_restirPtReservoirBufferSize,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            allocateFullPtReservoirs
                ? L"RTXDI PT Reservoir A"
                : L"RTXDI PT Reservoir Placeholder");
        m_restirPtReservoirB = allocateFullPtReservoirs
            ? CreateUavBuffer(
                m_restirPtReservoirBufferSize,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                L"RTXDI PT Reservoir B")
            : m_restirPtReservoirA;
    }

    m_restirDiReservoirCurrent = m_restirReservoirCurrent;
    m_restirDiReservoirHistory = m_restirReservoirHistory;
    m_restirDiReservoirSpatial = m_restirReservoirSpatial;

    D3D12_UNORDERED_ACCESS_VIEW_DESC reservoirUav = {};
    reservoirUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    reservoirUav.Buffer.NumElements = m_restirReservoirElementCount;
    reservoirUav.Buffer.StructureByteStride = RestirReservoirStride;
    m_device->CreateUnorderedAccessView(m_restirReservoirCurrent.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirCurrentUav));
    m_device->CreateUnorderedAccessView(m_restirReservoirHistory.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirHistoryUav));
    m_device->CreateUnorderedAccessView(m_restirReservoirSpatial.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirSpatialUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirCurrent.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiCurrentUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirHistory.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiHistoryUav));
    m_device->CreateUnorderedAccessView(m_restirDiReservoirSpatial.Get(), nullptr, &reservoirUav, CpuDescriptor(DescriptorRestirDiSpatialUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC giReservoirUav = {};
    giReservoirUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    giReservoirUav.Buffer.NumElements = m_restirGiReservoirElementCount;
    giReservoirUav.Buffer.StructureByteStride = RestirGiReservoirStride;
    m_device->CreateUnorderedAccessView(
        m_restirGiReservoirA.Get(), nullptr, &giReservoirUav,
        CpuDescriptor(DescriptorRestirGiCurrentUav));
    m_device->CreateUnorderedAccessView(
        m_restirGiReservoirB.Get(), nullptr, &giReservoirUav,
        CpuDescriptor(DescriptorRestirGiHistoryUav));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ptReservoirUav = {};
    ptReservoirUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ptReservoirUav.Buffer.NumElements = m_restirPtReservoirElementCount;
    ptReservoirUav.Buffer.StructureByteStride = RestirPtReservoirStride;
    m_device->CreateUnorderedAccessView(
        m_restirPtReservoirA.Get(), nullptr, &ptReservoirUav,
        CpuDescriptor(DescriptorRestirPtCurrentUav));
    m_device->CreateUnorderedAccessView(
        m_restirPtReservoirB.Get(), nullptr, &ptReservoirUav,
        CpuDescriptor(DescriptorRestirPtHistoryUav));

    // Table 0 presents physical A as current and B as previous. Clone every
    // binding (including the fixed two-buffer ReSTIR descriptors) into the odd
    // table, then swap only SurfaceGuide slots. Both tables are immutable for
    // their entire lifetime and are selected by the submitted-frame parity.
    m_device->CopyDescriptorsSimple(
        OutputTableDescriptorCount,
        CpuDescriptor(m_alternateOutputTableBase),
        CpuDescriptor(DescriptorOutputUav),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (m_accumulationAliasesTaaHistory)
    {
        m_device->CreateUnorderedAccessView(
            m_taaHistoryA.Get(), nullptr, &accumulationUav,
            CpuDescriptor(m_alternateOutputTableBase + DescriptorAccumulationUav));
    }
    if (finalTaaResourceSet)
    {
        m_device->CreateUnorderedAccessView(
            m_taaHistoryA.Get(), nullptr, &signalUav,
            CpuDescriptor(m_alternateOutputTableBase + DescriptorFinalResolvedHdrUav));
    }
    m_device->CreateUnorderedAccessView(
        m_previousDenoiseAov0.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(
        m_previousDenoiseAov1.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(
        m_previousDenoiseAov2.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorDenoiseAov2Uav));
    m_device->CreateUnorderedAccessView(
        m_denoiseAov0.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorPreviousDenoiseAov0Uav));
    m_device->CreateUnorderedAccessView(
        m_denoiseAov1.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorPreviousDenoiseAov1Uav));
    m_device->CreateUnorderedAccessView(
        m_denoiseAov2.Get(), nullptr, &signalUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorPreviousDenoiseAov2Uav));
    createTextureUav(
        m_previousSurfaceIdentity.Get(), DXGI_FORMAT_R32_UINT,
        m_alternateOutputTableBase + DescriptorSurfaceIdentityUav);
    createTextureUav(
        m_surfaceIdentity.Get(), DXGI_FORMAT_R32_UINT,
        m_alternateOutputTableBase + DescriptorPreviousSurfaceIdentityUav);
    m_device->CreateUnorderedAccessView(
        m_restirGiReservoirB.Get(), nullptr, &giReservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirGiCurrentUav));
    m_device->CreateUnorderedAccessView(
        m_restirGiReservoirA.Get(), nullptr, &giReservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirGiHistoryUav));
    m_device->CreateUnorderedAccessView(
        m_restirPtReservoirB.Get(), nullptr, &ptReservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirPtCurrentUav));
    m_device->CreateUnorderedAccessView(
        m_restirPtReservoirA.Get(), nullptr, &ptReservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirPtHistoryUav));
    m_device->CreateUnorderedAccessView(
        m_restirReservoirSpatialB.Get(), nullptr, &reservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirSpatialUav));
    m_device->CreateUnorderedAccessView(
        m_restirReservoirSpatialB.Get(), nullptr, &reservoirUav,
        CpuDescriptor(m_alternateOutputTableBase + DescriptorRestirDiSpatialUav));

    const std::array<ID3D12Resource*, 62> frameHistoryResources =
    {
        m_PathtracingOutput.Get(), m_accumulationOutput.Get(),
        m_denoiseAov0.Get(), m_denoiseAov1.Get(), m_denoiseAov2.Get(),
        m_reconstructionHistoryRadiance.Get(), m_reconstructionHistoryMoments.Get(), m_reconstructionHistoryLength.Get(),
        m_reconstructionHistoryRadianceB.Get(), m_reconstructionHistoryMomentsB.Get(), m_reconstructionHistoryLengthB.Get(),
        m_previousDenoiseAov0.Get(), m_previousDenoiseAov1.Get(), m_previousDenoiseAov2.Get(),
        m_surfaceIdentity.Get(), m_previousSurfaceIdentity.Get(),
        m_signalCurrentRadiance.Get(), m_signalDirect.Get(), m_signalIndirect.Get(), m_signalResidual.Get(),
        m_denoisePing.Get(), m_denoisePong.Get(),
        m_nrdMotion.Get(), m_nrdNormalRoughness.Get(), m_nrdViewZ.Get(),
        m_nrdDiffRadianceHitDistance.Get(), m_nrdSpecRadianceHitDistance.Get(),
        m_nrdDiffDenoised.Get(), m_nrdSpecDenoised.Get(),
        m_postDenoiseHdr.Get(), m_taaHistoryA.Get(), m_taaHistoryB.Get(),
        m_finalResolvedHdr.Get(),
        m_qualityCounterBuffer.Get(), m_qualityContribution.Get(),
        m_nrdDiffuseConfidence.Get(), m_nrdSpecularConfidence.Get(),
        m_restirReservoirCurrent.Get(), m_restirReservoirHistory.Get(),
        m_restirReservoirSpatial.Get(), m_restirReservoirSpatialB.Get(),
        m_restirDiReservoirCurrent.Get(), m_restirDiReservoirHistory.Get(), m_restirDiReservoirSpatial.Get(),
        m_restirGiReservoirA.Get(), m_restirGiReservoirB.Get(),
        m_restirPtReservoirA.Get(), m_restirPtReservoirB.Get(),
        m_dlssDepth.Get(), m_dlssMotion.Get(), m_dlssNormalRoughness.Get(),
        m_dlssAlbedo.Get(), m_dlssSpecularAlbedo.Get(), m_dlssExposure.Get(),
        m_primaryPositionCone.Get(), m_primaryGeometricNormal.Get(), m_primaryIdentity.Get(),
        m_secondaryTaskOffsets.Get(), m_secondaryGroupOffsets.Get(),
        m_secondaryTasks.Get(), m_secondaryResults.Get(), m_secondaryIndirectArgs.Get(),
    };
    std::vector<ID3D12Resource*> uniqueResources;
    uniqueResources.reserve(frameHistoryResources.size());
    m_frameHistoryResourceBytes = 0;
    for (ID3D12Resource* resource : frameHistoryResources)
    {
        if (!resource || std::find(uniqueResources.begin(), uniqueResources.end(), resource) != uniqueResources.end())
        {
            continue;
        }
        if (m_restirAliasHeapSize != 0u &&
            (resource == m_restirReservoirSpatial.Get() ||
             resource == m_restirReservoirSpatialB.Get() ||
             (allocateFullGiReservoirs &&
                (resource == m_restirGiReservoirA.Get() ||
                 resource == m_restirGiReservoirB.Get())) ||
             (allocateFullPtReservoirs &&
                (resource == m_restirPtReservoirA.Get() ||
                 resource == m_restirPtReservoirB.Get()))))
        {
            continue;
        }
        uniqueResources.push_back(resource);
        const D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
        m_frameHistoryResourceBytes += m_device->GetResourceAllocationInfo(0, 1, &resourceDesc).SizeInBytes;
    }
    m_frameHistoryResourceBytes += m_restirAliasHeapSize * m_restirAliasHeaps.size();
}

void D3D12PathTracingBackend::CreateGlobalRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, DescriptorVertexBuffer, 0, 0);
    CD3DX12_DESCRIPTOR_RANGE sceneBufferRange;
    sceneBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE textureRange;
    textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount + 2u, 0, 1);

    CD3DX12_ROOT_PARAMETER rootParameters[RootParameterCount];
    rootParameters[RootOutputTable].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootAccelerationStructure].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootSceneConstants].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootSceneBuffers].InitAsDescriptorTable(1, &sceneBufferRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[RootTextureTable].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_ALL);

    std::array<D3D12_STATIC_SAMPLER_DESC, 6> samplers = {};
    const D3D12_TEXTURE_ADDRESS_MODE addressModes[] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR };
    for (UINT i = 0; i < samplers.size(); ++i)
    {
        D3D12_STATIC_SAMPLER_DESC& sampler = samplers[i];
        sampler.Filter = i < 3 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = addressModes[i % 3];
        sampler.AddressV = addressModes[i % 3];
        sampler.AddressW = addressModes[i % 3];
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.ShaderRegister = i;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
    }

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(_countof(rootParameters), rootParameters,
        static_cast<UINT>(samplers.size()), samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr) && error)
    {
        OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
    }
    ThrowIfFailed(hr);
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSignature)));
}

void D3D12PathTracingBackend::CreateSceneBuffers()
{
    m_vertexBuffer = CreateDefaultBuffer(m_scene.vertices.data(), m_scene.vertices.size() * sizeof(Bistro::Vertex), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtVertices");
    m_indexBuffer = CreateDefaultBuffer(m_scene.indices.data(), m_scene.indices.size() * sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtIndices");
    m_geometryBuffer = CreateDefaultBuffer(m_geometryRecords.data(), m_geometryRecords.size() * sizeof(Bistro::RtGeometryRecord), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtGeometryRecords");
    m_instanceBuffer = CreateDefaultBuffer(m_rtInstances.data(), m_rtInstances.size() * sizeof(Bistro::RtInstance), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RtInstances");
    CreateLightBuffer();

    D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrv = {};
    vertexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vertexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vertexSrv.Buffer.NumElements = static_cast<UINT>(m_scene.vertices.size());
    vertexSrv.Buffer.StructureByteStride = sizeof(Bistro::Vertex);
    m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &vertexSrv, CpuDescriptor(DescriptorVertexBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC indexSrv = {};
    indexSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    indexSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    indexSrv.Buffer.NumElements = static_cast<UINT>(m_scene.indices.size());
    indexSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    m_device->CreateShaderResourceView(m_indexBuffer.Get(), &indexSrv, CpuDescriptor(DescriptorIndexBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC geometrySrv = {};
    geometrySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    geometrySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    geometrySrv.Buffer.NumElements = static_cast<UINT>(m_geometryRecords.size());
    geometrySrv.Buffer.StructureByteStride = sizeof(Bistro::RtGeometryRecord);
    m_device->CreateShaderResourceView(m_geometryBuffer.Get(), &geometrySrv, CpuDescriptor(DescriptorGeometryBuffer));

    D3D12_SHADER_RESOURCE_VIEW_DESC instanceSrv = {};
    instanceSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    instanceSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    instanceSrv.Buffer.NumElements = static_cast<UINT>(m_rtInstances.size());
    instanceSrv.Buffer.StructureByteStride = sizeof(Bistro::RtInstance);
    m_device->CreateShaderResourceView(m_instanceBuffer.Get(), &instanceSrv, CpuDescriptor(DescriptorInstanceBuffer));

    const UINT constantSize = CalculateConstantBufferByteSize(sizeof(SceneConstantBuffer));
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto constantDesc = CD3DX12_RESOURCE_DESC::Buffer(constantSize);
    for (UINT frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameContext& frameContext = m_frameContexts[frameIndex];
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &constantDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&frameContext.sceneConstantBuffer)));
        frameContext.sceneConstantBuffer->SetName((L"SceneConstantBuffer " + std::to_wstring(frameIndex)).c_str());
        ThrowIfFailed(frameContext.sceneConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&frameContext.mappedSceneConstants)));
    }
}

void D3D12PathTracingBackend::CreateLightBuffer()
{
    if (m_lights.empty())
    {
        m_lights.push_back(Bistro::RtLight{});
        m_activeLightCount = 0;
    }

    m_lightBuffer = CreateDefaultBuffer(
        m_lights.data(),
        m_lights.size() * sizeof(Bistro::RtLight),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        L"RtLights");

    D3D12_SHADER_RESOURCE_VIEW_DESC lightSrv = {};
    lightSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lightSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lightSrv.Buffer.NumElements = static_cast<UINT>(m_lights.size());
    lightSrv.Buffer.StructureByteStride = sizeof(Bistro::RtLight);
    m_device->CreateShaderResourceView(m_lightBuffer.Get(), &lightSrv, CpuDescriptor(DescriptorLightBuffer));
}

UINT D3D12PathTracingBackend::CreateTextureResource(
    const std::wstring& path,
    bool srgb,
    const uint8_t fallback[4],
    std::map<std::wstring, UINT>& cache,
    float alphaCoverageCutoff,
    bool environmentRadiance,
    rb::TextureResolutionPolicy resolutionPolicy)
{
    uint32_t maxDimension = 4096u;
    switch (resolutionPolicy)
    {
    case rb::TextureResolutionPolicy::Source: maxDimension = Bistro::RenderableTextureMaxDimension; break;
    case rb::TextureResolutionPolicy::Max4096: maxDimension = 4096u; break;
    case rb::TextureResolutionPolicy::Max2048: maxDimension = 2048u; break;
    case rb::TextureResolutionPolicy::Max1024: maxDimension = 1024u; break;
    case rb::TextureResolutionPolicy::Max512: maxDimension = 512u; break;
    case rb::TextureResolutionPolicy::Auto:
    default: maxDimension = m_textureBudgetBytes >= 2ull * 1024ull * 1024ull * 1024ull ? 8192u : 4096u; break;
    }
    std::wstring key = path.empty() ? (std::wstring(L"fallback:") + std::to_wstring(fallback[0]) + L"," + std::to_wstring(fallback[1]) + L"," + std::to_wstring(fallback[2]) + L"," + std::to_wstring(fallback[3]) + (srgb ? L":srgb" : L":linear")) : path + (srgb ? L":srgb" : L":linear");
    key += L":max:" + std::to_wstring(maxDimension);
    if (alphaCoverageCutoff >= 0.0f)
    {
        key += L":coverage:" + std::to_wstring(alphaCoverageCutoff);
    }
    if (environmentRadiance)
    {
        key += L":environment-radiance";
    }
    auto found = cache.find(key);
    if (found != cache.end())
    {
        return found->second;
    }

    Bistro::TextureData image = environmentRadiance
        ? Bistro::LoadEnvironmentRadianceTexture(path, fallback, maxDimension)
        : Bistro::LoadTextureD3D12(path, srgb, fallback, alphaCoverageCutoff, maxDimension);
    if (resolutionPolicy == rb::TextureResolutionPolicy::Auto)
    {
        while (!image.fallback && m_textureResidentBytes + image.residentBytes > m_textureBudgetBytes && maxDimension > 512u)
        {
            maxDimension /= 2u;
            image = environmentRadiance
                ? Bistro::LoadEnvironmentRadianceTexture(path, fallback, maxDimension)
                : Bistro::LoadTextureD3D12(path, srgb, fallback, alphaCoverageCutoff, maxDimension);
        }
        if (!image.fallback && m_textureResidentBytes + image.residentBytes > m_textureBudgetBytes)
        {
            image = Bistro::LoadTextureD3D12(L"", srgb, fallback, alphaCoverageCutoff, 1u);
        }
    }
    GpuTexture texture;
    texture.path = path;
    texture.format = image.format;
    texture.width = image.width;
    texture.height = image.height;
    texture.sourceWidth = image.sourceWidth;
    texture.sourceHeight = image.sourceHeight;
    texture.mipLevels = image.mipLevels;
    texture.container = image.container;
    texture.transcodeFormat = image.transcodeFormat;
    texture.fallback = image.fallback;
    if (image.mipLevels > 0xffffu)
    {
        throw std::runtime_error("Texture has too many mip levels for a D3D12 Texture2D resource.");
    }
    const UINT16 textureMipLevels = static_cast<UINT16>(image.mipLevels);
    D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(image.format, image.width, image.height, 1, textureMipLevels);
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource)));
    texture.residentBytes = m_device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.resource.Get(), 0, image.mipLevels);
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture.upload)));

    std::vector<D3D12_SUBRESOURCE_DATA> subresources(image.mipLevels);
    for (uint32_t mipIndex = 0; mipIndex < image.mipLevels; ++mipIndex)
    {
        const Bistro::TextureMip& mip = image.mips[mipIndex];
        subresources[mipIndex].pData = image.pixels.data() + mip.offset;
        subresources[mipIndex].RowPitch = static_cast<LONG_PTR>(mip.rowPitch);
        subresources[mipIndex].SlicePitch = static_cast<LONG_PTR>(mip.slicePitch);
    }
    UpdateSubresources(m_commandList.Get(), texture.resource.Get(), texture.upload.Get(), 0, 0, image.mipLevels, subresources.data());
    auto textureReadyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_commandList->ResourceBarrier(1, &textureReadyBarrier);

    const UINT index = static_cast<UINT>(m_textures.size());
    m_textures.push_back(texture);
    m_textureResidentBytes += texture.residentBytes;
    cache[key] = index;
    return index;
}

void D3D12PathTracingBackend::CreateMaterialBuffer()
{
    m_rtMaterials.resize(m_scene.materials.size());
    m_rtMaterialExtensions.resize(m_scene.materials.size());
    m_rtTextureBindings.resize(m_scene.materials.size() * TextureSlotCount);
    m_materialTextureExists.resize(m_scene.materials.size());
    for (size_t materialIndex = 0; materialIndex < m_scene.materials.size(); ++materialIndex)
    {
        const Bistro::Material& material = m_scene.materials[materialIndex];
        auto& textureExists = m_materialTextureExists[materialIndex];
        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            std::error_code existsError;
            textureExists[slot] = !material.textures[slot].empty() &&
                std::filesystem::exists(material.textures[slot], existsError) && !existsError;
        }
        Bistro::RtMaterial rtMaterial{};
        rtMaterial.baseColorFactor = material.baseColorFactor;
        rtMaterial.emissiveFactor = material.emissiveFactor;
        rtMaterial.textureBaseIndex = static_cast<uint32_t>(materialIndex * TextureSlotCount);
        rtMaterial.alphaMasked = material.alphaMasked ? 1u : 0u;
        rtMaterial.alphaCutoff = material.alphaCutoff;
        rtMaterial.normalStrength = material.normalStrength;
        rtMaterial.roughnessFactor = material.roughnessFactor;
        rtMaterial.metallicFactor = material.metallicFactor;
        rtMaterial.occlusionStrength = material.occlusionStrength;
        uint32_t materialFeatures = material.packedOcclusionRoughnessMetallic
            ? Bistro::RtMaterialFeaturePackedOcclusionRoughnessMetallic
            : 0u;
        if (material.gltfMetallicRoughness)
        {
            materialFeatures |= Bistro::RtMaterialFeatureGltfMetallicRoughness;
        }
        if (textureExists[Bistro::TextureSlotBaseColor])
        {
            materialFeatures |= Bistro::RtMaterialFeatureBaseColorTexture;
        }
        if (textureExists[Bistro::TextureSlotNormal])
        {
            materialFeatures |= Bistro::RtMaterialFeatureNormalTexture;
        }
        if (textureExists[Bistro::TextureSlotRoughness])
        {
            materialFeatures |= Bistro::RtMaterialFeatureRoughnessTexture;
        }
        if (textureExists[Bistro::TextureSlotMetallic])
        {
            materialFeatures |= Bistro::RtMaterialFeatureMetallicTexture;
        }
        if (textureExists[Bistro::TextureSlotOcclusion])
        {
            materialFeatures |= Bistro::RtMaterialFeatureOcclusionTexture;
        }
        if (textureExists[Bistro::TextureSlotEmissive])
        {
            materialFeatures |= Bistro::RtMaterialFeatureEmissiveTexture;
        }
        if (textureExists[Bistro::TextureSlotAlpha])
        {
            materialFeatures |= Bistro::RtMaterialFeatureAlphaTexture;
        }
        if (textureExists[Bistro::TextureSlotSpecularColor]) materialFeatures |= Bistro::RtMaterialFeatureSpecularColorTexture;
        if (textureExists[Bistro::TextureSlotSpecularFactor]) materialFeatures |= Bistro::RtMaterialFeatureSpecularFactorTexture;
        if (textureExists[Bistro::TextureSlotTransmission]) materialFeatures |= Bistro::RtMaterialFeatureTransmissionTexture;
        if (textureExists[Bistro::TextureSlotThickness]) materialFeatures |= Bistro::RtMaterialFeatureThicknessTexture;
        if (textureExists[Bistro::TextureSlotClearcoat]) materialFeatures |= Bistro::RtMaterialFeatureClearcoatTexture;
        if (textureExists[Bistro::TextureSlotClearcoatRoughness]) materialFeatures |= Bistro::RtMaterialFeatureClearcoatRoughnessTexture;
        if (textureExists[Bistro::TextureSlotClearcoatNormal]) materialFeatures |= Bistro::RtMaterialFeatureClearcoatNormalTexture;
        rtMaterial.materialFeatures = materialFeatures;
        rtMaterial.transmissionFactor = std::clamp(material.transmissionFactor, 0.0f, 1.0f);
        rtMaterial.indexOfRefraction = std::clamp(material.indexOfRefraction, 1.0001f, 3.0f);
        rtMaterial.thinDielectric = material.thinDielectric ? 1u : 0u;
        rtMaterial.uvScaleOffset = material.uvScaleOffset;
        m_rtMaterials[materialIndex] = rtMaterial;

        Bistro::RtMaterialExtension extension{};
        extension.specularColorFactorAndFactor = XMFLOAT4(
            material.specularColorFactor.x,
            material.specularColorFactor.y,
            material.specularColorFactor.z,
            std::clamp(material.specularFactor, 0.0f, 1.0f));
        extension.attenuationColorAndDistance = XMFLOAT4(
            std::max(material.attenuationColor.x, 0.0f),
            std::max(material.attenuationColor.y, 0.0f),
            std::max(material.attenuationColor.z, 0.0f),
            material.attenuationDistance > 0.0f ? material.attenuationDistance : FLT_MAX);
        extension.transmissionThicknessIorFeatures = XMFLOAT4(
            std::clamp(material.transmissionFactor, 0.0f, 1.0f),
            std::max(material.thicknessFactor, 0.0f),
            std::clamp(material.indexOfRefraction, 1.0f, 3.0f),
            static_cast<float>(material.extensionFeatureMask));
        extension.clearcoat = XMFLOAT4(
            std::clamp(material.clearcoatFactor, 0.0f, 1.0f),
            std::clamp(material.clearcoatRoughnessFactor, 0.0f, 1.0f),
            std::max(material.clearcoatNormalScale, 0.0f),
            0.0f);
        m_rtMaterialExtensions[materialIndex] = extension;

        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            const Bistro::TextureBinding& source = material.textureBindings[slot];
            Bistro::RtTextureBinding& binding = m_rtTextureBindings[materialIndex * TextureSlotCount + slot];
            // PBRT uses the legacy material-wide scale/offset. glTF bindings
            // add a per-slot transform, so compose the legacy affine terms
            // into the fixed binding ABI instead of dropping PBRT mapping.
            binding.offsetScale = XMFLOAT4(
                source.offset.x + material.uvScaleOffset.z,
                source.offset.y + material.uvScaleOffset.w,
                source.scale.x * material.uvScaleOffset.x,
                source.scale.y * material.uvScaleOffset.y);
            binding.rotationTexCoordSampler = XMFLOAT4(
                source.rotation,
                static_cast<float>(std::min(source.texCoord, 1u)),
                static_cast<float>(source.samplerIndex),
                static_cast<float>(source.resolutionPolicy));
        }
    }

    m_materialBuffer = CreateDefaultBuffer(
        m_rtMaterials.data(),
        m_rtMaterials.size() * sizeof(Bistro::RtMaterial),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        L"RtMaterials");
    D3D12_SHADER_RESOURCE_VIEW_DESC materialSrv = {};
    materialSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    materialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    materialSrv.Buffer.NumElements = static_cast<UINT>(m_rtMaterials.size());
    materialSrv.Buffer.StructureByteStride = sizeof(Bistro::RtMaterial);
    m_device->CreateShaderResourceView(m_materialBuffer.Get(), &materialSrv, CpuDescriptor(DescriptorMaterialBuffer));

    m_materialExtensionBuffer = CreateDefaultBuffer(
        m_rtMaterialExtensions.data(),
        m_rtMaterialExtensions.size() * sizeof(Bistro::RtMaterialExtension),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        L"RtMaterialExtensions");
    D3D12_SHADER_RESOURCE_VIEW_DESC extensionSrv = {};
    extensionSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    extensionSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    extensionSrv.Buffer.NumElements = static_cast<UINT>(m_rtMaterialExtensions.size());
    extensionSrv.Buffer.StructureByteStride = sizeof(Bistro::RtMaterialExtension);
    m_device->CreateShaderResourceView(m_materialExtensionBuffer.Get(), &extensionSrv, CpuDescriptor(DescriptorMaterialExtensionBuffer));

    m_textureBindingBuffer = CreateDefaultBuffer(
        m_rtTextureBindings.data(),
        m_rtTextureBindings.size() * sizeof(Bistro::RtTextureBinding),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        L"RtTextureBindings");
    D3D12_SHADER_RESOURCE_VIEW_DESC bindingSrv = {};
    bindingSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bindingSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bindingSrv.Buffer.NumElements = static_cast<UINT>(m_rtTextureBindings.size());
    bindingSrv.Buffer.StructureByteStride = sizeof(Bistro::RtTextureBinding);
    m_device->CreateShaderResourceView(m_textureBindingBuffer.Get(), &bindingSrv, CpuDescriptor(DescriptorTextureBindingBuffer));
}

void D3D12PathTracingBackend::CreateTextures()
{
    const uint8_t white[] = { 255, 255, 255, 255 };
    const uint8_t normal[] = { 128, 128, 255, 255 };
    const uint8_t roughness[] = { 122, 122, 122, 255 };
    const uint8_t metallic[] = { 0, 0, 0, 255 };
    const uint8_t black[] = { 0, 0, 0, 255 };
    // Disabled environments never sample this resource. White lets a PBRT
    // infinite light without a filename represent its authored constant L.
    const uint8_t environmentFallback[] = { 255, 255, 255, 255 };
    std::map<std::wstring, UINT> cache;
    m_materialTextureIndices.resize(m_scene.materials.size());

    for (size_t materialIndex = 0; materialIndex < m_scene.materials.size(); ++materialIndex)
    {
        const Bistro::Material& material = m_scene.materials[materialIndex];
        auto& indices = m_materialTextureIndices[materialIndex];
        const float alphaCoverageCutoff = material.alphaMasked
            ? std::clamp(material.alphaCutoff / std::max(material.baseColorFactor.w, 1.0e-6f), 0.0f, 1.0f)
            : -1.0f;
        indices[Bistro::TextureSlotBaseColor] = CreateTextureResource(
            material.textures[Bistro::TextureSlotBaseColor], true, white, cache, alphaCoverageCutoff, false,
            static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotBaseColor].resolutionPolicy));
        indices[Bistro::TextureSlotNormal] = CreateTextureResource(material.textures[Bistro::TextureSlotNormal], false, normal, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotNormal].resolutionPolicy));
        indices[Bistro::TextureSlotRoughness] = CreateTextureResource(material.textures[Bistro::TextureSlotRoughness], false, roughness, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotRoughness].resolutionPolicy));
        indices[Bistro::TextureSlotMetallic] = CreateTextureResource(material.textures[Bistro::TextureSlotMetallic], false, metallic, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotMetallic].resolutionPolicy));
        indices[Bistro::TextureSlotOcclusion] = CreateTextureResource(material.textures[Bistro::TextureSlotOcclusion], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotOcclusion].resolutionPolicy));
        indices[Bistro::TextureSlotEmissive] = CreateTextureResource(material.textures[Bistro::TextureSlotEmissive], true, black, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotEmissive].resolutionPolicy));
        indices[Bistro::TextureSlotAlpha] = CreateTextureResource(material.textures[Bistro::TextureSlotAlpha], false, white, cache, alphaCoverageCutoff, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotAlpha].resolutionPolicy));
        indices[Bistro::TextureSlotSpecularColor] = CreateTextureResource(material.textures[Bistro::TextureSlotSpecularColor], true, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotSpecularColor].resolutionPolicy));
        indices[Bistro::TextureSlotSpecularFactor] = CreateTextureResource(material.textures[Bistro::TextureSlotSpecularFactor], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotSpecularFactor].resolutionPolicy));
        indices[Bistro::TextureSlotTransmission] = CreateTextureResource(material.textures[Bistro::TextureSlotTransmission], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotTransmission].resolutionPolicy));
        indices[Bistro::TextureSlotThickness] = CreateTextureResource(material.textures[Bistro::TextureSlotThickness], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotThickness].resolutionPolicy));
        indices[Bistro::TextureSlotClearcoat] = CreateTextureResource(material.textures[Bistro::TextureSlotClearcoat], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotClearcoat].resolutionPolicy));
        indices[Bistro::TextureSlotClearcoatRoughness] = CreateTextureResource(material.textures[Bistro::TextureSlotClearcoatRoughness], false, white, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotClearcoatRoughness].resolutionPolicy));
        indices[Bistro::TextureSlotClearcoatNormal] = CreateTextureResource(material.textures[Bistro::TextureSlotClearcoatNormal], false, normal, cache, -1.0f, false, static_cast<rb::TextureResolutionPolicy>(material.textureBindings[Bistro::TextureSlotClearcoatNormal].resolutionPolicy));

        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            const GpuTexture& texture = m_textures[indices[slot]];
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = texture.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = texture.mipLevels;
            srvDesc.Texture2D.MostDetailedMip = 0;
            m_device->CreateShaderResourceView(texture.resource.Get(), &srvDesc, CpuDescriptor(DescriptorTextureBase + static_cast<UINT>(materialIndex) * TextureSlotCount + slot));
        }

    }

    const UINT environmentTexture = CreateTextureResource(
        m_environmentTexturePath, false, environmentFallback, cache, -1.0f, true);
    m_environmentDescriptorIndex = static_cast<UINT>(m_scene.materials.size()) * TextureSlotCount;
    const GpuTexture& environment = m_textures[environmentTexture];
    D3D12_SHADER_RESOURCE_VIEW_DESC environmentSrv = {};
    environmentSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    environmentSrv.Format = environment.format;
    environmentSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    environmentSrv.Texture2D.MipLevels = environment.mipLevels;
    environmentSrv.Texture2D.MostDetailedMip = 0;
    m_device->CreateShaderResourceView(environment.resource.Get(), &environmentSrv, CpuDescriptor(DescriptorTextureBase + m_environmentDescriptorIndex));

    const Bistro::TextureData environmentImportanceSource = Bistro::LoadEnvironmentImportanceSource(
        m_environmentTexturePath, environmentFallback);
    std::vector<Bistro::EnvironmentAliasEntry> environmentAlias = Bistro::BuildEnvironmentAliasTable(
        environmentImportanceSource,
        m_environmentEqualAreaMapping);
    if (environmentAlias.empty())
    {
        environmentAlias.push_back({});
    }
    LogDiagnostic(
        "Environment importance alias: " + std::to_string(std::max(environmentImportanceSource.width, 1u))
        + "x" + std::to_string(std::max(environmentImportanceSource.height, 1u))
        + ", " + std::to_string(environmentAlias.size()) + " entries.");

    GpuTexture aliasTexture;
    aliasTexture.path = m_environmentTexturePath;
    aliasTexture.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    aliasTexture.width = std::max(environmentImportanceSource.width, 1u);
    aliasTexture.height = std::max(environmentImportanceSource.height, 1u);
    aliasTexture.mipLevels = 1;
    const D3D12_RESOURCE_DESC aliasDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        aliasTexture.format, aliasTexture.width, aliasTexture.height, 1, 1);
    const auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &aliasDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&aliasTexture.resource)));
    const UINT64 aliasUploadSize = GetRequiredIntermediateSize(aliasTexture.resource.Get(), 0, 1);
    const auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto aliasUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(aliasUploadSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &aliasUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&aliasTexture.upload)));
    D3D12_SUBRESOURCE_DATA aliasSubresource{};
    aliasSubresource.pData = environmentAlias.data();
    aliasSubresource.RowPitch = static_cast<LONG_PTR>(aliasTexture.width * sizeof(Bistro::EnvironmentAliasEntry));
    aliasSubresource.SlicePitch = aliasSubresource.RowPitch * aliasTexture.height;
    UpdateSubresources(m_commandList.Get(), aliasTexture.resource.Get(), aliasTexture.upload.Get(), 0, 0, 1, &aliasSubresource);
    const auto aliasReadyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        aliasTexture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_commandList->ResourceBarrier(1, &aliasReadyBarrier);
    m_textures.push_back(aliasTexture);

    D3D12_SHADER_RESOURCE_VIEW_DESC aliasSrv{};
    aliasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    aliasSrv.Format = aliasTexture.format;
    aliasSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    aliasSrv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(
        aliasTexture.resource.Get(), &aliasSrv,
        CpuDescriptor(DescriptorTextureBase + m_environmentDescriptorIndex + 1u));

    CreateMaterialBuffer();
}

void D3D12PathTracingBackend::CreatePathtracingStateObject()
{
    const std::wstring exeDir = GetAssetFullPath(L"");
    byte* shaderData = nullptr;
    UINT shaderSize = 0;
    ThrowIfFailed(ReadDataFromFile((exeDir + ShaderFileName()).c_str(), &shaderData, &shaderSize));
    std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
    free(shaderData);

    CD3DX12_STATE_OBJECT_DESC pipeline(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    auto library = pipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE shaderBytecode = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
    library->SetDXILLibrary(&shaderBytecode);
    library->DefineExport(RayGenShaderName);
    library->DefineExport(PrimaryRayGenShaderName);
    library->DefineExport(SecondaryRayGenShaderName);
    library->DefineExport(ReviewProbeRayGenShaderName);
    library->DefineExport(MissShaderName);
    library->DefineExport(ShadowMissShaderName);
    library->DefineExport(ClosestHitShaderName);
    library->DefineExport(AnyHitShaderName);
    library->DefineExport(ShadowAnyHitShaderName);

    auto hitGroup = pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetHitGroupExport(HitGroupName);
    hitGroup->SetClosestHitShaderImport(ClosestHitShaderName);
    hitGroup->SetAnyHitShaderImport(AnyHitShaderName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    auto shadowHitGroup = pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    shadowHitGroup->SetHitGroupExport(ShadowHitGroupName);
    shadowHitGroup->SetAnyHitShaderImport(ShadowAnyHitShaderName);
    shadowHitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    // RayGen can invoke both the 32-byte path TraceRay call and the 12-byte
    // shadow TraceRay call. DXR requires every shader reachable from that
    // call graph to use a compatible shader configuration, so associating two
    // different payload maxima makes CreateStateObject fail with E_INVALIDARG.
    // The payload values themselves remain 32 and 24 bytes in HLSL; this
    // pipeline-wide maximum only declares the largest reachable payload.
    auto shaderConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shaderConfig->Config(sizeof(PathPayloadAbi), sizeof(XMFLOAT2));

    auto globalRootSignature = pipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_globalRootSignature.Get());

    auto pipelineConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pipelineConfig->Config(MaxTraceRecursionDepth());

    ThrowIfFailed(m_device->CreateStateObject(pipeline, IID_PPV_ARGS(&m_stateObject)));
    ThrowIfFailed(m_stateObject.As(&m_stateObjectProperties));
}

void D3D12PathTracingBackend::CreateRestirReusePipeline()
{
    m_rtxdiDiCandidatePipeline.Reset();
    m_rtxdiDiSpatialPipeline.Reset();
    m_rtxdiGiInitialPipeline.Reset();
    m_rtxdiGiFusedPipeline.Reset();
    m_rtxdiPtInitialPipeline.Reset();
    m_rtxdiPtFusedPipeline.Reset();
    m_rtxdiBackendRuntime.SetRendererEvaluationReady(false, false);
    if (!m_rtxdiAvailable || m_qualitySettings.restirBackend != rb::RestirBackend::Rtxdi ||
        (!UsesRestirDI(m_mode) && !UsesRestirGI(m_mode) && !UsesRestirPT(m_mode)) ||
        m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill)
    {
        return;
    }

    const std::wstring exeDir = GetAssetFullPath(L"");
    auto createPipeline = [&](const wchar_t* fileName, ComPtr<ID3D12PipelineState>& pipeline)
    {
        byte* shaderData = nullptr;
        UINT shaderSize = 0;
        ThrowIfFailed(ReadDataFromFile((exeDir + fileName).c_str(), &shaderData, &shaderSize));
        std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
        free(shaderData);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_globalRootSignature.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
        ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };

    // Candidate.cso is fused candidate+temporal Pass A. Spatial.cso is fused
    // spatial+visibility+shade Pass B. The two legacy entry points remain
    // shader-build compatible but no longer need PSOs or dispatches.
    if (UsesRestirDI(m_mode))
    {
        createPipeline(L"RtxdiDiCandidate.cso", m_rtxdiDiCandidatePipeline);
        createPipeline(L"RtxdiDiSpatial.cso", m_rtxdiDiSpatialPipeline);
    }
    if (UsesRestirGI(m_mode))
    {
        createPipeline(L"RtxdiGiInitial.cso", m_rtxdiGiInitialPipeline);
        createPipeline(L"RtxdiGiFused.cso", m_rtxdiGiFusedPipeline);
    }
    if (UsesRestirPT(m_mode))
    {
        createPipeline(L"RtxdiPtInitial.cso", m_rtxdiPtInitialPipeline);
        createPipeline(L"RtxdiPtFused.cso", m_rtxdiPtFusedPipeline);
    }
    m_rtxdiBackendRuntime.SetRendererEvaluationReady(
        m_rtxdiGiInitialPipeline && m_rtxdiGiFusedPipeline,
        m_rtxdiPtInitialPipeline && m_rtxdiPtFusedPipeline);
}

void D3D12PathTracingBackend::CreateDenoisePipeline()
{
    const std::wstring exeDir = GetAssetFullPath(L"");
    auto createPipeline = [&](const std::wstring& fileName, ComPtr<ID3D12PipelineState>& pipeline)
    {
        byte* shaderData = nullptr;
        UINT shaderSize = 0;
        ThrowIfFailed(ReadDataFromFile((exeDir + fileName).c_str(), &shaderData, &shaderSize));
        std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
        free(shaderData);

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_globalRootSignature.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
        ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };

    createPipeline(L"PathTracingDenoiseTemporal.cso", m_denoiseTemporalPipeline);
    for (UINT i = 0; i < DenoiseAtrousPipelineCount; ++i)
    {
        createPipeline(L"PathTracingDenoiseAtrous" + std::to_wstring(i) + L".cso", m_denoiseAtrousPipelines[i]);
    }
    createPipeline(L"PathTracingDenoiseComposite.cso", m_denoiseCompositePipeline);
    createPipeline(L"PathTracingNrdPrepare.cso", m_nrdPreparePipeline);
    createPipeline(L"PathTracingNrdComposite.cso", m_nrdCompositePipeline);
    createPipeline(L"PathTracingDlssPrepare.cso", m_dlssPreparePipeline);
    createPipeline(L"PathTracingFinalTaa.cso", m_finalTaaPipeline);
    createPipeline(L"PathTracingQualityCounters.cso", m_qualityCounterPipeline);
}

void D3D12PathTracingBackend::CreateSecondaryWorkPipelines()
{
    m_secondaryTaskCountPipeline.Reset();
    m_secondaryGroupScanPipeline.Reset();
    m_secondaryTaskScatterPipeline.Reset();
    m_secondaryResolvePipeline.Reset();
    if (!UsesCompactSecondaryWorkList())
    {
        return;
    }

    const std::wstring exeDir = GetAssetFullPath(L"");
    auto createPipeline = [&](
        const wchar_t* fileName,
        ComPtr<ID3D12PipelineState>& pipeline)
    {
        byte* shaderData = nullptr;
        UINT shaderSize = 0;
        ThrowIfFailed(ReadDataFromFile(
            (exeDir + fileName).c_str(),
            &shaderData,
            &shaderSize));
        std::vector<UINT8> shader(shaderData, shaderData + shaderSize);
        free(shaderData);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_globalRootSignature.Get();
        desc.CS = CD3DX12_SHADER_BYTECODE(shader.data(), shader.size());
        ThrowIfFailed(m_device->CreateComputePipelineState(
            &desc,
            IID_PPV_ARGS(&pipeline)));
    };
    createPipeline(L"SecondaryTaskCount.cso", m_secondaryTaskCountPipeline);
    createPipeline(L"SecondaryGroupScan.cso", m_secondaryGroupScanPipeline);
    createPipeline(L"SecondaryTaskScatter.cso", m_secondaryTaskScatterPipeline);
    createPipeline(L"SecondaryResolve.cso", m_secondaryResolvePipeline);
}

void D3D12PathTracingBackend::BuildAccelerationStructures()
{
    if (!m_scene.meshes.empty() && !m_scene.instances.empty())
    {
        BuildInstancedAccelerationStructures();
        return;
    }

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    geometryDescs.reserve(m_scene.draws.size());
    for (const Bistro::DrawItem& draw : m_scene.draws)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        desc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress();
        desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Bistro::Vertex);
        desc.Triangles.VertexCount = static_cast<UINT>(m_scene.vertices.size());
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress() + draw.startIndex * sizeof(uint32_t);
        desc.Triangles.IndexCount = draw.indexCount;
        desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        const bool requiresAnyHit = draw.materialIndex < m_scene.materials.size() &&
            (m_scene.materials[draw.materialIndex].alphaMasked ||
                m_scene.materials[draw.materialIndex].transmissionFactor > 0.0f);
        desc.Flags = requiresAnyHit
            ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
            : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometryDescs.push_back(desc);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInputs = {};
    bottomInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottomInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottomInputs.NumDescs = static_cast<UINT>(geometryDescs.size());
    bottomInputs.pGeometryDescs = geometryDescs.data();
    bottomInputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomInputs, &bottomInfo);
    if (bottomInfo.ResultDataMaxSizeInBytes == 0)
    {
        throw std::runtime_error("Failed to query BLAS size.");
    }

    m_blasOriginalBytes = bottomInfo.ResultDataMaxSizeInBytes;
    m_blasCompactedBytes = bottomInfo.ResultDataMaxSizeInBytes;
    m_bottomLevelAs.scratch = CreateUavBuffer(bottomInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"BLAS Scratch");
    m_bottomLevelAs.result = CreateUavBuffer(bottomInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS Uncompacted");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomBuild = {};
    bottomBuild.Inputs = bottomInputs;
    bottomBuild.ScratchAccelerationStructureData = m_bottomLevelAs.scratch->GetGPUVirtualAddress();
    bottomBuild.DestAccelerationStructureData = m_bottomLevelAs.result->GetGPUVirtualAddress();
    m_commandList->BuildRaytracingAccelerationStructure(&bottomBuild, 0, nullptr);
    auto bottomAsBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_bottomLevelAs.result.Get());
    m_commandList->ResourceBarrier(1, &bottomAsBarrier);

    // Compacted size is only known after the GPU finishes the BLAS build.
    // Submit this initialization prefix, read the 64-bit query, then continue
    // recording compact-copy and TLAS work on a fresh command list.
    ComPtr<ID3D12Resource> compactedSizeGpu = CreateUavBuffer(
        sizeof(UINT64),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"BLAS Compacted Size Query");
    ComPtr<ID3D12Resource> compactedSizeReadback;
    auto readbackHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto compactedSizeDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &readbackHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &compactedSizeDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&compactedSizeReadback)));
    compactedSizeReadback->SetName(L"BLAS Compacted Size Readback");

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postBuildInfo = {};
    postBuildInfo.DestBuffer = compactedSizeGpu->GetGPUVirtualAddress();
    postBuildInfo.InfoType =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    const D3D12_GPU_VIRTUAL_ADDRESS uncompactedBlasAddress =
        m_bottomLevelAs.result->GetGPUVirtualAddress();
    m_commandList->EmitRaytracingAccelerationStructurePostbuildInfo(
        &postBuildInfo,
        1u,
        &uncompactedBlasAddress);
    auto queryToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        compactedSizeGpu.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->ResourceBarrier(1u, &queryToCopy);
    m_commandList->CopyBufferRegion(
        compactedSizeReadback.Get(),
        0u,
        compactedSizeGpu.Get(),
        0u,
        sizeof(UINT64));

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* buildCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(buildCommandLists), buildCommandLists);
    WaitForPreviousFrame();

    UINT64 compactedSize = 0u;
    void* mappedSize = nullptr;
    D3D12_RANGE readRange = { 0u, sizeof(UINT64) };
    ThrowIfFailed(compactedSizeReadback->Map(0u, &readRange, &mappedSize));
    memcpy(&compactedSize, mappedSize, sizeof(compactedSize));
    D3D12_RANGE writtenRange = { 0u, 0u };
    compactedSizeReadback->Unmap(0u, &writtenRange);

    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    ThrowIfFailed(frameContext.commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));

    if (compactedSize > 0u && compactedSize <= bottomInfo.ResultDataMaxSizeInBytes)
    {
        ComPtr<ID3D12Resource> uncompactedBlas = m_bottomLevelAs.result;
        ComPtr<ID3D12Resource> compactedBlas = CreateUavBuffer(
            compactedSize,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            L"BLAS Compacted");
        m_commandList->CopyRaytracingAccelerationStructure(
            compactedBlas->GetGPUVirtualAddress(),
            uncompactedBlas->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        auto compactedBarrier = CD3DX12_RESOURCE_BARRIER::UAV(compactedBlas.Get());
        m_commandList->ResourceBarrier(1u, &compactedBarrier);
        // Keep the source alive until the initialization submission containing
        // the compact copy and dependent TLAS build has completed.
        m_uploadBuffers.push_back(uncompactedBlas);
        m_bottomLevelAs.result = compactedBlas;
        m_blasCompactedBytes = compactedSize;
    }
    m_bottomLevelAs.scratch.Reset();

    if (m_sceneLoadStage.load(std::memory_order_relaxed) == SceneLoadStage::BuildingBLAS)
        m_sceneLoadStage.store(SceneLoadStage::BuildingTLAS, std::memory_order_relaxed);

    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    instanceDesc.Transform[0][0] = 1.0f;
    instanceDesc.Transform[1][1] = 1.0f;
    instanceDesc.Transform[2][2] = 1.0f;
    instanceDesc.InstanceMask = 0xff;
    instanceDesc.AccelerationStructure = m_bottomLevelAs.result->GetGPUVirtualAddress();
    m_topLevelAs.instanceDesc = CreateUploadBuffer(&instanceDesc, sizeof(instanceDesc), L"TLAS Instance");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInputs = {};
    topInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topInputs.NumDescs = 1;
    topInputs.InstanceDescs = m_topLevelAs.instanceDesc->GetGPUVirtualAddress();
    topInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&topInputs, &topInfo);
    if (topInfo.ResultDataMaxSizeInBytes == 0)
    {
        throw std::runtime_error("Failed to query TLAS size.");
    }
    m_topLevelAs.scratch = CreateUavBuffer(topInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"TLAS Scratch");
    m_topLevelAs.result = CreateUavBuffer(topInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild = {};
    topBuild.Inputs = topInputs;
    topBuild.ScratchAccelerationStructureData = m_topLevelAs.scratch->GetGPUVirtualAddress();
    topBuild.DestAccelerationStructureData = m_topLevelAs.result->GetGPUVirtualAddress();
    m_commandList->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);
    auto topAsBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_topLevelAs.result.Get());
    m_commandList->ResourceBarrier(1, &topAsBarrier);
}

void D3D12PathTracingBackend::BuildInstancedAccelerationStructures()
{
    if (m_scene.meshes.empty() || m_scene.instances.empty())
    {
        throw std::runtime_error("Instanced acceleration-structure build requires meshes and instances.");
    }
    if (m_scene.instances.size() >= (1u << 24u))
    {
        throw std::runtime_error("Scene exceeds the DXR 24-bit instance limit.");
    }

    m_bottomLevelAs = {};
    m_bottomLevelInstances.clear();
    m_bottomLevelInstances.resize(m_scene.meshes.size());
    m_blasOriginalBytes = 0;
    m_blasCompactedBytes = 0;

    for (size_t meshIndex = 0; meshIndex < m_scene.meshes.size(); ++meshIndex)
    {
        const Bistro::MeshRange& mesh = m_scene.meshes[meshIndex];
        if (mesh.drawCount == 0 || mesh.drawOffset >= m_scene.draws.size() ||
            static_cast<size_t>(mesh.drawOffset) + mesh.drawCount > m_scene.draws.size())
        {
            throw std::runtime_error("Imported mesh has an invalid geometry range.");
        }
        if (mesh.drawOffset >= (1u << 24u))
        {
            throw std::runtime_error("Scene exceeds the DXR 24-bit geometry-base limit.");
        }

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
        geometryDescs.reserve(mesh.drawCount);
        for (uint32_t localGeometry = 0; localGeometry < mesh.drawCount; ++localGeometry)
        {
            const Bistro::DrawItem& draw = m_scene.draws[mesh.drawOffset + localGeometry];
            D3D12_RAYTRACING_GEOMETRY_DESC desc{};
            desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            desc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress();
            desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Bistro::Vertex);
            desc.Triangles.VertexCount = static_cast<UINT>(m_scene.vertices.size());
            desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            desc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(draw.startIndex) * sizeof(uint32_t);
            desc.Triangles.IndexCount = draw.indexCount;
            desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            const bool requiresAnyHit = draw.materialIndex < m_scene.materials.size() &&
                (m_scene.materials[draw.materialIndex].alphaMasked ||
                    m_scene.materials[draw.materialIndex].transmissionFactor > 0.0f);
            desc.Flags = requiresAnyHit ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometryDescs.push_back(desc);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        inputs.pGeometryDescs = geometryDescs.data();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (info.ResultDataMaxSizeInBytes == 0)
        {
            throw std::runtime_error("Failed to query instanced BLAS size.");
        }
        AccelerationStructureBuffers& buffers = m_bottomLevelInstances[meshIndex];
        buffers.scratch = CreateUavBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"PBRT BLAS Scratch");
        buffers.result = CreateUavBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"PBRT BLAS");
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = inputs;
        build.ScratchAccelerationStructureData = buffers.scratch->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = buffers.result->GetGPUVirtualAddress();
        m_commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(buffers.result.Get());
        m_commandList->ResourceBarrier(1, &barrier);
        m_blasOriginalBytes += info.ResultDataMaxSizeInBytes;
        m_blasCompactedBytes += info.ResultDataMaxSizeInBytes;
    }

    if (m_sceneLoadStage.load(std::memory_order_relaxed) == SceneLoadStage::BuildingBLAS)
        m_sceneLoadStage.store(SceneLoadStage::BuildingTLAS, std::memory_order_relaxed);

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(m_scene.instances.size());
    for (size_t instanceIndex = 0; instanceIndex < m_scene.instances.size(); ++instanceIndex)
    {
        const Bistro::SceneInstance& instance = m_scene.instances[instanceIndex];
        if (instance.meshIndex >= m_scene.meshes.size()) throw std::runtime_error("Scene instance references an invalid mesh.");
        const Bistro::MeshRange& mesh = m_scene.meshes[instance.meshIndex];
        D3D12_RAYTRACING_INSTANCE_DESC& desc = instanceDescs[instanceIndex];
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(XMLoadFloat4x4(&instance.transform)));
        memcpy(desc.Transform, &transposed, sizeof(desc.Transform));
        desc.InstanceID = mesh.drawOffset;
        desc.InstanceMask = 0xff;
        desc.InstanceContributionToHitGroupIndex = 0;
        desc.AccelerationStructure = m_bottomLevelInstances[instance.meshIndex].result->GetGPUVirtualAddress();
    }
    m_topLevelAs.instanceDesc = CreateUploadBuffer(
        instanceDescs.data(),
        instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
        L"PBRT TLAS Instances");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInputs{};
    topInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
    topInputs.InstanceDescs = m_topLevelAs.instanceDesc->GetGPUVirtualAddress();
    topInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&topInputs, &topInfo);
    if (topInfo.ResultDataMaxSizeInBytes == 0) throw std::runtime_error("Failed to query instanced TLAS size.");
    m_topLevelAs.scratch = CreateUavBuffer(topInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON, L"PBRT TLAS Scratch");
    m_topLevelAs.result = CreateUavBuffer(topInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"PBRT TLAS");
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topBuild{};
    topBuild.Inputs = topInputs;
    topBuild.ScratchAccelerationStructureData = m_topLevelAs.scratch->GetGPUVirtualAddress();
    topBuild.DestAccelerationStructureData = m_topLevelAs.result->GetGPUVirtualAddress();
    m_commandList->BuildRaytracingAccelerationStructure(&topBuild, 0, nullptr);
    auto topBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_topLevelAs.result.Get());
    m_commandList->ResourceBarrier(1, &topBarrier);
}

void D3D12PathTracingBackend::CreateShaderTables()
{
    const UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    const UINT recordSize = Align(shaderIdentifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    auto createTable = [&](const wchar_t* name, std::initializer_list<const wchar_t*> exports, ShaderTableInfo& table)
    {
        table.recordSize = recordSize;
        table.recordCount = static_cast<UINT>(exports.size());
        const UINT64 bufferSize = static_cast<UINT64>(recordSize) * table.recordCount;
        table.resource = CreateUploadBuffer(nullptr, bufferSize, name);
        UINT8* mapped = nullptr;
        ThrowIfFailed(table.resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        UINT index = 0;
        for (const wchar_t* exportName : exports)
        {
            void* identifier = m_stateObjectProperties->GetShaderIdentifier(exportName);
            if (!identifier)
            {
                throw std::runtime_error("Failed to resolve a Path Tracing shader identifier.");
            }
            memcpy(mapped + index * recordSize, identifier, shaderIdentifierSize);
            ++index;
        }
        table.resource->Unmap(0, nullptr);
    };

    createTable(L"RayGen Shader Table", { RayGenShaderName }, m_rayGenTable);
    createTable(L"Primary Visibility RayGen Shader Table", { PrimaryRayGenShaderName }, m_primaryRayGenTable);
    createTable(L"Compact Secondary RayGen Shader Table", { SecondaryRayGenShaderName }, m_secondaryRayGenTable);
    createTable(L"MCP Review Probe RayGen Shader Table", { ReviewProbeRayGenShaderName }, m_reviewProbeRayGenTable);
    createTable(L"Miss Shader Table", { MissShaderName, ShadowMissShaderName }, m_missTable);
    createTable(L"HitGroup Shader Table", { HitGroupName, ShadowHitGroupName }, m_hitGroupTable);

    m_dispatchRaysCommandSignature.Reset();
    if (UsesCompactSecondaryWorkList())
    {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
        D3D12_COMMAND_SIGNATURE_DESC signature{};
        signature.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
        signature.NumArgumentDescs = 1u;
        signature.pArgumentDescs = &argument;
        ThrowIfFailed(m_device->CreateCommandSignature(
            &signature,
            nullptr,
            IID_PPV_ARGS(&m_dispatchRaysCommandSignature)));

        D3D12_DISPATCH_RAYS_DESC dispatchTemplate{};
        dispatchTemplate.RayGenerationShaderRecord.StartAddress =
            m_secondaryRayGenTable.resource->GetGPUVirtualAddress();
        dispatchTemplate.RayGenerationShaderRecord.SizeInBytes =
            m_secondaryRayGenTable.recordSize;
        dispatchTemplate.MissShaderTable.StartAddress =
            m_missTable.resource->GetGPUVirtualAddress();
        dispatchTemplate.MissShaderTable.SizeInBytes =
            m_missTable.recordSize * m_missTable.recordCount;
        dispatchTemplate.MissShaderTable.StrideInBytes =
            m_missTable.recordSize;
        dispatchTemplate.HitGroupTable.StartAddress =
            m_hitGroupTable.resource->GetGPUVirtualAddress();
        dispatchTemplate.HitGroupTable.SizeInBytes =
            m_hitGroupTable.recordSize * m_hitGroupTable.recordCount;
        dispatchTemplate.HitGroupTable.StrideInBytes =
            m_hitGroupTable.recordSize;
        dispatchTemplate.Width = 1u;
        dispatchTemplate.Height = 1u;
        dispatchTemplate.Depth = 1u;
        ComPtr<ID3D12Resource> templateUpload = CreateUploadBuffer(
            &dispatchTemplate,
            sizeof(dispatchTemplate),
            L"Compact DispatchRays Argument Template");
        auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            m_secondaryIndirectArgs.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1u, &toCopyDest);
        m_commandList->CopyBufferRegion(
            m_secondaryIndirectArgs.Get(),
            0u,
            templateUpload.Get(),
            0u,
            sizeof(dispatchTemplate));
        auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            m_secondaryIndirectArgs.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1u, &toUav);
        m_uploadBuffers.push_back(templateUpload);
    }
}

void D3D12PathTracingBackend::OnUpdate()
{
    const auto cpuUpdateStart = std::chrono::steady_clock::now();
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        m_benchmarkCpuFrameStart = cpuUpdateStart;
        m_benchmarkCpuFenceWaitMs = 0.0;
        m_benchmarkCpuUpdateMs = 0.0;
        m_benchmarkCpuMcpMs = 0.0;
        m_benchmarkCpuUiMs = m_pendingEditorCpuMs;
        m_pendingEditorCpuMs = 0.0;
        m_benchmarkCpuCommandRecordingMs = 0.0;
        m_benchmarkCpuPresentMs = 0.0;
        m_benchmarkCpuNrdRecordingMs = 0.0;
    }

    if (m_resizePending)
    {
        m_resizePending = false;
        Resize(m_pendingResizeWidth, m_pendingResizeHeight);
    }

    PollAsyncSceneLoad();

    if (!m_pendingProjectPath.empty())
    {
        const std::wstring path = std::move(m_pendingProjectPath);
        m_pendingProjectPath.clear();
        std::string diagnostics;
        LogDiagnostic(L"Pending project load: " + path);
        if (!LoadProjectFromDisk(path, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            LogDiagnostic("Pending project load failed: " + diagnostics);
        }
        else
        {
            LogDiagnostic("Pending project load succeeded: " + diagnostics);
        }
    }
    if (!m_pendingScenePath.empty())
    {
        const std::wstring path = std::move(m_pendingScenePath);
        m_pendingScenePath.clear();
        LogDiagnostic(L"Pending scene load: " + path);
        BeginAsyncSceneLoad(path);
    }
    if (!m_pendingEnvironmentPath.empty())
    {
        const std::wstring path = std::move(m_pendingEnvironmentPath);
        m_pendingEnvironmentPath.clear();
        std::string diagnostics;
        LogDiagnostic(L"Pending environment load: " + path);
        if (!LoadEnvironmentPath(path, diagnostics))
        {
            m_projectDiagnostics = diagnostics;
            LogDiagnostic("Pending environment load failed: " + diagnostics);
        }
    }
    if (m_pendingGpuResourceRefresh != PendingGpuResourceRefresh::None)
    {
        const PendingGpuResourceRefresh refresh = m_pendingGpuResourceRefresh;
        m_pendingGpuResourceRefresh = PendingGpuResourceRefresh::None;
        if (refresh == PendingGpuResourceRefresh::FullScene)
        {
            LogDiagnostic("Pending full-scene GPU resource refresh.");
            CreateGpuResourcesForCurrentScene();
        }
        else
        {
            const bool reloadTextures = refresh == PendingGpuResourceRefresh::MaterialTextures;
            LogDiagnostic(reloadTextures
                ? "Pending material texture GPU refresh."
                : "Pending material data GPU refresh.");
            RefreshEditableGpuResources(reloadTextures, true);
        }
        m_projectDirty = true;
    }

    const auto mcpCommandStart = std::chrono::steady_clock::now();
    ProcessMcpCommands();
    ProcessMcpBenchmark();
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        m_benchmarkCpuMcpMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - mcpCommandStart).count();
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_minimized)
    {
        // OnRender has no submission while minimized. Do not advance temporal
        // parity, jitter, previous matrices, or sample blocks without a GPU
        // frame that actually writes the corresponding current histories.
        m_lastUpdate = now;
        const auto mcpSnapshotStart = std::chrono::steady_clock::now();
        ProcessMcpReview();
        UpdateMcpSnapshots();
        if (m_benchmarkHarness && !m_benchmarkFinished)
        {
            m_benchmarkCpuMcpMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - mcpSnapshotStart).count();
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - cpuUpdateStart).count();
            m_benchmarkCpuUpdateMs = (std::max)(elapsed - m_benchmarkCpuFenceWaitMs, 0.0);
        }
        return;
    }
    float deltaSeconds = std::chrono::duration<float>(now - m_lastUpdate).count();
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        if (!m_benchmarkHarness->GetFramePlan(m_benchmarkFrameIndex, m_benchmarkFramePlan, m_benchmarkDiagnostics))
        {
            throw std::runtime_error(m_benchmarkDiagnostics);
        }
        const auto& pose = m_benchmarkFramePlan.camera;
        m_camera.Reset(XMFLOAT3(pose.position[0], pose.position[1], pose.position[2]), pose.yaw, pose.pitch);
        m_camera.SetActive(false);
        deltaSeconds = static_cast<float>(m_benchmarkFramePlan.deltaSeconds);
        if (pose.cut)
        {
            InvalidateHistory(rb::FrameChangeMask::CameraCut);
        }
    }
    else
    {
        m_camera.SetActive(m_viewportFocused);
        m_camera.Update(deltaSeconds);
    }
    m_lastUpdate = now;
    UpdateConstantBuffer(deltaSeconds);
    const auto mcpSnapshotStart = std::chrono::steady_clock::now();
    ProcessMcpReview();
    UpdateMcpSnapshots();
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        m_benchmarkCpuMcpMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - mcpSnapshotStart).count();
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuUpdateStart).count();
        m_benchmarkCpuUpdateMs = (std::max)(elapsed - m_benchmarkCpuFenceWaitMs, 0.0);
    }
}

void D3D12PathTracingBackend::UpdateConstantBuffer(float)
{
    WaitForFrameContext(m_frameIndex);

    const float aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
    XMMATRIX view = m_camera.GetViewMatrix();
    XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(m_cameraFovDegrees), aspectRatio, 0.1f, 10000.0f);
    XMMATRIX viewProjection = view * projection;
    XMMATRIX inverseViewProjection = XMMatrixInverse(nullptr, viewProjection);
    const XMFLOAT4X4 previousNrdViewToClip = m_nrdViewToClip;
    const XMFLOAT4X4 previousNrdWorldToView = m_nrdWorldToView;
    XMFLOAT4X4 currentNrdViewToClip{};
    XMFLOAT4X4 currentNrdWorldToView{};
    // DirectXMath uses row vectors and XMFLOAT4X4 stores rows contiguously.
    // NRD consumes column vectors from column-major memory, so the same bytes
    // already describe the mathematically transposed transform NRD needs.
    // Explicitly transposing here would therefore transpose the transform twice.
    XMStoreFloat4x4(&currentNrdViewToClip, projection);
    XMStoreFloat4x4(&currentNrdWorldToView, view);
    m_nrdViewToClip = currentNrdViewToClip;
    m_nrdWorldToView = currentNrdWorldToView;
    m_nrdViewToClipPrev = m_hasPreviousViewProjection ? previousNrdViewToClip : currentNrdViewToClip;
    m_nrdWorldToViewPrev = m_hasPreviousViewProjection ? previousNrdWorldToView : currentNrdWorldToView;

    const XMFLOAT4 framePreviousCameraMotionState = m_previousCameraMotionState;
    const float framePreviousCameraPitch = m_previousCameraMotionPitch;
    UpdateCameraMotionState();
    UpdateRayBudget();
    UpdateDynamicResolution();

    // HasAccumulationStateChanged consumes m_resetAccumulationRequested.
    // Calling the public ResetAccumulation() here would set that request again
    // and create a permanent reset loop (the reference sample count would stay
    // at one forever). Apply the consumed reset directly, after the dynamic ray
    // budget has selected this frame's integrand, so a budget transition is not
    // discovered one frame late.
    if (HasAccumulationStateChanged())
    {
        m_accumulatedFrames = 0;
        m_validHistoryDomains &= ~rb::HistoryDomain::ReferenceAccumulation;
    }

    const XMFLOAT4X4 previousViewProjection = m_previousViewProjection;
    float jitterStrength = 1.0f;
    if (!m_cameraJitter || m_jitterMode == JitterMode::Off)
    {
        jitterStrength = 0.0f;
    }
    m_currentJitterStrength = std::clamp(jitterStrength, 0.0f, 1.0f);

    uint32_t jitterIndex = (m_frameCounter & 1023u) + 1u;
    if (m_jitterMode == JitterMode::Stable32)
    {
        jitterIndex = (m_frameCounter & 31u) + 1u;
    }
    const XMFLOAT2 baseJitter(Halton(jitterIndex, 2) - 0.5f, Halton(jitterIndex, 3) - 0.5f);
    m_currentJitter = m_currentJitterStrength > 0.0f
        ? XMFLOAT2(baseJitter.x * m_currentJitterStrength, baseJitter.y * m_currentJitterStrength)
        : XMFLOAT2(0.0f, 0.0f);
    const rb::HistoryDomain denoiserHistoryDomains = rb::HistoryDomain::Surface | rb::HistoryDomain::Denoiser;
    const bool temporalHistoryValid = m_temporalStabilityEnabled &&
        rb::HasAll(m_validHistoryDomains, denoiserHistoryDomains) &&
        m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested;
    const float taaDenoiserMaturity = temporalHistoryValid
        ? std::clamp(static_cast<float>(m_framesSinceCameraMotion) / 32.0f, 0.0f, 1.0f)
        : 0.0f;

    SceneConstantBuffer constants{};
    XMStoreFloat4x4(&constants.inverseViewProjection, inverseViewProjection);
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    constants.previousViewProjection = m_hasPreviousViewProjection ? previousViewProjection : constants.viewProjection;
    XMFLOAT3 cameraPosition = m_camera.GetPosition();
    constants.cameraPosition = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    XMFLOAT3 lightDirection = NormalizeFloat3(m_lightDirection);
    constants.lightDirection = XMFLOAT4(lightDirection.x, lightDirection.y, lightDirection.z, 0.0f);
    constants.lightColor = XMFLOAT4(m_lightColor[0], m_lightColor[1], m_lightColor[2], m_lightIntensity);
    constants.debugOptions = XMFLOAT4(static_cast<float>(m_debugViewMode), m_debugNormalMapYFlip ? 1.0f : 0.0f, m_shadowEnabled ? 1.0f : 0.0f, m_skyNeeEnabled ? 1.0f : 0.0f);
    constants.skyColor = XMFLOAT4(m_skyColor[0], m_skyColor[1], m_skyColor[2], m_skyIntensity);
    constants.skyHorizonColor = XMFLOAT4(m_skyHorizonColor[0], m_skyHorizonColor[1], m_skyHorizonColor[2], 0.0f);
    constants.skyZenithColor = XMFLOAT4(m_skyZenithColor[0], m_skyZenithColor[1], m_skyZenithColor[2], 0.0f);
    constants.skyGroundColor = XMFLOAT4(m_skyGroundColor[0], m_skyGroundColor[1], m_skyGroundColor[2], 0.0f);
    constants.skyOptions = XMFLOAT4(m_sunIntensity, m_sunAngularRadius, m_skyGroundBlend, m_skyEnabled ? 1.0f : 0.0f);
    constants.rayOptions = XMFLOAT4(
        m_rayTMin,
        m_rayTMax,
        static_cast<float>(m_renderWidth),
        static_cast<float>(m_renderHeight));
    // Keep the submitted-frame serial as an integer in the shared ABI. A float
    // loses odd/even precision after 2^24 submissions and would otherwise pin
    // temporal ping-pong selection to one side during long-running sessions.
    constants.frameOptions = XMUINT4(
        m_accumulatedFrames,
        static_cast<uint32_t>(std::max(m_maxAccumulatedFrames, 1)),
        m_freezeAccumulation ? 1u : 0u,
        m_frameCounter);
    constants.giOptions = XMFLOAT4(static_cast<float>(m_giSamplesPerFrame), m_giRadianceClamp, m_giTemporalClampScale, m_giTemporalClampMin);
    const bool useRestir = m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        m_rtxdiAvailable && m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi && UsesRestirDI(m_mode);
    constants.pathOptions = XMFLOAT4(static_cast<float>(m_maxPathBounces), static_cast<float>(m_minPathBounces), static_cast<float>(m_restirCandidateSamples), useRestir ? 1.0f : 0.0f);
    constants.restirOptions = XMFLOAT4(m_restirTemporalReuse ? 1.0f : 0.0f, static_cast<float>(m_restirSpatialReusePasses), static_cast<float>(m_restirSpatialRadius), m_restirMClamp);
    const bool combinedRestir = m_mode == PathTracingMode::ReSTIRCombined ||
        m_mode == PathTracingMode::ReSTIRPTCombined;
    constants.restirDiOptions = XMFLOAT4(
        (combinedRestir ? m_restirDiTemporalReuse : m_restirTemporalReuse) ? 1.0f : 0.0f,
        static_cast<float>(combinedRestir ? m_restirDiSpatialReusePasses : m_restirSpatialReusePasses),
        static_cast<float>(combinedRestir ? m_restirDiCandidateSamples : m_restirCandidateSamples),
        combinedRestir ? m_restirDiMClamp : m_restirMClamp);
    constants.lightOptions = XMFLOAT4(static_cast<float>(m_activeLightCount), m_emissiveLightsEnabled ? m_emissiveLightIntensity : 0.0f, m_proceduralLightsEnabled ? m_proceduralLightIntensity : 0.0f, static_cast<float>(m_environmentDescriptorIndex));
    constants.environmentOptions = XMFLOAT4(
        m_environmentMapEnabled ? (m_environmentEqualAreaMapping ? 2.0f : 1.0f) : 0.0f,
        m_environmentIntensity,
        m_environmentRotation,
        static_cast<float>(static_cast<uint32_t>(m_validHistoryDomains)));
    constants.environmentLightToWorld = m_environmentLightToWorld;
    constants.environmentWorldToLight = m_environmentWorldToLight;
    constants.environmentTint = XMFLOAT4(m_environmentTint.x, m_environmentTint.y, m_environmentTint.z, 1.0f);
    constants.denoiseOptions = XMFLOAT4(ShouldRunInternalDenoiser() ? 1.0f : 0.0f, static_cast<float>(m_denoiserSpatialIterations), m_denoiserNormalSigma, m_denoiserDepthSigma);
    const float taaSettleProgress = std::clamp(
        static_cast<float>(m_framesSinceCameraMotion) /
            static_cast<float>((std::max)(m_qualitySettings.rayBudget.settleFrames, 1u)),
        0.0f,
        1.0f);
    constants.denoiseOptions2 = XMFLOAT4(
        m_denoiserLuminanceSigma,
        m_denoiserAlbedoSigma,
        m_denoiserStrength,
        taaSettleProgress);
    constants.jitterOptions = XMFLOAT4(m_currentJitter.x, m_currentJitter.y, m_previousJitter.x, m_previousJitter.y);
    constants.reconstructionOptions = XMFLOAT4(m_realtimeReconstruction ? 1.0f : 0.0f, static_cast<float>(m_reconstructionMaxHistoryFrames), m_temporalAlphaMin, m_temporalAlphaMax);
    constants.validationOptions = XMFLOAT4(m_validationNormalDotThreshold, m_validationDepthRelativeThreshold, m_validationAlbedoThreshold, m_validationRoughnessThreshold);
    constants.atrousOptions = XMFLOAT4(static_cast<float>(m_atrousPassCount), m_atrousDiffuseStrength, m_atrousSpecularStrength, m_atrousVarianceScale);
    const bool finalTaaActive = (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill;
    // Internal mode classifies from its explicit moments; external denoisers
    // classify after the first path sample against immutable HDR TAA history.
    // Reference/off graphs never sample the 1x1 placeholder histories.
    const bool commonVarianceHistoryAvailable = ShouldRunInternalDenoiser() || finalTaaActive;
    constants.adaptiveOptions = XMFLOAT4(m_adaptiveSamplingEnabled && commonVarianceHistoryAvailable ? 1.0f : 0.0f, static_cast<float>(m_maxAdaptiveSamplesPerPixel), m_adaptiveVarianceThreshold, m_adaptiveDisocclusionBoost);
    constants.restirStabilityOptions = XMFLOAT4(m_reservoirReprojection ? 1.0f : 0.0f, m_reservoirValidation ? 1.0f : 0.0f, m_restirGiValidationRay ? 1.0f : 0.0f, static_cast<float>(m_reservoirMaxAge));
    constants.signalDenoiseOptions = XMFLOAT4(m_splitSignalDenoise ? 1.0f : 0.0f, m_historyClampSigma, m_reactiveThreshold, m_specularHistoryScale);
    constants.denoisePassOptions = XMFLOAT4(
        IsNrdSelected() && m_denoiseBackend == DenoiseBackend::NrdRelax ? 1.0f : 0.0f,
        static_cast<float>(m_atrousPassCount),
        finalTaaActive ? 1.0f : 0.0f,
        finalTaaActive ? m_qualitySettings.sharpenStrength : 0.0f);
    constants.stabilityOptions = XMFLOAT4(
        m_temporalStabilityEnabled ? 1.0f : 0.0f,
        m_cameraMotionAmount,
        m_currentJitterStrength,
        taaDenoiserMaturity);
    // viewOptions.w is the explicit GPU resource contract for SurfaceGuides
    // and LightingSignals. Never infer writable dimensions from radiance clamp:
    // Reference Still permits arbitrary unclamped radiance while normally
    // binding 1x1 placeholders. Benchmark reference captures opt into full AOVs.
    const bool writeSurfaceSignals =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill ||
        m_benchmarkHarness != nullptr;
    constants.viewOptions = XMFLOAT4(
        m_exposure,
        m_gamma,
        static_cast<float>(m_toneMapper),
        writeSurfaceSignals ? 1.0f : 0.0f);
    constants.materialFocusOptions = XMFLOAT4(
        static_cast<float>(m_materialFocusMode),
        static_cast<float>(m_selectedMaterial),
        static_cast<float>(m_samplingSeed & 0xffffu),
        static_cast<float>(m_samplingSeed >> 16u));
    const float cameraRayConeSpread = 2.0f * std::tan(XMConvertToRadians(m_cameraFovDegrees) * 0.5f) /
        static_cast<float>((std::max)(m_renderHeight, 1u));
    const bool denoiserRequestedForFrame = m_denoiserEnabled || m_debugViewMode >= 16;
    const bool denoiserConsumesSplitSignals = denoiserRequestedForFrame &&
        (ShouldRunInternalDenoiser() || (IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate()));
    constants.performanceOptions = XMFLOAT4(
        cameraRayConeSpread,
        m_activeSecondaryShadingRate,
        (m_benchmarkHarness && lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind)) ? 1.0f : 0.0f,
        denoiserConsumesSplitSignals ? 1.0f : 0.0f);
    m_fuseNrdFinalTaaForFrame = ShouldFuseNrdFinalTaa();
    const bool dlssEvaluationPlanned =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        m_denoiserEnabled &&
        m_debugViewMode == 0 &&
        IsDlssSelected() &&
        m_dlssBackendRuntime.CanEvaluateRayReconstruction() &&
        m_dlssPreparePipeline;
    constants.postProcessOptions = XMFLOAT4(
        m_fuseNrdFinalTaaForFrame ? 1.0f : 0.0f,
        dlssEvaluationPlanned ? 1.0f : 0.0f,
        UsesCompactSecondaryWorkList()
            ? (UsesRestirDI(m_mode) ? 2.0f : 1.0f)
            : 0.0f,
        0.0f);
    float areaLightPower = 0.0f;
    for (uint32_t lightIndex = 0; lightIndex < m_activeLightCount; ++lightIndex)
    {
        const Bistro::RtLight& light = m_lights[lightIndex];
        const float radianceLuminance =
            0.2126f * light.radianceCdf.x +
            0.7152f * light.radianceCdf.y +
            0.0722f * light.radianceCdf.z;
        const bool emissiveTriangle = light.edge0Type.w < 0.5f;
        const float liveMultiplier = emissiveTriangle
            ? (m_emissiveLightsEnabled ? m_emissiveLightIntensity : 0.0f)
            : (m_proceduralLightsEnabled ? m_proceduralLightIntensity : 0.0f);
        areaLightPower += (std::max)(light.positionArea.w * radianceLuminance * liveMultiplier, 0.0f);
    }
    // Preserve global power importance while reserving enough probability for
    // Sun/HDRI when hundreds of locally sparse emissive triangles dominate the
    // scene-wide area-light sum.
    constants.unifiedLightOptions = XMFLOAT4(
        areaLightPower,
        0.20f,
        0.0f,
        0.0f);
    constants.renderOutputOptions = XMFLOAT4(
        static_cast<float>(m_renderWidth),
        static_cast<float>(m_renderHeight),
        static_cast<float>(m_width),
        static_cast<float>(m_height));
    const XMMATRIX previousInverseViewProjection = m_hasPreviousViewProjection
        ? XMMatrixInverse(nullptr, XMLoadFloat4x4(&previousViewProjection))
        : inverseViewProjection;
    XMStoreFloat4x4(
        &constants.previousInverseViewProjection,
        previousInverseViewProjection);
    constants.previousCameraPosition = m_hasPreviousViewProjection
        ? XMFLOAT4(
            framePreviousCameraMotionState.x,
            framePreviousCameraMotionState.y,
            framePreviousCameraMotionState.z,
            1.0f)
        : constants.cameraPosition;
    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    memcpy(frameContext.mappedSceneConstants, &constants, sizeof(constants));

    const XMFLOAT2 framePreviousJitter = m_previousJitter;
    m_nrdCurrentJitterForFrame = m_currentJitter;
    m_nrdPreviousJitterForFrame = framePreviousJitter;
    XMStoreFloat4x4(&m_previousViewProjection, viewProjection);
    m_hasPreviousViewProjection = true;
    m_previousJitter = m_currentJitter;
    m_resetDenoiseHistoryRequested = false;
    m_denoiseHistoryValid = true;

    m_frameState.frameNumber = m_frameCounter;
    m_frameState.width = m_width;
    m_frameState.height = m_height;
    m_frameState.changes = m_pendingFrameChanges;
    m_frameState.cameraCut = rb::HasAny(
        m_pendingFrameChanges,
        rb::FrameChangeMask::CameraCut | rb::FrameChangeMask::Projection | rb::FrameChangeMask::Resolution);
    m_frameState.previousCameraPosition = {
        framePreviousCameraMotionState.x,
        framePreviousCameraMotionState.y,
        framePreviousCameraMotionState.z };
    m_frameState.currentCameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
    m_frameState.previousYaw = framePreviousCameraMotionState.w;
    m_frameState.currentYaw = m_camera.GetYawRadians();
    m_frameState.previousPitch = framePreviousCameraPitch;
    m_frameState.currentPitch = m_camera.GetPitchRadians();
    m_frameState.currentJitterPixels = { m_currentJitter.x, m_currentJitter.y };
    m_frameState.previousJitterPixels = { framePreviousJitter.x, framePreviousJitter.y };
    m_pendingFrameChanges = rb::FrameChangeMask::None;

    if (!m_freezeAccumulation)
    {
        m_accumulatedFrames = (std::min)(m_accumulatedFrames + 1u, static_cast<uint32_t>((std::max)(m_maxAccumulatedFrames, 1)));
    }
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill)
    {
        m_validHistoryDomains &= ~rb::HistoryDomain::Realtime;
    }
    else
    {
        rb::HistoryDomain producedDomains = rb::HistoryDomain::Surface | rb::HistoryDomain::Lighting;
        const bool denoiserRequested = m_denoiserEnabled || m_debugViewMode >= 16;
        if (ShouldRunInternalDenoiser() ||
            (denoiserRequested && IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate()))
        {
            producedDomains |= rb::HistoryDomain::Denoiser;
        }
        // All diagnostic views bypass FinalTaaCS before StoreCurrentHistory.
        // Modes 47-48 are generated by the TAA pass itself and therefore keep
        // the TAA domain valid while they are displayed.
        if (finalTaaActive && (m_debugViewMode == 0 || m_debugViewMode >= 47))
        {
            producedDomains |= rb::HistoryDomain::Taa;
        }
        m_validHistoryDomains &= ~rb::HistoryDomain::Realtime;
        m_validHistoryDomains |= producedDomains;
    }
    if (m_accumulatedFrames > 0)
    {
        m_validHistoryDomains |= rb::HistoryDomain::ReferenceAccumulation;
    }
    m_frameState.progressiveSampleCount = m_accumulatedFrames;
    m_frameState.validHistoryDomains = m_validHistoryDomains;
}

void D3D12PathTracingBackend::OnRender()
{
    if (m_minimized)
    {
        return;
    }

    if (m_frameLatencyWaitableObject)
    {
        WaitForSingleObjectEx(
            m_frameLatencyWaitableObject,
            1000,
            FALSE);
    }
    const UINT submittedFrameContextIndex = m_frameIndex;
    const double fenceBeforeRecording = m_benchmarkCpuFenceWaitMs;
    const auto commandRecordingStart = std::chrono::steady_clock::now();
    PopulateCommandList();
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - commandRecordingStart).count();
        m_benchmarkCpuCommandRecordingMs = (std::max)(
            elapsed - (m_benchmarkCpuFenceWaitMs - fenceBeforeRecording), 0.0);
    }
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    const UINT syncInterval = m_vsyncEnabled ? 1 : 0;
    const auto presentStart = std::chrono::steady_clock::now();
    ThrowIfFailed(m_swapChain->Present(syncInterval, 0));
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        m_benchmarkCpuPresentMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - presentStart).count();
    }
    const UINT64 submissionSerial = SignalFrameAndAdvance();
    // FrameState.frameNumber is a submitted-frame sequence, independent of
    // progressive accumulation. Advance it only after this frame was actually
    // executed and presented, including while accumulation is frozen.
    ++m_frameCounter;
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        const double cpuFrameElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - m_benchmarkCpuFrameStart).count();
        const double cpuFrameMs = (std::max)(cpuFrameElapsedMs - m_benchmarkCpuFenceWaitMs, 0.0);
        const bool finalSubmission = m_benchmarkFrameIndex + 1u == m_benchmarkHarness->TotalFrames();
        StageBenchmarkFrameForSubmission(
            submittedFrameContextIndex,
            submissionSerial,
            cpuFrameMs,
            m_benchmarkCpuFenceWaitMs);
        if (m_benchmarkHarness->ShouldCaptureFrame(m_benchmarkFrameIndex))
        {
            std::string captureDiagnostics;
            if (!SaveBenchmarkFrameArtifacts(m_benchmarkFrameIndex, captureDiagnostics))
            {
                throw std::runtime_error("Benchmark sequence capture failed: " + captureDiagnostics);
            }
        }
        ++m_benchmarkFrameIndex;
        if (finalSubmission)
        {
            FinalizeBenchmarkRun();
        }
    }
}

void D3D12PathTracingBackend::StageBenchmarkFrameForSubmission(
    UINT frameContextIndex,
    UINT64 submissionSerial,
    double cpuFrameMs,
    double cpuFenceWaitMs)
{
    const auto aggregationStart = std::chrono::steady_clock::now();
    if (!m_benchmarkHarness || m_benchmarkFinished || frameContextIndex >= FrameCount ||
        m_benchmarkFrameIndex >= m_benchmarkHarness->TotalFrames())
    {
        throw std::runtime_error("Benchmark submission snapshot is outside the active run.");
    }

    FrameContext& frameContext = m_frameContexts[frameContextIndex];
    if (frameContext.benchmarkMetricsPending || frameContext.submissionSerial != submissionSerial)
    {
        throw std::runtime_error("Benchmark frame context was reused before its metrics completed.");
    }

    const double primaryRays = static_cast<double>(m_renderWidth) * static_cast<double>(m_renderHeight) *
        static_cast<double>((std::max)(m_giSamplesPerFrame, 1));
    const bool targetAdapter = m_adapterDescription.find(L"RTX 4070") != std::wstring::npos;
    const bool targetScene = m_scenePath.find(L"BistroExterior") != std::wstring::npos ||
        m_scenePath.find(L"BistroInterior") != std::wstring::npos;
    const bool interactiveProfile = m_qualitySettings.qualityProfile == rb::QualityProfile::InteractiveGame;
    const bool beautyView = m_debugViewMode == 0;
    const bool finalTaaActive = (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill && m_finalTaaPipeline;
    const bool requestedNrdReblur = m_denoiseBackend == DenoiseBackend::NrdReblur;
    const bool activeNrdReblur = requestedNrdReblur && m_denoiserEnabled && m_nrdBackendRuntime.CanEvaluate();
    const DlssStatus& dlssStatus = m_dlssBackendRuntime.Status();
    const bool requestedDlssRr = IsDlssSelected() && m_denoiserEnabled;
    const bool requestedRtxdi = m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi &&
        m_mode == PathTracingMode::ReSTIRCombined;
    // The target benchmark mode requests both DI and GI. Keep the combined
    // gate false until RTXDI ReSTIR PT/GI is also evaluation-ready; DI-only
    // integration must not be misreported as the completed combined backend.
    const RtxdiStatus& rtxdiStatus = m_rtxdiBackendRuntime.Status();
    const bool activeRtxdi = requestedRtxdi && m_rtxdiAvailable &&
        rtxdiStatus.diEvaluationReady && rtxdiStatus.giEvaluationReady &&
        m_rtxdiDiCandidatePipeline && m_rtxdiDiSpatialPipeline &&
        m_rtxdiGiInitialPipeline && m_rtxdiGiFusedPipeline;
    const bool requestedRtxdiPt =
        m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi &&
        UsesRestirPT(m_mode);
    const bool activeRtxdiPt = requestedRtxdiPt && m_rtxdiAvailable &&
        rtxdiStatus.ptEvaluationReady &&
        m_rtxdiPtInitialPipeline && m_rtxdiPtFusedPipeline &&
        (!UsesRestirDI(m_mode) ||
            (rtxdiStatus.diEvaluationReady &&
             m_rtxdiDiCandidatePipeline && m_rtxdiDiSpatialPipeline));
    const rb::RayBudgetSettings& configuredBudget = m_qualitySettings.rayBudget;
    frameContext.benchmarkFrameIndex = m_benchmarkFrameIndex;
    frameContext.benchmarkMetrics =
    {
        { "cpu_frame_ms", cpuFrameMs },
        { "cpu_fence_wait_ms", cpuFenceWaitMs },
        { "cpu_update_ms", m_benchmarkCpuUpdateMs },
        { "cpu_mcp_ms", m_benchmarkCpuMcpMs },
        { "cpu_ui_ms", m_benchmarkCpuUiMs },
        { "cpu_command_recording_ms", m_benchmarkCpuCommandRecordingMs },
        { "cpu_present_ms", m_benchmarkCpuPresentMs },
        { "cpu_nrd_recording_ms", m_benchmarkCpuNrdRecordingMs },
        { "cpu_benchmark_aggregate_ms", 0.0 },
        { "gpu_pipeline_ms", -1.0 },
        { "gpu_path_trace_ms", -1.0 },
        { "gpu_restir_ms", -1.0 },
        { "gpu_restir_candidate_ms", -1.0 },
        { "gpu_restir_temporal_ms", -1.0 },
        { "gpu_restir_spatial_ms", -1.0 },
        { "gpu_restir_shade_ms", -1.0 },
        { "gpu_restir_publish_ms", -1.0 },
        { "gpu_restir_gi_initial_ms", -1.0 },
        { "gpu_restir_gi_fused_ms", -1.0 },
        { "gpu_restir_pt_initial_ms", -1.0 },
        { "gpu_restir_pt_fused_ms", -1.0 },
        { "gpu_denoise_ms", -1.0 },
        { "gpu_denoise_prepare_ms", -1.0 },
        { "gpu_denoise_core_ms", -1.0 },
        { "gpu_denoise_composite_ms", -1.0 },
        { "gpu_final_taa_ms", -1.0 },
        { "gpu_quality_counters_ms", -1.0 },
        { "gpu_history_publish_ms", -1.0 },
        { "gpu_copy_ms", -1.0 },
        { "gpu_ui_ms", -1.0 },
        { "gpu_timing_valid", 0.0 },
        { "submission_serial", static_cast<double>(submissionSerial) },
        { "gpu_timing_serial", -1.0 },
        { "frame_context_index", static_cast<double>(frameContextIndex) },
        { "primary_rays_estimated", primaryRays },
        { "path_ray_budget_estimated", primaryRays * static_cast<double>((std::max)(m_maxPathBounces, 1)) },
        { "samples_per_pixel", static_cast<double>(m_giSamplesPerFrame) },
        { "max_bounces", static_cast<double>(m_maxPathBounces) },
        { "active_secondary_rate", static_cast<double>(m_activeSecondaryShadingRate) },
        { "secondary_half_active", m_activeSecondaryShadingRate < 0.75f ? 1.0 : 0.0 },
        { "secondary_ray_budget_estimated",
            primaryRays * static_cast<double>((std::max)(m_maxPathBounces - 1, 0)) *
                static_cast<double>(m_activeSecondaryShadingRate) },
        { "diagnostic_counters_enabled",
            lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind) ? 1.0 : 0.0 },
        { "camera_motion", static_cast<double>(m_cameraMotionAmount) },
        { "history_valid", m_denoiseHistoryValid && !m_resetDenoiseHistoryRequested ? 1.0 : 0.0 },
        { "accumulated_samples", static_cast<double>(m_accumulatedFrames) },
        { "frame_history_mib", static_cast<double>(m_frameHistoryResourceBytes) / (1024.0 * 1024.0) },
        { "vram_frame_history_peak_mib", static_cast<double>(m_frameHistoryResourceBytes) / (1024.0 * 1024.0) },
        { "vram_restir_alias_heap_mib",
            static_cast<double>(m_restirAliasHeapSize * m_restirAliasHeaps.size()) / (1024.0 * 1024.0) },
        { "compacted_blas_bytes", static_cast<double>(m_blasCompactedBytes) },
        { "blas_compaction_ratio", m_blasOriginalBytes > 0u
            ? static_cast<double>(m_blasCompactedBytes) / static_cast<double>(m_blasOriginalBytes)
            : 1.0 },
        { "render_width", static_cast<double>(m_renderWidth) },
        { "render_height", static_cast<double>(m_renderHeight) },
        { "output_width", static_cast<double>(m_width) },
        { "output_height", static_cast<double>(m_height) },
        { "render_scale", static_cast<double>(m_activeRenderScale) },
        { "dynamic_resolution_active",
            m_qualitySettings.resolutionMode == rb::ResolutionMode::Dynamic ? 1.0 : 0.0 },
        { "taau_active", UsesTemporalUpscale() ? 1.0 : 0.0 },
        { "primary_visibility_separate", UsesCompactSecondaryWorkList() ? 1.0 : 0.0 },
        { "compact_secondary_worklist", UsesCompactSecondaryWorkList() ? 1.0 : 0.0 },
        { "secondary_execute_indirect",
            UsesCompactSecondaryWorkList() && m_dispatchRaysCommandSignature ? 1.0 : 0.0 },
        { "secondary_task_capacity", static_cast<double>(m_secondaryTaskCapacity) },
        { "requested_dlss_rr", requestedDlssRr ? 1.0 : 0.0 },
        { "dlss_runtime_available", dlssStatus.runtimeAvailable ? 1.0 : 0.0 },
        { "dlss_initialized", dlssStatus.initialized ? 1.0 : 0.0 },
        { "dlss_device_registered", dlssStatus.deviceRegistered ? 1.0 : 0.0 },
        { "dlss_application_identity_configured", dlssStatus.applicationIdentityConfigured ? 1.0 : 0.0 },
        { "dlss_feature_supported", dlssStatus.featureSupported ? 1.0 : 0.0 },
        { "dlss_evaluation_ready", dlssStatus.evaluationReady ? 1.0 : 0.0 },
        { "active_dlss_rr", requestedDlssRr && dlssStatus.lastEvaluationSucceeded ? 1.0 : 0.0 },
        { "dlss_successful_evaluations", static_cast<double>(dlssStatus.successfulEvaluations) },
        { "dlss_failed_evaluations", static_cast<double>(dlssStatus.failedEvaluations) },
        { "dlss_last_result_code", static_cast<double>(dlssStatus.lastResultCode) },
        { "target_adapter_rtx_4070", targetAdapter ? 1.0 : 0.0 },
        { "target_scene_bistro", targetScene ? 1.0 : 0.0 },
        { "quality_profile_interactive", interactiveProfile ? 1.0 : 0.0 },
        { "beauty_view", beautyView ? 1.0 : 0.0 },
        { "final_taa_active", finalTaaActive ? 1.0 : 0.0 },
        { "requested_denoiser_nrd_reblur", requestedNrdReblur ? 1.0 : 0.0 },
        { "active_denoiser_nrd_reblur", activeNrdReblur ? 1.0 : 0.0 },
        { "requested_restir_rtxdi_combined", requestedRtxdi ? 1.0 : 0.0 },
        { "active_restir_rtxdi_combined", activeRtxdi ? 1.0 : 0.0 },
        { "requested_restir_rtxdi_pt", requestedRtxdiPt ? 1.0 : 0.0 },
        { "active_restir_rtxdi_pt", activeRtxdiPt ? 1.0 : 0.0 },
        { "budget_moving_spp", static_cast<double>(configuredBudget.movingSpp) },
        { "budget_moving_bounces", static_cast<double>(configuredBudget.movingBounces) },
        { "budget_static_base_spp", static_cast<double>(configuredBudget.staticBaseSpp) },
        { "budget_static_max_spp", static_cast<double>(configuredBudget.staticMaxSpp) },
        { "budget_static_bounces", static_cast<double>(configuredBudget.staticBounces) },
        { "budget_settle_frames", static_cast<double>(configuredBudget.settleFrames) },
        { "budget_target_gpu_ms", configuredBudget.targetGpuMs },
        { "surface_history_tested_pixels", 0.0 },
        { "surface_history_accepted_pixels", 0.0 },
        { "surface_history_reject_oob_pixels", 0.0 },
        { "surface_history_reject_depth_pixels", 0.0 },
        { "surface_history_reject_normal_pixels", 0.0 },
        { "surface_history_reject_roughness_pixels", 0.0 },
        { "surface_history_reject_identity_pixels", 0.0 },
        { "surface_history_dilated_pixels", 0.0 },
        { "taa_history_tested_pixels", 0.0 },
        { "taa_history_accepted_pixels", 0.0 },
        { "disoccluded_pixels", 0.0 },
        { "contribution_input_energy", 0.0 },
        { "contribution_output_energy", 0.0 },
        { "contribution_clamped_energy", 0.0 },
        { "contribution_clamped_samples", 0.0 },
        { "non_finite_pixels", 0.0 },
        { "primary_rays_actual", 0.0 },
        { "secondary_rays_actual", 0.0 },
        { "shadow_rays_actual", 0.0 },
        { "di_visibility_rays_actual", 0.0 },
        { "gi_visibility_rays_actual", 0.0 },
        { "pt_visibility_rays_actual", 0.0 },
        { "anyhit_invocations_actual", 0.0 },
    };
    frameContext.benchmarkMetrics["cpu_benchmark_aggregate_ms"] =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - aggregationStart).count();
    frameContext.benchmarkMetricsPending = true;
}

void D3D12PathTracingBackend::CompleteBenchmarkFrame(FrameContext& frameContext, const GpuTimingSample& timing)
{
    if (!frameContext.benchmarkMetricsPending)
    {
        return;
    }

    lookdevpt::benchmark::MetricValues metrics = std::move(frameContext.benchmarkMetrics);
    metrics["gpu_pipeline_ms"] = timing.pipelineMs;
    metrics["gpu_path_trace_ms"] = timing.pathTraceMs;
    metrics["gpu_restir_ms"] = timing.restirMs;
    metrics["gpu_restir_candidate_ms"] = timing.restirCandidateMs;
    metrics["gpu_restir_temporal_ms"] = timing.restirTemporalMs;
    metrics["gpu_restir_spatial_ms"] = timing.restirSpatialMs;
    metrics["gpu_restir_shade_ms"] = timing.restirShadeMs;
    metrics["gpu_restir_publish_ms"] = timing.restirPublishMs;
    metrics["gpu_restir_gi_initial_ms"] = timing.restirGiInitialMs;
    metrics["gpu_restir_gi_fused_ms"] = timing.restirGiFusedMs;
    metrics["gpu_restir_pt_initial_ms"] = timing.restirPtInitialMs;
    metrics["gpu_restir_pt_fused_ms"] = timing.restirPtFusedMs;
    metrics["gpu_denoise_ms"] = timing.denoiseMs;
    metrics["gpu_denoise_prepare_ms"] = timing.denoisePrepareMs;
    metrics["gpu_denoise_core_ms"] = timing.denoiseCoreMs;
    metrics["gpu_denoise_composite_ms"] = timing.denoiseCompositeMs;
    metrics["gpu_final_taa_ms"] = timing.finalTaaMs;
    metrics["gpu_quality_counters_ms"] = timing.qualityCountersMs;
    metrics["gpu_history_publish_ms"] = timing.historyPublishMs;
    metrics["gpu_copy_ms"] = timing.copyMs;
    metrics["gpu_ui_ms"] = timing.uiMs;
    metrics["gpu_timing_valid"] = timing.valid ? 1.0 : 0.0;
    metrics["gpu_timing_serial"] = timing.valid ? static_cast<double>(frameContext.submissionSerial) : -1.0;

    const uint32_t benchmarkFrameIndex = frameContext.benchmarkFrameIndex;
    frameContext.benchmarkMetrics.clear();
    frameContext.benchmarkMetricsPending = false;
    if (m_completedBenchmarkMetrics.contains(benchmarkFrameIndex))
    {
        throw std::runtime_error("Benchmark frame metrics completed more than once.");
    }
    m_completedBenchmarkMetrics.emplace(benchmarkFrameIndex, std::move(metrics));
    DrainCompletedBenchmarkMetrics();
}

void D3D12PathTracingBackend::DrainCompletedBenchmarkMetrics()
{
    if (!m_benchmarkHarness)
    {
        return;
    }

    for (;;)
    {
        auto completed = m_completedBenchmarkMetrics.find(m_benchmarkRecordedFrameCount);
        if (completed == m_completedBenchmarkMetrics.end())
        {
            break;
        }
        if (!m_benchmarkHarness->RecordFrameMetrics(completed->first, completed->second, m_benchmarkDiagnostics))
        {
            throw std::runtime_error(m_benchmarkDiagnostics);
        }
        m_completedBenchmarkMetrics.erase(completed);
        ++m_benchmarkRecordedFrameCount;
    }
}

void D3D12PathTracingBackend::FinalizeBenchmarkRun()
{
    if (!m_benchmarkHarness || m_benchmarkFinished)
    {
        return;
    }

    // Wait for every submitted context, then drain completed snapshots by
    // benchmark frame index so the harness always receives strict ordering.
    WaitForPreviousFrame();
    DrainCompletedBenchmarkMetrics();
    if (m_benchmarkFrameIndex != m_benchmarkHarness->TotalFrames() ||
        m_benchmarkRecordedFrameCount != m_benchmarkHarness->TotalFrames() ||
        !m_completedBenchmarkMetrics.empty())
    {
        throw std::runtime_error("Benchmark finalization did not receive every submitted frame metric.");
    }

    std::string artifactDiagnostics;
    if (!SaveBenchmarkArtifacts(artifactDiagnostics))
    {
        const std::string failure = "Benchmark artifact capture failed: " + artifactDiagnostics;
        LogDiagnostic(failure);
        throw std::runtime_error(failure);
    }
    if (!m_benchmarkHarness->WriteOutputs(m_benchmarkDiagnostics))
    {
        throw std::runtime_error(m_benchmarkDiagnostics);
    }
    m_benchmarkFinished = true;
    LogDiagnostic(m_benchmarkDiagnostics);
}

void D3D12PathTracingBackend::PopulateCommandList()
{
    WaitForFrameContext(m_frameIndex);
    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    ThrowIfFailed(frameContext.commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(frameContext.commandAllocator.Get(), nullptr));

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    BeginGpuTimingFrame();
    if (m_benchmarkHarness &&
        lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind))
    {
        const UINT clearValues[4] = {};
        m_commandList->ClearUnorderedAccessViewUint(
            GpuDescriptor(DescriptorQualityCounterUav),
            CpuDescriptor(DescriptorQualityCounterUav),
            m_qualityCounterBuffer.Get(),
            clearValues,
            0u,
            nullptr);
        auto clearBarrier =
            CD3DX12_RESOURCE_BARRIER::UAV(m_qualityCounterBuffer.Get());
        m_commandList->ResourceBarrier(1u, &clearBarrier);
    }
    DispatchRays();
    WriteGpuTimestamp(GpuTimestampAfterPathTrace);
    RunRestirReusePass();
    RunRestirGiPass();
    RunRestirPtPass();
    RunDenoisePass();
    RunFinalTaaPass();
    if (m_dlssFallbackRebuildAfterFrame)
    {
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        m_dlssFallbackRebuildAfterFrame = false;
    }
    WriteGpuTimestamp(GpuTimestampAfterFinalTaa);
    if (!m_benchmarkHarness ||
        lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind))
    {
        RunQualityCounterPass();
    }
    WriteGpuTimestamp(GpuTimestampAfterQualityCounters);
    SealSurfaceGuideFrame();
    WriteGpuTimestamp(GpuTimestampAfterHistoryPublish);
    CopyOutputToBackBuffer();
    WriteGpuTimestamp(GpuTimestampAfterCopy);

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
    WriteGpuTimestamp(GpuTimestampFrameEnd);
    ResolveGpuTimingQueries();
    ThrowIfFailed(m_commandList->Close());
}

void D3D12PathTracingBackend::DispatchRays()
{
    if (UsesCompactSecondaryWorkList() &&
        m_dispatchRaysCommandSignature &&
        m_secondaryTaskCountPipeline &&
        m_secondaryGroupScanPipeline &&
        m_secondaryTaskScatterPipeline &&
        m_secondaryResolvePipeline)
    {
        DispatchCompactSecondaryWork();
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenTable.resource->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenTable.recordSize;
    dispatchDesc.MissShaderTable.StartAddress = m_missTable.resource->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missTable.recordSize * m_missTable.recordCount;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missTable.recordSize;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupTable.resource->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupTable.recordSize * m_hitGroupTable.recordCount;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupTable.recordSize;
    dispatchDesc.Width = m_renderWidth;
    dispatchDesc.Height = m_renderHeight;
    dispatchDesc.Depth = 1;
    m_commandList->DispatchRays(&dispatchDesc);
    const UINT currentGuideParity = CurrentSurfaceGuideParity();
    D3D12_RESOURCE_BARRIER uavBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(AccumulationResource(currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(0u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(1u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(2u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideIdentityResource(currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalCurrentRadiance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalResidual.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdDiffuseConfidence.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdSpecularConfidence.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_primaryPositionCone.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_primaryGeometricNormal.Get())
    };
    m_commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
}

void D3D12PathTracingBackend::DispatchCompactSecondaryWork()
{
    auto bindGlobalState = [this]()
    {
        m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
        m_commandList->SetComputeRootDescriptorTable(
            RootOutputTable,
            CurrentOutputTableGpuDescriptor());
        m_commandList->SetComputeRootShaderResourceView(
            RootAccelerationStructure,
            m_topLevelAs.result->GetGPUVirtualAddress());
        m_commandList->SetComputeRootConstantBufferView(
            RootSceneConstants,
            m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
        m_commandList->SetComputeRootDescriptorTable(
            RootSceneBuffers,
            GpuDescriptor(DescriptorVertexBuffer));
        m_commandList->SetComputeRootDescriptorTable(
            RootTextureTable,
            GpuDescriptor(DescriptorTextureBase));
    };
    bindGlobalState();
    m_commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC primaryDispatch{};
    primaryDispatch.RayGenerationShaderRecord.StartAddress =
        m_primaryRayGenTable.resource->GetGPUVirtualAddress();
    primaryDispatch.RayGenerationShaderRecord.SizeInBytes =
        m_primaryRayGenTable.recordSize;
    primaryDispatch.MissShaderTable.StartAddress =
        m_missTable.resource->GetGPUVirtualAddress();
    primaryDispatch.MissShaderTable.SizeInBytes =
        m_missTable.recordSize * m_missTable.recordCount;
    primaryDispatch.MissShaderTable.StrideInBytes =
        m_missTable.recordSize;
    primaryDispatch.HitGroupTable.StartAddress =
        m_hitGroupTable.resource->GetGPUVirtualAddress();
    primaryDispatch.HitGroupTable.SizeInBytes =
        m_hitGroupTable.recordSize * m_hitGroupTable.recordCount;
    primaryDispatch.HitGroupTable.StrideInBytes =
        m_hitGroupTable.recordSize;
    primaryDispatch.Width = m_renderWidth;
    primaryDispatch.Height = m_renderHeight;
    primaryDispatch.Depth = 1u;
    m_commandList->DispatchRays(&primaryDispatch);

    const UINT currentGuideParity = CurrentSurfaceGuideParity();
    D3D12_RESOURCE_BARRIER primaryBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(0u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(1u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(2u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideIdentityResource(currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalResidual.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_primaryPositionCone.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_primaryGeometricNormal.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_primaryIdentity.Get()),
    };
    m_commandList->ResourceBarrier(
        _countof(primaryBarriers),
        primaryBarriers);

    bindGlobalState();
    m_commandList->SetPipelineState(m_secondaryTaskCountPipeline.Get());
    m_commandList->Dispatch(m_secondaryGroupCount, 1u, 1u);
    D3D12_RESOURCE_BARRIER countBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryTaskOffsets.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryGroupOffsets.Get()),
    };
    m_commandList->ResourceBarrier(
        _countof(countBarriers),
        countBarriers);

    m_commandList->SetPipelineState(m_secondaryGroupScanPipeline.Get());
    m_commandList->Dispatch(1u, 1u, 1u);
    D3D12_RESOURCE_BARRIER scanBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryGroupOffsets.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryTasks.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryIndirectArgs.Get()),
    };
    m_commandList->ResourceBarrier(
        _countof(scanBarriers),
        scanBarriers);

    m_commandList->SetPipelineState(m_secondaryTaskScatterPipeline.Get());
    m_commandList->Dispatch(m_secondaryGroupCount, 1u, 1u);
    auto taskBarrier =
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryTasks.Get());
    m_commandList->ResourceBarrier(1u, &taskBarrier);

    auto argsToIndirect = CD3DX12_RESOURCE_BARRIER::Transition(
        m_secondaryIndirectArgs.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    m_commandList->ResourceBarrier(1u, &argsToIndirect);
    bindGlobalState();
    m_commandList->SetPipelineState1(m_stateObject.Get());
    m_commandList->ExecuteIndirect(
        m_dispatchRaysCommandSignature.Get(),
        1u,
        m_secondaryIndirectArgs.Get(),
        0u,
        nullptr,
        0u);
    auto argsToUav = CD3DX12_RESOURCE_BARRIER::Transition(
        m_secondaryIndirectArgs.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER secondaryBarriers[] =
    {
        argsToUav,
        CD3DX12_RESOURCE_BARRIER::UAV(m_secondaryResults.Get()),
    };
    m_commandList->ResourceBarrier(
        _countof(secondaryBarriers),
        secondaryBarriers);

    bindGlobalState();
    m_commandList->SetPipelineState(m_secondaryResolvePipeline.Get());
    m_commandList->Dispatch(
        (m_renderWidth + 7u) / 8u,
        (m_renderHeight + 7u) / 8u,
        1u);
    D3D12_RESOURCE_BARRIER resolveBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(AccumulationResource(currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalResidual.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdDiffuseConfidence.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdSpecularConfidence.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
    };
    m_commandList->ResourceBarrier(
        _countof(resolveBarriers),
        resolveBarriers);
}

void D3D12PathTracingBackend::RunRestirReusePass()
{
    if (m_pendingGpuResourceRefresh == PendingGpuResourceRefresh::FullScene ||
        !m_rtxdiAvailable || m_qualitySettings.restirBackend != rb::RestirBackend::Rtxdi ||
        !UsesRestirDI(m_mode) || m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        !m_rtxdiDiCandidatePipeline || !m_rtxdiDiSpatialPipeline)
    {
        WriteGpuTimestamp(GpuTimestampAfterRestirCandidate);
        WriteGpuTimestamp(GpuTimestampAfterRestirTemporal);
        WriteGpuTimestamp(GpuTimestampAfterRestirSpatial);
        WriteGpuTimestamp(GpuTimestampAfterRestirShade);
        WriteGpuTimestamp(GpuTimestampAfterRestirPublish);
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    const UINT dispatchX = (m_renderWidth + 7) / 8;
    const UINT dispatchY = (m_renderHeight + 7) / 8;
    const UINT currentParity = CurrentSurfaceGuideParity();
    ID3D12Resource* spatialReservoir = currentParity == 0u
        ? m_restirReservoirSpatial.Get()
        : m_restirReservoirSpatialB.Get();
    if (m_restirAliasHeapSize != 0u)
    {
        ID3D12Resource* currentIndirectReservoir = UsesRestirGI(m_mode)
            ? (currentParity == 0u
                ? m_restirGiReservoirA.Get()
                : m_restirGiReservoirB.Get())
            : (currentParity == 0u
                ? m_restirPtReservoirA.Get()
                : m_restirPtReservoirB.Get());
        auto activateDiScratch = CD3DX12_RESOURCE_BARRIER::Aliasing(
            currentIndirectReservoir,
            spatialReservoir);
        m_commandList->ResourceBarrier(1u, &activateDiScratch);
    }

    // Pass A fuses candidate generation and temporal reuse. Local candidates
    // never touch memory; the immutable previous history A is read and the
    // combined reservoir is written once to scratch B.
    m_commandList->SetPipelineState(m_rtxdiDiCandidatePipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    auto passABarrier = CD3DX12_RESOURCE_BARRIER::UAV(spatialReservoir);
    m_commandList->ResourceBarrier(1, &passABarrier);
    WriteGpuTimestamp(GpuTimestampAfterRestirCandidate);
    // Candidate and temporal retain their public metrics. Temporal is zero-ish
    // because both stages are now one dispatch measured as Pass A.
    WriteGpuTimestamp(GpuTimestampAfterRestirTemporal);

    // Pass B reads only immutable scratch B, adaptively combines four stable
    // or eight young/disoccluded neighbors, performs exact visibility/shading,
    // and publishes next history directly to A.
    m_commandList->SetPipelineState(m_rtxdiDiSpatialPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    const bool denoiserRequested = m_denoiserEnabled || m_debugViewMode >= 16;
    const bool signalOnly = denoiserRequested &&
        (ShouldRunInternalDenoiser() || (IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate()));
    if (signalOnly)
    {
        D3D12_RESOURCE_BARRIER shadeBarriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(m_restirReservoirCurrent.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get())
        };
        m_commandList->ResourceBarrier(_countof(shadeBarriers), shadeBarriers);
    }
    else
    {
        D3D12_RESOURCE_BARRIER shadeBarriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(m_restirReservoirCurrent.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(AccumulationResource(currentParity)),
            CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get())
        };
        m_commandList->ResourceBarrier(_countof(shadeBarriers), shadeBarriers);
    }
    WriteGpuTimestamp(GpuTimestampAfterRestirSpatial);
    // Spatial and shade retain their public metrics. Shade is zero-ish because
    // both stages are now one dispatch measured as Pass B.
    WriteGpuTimestamp(GpuTimestampAfterRestirShade);
    // History publication is the Pass-B UAV write itself; no copy/transition.
    WriteGpuTimestamp(GpuTimestampAfterRestirPublish);
}

void D3D12PathTracingBackend::RunRestirGiPass()
{
    if (m_pendingGpuResourceRefresh == PendingGpuResourceRefresh::FullScene ||
        !m_rtxdiAvailable ||
        m_qualitySettings.restirBackend != rb::RestirBackend::Rtxdi ||
        !UsesRestirGI(m_mode) ||
        m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        !m_rtxdiGiInitialPipeline ||
        !m_rtxdiGiFusedPipeline)
    {
        WriteGpuTimestamp(GpuTimestampAfterGiInitial);
        WriteGpuTimestamp(GpuTimestampAfterGiFused);
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(
        RootOutputTable,
        CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(
        RootAccelerationStructure,
        m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(
        RootSceneConstants,
        m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(
        RootSceneBuffers,
        GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(
        RootTextureTable,
        GpuDescriptor(DescriptorTextureBase));

    const UINT dispatchX = (m_renderWidth + 7u) / 8u;
    const UINT dispatchY = (m_renderHeight + 7u) / 8u;
    ID3D12Resource* currentGiReservoir = CurrentSurfaceGuideParity() == 0u
        ? m_restirGiReservoirA.Get()
        : m_restirGiReservoirB.Get();
    if (m_restirAliasHeapSize != 0u)
    {
        ID3D12Resource* currentDiScratch = CurrentSurfaceGuideParity() == 0u
            ? m_restirReservoirSpatial.Get()
            : m_restirReservoirSpatialB.Get();
        auto activateGiReservoir = CD3DX12_RESOURCE_BARRIER::Aliasing(
            currentDiScratch,
            currentGiReservoir);
        m_commandList->ResourceBarrier(1u, &activateGiReservoir);
    }

    m_commandList->SetPipelineState(m_rtxdiGiInitialPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1u);
    auto initialBarrier = CD3DX12_RESOURCE_BARRIER::UAV(currentGiReservoir);
    m_commandList->ResourceBarrier(1u, &initialBarrier);
    WriteGpuTimestamp(GpuTimestampAfterGiInitial);

    m_commandList->SetPipelineState(m_rtxdiGiFusedPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1u);
    D3D12_RESOURCE_BARRIER fusedBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(currentGiReservoir),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(AccumulationResource(CurrentSurfaceGuideParity())),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
    };
    m_commandList->ResourceBarrier(_countof(fusedBarriers), fusedBarriers);
    WriteGpuTimestamp(GpuTimestampAfterGiFused);
}

void D3D12PathTracingBackend::RunRestirPtPass()
{
    if (m_pendingGpuResourceRefresh == PendingGpuResourceRefresh::FullScene ||
        !m_rtxdiAvailable ||
        m_qualitySettings.restirBackend != rb::RestirBackend::Rtxdi ||
        !UsesRestirPT(m_mode) ||
        m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        !m_rtxdiPtInitialPipeline ||
        !m_rtxdiPtFusedPipeline)
    {
        WriteGpuTimestamp(GpuTimestampAfterPtInitial);
        WriteGpuTimestamp(GpuTimestampAfterPtFused);
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(
        RootOutputTable,
        CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(
        RootAccelerationStructure,
        m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(
        RootSceneConstants,
        m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(
        RootSceneBuffers,
        GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(
        RootTextureTable,
        GpuDescriptor(DescriptorTextureBase));

    const UINT parity = CurrentSurfaceGuideParity();
    const UINT dispatchX = (m_renderWidth + 7u) / 8u;
    const UINT dispatchY = (m_renderHeight + 7u) / 8u;
    ID3D12Resource* currentPtReservoir = parity == 0u
        ? m_restirPtReservoirA.Get()
        : m_restirPtReservoirB.Get();
    if (m_restirAliasHeapSize != 0u)
    {
        ID3D12Resource* currentDiScratch = parity == 0u
            ? m_restirReservoirSpatial.Get()
            : m_restirReservoirSpatialB.Get();
        auto activatePtReservoir = CD3DX12_RESOURCE_BARRIER::Aliasing(
            currentDiScratch,
            currentPtReservoir);
        m_commandList->ResourceBarrier(1u, &activatePtReservoir);
    }

    m_commandList->SetPipelineState(m_rtxdiPtInitialPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1u);
    auto initialBarrier = CD3DX12_RESOURCE_BARRIER::UAV(currentPtReservoir);
    m_commandList->ResourceBarrier(1u, &initialBarrier);
    WriteGpuTimestamp(GpuTimestampAfterPtInitial);

    m_commandList->SetPipelineState(m_rtxdiPtFusedPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1u);
    D3D12_RESOURCE_BARRIER fusedBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(currentPtReservoir),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalDirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_signalIndirect.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(AccumulationResource(parity)),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
    };
    m_commandList->ResourceBarrier(_countof(fusedBarriers), fusedBarriers);
    WriteGpuTimestamp(GpuTimestampAfterPtFused);
}

void D3D12PathTracingBackend::RunDenoisePass()
{
    const auto writeInactiveDenoiseTimestamps = [this]()
    {
        WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);
        WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
    };
    // Reference Still is defined as unclamped Baseline MIS accumulation. Its
    // denoiser resources are descriptor-valid 1x1 placeholders even if a user
    // subsequently selects a denoiser through UI/MCP, so the execution guard
    // is mandatory for both correctness and GPU memory safety.
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill)
    {
        writeInactiveDenoiseTimestamps();
        return;
    }

    if (m_pendingGpuResourceRefresh == PendingGpuResourceRefresh::FullScene)
    {
        // UI selection changes after constants are written. RayGen's HDR
        // passthrough is valid for this transition frame; backend dispatches
        // resume after OnUpdate rebuilds matching resources and constants.
        writeInactiveDenoiseTimestamps();
        return;
    }

    // UI is built after this frame's constants. If the user disables NRD in
    // that window, finish the already-declared fused frame instead of feeding
    // stale NRD outputs to FinalTaaCS; the toggle takes effect next submission.
    const bool denoiserRequested = m_fuseNrdFinalTaaForFrame ||
        m_denoiserEnabled || m_debugViewMode >= 16;
    if (denoiserRequested &&
        IsDlssSelected() &&
        m_dlssBackendRuntime.CanEvaluateRayReconstruction() &&
        m_dlssPreparePipeline)
    {
        (void)RunDlssRayReconstructionPass();
        return;
    }
    if (denoiserRequested && IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate() && m_nrdPreparePipeline && m_nrdCompositePipeline)
    {
        if (RunNrdDenoisePass())
        {
            return;
        }
        // Evaluate can demote NRD to unavailable. Its radiance resources were
        // allocated for NRD, not for the internal history layout, so never
        // reinterpret partially written NRD data in this frame. RayGen already
        // provided an unfiltered HDR fallback; rebuild the backend-specific
        // graph on the next update before enabling the internal fallback.
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        LogDiagnostic("NRD evaluation failed; queued internal fallback resource rebuild.");
        return;
    }

    if (!m_denoiseTemporalPipeline || !m_denoiseCompositePipeline || !ShouldRunInternalDenoiser())
    {
        writeInactiveDenoiseTimestamps();
        return;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));

    const UINT dispatchX = (m_renderWidth + 7) / 8;
    const UINT dispatchY = (m_renderHeight + 7) / 8;
    m_commandList->SetPipelineState(m_denoiseTemporalPipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    D3D12_RESOURCE_BARRIER temporalBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryRadiance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryMoments.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryLength.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryMomentsB.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryLengthB.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePing.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdDiffRadianceHitDistance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdSpecRadianceHitDistance.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdDiffDenoised.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdSpecDenoised.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdDiffuseConfidence.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_nrdSpecularConfidence.Get())
    };
    m_commandList->ResourceBarrier(_countof(temporalBarriers), temporalBarriers);
    WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);

    const UINT atrousPasses = (std::min)(static_cast<UINT>((std::max)(m_atrousPassCount, 0)), DenoiseAtrousPipelineCount);
    for (UINT pass = 0; pass < atrousPasses; ++pass)
    {
        if (!m_denoiseAtrousPipelines[pass])
        {
            break;
        }
        m_commandList->SetPipelineState(m_denoiseAtrousPipelines[pass].Get());
        m_commandList->Dispatch(dispatchX, dispatchY, 1);
        D3D12_RESOURCE_BARRIER atrousBarriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryRadiance.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_reconstructionHistoryRadianceB.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePing.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_denoisePong.Get())
        };
        m_commandList->ResourceBarrier(_countof(atrousBarriers), atrousBarriers);
    }
    WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);

    m_commandList->SetPipelineState(m_denoiseCompositePipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get())
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
    WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
}

bool D3D12PathTracingBackend::ShouldFuseNrdFinalTaa() const
{
    // Keep diagnostic runs on the explicit composite resource so validation
    // observes the same intermediate that it reports. The fused path is the
    // ordinary full-resolution NRD beauty graph only.
    const bool qualityDiagnosticsEnabled = m_benchmarkHarness &&
        lookdevpt::benchmark::IncludesQuality(m_benchmarkOptions.benchmarkKind);
    return m_pendingGpuResourceRefresh != PendingGpuResourceRefresh::FullScene &&
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        (m_qualitySettings.finalTaa || UsesTemporalUpscale()) &&
        m_denoiserEnabled && m_debugViewMode == 0 &&
        IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate() &&
        m_nrdPreparePipeline && m_finalTaaPipeline && !qualityDiagnosticsEnabled;
}

bool D3D12PathTracingBackend::RunNrdDenoisePass()
{
    if (!m_nrdPreparePipeline || !m_nrdCompositePipeline)
    {
        WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);
        WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
        return false;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));

    const UINT dispatchX = (m_renderWidth + 7) / 8;
    const UINT dispatchY = (m_renderHeight + 7) / 8;
    m_commandList->SetPipelineState(m_nrdPreparePipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);
    // NrdBackend transitions every prepared external input from UAV to SRV
    // before its first use. That transition already provides the required UAV
    // ordering, so a full redundant barrier set here only serializes the graph.
    WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);

    NrdEvaluationDesc nrdDesc{};
    nrdDesc.commandList = m_commandList.Get();
    nrdDesc.resourceWidth = m_renderWidth;
    nrdDesc.resourceHeight = m_renderHeight;
    nrdDesc.frameIndex = m_frameCounter;
    nrdDesc.frameContextIndex = m_frameIndex;
    nrdDesc.denoisingRange = m_rayTMax;
    nrdDesc.cameraJitter = m_nrdCurrentJitterForFrame;
    nrdDesc.previousCameraJitter = m_nrdPreviousJitterForFrame;
    nrdDesc.viewToClip = m_nrdViewToClip;
    nrdDesc.viewToClipPrev = m_nrdViewToClipPrev;
    nrdDesc.worldToView = m_nrdWorldToView;
    nrdDesc.worldToViewPrev = m_nrdWorldToViewPrev;
    nrdDesc.motion.resource = m_nrdMotion.Get();
    nrdDesc.normalRoughness.resource = m_nrdNormalRoughness.Get();
    nrdDesc.viewZ.resource = m_nrdViewZ.Get();
    nrdDesc.diffuseRadianceHitDistance.resource = m_nrdDiffRadianceHitDistance.Get();
    nrdDesc.specularRadianceHitDistance.resource = m_nrdSpecRadianceHitDistance.Get();
    nrdDesc.diffuseHistoryConfidence.resource = m_nrdDiffuseConfidence.Get();
    nrdDesc.specularHistoryConfidence.resource = m_nrdSpecularConfidence.Get();
    nrdDesc.diffuseOutput.resource = m_nrdDiffDenoised.Get();
    nrdDesc.specularOutput.resource = m_nrdSpecDenoised.Get();

    const auto nrdRecordingStart = std::chrono::steady_clock::now();
    const bool evaluated = m_nrdBackendRuntime.Evaluate(nrdDesc);
    if (m_benchmarkHarness && !m_benchmarkFinished)
    {
        m_benchmarkCpuNrdRecordingMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - nrdRecordingStart).count();
    }
    WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    if (!evaluated)
    {
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
        return false;
    }

    if (m_fuseNrdFinalTaaForFrame)
    {
        // FinalTaaCS consumes the NRD lobe outputs and reconstructs current HDR
        // directly. The NRD bridge has already returned every external output
        // to UAV state with the required ordering, so no intermediate UAV or
        // full-screen composite dispatch is needed here.
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
        return true;
    }

    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetPipelineState(m_nrdCompositePipeline.Get());
    m_commandList->Dispatch(dispatchX, dispatchY, 1);

    D3D12_RESOURCE_BARRIER compositeBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get())
    };
    m_commandList->ResourceBarrier(_countof(compositeBarriers), compositeBarriers);
    WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
    return true;
}

bool D3D12PathTracingBackend::RunDlssRayReconstructionPass()
{
    const auto writeInactiveTail = [this]()
    {
        WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
    };
    if (!m_dlssPreparePipeline ||
        !m_dlssBackendRuntime.CanEvaluateRayReconstruction())
    {
        WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);
        writeInactiveTail();
        return false;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(
        RootOutputTable,
        CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootConstantBufferView(
        RootSceneConstants,
        m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetPipelineState(m_dlssPreparePipeline.Get());
    m_commandList->Dispatch(
        (m_renderWidth + 7u) / 8u,
        (m_renderHeight + 7u) / 8u,
        1u);
    D3D12_RESOURCE_BARRIER guideBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssDepth.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssMotion.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssNormalRoughness.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssAlbedo.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssSpecularAlbedo.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_dlssExposure.Get()),
    };
    m_commandList->ResourceBarrier(_countof(guideBarriers), guideBarriers);
    WriteGpuTimestamp(GpuTimestampAfterDenoisePrepare);

    DlssEvaluationDesc desc{};
    desc.commandList = m_commandList.Get();
    desc.color = m_postDenoiseHdr.Get();
    desc.output =
        FinalResolvedHdrResource(CurrentSurfaceGuideParity());
    desc.linearDepth = m_dlssDepth.Get();
    desc.motion = m_dlssMotion.Get();
    desc.normalRoughness = m_dlssNormalRoughness.Get();
    desc.albedo = m_dlssAlbedo.Get();
    desc.specularAlbedo = m_dlssSpecularAlbedo.Get();
    desc.exposure = m_dlssExposure.Get();
    desc.renderWidth = m_renderWidth;
    desc.renderHeight = m_renderHeight;
    desc.outputWidth = m_width;
    desc.outputHeight = m_height;
    desc.frameIndex = m_frameCounter;
    desc.jitter = m_currentJitter;
    desc.cameraPosition = m_camera.GetPosition();
    desc.viewToClip = m_nrdViewToClip;
    desc.previousViewToClip = m_nrdViewToClipPrev;
    desc.worldToView = m_nrdWorldToView;
    desc.previousWorldToView = m_nrdWorldToViewPrev;
    desc.reset = m_frameState.cameraCut ||
        !rb::HasAny(m_validHistoryDomains, rb::HistoryDomain::Lighting);

    const bool evaluated =
        m_dlssBackendRuntime.EvaluateRayReconstruction(desc);
    WriteGpuTimestamp(GpuTimestampAfterDenoiseCore);
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    if (!evaluated)
    {
        // FinalTaaCS was told that DLSS would produce the frame. Switch that
        // mapped constant back before submission so it overwrites any partial
        // plugin output with the native raw-HDR TAAU result from this frame.
        auto* constants = reinterpret_cast<SceneConstantBuffer*>(
            m_frameContexts[m_frameIndex].mappedSceneConstants);
        if (constants)
        {
            constants->postProcessOptions.y = 0.0f;
        }
        if (m_dlssBackendRuntime.ConsumeFallbackRebuildRequest())
        {
            m_dlssFallbackRebuildAfterFrame = true;
            LogDiagnostic(
                "DLSS-RR evaluation failed; queued native internal fallback resource rebuild.");
        }
        WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
        return false;
    }

    auto outputBarrier = CD3DX12_RESOURCE_BARRIER::UAV(
        FinalResolvedHdrResource(CurrentSurfaceGuideParity()));
    m_commandList->ResourceBarrier(1u, &outputBarrier);
    WriteGpuTimestamp(GpuTimestampAfterDenoiseComposite);
    return true;
}

void D3D12PathTracingBackend::RunFinalTaaPass()
{
    if (m_pendingGpuResourceRefresh == PendingGpuResourceRefresh::FullScene ||
        !m_finalTaaPipeline ||
        (!m_qualitySettings.finalTaa &&
            !UsesTemporalUpscale() &&
            !(IsDlssSelected() &&
                m_dlssBackendRuntime.CanEvaluateRayReconstruction())) ||
        m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill)
    {
        return;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootShaderResourceView(RootAccelerationStructure, m_topLevelAs.result->GetGPUVirtualAddress());
    m_commandList->SetComputeRootConstantBufferView(RootSceneConstants, m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetPipelineState(m_finalTaaPipeline.Get());
    m_commandList->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

    ID3D12Resource* currentTaaHistory =
        FinalResolvedHdrResource(CurrentSurfaceGuideParity());
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(currentTaaHistory),
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
}

void D3D12PathTracingBackend::RunQualityCounterPass()
{
    if (!m_benchmarkHarness || !m_qualityCounterPipeline || !m_qualityCounterBuffer ||
        !m_qualityContribution || !m_frameContexts[m_frameIndex].qualityCounterReadback)
    {
        return;
    }

    // The validation pass consumes the immutable previous guides after final
    // TAA so finite checks cover the final HDR result. Fixed A/B descriptors
    // keep previous guides immutable without an end-of-frame copy.
    const UINT currentGuideParity = CurrentSurfaceGuideParity();
    D3D12_RESOURCE_BARRIER inputBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(m_qualityContribution.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(m_PathtracingOutput.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(0u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(1u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(SurfaceGuideAovResource(2u, currentGuideParity)),
        CD3DX12_RESOURCE_BARRIER::UAV(m_postDenoiseHdr.Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(FinalResolvedHdrResource(currentGuideParity)),
    };
    m_commandList->ResourceBarrier(_countof(inputBarriers), inputBarriers);

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    m_commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(RootOutputTable, CurrentOutputTableGpuDescriptor());
    m_commandList->SetComputeRootDescriptorTable(RootSceneBuffers, GpuDescriptor(DescriptorVertexBuffer));
    m_commandList->SetComputeRootDescriptorTable(RootTextureTable, GpuDescriptor(DescriptorTextureBase));
    m_commandList->SetComputeRootConstantBufferView(
        RootSceneConstants,
        m_frameContexts[m_frameIndex].sceneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->SetPipelineState(m_qualityCounterPipeline.Get());
    m_commandList->Dispatch(m_qualityCounterTileCountX, m_qualityCounterTileCountY, 1u);

    auto counterBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_qualityCounterBuffer.Get());
    m_commandList->ResourceBarrier(1, &counterBarrier);
    auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        m_qualityCounterBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->ResourceBarrier(1, &toCopy);
    m_commandList->CopyBufferRegion(
        m_frameContexts[m_frameIndex].qualityCounterReadback.Get(),
        0,
        m_qualityCounterBuffer.Get(),
        0,
        m_qualityCounterBufferSize);
    auto backToUav = CD3DX12_RESOURCE_BARRIER::Transition(
        m_qualityCounterBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->ResourceBarrier(1, &backToUav);
    m_frameContexts[m_frameIndex].qualityCountersPending = true;
}

void D3D12PathTracingBackend::SealSurfaceGuideFrame()
{
    const UINT previousGuideParity = CurrentSurfaceGuideParity() ^ 1u;
    ID3D12Resource* previousAov0 = SurfaceGuideAovResource(0u, previousGuideParity);
    ID3D12Resource* previousAov1 = SurfaceGuideAovResource(1u, previousGuideParity);
    ID3D12Resource* previousAov2 = SurfaceGuideAovResource(2u, previousGuideParity);
    ID3D12Resource* previousIdentity = SurfaceGuideIdentityResource(previousGuideParity);
    if (!previousAov0 || !previousAov1 || !previousAov2 || !previousIdentity)
    {
        return;
    }

    // These resources were read as immutable previous guides this frame and
    // become the writable current set on the next frame. One UAV ordering
    // point replaces four copies and sixteen state transitions.
    D3D12_RESOURCE_BARRIER reuseBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(previousAov0),
        CD3DX12_RESOURCE_BARRIER::UAV(previousAov1),
        CD3DX12_RESOURCE_BARRIER::UAV(previousAov2),
        CD3DX12_RESOURCE_BARRIER::UAV(previousIdentity),
    };
    m_commandList->ResourceBarrier(_countof(reuseBarriers), reuseBarriers);
}

void D3D12PathTracingBackend::CopyOutputToBackBuffer()
{
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
    m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_PathtracingOutput.Get());
    auto outputBackToUav = CD3DX12_RESOURCE_BARRIER::Transition(m_PathtracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_commandList->ResourceBarrier(1, &outputBackToUav);
}

void D3D12PathTracingBackend::BeginGpuTimingFrame()
{
    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    if (!m_gpuTimingSupported || !m_gpuTimestampQueryHeap || !frameContext.gpuTimestampReadback || !m_commandList)
    {
        return;
    }

    frameContext.gpuTimingPending = false;
    WriteGpuTimestamp(GpuTimestampFrameBegin);
}

void D3D12PathTracingBackend::WriteGpuTimestamp(GpuTimestamp timestamp)
{
    if (!m_gpuTimingSupported || !m_gpuTimestampQueryHeap || !m_commandList)
    {
        return;
    }

    const UINT queryIndex = m_frameIndex * GpuTimestampCount + static_cast<UINT>(timestamp);
    m_commandList->EndQuery(m_gpuTimestampQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

void D3D12PathTracingBackend::ResolveGpuTimingQueries()
{
    FrameContext& frameContext = m_frameContexts[m_frameIndex];
    if (!m_gpuTimingSupported || !m_gpuTimestampQueryHeap || !frameContext.gpuTimestampReadback || !m_commandList)
    {
        return;
    }

    m_commandList->ResolveQueryData(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        m_frameIndex * GpuTimestampCount,
        GpuTimestampCount,
        frameContext.gpuTimestampReadback.Get(),
        0);
    frameContext.gpuTimingPending = true;
}

void D3D12PathTracingBackend::ReadbackGpuTimingQueries(UINT frameIndex)
{
    if (frameIndex >= FrameCount)
    {
        return;
    }

    FrameContext& frameContext = m_frameContexts[frameIndex];
    if (!frameContext.gpuTimingPending && !frameContext.qualityCountersPending &&
        !frameContext.benchmarkMetricsPending)
    {
        return;
    }

    GpuTimingSample timing;
    if (frameContext.gpuTimingPending && frameContext.gpuTimestampReadback && m_gpuTimestampFrequency != 0)
    {
        D3D12_RANGE readRange = { 0, sizeof(UINT64) * GpuTimestampCount };
        void* mappedData = nullptr;
        if (SUCCEEDED(frameContext.gpuTimestampReadback->Map(0, &readRange, &mappedData)) && mappedData)
        {
            const UINT64* timestamps = static_cast<const UINT64*>(mappedData);
            const UINT64 frameBegin = timestamps[GpuTimestampFrameBegin];
            const UINT64 afterPathTrace = timestamps[GpuTimestampAfterPathTrace];
            const UINT64 afterRestirCandidate = timestamps[GpuTimestampAfterRestirCandidate];
            const UINT64 afterRestirTemporal = timestamps[GpuTimestampAfterRestirTemporal];
            const UINT64 afterRestirSpatial = timestamps[GpuTimestampAfterRestirSpatial];
            const UINT64 afterRestirShade = timestamps[GpuTimestampAfterRestirShade];
            const UINT64 afterRestirPublish = timestamps[GpuTimestampAfterRestirPublish];
            const UINT64 afterGiInitial = timestamps[GpuTimestampAfterGiInitial];
            const UINT64 afterGiFused = timestamps[GpuTimestampAfterGiFused];
            const UINT64 afterPtInitial = timestamps[GpuTimestampAfterPtInitial];
            const UINT64 afterPtFused = timestamps[GpuTimestampAfterPtFused];
            const UINT64 afterDenoisePrepare = timestamps[GpuTimestampAfterDenoisePrepare];
            const UINT64 afterDenoiseCore = timestamps[GpuTimestampAfterDenoiseCore];
            const UINT64 afterDenoiseComposite = timestamps[GpuTimestampAfterDenoiseComposite];
            const UINT64 afterFinalTaa = timestamps[GpuTimestampAfterFinalTaa];
            const UINT64 afterQualityCounters = timestamps[GpuTimestampAfterQualityCounters];
            const UINT64 afterHistoryPublish = timestamps[GpuTimestampAfterHistoryPublish];
            const UINT64 afterCopy = timestamps[GpuTimestampAfterCopy];
            const UINT64 frameEnd = timestamps[GpuTimestampFrameEnd];
            D3D12_RANGE writeRange = { 0, 0 };
            frameContext.gpuTimestampReadback->Unmap(0, &writeRange);

            if (afterPathTrace >= frameBegin &&
                afterRestirCandidate >= afterPathTrace &&
                afterRestirTemporal >= afterRestirCandidate &&
                afterRestirSpatial >= afterRestirTemporal &&
                afterRestirShade >= afterRestirSpatial &&
                afterRestirPublish >= afterRestirShade &&
                afterGiInitial >= afterRestirPublish &&
                afterGiFused >= afterGiInitial &&
                afterPtInitial >= afterGiFused &&
                afterPtFused >= afterPtInitial &&
                afterDenoisePrepare >= afterPtFused &&
                afterDenoiseCore >= afterDenoisePrepare &&
                afterDenoiseComposite >= afterDenoiseCore &&
                afterFinalTaa >= afterDenoiseComposite &&
                afterQualityCounters >= afterFinalTaa &&
                afterHistoryPublish >= afterQualityCounters &&
                afterCopy >= afterHistoryPublish &&
                frameEnd >= afterCopy)
            {
                const double tickToMs = 1000.0 / static_cast<double>(m_gpuTimestampFrequency);
                timing.pathTraceMs = static_cast<double>(afterPathTrace - frameBegin) * tickToMs;
                timing.restirCandidateMs = static_cast<double>(afterRestirCandidate - afterPathTrace) * tickToMs;
                timing.restirTemporalMs = static_cast<double>(afterRestirTemporal - afterRestirCandidate) * tickToMs;
                timing.restirSpatialMs = static_cast<double>(afterRestirSpatial - afterRestirTemporal) * tickToMs;
                timing.restirShadeMs = static_cast<double>(afterRestirShade - afterRestirSpatial) * tickToMs;
                timing.restirPublishMs = static_cast<double>(afterRestirPublish - afterRestirShade) * tickToMs;
                timing.restirGiInitialMs = static_cast<double>(afterGiInitial - afterRestirPublish) * tickToMs;
                timing.restirGiFusedMs = static_cast<double>(afterGiFused - afterGiInitial) * tickToMs;
                timing.restirPtInitialMs = static_cast<double>(afterPtInitial - afterGiFused) * tickToMs;
                timing.restirPtFusedMs = static_cast<double>(afterPtFused - afterPtInitial) * tickToMs;
                timing.restirMs = static_cast<double>(afterPtFused - afterPathTrace) * tickToMs;
                timing.denoisePrepareMs = static_cast<double>(afterDenoisePrepare - afterPtFused) * tickToMs;
                timing.denoiseCoreMs = static_cast<double>(afterDenoiseCore - afterDenoisePrepare) * tickToMs;
                timing.denoiseCompositeMs = static_cast<double>(afterDenoiseComposite - afterDenoiseCore) * tickToMs;
                timing.finalTaaMs = static_cast<double>(afterFinalTaa - afterDenoiseComposite) * tickToMs;
                timing.qualityCountersMs = static_cast<double>(afterQualityCounters - afterFinalTaa) * tickToMs;
                timing.historyPublishMs = static_cast<double>(afterHistoryPublish - afterQualityCounters) * tickToMs;
                // Preserve the legacy aggregate: denoiser, final TAA,
                // diagnostics and guide publication are all included.
                timing.denoiseMs = static_cast<double>(afterHistoryPublish - afterPtFused) * tickToMs;
                timing.copyMs = static_cast<double>(afterCopy - afterHistoryPublish) * tickToMs;
                timing.uiMs = static_cast<double>(frameEnd - afterCopy) * tickToMs;
                timing.pipelineMs = static_cast<double>(frameEnd - frameBegin) * tickToMs;
                timing.valid = true;
            }
        }
    }
    frameContext.gpuTimingPending = false;

    if (frameContext.qualityCountersPending)
    {
        if (!frameContext.benchmarkMetricsPending || !frameContext.qualityCounterReadback)
        {
            throw std::runtime_error("GPU quality-counter readback lost its benchmark submission association.");
        }
        D3D12_RANGE qualityReadRange = { 0, static_cast<SIZE_T>(m_qualityCounterBufferSize) };
        void* mappedQualityData = nullptr;
        ThrowIfFailed(frameContext.qualityCounterReadback->Map(0, &qualityReadRange, &mappedQualityData));
        if (!mappedQualityData)
        {
            throw std::runtime_error("GPU quality-counter readback returned no data.");
        }
        std::string qualityDiagnostics;
        const bool qualityValid = lookdevpt::benchmark::AggregateQualityCounterTiles(
            static_cast<const lookdevpt::benchmark::QualityCounterTileV1*>(mappedQualityData),
            m_qualityCounterTileCount,
            frameContext.benchmarkMetrics,
            qualityDiagnostics);
        D3D12_RANGE qualityWriteRange = { 0, 0 };
        frameContext.qualityCounterReadback->Unmap(0, &qualityWriteRange);
        frameContext.qualityCountersPending = false;
        if (!qualityValid)
        {
            throw std::runtime_error("GPU quality-counter aggregation failed: " + qualityDiagnostics);
        }
    }

    // WaitForPreviousFrame may visit contexts by swap-chain index rather than
    // submission order. Never let an older completed context replace newer UI
    // timing statistics.
    if (frameContext.submissionSerial >= m_gpuTimingFrameSerial)
    {
        m_gpuTimingFrameSerial = frameContext.submissionSerial;
        m_gpuTimingValid = timing.valid;
        if (timing.valid)
        {
            m_gpuFrameMs = timing.pipelineMs;
            m_gpuPathTraceMs = timing.pathTraceMs;
            m_gpuRestirMs = timing.restirMs;
            m_gpuRestirCandidateMs = timing.restirCandidateMs;
            m_gpuRestirTemporalMs = timing.restirTemporalMs;
            m_gpuRestirSpatialMs = timing.restirSpatialMs;
            m_gpuRestirShadeMs = timing.restirShadeMs;
            m_gpuRestirPublishMs = timing.restirPublishMs;
            m_gpuRestirGiInitialMs = timing.restirGiInitialMs;
            m_gpuRestirGiFusedMs = timing.restirGiFusedMs;
            m_gpuRestirPtInitialMs = timing.restirPtInitialMs;
            m_gpuRestirPtFusedMs = timing.restirPtFusedMs;
            m_gpuDenoiseMs = timing.denoiseMs;
            m_gpuDenoisePrepareMs = timing.denoisePrepareMs;
            m_gpuDenoiseCoreMs = timing.denoiseCoreMs;
            m_gpuDenoiseCompositeMs = timing.denoiseCompositeMs;
            m_gpuFinalTaaMs = timing.finalTaaMs;
            m_gpuQualityCountersMs = timing.qualityCountersMs;
            m_gpuHistoryPublishMs = timing.historyPublishMs;
            m_gpuCopyMs = timing.copyMs;
            m_gpuUiMs = timing.uiMs;
        }
    }

    CompleteBenchmarkFrame(frameContext, timing);
}

void D3D12PathTracingBackend::OnKeyDown(UINT8 key)
{
    if (key == VK_SPACE)
    {
        ResetCameraView();
        ResetRenderingHistory();
    }
}

void D3D12PathTracingBackend::OnWindowMessage(UINT, WPARAM, LPARAM)
{
}

void D3D12PathTracingBackend::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
    DXSample::ParseCommandLineArgs(argv, argc);

    const lookdevpt::benchmark::CommandLineParseResult benchmarkCommandLine =
        lookdevpt::benchmark::ParseCommandLine(argc, argv, false);
    m_benchmarkCommandLineValid = benchmarkCommandLine.ok;
    m_benchmarkDiagnostics = benchmarkCommandLine.diagnostics;
    if (benchmarkCommandLine.ok)
    {
        m_benchmarkOptions = benchmarkCommandLine.options;
    }

    auto matchesPathArgument = [](const std::wstring& argument, const wchar_t* prefix)
    {
        const size_t prefixLength = wcslen(prefix);
        return _wcsnicmp(argument.c_str(), prefix, prefixLength) == 0 &&
            (argument.size() == prefixLength || argument[prefixLength] == L'=');
    };

    auto consumePathArgument = [&](int& index, const wchar_t* prefix) -> std::wstring
    {
        const std::wstring argument = argv[index];
        const size_t prefixLength = wcslen(prefix);
        if (argument.size() > prefixLength && argument[prefixLength] == L'=')
        {
            return argument.substr(prefixLength + 1);
        }
        if (index + 1 < argc)
        {
            ++index;
            return argv[index];
        }
        return {};
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        if (matchesPathArgument(argument, L"--startup-config") || matchesPathArgument(argument, L"-startup-config") || matchesPathArgument(argument, L"/startup-config"))
        {
            const wchar_t* prefix = argument.starts_with(L"--startup-config") ? L"--startup-config" : argument.starts_with(L"/startup-config") ? L"/startup-config" : L"-startup-config";
            m_startupSettingsPath = consumePathArgument(i, prefix);
            m_hasStartupSettingsPath = true;
        }
        else if (matchesPathArgument(argument, L"--project") || matchesPathArgument(argument, L"-project") || matchesPathArgument(argument, L"/project"))
        {
            const wchar_t* prefix = argument.starts_with(L"--project") ? L"--project" : argument.starts_with(L"/project") ? L"/project" : L"-project";
            m_startupProjectPath = consumePathArgument(i, prefix);
            m_hasCommandLineProjectPath = true;
        }
        else if (matchesPathArgument(argument, L"--scene") || matchesPathArgument(argument, L"-scene") || matchesPathArgument(argument, L"/scene"))
        {
            const wchar_t* prefix = argument.starts_with(L"--scene") ? L"--scene" : argument.starts_with(L"/scene") ? L"/scene" : L"-scene";
            m_startupScenePath = consumePathArgument(i, prefix);
            m_hasCommandLineScenePath = true;
        }
        else if (matchesPathArgument(argument, L"--environment") || matchesPathArgument(argument, L"-environment") || matchesPathArgument(argument, L"/environment"))
        {
            const wchar_t* prefix = argument.starts_with(L"--environment") ? L"--environment" : argument.starts_with(L"/environment") ? L"/environment" : L"-environment";
            m_startupEnvironmentPath = consumePathArgument(i, prefix);
            m_hasCommandLineEnvironmentPath = true;
        }
        else if (_wcsicmp(argument.c_str(), L"--mcp-server") == 0 || _wcsicmp(argument.c_str(), L"-mcp-server") == 0 || _wcsicmp(argument.c_str(), L"/mcp-server") == 0)
        {
            m_startupMcpServer = true;
        }
        else if (_wcsicmp(argument.c_str(), L"--mcp-pair") == 0 || _wcsicmp(argument.c_str(), L"-mcp-pair") == 0 || _wcsicmp(argument.c_str(), L"/mcp-pair") == 0)
        {
            m_startupMcpServer = true;
            m_startupMcpPairing = true;
        }
        else if (matchesPathArgument(argument, L"--mcp-port") || matchesPathArgument(argument, L"-mcp-port") || matchesPathArgument(argument, L"/mcp-port"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-port") ? L"--mcp-port" : argument.starts_with(L"/mcp-port") ? L"/mcp-port" : L"-mcp-port";
            const std::wstring portText = consumePathArgument(i, prefix);
            if (!portText.empty())
            {
                m_startupMcpPort = static_cast<UINT>(std::clamp(_wtoi(portText.c_str()), 1, 65535));
            }
        }
        else if (matchesPathArgument(argument, L"--mcp-token") || matchesPathArgument(argument, L"-mcp-token") || matchesPathArgument(argument, L"/mcp-token"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-token") ? L"--mcp-token" : argument.starts_with(L"/mcp-token") ? L"/mcp-token" : L"-mcp-token";
            m_startupMcpToken = WideToUtf8(consumePathArgument(i, prefix));
        }
        else if (matchesPathArgument(argument, L"--mcp-auth") || matchesPathArgument(argument, L"-mcp-auth") || matchesPathArgument(argument, L"/mcp-auth"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-auth") ? L"--mcp-auth" : argument.starts_with(L"/mcp-auth") ? L"/mcp-auth" : L"-mcp-auth";
            m_startupMcpAuthenticationMode =
                mcp::AuthenticationModeFromName(
                    WideToUtf8(consumePathArgument(i, prefix)),
                    mcp::AuthenticationMode::BearerToken);
            m_hasStartupMcpAuthenticationMode = true;
        }
        else if (matchesPathArgument(argument, L"--mcp-access") || matchesPathArgument(argument, L"-mcp-access") || matchesPathArgument(argument, L"/mcp-access"))
        {
            const wchar_t* prefix = argument.starts_with(L"--mcp-access") ? L"--mcp-access" : argument.starts_with(L"/mcp-access") ? L"/mcp-access" : L"-mcp-access";
            m_startupMcpAccessMode = mcp::AccessModeFromName(WideToUtf8(consumePathArgument(i, prefix)), mcp::AccessMode::ConfirmMutations);
            m_hasStartupMcpAccessMode = true;
        }
    }
}

void D3D12PathTracingBackend::OnDestroy()
{
    m_sceneLoadCancelRequested.store(true, std::memory_order_relaxed);
    if (m_sceneLoadFuture.valid()) m_sceneLoadFuture.wait();
    StopMcpServer();
    m_mcpDispatcher.CancelAll("Application is shutting down.");
    WaitForPreviousFrame();
    for (FrameContext& frameContext : m_frameContexts)
    {
        if (frameContext.sceneConstantBuffer && frameContext.mappedSceneConstants)
        {
            frameContext.sceneConstantBuffer->Unmap(0, nullptr);
        }
        frameContext.mappedSceneConstants = nullptr;
    }
    m_nrdBackendRuntime.Shutdown();
    m_dlssBackendRuntime.Shutdown();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    if (m_frameLatencyWaitableObject)
    {
        CloseHandle(m_frameLatencyWaitableObject);
        m_frameLatencyWaitableObject = nullptr;
    }
}

void D3D12PathTracingBackend::WaitForFrameContext(UINT frameIndex)
{
    if (frameIndex >= FrameCount || !m_fence || !m_fenceEvent)
    {
        return;
    }

    FrameContext& frameContext = m_frameContexts[frameIndex];
    if (frameContext.fenceValue != 0 && m_fence->GetCompletedValue() < frameContext.fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(frameContext.fenceValue, m_fenceEvent));
        const auto waitStart = std::chrono::steady_clock::now();
        WaitForSingleObject(m_fenceEvent, INFINITE);
        if (m_benchmarkHarness && !m_benchmarkFinished)
        {
            m_benchmarkCpuFenceWaitMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - waitStart).count();
        }
    }

    if (frameContext.fenceValue != 0)
    {
        ReadbackGpuTimingQueries(frameIndex);
        frameContext.fenceValue = 0;
    }
}

UINT64 D3D12PathTracingBackend::SignalFrameAndAdvance()
{
    FrameContext& submittedFrame = m_frameContexts[m_frameIndex];
    const UINT64 submissionSerial = m_nextSubmissionSerial++;
    const UINT64 fenceValue = m_fenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    submittedFrame.submissionSerial = submissionSerial;
    submittedFrame.fenceValue = fenceValue;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return submissionSerial;
}

void D3D12PathTracingBackend::WaitForPreviousFrame()
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
    {
        return;
    }

    const UINT64 fenceValue = m_fenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    for (UINT frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        ReadbackGpuTimingQueries(frameIndex);
        m_frameContexts[frameIndex].fenceValue = 0;
    }
    if (m_swapChain)
    {
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }
}

void D3D12PathTracingBackend::ResetLight()
{
    m_lightDirection[0] = -0.35f;
    m_lightDirection[1] = -0.8f;
    m_lightDirection[2] = 0.45f;
    m_lightColor[0] = 1.0f;
    m_lightColor[1] = 0.96f;
    m_lightColor[2] = 0.88f;
    m_lightIntensity = 4.0f;
}

void D3D12PathTracingBackend::ResetCameraView()
{
    m_camera.Reset(m_defaultCameraPosition, m_defaultCameraYaw, m_defaultCameraPitch, m_defaultCameraRoll);
    m_cameraFovDegrees = m_defaultCameraFovDegrees;
}

void D3D12PathTracingBackend::ResetCameraSpeeds()
{
    m_baseMoveSpeed = 17.0f;
    m_fastMoveSpeed = 58.2f;
    m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
}

void D3D12PathTracingBackend::ResetAccumulation()
{
    m_accumulatedFrames = 0;
    m_resetAccumulationRequested = true;
    m_validHistoryDomains &= ~rb::HistoryDomain::ReferenceAccumulation;
}

void D3D12PathTracingBackend::ResetDenoiseHistory()
{
    InvalidateHistoryDomains(
        rb::HistoryDomain::Denoiser | rb::HistoryDomain::Taa,
        rb::FrameChangeMask::DenoiserSettings);
}

void D3D12PathTracingBackend::InvalidateHistory(rb::FrameChangeMask changes)
{
    InvalidateHistoryDomains(rb::HistoryDomainsForChange(changes), changes);
}

void D3D12PathTracingBackend::InvalidateHistoryDomains(rb::HistoryDomain domains, rb::FrameChangeMask changes)
{
    m_pendingFrameChanges |= changes;
    m_validHistoryDomains &= ~domains;

    if (rb::HasAny(changes, rb::FrameChangeMask::Geometry))
    {
        m_mcpSceneAuditFresh = false;
        ++m_frameState.revisions.scene;
        ++m_frameState.revisions.geometry;
    }
    if (rb::HasAny(changes, rb::FrameChangeMask::Material))
    {
        m_mcpSceneAuditFresh = false;
        ++m_frameState.revisions.material;
    }
    if (rb::HasAny(changes, rb::FrameChangeMask::Light)) ++m_frameState.revisions.light;
    if (rb::HasAny(changes, rb::FrameChangeMask::Hdri)) ++m_frameState.revisions.hdri;
    if (rb::HasAny(changes, rb::FrameChangeMask::Backend)) ++m_frameState.revisions.backend;
    if (rb::HasAny(changes, rb::FrameChangeMask::QualityProfile)) ++m_frameState.revisions.qualityProfile;

    if (rb::HasAny(domains, rb::HistoryDomain::ReferenceAccumulation))
    {
        ResetAccumulation();
    }
    if (rb::HasAny(domains, rb::HistoryDomain::Surface))
    {
        // Surface reprojection is the owner of the previous camera and jitter.
        // Lighting/denoiser/TAA-only edits deliberately preserve these guides.
        m_hasPreviousViewProjection = false;
        m_previousJitter = XMFLOAT2(0.0f, 0.0f);
        m_nrdCurrentJitterForFrame = XMFLOAT2(0.0f, 0.0f);
        m_nrdPreviousJitterForFrame = XMFLOAT2(0.0f, 0.0f);
        m_cameraMotionTrackingInitialized = false;
    }
    if (rb::HasAny(domains, rb::HistoryDomain::Denoiser))
    {
        m_denoiseHistoryValid = false;
        m_resetDenoiseHistoryRequested = true;
        m_nrdBackendRuntime.ResetHistory();
    }
}

void D3D12PathTracingBackend::ResetRenderingHistory()
{
    InvalidateHistoryDomains(rb::HistoryDomain::All, rb::FrameChangeMask::ManualReset);
}

void D3D12PathTracingBackend::ApplyNoisePreset(NoisePreset preset)
{
    m_noisePreset = preset;
    m_denoiserEnabled = true;
    m_splitSignalDenoise = true;
    m_realtimeReconstruction = true;
    m_cameraJitter = preset != NoisePreset::StillCapture;
    m_temporalStabilityEnabled = true;
    m_jitterMode = JitterMode::Stable32;

    switch (preset)
    {
    case NoisePreset::SharpPreview:
        m_movingJitterScale = 0.35f;
        m_reconstructionMaxHistoryFrames = 18;
        m_temporalAlphaMin = 0.08f;
        m_temporalAlphaMax = 0.32f;
        m_historyClampSigma = 1.10f;
        m_reactiveThreshold = 0.28f;
        m_specularHistoryScale = 0.35f;
        m_atrousPassCount = 2;
        m_atrousDiffuseStrength = 0.70f;
        m_atrousSpecularStrength = 0.22f;
        m_atrousVarianceScale = 1.00f;
        m_denoiserStrength = 0.65f;
        break;
    case NoisePreset::StillCapture:
        m_cameraJitter = true;
        m_movingJitterScale = 0.50f;
        m_reconstructionMaxHistoryFrames = 96;
        m_temporalAlphaMin = 0.02f;
        m_temporalAlphaMax = 0.12f;
        m_historyClampSigma = 2.00f;
        m_reactiveThreshold = 0.45f;
        m_specularHistoryScale = 0.60f;
        m_atrousPassCount = 5;
        m_atrousDiffuseStrength = 0.95f;
        m_atrousSpecularStrength = 0.45f;
        m_atrousVarianceScale = 1.60f;
        m_denoiserStrength = 0.95f;
        break;
    default:
        m_movingJitterScale = 0.25f;
        m_reconstructionMaxHistoryFrames = 32;
        m_temporalAlphaMin = 0.04f;
        m_temporalAlphaMax = 0.22f;
        m_historyClampSigma = 1.50f;
        m_reactiveThreshold = 0.35f;
        m_specularHistoryScale = 0.45f;
        m_atrousPassCount = 3;
        m_atrousDiffuseStrength = 0.85f;
        m_atrousSpecularStrength = 0.35f;
        m_atrousVarianceScale = 1.25f;
        m_denoiserStrength = 0.85f;
        break;
    }
}

void D3D12PathTracingBackend::ApplyQualitySettingsToRenderer()
{
    m_overBudgetFrameCount = 0;
    m_underBudgetFrameCount = 0;
    m_rayBudgetBouncePenalty = 0;
    m_rayBudgetExtraSampleEnabled = true;
    m_autoSecondaryHalfActive = false;
    m_activeSecondaryShadingRate =
        m_qualitySettings.qualityProfile == rb::QualityProfile::InteractiveGame &&
        m_qualitySettings.secondaryShadingRate == rb::SecondaryShadingRate::AdaptiveHalf
        ? 0.5f
        : 1.0f;
    m_dynamicResolutionOverBudgetFrames = 0;
    m_dynamicResolutionUnderBudgetFrames = 0;
    ApplyConfiguredRenderScale(true);

    switch (m_qualitySettings.qualityProfile)
    {
    case rb::QualityProfile::SharpPreview:
        ApplyNoisePreset(NoisePreset::SharpPreview);
        m_denoiseBackend = DenoiseBackend::NrdRelax;
        m_denoiserEnabled = true;
        m_adaptiveSamplingEnabled = true;
        m_giRadianceClamp = 8.0f;
        break;
    case rb::QualityProfile::ReferenceStill:
        ApplyNoisePreset(NoisePreset::StillCapture);
        m_mode = PathTracingMode::Pathtracing;
        m_denoiseBackend = DenoiseBackend::Off;
        m_denoiserEnabled = false;
        m_realtimeReconstruction = false;
        m_temporalStabilityEnabled = false;
        m_adaptiveSamplingEnabled = false;
        m_giRadianceClamp = 0.0f;
        m_maxAccumulatedFrames = static_cast<int>((std::min)(m_qualitySettings.referenceSpp, 1048576u));
        m_maxPathBounces = 8;
        m_minPathBounces = 3;
        break;
    case rb::QualityProfile::InteractiveGame:
    default:
        ApplyNoisePreset(NoisePreset::InteractiveStable);
        m_denoiseBackend = DenoiseBackend::NrdReblur;
        m_denoiserEnabled = true;
        m_adaptiveSamplingEnabled = true;
        m_giRadianceClamp = 8.0f;
        break;
    }

    if (m_device)
    {
        WaitForPreviousFrame();
        m_nrdBackendRuntime.UpdateMethod(m_device.Get(), SelectedNrdMethod());
    }
}

void D3D12PathTracingBackend::UpdateRayBudget()
{
    constexpr int MaxAutomaticBouncePenalty = 2;
    const bool interactiveProfile = m_qualitySettings.qualityProfile == rb::QualityProfile::InteractiveGame;
    const bool forceAdaptiveHalf = interactiveProfile &&
        m_qualitySettings.secondaryShadingRate == rb::SecondaryShadingRate::AdaptiveHalf;
    const bool automaticSecondaryRate = interactiveProfile &&
        m_qualitySettings.secondaryShadingRate == rb::SecondaryShadingRate::Auto;
    if (!automaticSecondaryRate)
    {
        m_autoSecondaryHalfActive = false;
    }

    const auto updateActiveSecondaryRate = [&]()
    {
        // Sharp Preview and Reference Still always retain full-rate secondary
        // transport. Explicit AdaptiveHalf is restricted to Interactive, and
        // Auto only becomes active after every cheaper budget lever is spent.
        m_activeSecondaryShadingRate = interactiveProfile &&
            (forceAdaptiveHalf || (automaticSecondaryRate && m_autoSecondaryHalfActive))
            ? 0.5f
            : 1.0f;
    };
    const auto publishControllerState = [&]()
    {
        m_rayBudgetState.targetGpuMs = m_qualitySettings.rayBudget.targetGpuMs;
        m_rayBudgetState.lastGpuMs = m_gpuTimingValid ? m_gpuFrameMs : 0.0;
        m_rayBudgetState.overBudgetFrames = m_overBudgetFrameCount;
        m_rayBudgetState.underBudgetFrames = m_underBudgetFrameCount;
        m_rayBudgetState.extraSampleQuotaEnabled = m_rayBudgetExtraSampleEnabled;
    };

    updateActiveSecondaryRate();
    publishControllerState();

    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill)
    {
        m_giSamplesPerFrame = 1;
        m_maxAdaptiveSamplesPerPixel = 1;
        m_rayBudgetState.requestedSpp = 1;
        m_rayBudgetState.effectiveSpp = 1;
        m_rayBudgetState.requestedBounces = 8;
        m_rayBudgetState.effectiveBounces = static_cast<uint32_t>(m_maxPathBounces);
        m_rayBudgetState.moving = false;
        m_autoSecondaryHalfActive = false;
        updateActiveSecondaryRate();
        publishControllerState();
        return;
    }

    const rb::RayBudgetSettings& budget = m_qualitySettings.rayBudget;
    if (m_gpuTimingValid)
    {
        if (m_gpuFrameMs > static_cast<double>(budget.targetGpuMs))
        {
            ++m_overBudgetFrameCount;
            m_underBudgetFrameCount = 0;
            if (m_overBudgetFrameCount >= 3)
            {
                if (m_rayBudgetExtraSampleEnabled)
                {
                    m_rayBudgetExtraSampleEnabled = false;
                }
                else if (m_rayBudgetBouncePenalty < MaxAutomaticBouncePenalty)
                {
                    ++m_rayBudgetBouncePenalty;
                }
                else if (automaticSecondaryRate && !m_autoSecondaryHalfActive)
                {
                    m_autoSecondaryHalfActive = true;
                }
                else
                {
                    m_rayBudgetBouncePenalty = MaxAutomaticBouncePenalty;
                }
                m_overBudgetFrameCount = 0;
            }
        }
        else if (m_gpuFrameMs < static_cast<double>((std::max)(budget.targetGpuMs - 1.5f, 1.0f)))
        {
            ++m_underBudgetFrameCount;
            m_overBudgetFrameCount = 0;
            if (m_underBudgetFrameCount >= 60)
            {
                if (automaticSecondaryRate && m_autoSecondaryHalfActive)
                {
                    // Return to full-rate transport as one atomic quality step.
                    // Bounce/SPP recovery begins only after another 60 stable
                    // frames, avoiding an immediate oscillation around target.
                    m_autoSecondaryHalfActive = false;
                }
                else if (m_rayBudgetBouncePenalty > 0)
                {
                    --m_rayBudgetBouncePenalty;
                }
                else
                {
                    m_rayBudgetExtraSampleEnabled = true;
                }
                m_underBudgetFrameCount = 0;
            }
        }
        else
        {
            m_overBudgetFrameCount = 0;
            m_underBudgetFrameCount = 0;
        }
    }
    updateActiveSecondaryRate();
    publishControllerState();

    const bool moving = m_cameraMotionAmount > 0.001f;
    if (moving)
    {
        m_giSamplesPerFrame = static_cast<int>(budget.movingSpp);
        m_maxAdaptiveSamplesPerPixel = m_giSamplesPerFrame;
        m_maxPathBounces = static_cast<int>(budget.movingBounces);
        m_rayBudgetState.requestedSpp = budget.movingSpp;
        m_rayBudgetState.effectiveSpp = static_cast<uint32_t>(m_giSamplesPerFrame);
        m_rayBudgetState.requestedBounces = budget.movingBounces;
        m_rayBudgetState.effectiveBounces = static_cast<uint32_t>(m_maxPathBounces);
        m_rayBudgetState.moving = true;
        publishControllerState();
        return;
    }

    const uint32_t settleFrames = (std::max)(budget.settleFrames, 1u);
    const bool settled = m_framesSinceCameraMotion >= settleFrames;
    m_giSamplesPerFrame = static_cast<int>(budget.staticBaseSpp);
    m_maxAdaptiveSamplesPerPixel = settled && m_rayBudgetExtraSampleEnabled
        ? static_cast<int>(budget.staticMaxSpp)
        : m_giSamplesPerFrame;

    int bounceLimit = static_cast<int>(budget.staticBounces);
    if (!settled)
    {
        bounceLimit = m_framesSinceCameraMotion <= settleFrames / 2u
            ? (std::max)(static_cast<int>(budget.movingBounces) + 1, 3)
            : static_cast<int>(budget.staticBounces);
    }
    m_maxPathBounces = (std::max)(bounceLimit - m_rayBudgetBouncePenalty, static_cast<int>(budget.movingBounces));
    m_minPathBounces = (std::min)(m_minPathBounces, m_maxPathBounces);
    m_rayBudgetState.requestedSpp = settled ? budget.staticMaxSpp : budget.staticBaseSpp;
    m_rayBudgetState.effectiveSpp = static_cast<uint32_t>(m_maxAdaptiveSamplesPerPixel);
    m_rayBudgetState.requestedBounces = budget.staticBounces;
    m_rayBudgetState.effectiveBounces = static_cast<uint32_t>(m_maxPathBounces);
    m_rayBudgetState.moving = false;
    m_rayBudgetState.overBudgetFrames = m_overBudgetFrameCount;
    m_rayBudgetState.underBudgetFrames = m_underBudgetFrameCount;
    m_rayBudgetState.extraSampleQuotaEnabled = m_rayBudgetExtraSampleEnabled;
}

bool D3D12PathTracingBackend::IsDlssSelected() const
{
    return m_denoiseBackend == DenoiseBackend::DlssRayReconstruction;
}

bool D3D12PathTracingBackend::IsNrdSelected() const
{
    return m_denoiseBackend == DenoiseBackend::NrdReblur || m_denoiseBackend == DenoiseBackend::NrdRelax;
}

NrdMethod D3D12PathTracingBackend::SelectedNrdMethod() const
{
    return m_denoiseBackend == DenoiseBackend::NrdRelax
        ? NrdMethod::RelaxDiffuseSpecular
        : NrdMethod::ReblurDiffuseSpecular;
}

const char* D3D12PathTracingBackend::ActiveDenoiseBackendName() const
{
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        m_denoiseBackend == DenoiseBackend::Off)
    {
        return "off";
    }
    if (IsDlssSelected() && m_dlssBackendRuntime.CanEvaluateRayReconstruction())
    {
        return "dlss_rr";
    }
    if (IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate())
    {
        return DenoiseBackendName(m_denoiseBackend);
    }
    return "internal";
}

const char* D3D12PathTracingBackend::ActiveDenoiseBackendDisplayName() const
{
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        m_denoiseBackend == DenoiseBackend::Off)
    {
        return "Off";
    }
    if (IsDlssSelected() && m_dlssBackendRuntime.CanEvaluateRayReconstruction())
    {
        return "DLSS Ray Reconstruction";
    }
    if (IsNrdSelected() && m_nrdBackendRuntime.CanEvaluate())
    {
        return DenoiseBackendDisplayName(m_denoiseBackend);
    }
    return "Internal";
}

bool D3D12PathTracingBackend::ShouldRunInternalDenoiser() const
{
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        m_denoiseBackend == DenoiseBackend::Off)
    {
        return false;
    }
    if (m_denoiseBackend == DenoiseBackend::Internal)
    {
        return m_denoiserEnabled || m_debugViewMode >= 16;
    }
    if (IsNrdSelected())
    {
        return !m_nrdBackendRuntime.CanEvaluate() && (m_denoiserEnabled || m_debugViewMode >= 16);
    }
    return !m_dlssBackendRuntime.CanEvaluateRayReconstruction() && (m_denoiserEnabled || m_debugViewMode >= 16);
}

std::string D3D12PathTracingBackend::BuildDlssStatusJson() const
{
    const DlssStatus& status = m_dlssBackendRuntime.Status();
    const bool active =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        IsDlssSelected() && status.evaluationReady;
    std::ostringstream out;
    out << "{";
    out << "\"compiled\":" << (status.compiled ? "true" : "false") << ",";
    out << "\"selected\":" << (IsDlssSelected() ? "true" : "false") << ",";
    out << "\"active\":" << (active ? "true" : "false") << ",";
    out << "\"runtimeAvailable\":" << (status.runtimeAvailable ? "true" : "false") << ",";
    out << "\"initialized\":" << (status.initialized ? "true" : "false") << ",";
    out << "\"deviceRegistered\":" << (status.deviceRegistered ? "true" : "false") << ",";
    out << "\"applicationIdentityConfigured\":"
        << (status.applicationIdentityConfigured ? "true" : "false") << ",";
    out << "\"featureSupported\":" << (status.featureSupported ? "true" : "false") << ",";
    out << "\"evaluationReady\":" << (status.evaluationReady ? "true" : "false") << ",";
    out << "\"lastEvaluationSucceeded\":"
        << (status.lastEvaluationSucceeded ? "true" : "false") << ",";
    out << "\"fallbackRebuildRequested\":"
        << (status.fallbackRebuildRequested ? "true" : "false") << ",";
    out << "\"successfulEvaluations\":" << status.successfulEvaluations << ",";
    out << "\"failedEvaluations\":" << status.failedEvaluations << ",";
    out << "\"lastResultCode\":" << status.lastResultCode << ",";
    out << "\"historyResetRequested\":" << (status.historyResetRequested ? "true" : "false") << ",";
    out << "\"mode\":\"" << DlssModeName(m_dlssMode) << "\",";
    out << "\"modeLabel\":\"" << DlssModeDisplayName(m_dlssMode) << "\",";
    out << "\"renderResolution\":{\"width\":" << (status.recommendedRenderWidth > 0 ? status.recommendedRenderWidth : m_renderWidth)
        << ",\"height\":" << (status.recommendedRenderHeight > 0 ? status.recommendedRenderHeight : m_renderHeight) << "},";
    out << "\"outputResolution\":{\"width\":" << m_width << ",\"height\":" << m_height << "},";
    out << "\"runtimePath\":\"" << cld::EscapeJson(WideToUtf8(status.runtimePath)) << "\",";
    out << "\"lastError\":\"" << cld::EscapeJson(status.lastError) << "\",";
    out << "\"lastFailureStage\":\"" << cld::EscapeJson(status.lastFailureStage) << "\",";
    out << "\"fallbackReason\":\"" << cld::EscapeJson(status.fallbackReason) << "\"";
    out << "}";
    return out.str();
}

std::string D3D12PathTracingBackend::BuildNrdStatusJson() const
{
    const NrdStatus& status = m_nrdBackendRuntime.Status();
    const bool active =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        IsNrdSelected() && status.evaluationReady;
    std::ostringstream out;
    out << "{";
    out << "\"compiled\":" << (status.compiled ? "true" : "false") << ",";
    out << "\"selected\":" << (IsNrdSelected() ? "true" : "false") << ",";
    out << "\"active\":" << (active ? "true" : "false") << ",";
    out << "\"headersAvailable\":" << (status.headersAvailable ? "true" : "false") << ",";
    out << "\"libraryAvailable\":" << (status.libraryAvailable ? "true" : "false") << ",";
    out << "\"initialized\":" << (status.initialized ? "true" : "false") << ",";
    out << "\"instanceCreated\":" << (status.instanceCreated ? "true" : "false") << ",";
    out << "\"evaluationReady\":" << (status.evaluationReady ? "true" : "false") << ",";
    out << "\"historyResetRequested\":" << (status.historyResetRequested ? "true" : "false") << ",";
    out << "\"version\":\"" << status.versionMajor << "." << status.versionMinor << "." << status.versionBuild << "\",";
    out << "\"selectedDenoiser\":\"" << cld::EscapeJson(status.selectedDenoiser) << "\",";
    out << "\"normalEncoding\":\"" << cld::EscapeJson(status.normalEncoding) << "\",";
    out << "\"roughnessEncoding\":\"" << cld::EscapeJson(status.roughnessEncoding) << "\",";
    out << "\"resourceResolution\":{\"width\":" << status.resourceWidth << ",\"height\":" << status.resourceHeight << "},";
    out << "\"supportedDenoisers\":" << status.supportedDenoiserCount << ",";
    out << "\"pipelines\":" << status.pipelineCount << ",";
    out << "\"permanentPoolSize\":" << status.permanentPoolSize << ",";
    out << "\"transientPoolSize\":" << status.transientPoolSize << ",";
    out << "\"constantBufferMaxDataSize\":" << status.constantBufferMaxDataSize << ",";
    out << "\"lastError\":\"" << cld::EscapeJson(status.lastError) << "\",";
    out << "\"fallbackReason\":\"" << cld::EscapeJson(status.fallbackReason) << "\"";
    out << "}";
    return out.str();
}

void D3D12PathTracingBackend::UpdateCameraMotionState()
{
    const XMFLOAT3 cameraPosition = m_camera.GetPosition();
    const float yaw = m_camera.GetYawRadians();
    const float pitch = m_camera.GetPitchRadians();
    const float roll = m_camera.GetRollRadians();

    if (!m_cameraMotionTrackingInitialized)
    {
        m_previousCameraMotionState = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, yaw);
        m_previousCameraMotionPitch = pitch;
        m_previousCameraMotionRollFov = XMFLOAT2(roll, m_cameraFovDegrees);
        m_cameraMotionAmount = 0.0f;
        m_framesSinceCameraMotion = 4;
        m_cameraMotionTrackingInitialized = true;
        return;
    }

    const float dx = cameraPosition.x - m_previousCameraMotionState.x;
    const float dy = cameraPosition.y - m_previousCameraMotionState.y;
    const float dz = cameraPosition.z - m_previousCameraMotionState.z;
    const float positionDelta = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float yawDelta = std::remainder(yaw - m_previousCameraMotionState.w, XM_2PI);
    const float pitchDelta = pitch - m_previousCameraMotionPitch;
    const float rollDelta = std::remainder(roll - m_previousCameraMotionRollFov.x, XM_2PI);
    const float fovDelta = XMConvertToRadians(m_cameraFovDegrees - m_previousCameraMotionRollFov.y);
    const float angleDelta = std::abs(yawDelta) + std::abs(pitchDelta) + std::abs(rollDelta) + std::abs(fovDelta);
    m_cameraMotionAmount = std::clamp(positionDelta * 0.20f + angleDelta * 2.0f, 0.0f, 1.0f);
    if (m_cameraMotionAmount > 0.001f)
    {
        InvalidateHistory(rb::FrameChangeMask::CameraMotion);
    }

    const float sceneDx = m_scene.boundsMax.x - m_scene.boundsMin.x;
    const float sceneDy = m_scene.boundsMax.y - m_scene.boundsMin.y;
    const float sceneDz = m_scene.boundsMax.z - m_scene.boundsMin.z;
    const float sceneDiagonal = std::sqrt(sceneDx * sceneDx + sceneDy * sceneDy + sceneDz * sceneDz);
    const float cutDistance = (std::max)(sceneDiagonal * 0.25f, 1.0f);
    if (!m_forcePreserveCameraHistoryOnce &&
        (positionDelta > cutDistance || angleDelta > XMConvertToRadians(45.0f)))
    {
        InvalidateHistory(rb::FrameChangeMask::CameraCut);
    }
    m_forcePreserveCameraHistoryOnce = false;

    if (m_cameraMotionAmount > 0.001f)
    {
        m_framesSinceCameraMotion = 0;
    }
    else
    {
        // TAA continues admitting the progressively sharper NRD estimate until
        // its 30-frame REBLUR accumulation has matured. Do not clamp this age at
        // the shorter ray-budget settle interval.
        const uint32_t settleLimit = (std::max)(m_qualitySettings.rayBudget.settleFrames + 1u, 32u);
        m_framesSinceCameraMotion = (std::min)(m_framesSinceCameraMotion + 1u, settleLimit);
    }

    m_previousCameraMotionState = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, yaw);
    m_previousCameraMotionPitch = pitch;
    m_previousCameraMotionRollFov = XMFLOAT2(roll, m_cameraFovDegrees);
    m_cameraMotionTrackingInitialized = true;
}

bool D3D12PathTracingBackend::HasAccumulationStateChanged()
{
    XMFLOAT3 cameraPosition = m_camera.GetPosition();
    XMFLOAT4 cameraAndYaw(cameraPosition.x, cameraPosition.y, cameraPosition.z, m_camera.GetYawRadians());
    const float cameraPitch = m_camera.GetPitchRadians();
    const XMFLOAT2 cameraRollFov(m_camera.GetRollRadians(), m_cameraFovDegrees);
    XMFLOAT4 lighting(m_lightDirection[0], m_lightDirection[1], m_lightDirection[2], m_lightIntensity + static_cast<float>(m_debugViewMode) + (m_shadowEnabled ? 1.0f : 0.0f) + (m_skyNeeEnabled ? 2.0f : 0.0f));
    XMFLOAT4 giOptions(static_cast<float>(m_giSamplesPerFrame), m_giRadianceClamp, m_giTemporalClampScale, m_giTemporalClampMin);
    const bool useRestir = m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        m_rtxdiAvailable && m_qualitySettings.restirBackend == rb::RestirBackend::Rtxdi && UsesRestirDI(m_mode);
    XMFLOAT4 pathOptions(static_cast<float>(m_maxPathBounces), static_cast<float>(m_minPathBounces), static_cast<float>(m_restirCandidateSamples), useRestir ? 1.0f : 0.0f);
    XMFLOAT4 restirOptions(m_restirTemporalReuse ? 1.0f : 0.0f, static_cast<float>(m_restirSpatialReusePasses), static_cast<float>(m_restirSpatialRadius), m_restirMClamp);
    const bool combinedRestir = m_mode == PathTracingMode::ReSTIRCombined ||
        m_mode == PathTracingMode::ReSTIRPTCombined;
    XMFLOAT4 restirDiOptions(
        (combinedRestir ? m_restirDiTemporalReuse : m_restirTemporalReuse) ? 1.0f : 0.0f,
        static_cast<float>(combinedRestir ? m_restirDiSpatialReusePasses : m_restirSpatialReusePasses),
        static_cast<float>(combinedRestir ? m_restirDiCandidateSamples : m_restirCandidateSamples),
        combinedRestir ? m_restirDiMClamp : m_restirMClamp);
    XMFLOAT4 lightSystemOptions(
        (m_emissiveLightsEnabled ? m_emissiveLightIntensity : 0.0f) + (m_proceduralLightsEnabled ? m_proceduralLightIntensity : 0.0f),
        m_environmentMapEnabled ? m_environmentIntensity : 0.0f,
        m_environmentRotation,
        static_cast<float>(m_activeLightCount));
    XMFLOAT4 signalDenoiseOptions(m_splitSignalDenoise ? 1.0f : 0.0f, m_historyClampSigma, m_reactiveThreshold, m_specularHistoryScale);
    // Editor selection does not change the rendered image in Normal focus mode.
    // Exclude it from progressive accumulation tracking unless the focus view
    // actually alters shading.
    const float materialViewKey = m_materialFocusMode == MaterialFocusMode::Normal
        ? 0.0f
        : static_cast<float>(m_materialFocusMode) + static_cast<float>(m_selectedMaterial) * 4.0f;
    XMFLOAT4 viewOptions(m_exposure, m_gamma, static_cast<float>(m_toneMapper), materialViewKey);
    const bool changed =
        m_resetAccumulationRequested ||
        memcmp(&cameraAndYaw, &m_lastCameraAndYaw, sizeof(XMFLOAT4)) != 0 ||
        cameraPitch != m_lastCameraPitch ||
        memcmp(&cameraRollFov, &m_lastCameraRollFov, sizeof(XMFLOAT2)) != 0 ||
        memcmp(&lighting, &m_lastLighting, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&giOptions, &m_lastGiOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&pathOptions, &m_lastPathOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&restirOptions, &m_lastRestirOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&restirDiOptions, &m_lastRestirDiOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&lightSystemOptions, &m_lastLightSystemOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&signalDenoiseOptions, &m_lastSignalDenoiseOptions, sizeof(XMFLOAT4)) != 0 ||
        memcmp(&viewOptions, &m_lastViewOptions, sizeof(XMFLOAT4)) != 0;
    m_lastCameraAndYaw = cameraAndYaw;
    m_lastCameraPitch = cameraPitch;
    m_lastCameraRollFov = cameraRollFov;
    m_lastLighting = lighting;
    m_lastGiOptions = giOptions;
    m_lastPathOptions = pathOptions;
    m_lastRestirOptions = restirOptions;
    m_lastRestirDiOptions = restirDiOptions;
    m_lastLightSystemOptions = lightSystemOptions;
    m_lastSignalDenoiseOptions = signalDenoiseOptions;
    m_lastViewOptions = viewOptions;
    m_resetAccumulationRequested = false;
    return changed;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateDefaultBuffer(const void* data, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES finalState, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    if (data && size > 0)
    {
        ComPtr<ID3D12Resource> upload = CreateUploadBuffer(data, size, L"UploadBuffer");
        m_uploadBuffers.push_back(upload);
        m_commandList->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, size);
        auto uploadReadyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        m_commandList->ResourceBarrier(1, &uploadReadyBarrier);
    }
    return resource;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateUploadBuffer(const void* data, UINT64 size, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    if (data && size > 0)
    {
        void* mapped = nullptr;
        ThrowIfFailed(resource->Map(0, nullptr, &mapped));
        memcpy(mapped, data, size);
        resource->Unmap(0, nullptr);
    }
    return resource;
}

ComPtr<ID3D12Resource> D3D12PathTracingBackend::CreateUavBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState, const wchar_t* name)
{
    ComPtr<ID3D12Resource> resource;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource)));
    resource->SetName(name);
    return resource;
}

UINT D3D12PathTracingBackend::CurrentSurfaceGuideParity() const
{
    return m_frameCounter & 1u;
}

UINT D3D12PathTracingBackend::LastSubmittedSurfaceGuideParity() const
{
    // Artifact capture occurs after OnRender advances frameCounter. Keep the
    // zero-frame fallback useful for explicit capture initialization paths.
    return m_frameCounter == 0u ? CurrentSurfaceGuideParity() : ((m_frameCounter - 1u) & 1u);
}

ID3D12Resource* D3D12PathTracingBackend::AccumulationResource(UINT parity) const
{
    if (!m_accumulationAliasesTaaHistory)
    {
        return m_accumulationOutput.Get();
    }
    return (parity & 1u) != 0u ? m_taaHistoryA.Get() : m_taaHistoryB.Get();
}

ID3D12Resource* D3D12PathTracingBackend::FinalResolvedHdrResource(UINT parity) const
{
    const bool finalTaaResourceSet =
        m_qualitySettings.qualityProfile != rb::QualityProfile::ReferenceStill &&
        (m_qualitySettings.finalTaa ||
            UsesTemporalUpscale() ||
            (IsDlssSelected() &&
                m_dlssBackendRuntime.CanEvaluateRayReconstruction()));
    if (finalTaaResourceSet)
    {
        return (parity & 1u) != 0u ? m_taaHistoryA.Get() : m_taaHistoryB.Get();
    }
    return m_finalResolvedHdr.Get();
}

ID3D12Resource* D3D12PathTracingBackend::SurfaceGuideAovResource(UINT plane, UINT parity) const
{
    const bool setB = (parity & 1u) != 0u;
    switch (plane)
    {
    case 0u: return setB ? m_previousDenoiseAov0.Get() : m_denoiseAov0.Get();
    case 1u: return setB ? m_previousDenoiseAov1.Get() : m_denoiseAov1.Get();
    case 2u: return setB ? m_previousDenoiseAov2.Get() : m_denoiseAov2.Get();
    default: return nullptr;
    }
}

ID3D12Resource* D3D12PathTracingBackend::SurfaceGuideIdentityResource(UINT parity) const
{
    return (parity & 1u) != 0u ? m_previousSurfaceIdentity.Get() : m_surfaceIdentity.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12PathTracingBackend::CurrentOutputTableGpuDescriptor() const
{
    return GpuDescriptor(CurrentSurfaceGuideParity() == 0u
        ? static_cast<UINT>(DescriptorOutputUav)
        : m_alternateOutputTableBase);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12PathTracingBackend::CpuDescriptor(UINT index) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), index, m_descriptorSize);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12PathTracingBackend::GpuDescriptor(UINT index) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), index, m_descriptorSize);
}

std::wstring D3D12PathTracingBackend::ShaderFileName() const
{
    if (m_qualitySettings.qualityProfile == rb::QualityProfile::ReferenceStill ||
        !m_rtxdiAvailable || m_qualitySettings.restirBackend != rb::RestirBackend::Rtxdi)
    {
        return L"PathTracing.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIR)
    {
        return L"PathTracingReSTIR.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRDI)
    {
        return L"PathTracingReSTIRDI.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRCombined)
    {
        return L"PathTracingReSTIRCombined.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRPT)
    {
        return L"PathTracingReSTIRPT.lib.cso";
    }
    if (m_mode == PathTracingMode::ReSTIRPTCombined)
    {
        return L"PathTracingReSTIRPTCombined.lib.cso";
    }
    return L"PathTracing.lib.cso";
}

std::wstring D3D12PathTracingBackend::DenoiseShaderFileName() const
{
    return L"PathTracingDenoise.cso";
}

UINT D3D12PathTracingBackend::MaxTraceRecursionDepth() const
{
    return 1u;
}

UINT D3D12PathTracingBackend::Align(UINT value, UINT alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
