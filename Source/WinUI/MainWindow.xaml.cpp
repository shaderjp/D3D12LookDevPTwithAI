#include "pch.h"
#include "MainWindow.xaml.h"

#include "SimpleJson.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Input;

namespace
{
std::filesystem::path LayoutPath()
{
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_RoamingAppData,
            KF_FLAG_CREATE,
            nullptr,
            &appData)))
    {
        return {};
    }
    std::filesystem::path result(appData);
    CoTaskMemFree(appData);
    return result / L"D3D12LookDevPTWinUI" / L"ui.json";
}

double Clamp(double value, double minimum, double maximum)
{
    return std::clamp(value, minimum, maximum);
}

bool Checked(ToggleMenuFlyoutItem const& item)
{
    return item && item.IsChecked();
}
}

namespace winrt::D3D12LookDevPTWinUI::implementation
{
MainWindow::MainWindow()
{
    InitializeComponent();
    LoadLayout();
    ApplyTheme(m_themeMode);

    m_viewModel =
        winrt::make_self<lookdevpt::winui::EditorViewModel>();
    MaterialCombo().ItemsSource(m_viewModel->Materials());
    VariantList().ItemsSource(m_viewModel->Variants());
    PresetList().ItemsSource(m_viewModel->Presets());
    ApprovalList().ItemsSource(m_viewModel->Approvals());
    RecentRequestsList().ItemsSource(m_viewModel->RecentRequests());

    m_controller =
        std::make_unique<lookdevpt::winui::RendererController>();
    m_controller->Start();

    m_timer = DispatcherQueue().CreateTimer();
    m_timer.Interval(std::chrono::milliseconds(100));
    m_timer.IsRepeating(true);
    auto weak = get_weak();
    m_timer.Tick(
        [weak](
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
            IInspectable const&)
        {
            if (auto self = weak.get())
            {
                self->RefreshSnapshot();
                self->AttachViewport();
            }
        });
    m_timer.Start();
}

void MainWindow::OnLoaded(
    IInspectable const&,
    RoutedEventArgs const&)
{
    m_windowHandle = WindowHandle();
    if (m_windowHandle)
    {
        SetWindowSubclass(
            m_windowHandle,
            WindowSubclassProc,
            1,
            reinterpret_cast<DWORD_PTR>(this));
    }
    UpdateTitleBarTheme();
    ApplyPanelVisibility();
    OnViewportHostSizeChanged(nullptr, nullptr);
    ViewportInputSurface().Focus(FocusState::Programmatic);
}

void MainWindow::OnClosed(
    IInspectable const&,
    WindowEventArgs const&)
{
    if (m_closing)
    {
        return;
    }
    m_closing = true;
    if (m_windowHandle)
    {
        RemoveWindowSubclass(
            m_windowHandle,
            WindowSubclassProc,
            1);
        m_windowHandle = nullptr;
    }
    SaveLayout();
    if (m_timer)
    {
        m_timer.Stop();
    }

    // Stop the renderer first so no more presents can race the UI-thread
    // SetSwapChain(nullptr) detach.
    if (m_controller)
    {
        m_controller->RequestRenderStopAndWait();
    }
    if (m_attachedPanel && m_controller)
    {
        std::wstring diagnostics;
        auto native =
            m_attachedPanel.try_as<ISwapChainPanelNative>();
        m_controller->DetachViewport(
            native.get(), diagnostics);
        m_attachedPanel = nullptr;
    }
    else if (m_controller)
    {
        std::wstring diagnostics;
        m_controller->DetachViewport(nullptr, diagnostics);
    }
    if (m_controller)
    {
        m_controller->StopAndJoin();
    }
    m_controller.reset();
}

void MainWindow::OnActualThemeChanged(
    FrameworkElement const&,
    IInspectable const&)
{
    UpdateTitleBarTheme();
}

void MainWindow::OnViewportLoaded(
    IInspectable const&,
    RoutedEventArgs const&)
{
    AttachViewport();
}

void MainWindow::AttachViewport()
{
    if (m_closing || m_attachedPanel || !m_controller ||
        !ViewportPanel())
    {
        return;
    }
    auto native =
        ViewportPanel().try_as<ISwapChainPanelNative>();
    std::wstring diagnostics;
    if (m_controller->AttachViewport(
            native.get(), diagnostics))
    {
        m_attachedPanel = ViewportPanel();
        LoadingRing().IsActive(false);
        LoadingOverlay().Visibility(Visibility::Collapsed);
    }
    else if (!diagnostics.empty())
    {
        LoadingText().Text(diagnostics);
    }
}

void MainWindow::OnViewportGotFocus(
    IInspectable const&,
    RoutedEventArgs const&)
{
    m_viewportFocused = true;
    if (!m_editing && m_controller)
    {
        m_controller->SetViewportFocused(true);
    }
}

void MainWindow::OnViewportLostFocus(
    IInspectable const&,
    RoutedEventArgs const&)
{
    m_viewportFocused = false;
    if (m_controller)
    {
        m_controller->SetViewportFocused(false);
    }
}

void MainWindow::OnViewportHostSizeChanged(
    IInspectable const&,
    SizeChangedEventArgs const&)
{
    if (!ViewportHost() || !ViewportPanel())
    {
        return;
    }
    const double availableWidth = ViewportHost().ActualWidth();
    const double availableHeight = ViewportHost().ActualHeight();
    if (availableWidth <= 0.0 || availableHeight <= 0.0)
    {
        return;
    }

    constexpr double aspect = 16.0 / 9.0;
    double width = availableWidth;
    double height = width / aspect;
    if (height > availableHeight)
    {
        height = availableHeight;
        width = height * aspect;
    }
    ViewportPanel().Width(width);
    ViewportPanel().Height(height);
}

void MainWindow::SendPointer(
    lookdevpt::winui::PointerEventType type,
    PointerRoutedEventArgs const& args)
{
    if (!m_controller)
    {
        return;
    }
    auto point = args.GetCurrentPoint(ViewportPanel());
    auto properties = point.Properties();
    lookdevpt::winui::EditorCommand command;
    command.type =
        lookdevpt::winui::EditorCommandType::Pointer;
    command.pointer.type = type;
    command.pointer.x = point.Position().X;
    command.pointer.y = point.Position().Y;
    command.pointer.wheelDelta =
        static_cast<float>(properties.MouseWheelDelta());
    command.pointer.leftButton =
        properties.IsLeftButtonPressed();
    command.pointer.middleButton =
        properties.IsMiddleButtonPressed();
    command.pointer.rightButton =
        properties.IsRightButtonPressed();
    Submit(std::move(command));
}

void MainWindow::OnViewportPointerPressed(
    IInspectable const&,
    PointerRoutedEventArgs const& args)
{
    ViewportInputSurface().Focus(FocusState::Pointer);
    auto point = args.GetCurrentPoint(ViewportPanel());
    if (point.Properties().IsRightButtonPressed())
    {
        ViewportInputSurface().CapturePointer(args.Pointer());
    }
    SendPointer(
        lookdevpt::winui::PointerEventType::Pressed,
        args);
    args.Handled(true);
}

void MainWindow::OnViewportPointerMoved(
    IInspectable const&,
    PointerRoutedEventArgs const& args)
{
    SendPointer(
        lookdevpt::winui::PointerEventType::Moved,
        args);
    args.Handled(true);
}

void MainWindow::OnViewportPointerReleased(
    IInspectable const&,
    PointerRoutedEventArgs const& args)
{
    SendPointer(
        lookdevpt::winui::PointerEventType::Released,
        args);
    ViewportInputSurface().ReleasePointerCapture(args.Pointer());
    args.Handled(true);
}

void MainWindow::OnViewportPointerExited(
    IInspectable const&,
    PointerRoutedEventArgs const& args)
{
    SendPointer(
        lookdevpt::winui::PointerEventType::Exited,
        args);
}

void MainWindow::OnViewportPointerWheelChanged(
    IInspectable const&,
    PointerRoutedEventArgs const& args)
{
    SendPointer(
        lookdevpt::winui::PointerEventType::Wheel,
        args);
    args.Handled(true);
}

void MainWindow::OnPreviewKeyDown(
    IInspectable const&,
    KeyRoutedEventArgs const& args)
{
    if (!m_viewportFocused || m_editing || !m_controller)
    {
        return;
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::Key,
        .key = {
            static_cast<std::uint32_t>(args.Key()),
            true },
    });
    args.Handled(true);
}

