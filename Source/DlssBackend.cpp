#include "stdafx.h"
#include "DlssBackend.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

#ifndef D3D12LOOKDEVPT_WITH_DLSS
#define D3D12LOOKDEVPT_WITH_DLSS 0
#endif

#if D3D12LOOKDEVPT_WITH_DLSS
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss_d.h>
#include <sl_helpers.h>
#endif

namespace
{
    std::wstring JoinPath(const std::wstring& base, const wchar_t* child)
    {
        return (std::filesystem::path(base) / child).wstring();
    }

    std::string WideToNarrowLossy(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t ch : text)
        {
            result.push_back(ch >= 0 && ch <= 127 ? static_cast<char>(ch) : '?');
        }
        return result;
    }

#if D3D12LOOKDEVPT_WITH_DLSS
    struct StreamlineApi
    {
        PFun_slInit* init = nullptr;
        PFun_slShutdown* shutdown = nullptr;
        PFun_slSetD3DDevice* setD3DDevice = nullptr;
        PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
        PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
        PFun_slEvaluateFeature* evaluateFeature = nullptr;
        PFun_slSetTagForFrame* setTagForFrame = nullptr;
        PFun_slSetConstants* setConstants = nullptr;
        PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    };

    StreamlineApi& Api()
    {
        static StreamlineApi api;
        return api;
    }

    template <typename T>
    bool LoadFunction(HMODULE module, const char* name, T*& out)
    {
        out = reinterpret_cast<T*>(GetProcAddress(module, name));
        return out != nullptr;
    }

    const char* ResultName(sl::Result result)
    {
        return sl::getResultAsStr(result);
    }

    sl::DLSSMode ToStreamlineMode(DlssMode mode)
    {
        switch (mode)
        {
        case DlssMode::Balanced:
            return sl::DLSSMode::eBalanced;
        case DlssMode::Performance:
            return sl::DLSSMode::eMaxPerformance;
        case DlssMode::UltraPerformance:
            return sl::DLSSMode::eUltraPerformance;
        default:
            return sl::DLSSMode::eMaxQuality;
        }
    }

    bool TryLoadDlssdFunction(const char* name, void*& function)
    {
        function = nullptr;
        if (!Api().getFeatureFunction)
        {
            return false;
        }
        const sl::Result result = Api().getFeatureFunction(sl::kFeatureDLSS_RR, name, function);
        return result == sl::Result::eOk && function != nullptr;
    }

    sl::float4x4 ToSlMatrix(const DirectX::XMFLOAT4X4& matrix)
    {
        sl::float4x4 result{};
        result.setRow(0u, sl::float4(matrix._11, matrix._12, matrix._13, matrix._14));
        result.setRow(1u, sl::float4(matrix._21, matrix._22, matrix._23, matrix._24));
        result.setRow(2u, sl::float4(matrix._31, matrix._32, matrix._33, matrix._34));
        result.setRow(3u, sl::float4(matrix._41, matrix._42, matrix._43, matrix._44));
        return result;
    }

    sl::float4x4 IdentitySlMatrix()
    {
        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        return ToSlMatrix(identity);
    }

    sl::Resource MakeTextureResource(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES state)
    {
        sl::Resource result(
            sl::ResourceType::eTex2d,
            resource,
            static_cast<uint32_t>(state));
        if (resource)
        {
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            result.width = static_cast<uint32_t>(desc.Width);
            result.height = desc.Height;
            result.nativeFormat = static_cast<uint32_t>(desc.Format);
            result.mipLevels = desc.MipLevels;
            result.arrayLayers = desc.DepthOrArraySize;
        }
        return result;
    }
#endif
}

DlssBackend::DlssBackend()
{
    m_status.compiled = D3D12LOOKDEVPT_WITH_DLSS != 0;
    if (!m_status.compiled)
    {
        m_status.fallbackReason = "DLSS was disabled at build time.";
    }
}

