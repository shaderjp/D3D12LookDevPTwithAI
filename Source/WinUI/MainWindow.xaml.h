#pragma once

#include "MainWindow.g.h"
#include "EditorViewModel.h"
#include "RendererController.h"

namespace lookdevpt::assistant
{
struct AssistantEnvelope;
struct AssistantHostBridgeStateUpdate;
}

namespace winrt::D3D12LookDevPTwithAI::implementation
{
enum class EditorThemeMode
{
    System,
    Light,
    Dark,
};

enum class RightDockMode
{
    Inspector,
    Assistant,
};

struct AssistantUiState;

struct MainWindow : MainWindowT<MainWindow>
{
    MainWindow();

    void OnLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnClosed(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::WindowEventArgs const&);
    void OnActualThemeChanged(
        Microsoft::UI::Xaml::FrameworkElement const&,
        Windows::Foundation::IInspectable const&);
    void OnViewportLoaded(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnViewportGotFocus(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnViewportLostFocus(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnViewportHostSizeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnViewportPointerPressed(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void OnViewportPointerMoved(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void OnViewportPointerReleased(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void OnViewportPointerExited(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void OnViewportPointerWheelChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
    void OnPreviewKeyDown(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnPreviewKeyUp(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnEditGotFocus(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnEditLostFocus(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnNumberChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
    void OnSliderChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void OnToggleChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnComboChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnColorChanged(
        Microsoft::UI::Xaml::Controls::ColorPicker const&,
        Microsoft::UI::Xaml::Controls::ColorChangedEventArgs const&);
    void OnActionClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnProjectMenuClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPanelToggleClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnThemeClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnShowAllPanels(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnResetLayout(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRenderOnlyClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRightDockModeChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiAssistantMenuClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiSendClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiStopClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiQuickPromptClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiSendAcceleratorInvoked(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
    void OnAiConversationChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnAiNewConversationClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiSetupClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiSetupCancelClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiLicenseClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiLicenseConsentChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAiRetryClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnMaterialSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnMaterialSearchChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void OnVariantSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnPresetSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnApprovalSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnTextureClick(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnTextureResolutionSelectionChanged(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnCopyToken(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnExportMcpSettings(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnLeftSplitterDrag(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const&);
    void OnRightSplitterDrag(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const&);
    void OnBottomSplitterDrag(
        Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const&);

private:
    void Submit(
        lookdevpt::winui::EditorCommand command);
    void RefreshSnapshot();
    void AttachViewport();
    void SendPointer(
        lookdevpt::winui::PointerEventType type,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void ApplyRenderOnly(bool enabled);
    void ToggleRenderOnly();
    void ApplyRightDockMode();
    void ToggleAssistant();
    void StartAssistant();
    void StopAssistant() noexcept;
    void UpdateAssistantContext(
        lookdevpt::winui::EditorSnapshot const& snapshot);
    void HandleAssistantEvent(
        lookdevpt::assistant::AssistantEnvelope event);
    void HandleAssistantHostState(
        lookdevpt::assistant::AssistantHostBridgeStateUpdate update);
    void TryInitializeAssistant();
    void UpdateAssistantInteractionState();
    bool PostAssistantRequest(
        std::string method,
        std::string payload,
        std::string* queuedRequestId = nullptr);
    void SendAssistantTurn(
        std::wstring text,
        std::wstring promptId = {});
    void ApplyPanelVisibility();
    void ApplyTheme(EditorThemeMode mode);
    void UpdateTitleBarTheme();
    void LoadLayout();
    void SaveLayout();
    Windows::Foundation::IAsyncAction PickFile(
        std::wstring action);
    Windows::Foundation::IAsyncAction PickTexture(
        std::int32_t slot);
    Windows::Foundation::IAsyncAction SaveProjectAs();
    Windows::Foundation::IAsyncAction ConfirmNewScene();
    Windows::Foundation::IAsyncAction ExportMcpSettings();
    HWND WindowHandle();
    static LRESULT CALLBACK WindowSubclassProc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);

    static std::wstring Tag(
        Windows::Foundation::IInspectable const& sender);
    static bool TryDouble(
        lookdevpt::winui::EditorSnapshot const& snapshot,
        std::wstring const& property,
        double& value);
    static bool TryInteger(
        lookdevpt::winui::EditorSnapshot const& snapshot,
        std::wstring const& property,
        std::int64_t& value);
    static bool TryBool(
        lookdevpt::winui::EditorSnapshot const& snapshot,
        std::wstring const& property,
        bool& value);

    std::unique_ptr<lookdevpt::winui::RendererController> m_controller;
    winrt::com_ptr<lookdevpt::winui::EditorViewModel> m_viewModel;
    Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
    Microsoft::UI::Xaml::Controls::SwapChainPanel
        m_attachedPanel{ nullptr };
    std::uint64_t m_revision = 0;
    bool m_refreshing = false;
    bool m_closing = false;
    bool m_viewportFocused = false;
    bool m_editing = false;
    bool m_renderOnly = false;
    bool m_showLeft = true;
    bool m_showRight = true;
    bool m_showBottom = true;
    EditorThemeMode m_themeMode = EditorThemeMode::System;
    RightDockMode m_rightDockMode = RightDockMode::Assistant;
    double m_leftWidth = 330.0;
    double m_rightWidth = 420.0;
    double m_bottomHeight = 250.0;
    std::int32_t m_selectedVariant = -1;
    std::int32_t m_selectedPreset = -1;
    std::uint64_t m_selectedApproval = 0;
    std::vector<std::wstring> m_pairedClientIds;
    std::vector<std::wstring> m_pairedClientNames;
    std::shared_ptr<AssistantUiState> m_assistant;
    bool m_aiMcpStartRequested = false;
    HWND m_windowHandle = nullptr;
};
}

namespace winrt::D3D12LookDevPTwithAI::factory_implementation
{
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{
};
}