void MainWindow::OnPreviewKeyUp(
    IInspectable const&,
    KeyRoutedEventArgs const& args)
{
    if (!m_viewportFocused || m_editing || !m_controller)
    {
        return;
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::Key,
        .key = {
            static_cast<std::uint32_t>(args.Key()),
            false },
    });
    args.Handled(true);
}

void MainWindow::OnEditGotFocus(
    IInspectable const&,
    RoutedEventArgs const& args)
{
    IInspectable source = args.OriginalSource();
    if (!source.try_as<TextBox>() &&
        !source.try_as<NumberBox>())
    {
        return;
    }
    m_editing = true;
    if (m_controller)
    {
        m_controller->SetViewportFocused(false);
    }
}

void MainWindow::OnEditLostFocus(
    IInspectable const&,
    RoutedEventArgs const& args)
{
    IInspectable source = args.OriginalSource();
    if (!source.try_as<TextBox>() &&
        !source.try_as<NumberBox>())
    {
        return;
    }
    m_editing = false;
    if (m_controller)
    {
        m_controller->SetViewportFocused(m_viewportFocused);
    }
}

void MainWindow::OnNumberChanged(
    IInspectable const& sender,
    NumberBoxValueChangedEventArgs const& args)
{
    if (m_refreshing || std::isnan(args.NewValue()))
    {
        return;
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = Tag(sender),
        .value = args.NewValue(),
    });
}

void MainWindow::OnSliderChanged(
    IInspectable const& sender,
    RangeBaseValueChangedEventArgs const& args)
{
    if (m_refreshing)
    {
        return;
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = Tag(sender),
        .value = args.NewValue(),
    });
}

void MainWindow::OnToggleChanged(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    if (m_refreshing)
    {
        return;
    }
    bool value = false;
    if (auto switchControl = sender.try_as<ToggleSwitch>())
    {
        value = switchControl.IsOn();
    }
    else if (auto toggleButton = sender.try_as<ToggleButton>())
    {
        value = unbox_value_or<bool>(
            toggleButton.IsChecked(), false);
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = Tag(sender),
        .value = value,
    });
}

void MainWindow::OnComboChanged(
    IInspectable const& sender,
    SelectionChangedEventArgs const&)
{
    if (m_refreshing)
    {
        return;
    }
    auto combo = sender.try_as<ComboBox>();
    if (!combo || combo.SelectedIndex() < 0)
    {
        return;
    }
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = Tag(sender),
        .value =
            static_cast<std::int64_t>(combo.SelectedIndex()),
    });
}

void MainWindow::OnColorChanged(
    ColorPicker const& sender,
    ColorChangedEventArgs const& args)
{
    if (m_refreshing)
    {
        return;
    }
    Color color = args.NewColor();
    const double scale = 1.0 / 255.0;
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = Tag(sender),
        .value = std::array<double, 4>{
            color.R * scale,
            color.G * scale,
            color.B * scale,
            color.A * scale },
    });
}

void MainWindow::OnActionClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    lookdevpt::winui::EditorCommand command;
    command.type =
        lookdevpt::winui::EditorCommandType::Action;
    command.property = Tag(sender);
    if (command.property == L"material.variant.save")
    {
        command.value =
            std::wstring(VariantNameBox().Text().c_str());
    }
    else if (command.property == L"material.preset.save")
    {
        command.value =
            std::wstring(PresetNameBox().Text().c_str());
    }
    else if (command.property.starts_with(
                 L"material.variant."))
    {
        command.index = m_selectedVariant;
    }
    else if (command.property == L"material.preset.apply")
    {
        command.index = m_selectedPreset;
    }
    else if (command.property == L"mcp.approve" ||
             command.property == L"mcp.reject")
    {
        command.id = m_selectedApproval;
    }
    Submit(std::move(command));
}

void MainWindow::OnProjectMenuClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const std::wstring action = Tag(sender);
    if (action == L"saveProject")
    {
        Submit({
            .type = lookdevpt::winui::EditorCommandType::Action,
            .property = L"project.save",
        });
    }
    else if (action == L"saveStartup")
    {
        Submit({
            .type = lookdevpt::winui::EditorCommandType::Action,
            .property = L"startup.save",
        });
    }
    else if (action == L"clearStartup")
    {
        Submit({
            .type = lookdevpt::winui::EditorCommandType::Action,
            .property = L"startup.clear",
        });
    }
    else if (action == L"saveProjectAs")
    {
        SaveProjectAs();
    }
    else
    {
        PickFile(action);
    }
}

void MainWindow::OnPanelToggleClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    ApplyPanelVisibility();
}

void MainWindow::OnThemeClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const std::wstring theme = Tag(sender);
    if (theme == L"light")
    {
        ApplyTheme(EditorThemeMode::Light);
    }
    else if (theme == L"dark")
    {
        ApplyTheme(EditorThemeMode::Dark);
    }
    else
    {
        ApplyTheme(EditorThemeMode::System);
    }
    SaveLayout();
}