DlssBackend::~DlssBackend()
{
    Shutdown();
}

void DlssBackend::InitializeBeforeDevice(const std::wstring& executableDirectory)
{
    Shutdown();
    m_status = {};
    m_status.compiled = D3D12LOOKDEVPT_WITH_DLSS != 0;

#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)executableDirectory;
    m_status.fallbackReason = "DLSS was disabled at build time.";
    return;
#else
    const std::array<std::wstring, 4> candidates =
    {
        JoinPath(executableDirectory, L"Streamline\\sl.interposer.dll"),
        JoinPath(executableDirectory, L"sl.interposer.dll"),
        JoinPath(executableDirectory, L"Streamline\\bin\\x64\\sl.interposer.dll"),
        JoinPath(std::filesystem::current_path().wstring(), L"ThirdParty\\Streamline\\bin\\x64\\sl.interposer.dll"),
    };

    std::wstring runtimePath;
    for (const std::wstring& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            runtimePath = candidate;
            break;
        }
    }

    if (runtimePath.empty())
    {
        m_status.fallbackReason = "Streamline runtime DLL was not found.";
        m_status.lastError = "Missing sl.interposer.dll. Place it in Bin\\x64\\<Config>\\Streamline.";
        return;
    }

    HMODULE module = LoadLibraryW(runtimePath.c_str());
    if (!module)
    {
        const DWORD error = GetLastError();
        m_status.fallbackReason = "Streamline runtime DLL could not be loaded.";
        m_status.lastError = "LoadLibrary failed with Win32 error " + std::to_string(error) + ".";
        m_status.runtimePath = runtimePath;
        return;
    }

    m_runtime = module;
    m_status.runtimeAvailable = true;
    m_status.runtimePath = runtimePath;

    StreamlineApi api{};
    if (!LoadFunction(module, "slInit", api.init) ||
        !LoadFunction(module, "slShutdown", api.shutdown) ||
        !LoadFunction(module, "slSetD3DDevice", api.setD3DDevice) ||
        !LoadFunction(module, "slIsFeatureSupported", api.isFeatureSupported) ||
        !LoadFunction(module, "slGetFeatureFunction", api.getFeatureFunction) ||
        !LoadFunction(module, "slEvaluateFeature", api.evaluateFeature) ||
        !LoadFunction(module, "slSetTagForFrame", api.setTagForFrame) ||
        !LoadFunction(module, "slSetConstants", api.setConstants) ||
        !LoadFunction(module, "slGetNewFrameToken", api.getNewFrameToken))
    {
        m_status.fallbackReason = "Streamline runtime is missing required exports.";
        m_status.lastError = "Required Streamline core functions were not found.";
        FreeLibrary(module);
        m_runtime = nullptr;
        return;
    }
    Api() = api;

    const std::filesystem::path pluginPath = std::filesystem::path(runtimePath).parent_path();
    const wchar_t* pluginPaths[] = { pluginPath.c_str() };
    const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
    sl::Preferences preferences{};
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "D3D12LookDevPT";
    // Production NGX components require an application identity issued by
    // NVIDIA. Keep it outside project files/source control and never substitute
    // Streamline's temporary development id in a production plugin build.
    wchar_t applicationIdText[32] = {};
    size_t applicationIdLength = 0u;
    if (_wgetenv_s(
            &applicationIdLength,
            applicationIdText,
            std::size(applicationIdText),
            L"D3D12LOOKDEVPT_NGX_APPLICATION_ID") == 0 &&
        applicationIdLength > 1u)
    {
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::wcstoul(applicationIdText, &end, 10);
        if (errno == 0 && end && *end == L'\0' && parsed > 0u &&
            parsed <= UINT32_MAX)
        {
            m_applicationId = static_cast<uint32_t>(parsed);
            preferences.applicationId = m_applicationId;
            m_status.applicationIdentityConfigured = true;
        }
    }
    preferences.projectId = nullptr;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    const sl::Result initResult = Api().init(preferences, sl::kSDKVersion);
    m_status.lastResultCode = static_cast<int32_t>(initResult);
    if (initResult != sl::Result::eOk)
    {
        m_status.lastFailureStage = "slInit";
        m_status.fallbackReason = "Streamline initialization failed.";
        m_status.lastError = std::string("slInit returned ") + ResultName(initResult) + ".";
        return;
    }

    m_status.initialized = true;
    m_status.fallbackReason = m_status.applicationIdentityConfigured
        ? "DLSS-RR device support has not been checked yet."
        : "DLSS-RR requires D3D12LOOKDEVPT_NGX_APPLICATION_ID issued by NVIDIA.";
