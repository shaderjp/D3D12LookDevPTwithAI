#include "stdafx.h"
#include "D3D12PathTracingBackend.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace
{
using lookdevpt::winui::EditorCommand;
using lookdevpt::winui::EditorValue;

double Number(EditorValue const& value, double fallback = 0.0)
{
    if (auto number = std::get_if<double>(&value))
    {
        return std::isfinite(*number) ? *number : fallback;
    }
    if (auto integer = std::get_if<std::int64_t>(&value))
    {
        return static_cast<double>(*integer);
    }
    return fallback;
}

std::int64_t Integer(EditorValue const& value, std::int64_t fallback = 0)
{
    if (auto integer = std::get_if<std::int64_t>(&value))
    {
        return *integer;
    }
    if (auto number = std::get_if<double>(&value))
    {
        return std::isfinite(*number)
            ? static_cast<std::int64_t>(*number)
            : fallback;
    }
    return fallback;
}

bool Boolean(EditorValue const& value, bool fallback = false)
{
    if (auto boolean = std::get_if<bool>(&value))
    {
        return *boolean;
    }
    return fallback;
}

std::wstring Wide(EditorValue const& value)
{
    if (auto text = std::get_if<std::wstring>(&value))
    {
        return *text;
    }
    if (auto text = std::get_if<std::string>(&value))
    {
        if (text->empty())
        {
            return {};
        }
        const int size = MultiByteToWideChar(
            CP_UTF8, 0, text->data(), static_cast<int>(text->size()),
            nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, text->data(), static_cast<int>(text->size()),
            result.data(), size);
        return result;
    }
    return {};
}

std::string Utf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Widen(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size);
    return result;
}

template<typename T>
void SetNumber(
    EditorValue const& value,
    T& target,
    T minimum,
    T maximum)
{
    if constexpr (std::is_integral_v<T>)
    {
        target = std::clamp(
            static_cast<T>(Integer(value, target)),
            minimum,
            maximum);
    }
    else
    {
        target = std::clamp(
            static_cast<T>(Number(value, target)),
            minimum,
            maximum);
    }
}

bool SetColor3(
    EditorValue const& value,
    float (&target)[3])
{
    if (auto color = std::get_if<std::array<double, 4>>(&value))
    {
        for (size_t index = 0; index < 3; ++index)
        {
            target[index] = static_cast<float>(
                std::clamp((*color)[index], 0.0, 1.0));
        }
        return true;
    }
    if (auto color = std::get_if<std::array<double, 3>>(&value))
    {
        for (size_t index = 0; index < 3; ++index)
        {
            target[index] = static_cast<float>(
                std::clamp((*color)[index], 0.0, 1.0));
        }
        return true;
    }
    return false;
}

constexpr wchar_t const* TextureNames[] =
{
    L"Base Color",
    L"Normal",
    L"Roughness",
    L"Metallic",
    L"Occlusion",
    L"Emissive",
    L"Alpha",
};
}

void D3D12PathTracingBackend::SetViewportFocused(bool focused) noexcept
{
    m_viewportFocused = focused;
    m_camera.SetActive(focused);
}

void D3D12PathTracingBackend::AddEditorCpuTime(
    double milliseconds) noexcept
{
    if (std::isfinite(milliseconds) && milliseconds > 0.0)
    {
        m_pendingEditorCpuMs += milliseconds;
    }
}