void MainWindow::OnShowAllPanels(
    IInspectable const&,
    RoutedEventArgs const&)
{
    for (ToggleMenuFlyoutItem const& item : {
             ScenePanelMenu(), MaterialPanelMenu(),
             LightingPanelMenu(), ViewportPanelMenu(),
             PathPanelMenu(), DenoisePanelMenu(),
             RestirPanelMenu(), DiagnosticsPanelMenu(),
             McpPanelMenu() })
    {
        item.IsChecked(true);
    }
    ApplyPanelVisibility();
}

void MainWindow::OnResetLayout(
    IInspectable const&,
    RoutedEventArgs const&)
{
    m_leftWidth = 330.0;
    m_rightWidth = 380.0;
    m_bottomHeight = 250.0;
    OnShowAllPanels(nullptr, nullptr);
    LeftTabs().SelectedItem(SceneTab());
    MaterialEditorTabs().SelectedIndex(0);
    RightTabs().SelectedItem(ViewportSettingsTab());
    BottomTabs().SelectedItem(DiagnosticsTab());
}

void MainWindow::OnRenderOnlyClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    ApplyRenderOnly(RenderOnlyMenu().IsChecked());
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = L"viewport.renderOnly",
        .value = m_renderOnly,
    });
}

void MainWindow::ToggleRenderOnly()
{
    ApplyRenderOnly(!m_renderOnly);
    Submit({
        .type = lookdevpt::winui::EditorCommandType::SetValue,
        .property = L"viewport.renderOnly",
        .value = m_renderOnly,
    });
}

void MainWindow::OnMaterialSelectionChanged(
    IInspectable const&,
    SelectionChangedEventArgs const&)
{
    if (!m_refreshing && MaterialCombo().SelectedIndex() >= 0)
    {
        const std::int32_t sourceIndex =
            m_viewModel->MaterialSourceIndex(
                MaterialCombo().SelectedIndex());
        if (sourceIndex < 0)
        {
            return;
        }
        Submit({
            .type =
                lookdevpt::winui::EditorCommandType::SetValue,
            .property = L"material.selected",
            .value = static_cast<std::int64_t>(sourceIndex),
        });
    }
}

void MainWindow::OnMaterialSearchChanged(
    IInspectable const&,
    TextChangedEventArgs const&)
{
    if (!m_viewModel)
    {
        return;
    }
    m_viewModel->SetMaterialFilter(
        std::wstring(MaterialSearchBox().Text().c_str()));
    auto snapshot = m_viewModel->Snapshot();
    std::int64_t selected = -1;
    if (snapshot &&
        TryInteger(*snapshot, L"material.selected", selected))
    {
        MaterialCombo().SelectedIndex(
            m_viewModel->MaterialDisplayIndex(
                static_cast<std::int32_t>(selected)));
    }
}

void MainWindow::OnVariantSelectionChanged(
    IInspectable const&,
    SelectionChangedEventArgs const&)
{
    m_selectedVariant = VariantList().SelectedIndex();
}

void MainWindow::OnPresetSelectionChanged(
    IInspectable const&,
    SelectionChangedEventArgs const&)
{
    m_selectedPreset = PresetList().SelectedIndex();
}

void MainWindow::OnApprovalSelectionChanged(
    IInspectable const&,
    SelectionChangedEventArgs const&)
{
    if (m_refreshing)
    {
        return;
    }
    auto snapshot = m_viewModel->Snapshot();
    const int index = ApprovalList().SelectedIndex();
    m_selectedApproval =
        snapshot && index >= 0 &&
                static_cast<size_t>(index) <
                    snapshot->approvals.size()
            ? snapshot->approvals[static_cast<size_t>(index)].id
            : 0;
}

void MainWindow::OnTextureClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const std::wstring tag = Tag(sender);
    const size_t delimiter = tag.find(L':');
    if (delimiter == std::wstring::npos)
    {
        return;
    }
    const std::wstring action = tag.substr(0, delimiter);
    const int slot = _wtoi(tag.c_str() + delimiter + 1);
    if (action == L"load")
    {
        PickTexture(slot);
    }
    else
    {
        Submit({
            .type = lookdevpt::winui::EditorCommandType::Action,
            .property =
                action == L"clear"
                    ? L"material.texture.clear"
                    : L"material.texture.reset",
            .index = slot,
        });
    }
}

void MainWindow::OnCopyToken(
    IInspectable const&,
    RoutedEventArgs const&)
{
    DataPackage package;
    package.SetText(McpTokenBox().Text());
    Clipboard::SetContent(package);
}

void MainWindow::OnLeftSplitterDrag(
    IInspectable const&,
    DragDeltaEventArgs const& args)
{
    m_leftWidth =
        Clamp(m_leftWidth + args.HorizontalChange(), 220.0, 600.0);
    LeftPaneColumn().Width(GridLengthHelper::FromPixels(m_leftWidth));
}

void MainWindow::OnRightSplitterDrag(
    IInspectable const&,
    DragDeltaEventArgs const& args)
{
    m_rightWidth =
        Clamp(m_rightWidth - args.HorizontalChange(), 260.0, 700.0);
    RightPaneColumn().Width(GridLengthHelper::FromPixels(m_rightWidth));
}

void MainWindow::OnBottomSplitterDrag(
    IInspectable const&,
    DragDeltaEventArgs const& args)
{
    m_bottomHeight =
        Clamp(m_bottomHeight - args.VerticalChange(), 120.0, 520.0);
    BottomPaneRow().Height(
        GridLengthHelper::FromPixels(m_bottomHeight));
}

void MainWindow::Submit(
    lookdevpt::winui::EditorCommand command)
{
    if (m_controller &&
        (!command.property.empty() ||
         command.type !=
             lookdevpt::winui::EditorCommandType::Action))
    {
        m_controller->Enqueue(std::move(command));
    }
}