#endif
}

void DlssBackend::SetD3DDevice(ID3D12Device* device, IDXGIAdapter1* adapter, uint32_t outputWidth, uint32_t outputHeight, DlssMode mode)
{
#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)device;
    (void)adapter;
    (void)outputWidth;
    (void)outputHeight;
    (void)mode;
    return;
#else
    if (!m_status.initialized || !Api().setD3DDevice || !Api().isFeatureSupported)
    {
        return;
    }

    const sl::Result deviceResult = Api().setD3DDevice(device);
    m_status.lastResultCode = static_cast<int32_t>(deviceResult);
    if (deviceResult != sl::Result::eOk)
    {
        m_status.lastFailureStage = "slSetD3DDevice";
        m_status.fallbackReason = "Streamline did not accept the D3D12 device.";
        m_status.lastError = std::string("slSetD3DDevice returned ") + ResultName(deviceResult) + ".";
        return;
    }
    m_status.deviceRegistered = true;
    if (!m_status.applicationIdentityConfigured)
    {
        m_status.featureSupported = false;
        m_status.evaluationReady = false;
        m_status.fallbackReason =
            "DLSS-RR application identity is not configured; native reconstruction remains active.";
        m_status.lastError =
            "Set D3D12LOOKDEVPT_NGX_APPLICATION_ID to the NVIDIA-issued decimal NGX application ID.";
        m_status.lastFailureStage = "applicationIdentity";
        return;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (adapter)
    {
        adapter->GetDesc1(&desc);
    }
    sl::AdapterInfo adapterInfo{};
    adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
    adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);

    const sl::Result supportResult = Api().isFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
    m_status.lastResultCode = static_cast<int32_t>(supportResult);
    if (supportResult != sl::Result::eOk)
    {
        m_status.lastFailureStage = "slIsFeatureSupported";
        m_status.featureSupported = false;
        m_status.evaluationReady = false;
        m_status.fallbackReason = "DLSS Ray Reconstruction is not supported on this adapter or driver.";
        m_status.lastError = std::string("slIsFeatureSupported returned ") + ResultName(supportResult) + ".";
        return;
    }

    m_status.featureSupported = true;
    UpdateMode(outputWidth, outputHeight, mode);
#endif
}

