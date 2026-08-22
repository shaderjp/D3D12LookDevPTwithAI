#include "pch.h"
#include "MainWindow.xaml.h"

#include "Services/AssistantHostBridge.h"
#include "SimpleJson.h"

#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace
{
std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr) != required)
    {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0)
    {
        return L"Invalid text received from the local assistant.";
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) != required)
    {
        return L"Invalid text received from the local assistant.";
    }
    return result;
}

std::wstring Trim(std::wstring value)
{
    const auto whitespace = [](wchar_t character)
    {
        return std::iswspace(character) != 0;
    };
    const auto first = std::find_if_not(
        value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), whitespace).base();
    if (first >= last)
    {
        return {};
    }
    return std::wstring(first, last);
}

std::string NewIdentifier()
{
    GUID identifier{};
    check_hresult(CoCreateGuid(&identifier));
    std::array<wchar_t, 40> buffer{};
    if (StringFromGUID2(
            identifier,
            buffer.data(),
            static_cast<int>(buffer.size())) <= 0)
    {
        throw hresult_error(E_FAIL);
    }
    std::wstring text(buffer.data());
    std::erase(text, L'{');
    std::erase(text, L'}');
    std::transform(
        text.begin(), text.end(), text.begin(),
        [](wchar_t value)
        {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return WideToUtf8(text);
}

std::string NormalizeContextPath(std::wstring const& value)
{
    if (value.empty())
    {
        return {};
    }
    std::wstring normalized;
    try
    {
        normalized = std::filesystem::path(value)
            .lexically_normal().wstring();
    }
    catch (...)
    {
        normalized = value;
    }
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return WideToUtf8(normalized);
}

std::string ProjectContextKey(
    std::wstring const& projectPath,
    std::wstring const& scenePath)
{
    std::string source;
    if (!projectPath.empty())
    {
        source = "project:" + NormalizeContextPath(projectPath);
    }
    else if (!scenePath.empty())
    {
        source = "scene:" + NormalizeContextPath(scenePath);
    }
    else
    {
        source = "preview-scene";
    }

    // A short deterministic key keeps long or non-ASCII paths out of the
    // history database while still separating projects and loose scenes.
    constexpr std::uint64_t fnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t fnvPrime = 1099511628211ull;
    std::uint64_t first = fnvOffset;
    std::uint64_t second = fnvOffset ^ 0x9e3779b97f4a7c15ull;
    for (unsigned char byte : source)
    {
        first = (first ^ byte) * fnvPrime;
        second = (second ^ static_cast<unsigned char>(byte + 0x51u)) *
            fnvPrime;
    }

    std::ostringstream key;
    key << "lookdev-" << std::hex << std::setfill('0')
        << std::setw(16) << first
        << std::setw(16) << second;
    return key.str();
}

cld::JsonValue const* Member(
    cld::JsonValue const& value,
    char const* name)
{
    return cld::FindMember(value, name);
}

cld::JsonValue const* ObjectMember(
    cld::JsonValue const& value,
    char const* name)
{
    const cld::JsonValue* member = Member(value, name);
    return member && member->type == cld::JsonValue::Type::Object
        ? member
        : nullptr;
}

cld::JsonValue const* ArrayMember(
    cld::JsonValue const& value,
    char const* name)
{
    const cld::JsonValue* member = Member(value, name);
    return member && member->type == cld::JsonValue::Type::Array
        ? member
        : nullptr;
}

std::wstring DisplayRole(std::string const& role, bool isError)
{
    if (isError || role == "error")
    {
        return L"Error";
    }
    if (role == "user")
    {
        return L"You";
    }
    if (role == "system")
    {
        return L"System";
    }
    return L"AI Assistant";
}

TextBlock AddTranscriptCard(
    ListView const& transcript,
    std::wstring const& role,
    std::wstring const& content,
    bool isError = false)
{
    Border card;
    card.Padding(Thickness{ 10, 8, 10, 8 });
    card.Margin(Thickness{ 0, 0, 0, 7 });
    card.CornerRadius(CornerRadius{ 6 });
    card.HorizontalAlignment(HorizontalAlignment::Stretch);
    const Color tint = isError
        ? ColorHelper::FromArgb(34, 220, 64, 64)
        : role == L"You"
            ? ColorHelper::FromArgb(30, 0, 120, 215)
            : ColorHelper::FromArgb(20, 128, 128, 128);
    card.Background(SolidColorBrush(tint));

    StackPanel panel;
    panel.Spacing(4);
    TextBlock heading;
    heading.Text(role);
    heading.Opacity(0.72);
    TextBlock body;
    body.Text(content);
    body.TextWrapping(TextWrapping::Wrap);
    body.IsTextSelectionEnabled(true);
    panel.Children().Append(heading);
    panel.Children().Append(body);
    card.Child(panel);
    transcript.Items().Append(card);
    transcript.ScrollIntoView(card);
    return body;
}

std::wstring QuickPrompt(std::wstring const& identifier)
{
    if (identifier == L"lookdev.scene.describe")
    {
        return L"現在のシーン、主要マテリアル、ライティングを要約してください。";
    }
    if (identifier == L"lookdev.view.review")
    {
        return L"現在のビューを LookDev の観点からレビューし、改善点を優先順に提案してください。";
    }
    if (identifier == L"lookdev.camera.compose")
    {
        return L"被写体が見やすくなるカメラ構図を提案し、必要ならカメラを調整してください。";
    }
    if (identifier == L"lookdev.denoise.suggest")
    {
        return L"現在のレンダリング状態に合うデノイズ設定を提案してください。";
    }
    if (identifier == L"lookdev.compare")
    {
        return L"現在の設定と直前の状態を比較し、見た目と性能の差を説明してください。";
    }
    if (identifier == L"lookdev.benchmark")
    {
        return L"現在のシーンで展示向けの短いベンチマークを実行し、結果を要約してください。";
    }
    return {};
}
}

namespace winrt::D3D12LookDevPTwithAI::implementation
{
struct AssistantUiState
{
    std::unique_ptr<lookdevpt::assistant::AssistantHostBridge> bridge;
    std::uint64_t outboundSequence = 0;
    bool connected = false;
    bool initialized = false;
    bool initializing = false;
    bool mcpReady = false;
    bool turnActive = false;
    bool fillingConversations = false;
    std::string instanceId;
    std::string projectContextKey;
    std::string activeConversationId;
    std::string pendingCreateRequestId;
    std::string pendingConversationId;
    std::string pendingConversationRequestId;
    std::string activeTurnId;
    std::vector<std::string> conversationIds;
    std::unordered_map<std::string, TextBlock> messageBodies;
};

void RestoreActiveConversationSelection(
    AssistantUiState& state,
    ComboBox const& conversations)
{
    int selectedIndex = -1;
    for (std::size_t index = 0;
         index < state.conversationIds.size();
         ++index)
    {
        if (state.conversationIds[index] == state.activeConversationId)
        {
            selectedIndex = static_cast<int>(index);
            break;
        }
    }
    state.fillingConversations = true;
    conversations.SelectedIndex(selectedIndex);
    state.fillingConversations = false;
}

void MainWindow::ApplyRightDockMode()
{
    const bool assistant =
        m_rightDockMode == RightDockMode::Assistant;
    if (InspectorModeButton())
    {
        InspectorModeButton().IsChecked(!assistant);
    }
    if (AssistantModeButton())
    {
        AssistantModeButton().IsChecked(assistant);
    }
    if (RightTabs())
    {
        RightTabs().Visibility(
            assistant ? Visibility::Collapsed : Visibility::Visible);
    }
    if (AssistantPane())
    {
        AssistantPane().Visibility(
            assistant ? Visibility::Visible : Visibility::Collapsed);
    }
}

void MainWindow::UpdateAssistantInteractionState()
{
    const auto state = m_assistant;
    const bool ready = state && state->connected && state->initialized;
    const bool requestPending = state &&
        (!state->pendingCreateRequestId.empty() ||
         !state->pendingConversationRequestId.empty());
    const bool idle = ready && !state->turnActive && !requestPending;
    const bool canSend = idle && !state->activeConversationId.empty();
    AiConversationCombo().IsEnabled(idle);
    AiNewConversationButton().IsEnabled(idle);
    AiSendButton().IsEnabled(canSend);
    for (const auto& child : AiQuickPromptPanel().Children())
    {
        if (const auto button = child.try_as<Button>())
        {
            button.IsEnabled(canSend);
        }
    }
}

void MainWindow::OnRightDockModeChanged(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const auto radio = sender.try_as<RadioButton>();
    if (!radio || !unbox_value_or<bool>(radio.IsChecked(), false))
    {
        return;
    }
    m_rightDockMode = radio == AssistantModeButton()
        ? RightDockMode::Assistant
        : RightDockMode::Inspector;
    ApplyPanelVisibility();
}

void MainWindow::ToggleAssistant()
{
    if (m_renderOnly)
    {
        ToggleRenderOnly();
    }
    const bool inspectorAvailable =
        InspectorModeButton() && InspectorModeButton().IsEnabled();
    m_rightDockMode =
        m_rightDockMode == RightDockMode::Assistant && inspectorAvailable
            ? RightDockMode::Inspector
            : RightDockMode::Assistant;
    ApplyPanelVisibility();
    if (m_rightDockMode == RightDockMode::Assistant && AiPromptBox())
    {
        AiPromptBox().Focus(FocusState::Programmatic);
    }
}

void MainWindow::OnAiAssistantMenuClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    if (m_renderOnly)
    {
        ToggleRenderOnly();
    }
    m_rightDockMode = RightDockMode::Assistant;
    ApplyPanelVisibility();
    AiPromptBox().Focus(FocusState::Programmatic);
}

void MainWindow::StartAssistant()
{
    if (m_closing || m_assistant)
    {
        return;
    }

    auto state = std::make_shared<AssistantUiState>();
    state->instanceId = NewIdentifier();
    state->bridge = std::make_unique<
        lookdevpt::assistant::AssistantHostBridge>();
    m_assistant = state;

    AiRuntimeStatusText().Text(L"Starting local AI host\u2026");
    AiModelStatusText().Text(L"Model: waiting for runtime");
    AiSetupProgress().Visibility(Visibility::Visible);
    AiRetryButton().IsEnabled(false);
    AiSendButton().IsEnabled(false);
    AiStopButton().IsEnabled(false);
    AiConversationCombo().IsEnabled(false);
    AiNewConversationButton().IsEnabled(false);
    UpdateAssistantInteractionState();

    const auto weakWindow = get_weak();
    const std::weak_ptr<AssistantUiState> weakState = state;
    const auto queue = DispatcherQueue();
    try
    {
        state->bridge->Start(
            {},
            [weakWindow, weakState, queue](
                lookdevpt::assistant::AssistantEnvelope envelope)
            {
                auto message = std::make_shared<
                    lookdevpt::assistant::AssistantEnvelope>(
                        std::move(envelope));
                queue.TryEnqueue(
                    [weakWindow, weakState, message]() mutable
                    {
                        auto session = weakState.lock();
                        if (auto self = weakWindow.get();
                            self && session &&
                            self->m_assistant == session &&
                            !self->m_closing)
                        {
                            self->HandleAssistantEvent(
                                std::move(*message));
                        }
                    });
            },
            [weakWindow, weakState, queue](
                lookdevpt::assistant::AssistantHostBridgeStateUpdate update)
            {
                auto message = std::make_shared<
                    lookdevpt::assistant::AssistantHostBridgeStateUpdate>(
                        std::move(update));
                queue.TryEnqueue(
                    [weakWindow, weakState, message]() mutable
                    {
                        auto session = weakState.lock();
                        if (auto self = weakWindow.get();
                            self && session &&
                            self->m_assistant == session &&
                            !self->m_closing)
                        {
                            self->HandleAssistantHostState(
                                std::move(*message));
                        }
                    });
            });
    }
    catch (...)
    {
        AiSetupProgress().Visibility(Visibility::Collapsed);
        AiRuntimeStatusText().Text(
            L"Local AI host could not be started.");
        AiModelStatusText().Text(
            L"Build or install the local ChatHost, then retry.");
        AiRetryButton().IsEnabled(true);
    }

    // OnLoaded may run after the renderer snapshot revision was already
    // consumed by the refresh timer. Seed the assistant from that immutable
    // snapshot instead of waiting for a renderer-side change.
    if (m_controller)
    {
        if (auto snapshot = m_controller->LatestSnapshot())
        {
            UpdateAssistantContext(*snapshot);
        }
    }
}

void MainWindow::StopAssistant() noexcept
{
    auto state = m_assistant;
    if (!state)
    {
        return;
    }
    if (state->initialized && state->connected && state->bridge)
    {
        // StopAndJoin gives a successfully queued shutdown request a bounded
        // grace period before terminating the owned process tree.
        try
        {
            (void)PostAssistantRequest("shutdown", "{}");
        }
        catch (...)
        {
            // StopAssistant is a shutdown boundary and must remain noexcept.
        }
    }
    state = std::move(m_assistant);
    try
    {
        if (state->bridge)
        {
            state->bridge->StopAndJoin();
        }
    }
    catch (...)
    {
        // Window shutdown must continue even if the helper process is broken.
    }
}

void MainWindow::UpdateAssistantContext(
    lookdevpt::winui::EditorSnapshot const& snapshot)
{
    if (!m_assistant || m_closing)
    {
        return;
    }

    if (snapshot.rendererReady && !snapshot.mcpRunning &&
        !m_aiMcpStartRequested)
    {
        m_aiMcpStartRequested = true;
        Submit({
            .type = lookdevpt::winui::EditorCommandType::Action,
            .property = L"mcp.start",
        });
        AiRuntimeStatusText().Text(
            L"Starting the LookDev MCP service\u2026");
    }
    if (snapshot.mcpRunning)
    {
        m_aiMcpStartRequested = false;
    }

    const std::string contextKey = ProjectContextKey(
        snapshot.projectPath, snapshot.scenePath);

    // The current ChatHost binds one project context for its lifetime. A new
    // project therefore gets a fresh local host and history partition.
    if (m_assistant->initialized &&
        (!m_assistant->projectContextKey.empty() &&
         m_assistant->projectContextKey != contextKey))
    {
        StopAssistant();
        if (!m_closing)
        {
            StartAssistant();
            if (!m_assistant)
            {
                return;
            }
        }
    }

    auto state = m_assistant;
    if (!state)
    {
        return;
    }
    state->mcpReady = snapshot.mcpRunning;
    state->projectContextKey = contextKey;

    if (!snapshot.mcpLastError.empty() && !snapshot.mcpRunning)
    {
        AiRuntimeStatusText().Text(
            L"LookDev MCP could not be started.");
        AiModelStatusText().Text(snapshot.mcpLastError);
        AiRetryButton().IsEnabled(true);
    }
    TryInitializeAssistant();
}

void MainWindow::HandleAssistantHostState(
    lookdevpt::assistant::AssistantHostBridgeStateUpdate update)
{
    if (!m_assistant || m_closing)
    {
        return;
    }
    using State = lookdevpt::assistant::AssistantHostBridgeState;
    switch (update.state)
    {
    case State::Starting:
        AiRuntimeStatusText().Text(L"Starting local AI host\u2026");
        AiSetupProgress().Visibility(Visibility::Visible);
        break;
    case State::WaitingForConnection:
        AiRuntimeStatusText().Text(L"Waiting for local AI host\u2026");
        AiSetupProgress().Visibility(Visibility::Visible);
        break;
    case State::Connected:
        m_assistant->connected = true;
        AiRuntimeStatusText().Text(
            m_assistant->mcpReady
                ? L"Connecting the assistant to LookDev\u2026"
                : L"Local AI host connected; waiting for MCP\u2026");
        AiRetryButton().IsEnabled(false);
        TryInitializeAssistant();
        break;
    case State::Failed:
        m_assistant->connected = false;
        m_assistant->initializing = false;
        m_assistant->initialized = false;
        m_assistant->turnActive = false;
        m_assistant->activeTurnId.clear();
        m_assistant->pendingCreateRequestId.clear();
        m_assistant->pendingConversationId.clear();
        m_assistant->pendingConversationRequestId.clear();
        AiSetupProgress().Visibility(Visibility::Collapsed);
        AiRuntimeStatusText().Text(L"Local AI host disconnected.");
        AiModelStatusText().Text(
            L"Retry the private local connection.");
        AiRetryButton().IsEnabled(true);
        AiSendButton().IsEnabled(false);
        AiStopButton().IsEnabled(false);
        UpdateAssistantInteractionState();
        break;
    case State::Stopped:
        m_assistant->connected = false;
        m_assistant->initializing = false;
        m_assistant->initialized = false;
        m_assistant->turnActive = false;
        m_assistant->activeTurnId.clear();
        m_assistant->pendingCreateRequestId.clear();
        m_assistant->pendingConversationId.clear();
        m_assistant->pendingConversationRequestId.clear();
        AiSetupProgress().Visibility(Visibility::Collapsed);
        AiSendButton().IsEnabled(false);
        AiStopButton().IsEnabled(false);
        UpdateAssistantInteractionState();
        break;
    }
}

void MainWindow::TryInitializeAssistant()
{
    auto state = m_assistant;
    if (!state || state->initialized || state->initializing ||
        !state->connected || !state->mcpReady ||
        state->projectContextKey.empty())
    {
        return;
    }

    std::ostringstream payload;
    payload << "{"
        << "\"instanceId\":\""
        << cld::EscapeJson(state->instanceId) << "\","
        << "\"projectContextKey\":\""
        << cld::EscapeJson(state->projectContextKey) << "\"}"
        ;
    state->initializing = true;
    AiRuntimeStatusText().Text(L"Initializing local assistant\u2026");
    AiModelStatusText().Text(L"Runtime: connecting to ChatHost");
    AiSetupProgress().Visibility(Visibility::Visible);
    if (!PostAssistantRequest("initialize", payload.str()))
    {
        state->initializing = false;
    }
}

bool MainWindow::PostAssistantRequest(
    std::string method,
    std::string payload,
    std::string* queuedRequestId)
{
    if (queuedRequestId)
    {
        queuedRequestId->clear();
    }
    auto state = m_assistant;
    if (!state || !state->bridge)
    {
        return false;
    }
    try
    {
        const std::string requestId = NewIdentifier();
        const std::uint64_t sequence = ++state->outboundSequence;
        std::ostringstream envelope;
        envelope << "{"
            << "\"protocolVersion\":1,"
            << "\"kind\":\"request\","
            << "\"requestId\":\""
            << cld::EscapeJson(requestId) << "\","
            << "\"sequence\":" << sequence << ","
            << "\"method\":\""
            << cld::EscapeJson(method) << "\","
            << "\"payload\":" << payload
            << "}";
        if (state->bridge->Post(envelope.str()))
        {
            if (queuedRequestId)
            {
                *queuedRequestId = requestId;
            }
            return true;
        }
    }
    catch (lookdevpt::assistant::ProtocolError const&)
    {
    }
    catch (...)
    {
    }
    AiRuntimeStatusText().Text(
        L"The local assistant request could not be queued.");
    AiRetryButton().IsEnabled(true);
    return false;
}

void MainWindow::HandleAssistantEvent(
    lookdevpt::assistant::AssistantEnvelope event)
{
    auto state = m_assistant;
    if (!state || m_closing)
    {
        return;
    }
    const cld::JsonValue& root = event.root;
    const std::string kind = cld::JsonStringOr(root, "kind");
    const std::string method = cld::JsonStringOr(root, "method");
    const cld::JsonValue* payload = ObjectMember(root, "payload");
    if (!payload)
    {
        return;
    }

    if (kind == "response")
    {
        if (method == "conversation.create" &&
            (state->pendingCreateRequestId.empty() ||
             event.requestId != state->pendingCreateRequestId))
        {
            return;
        }
        if (method == "conversation.select" &&
            !state->pendingConversationRequestId.empty() &&
            event.requestId != state->pendingConversationRequestId)
        {
            // A user selection supersedes the automatic history load issued
            // during initialization. Do not paint that stale response.
            return;
        }
        if (const cld::JsonValue* error = ObjectMember(root, "error"))
        {
            const std::wstring code = Utf8ToWide(
                cld::JsonStringOr(*error, "code", "request_failed"));
            const std::wstring message = Utf8ToWide(
                cld::JsonStringOr(
                    *error, "message", "The local request failed."));
            AddTranscriptCard(
                AiTranscriptList(), L"Error",
                code + L": " + message, true);
            if (method == "initialize")
            {
                state->initializing = false;
                state->initialized = false;
                AiRetryButton().IsEnabled(true);
            }
            if (method == "sendTurn")
            {
                state->turnActive = false;
                state->activeTurnId.clear();
                AiStopButton().IsEnabled(false);
            }
            if (method == "conversation.select")
            {
                state->pendingConversationId.clear();
                state->pendingConversationRequestId.clear();
                RestoreActiveConversationSelection(
                    *state, AiConversationCombo());
            }
            if (method == "conversation.create")
            {
                state->pendingCreateRequestId.clear();
            }
            AiSetupProgress().Visibility(Visibility::Collapsed);
            UpdateAssistantInteractionState();
            return;
        }

        if (method == "initialize")
        {
            state->initializing = false;
            state->initialized = true;
            state->activeConversationId =
                cld::JsonStringOr(*payload, "activeConversationId");
            const std::string hostVersion =
                cld::JsonStringOr(*payload, "hostVersion");
            state->fillingConversations = true;
            state->conversationIds.clear();
            AiConversationCombo().Items().Clear();
            int selectedIndex = -1;
            if (const cld::JsonValue* conversations =
                    ArrayMember(*payload, "conversations"))
            {
                for (const cld::JsonValue& conversation :
                     conversations->array)
                {
                    if (conversation.type !=
                        cld::JsonValue::Type::Object)
                    {
                        continue;
                    }
                    const std::string id =
                        cld::JsonStringOr(conversation, "id");
                    const std::string title = cld::JsonStringOr(
                        conversation, "title", "New chat");
                    if (id.empty())
                    {
                        continue;
                    }
                    if (id == state->activeConversationId)
                    {
                        selectedIndex = static_cast<int>(
                            state->conversationIds.size());
                    }
                    state->conversationIds.push_back(id);
                    AiConversationCombo().Items().Append(
                        box_value(to_hstring(title)));
                }
            }
            AiConversationCombo().SelectedIndex(selectedIndex);
            state->fillingConversations = false;
            AiSetupProgress().Visibility(Visibility::Collapsed);
            AiRuntimeStatusText().Text(L"Local assistant is ready");
            AiModelStatusText().Text(
                hostVersion.empty()
                    ? L"Runtime: connected"
                    : L"Runtime: ChatHost " + Utf8ToWide(hostVersion));
            UpdateAssistantInteractionState();
            if (!state->activeConversationId.empty())
            {
                PostAssistantRequest(
                    "conversation.select",
                    "{\"conversationId\":\"" +
                        cld::EscapeJson(state->activeConversationId) +
                        "\"}");
            }
            return;
        }

        if (method == "conversation.list")
        {
            const std::string active = cld::JsonStringOr(
                *payload,
                "activeConversationId",
                state->activeConversationId);
            state->fillingConversations = true;
            state->activeConversationId = active;
            state->conversationIds.clear();
            AiConversationCombo().Items().Clear();
            int selectedIndex = -1;
            if (const cld::JsonValue* conversations =
                    ArrayMember(*payload, "conversations"))
            {
                for (const cld::JsonValue& conversation :
                     conversations->array)
                {
                    if (conversation.type !=
                        cld::JsonValue::Type::Object)
                    {
                        continue;
                    }
                    const std::string id =
                        cld::JsonStringOr(conversation, "id");
                    if (id.empty())
                    {
                        continue;
                    }
                    if (id == active)
                    {
                        selectedIndex = static_cast<int>(
                            state->conversationIds.size());
                    }
                    state->conversationIds.push_back(id);
                    AiConversationCombo().Items().Append(box_value(
                        to_hstring(cld::JsonStringOr(
                            conversation, "title", "New chat"))));
                }
            }
            AiConversationCombo().SelectedIndex(selectedIndex);
            state->fillingConversations = false;
            UpdateAssistantInteractionState();
            return;
        }

        if (method == "conversation.create")
        {
            if (const cld::JsonValue* conversation =
                    ObjectMember(*payload, "conversation"))
            {
                const std::string id =
                    cld::JsonStringOr(*conversation, "id");
                const std::string title = cld::JsonStringOr(
                    *conversation, "title", "New chat");
                state->pendingCreateRequestId.clear();
                if (!id.empty())
                {
                    auto found = std::find(
                        state->conversationIds.begin(),
                        state->conversationIds.end(),
                        id);
                    std::size_t selectedIndex = 0;
                    state->fillingConversations = true;
                    if (found == state->conversationIds.end())
                    {
                        state->conversationIds.insert(
                            state->conversationIds.begin(), id);
                        AiConversationCombo().Items().InsertAt(
                            0, box_value(to_hstring(title)));
                    }
                    else
                    {
                        selectedIndex = static_cast<std::size_t>(
                            std::distance(
                                state->conversationIds.begin(), found));
                        AiConversationCombo().Items().SetAt(
                            static_cast<std::uint32_t>(selectedIndex),
                            box_value(to_hstring(title)));
                    }
                    state->activeConversationId = id;
                    AiConversationCombo().SelectedIndex(
                        static_cast<int>(selectedIndex));
                    state->fillingConversations = false;
                    AiTranscriptList().Items().Clear();
                    state->messageBodies.clear();
                    UpdateAssistantInteractionState();
                    return;
                }
            }
            state->pendingCreateRequestId.clear();
            RestoreActiveConversationSelection(
                *state, AiConversationCombo());
            AddTranscriptCard(
                AiTranscriptList(),
                L"Error",
                L"The new conversation response was invalid.",
                true);
            UpdateAssistantInteractionState();
            return;
        }

        if (method == "conversation.select")
        {
            std::string selectedConversationId;
            if (const cld::JsonValue* conversation =
                    ObjectMember(*payload, "conversation"))
            {
                selectedConversationId =
                    cld::JsonStringOr(*conversation, "id");
            }
            if (selectedConversationId.empty() ||
                (!state->pendingConversationId.empty() &&
                 selectedConversationId !=
                    state->pendingConversationId))
            {
                state->pendingConversationId.clear();
                state->pendingConversationRequestId.clear();
                RestoreActiveConversationSelection(
                    *state, AiConversationCombo());
                UpdateAssistantInteractionState();
                AddTranscriptCard(
                    AiTranscriptList(),
                    L"Error",
                    L"The selected conversation response was invalid.",
                    true);
                return;
            }
            state->activeConversationId =
                std::move(selectedConversationId);
            state->pendingConversationId.clear();
            state->pendingConversationRequestId.clear();
            RestoreActiveConversationSelection(
                *state, AiConversationCombo());
            AiTranscriptList().Items().Clear();
            state->messageBodies.clear();
            if (const cld::JsonValue* messages =
                    ArrayMember(*payload, "messages"))
            {
                for (const cld::JsonValue& message : messages->array)
                {
                    if (message.type !=
                        cld::JsonValue::Type::Object)
                    {
                        continue;
                    }
                    const std::string id =
                        cld::JsonStringOr(message, "id");
                    const std::string role =
                        cld::JsonStringOr(message, "role", "assistant");
                    const bool isError =
                        cld::JsonBoolOr(message, "isError", false);
                    TextBlock body = AddTranscriptCard(
                        AiTranscriptList(),
                        DisplayRole(role, isError),
                        Utf8ToWide(cld::JsonStringOr(
                            message, "content")),
                        isError);
                    if (!id.empty())
                    {
                        state->messageBodies.insert_or_assign(id, body);
                    }
                }
            }
            UpdateAssistantInteractionState();
            return;
        }
    }

    if (kind != "event")
    {
        return;
    }
    if (method == "runtimeState")
    {
        const std::string runtimeStatus =
            cld::JsonStringOr(*payload, "status", "ready");
        const std::string backend =
            cld::JsonStringOr(*payload, "backend", "local");
        AiRuntimeStatusText().Text(
            runtimeStatus == "ready"
                ? L"Local assistant is ready"
                : L"Assistant: " + Utf8ToWide(runtimeStatus));
        AiModelStatusText().Text(
            L"Runtime backend: " + Utf8ToWide(backend));
        return;
    }
    if (method == "messageAdded")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId)
        {
            return;
        }
        const cld::JsonValue* message =
            ObjectMember(*payload, "message");
        if (!message)
        {
            return;
        }
        const std::string id = cld::JsonStringOr(*message, "id");
        const std::string role =
            cld::JsonStringOr(*message, "role", "assistant");
        const std::wstring content = Utf8ToWide(
            cld::JsonStringOr(*message, "content"));
        const bool isError =
            cld::JsonBoolOr(*message, "isError", false);
        auto found = state->messageBodies.find(id);
        if (found != state->messageBodies.end())
        {
            found->second.Text(content);
        }
        else
        {
            TextBlock body = AddTranscriptCard(
                AiTranscriptList(),
                DisplayRole(role, isError),
                content,
                isError);
            if (!id.empty())
            {
                state->messageBodies.insert_or_assign(id, body);
            }
        }
        return;
    }
    if (method == "textDelta")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId)
        {
            return;
        }
        const std::string id =
            cld::JsonStringOr(*payload, "messageId");
        const std::wstring delta = Utf8ToWide(
            cld::JsonStringOr(*payload, "delta"));
        auto found = state->messageBodies.find(id);
        if (found == state->messageBodies.end())
        {
            TextBlock body = AddTranscriptCard(
                AiTranscriptList(), L"AI Assistant", delta);
            if (!id.empty())
            {
                state->messageBodies.insert_or_assign(id, body);
            }
        }
        else
        {
            found->second.Text(found->second.Text() + delta);
            if (auto parent = found->second.Parent())
            {
                AiTranscriptList().ScrollIntoView(parent);
            }
        }
        return;
    }
    if (method == "completed")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        if (!state->activeTurnId.empty() &&
            turnId == state->activeTurnId)
        {
            state->turnActive = false;
            state->activeTurnId.clear();
            AiStopButton().IsEnabled(false);
            UpdateAssistantInteractionState();
        }
        return;
    }
    if (method == "error")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId)
        {
            return;
        }
        const std::wstring code = Utf8ToWide(
            cld::JsonStringOr(*payload, "code", "turn_failed"));
        const std::wstring message = Utf8ToWide(
            cld::JsonStringOr(
                *payload, "message", "The local turn failed."));
        AddTranscriptCard(
            AiTranscriptList(), L"Error",
            code + L": " + message, true);
        state->turnActive = false;
        state->activeTurnId.clear();
        AiStopButton().IsEnabled(false);
        UpdateAssistantInteractionState();
        return;
    }
    if (method == "toolApprovalRequired" ||
        method == "approvalRequested")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId)
        {
            return;
        }
        const std::string approvalId =
            cld::JsonStringOr(*payload, "approvalId");
        const std::wstring tool = Utf8ToWide(
            cld::JsonStringOr(*payload, "tool", "LookDev tool"));
        const std::wstring summary = Utf8ToWide(
            cld::JsonStringOr(
                *payload, "summary", "Approval is required."));

        Border card;
        card.Padding(Thickness{ 10 });
        card.Margin(Thickness{ 0, 0, 0, 7 });
        card.CornerRadius(CornerRadius{ 6 });
        card.Background(SolidColorBrush(
            ColorHelper::FromArgb(34, 255, 170, 0)));
        StackPanel panel;
        panel.Spacing(6);
        TextBlock heading;
        heading.Text(L"Approval required · " + tool);
        TextBlock detail;
        detail.Text(
            summary +
            L"\n\nAllow once is unavailable until the native "
            L"one-time grant broker is implemented. You can deny this "
            L"request safely.");
        detail.TextWrapping(TextWrapping::Wrap);
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(6);
        Button allow;
        allow.Content(box_value(L"Allow once (unavailable)"));
        allow.IsEnabled(false);
        Button deny;
        deny.Content(box_value(L"Deny"));
        deny.IsEnabled(!approvalId.empty());
        const auto weakWindow = get_weak();
        const std::weak_ptr<AssistantUiState> weakState = state;
        deny.Click([weakWindow, weakState, approvalId, turnId](
            IInspectable const& sender, RoutedEventArgs const&)
        {
            auto session = weakState.lock();
            if (auto self = weakWindow.get();
                self && session &&
                self->m_assistant == session &&
                session->activeTurnId == turnId &&
                !self->m_closing)
            {
                const bool posted = self->PostAssistantRequest(
                    "approval.respond",
                    "{\"approvalId\":\"" +
                        cld::EscapeJson(approvalId) +
                        "\",\"decision\":\"deny\"}");
                if (posted)
                {
                    if (auto button = sender.try_as<Button>())
                    {
                        button.IsEnabled(false);
                    }
                }
            }
        });
        actions.Children().Append(allow);
        actions.Children().Append(deny);
        panel.Children().Append(heading);
        panel.Children().Append(detail);
        panel.Children().Append(actions);
        card.Child(panel);
        AiTranscriptList().Items().Append(card);
        AiTranscriptList().ScrollIntoView(card);
    }
}