void MainWindow::RefreshSnapshot()
{
    if (!m_controller || m_closing)
    {
        return;
    }
    auto snapshot = m_controller->LatestSnapshot();
    if (!snapshot || snapshot->revision == m_revision)
    {
        return;
    }
    m_revision = snapshot->revision;
    m_refreshing = true;
    m_viewModel->Apply(snapshot);

    // Approval labels include a live countdown, so their observable list is
    // periodically rebuilt. Keep the selection stable by command id and
    // select the first request when a new approval arrives.
    int approvalIndex = -1;
    for (size_t index = 0; index < snapshot->approvals.size(); ++index)
    {
        if (snapshot->approvals[index].id == m_selectedApproval)
        {
            approvalIndex = static_cast<int>(index);
            break;
        }
    }
    if (approvalIndex < 0 && !snapshot->approvals.empty())
    {
        approvalIndex = 0;
    }
    m_selectedApproval = approvalIndex >= 0
        ? snapshot->approvals[static_cast<size_t>(approvalIndex)].id
        : 0;
    ApprovalList().SelectedIndex(approvalIndex);

    SceneNameText().Text(snapshot->sceneName);
    SceneSummaryText().Text(
        snapshot->projectName.empty()
            ? L"No project file"
            : snapshot->projectName +
                (snapshot->projectDirty ? L" *" : L""));
    DiagnosticsText().Text(snapshot->diagnostics);
    StatsText().Text(snapshot->stats);
    RendererInfoBar().Message(snapshot->status);
    RendererInfoBar().Severity(
        snapshot->rendererStopped
            ? InfoBarSeverity::Error
            : InfoBarSeverity::Informational);
    McpStatusText().Text(
        snapshot->mcpRunning
            ? L"Listening for requests"
            : L"Server stopped");
    std::int64_t mcpSessions = 0;
    std::int64_t mcpActiveRequests = 0;
    std::int64_t mcpPendingCommands = 0;
    TryInteger(*snapshot, L"mcp.sessions", mcpSessions);
    TryInteger(
        *snapshot, L"mcp.activeRequests", mcpActiveRequests);
    TryInteger(
        *snapshot, L"mcp.pendingCommands", mcpPendingCommands);
    McpCountsText().Text(
        L"Sessions: " + std::to_wstring(mcpSessions) +
        L" · Active requests: " +
        std::to_wstring(mcpActiveRequests) +
        L" · Pending commands: " +
        std::to_wstring(mcpPendingCommands));
    McpIdleText().Text(
        snapshot->mcpRunning
            ? L"Waiting for requests\u2026"
            : L"No requests");
    McpIdleText().Visibility(
        snapshot->recentRequests.empty()
            ? Visibility::Visible
            : Visibility::Collapsed);
    McpEndpointText().Text(snapshot->mcpEndpoint);
    McpTokenBox().Text(snapshot->mcpToken);
    McpErrorText().Text(snapshot->mcpLastError);
    SaveProjectMenu().IsEnabled(!snapshot->projectName.empty());
    SaveStartupMenu().IsEnabled(
        !snapshot->projectName.empty() ||
        snapshot->sceneName != L"Preview cube");
    MaterialEditorTabs().IsEnabled(!snapshot->materials.empty());
    ApplyVariantButton().IsEnabled(m_selectedVariant >= 0);
    DeleteVariantButton().IsEnabled(m_selectedVariant >= 0);
    ApplyPresetButton().IsEnabled(m_selectedPreset >= 0);
    ApproveButton().IsEnabled(m_selectedApproval != 0);
    RejectButton().IsEnabled(m_selectedApproval != 0);
    McpStartButton().IsEnabled(!snapshot->mcpRunning);
    McpStopButton().IsEnabled(snapshot->mcpRunning);
    McpPortNumber().IsEnabled(!snapshot->mcpRunning);

    auto updateNumber =
        [&](NumberBox const& control, wchar_t const* property)
        {
            double value = 0.0;
            if (TryDouble(*snapshot, property, value))
            {
                control.Value(value);
            }
        };
    for (auto const& item : {
             std::pair{ MoveSpeedNumber(), L"camera.moveSpeed" },
             std::pair{ FastSpeedNumber(), L"camera.fastSpeed" },
             std::pair{ CameraXNumber(), L"camera.position.x" },
             std::pair{ CameraYNumber(), L"camera.position.y" },
             std::pair{ CameraZNumber(), L"camera.position.z" },
             std::pair{ CameraYawNumber(), L"camera.yawDegrees" },
             std::pair{ CameraPitchNumber(), L"camera.pitchDegrees" },
             std::pair{ GamepadLookSpeedNumber(), L"gamepad.lookSpeed" },
             std::pair{ RoughnessNumber(), L"material.roughness" },
             std::pair{ MetallicNumber(), L"material.metallic" },
             std::pair{ OcclusionNumber(), L"material.occlusion" },
             std::pair{ NormalStrengthNumber(), L"material.normalStrength" },
             std::pair{ AlphaCutoffNumber(), L"material.alphaCutoff" },
             std::pair{ LightIntensityNumber(), L"lighting.intensity" },
             std::pair{ LightDirectionXNumber(), L"lighting.direction.x" },
             std::pair{ LightDirectionYNumber(), L"lighting.direction.y" },
             std::pair{ LightDirectionZNumber(), L"lighting.direction.z" },
             std::pair{ RayTMinNumber(), L"lighting.rayTMin" },
             std::pair{ SkyIntensityNumber(), L"lighting.skyIntensity" },
             std::pair{ SunIntensityNumber(), L"lighting.sunIntensity" },
             std::pair{ SunSizeNumber(), L"lighting.sunSize" },
             std::pair{ EnvironmentIntensityNumber(), L"lighting.environmentIntensity" },
             std::pair{ EnvironmentRotationNumber(), L"lighting.environmentRotation" },
             std::pair{ EmissiveIntensityNumber(), L"lighting.emissiveIntensity" },
             std::pair{ AreaIntensityNumber(), L"lighting.areaIntensity" },
             std::pair{ ExposureNumber(), L"viewport.exposure" },
             std::pair{ GammaNumber(), L"viewport.gamma" },
             std::pair{ SamplesNumber(), L"path.samples" },
             std::pair{ MaxBouncesNumber(), L"path.maxBounces" },
             std::pair{ MinBouncesNumber(), L"path.minBounces" },
             std::pair{ RadianceClampNumber(), L"path.radianceClamp" },
             std::pair{ TemporalClampNumber(), L"path.temporalClamp" },
             std::pair{ MaxAccumulationNumber(), L"path.maxAccumulation" },
             std::pair{ MaxAdaptiveNumber(), L"path.maxAdaptive" },
             std::pair{ VarianceThresholdNumber(), L"path.varianceThreshold" },
             std::pair{ DisocclusionBoostNumber(), L"path.disocclusionBoost" },
             std::pair{ HistoryFramesNumber(), L"denoise.historyFrames" },
             std::pair{ MovingJitterScaleNumber(), L"denoise.movingJitterScale" },
             std::pair{ TemporalAlphaMinNumber(), L"denoise.temporalAlphaMin" },
             std::pair{ TemporalAlphaMaxNumber(), L"denoise.temporalAlphaMax" },
             std::pair{ HistoryClampNumber(), L"denoise.historyClamp" },
             std::pair{ ReactiveNumber(), L"denoise.reactive" },
             std::pair{ SpecularHistoryScaleNumber(), L"denoise.specularHistoryScale" },
             std::pair{ SpatialIterationsNumber(), L"denoise.spatialIterations" },
             std::pair{ AtrousPassesNumber(), L"denoise.atrousPasses" },
             std::pair{ DiffuseStrengthNumber(), L"denoise.diffuseStrength" },
             std::pair{ SpecularStrengthNumber(), L"denoise.specularStrength" },
             std::pair{ DenoiseVarianceScaleNumber(), L"denoise.varianceScale" },
             std::pair{ NormalSigmaNumber(), L"denoise.normalSigma" },
             std::pair{ DepthSigmaNumber(), L"denoise.depthSigma" },
             std::pair{ LuminanceSigmaNumber(), L"denoise.luminanceSigma" },
             std::pair{ AlbedoSigmaNumber(), L"denoise.albedoSigma" },
             std::pair{ DenoiseStrengthNumber(), L"denoise.strength" },
             std::pair{ RestirSpatialPassesNumber(), L"restir.spatialPasses" },
             std::pair{ RestirRadiusNumber(), L"restir.radius" },
             std::pair{ RestirCandidatesNumber(), L"restir.candidates" },
             std::pair{ RestirClampNumber(), L"restir.mClamp" },
             std::pair{ RestirDiSpatialPassesNumber(), L"restir.diSpatialPasses" },
             std::pair{ RestirDiCandidatesNumber(), L"restir.diCandidates" },
             std::pair{ RestirDiClampNumber(), L"restir.diMClamp" },
             std::pair{ RestirMaxAgeNumber(), L"restir.maxAge" },
             std::pair{ McpPortNumber(), L"mcp.port" },
             std::pair{ McpTimeoutNumber(), L"mcp.timeout" } })
    {
        updateNumber(item.first, item.second);
    }

    auto updateSlider =
        [&](Slider const& control, wchar_t const* property)
        {
            double value = 0.0;
            if (TryDouble(*snapshot, property, value))
            {
                control.Value(value);
            }
        };
    updateSlider(RoughnessSlider(), L"material.roughness");
    updateSlider(MetallicSlider(), L"material.metallic");
    updateSlider(LightIntensitySlider(), L"lighting.intensity");
    updateSlider(ExposureSlider(), L"viewport.exposure");

    auto updateCombo =
        [&](ComboBox const& control, wchar_t const* property)
        {
            std::int64_t value = 0;
            if (TryInteger(*snapshot, property, value))
            {
                control.SelectedIndex(static_cast<int>(value));
            }
        };
    for (auto const& item : {
             std::pair{ MaterialFocusCombo(), L"material.focus" },
             std::pair{ RenderModeCombo(), L"renderer.mode" },
             std::pair{ ResolutionCombo(), L"viewport.resolution" },
             std::pair{ DebugViewCombo(), L"viewport.debugView" },
             std::pair{ ToneMapperCombo(), L"viewport.toneMapper" },
             std::pair{ DenoiseBackendCombo(), L"denoise.backend" },
             std::pair{ DlssModeCombo(), L"denoise.dlssMode" },
             std::pair{ NoisePresetCombo(), L"denoise.preset" },
             std::pair{ JitterModeCombo(), L"denoise.jitterMode" },
             std::pair{ McpAccessCombo(), L"mcp.access" } })
    {
        updateCombo(item.first, item.second);
    }

    auto updateSwitch =
        [&](ToggleSwitch const& control, wchar_t const* property)
        {
            bool value = false;
            if (TryBool(*snapshot, property, value))
            {
                control.IsOn(value);
            }
        };
    for (auto const& item : {
             std::pair{ GamepadEnabledToggle(), L"gamepad.enabled" },
             std::pair{ SkyEnabledToggle(), L"lighting.skyEnabled" },
             std::pair{ EnvironmentEnabledToggle(), L"lighting.environmentEnabled" },
             std::pair{ SunNeeToggle(), L"lighting.sunNee" },
             std::pair{ SkyNeeToggle(), L"lighting.skyNee" },
             std::pair{ EmissiveEnabledToggle(), L"lighting.emissiveEnabled" },
             std::pair{ AreaEnabledToggle(), L"lighting.areaEnabled" },
             std::pair{ VsyncToggle(), L"viewport.vsync" },
             std::pair{ FreezeToggle(), L"path.freeze" },
             std::pair{ AdaptiveToggle(), L"path.adaptive" },
             std::pair{ DenoiseEnabledToggle(), L"denoise.enabled" },
             std::pair{ DlssAvailableToggle(), L"denoise.dlssAvailable" },
             std::pair{ SplitSignalToggle(), L"denoise.splitSignal" },
             std::pair{ RealtimeToggle(), L"denoise.realtime" },
             std::pair{ TemporalStabilityToggle(), L"denoise.temporalStability" },
             std::pair{ CameraJitterToggle(), L"denoise.cameraJitter" },
             std::pair{ RestirTemporalToggle(), L"restir.temporal" },
             std::pair{ RestirDiTemporalToggle(), L"restir.diTemporal" },
             std::pair{ RestirReprojectionToggle(), L"restir.reprojection" },
             std::pair{ RestirValidationToggle(), L"restir.validation" },
             std::pair{ RestirGiValidationToggle(), L"restir.giValidationRay" } })
    {
        updateSwitch(item.first, item.second);
    }

    auto updateCheck =
        [&](CheckBox const& control, wchar_t const* property)
        {
            bool value = false;
            if (TryBool(*snapshot, property, value))
            {
                control.IsChecked(value);
            }
        };
    updateCheck(GamepadInvertToggle(), L"gamepad.invertY");
    updateCheck(AlphaMaskedToggle(), L"material.alphaMasked");
    updateCheck(PackedOrmToggle(), L"material.packedOrm");
    updateCheck(NormalYFlipToggle(), L"viewport.normalYFlip");

    std::int64_t selectedMaterial = 0;
    if (TryInteger(
            *snapshot, L"material.selected", selectedMaterial))
    {
        MaterialCombo().SelectedIndex(
            m_viewModel->MaterialDisplayIndex(
                static_cast<std::int32_t>(selectedMaterial)));
        MaterialDetailsText().Text(
            selectedMaterial >= 0 &&
            static_cast<size_t>(selectedMaterial) <
                snapshot->materials.size()
                ? snapshot->materials[
                    static_cast<size_t>(
                        selectedMaterial)].detail
                : L"");
    }

    auto updateColor =
        [&](ColorPicker const& picker, wchar_t const* property)
        {
            auto found = snapshot->values.find(property);
            if (found == snapshot->values.end())
            {
                return;
            }
            auto color =
                std::get_if<std::array<double, 4>>(
                    &found->second);
            if (!color)
            {
                return;
            }
            picker.Color(ColorHelper::FromArgb(
                static_cast<uint8_t>(
                    Clamp((*color)[3], 0, 1) * 255),
                static_cast<uint8_t>(
                    Clamp((*color)[0], 0, 1) * 255),
                static_cast<uint8_t>(
                    Clamp((*color)[1], 0, 1) * 255),
                static_cast<uint8_t>(
                    Clamp((*color)[2], 0, 1) * 255)));
        };
    updateColor(BaseColorPicker(), L"material.baseColor");
    updateColor(EmissiveColorPicker(), L"material.emissive");
    updateColor(LightColorPicker(), L"lighting.lightColor");
    updateColor(SkyColorPicker(), L"lighting.skyColor");
    updateColor(SkyHorizonColorPicker(), L"lighting.skyHorizonColor");
    updateColor(SkyZenithColorPicker(), L"lighting.skyZenithColor");
    updateColor(SkyGroundColorPicker(), L"lighting.skyGroundColor");

    std::array<TextBlock, 6> textureText = {
        Texture0Text(), Texture1Text(), Texture2Text(),
        Texture3Text(), Texture4Text(), Texture5Text() };
    for (size_t index = 0; index < textureText.size(); ++index)
    {
        if (index < snapshot->textureSlots.size())
        {
            auto const& slot = snapshot->textureSlots[index];
            textureText[index].Text(
                slot.name + L"\n" + slot.status + L"\nCurrent: " +
                (slot.currentPath.empty()
                    ? L"<fallback>"
                    : slot.currentPath) +
                L"\nSource: " +
                (slot.sourcePath.empty()
                    ? L"<fallback>"
                    : slot.sourcePath));
        }
    }

    bool connected = false;
    bool calibrating = false;
    TryBool(*snapshot, L"gamepad.connected", connected);
    TryBool(*snapshot, L"gamepad.calibrating", calibrating);
    GamepadStatusText().Text(
        calibrating
            ? L"Controller calibrating; release both sticks…"
            : connected
            ? L"Controller connected"
            : L"Waiting for controller…");

    bool nrdCompiled = false;
    bool nrdReady = false;
    bool dlssCompiled = false;
    bool dlssRuntime = false;
    TryBool(*snapshot, L"denoise.nrdCompiled", nrdCompiled);
    TryBool(*snapshot, L"denoise.nrdReady", nrdReady);
    TryBool(*snapshot, L"denoise.dlssCompiled", dlssCompiled);
    TryBool(*snapshot, L"denoise.dlssRuntime", dlssRuntime);
    DenoiseStatusText().Text(
        L"NRD: " +
        std::wstring(nrdCompiled ? L"compiled" : L"not compiled") +
        L", " + (nrdReady ? L"ready" : L"fallback") +
        L"\nDLSS: " +
        std::wstring(dlssCompiled ? L"compiled" : L"not compiled") +
        L", " + (dlssRuntime ? L"runtime found" : L"runtime missing"));

    bool restirActive = false;
    TryBool(*snapshot, L"restir.active", restirActive);
    RestirStatusText().Text(
        restirActive
            ? L"ReSTIR controls are active for the selected rendering mode."
            : L"Inactive for Baseline PT.");
    RestirTemporalToggle().IsEnabled(restirActive);
    RestirSpatialPassesNumber().IsEnabled(restirActive);
    RestirRadiusNumber().IsEnabled(restirActive);
    RestirCandidatesNumber().IsEnabled(restirActive);
    RestirClampNumber().IsEnabled(restirActive);
    RestirDiTemporalToggle().IsEnabled(restirActive);
    RestirDiSpatialPassesNumber().IsEnabled(restirActive);
    RestirDiCandidatesNumber().IsEnabled(restirActive);
    RestirDiClampNumber().IsEnabled(restirActive);
    RestirReprojectionToggle().IsEnabled(restirActive);
    RestirValidationToggle().IsEnabled(restirActive);
    RestirGiValidationToggle().IsEnabled(restirActive);
    RestirMaxAgeNumber().IsEnabled(restirActive);

    if (snapshot->renderOnly != m_renderOnly)
    {
        ApplyRenderOnly(snapshot->renderOnly);
    }
    m_refreshing = false;
    if (snapshot->benchmarkFinished)
    {
        Close();
    }
}