void D3D12PathTracingBackend::ApplyEditorCommand(
    EditorCommand const& command)
{
    using namespace lookdevpt::winui;

    if (command.type == EditorCommandType::LoadScene)
    {
        m_pendingScenePath = command.path;
        m_sceneDiagnostics = "Scene load queued.";
        return;
    }
    if (command.type == EditorCommandType::LoadEnvironment)
    {
        m_pendingEnvironmentPath = command.path;
        m_projectDiagnostics = "Environment load queued.";
        return;
    }
    if (command.type == EditorCommandType::LoadProject)
    {
        m_pendingProjectPath = command.path;
        m_projectDiagnostics = "Project load queued.";
        return;
    }
    if (command.type == EditorCommandType::SaveProjectAs)
    {
        SaveProjectToDisk(command.path);
        return;
    }
    if (command.type == EditorCommandType::LoadMaterialTexture)
    {
        std::string diagnostics;
        ApplyMaterialTextureOverride(
            m_selectedMaterial,
            static_cast<UINT>(std::clamp(
                command.index, 0,
                static_cast<int>(TextureSlotCount) - 1)),
            command.path,
            true,
            diagnostics);
        m_projectDiagnostics = diagnostics;
        return;
    }
    if (command.type == EditorCommandType::Pointer)
    {
        if (command.pointer.type == PointerEventType::Pressed ||
            command.pointer.type == PointerEventType::Released ||
            command.pointer.type == PointerEventType::Exited)
        {
            m_camera.OnPointerButton(
                command.pointer.type == PointerEventType::Pressed &&
                    command.pointer.rightButton,
                command.pointer.x,
                command.pointer.y);
        }
        else if (command.pointer.type == PointerEventType::Moved)
        {
            m_camera.OnPointerMove(
                command.pointer.x,
                command.pointer.y);
        }
        return;
    }
    if (command.type == EditorCommandType::Key)
    {
        m_camera.SetKeyDown(command.key.virtualKey, command.key.down);
        if (command.key.down && command.key.virtualKey == VK_SPACE)
        {
            OnKeyDown(VK_SPACE);
        }
        if (command.key.down && command.key.virtualKey == VK_F10)
        {
            m_renderOnlyMode = !m_renderOnlyMode;
        }
        return;
    }

    auto invalidate = [&](rb::FrameChangeMask change)
    {
        InvalidateHistory(change);
        m_projectDirty = true;
    };
    auto materialChanged = [&](bool textureReload = false)
    {
        RequestGpuResourceRefresh(
            textureReload
                ? PendingGpuResourceRefresh::MaterialTextures
                : PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Material);
    };

    if (command.type == EditorCommandType::Action)
    {
        std::wstring const& action = command.property;
        if (action == L"project.save" && !m_projectPath.empty())
        {
            SaveProjectToDisk(m_projectPath);
        }
        else if (action == L"startup.save")
        {
            SaveStartupSettingsToDisk();
        }
        else if (action == L"startup.clear")
        {
            DeleteStartupSettings();
        }
        else if (action == L"history.reset")
        {
            ResetRenderingHistory();
        }
        else if (action == L"scene.cancelLoad")
        {
            CancelAsyncSceneLoad();
        }
        else if (action == L"camera.reset")
        {
            ResetCameraView();
            invalidate(rb::FrameChangeMask::CameraCut);
        }
        else if (action == L"camera.speed.reset")
        {
            ResetCameraSpeeds();
        }
        else if (action == L"gamepad.recalibrate")
        {
            m_camera.RecalibrateGamepad();
        }
        else if (action == L"gamepad.standardCenter")
        {
            m_camera.UseDefaultGamepadCenter();
        }
        else if (action == L"light.reset")
        {
            ResetLight();
            RequestGpuResourceRefresh(PendingGpuResourceRefresh::MaterialData);
            invalidate(rb::FrameChangeMask::Light);
        }
        else if (action == L"denoise.reset")
        {
            ResetDenoiseHistory();
        }
        else if (action == L"dlss.reset")
        {
            m_dlssBackendRuntime.ResetHistory();
            ResetDenoiseHistory();
        }
        else if (action == L"nrd.reset")
        {
            m_nrdBackendRuntime.ResetHistory();
            ResetDenoiseHistory();
        }
        else if (action == L"restir.reset")
        {
            invalidate(rb::FrameChangeMask::Backend);
        }
        else if (action == L"material.reset" &&
                 !m_scene.materials.empty())
        {
            ResetMaterialToSource(m_selectedMaterial);
            materialChanged(true);
        }
        else if (action == L"material.texture.clear")
        {
            std::string diagnostics;
            ApplyMaterialTextureOverride(
                m_selectedMaterial,
                static_cast<UINT>(std::clamp(
                    command.index, 0,
                    static_cast<int>(TextureSlotCount) - 1)),
                {},
                true,
                diagnostics);
            m_projectDiagnostics = diagnostics;
        }
        else if (action == L"material.texture.reset")
        {
            const UINT slot = static_cast<UINT>(std::clamp(
                command.index, 0,
                static_cast<int>(TextureSlotCount) - 1));
            std::wstring source;
            if (m_selectedMaterial >= 0 &&
                static_cast<size_t>(m_selectedMaterial) <
                    m_sourceMaterials.size())
            {
                source =
                    m_sourceMaterials[m_selectedMaterial].textures[slot];
            }
            std::string diagnostics;
            ApplyMaterialTextureOverride(
                m_selectedMaterial,
                slot,
                source,
                false,
                diagnostics);
            m_projectDiagnostics = diagnostics;
        }
        else if (action == L"material.texture.resolution" &&
                 m_selectedMaterial >= 0 && static_cast<size_t>(m_selectedMaterial) < m_scene.materials.size())
        {
            const UINT slot = static_cast<UINT>(std::clamp(command.index, 0, static_cast<int>(TextureSlotCount) - 1));
            Bistro::Material& material = m_scene.materials[m_selectedMaterial];
            material.textureBindings[slot].resolutionPolicy = static_cast<uint32_t>(std::clamp(
                static_cast<int>(std::get<std::int64_t>(command.value)), 0, 5));
            if (static_cast<size_t>(m_selectedMaterial) < m_textureBindingOverrideEnabled.size())
            {
                m_textureBindingOverrideEnabled[m_selectedMaterial][slot] = true;
            }
            materialChanged(true);
        }
        else if (action == L"material.compare.setA")
        {
            m_materialCompareA =
                CaptureMaterialSnapshot(m_selectedMaterial);
            m_hasMaterialCompareA = true;
        }
        else if (action == L"material.compare.setB")
        {
            m_materialCompareB =
                CaptureMaterialSnapshot(m_selectedMaterial);
            m_hasMaterialCompareB = true;
        }
        else if ((action == L"material.compare.applyA" &&
                  m_hasMaterialCompareA) ||
                 (action == L"material.compare.applyB" &&
                  m_hasMaterialCompareB))
        {
            MaterialSnapshot const& target =
                action.ends_with(L"applyA")
                    ? m_materialCompareA
                    : m_materialCompareB;
            MaterialSnapshot before =
                CaptureMaterialSnapshot(m_selectedMaterial);
            ApplyMaterialSnapshot(
                m_selectedMaterial, target, true);
            RequestGpuResourceRefresh(
                RequiresMaterialBlasRebuild(before, target)
                    ? PendingGpuResourceRefresh::FullScene
                    : PendingGpuResourceRefresh::MaterialTextures);
            invalidate(rb::FrameChangeMask::Material);
        }
        else if (action == L"material.variant.save")
        {
            cld::JsonValue params;
            params.type = cld::JsonValue::Type::Object;
            cld::JsonValue index;
            index.type = cld::JsonValue::Type::Number;
            index.number = m_selectedMaterial;
            cld::JsonValue name;
            name.type = cld::JsonValue::Type::String;
            name.string = Utf8(Wide(command.value));
            params.object["index"] = index;
            params.object["variant"] = name;
            std::string diagnostics;
            ApplyAction(
                "save_material_variant",
                params,
                diagnostics,
                false);
            m_projectDiagnostics = diagnostics;
        }
        else if (action == L"material.variant.apply" &&
                 command.index >= 0 &&
                 static_cast<size_t>(command.index) <
                    m_materialVariants.size())
        {
            MaterialSnapshot const& target =
                m_materialVariants[command.index].snapshot;
            MaterialSnapshot before =
                CaptureMaterialSnapshot(m_selectedMaterial);
            ApplyMaterialSnapshot(
                m_selectedMaterial, target, true);
            RequestGpuResourceRefresh(
                RequiresMaterialBlasRebuild(before, target)
                    ? PendingGpuResourceRefresh::FullScene
                    : PendingGpuResourceRefresh::MaterialTextures);
            invalidate(rb::FrameChangeMask::Material);
        }
        else if (action == L"material.variant.delete" &&
                 command.index >= 0 &&
                 static_cast<size_t>(command.index) <
                    m_materialVariants.size())
        {
            m_materialVariants.erase(
                m_materialVariants.begin() + command.index);
            ++m_mcpMaterialCatalogRevision;
            m_projectDirty = true;
        }
        else if (action == L"material.preset.save")
        {
            std::string diagnostics;
            SaveUserMaterialPreset(
                Utf8(Wide(command.value)),
                m_selectedMaterial,
                diagnostics);
            m_projectDiagnostics = diagnostics;
        }
        else if (action == L"material.preset.reload")
        {
            LoadMaterialPresets();
        }
        else if (action == L"material.preset.apply" &&
                 command.index >= 0)
        {
            std::string diagnostics;
            if (ApplyMaterialPreset(
                    m_selectedMaterial,
                    static_cast<size_t>(command.index),
                    diagnostics))
            {
                m_projectDirty = true;
            }
            m_projectDiagnostics = diagnostics;
        }
        else if (action == L"mcp.start")
        {
            StartMcpServer();
        }
        else if (action == L"mcp.stop")
        {
            StopMcpServer();
        }
        else if (action == L"mcp.review.cancel")
        {
            std::lock_guard lock(m_mcpSnapshotMutex);
            for (auto& review : m_mcpReviews)
            {
                if (review.id == m_activeMcpReviewId)
                {
                    review.cancelRequested = true;
                    break;
                }
            }
        }
        else if (action == L"mcp.pairing.begin")
        {
            if (m_mcpServer.IsRunning())
            {
                m_mcpServer.BeginPairing();
                m_mcpUiDiagnostics = "LocalMCPChatClient pairing code created for 90 seconds.";
            }
            else
            {
                m_mcpUiDiagnostics = "Start the MCP server before pairing.";
            }
        }
        else if (action == L"mcp.pairing.revokeAll")
        {
            m_mcpServer.RevokeAllPairedClients();
            m_mcpUiDiagnostics = "All paired LocalMCPChatClient tokens were revoked.";
        }
        else if (action == L"mcp.pairing.revoke")
        {
            const std::string clientId = Utf8(Wide(command.value));
            m_mcpUiDiagnostics = m_mcpServer.RevokePairedClient(clientId)
                ? "The selected LocalMCPChatClient token was revoked."
                : "The selected paired client no longer exists.";
        }
        else if (action == L"mcp.token.regenerate")
        {
            {
                std::lock_guard lock(m_mcpSettingsMutex);
                m_mcpSettings.token = mcp::GenerateToken();
            }
            SaveMcpUserSettings();
            m_mcpUiDiagnostics =
                "Bearer token regenerated. Restart MCP server to apply it.";
        }
        else if (action == L"mcp.approve")
        {
            m_mcpDispatcher.Approve(command.id);
        }
        else if (action == L"mcp.reject")
        {
            m_mcpDispatcher.Reject(
                command.id,
                "MCP request was rejected in the WinUI editor.");
        }
        return;
    }

    if (command.type != EditorCommandType::SetValue)
    {
        return;
    }

    std::wstring const& property = command.property;
    if (property == L"viewport.focused")
    {
        SetViewportFocused(Boolean(command.value));
    }
    else if (property == L"viewport.renderOnly")
    {
        m_renderOnlyMode = Boolean(command.value);
    }
    else if (property == L"viewport.resolution")
    {
        m_displayResolutionPreset = static_cast<int>(
            std::clamp<std::int64_t>(
                Integer(command.value, 1), 0, 2));
        constexpr UINT widths[] = { 1280, 1920, 3840 };
        constexpr UINT heights[] = { 720, 1080, 2160 };
        m_pendingResizeWidth = widths[m_displayResolutionPreset];
        m_pendingResizeHeight = heights[m_displayResolutionPreset];
        m_resizePending = true;
    }
    else if (property == L"renderer.mode")
    {
        m_mode = static_cast<PathTracingMode>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 5));
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"viewport.debugView")
    {
        SetNumber(command.value, m_debugViewMode, 0, 64);
        if (IsNrdSelected())
        {
            RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        }
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"viewport.normalYFlip")
    {
        m_debugNormalMapYFlip = Boolean(command.value);
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"viewport.toneMapper")
    {
        m_toneMapper = static_cast<ToneMapper>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 2));
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"viewport.exposure")
    {
        SetNumber(command.value, m_exposure, -6.0f, 6.0f);
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"viewport.gamma")
    {
        SetNumber(command.value, m_gamma, 1.0f, 3.0f);
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"quality.profile")
    {
        m_qualitySettings.qualityProfile =
            static_cast<rb::QualityProfile>(std::clamp<std::int64_t>(
                Integer(command.value), 0, 2));
        ApplyQualitySettingsToRenderer();
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(
            rb::FrameChangeMask::Backend |
            rb::FrameChangeMask::QualityProfile);
    }
    else if (property == L"quality.restirBackend")
    {
        m_qualitySettings.restirBackend =
            static_cast<rb::RestirBackend>(std::clamp<std::int64_t>(
                Integer(command.value), 0, 1));
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"quality.secondaryRate")
    {
        m_qualitySettings.secondaryShadingRate =
            static_cast<rb::SecondaryShadingRate>(
                std::clamp<std::int64_t>(Integer(command.value), 0, 2));
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"quality.resolutionMode")
    {
        m_qualitySettings.resolutionMode =
            static_cast<rb::ResolutionMode>(std::clamp<std::int64_t>(
                Integer(command.value), 0, 2));
        ApplyConfiguredRenderScale(true);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Resolution);
    }
    else if (property == L"quality.fixedRenderScale" ||
             property == L"quality.minRenderScale" ||
             property == L"quality.maxRenderScale")
    {
        if (property == L"quality.fixedRenderScale")
        {
            SetNumber(
                command.value,
                m_qualitySettings.fixedRenderScale,
                0.25f,
                1.0f);
        }
        else if (property == L"quality.minRenderScale")
        {
            SetNumber(
                command.value,
                m_qualitySettings.minRenderScale,
                0.25f,
                m_qualitySettings.maxRenderScale);
        }
        else
        {
            SetNumber(
                command.value,
                m_qualitySettings.maxRenderScale,
                m_qualitySettings.minRenderScale,
                1.0f);
        }
        m_qualitySettings.fixedRenderScale = std::clamp(
            m_qualitySettings.fixedRenderScale,
            m_qualitySettings.minRenderScale,
            m_qualitySettings.maxRenderScale);
        ApplyConfiguredRenderScale(true);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Resolution);
    }
    else if (property == L"quality.finalTaa")
    {
        m_qualitySettings.finalTaa = Boolean(command.value);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::DenoiserSettings);
    }
    else if (property == L"quality.sharpen")
    {
        SetNumber(
            command.value,
            m_qualitySettings.sharpenStrength,
            0.0f,
            1.0f);
        invalidate(rb::FrameChangeMask::View);
    }
    else if (property == L"quality.movingSpp" ||
             property == L"quality.movingBounces" ||
             property == L"quality.staticBaseSpp" ||
             property == L"quality.staticMaxSpp" ||
             property == L"quality.staticBounces" ||
             property == L"quality.settleFrames" ||
             property == L"quality.targetGpuMs" ||
             property == L"quality.referenceSpp")
    {
        auto& budget = m_qualitySettings.rayBudget;
        if (property == L"quality.movingSpp")
            SetNumber(command.value, budget.movingSpp, 1u, 8u);
        else if (property == L"quality.movingBounces")
            SetNumber(command.value, budget.movingBounces, 1u, 16u);
        else if (property == L"quality.staticBaseSpp")
            SetNumber(command.value, budget.staticBaseSpp, 1u, 8u);
        else if (property == L"quality.staticMaxSpp")
            SetNumber(command.value, budget.staticMaxSpp, 1u, 16u);
        else if (property == L"quality.staticBounces")
            SetNumber(command.value, budget.staticBounces, 1u, 16u);
        else if (property == L"quality.settleFrames")
            SetNumber(command.value, budget.settleFrames, 0u, 120u);
        else if (property == L"quality.targetGpuMs")
            SetNumber(command.value, budget.targetGpuMs, 1.0f, 100.0f);
        else
            SetNumber(
                command.value,
                m_qualitySettings.referenceSpp,
                1u,
                1048576u);
        budget.staticMaxSpp = (std::max)(
            budget.staticMaxSpp,
            budget.staticBaseSpp);
        if (m_qualitySettings.qualityProfile ==
                rb::QualityProfile::ReferenceStill)
        {
            m_maxAccumulatedFrames = static_cast<int>(
                (std::min)(m_qualitySettings.referenceSpp, 1048576u));
        }
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"viewport.vsync")
    {
        m_vsyncEnabled = Boolean(command.value);
    }
    else if (property == L"camera.moveSpeed")
    {
        SetNumber(command.value, m_baseMoveSpeed, 0.1f, 50.0f);
        m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
        m_projectDirty = true;
    }
    else if (property == L"camera.fastSpeed")
    {
        SetNumber(command.value, m_fastMoveSpeed, 0.1f, 100.0f);
        m_camera.SetMoveSpeeds(m_baseMoveSpeed, m_fastMoveSpeed);
        m_projectDirty = true;
    }
    else if (property == L"camera.position.x" ||
             property == L"camera.position.y" ||
             property == L"camera.position.z" ||
             property == L"camera.yawDegrees" ||
             property == L"camera.pitchDegrees" ||
             property == L"camera.rollDegrees" ||
             property == L"camera.fovDegrees")
    {
        XMFLOAT3 position = m_camera.GetPosition();
        float yaw = m_camera.GetYawRadians();
        float pitch = m_camera.GetPitchRadians();
        float roll = m_camera.GetRollRadians();
        if (property == L"camera.position.x")
            SetNumber(command.value, position.x, -10000.0f, 10000.0f);
        else if (property == L"camera.position.y")
            SetNumber(command.value, position.y, -10000.0f, 10000.0f);
        else if (property == L"camera.position.z")
            SetNumber(command.value, position.z, -10000.0f, 10000.0f);
        else if (property == L"camera.yawDegrees")
            yaw = XMConvertToRadians(static_cast<float>(
                std::clamp(Number(command.value), -360.0, 360.0)));
        else if (property == L"camera.pitchDegrees")
            pitch = XMConvertToRadians(static_cast<float>(
                std::clamp(Number(command.value), -83.0, 83.0)));
        else if (property == L"camera.rollDegrees")
            roll = XMConvertToRadians(static_cast<float>(
                std::clamp(Number(command.value), -180.0, 180.0)));
        else
            SetNumber(command.value, m_cameraFovDegrees, 1.0f, 179.0f);
        m_camera.Reset(position, yaw, pitch, roll);
        invalidate(rb::FrameChangeMask::CameraCut);
    }
    else if (property == L"gamepad.enabled")
    {
        m_camera.SetGamepadEnabled(Boolean(command.value));
    }
    else if (property == L"gamepad.lookSpeed")
    {
        m_camera.SetGamepadLookSpeed(
            static_cast<float>(Number(command.value, 2.5)));
    }
    else if (property == L"gamepad.invertY")
    {
        m_camera.SetGamepadInvertY(Boolean(command.value));
    }
    else if (property == L"material.selected")
    {
        m_selectedMaterial = static_cast<int>(
            std::clamp<std::int64_t>(
                Integer(command.value),
                0,
                (std::max<std::int64_t>)(
                    static_cast<std::int64_t>(
                        m_scene.materials.size()) - 1,
                    0)));
    }
    else if (!m_scene.materials.empty() &&
             property.starts_with(L"material."))
    {
        MaterialSnapshot const before =
            CaptureMaterialSnapshot(m_selectedMaterial);
        Bistro::Material& material =
            m_scene.materials[static_cast<size_t>(
                std::clamp(
                    m_selectedMaterial, 0,
                    static_cast<int>(
                        m_scene.materials.size()) - 1))];
        if (property == L"material.roughness")
            SetNumber(command.value, material.roughnessFactor, 0.02f, 1.0f);
        else if (property == L"material.metallic")
            SetNumber(command.value, material.metallicFactor, 0.0f, 1.0f);
        else if (property == L"material.occlusion")
            SetNumber(command.value, material.occlusionStrength, 0.0f, 2.0f);
        else if (property == L"material.normalStrength")
            SetNumber(command.value, material.normalStrength, 0.0f, 2.0f);
        else if (property == L"material.alphaCutoff")
            SetNumber(command.value, material.alphaCutoff, 0.0f, 1.0f);
        else if (property == L"material.alphaMasked")
            material.alphaMasked = Boolean(command.value);
        else if (property == L"material.packedOrm")
            material.packedOcclusionRoughnessMetallic =
                Boolean(command.value);
        else if (property == L"material.focus")
            m_materialFocusMode = static_cast<MaterialFocusMode>(
                std::clamp<std::int64_t>(
                    Integer(command.value), 0, 2));
        else if (property == L"material.baseColor")
        {
            if (auto color =
                    std::get_if<std::array<double, 4>>(
                        &command.value))
            {
                material.baseColorFactor = XMFLOAT4(
                    static_cast<float>(
                        std::clamp((*color)[0], 0.0, 1.0)),
                    static_cast<float>(
                        std::clamp((*color)[1], 0.0, 1.0)),
                    static_cast<float>(
                        std::clamp((*color)[2], 0.0, 1.0)),
                    static_cast<float>(
                        std::clamp((*color)[3], 0.0, 1.0)));
            }
        }
        else if (property == L"material.emissive")
        {
            if (auto color =
                    std::get_if<std::array<double, 4>>(
                        &command.value))
            {
                material.emissiveFactor = XMFLOAT4(
                    static_cast<float>(
                        std::clamp((*color)[0], 0.0, 100.0)),
                    static_cast<float>(
                        std::clamp((*color)[1], 0.0, 100.0)),
                    static_cast<float>(
                        std::clamp((*color)[2], 0.0, 100.0)),
                    static_cast<float>(
                        std::clamp((*color)[3], 0.0, 100.0)));
            }
        }
        MaterialSnapshot const after =
            CaptureMaterialSnapshot(m_selectedMaterial);
        RequestGpuResourceRefresh(
            RequiresMaterialBlasRebuild(before, after)
                ? PendingGpuResourceRefresh::FullScene
                : PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Material);
    }
    else if (property == L"lighting.direction")
    {
        if (auto value =
                std::get_if<std::array<double, 3>>(
                    &command.value))
        {
            for (size_t i = 0; i < 3; ++i)
                m_lightDirection[i] = static_cast<float>(
                    std::clamp((*value)[i], -1.0, 1.0));
            invalidate(rb::FrameChangeMask::Light);
        }
    }
    else if (property == L"lighting.direction.x" ||
             property == L"lighting.direction.y" ||
             property == L"lighting.direction.z")
    {
        const size_t component =
            property.ends_with(L".x") ? 0 :
            property.ends_with(L".y") ? 1 : 2;
        SetNumber(
            command.value,
            m_lightDirection[component],
            -1.0f,
            1.0f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.lightColor")
    {
        if (SetColor3(command.value, m_lightColor))
            invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.intensity")
    {
        SetNumber(command.value, m_lightIntensity, 0.0f, 20.0f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.rayTMin")
    {
        SetNumber(command.value, m_rayTMin, 0.001f, 0.25f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyEnabled")
    {
        m_skyEnabled = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyIntensity")
    {
        SetNumber(command.value, m_skyIntensity, 0.0f, 10.0f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyColor")
    {
        if (SetColor3(command.value, m_skyColor))
            invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyHorizonColor")
    {
        if (SetColor3(command.value, m_skyHorizonColor))
            invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyZenithColor")
    {
        if (SetColor3(command.value, m_skyZenithColor))
            invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyGroundColor")
    {
        if (SetColor3(command.value, m_skyGroundColor))
            invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.sunIntensity")
    {
        SetNumber(command.value, m_sunIntensity, 0.0f, 50.0f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.sunSize")
    {
        SetNumber(command.value, m_sunAngularRadius, 0.001f, 0.08f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.environmentEnabled")
    {
        m_environmentMapEnabled = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.environmentIntensity")
    {
        SetNumber(command.value, m_environmentIntensity, 0.0f, 10.0f);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.environmentRotation")
    {
        SetNumber(command.value, m_environmentRotation, -XM_PI, XM_PI);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.sunNee")
    {
        m_shadowEnabled = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.skyNee")
    {
        m_skyNeeEnabled = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.emissiveEnabled")
    {
        m_emissiveLightsEnabled = Boolean(command.value);
        RequestGpuResourceRefresh(
            PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.emissiveIntensity")
    {
        SetNumber(
            command.value,
            m_emissiveLightIntensity,
            0.0f,
            30.0f);
        RequestGpuResourceRefresh(
            PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.areaEnabled")
    {
        m_proceduralLightsEnabled = Boolean(command.value);
        RequestGpuResourceRefresh(
            PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"lighting.areaIntensity")
    {
        SetNumber(
            command.value,
            m_proceduralLightIntensity,
            0.0f,
            50.0f);
        RequestGpuResourceRefresh(
            PendingGpuResourceRefresh::MaterialData);
        invalidate(rb::FrameChangeMask::Light);
    }
    else if (property == L"path.samples")
    {
        SetNumber(command.value, m_giSamplesPerFrame, 1, 8);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.maxBounces")
    {
        SetNumber(command.value, m_maxPathBounces, 1, 8);
        m_minPathBounces =
            (std::min)(m_minPathBounces, m_maxPathBounces);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.minBounces")
    {
        SetNumber(command.value, m_minPathBounces, 0, 4);
        m_minPathBounces =
            (std::min)(m_minPathBounces, m_maxPathBounces);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.radianceClamp")
    {
        SetNumber(command.value, m_giRadianceClamp, 1.0f, 100.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.temporalClamp")
    {
        SetNumber(
            command.value,
            m_giTemporalClampScale,
            0.25f,
            4.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.maxAccumulation")
    {
        SetNumber(
            command.value,
            m_maxAccumulatedFrames,
            1,
            4096);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.freeze")
    {
        m_freezeAccumulation = Boolean(command.value);
    }
    else if (property == L"path.adaptive")
    {
        m_adaptiveSamplingEnabled = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.maxAdaptive")
    {
        SetNumber(
            command.value,
            m_maxAdaptiveSamplesPerPixel,
            1,
            4);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.varianceThreshold")
    {
        SetNumber(
            command.value,
            m_adaptiveVarianceThreshold,
            0.02f,
            1.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"path.disocclusionBoost")
    {
        SetNumber(
            command.value,
            m_adaptiveDisocclusionBoost,
            0.0f,
            4.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"denoise.enabled")
    {
        m_denoiserEnabled = Boolean(command.value);
        if (IsNrdSelected())
        {
            RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        }
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.backend")
    {
        m_denoiseBackend = static_cast<DenoiseBackend>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 4));
        if (m_device)
        {
            m_nrdBackendRuntime.UpdateMethod(
                m_device.Get(), SelectedNrdMethod());
        }
        ApplyConfiguredRenderScale(true);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.dlssMode")
    {
        m_dlssMode = static_cast<DlssMode>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 3));
        m_dlssBackendRuntime.UpdateMode(
            m_width, m_height, m_dlssMode);
        ApplyConfiguredRenderScale(true);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.dlssAvailable")
    {
        m_dlssEnabledWhenAvailable = Boolean(command.value);
        RequestGpuResourceRefresh(PendingGpuResourceRefresh::FullScene);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.preset")
    {
        ApplyNoisePreset(static_cast<NoisePreset>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 2)));
        m_projectDirty = true;
    }
    else if (property == L"denoise.jitterMode")
    {
        m_jitterMode = static_cast<JitterMode>(
            std::clamp<std::int64_t>(
                Integer(command.value), 0, 2));
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.splitSignal")
    {
        m_splitSignalDenoise = Boolean(command.value);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.realtime")
    {
        m_realtimeReconstruction = Boolean(command.value);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.temporalStability")
    {
        m_temporalStabilityEnabled = Boolean(command.value);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.cameraJitter")
    {
        m_cameraJitter = Boolean(command.value);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.movingJitterScale")
    {
        SetNumber(command.value, m_movingJitterScale, 0.0f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.historyFrames")
    {
        SetNumber(
            command.value,
            m_reconstructionMaxHistoryFrames,
            1,
            128);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.historyClamp")
    {
        SetNumber(
            command.value,
            m_historyClampSigma,
            0.5f,
            4.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.temporalAlphaMin")
    {
        SetNumber(command.value, m_temporalAlphaMin, 0.01f, 0.5f);
        m_temporalAlphaMin =
            (std::min)(m_temporalAlphaMin, m_temporalAlphaMax);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.temporalAlphaMax")
    {
        SetNumber(command.value, m_temporalAlphaMax, 0.02f, 0.8f);
        m_temporalAlphaMax =
            (std::max)(m_temporalAlphaMax, m_temporalAlphaMin);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.reactive")
    {
        SetNumber(
            command.value,
            m_reactiveThreshold,
            0.05f,
            1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.specularHistoryScale")
    {
        SetNumber(command.value, m_specularHistoryScale, 0.0f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.spatialIterations")
    {
        SetNumber(
            command.value,
            m_denoiserSpatialIterations,
            0,
            4);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.atrousPasses")
    {
        SetNumber(command.value, m_atrousPassCount, 0, 5);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.diffuseStrength")
    {
        SetNumber(command.value, m_atrousDiffuseStrength, 0.0f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.specularStrength")
    {
        SetNumber(command.value, m_atrousSpecularStrength, 0.0f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.varianceScale")
    {
        SetNumber(command.value, m_atrousVarianceScale, 0.25f, 4.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.normalSigma")
    {
        SetNumber(command.value, m_denoiserNormalSigma, 0.05f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.depthSigma")
    {
        SetNumber(command.value, m_denoiserDepthSigma, 0.002f, 0.10f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.luminanceSigma")
    {
        SetNumber(command.value, m_denoiserLuminanceSigma, 0.1f, 8.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.albedoSigma")
    {
        SetNumber(command.value, m_denoiserAlbedoSigma, 0.05f, 1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"denoise.strength")
    {
        SetNumber(
            command.value,
            m_denoiserStrength,
            0.0f,
            1.0f);
        ResetDenoiseHistory();
        m_projectDirty = true;
    }
    else if (property == L"restir.temporal")
    {
        m_restirTemporalReuse = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.spatialPasses")
    {
        SetNumber(
            command.value,
            m_restirSpatialReusePasses,
            0,
            4);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.radius")
    {
        SetNumber(command.value, m_restirSpatialRadius, 1, 64);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.candidates")
    {
        SetNumber(
            command.value,
            m_restirCandidateSamples,
            1,
            4);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.mClamp")
    {
        SetNumber(command.value, m_restirMClamp, 1.0f, 64.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.diTemporal")
    {
        m_restirDiTemporalReuse = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.diSpatialPasses")
    {
        SetNumber(command.value, m_restirDiSpatialReusePasses, 0, 4);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.diCandidates")
    {
        SetNumber(command.value, m_restirDiCandidateSamples, 1, 4);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.diMClamp")
    {
        SetNumber(command.value, m_restirDiMClamp, 1.0f, 64.0f);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.reprojection")
    {
        m_reservoirReprojection = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.validation")
    {
        m_reservoirValidation = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.giValidationRay")
    {
        m_restirGiValidationRay = Boolean(command.value);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"restir.maxAge")
    {
        SetNumber(command.value, m_reservoirMaxAge, 1, 32);
        invalidate(rb::FrameChangeMask::Backend);
    }
    else if (property == L"mcp.port")
    {
        if (!m_mcpServer.GetStatus().running)
        {
            {
                std::lock_guard lock(m_mcpSettingsMutex);
                m_mcpSettings.port = static_cast<uint16_t>(
                    std::clamp<std::int64_t>(
                        Integer(
                            command.value,
                            m_mcpSettings.port),
                        1,
                        65535));
            }
            SaveMcpUserSettings();
        }
    }
    else if (property == L"mcp.timeout")
    {
        {
            std::lock_guard lock(m_mcpSettingsMutex);
            m_mcpSettings.requestTimeoutSeconds = static_cast<int>(
                std::clamp<std::int64_t>(
                    Integer(
                        command.value,
                        m_mcpSettings.requestTimeoutSeconds),
                    5,
                    300));
        }
        SaveMcpUserSettings();
    }
    else if (property == L"mcp.access")
    {
        {
            std::lock_guard lock(m_mcpSettingsMutex);
            m_mcpSettings.accessMode = static_cast<mcp::AccessMode>(
                std::clamp<std::int64_t>(
                    Integer(command.value), 0, 2));
        }
        SaveMcpUserSettings();
    }
    else if (property == L"mcp.auth")
    {
        if (!m_mcpServer.GetStatus().running)
        {
            {
                std::lock_guard lock(m_mcpSettingsMutex);
                m_mcpSettings.authenticationMode =
                    static_cast<mcp::AuthenticationMode>(
                        std::clamp<std::int64_t>(
                            Integer(command.value), 0, 1));
            }
            SaveMcpUserSettings();
        }
    }
}

lookdevpt::winui::EditorSnapshot
D3D12PathTracingBackend::CaptureEditorSnapshot() const
{
    using namespace lookdevpt::winui;

    EditorSnapshot snapshot;
    snapshot.rendererReady = true;
    snapshot.projectDirty = m_projectDirty;
    snapshot.renderOnly = m_renderOnlyMode;
    snapshot.sceneName = m_scenePath.empty()
        ? L"Preview cube"
        : std::filesystem::path(m_scenePath).filename().wstring();
    snapshot.projectName = m_projectPath.empty()
        ? std::wstring{}
        : std::filesystem::path(m_projectPath).filename().wstring();
    const SceneLoadStage sceneLoadStage =
        m_sceneLoadStage.load(std::memory_order_relaxed);
    const bool sceneLoading =
        sceneLoadStage == SceneLoadStage::Parsing ||
        sceneLoadStage == SceneLoadStage::LoadingAssets ||
        sceneLoadStage == SceneLoadStage::BuildingBLAS ||
        sceneLoadStage == SceneLoadStage::BuildingTLAS;
    snapshot.status = sceneLoading
        ? L"Loading scene"
        : L"Rendering " + std::to_wstring(m_renderWidth) + L" × " +
            std::to_wstring(m_renderHeight) + L" → " +
            std::to_wstring(m_width) + L" × " +
            std::to_wstring(m_height);
    snapshot.diagnostics =
        Widen(m_sceneDiagnostics + "\n" +
              m_projectDiagnostics + "\n" +
              m_startupDiagnostics);

    std::wostringstream stats;
    NrdStatus const& nrdStatus = m_nrdBackendRuntime.Status();
    DlssStatus const& dlssStatus = m_dlssBackendRuntime.Status();
    RtxdiStatus const& rtxdiStatus = m_rtxdiBackendRuntime.Status();
    wchar_t const* requestedDenoise = L"Internal";
    switch (m_denoiseBackend)
    {
    case DenoiseBackend::NrdReblur:
        requestedDenoise = L"NRD REBLUR";
        break;
    case DenoiseBackend::NrdRelax:
        requestedDenoise = L"NRD RELAX";
        break;
    case DenoiseBackend::DlssRayReconstruction:
        requestedDenoise = L"DLSS Ray Reconstruction";
        break;
    case DenoiseBackend::Off:
        requestedDenoise = L"Off";
        break;
    default:
        break;
    }
    stats << L"Direct3D 12 DXR\n"
          << L"Adapter: " << m_adapterDescription << L"\n"
          << L"Resolution: " << m_renderWidth << L" × "
          << m_renderHeight << L" → " << m_width << L" × "
          << m_height << L" (" << m_activeRenderScale << L"×"
          << (UsesTemporalUpscale() ? L", TAAU" : L"") << L")\n"
          << L"Quality: "
          << Widen(rb::QualityProfileName(
              m_qualitySettings.qualityProfile)) << L"\n"
          << L"GPU PT pipeline: " << std::fixed
          << std::setprecision(3) << m_gpuFrameMs << L" ms\n"
          << L"Path trace: " << m_gpuPathTraceMs << L" ms\n"
          << L"ReSTIR: " << m_gpuRestirMs << L" ms\n"
          << L"ReSTIR GI initial / fused: "
          << m_gpuRestirGiInitialMs << L" / "
          << m_gpuRestirGiFusedMs << L" ms\n"
          << L"ReSTIR PT initial / fused: "
          << m_gpuRestirPtInitialMs << L" / "
          << m_gpuRestirPtFusedMs << L" ms\n"
          << L"Denoise: " << m_gpuDenoiseMs << L" ms\n"
          << L"Copy/final transition: " << m_gpuCopyMs
          << L" / " << m_gpuUiMs << L" ms\n"
          << L"Accumulated samples: " << m_accumulatedFrames << L"\n"
          << L"Meshes / materials / triangles: "
          << m_scene.draws.size() << L" / "
          << m_scene.materials.size() << L" / "
          << m_scene.indices.size() / 3 << L"\n"
          << L"BLAS geometries / TLAS instances: "
          << m_geometryRecords.size() << L" / "
          << (m_scene.instances.empty()
              ? size_t{ 1 }
              : m_scene.instances.size()) << L"\n"
          << L"Textures / lights: " << m_textures.size()
          << L" / " << m_activeLightCount << L"\n"
          << L"Denoise requested / active: "
          << requestedDenoise
          << L" / " << Widen(ActiveDenoiseBackendDisplayName())
          << L"\n"
          << L"NRD: "
          << (nrdStatus.compiled ? L"compiled" : L"not compiled")
          << L", "
          << (nrdStatus.instanceCreated ? L"ready" : L"fallback")
          << L"\n"
          << L"DLSS: "
          << (dlssStatus.compiled ? L"compiled" : L"not compiled")
          << L", "
          << (dlssStatus.runtimeAvailable ? L"runtime found" : L"runtime missing");
    snapshot.stats = stats.str();

    auto& values = snapshot.values;
    values[L"viewport.resolution"] =
        static_cast<std::int64_t>(m_displayResolutionPreset);
    values[L"viewport.renderOnly"] = m_renderOnlyMode;
    values[L"viewport.vsync"] = m_vsyncEnabled;
    values[L"renderer.mode"] =
        static_cast<std::int64_t>(m_mode);
    values[L"viewport.debugView"] =
        static_cast<std::int64_t>(m_debugViewMode);
    values[L"viewport.normalYFlip"] = m_debugNormalMapYFlip;
    values[L"viewport.toneMapper"] =
        static_cast<std::int64_t>(m_toneMapper);
    values[L"viewport.exposure"] = static_cast<double>(m_exposure);
    values[L"viewport.gamma"] = static_cast<double>(m_gamma);
    values[L"quality.profile"] = static_cast<std::int64_t>(
        m_qualitySettings.qualityProfile);
    values[L"quality.restirBackend"] = static_cast<std::int64_t>(
        m_qualitySettings.restirBackend);
    values[L"quality.secondaryRate"] = static_cast<std::int64_t>(
        m_qualitySettings.secondaryShadingRate);
    values[L"quality.resolutionMode"] = static_cast<std::int64_t>(
        m_qualitySettings.resolutionMode);
    values[L"quality.fixedRenderScale"] =
        static_cast<double>(m_qualitySettings.fixedRenderScale);
    values[L"quality.minRenderScale"] =
        static_cast<double>(m_qualitySettings.minRenderScale);
    values[L"quality.maxRenderScale"] =
        static_cast<double>(m_qualitySettings.maxRenderScale);
    values[L"quality.finalTaa"] = m_qualitySettings.finalTaa;
    values[L"quality.sharpen"] =
        static_cast<double>(m_qualitySettings.sharpenStrength);
    values[L"quality.movingSpp"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.movingSpp);
    values[L"quality.movingBounces"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.movingBounces);
    values[L"quality.staticBaseSpp"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.staticBaseSpp);
    values[L"quality.staticMaxSpp"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.staticMaxSpp);
    values[L"quality.staticBounces"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.staticBounces);
    values[L"quality.settleFrames"] = static_cast<std::int64_t>(
        m_qualitySettings.rayBudget.settleFrames);
    values[L"quality.targetGpuMs"] =
        static_cast<double>(m_qualitySettings.rayBudget.targetGpuMs);
    values[L"quality.referenceSpp"] = static_cast<std::int64_t>(
        m_qualitySettings.referenceSpp);
    values[L"quality.renderWidth"] =
        static_cast<std::int64_t>(m_renderWidth);
    values[L"quality.renderHeight"] =
        static_cast<std::int64_t>(m_renderHeight);
    values[L"quality.outputWidth"] = static_cast<std::int64_t>(m_width);
    values[L"quality.outputHeight"] = static_cast<std::int64_t>(m_height);
    values[L"quality.activeScale"] =
        static_cast<double>(m_activeRenderScale);
    values[L"quality.taau"] = UsesTemporalUpscale();
    values[L"camera.moveSpeed"] =
        static_cast<double>(m_baseMoveSpeed);
    values[L"camera.fastSpeed"] =
        static_cast<double>(m_fastMoveSpeed);
    XMFLOAT3 const cameraPosition = m_camera.GetPosition();
    values[L"camera.position.x"] =
        static_cast<double>(cameraPosition.x);
    values[L"camera.position.y"] =
        static_cast<double>(cameraPosition.y);
    values[L"camera.position.z"] =
        static_cast<double>(cameraPosition.z);
    values[L"camera.yawDegrees"] =
        static_cast<double>(XMConvertToDegrees(
            m_camera.GetYawRadians()));
    values[L"camera.pitchDegrees"] =
        static_cast<double>(XMConvertToDegrees(
            m_camera.GetPitchRadians()));
    values[L"camera.rollDegrees"] =
        static_cast<double>(XMConvertToDegrees(
            m_camera.GetRollRadians()));
    values[L"camera.fovDegrees"] =
        static_cast<double>(m_cameraFovDegrees);
    values[L"gamepad.enabled"] = m_camera.IsGamepadEnabled();
    values[L"gamepad.connected"] = m_camera.IsGamepadConnected();
    values[L"gamepad.calibrating"] =
        m_camera.IsGamepadCalibrating();
    values[L"gamepad.lookSpeed"] =
        static_cast<double>(m_camera.GetGamepadLookSpeed());
    values[L"gamepad.invertY"] = m_camera.GetGamepadInvertY();
    values[L"material.selected"] =
        static_cast<std::int64_t>(m_selectedMaterial);
    values[L"material.focus"] =
        static_cast<std::int64_t>(m_materialFocusMode);
    values[L"lighting.direction"] = std::array<double, 3>{
        m_lightDirection[0],
        m_lightDirection[1],
        m_lightDirection[2] };
    values[L"lighting.direction.x"] =
        static_cast<double>(m_lightDirection[0]);
    values[L"lighting.direction.y"] =
        static_cast<double>(m_lightDirection[1]);
    values[L"lighting.direction.z"] =
        static_cast<double>(m_lightDirection[2]);
    values[L"lighting.lightColor"] = std::array<double, 4>{
        m_lightColor[0], m_lightColor[1], m_lightColor[2], 1.0 };
    values[L"lighting.intensity"] =
        static_cast<double>(m_lightIntensity);
    values[L"lighting.rayTMin"] = static_cast<double>(m_rayTMin);
    values[L"lighting.skyEnabled"] = m_skyEnabled;
    values[L"lighting.skyColor"] = std::array<double, 4>{
        m_skyColor[0], m_skyColor[1], m_skyColor[2], 1.0 };
    values[L"lighting.skyHorizonColor"] = std::array<double, 4>{
        m_skyHorizonColor[0], m_skyHorizonColor[1],
        m_skyHorizonColor[2], 1.0 };
    values[L"lighting.skyZenithColor"] = std::array<double, 4>{
        m_skyZenithColor[0], m_skyZenithColor[1],
        m_skyZenithColor[2], 1.0 };
    values[L"lighting.skyGroundColor"] = std::array<double, 4>{
        m_skyGroundColor[0], m_skyGroundColor[1],
        m_skyGroundColor[2], 1.0 };
    values[L"lighting.skyIntensity"] =
        static_cast<double>(m_skyIntensity);
    values[L"lighting.sunIntensity"] =
        static_cast<double>(m_sunIntensity);
    values[L"lighting.sunSize"] =
        static_cast<double>(m_sunAngularRadius);
    values[L"lighting.environmentEnabled"] =
        m_environmentMapEnabled;
    values[L"lighting.environmentIntensity"] =
        static_cast<double>(m_environmentIntensity);
    values[L"lighting.environmentRotation"] =
        static_cast<double>(m_environmentRotation);
    values[L"lighting.sunNee"] = m_shadowEnabled;
    values[L"lighting.skyNee"] = m_skyNeeEnabled;
    values[L"lighting.emissiveEnabled"] =
        m_emissiveLightsEnabled;
    values[L"lighting.emissiveIntensity"] =
        static_cast<double>(m_emissiveLightIntensity);
    values[L"lighting.areaEnabled"] =
        m_proceduralLightsEnabled;
    values[L"lighting.areaIntensity"] =
        static_cast<double>(m_proceduralLightIntensity);
    values[L"path.samples"] =
        static_cast<std::int64_t>(m_giSamplesPerFrame);
    values[L"path.maxBounces"] =
        static_cast<std::int64_t>(m_maxPathBounces);
    values[L"path.minBounces"] =
        static_cast<std::int64_t>(m_minPathBounces);
    values[L"path.radianceClamp"] =
        static_cast<double>(m_giRadianceClamp);
    values[L"path.temporalClamp"] =
        static_cast<double>(m_giTemporalClampScale);
    values[L"path.maxAccumulation"] =
        static_cast<std::int64_t>(m_maxAccumulatedFrames);
    values[L"path.freeze"] = m_freezeAccumulation;
    values[L"path.adaptive"] = m_adaptiveSamplingEnabled;
    values[L"path.maxAdaptive"] =
        static_cast<std::int64_t>(m_maxAdaptiveSamplesPerPixel);
    values[L"path.varianceThreshold"] =
        static_cast<double>(m_adaptiveVarianceThreshold);
    values[L"path.disocclusionBoost"] =
        static_cast<double>(m_adaptiveDisocclusionBoost);
    values[L"denoise.enabled"] = m_denoiserEnabled;
    values[L"denoise.backend"] =
        static_cast<std::int64_t>(m_denoiseBackend);
    values[L"denoise.dlssMode"] =
        static_cast<std::int64_t>(m_dlssMode);
    values[L"denoise.dlssAvailable"] =
        m_dlssEnabledWhenAvailable;
    values[L"denoise.preset"] =
        static_cast<std::int64_t>(m_noisePreset);
    values[L"denoise.jitterMode"] =
        static_cast<std::int64_t>(m_jitterMode);
    values[L"denoise.splitSignal"] = m_splitSignalDenoise;
    values[L"denoise.realtime"] = m_realtimeReconstruction;
    values[L"denoise.temporalStability"] =
        m_temporalStabilityEnabled;
    values[L"denoise.cameraJitter"] = m_cameraJitter;
    values[L"denoise.movingJitterScale"] =
        static_cast<double>(m_movingJitterScale);
    values[L"denoise.historyFrames"] =
        static_cast<std::int64_t>(
            m_reconstructionMaxHistoryFrames);
    values[L"denoise.historyClamp"] =
        static_cast<double>(m_historyClampSigma);
    values[L"denoise.temporalAlphaMin"] =
        static_cast<double>(m_temporalAlphaMin);
    values[L"denoise.temporalAlphaMax"] =
        static_cast<double>(m_temporalAlphaMax);
    values[L"denoise.reactive"] =
        static_cast<double>(m_reactiveThreshold);
    values[L"denoise.specularHistoryScale"] =
        static_cast<double>(m_specularHistoryScale);
    values[L"denoise.spatialIterations"] =
        static_cast<std::int64_t>(m_denoiserSpatialIterations);
    values[L"denoise.atrousPasses"] =
        static_cast<std::int64_t>(m_atrousPassCount);
    values[L"denoise.diffuseStrength"] =
        static_cast<double>(m_atrousDiffuseStrength);
    values[L"denoise.specularStrength"] =
        static_cast<double>(m_atrousSpecularStrength);
    values[L"denoise.varianceScale"] =
        static_cast<double>(m_atrousVarianceScale);
    values[L"denoise.normalSigma"] =
        static_cast<double>(m_denoiserNormalSigma);
    values[L"denoise.depthSigma"] =
        static_cast<double>(m_denoiserDepthSigma);
    values[L"denoise.luminanceSigma"] =
        static_cast<double>(m_denoiserLuminanceSigma);
    values[L"denoise.albedoSigma"] =
        static_cast<double>(m_denoiserAlbedoSigma);
    values[L"denoise.strength"] =
        static_cast<double>(m_denoiserStrength);
    values[L"denoise.nrdCompiled"] = nrdStatus.compiled;
    values[L"denoise.nrdReady"] = nrdStatus.instanceCreated;
    values[L"denoise.dlssCompiled"] = dlssStatus.compiled;
    values[L"denoise.dlssRuntime"] = dlssStatus.runtimeAvailable;
    values[L"denoise.dlssIdentity"] =
        dlssStatus.applicationIdentityConfigured;
    values[L"denoise.dlssSupported"] = dlssStatus.featureSupported;
    values[L"denoise.dlssReady"] = dlssStatus.evaluationReady;
    values[L"denoise.dlssLastSucceeded"] =
        dlssStatus.lastEvaluationSucceeded;
    values[L"denoise.dlssSuccesses"] = static_cast<std::int64_t>(
        dlssStatus.successfulEvaluations);
    values[L"denoise.dlssFailures"] = static_cast<std::int64_t>(
        dlssStatus.failedEvaluations);
    values[L"denoise.dlssResultCode"] = static_cast<std::int64_t>(
        dlssStatus.lastResultCode);
    values[L"denoise.dlssFailureStage"] =
        Widen(dlssStatus.lastFailureStage);
    values[L"denoise.dlssFallbackReason"] =
        Widen(dlssStatus.fallbackReason);
    values[L"denoise.dlssError"] = Widen(dlssStatus.lastError);
    values[L"restir.temporal"] = m_restirTemporalReuse;
    values[L"restir.spatialPasses"] =
        static_cast<std::int64_t>(m_restirSpatialReusePasses);
    values[L"restir.radius"] =
        static_cast<std::int64_t>(m_restirSpatialRadius);
    values[L"restir.candidates"] =
        static_cast<std::int64_t>(m_restirCandidateSamples);
    values[L"restir.mClamp"] =
        static_cast<double>(m_restirMClamp);
    values[L"restir.diTemporal"] = m_restirDiTemporalReuse;
    values[L"restir.diSpatialPasses"] =
        static_cast<std::int64_t>(m_restirDiSpatialReusePasses);
    values[L"restir.diCandidates"] =
        static_cast<std::int64_t>(m_restirDiCandidateSamples);
    values[L"restir.diMClamp"] =
        static_cast<double>(m_restirDiMClamp);
    values[L"restir.reprojection"] = m_reservoirReprojection;
    values[L"restir.validation"] = m_reservoirValidation;
    values[L"restir.giValidationRay"] =
        m_restirGiValidationRay;
    values[L"restir.active"] =
        m_mode != PathTracingMode::Pathtracing;
    values[L"restir.rtxdiCompiled"] = rtxdiStatus.compiled;
    values[L"restir.rtxdiReady"] = rtxdiStatus.evaluationReady;
    values[L"restir.giReady"] = rtxdiStatus.giEvaluationReady;
    values[L"restir.ptReady"] = rtxdiStatus.ptEvaluationReady;
    values[L"restir.diReady"] = rtxdiStatus.diEvaluationReady;
    values[L"restir.fallbackReason"] =
        Widen(rtxdiStatus.fallbackReason);
    values[L"restir.maxAge"] =
        static_cast<std::int64_t>(m_reservoirMaxAge);
    values[L"scene.loading"] = sceneLoading;
    values[L"scene.loadStage"] =
        static_cast<std::int64_t>(sceneLoadStage);
    const std::uint64_t sceneLoadCompleted =
        m_sceneLoadCompleted.load(std::memory_order_relaxed);
    const std::uint64_t sceneLoadTotal =
        m_sceneLoadTotal.load(std::memory_order_relaxed);
    values[L"scene.loadCompleted"] =
        static_cast<std::int64_t>(sceneLoadCompleted);
    values[L"scene.loadTotal"] =
        static_cast<std::int64_t>(sceneLoadTotal);
    values[L"scene.loadFraction"] = sceneLoadTotal > 0
        ? static_cast<double>(sceneLoadCompleted) /
            static_cast<double>(sceneLoadTotal)
        : 0.0;
    {
        std::scoped_lock lock(m_sceneLoadProgressMutex);
        values[L"scene.loadCurrentAsset"] = m_sceneLoadCurrentAsset;
    }
    values[L"material.compare.hasA"] = m_hasMaterialCompareA;
    values[L"material.compare.hasB"] = m_hasMaterialCompareB;

    for (size_t index = 0;
         index < m_scene.materials.size();
         ++index)
    {
        MaterialItem item;
        item.name = m_scene.materials[index].name;
        if (index < m_materialUsage.size())
        {
            item.detail = std::to_wstring(
                m_materialUsage[index].meshCount) +
                L" meshes, " +
                std::to_wstring(
                    m_materialUsage[index].triangleCount) +
                L" triangles";
        }
        item.detail +=
            m_scene.materials[index].alphaMasked
                ? L" · alpha mask"
                : L" · opaque";
        if (m_scene.materials[index].packedOcclusionRoughnessMetallic)
        {
            item.detail += L" · packed ORM";
        }
        XMFLOAT4 const emissive =
            m_scene.materials[index].emissiveFactor;
        if (emissive.x > 0.0f || emissive.y > 0.0f ||
            emissive.z > 0.0f)
        {
            item.detail += L" · emissive";
        }
        snapshot.materials.push_back(std::move(item));
    }

    if (!m_scene.materials.empty())
    {
        const size_t selected = static_cast<size_t>(std::clamp(
            m_selectedMaterial,
            0,
            static_cast<int>(m_scene.materials.size()) - 1));
        Bistro::Material const& material =
            m_scene.materials[selected];
        values[L"material.baseColor"] = std::array<double, 4>{
            material.baseColorFactor.x,
            material.baseColorFactor.y,
            material.baseColorFactor.z,
            material.baseColorFactor.w };
        values[L"material.emissive"] = std::array<double, 4>{
            material.emissiveFactor.x,
            material.emissiveFactor.y,
            material.emissiveFactor.z,
            material.emissiveFactor.w };
        values[L"material.roughness"] =
            static_cast<double>(material.roughnessFactor);
        values[L"material.metallic"] =
            static_cast<double>(material.metallicFactor);
        values[L"material.occlusion"] =
            static_cast<double>(material.occlusionStrength);
        values[L"material.normalStrength"] =
            static_cast<double>(material.normalStrength);
        values[L"material.alphaCutoff"] =
            static_cast<double>(material.alphaCutoff);
        values[L"material.alphaMasked"] = material.alphaMasked;
        values[L"material.packedOrm"] =
            material.packedOcclusionRoughnessMetallic;

        for (UINT slot = 0; slot < TextureSlotCount; ++slot)
        {
            TextureSlotItem item;
            item.name = TextureNames[slot];
            item.currentPath = material.textures[slot];
            if (selected < m_sourceMaterials.size())
            {
                item.sourcePath =
                    m_sourceMaterials[selected].textures[slot];
            }
            const bool overridden =
                selected < m_textureOverrideEnabled.size() &&
                m_textureOverrideEnabled[selected][slot];
            const bool exists =
                selected < m_materialTextureExists.size() &&
                m_materialTextureExists[selected][slot];
            item.status = overridden ? L"Override" : L"Source";
            item.status += exists ? L" · loaded" : L" · fallback";
            item.resolutionPolicy = static_cast<std::int32_t>(material.textureBindings[slot].resolutionPolicy);
            if (selected < m_materialTextureIndices.size())
            {
                const UINT textureIndex =
                    m_materialTextureIndices[selected][slot];
                if (textureIndex < m_textures.size())
                {
                    GpuTexture const& texture =
                        m_textures[textureIndex];
                    item.status += L" · " +
                        std::to_wstring(texture.width) + L"×" +
                        std::to_wstring(texture.height) + L" · " +
                        std::to_wstring(texture.mipLevels) +
                        L" mips · format " +
                        std::to_wstring(
                            static_cast<int>(texture.format));
                }
            }
            snapshot.textureSlots.push_back(std::move(item));
        }
    }

    for (MaterialVariant const& variant : m_materialVariants)
    {
        snapshot.variants.push_back({
            Widen(variant.name),
            variant.materialName });
    }
    for (MaterialPreset const& preset : m_materialPresets)
    {
        snapshot.presets.push_back({
            Widen(preset.name),
            Widen(preset.category) });
    }

    mcp::ServerStatus const status = m_mcpServer.GetStatus();
    snapshot.mcpRunning = status.running;
    snapshot.mcpEndpoint = Widen(status.endpoint);
    snapshot.mcpLastError = Widen(status.lastError);
    values[L"mcp.sessions"] =
        static_cast<std::int64_t>(status.activeLegacySessions);
    values[L"mcp.subscriptions"] =
        static_cast<std::int64_t>(status.activeSubscriptions);
    values[L"mcp.activeRequests"] =
        static_cast<std::int64_t>(status.activeRequests);
    values[L"mcp.pairing.code"] = Widen(status.pairingCode);
    values[L"mcp.pairing.seconds"] = static_cast<std::int64_t>(status.pairingSecondsRemaining);
    values[L"mcp.pairing.clients"] = static_cast<std::int64_t>(status.pairedClients.size());
    for (size_t index = 0; index < status.pairedClients.size(); ++index)
    {
        const std::wstring prefix = L"mcp.pairing.client." + std::to_wstring(index);
        values[prefix + L".id"] = Widen(status.pairedClients[index].first);
        values[prefix + L".name"] = Widen(status.pairedClients[index].second);
    }
    values[L"mcp.pendingCommands"] =
        static_cast<std::int64_t>(
            m_mcpDispatcher.PendingCount());
    {
        std::lock_guard lock(m_mcpSnapshotMutex);
        values[L"mcp.audit.info"] = static_cast<std::int64_t>(m_mcpAuditInfoCount);
        values[L"mcp.audit.warning"] = static_cast<std::int64_t>(m_mcpAuditWarningCount);
        values[L"mcp.audit.error"] = static_cast<std::int64_t>(m_mcpAuditErrorCount);
        values[L"mcp.review.id"] = static_cast<std::int64_t>(m_activeMcpReviewId);
        values[L"mcp.review.progress"] = 0.0;
        values[L"mcp.review.state"] = std::wstring(L"idle");
        values[L"mcp.review.stage"] = std::wstring(L"No active review");
        for (const auto& review : m_mcpReviews)
        {
            if (review.id == m_activeMcpReviewId)
            {
                values[L"mcp.review.progress"] = review.progress;
                values[L"mcp.review.state"] = Widen(review.state);
                values[L"mcp.review.stage"] = Widen(review.stage);
                break;
            }
        }
    }
    for (std::string const& request : status.recentRequests)
    {
        snapshot.recentRequests.push_back(Widen(request));
    }
    {
        std::lock_guard lock(m_mcpSettingsMutex);
        snapshot.mcpToken = Widen(m_mcpSettings.token);
        values[L"mcp.port"] =
            static_cast<std::int64_t>(m_mcpSettings.port);
        values[L"mcp.timeout"] =
            static_cast<std::int64_t>(
                m_mcpSettings.requestTimeoutSeconds);
        values[L"mcp.access"] =
            static_cast<std::int64_t>(m_mcpSettings.accessMode);
        values[L"mcp.auth"] =
            static_cast<std::int64_t>(
                m_mcpSettings.authenticationMode);
    }
    for (mcp::PendingApproval const& approval :
         m_mcpDispatcher.PendingApprovals())
    {
        snapshot.approvals.push_back({
            approval.id,
            Widen(approval.toolName),
            Widen(approval.summary),
            approval.secondsRemaining });
    }

    return snapshot;
}