void DlssBackend::UpdateMode(uint32_t outputWidth, uint32_t outputHeight, DlssMode mode)
{
#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)outputWidth;
    (void)outputHeight;
    (void)mode;
    return;
#else
    m_outputWidth = outputWidth;
    m_outputHeight = outputHeight;
    m_mode = mode;
    if (!m_status.initialized || !m_status.featureSupported)
    {
        return;
    }

    m_status.recommendedRenderWidth = outputWidth;
    m_status.recommendedRenderHeight = outputHeight;

    void* rawGetOptimalSettings = nullptr;
    if (!TryLoadDlssdFunction("slDLSSDGetOptimalSettings", rawGetOptimalSettings))
    {
        return;
    }

    auto getOptimalSettings = reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(rawGetOptimalSettings);
    sl::DLSSDOptions options{};
    options.mode = ToStreamlineMode(mode);
    options.outputWidth = outputWidth;
    options.outputHeight = outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;

    sl::DLSSDOptimalSettings settings{};
    const sl::Result result = getOptimalSettings(options, settings);
    m_status.lastResultCode = static_cast<int32_t>(result);
    if (result == sl::Result::eOk)
    {
        m_status.recommendedRenderWidth = settings.optimalRenderWidth;
        m_status.recommendedRenderHeight = settings.optimalRenderHeight;
    }
    else
    {
        m_status.lastFailureStage = "slDLSSDGetOptimalSettings";
        m_status.lastError = std::string("slDLSSDGetOptimalSettings returned ") + ResultName(result) + ".";
    }

    void* rawSetOptions = nullptr;
    if (!TryLoadDlssdFunction("slDLSSDSetOptions", rawSetOptions) ||
        !Api().evaluateFeature ||
        !Api().setTagForFrame ||
        !Api().setConstants ||
        !Api().getNewFrameToken)
    {
        m_status.evaluationReady = false;
        m_status.fallbackReason = "DLSS-RR per-frame functions are unavailable.";
        m_status.lastError =
            "The loaded Streamline runtime did not expose the complete tagging/evaluation API.";
        return;
    }

    auto setOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(rawSetOptions);
    options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
    options.worldToCameraView = IdentitySlMatrix();
    options.cameraViewToWorld = IdentitySlMatrix();
    const sl::ViewportHandle viewport(0u);
    const sl::Result optionsResult = setOptions(viewport, options);
    m_status.lastResultCode = static_cast<int32_t>(optionsResult);
    if (optionsResult != sl::Result::eOk)
    {
        m_status.lastFailureStage = "slDLSSDSetOptions";
        m_status.evaluationReady = false;
        m_status.fallbackReason = "DLSS-RR options were rejected.";
        m_status.lastError =
            std::string("slDLSSDSetOptions returned ") + ResultName(optionsResult) + ".";
        return;
    }

    m_status.evaluationReady = true;
    m_status.fallbackRebuildRequested = false;
    m_status.lastError.clear();
    m_status.lastFailureStage.clear();
    m_status.fallbackReason.clear();
#endif
}

void DlssBackend::ResetHistory()
{
    m_status.historyResetRequested = true;
}