void MainWindow::ApplyRenderOnly(bool enabled)
{
    m_renderOnly = enabled;
    RenderOnlyMenu().IsChecked(enabled);
    MainMenu().Visibility(
        enabled ? Visibility::Collapsed : Visibility::Visible);
    RendererInfoBar().Visibility(
        enabled ? Visibility::Collapsed : Visibility::Visible);
    if (enabled)
    {
        LeftPane().Visibility(Visibility::Collapsed);
        LeftSplitter().Visibility(Visibility::Collapsed);
        RightSplitter().Visibility(Visibility::Collapsed);
        RightPane().Visibility(Visibility::Collapsed);
        BottomSplitter().Visibility(Visibility::Collapsed);
        BottomPane().Visibility(Visibility::Collapsed);
        LeftPaneColumn().MinWidth(0);
        RightPaneColumn().MinWidth(0);
        BottomPaneRow().MinHeight(0);
        LeftPaneColumn().Width(GridLengthHelper::FromPixels(0));
        LeftSplitterColumn().Width(GridLengthHelper::FromPixels(0));
        RightSplitterColumn().Width(GridLengthHelper::FromPixels(0));
        RightPaneColumn().Width(GridLengthHelper::FromPixels(0));
        BottomSplitterRow().Height(
            GridLengthHelper::FromPixels(0));
        BottomPaneRow().Height(GridLengthHelper::FromPixels(0));
    }
    else
    {
        ApplyPanelVisibility();
    }
}