void MainWindow::SendAssistantTurn(
    std::wstring text,
    std::wstring promptId)
{
    auto state = m_assistant;
    text = Trim(std::move(text));
    if (!state || !state->initialized || state->turnActive ||
        !state->pendingCreateRequestId.empty() ||
        !state->pendingConversationRequestId.empty() ||
        state->activeConversationId.empty() || text.empty())
    {
        return;
    }

    const std::string turnId = NewIdentifier();
    std::ostringstream payload;
    payload << "{"
        << "\"turnId\":\"" << cld::EscapeJson(turnId) << "\","
        << "\"conversationId\":\""
        << cld::EscapeJson(state->activeConversationId) << "\","
        << "\"text\":\""
        << cld::EscapeJson(WideToUtf8(text)) << "\"";
    if (!promptId.empty())
    {
        // Kept as forward-compatible metadata; older ChatHost builds ignore
        // unknown payload properties.
        payload << ",\"promptId\":\""
            << cld::EscapeJson(WideToUtf8(promptId)) << "\"";
    }
    payload << "}";
    if (!PostAssistantRequest("sendTurn", payload.str()))
    {
        return;
    }

    state->activeTurnId = turnId;
    state->turnActive = true;
    AiPromptBox().Text(L"");
    UpdateAssistantInteractionState();
    AiStopButton().IsEnabled(true);
}