bool DlssBackend::EvaluateRayReconstruction(const DlssEvaluationDesc& desc)
{
#if !D3D12LOOKDEVPT_WITH_DLSS
    (void)desc;
    return false;
#else
    const bool resourcesValid =
        desc.commandList &&
        desc.color &&
        desc.output &&
        desc.linearDepth &&
        desc.motion &&
        desc.normalRoughness &&
        desc.albedo &&
        desc.specularAlbedo &&
        desc.exposure &&
        desc.renderWidth > 0u &&
        desc.renderHeight > 0u &&
        desc.outputWidth > 0u &&
        desc.outputHeight > 0u;
    if (!m_status.evaluationReady || !resourcesValid)
    {
        m_status.lastEvaluationSucceeded = false;
        if (!resourcesValid)
        {
            m_status.lastError = "DLSS-RR evaluation received an incomplete resource contract.";
        }
        return false;
    }

    auto failEvaluation = [&](const char* operation, sl::Result result)
    {
        m_status.lastEvaluationSucceeded = false;
        ++m_status.failedEvaluations;
        m_status.evaluationReady = false;
        m_status.fallbackRebuildRequested = true;
        m_status.fallbackReason =
            "DLSS-RR runtime evaluation failed; native internal reconstruction is queued.";
        m_status.lastResultCode = static_cast<int32_t>(result);
        m_status.lastFailureStage = operation;
        m_status.lastError =
            std::string(operation) + " returned " + ResultName(result) + ".";
        return false;
    };

    sl::FrameToken* frameToken = nullptr;
    const sl::Result tokenResult =
        Api().getNewFrameToken(frameToken, &desc.frameIndex);
    if (tokenResult != sl::Result::eOk || !frameToken)
    {
        return failEvaluation("slGetNewFrameToken", tokenResult);
    }

    using namespace DirectX;
    const XMMATRIX viewToClip = XMLoadFloat4x4(&desc.viewToClip);
    const XMMATRIX previousViewToClip =
        XMLoadFloat4x4(&desc.previousViewToClip);
    const XMMATRIX worldToView = XMLoadFloat4x4(&desc.worldToView);
    const XMMATRIX previousWorldToView =
        XMLoadFloat4x4(&desc.previousWorldToView);
    const XMMATRIX viewToWorld = XMMatrixInverse(nullptr, worldToView);
    const XMMATRIX viewProjection = worldToView * viewToClip;
    const XMMATRIX previousViewProjection =
        previousWorldToView * previousViewToClip;
    const XMMATRIX clipToPrevClip =
        XMMatrixInverse(nullptr, viewProjection) * previousViewProjection;
    const XMMATRIX prevClipToClip =
        XMMatrixInverse(nullptr, clipToPrevClip);

    XMFLOAT4X4 clipToViewFloat{};
    XMFLOAT4X4 clipToPrevFloat{};
    XMFLOAT4X4 prevClipToClipFloat{};
    XMFLOAT4X4 viewToWorldFloat{};
    XMStoreFloat4x4(&clipToViewFloat, XMMatrixInverse(nullptr, viewToClip));
    XMStoreFloat4x4(&clipToPrevFloat, clipToPrevClip);
    XMStoreFloat4x4(&prevClipToClipFloat, prevClipToClip);
    XMStoreFloat4x4(&viewToWorldFloat, viewToWorld);

    sl::Constants constants{};
    constants.cameraViewToClip = ToSlMatrix(desc.viewToClip);
    constants.clipToCameraView = ToSlMatrix(clipToViewFloat);
    constants.clipToLensClip = IdentitySlMatrix();
    constants.clipToPrevClip = ToSlMatrix(clipToPrevFloat);
    constants.prevClipToClip = ToSlMatrix(prevClipToClipFloat);
    constants.jitterOffset = sl::float2(desc.jitter.x, desc.jitter.y);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(
        desc.cameraPosition.x,
        desc.cameraPosition.y,
        desc.cameraPosition.z);
    constants.cameraRight = sl::float3(
        viewToWorldFloat._11,
        viewToWorldFloat._12,
        viewToWorldFloat._13);
    constants.cameraUp = sl::float3(
        viewToWorldFloat._21,
        viewToWorldFloat._22,
        viewToWorldFloat._23);
    constants.cameraFwd = sl::float3(
        viewToWorldFloat._31,
        viewToWorldFloat._32,
        viewToWorldFloat._33);
    constants.cameraNear = desc.cameraNear;
    constants.cameraFar = desc.cameraFar;
    constants.cameraFOV = desc.cameraFovRadians;
    constants.cameraAspectRatio =
        static_cast<float>(desc.outputWidth) /
        static_cast<float>(desc.outputHeight);
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset =
        (desc.reset || m_status.historyResetRequested)
        ? sl::Boolean::eTrue
        : sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;

    const sl::ViewportHandle viewport(0u);
    sl::Result result =
        Api().setConstants(constants, *frameToken, viewport);
    if (result != sl::Result::eOk)
    {
        return failEvaluation("slSetConstants", result);
    }

    void* rawSetOptions = nullptr;
    if (!TryLoadDlssdFunction("slDLSSDSetOptions", rawSetOptions))
    {
        return failEvaluation(
            "slGetFeatureFunction(slDLSSDSetOptions)",
            sl::Result::eErrorFeatureMissing);
    }
    auto setOptions =
        reinterpret_cast<PFun_slDLSSDSetOptions*>(rawSetOptions);
    sl::DLSSDOptions options{};
    options.mode = ToStreamlineMode(m_mode);
    options.outputWidth = desc.outputWidth;
    options.outputHeight = desc.outputHeight;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.preExposure = 1.0f;
    options.exposureScale = 1.0f;
    options.normalRoughnessMode =
        sl::DLSSDNormalRoughnessMode::ePacked;
    options.worldToCameraView = ToSlMatrix(desc.worldToView);
    options.cameraViewToWorld = ToSlMatrix(viewToWorldFloat);
    result = setOptions(viewport, options);
    if (result != sl::Result::eOk)
    {
        return failEvaluation("slDLSSDSetOptions", result);
    }

    sl::Resource color = MakeTextureResource(
        desc.color,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource output = MakeTextureResource(
        desc.output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource depth = MakeTextureResource(
        desc.linearDepth,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource motion = MakeTextureResource(
        desc.motion,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource normalRoughness = MakeTextureResource(
        desc.normalRoughness,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource albedo = MakeTextureResource(
        desc.albedo,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource specularAlbedo = MakeTextureResource(
        desc.specularAlbedo,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource exposure = MakeTextureResource(
        desc.exposure,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const sl::Extent renderExtent{
        0u,
        0u,
        desc.renderWidth,
        desc.renderHeight };
    const sl::Extent outputExtent{
        0u,
        0u,
        desc.outputWidth,
        desc.outputHeight };
    const sl::Extent exposureExtent{ 0u, 0u, 1u, 1u };
    const std::array<sl::ResourceTag, 8> tags =
    {
        sl::ResourceTag(
            &color,
            sl::kBufferTypeScalingInputColor,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &output,
            sl::kBufferTypeScalingOutputColor,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &outputExtent),
        sl::ResourceTag(
            &depth,
            sl::kBufferTypeLinearDepth,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &motion,
            sl::kBufferTypeMotionVectors,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &normalRoughness,
            sl::kBufferTypeNormalRoughness,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &albedo,
            sl::kBufferTypeAlbedo,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &specularAlbedo,
            sl::kBufferTypeSpecularAlbedo,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &renderExtent),
        sl::ResourceTag(
            &exposure,
            sl::kBufferTypeExposure,
            sl::ResourceLifecycle::eValidUntilEvaluate,
            &exposureExtent),
    };
    auto* commandBuffer =
        reinterpret_cast<sl::CommandBuffer*>(desc.commandList);
    result = Api().setTagForFrame(
        *frameToken,
        viewport,
        tags.data(),
        static_cast<uint32_t>(tags.size()),
        commandBuffer);
    if (result != sl::Result::eOk)
    {
        return failEvaluation("slSetTagForFrame", result);
    }

    result = Api().evaluateFeature(
        sl::kFeatureDLSS_RR,
        *frameToken,
        nullptr,
        0u,
        commandBuffer);
    if (result != sl::Result::eOk)
    {
        return failEvaluation("slEvaluateFeature", result);
    }

    m_status.lastEvaluationSucceeded = true;
    ++m_status.successfulEvaluations;
    m_status.lastResultCode = static_cast<int32_t>(result);
    m_status.historyResetRequested = false;
    m_status.lastError.clear();
    m_status.lastFailureStage.clear();
    return true;
#endif
}

bool DlssBackend::ConsumeFallbackRebuildRequest()
{
    const bool requested = m_status.fallbackRebuildRequested;
    m_status.fallbackRebuildRequested = false;
    return requested;
}

void DlssBackend::Shutdown()
{
#if D3D12LOOKDEVPT_WITH_DLSS
    if (m_status.initialized && Api().shutdown)
    {
        Api().shutdown();
    }
    if (m_runtime)
    {
        FreeLibrary(reinterpret_cast<HMODULE>(m_runtime));
        m_runtime = nullptr;
    }
    Api() = {};
#endif
    m_outputWidth = 0u;
    m_outputHeight = 0u;
    m_applicationId = 0u;
}