void MainWindow::ApplyPanelVisibility()
{
    m_showLeft =
        Checked(ScenePanelMenu()) ||
        Checked(MaterialPanelMenu()) ||
        Checked(LightingPanelMenu());
    m_showRight =
        Checked(ViewportPanelMenu()) ||
        Checked(PathPanelMenu()) ||
        Checked(DenoisePanelMenu()) ||
        Checked(RestirPanelMenu());
    m_showBottom =
        Checked(DiagnosticsPanelMenu()) ||
        Checked(McpPanelMenu());

    SceneTab().Visibility(
        Checked(ScenePanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    MaterialTab().Visibility(
        Checked(MaterialPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    LightingTab().Visibility(
        Checked(LightingPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    ViewportSettingsTab().Visibility(
        Checked(ViewportPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    PathTracingTab().Visibility(
        Checked(PathPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    DenoiseTab().Visibility(
        Checked(DenoisePanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    RestirTab().Visibility(
        Checked(RestirPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    DiagnosticsTab().Visibility(
        Checked(DiagnosticsPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);
    McpTab().Visibility(
        Checked(McpPanelMenu())
            ? Visibility::Visible
            : Visibility::Collapsed);

    LeftPane().Visibility(
        m_showLeft ? Visibility::Visible : Visibility::Collapsed);
    LeftSplitter().Visibility(
        m_showLeft ? Visibility::Visible : Visibility::Collapsed);
    RightSplitter().Visibility(
        m_showRight ? Visibility::Visible : Visibility::Collapsed);
    RightPane().Visibility(
        m_showRight ? Visibility::Visible : Visibility::Collapsed);
    BottomSplitter().Visibility(
        m_showBottom ? Visibility::Visible : Visibility::Collapsed);
    BottomPane().Visibility(
        m_showBottom ? Visibility::Visible : Visibility::Collapsed);
    LeftPaneColumn().MinWidth(m_showLeft ? 220.0 : 0.0);
    RightPaneColumn().MinWidth(m_showRight ? 260.0 : 0.0);
    BottomPaneRow().MinHeight(m_showBottom ? 120.0 : 0.0);
    LeftPaneColumn().Width(GridLengthHelper::FromPixels(
        m_showLeft ? m_leftWidth : 0));
    LeftSplitterColumn().Width(GridLengthHelper::FromPixels(
        m_showLeft ? 6 : 0));
    RightSplitterColumn().Width(GridLengthHelper::FromPixels(
        m_showRight ? 6 : 0));
    RightPaneColumn().Width(GridLengthHelper::FromPixels(
        m_showRight ? m_rightWidth : 0));
    BottomSplitterRow().Height(GridLengthHelper::FromPixels(
        m_showBottom ? 6 : 0));
    BottomPaneRow().Height(GridLengthHelper::FromPixels(
        m_showBottom ? m_bottomHeight : 0));
}

void MainWindow::ApplyTheme(EditorThemeMode mode)
{
    m_themeMode = mode;
    SystemThemeMenu().IsChecked(mode == EditorThemeMode::System);
    LightThemeMenu().IsChecked(mode == EditorThemeMode::Light);
    DarkThemeMenu().IsChecked(mode == EditorThemeMode::Dark);

    ElementTheme requestedTheme = ElementTheme::Default;
    if (mode == EditorThemeMode::Light)
    {
        requestedTheme = ElementTheme::Light;
    }
    else if (mode == EditorThemeMode::Dark)
    {
        requestedTheme = ElementTheme::Dark;
    }
    EditorRoot().RequestedTheme(requestedTheme);
    UpdateTitleBarTheme();
}

void MainWindow::UpdateTitleBarTheme()
{
    if (!m_windowHandle)
    {
        return;
    }
    const BOOL useDarkMode =
        EditorRoot().ActualTheme() == ElementTheme::Dark;
    DwmSetWindowAttribute(
        m_windowHandle,
        DWMWA_USE_IMMERSIVE_DARK_MODE,
        &useDarkMode,
        sizeof(useDarkMode));

    const COLORREF captionColor = useDarkMode
        ? RGB(32, 32, 32)
        : RGB(243, 243, 243);
    const COLORREF textColor = useDarkMode
        ? RGB(255, 255, 255)
        : RGB(0, 0, 0);
    const COLORREF borderColor = useDarkMode
        ? RGB(48, 48, 48)
        : RGB(229, 229, 229);
    DwmSetWindowAttribute(
        m_windowHandle,
        DWMWA_CAPTION_COLOR,
        &captionColor,
        sizeof(captionColor));
    DwmSetWindowAttribute(
        m_windowHandle,
        DWMWA_TEXT_COLOR,
        &textColor,
        sizeof(textColor));
    DwmSetWindowAttribute(
        m_windowHandle,
        DWMWA_BORDER_COLOR,
        &borderColor,
        sizeof(borderColor));
    RedrawWindow(
        m_windowHandle,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_FRAME);
}

void MainWindow::LoadLayout()
{
    const std::filesystem::path path = LayoutPath();
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return;
    }
    try
    {
        std::ostringstream text;
        text << file.rdbuf();
        cld::JsonValue root =
            cld::JsonParser(text.str()).Parse();
        const std::string theme =
            cld::JsonStringOr(root, "theme", "system");
        if (theme == "light")
        {
            m_themeMode = EditorThemeMode::Light;
        }
        else if (theme == "dark")
        {
            m_themeMode = EditorThemeMode::Dark;
        }
        else
        {
            m_themeMode = EditorThemeMode::System;
        }
        m_leftWidth = Clamp(
            cld::JsonNumberOr(root, "leftWidth", m_leftWidth),
            220.0, 600.0);
        m_rightWidth = Clamp(
            cld::JsonNumberOr(root, "rightWidth", m_rightWidth),
            260.0, 700.0);
        m_bottomHeight = Clamp(
            cld::JsonNumberOr(
                root, "bottomHeight", m_bottomHeight),
            120.0, 520.0);
        ScenePanelMenu().IsChecked(
            cld::JsonBoolOr(root, "scene", true));
        MaterialPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "material", true));
        LightingPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "lighting", true));
        ViewportPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "viewport", true));
        PathPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "pathTracing", true));
        DenoisePanelMenu().IsChecked(
            cld::JsonBoolOr(root, "denoise", true));
        RestirPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "restir", true));
        DiagnosticsPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "diagnostics", true));
        McpPanelMenu().IsChecked(
            cld::JsonBoolOr(root, "mcp", true));
        LeftTabs().SelectedIndex(static_cast<int>(std::clamp(
            cld::JsonNumberOr(root, "leftSelectedTab", 0.0),
            0.0, 2.0)));
        MaterialEditorTabs().SelectedIndex(static_cast<int>(std::clamp(
            cld::JsonNumberOr(root, "materialSelectedTab", 0.0),
            0.0, 3.0)));
        RightTabs().SelectedIndex(static_cast<int>(std::clamp(
            cld::JsonNumberOr(root, "rightSelectedTab", 0.0),
            0.0, 3.0)));
        BottomTabs().SelectedIndex(static_cast<int>(std::clamp(
            cld::JsonNumberOr(root, "bottomSelectedTab", 0.0),
            0.0, 1.0)));
    }
    catch (...)
    {
        // Invalid layout data falls back to the documented defaults.
    }
}

void MainWindow::SaveLayout()
{
    const std::filesystem::path path = LayoutPath();
    if (path.empty())
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(
        path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        return;
    }
    const char* theme = "system";
    if (m_themeMode == EditorThemeMode::Light)
    {
        theme = "light";
    }
    else if (m_themeMode == EditorThemeMode::Dark)
    {
        theme = "dark";
    }
    file << "{\n"
         << "  \"theme\": \"" << theme << "\",\n"
         << "  \"leftWidth\": " << m_leftWidth << ",\n"
         << "  \"rightWidth\": " << m_rightWidth << ",\n"
         << "  \"bottomHeight\": " << m_bottomHeight << ",\n"
         << "  \"leftSelectedTab\": " << LeftTabs().SelectedIndex() << ",\n"
         << "  \"materialSelectedTab\": " << MaterialEditorTabs().SelectedIndex() << ",\n"
         << "  \"rightSelectedTab\": " << RightTabs().SelectedIndex() << ",\n"
         << "  \"bottomSelectedTab\": " << BottomTabs().SelectedIndex() << ",\n"
         << "  \"scene\": " << (Checked(ScenePanelMenu()) ? "true" : "false") << ",\n"
         << "  \"material\": " << (Checked(MaterialPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"lighting\": " << (Checked(LightingPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"viewport\": " << (Checked(ViewportPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"pathTracing\": " << (Checked(PathPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"denoise\": " << (Checked(DenoisePanelMenu()) ? "true" : "false") << ",\n"
         << "  \"restir\": " << (Checked(RestirPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"diagnostics\": " << (Checked(DiagnosticsPanelMenu()) ? "true" : "false") << ",\n"
         << "  \"mcp\": " << (Checked(McpPanelMenu()) ? "true" : "false") << "\n"
         << "}\n";
}

IAsyncAction MainWindow::PickFile(std::wstring action)
{
    auto lifetime = get_strong();
    FileOpenPicker picker;
    check_hresult(
        picker.as<::IInitializeWithWindow>()->Initialize(
            WindowHandle()));
    picker.ViewMode(PickerViewMode::List);
    picker.SuggestedStartLocation(
        PickerLocationId::DocumentsLibrary);

    if (action == L"openScene")
    {
        for (hstring const extension :
             { L".gltf", L".glb", L".fbx", L".obj" })
        {
            picker.FileTypeFilter().Append(extension);
        }
    }
    else if (action == L"openEnvironment")
    {
        for (hstring const extension :
             { L".hdr", L".dds", L".png", L".jpg",
               L".jpeg", L".tga" })
        {
            picker.FileTypeFilter().Append(extension);
        }
    }
    else
    {
        picker.FileTypeFilter().Append(L".json");
    }

    StorageFile file = co_await picker.PickSingleFileAsync();
    if (!file || m_closing)
    {
        co_return;
    }
    lookdevpt::winui::EditorCommand command;
    command.path = file.Path().c_str();
    command.type =
        action == L"openScene"
            ? lookdevpt::winui::EditorCommandType::LoadScene
            : action == L"openEnvironment"
                ? lookdevpt::winui::EditorCommandType::LoadEnvironment
                : lookdevpt::winui::EditorCommandType::LoadProject;
    Submit(std::move(command));
}

IAsyncAction MainWindow::PickTexture(std::int32_t slot)
{
    auto lifetime = get_strong();
    FileOpenPicker picker;
    check_hresult(
        picker.as<::IInitializeWithWindow>()->Initialize(
            WindowHandle()));
    for (hstring const extension :
         { L".png", L".jpg", L".jpeg", L".tga",
           L".dds", L".hdr", L".bmp" })
    {
        picker.FileTypeFilter().Append(extension);
    }
    StorageFile file = co_await picker.PickSingleFileAsync();
    if (file && !m_closing)
    {
        Submit({
            .type =
                lookdevpt::winui::EditorCommandType::LoadMaterialTexture,
            .path = file.Path().c_str(),
            .index = slot,
        });
    }
}

IAsyncAction MainWindow::SaveProjectAs()
{
    auto lifetime = get_strong();
    FileSavePicker picker;
    check_hresult(
        picker.as<::IInitializeWithWindow>()->Initialize(
            WindowHandle()));
    picker.SuggestedStartLocation(
        PickerLocationId::DocumentsLibrary);
    picker.FileTypeChoices().Insert(
        L"LookDevPT project",
        single_threaded_vector<hstring>({ L".json" }));
    picker.DefaultFileExtension(L".json");
    picker.SuggestedFileName(L"scene.lookdevpt");
    StorageFile file = co_await picker.PickSaveFileAsync();
    if (file && !m_closing)
    {
        Submit({
            .type =
                lookdevpt::winui::EditorCommandType::SaveProjectAs,
            .path = file.Path().c_str(),
        });
    }
}

HWND MainWindow::WindowHandle()
{
    HWND handle = nullptr;
    auto lifetime = get_strong();
    auto native = lifetime.as<::IWindowNative>();
    check_hresult(native->get_WindowHandle(&handle));
    return handle;
}

LRESULT CALLBACK MainWindow::WindowSubclassProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR referenceData)
{
    auto self = reinterpret_cast<MainWindow*>(referenceData);
    if (self && wparam == VK_F10 &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN))
    {
        constexpr LPARAM PreviousKeyState = 1LL << 30;
        if ((lparam & PreviousKeyState) == 0)
        {
            self->ToggleRenderOnly();
        }
        return 0;
    }
    if (wparam == VK_F10 &&
        (message == WM_KEYUP || message == WM_SYSKEYUP))
    {
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

std::wstring MainWindow::Tag(IInspectable const& sender)
{
    IInspectable value{ nullptr };
    if (auto element = sender.try_as<FrameworkElement>())
    {
        value = element.Tag();
    }
    else if (auto menuItem = sender.try_as<MenuFlyoutItem>())
    {
        value = menuItem.Tag();
    }
    else if (auto toggleMenuItem =
                 sender.try_as<ToggleMenuFlyoutItem>())
    {
        value = toggleMenuItem.Tag();
    }
    return unbox_value_or<hstring>(value, L"").c_str();
}

bool MainWindow::TryDouble(
    lookdevpt::winui::EditorSnapshot const& snapshot,
    std::wstring const& property,
    double& value)
{
    auto found = snapshot.values.find(property);
    if (found == snapshot.values.end())
    {
        return false;
    }
    if (auto number = std::get_if<double>(&found->second))
    {
        value = *number;
        return true;
    }
    if (auto integer =
            std::get_if<std::int64_t>(&found->second))
    {
        value = static_cast<double>(*integer);
        return true;
    }
    return false;
}

bool MainWindow::TryInteger(
    lookdevpt::winui::EditorSnapshot const& snapshot,
    std::wstring const& property,
    std::int64_t& value)
{
    auto found = snapshot.values.find(property);
    if (found == snapshot.values.end())
    {
        return false;
    }
    if (auto integer =
            std::get_if<std::int64_t>(&found->second))
    {
        value = *integer;
        return true;
    }
    if (auto number = std::get_if<double>(&found->second))
    {
        value = static_cast<std::int64_t>(*number);
        return true;
    }
    return false;
}

bool MainWindow::TryBool(
    lookdevpt::winui::EditorSnapshot const& snapshot,
    std::wstring const& property,
    bool& value)
{
    auto found = snapshot.values.find(property);
    if (found == snapshot.values.end())
    {
        return false;
    }
    if (auto boolean = std::get_if<bool>(&found->second))
    {
        value = *boolean;
        return true;
    }
    return false;
}
}