void MainWindow::OnAiSendClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    SendAssistantTurn(AiPromptBox().Text().c_str());
}

void MainWindow::OnAiStopClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    if (!m_assistant || !m_assistant->turnActive ||
        m_assistant->activeTurnId.empty())
    {
        return;
    }
    if (PostAssistantRequest(
            "cancelTurn",
            "{\"turnId\":\"" +
                cld::EscapeJson(m_assistant->activeTurnId) +
                "\"}"))
    {
        AiStopButton().IsEnabled(false);
        AiRuntimeStatusText().Text(L"Stopping the current turn\u2026");
    }
}

void MainWindow::OnAiQuickPromptClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const std::wstring identifier = Tag(sender);
    SendAssistantTurn(QuickPrompt(identifier), identifier);
}

void MainWindow::OnAiPromptKeyDown(
    IInspectable const&,
    KeyRoutedEventArgs const& args)
{
    if (args.Key() == VirtualKey::Enter &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        args.Handled(true);
        SendAssistantTurn(AiPromptBox().Text().c_str());
    }
}

void MainWindow::OnAiConversationChanged(
    IInspectable const&,
    SelectionChangedEventArgs const&)
{
    auto state = m_assistant;
    if (!state || state->fillingConversations ||
        !state->initialized || state->turnActive ||
        !state->pendingCreateRequestId.empty() ||
        !state->pendingConversationRequestId.empty())
    {
        return;
    }
    const int selected = AiConversationCombo().SelectedIndex();
    if (selected < 0 ||
        static_cast<std::size_t>(selected) >=
            state->conversationIds.size())
    {
        return;
    }
    const std::string id =
        state->conversationIds[static_cast<std::size_t>(selected)];
    if (id.empty() || id == state->activeConversationId)
    {
        return;
    }
    std::string requestId;
    if (PostAssistantRequest(
            "conversation.select",
            "{\"conversationId\":\"" + cld::EscapeJson(id) +
                "\"}",
            &requestId))
    {
        state->pendingConversationId = id;
        state->pendingConversationRequestId = std::move(requestId);
        UpdateAssistantInteractionState();
        return;
    }

    RestoreActiveConversationSelection(*state, AiConversationCombo());
    UpdateAssistantInteractionState();
}

void MainWindow::OnAiNewConversationClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    auto state = m_assistant;
    if (state && state->initialized && !state->turnActive &&
        state->pendingCreateRequestId.empty() &&
        state->pendingConversationRequestId.empty())
    {
        std::string requestId;
        if (PostAssistantRequest(
                "conversation.create", "{}", &requestId))
        {
            state->pendingCreateRequestId = std::move(requestId);
            UpdateAssistantInteractionState();
        }
    }
}

void MainWindow::OnAiSetupClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    AiSetupProgress().Visibility(Visibility::Visible);
    AiRuntimeStatusText().Text(L"Preparing local assistant services\u2026");
    AiModelStatusText().Text(
        L"Model setup is delegated to the installed local ChatHost runtime.");
    m_aiMcpStartRequested = false;
    if (!m_assistant)
    {
        StartAssistant();
    }
    if (m_controller)
    {
        if (auto snapshot = m_controller->LatestSnapshot())
        {
            UpdateAssistantContext(*snapshot);
        }
    }
}

void MainWindow::OnAiRetryClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    StopAssistant();
    m_aiMcpStartRequested = false;
    AiTranscriptList().Items().Clear();
    StartAssistant();
}
}
