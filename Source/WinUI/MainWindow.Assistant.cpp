#include "pch.h"
#include "MainWindow.xaml.h"
#include "AssistantMarkdownRenderer.h"

#include "Services/AssistantHostBridge.h"
#include "McpServer.h"
#include "SimpleJson.h"

#include <shobjidl.h>

#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
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

std::wstring AssistantMarkdownFileName(std::wstring title)
{
    constexpr std::wstring_view Invalid = L"<>:\"/\\|?*";
    for (wchar_t& character : title)
    {
        if (std::iswcntrl(character) || Invalid.find(character) != std::wstring_view::npos)
        {
            character = L'_';
        }
    }
    while (!title.empty() && (std::iswspace(title.back()) || title.back() == L'.'))
    {
        title.pop_back();
    }
    if (title.size() > 80)
    {
        title.resize(80);
        if (!title.empty() && IS_HIGH_SURROGATE(title.back()))
        {
            title.pop_back();
        }
    }
    return title.empty() ? L"lookdev-chat" : title;
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

bool IsPrivateLookDevMcpEndpoint(std::string_view endpoint)
{
    constexpr std::string_view prefix = "http://127.0.0.1:";
    constexpr std::string_view suffix = "/mcp";
    if (!endpoint.starts_with(prefix) || !endpoint.ends_with(suffix))
    {
        return false;
    }
    const std::string_view portText = endpoint.substr(
        prefix.size(), endpoint.size() - prefix.size() - suffix.size());
    if (portText.empty() || portText.size() > 5 ||
        !std::all_of(portText.begin(), portText.end(), [](unsigned char value)
        {
            return value >= '0' && value <= '9';
        }))
    {
        return false;
    }
    unsigned port = 0;
    for (unsigned char value : portText)
    {
        port = port * 10u + static_cast<unsigned>(value - '0');
    }
    return port > 0 && port <= 65535;
}

std::wstring SelectedComboTag(ComboBox const& combo)
{
    if (!combo)
    {
        return {};
    }
    const auto item = combo.SelectedItem().try_as<ComboBoxItem>();
    return item
        ? unbox_value_or<hstring>(item.Tag(), L"").c_str()
        : std::wstring{};
}

std::wstring FormatSetupBytes(double bytes)
{
    constexpr double mib = 1024.0 * 1024.0;
    constexpr double gib = 1024.0 * mib;
    std::wostringstream text;
    text << std::fixed;
    if (bytes >= gib)
    {
        text << std::setprecision(2) << bytes / gib << L" GiB";
    }
    else
    {
        text << std::setprecision(0) << bytes / mib << L" MiB";
    }
    return text.str();
}

void SecureClear(std::string& value) noexcept
{
    if (!value.empty())
    {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

struct ScopedSecret
{
    ~ScopedSecret()
    {
        SecureClear(value);
    }

    std::string value;
};

struct AssistantApprovalBinding
{
    ~AssistantApprovalBinding()
    {
        Clear();
    }

    void Clear() noexcept
    {
        SecureClear(approvalId);
        SecureClear(turnId);
        SecureClear(toolName);
        SecureClear(mcpSessionId);
        SecureClear(argumentsHash);
    }

    std::string approvalId;
    std::string turnId;
    std::string toolName;
    std::string mcpSessionId;
    std::string argumentsHash;
};

constexpr std::size_t MaximumAssistantToolNameBytes = 256;
constexpr std::size_t MaximumAssistantToolCallIdBytes = 256;
constexpr std::size_t MaximumAssistantApprovalIdBytes = 64;
constexpr std::size_t MaximumAssistantMcpSessionIdBytes = 256;
constexpr std::size_t MaximumAssistantApprovalArgumentsBytes = 8 * 1024;
constexpr std::size_t MaximumAssistantApprovalSummaryBytes = 4 * 1024;
constexpr std::size_t MaximumAssistantToolCodeBytes = 64;

bool IsSafeAssistantProtocolText(
    std::string_view value,
    std::size_t maximumBytes)
{
    return !value.empty() && value.size() <= maximumBytes &&
        std::none_of(value.begin(), value.end(), [](unsigned char character)
        {
            return character < 0x20 || character == 0x7f;
        });
}

bool IsLowerHexDigest(std::string_view value)
{
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char character)
        {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool ApprovalArgumentsMatchHash(
    std::string const& value,
    std::string_view expectedHash)
{
    if (value.empty() ||
        value.size() > MaximumAssistantApprovalArgumentsBytes ||
        !IsLowerHexDigest(expectedHash))
    {
        return false;
    }
    try
    {
        const auto arguments = cld::JsonParser(value).Parse();
        return arguments.type == cld::JsonValue::Type::Object &&
            mcp::CanonicalArgumentsJson(arguments) == value &&
            mcp::CanonicalArgumentsSha256(arguments) == expectedHash;
    }
    catch (...)
    {
        return false;
    }
}

bool IsToolCompletionStatus(std::string_view value)
{
    return value == "succeeded" || value == "failed" ||
        value == "denied" || value == "unknown";
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

struct AssistantTranscriptBody
{
    Border card{ nullptr };
    RichTextBlock body{ nullptr };
    std::wstring content;
    bool renderMarkdown = false;
    bool renderPending = false;
    std::chrono::steady_clock::time_point lastRender{};
};

void RenderTranscriptContent(AssistantTranscriptBody& entry)
{
    if (entry.renderMarkdown)
    {
        lookdevpt::ui::RenderAssistantMarkdown(entry.body, entry.content);
    }
    else
    {
        lookdevpt::ui::RenderAssistantPlainText(entry.body, entry.content);
    }
    entry.renderPending = false;
    entry.lastRender = std::chrono::steady_clock::now();
}

void SetTranscriptContent(
    AssistantTranscriptBody& entry,
    std::wstring const& content)
{
    entry.content = content;
    RenderTranscriptContent(entry);
}

void AppendTranscriptContent(
    AssistantTranscriptBody& entry,
    std::wstring_view delta)
{
    entry.content.append(delta);
    entry.renderPending = true;
    constexpr auto MinimumRenderInterval = std::chrono::milliseconds(33);
    if (std::chrono::steady_clock::now() - entry.lastRender >=
        MinimumRenderInterval)
    {
        RenderTranscriptContent(entry);
    }
}

std::shared_ptr<AssistantTranscriptBody> AddTranscriptCard(
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
    RichTextBlock body;
    body.TextWrapping(TextWrapping::Wrap);
    body.IsTextSelectionEnabled(true);
    body.HorizontalAlignment(HorizontalAlignment::Stretch);
    panel.Children().Append(heading);
    panel.Children().Append(body);
    card.Child(panel);
    transcript.Items().Append(card);
    transcript.ScrollIntoView(card);
    auto entry = std::make_shared<AssistantTranscriptBody>();
    entry->card = card;
    entry->body = body;
    entry->renderMarkdown = !isError && role == L"AI Assistant";
    SetTranscriptContent(*entry, content);
    return entry;
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
struct AssistantApprovalUiEntry
{
    std::string turnId;
    std::string toolCallId;
    std::string toolName;
    Button allow{ nullptr };
    Button deny{ nullptr };
    TextBlock status{ nullptr };
    std::shared_ptr<AssistantApprovalBinding> binding;
    bool pending = true;
    bool allowed = false;
    bool mutationStarted = false;
    bool terminal = false;
};

struct AssistantToolUiEntry
{
    std::string turnId;
    std::string toolName;
    Border card{ nullptr };
    TextBlock status{ nullptr };
    bool started = false;
    bool terminal = false;
    bool mutation = false;
};

struct AssistantUiState
{
    std::unique_ptr<lookdevpt::assistant::AssistantHostBridge> bridge;
    std::uint64_t outboundSequence = 0;
    bool connected = false;
    bool initialized = false;
    bool initializing = false;
    bool mcpReady = false;
    std::uint64_t mcpGeneration = 0;
    bool turnActive = false;
    bool fillingConversations = false;
    std::string instanceId;
    std::string projectContextKey;
    std::string activeConversationId;
    std::string pendingCreateRequestId;
    std::string pendingConversationId;
    std::string pendingConversationRequestId;
    std::string pendingResetRequestId;
    std::string pendingExportRequestId;
    std::string modelSetupRequestId;
    std::string activeTurnId;
    std::vector<std::string> conversationIds;
    std::unordered_map<
        std::string,
        std::shared_ptr<AssistantTranscriptBody>> messageBodies;
    std::unordered_map<
        std::string,
        std::shared_ptr<AssistantApprovalUiEntry>> approvals;
    std::unordered_map<std::string, std::string> approvalByToolCallId;
    std::unordered_map<
        std::string,
        std::shared_ptr<AssistantToolUiEntry>> toolCards;
    bool cancelRequested = false;
    bool modelSetupActive = false;
};

void SetAssistantToolStatus(
    AssistantToolUiEntry& entry,
    std::wstring const& text,
    Color const& tint) noexcept
{
    try
    {
        if (entry.status)
        {
            entry.status.Text(text);
        }
        if (entry.card)
        {
            entry.card.Background(SolidColorBrush(tint));
        }
    }
    catch (...)
    {
        // UI cleanup must not retain a capability if a visual was torn down.
    }
}

void CloseAssistantApproval(
    AssistantApprovalUiEntry& entry,
    std::wstring const& status,
    bool replaceStatus = true) noexcept
{
    entry.pending = false;
    if (entry.binding)
    {
        entry.binding->Clear();
    }
    try
    {
        if (entry.allow)
        {
            entry.allow.IsEnabled(false);
        }
        if (entry.deny)
        {
            entry.deny.IsEnabled(false);
        }
        if (replaceStatus && entry.status)
        {
            entry.status.Text(status);
        }
    }
    catch (...)
    {
        // The binding was already cleared before touching fallible UI state.
    }
}

void CloseAssistantApprovals(
    AssistantUiState& state,
    std::wstring const& status,
    bool replaceResolvedStatus = false) noexcept
{
    for (auto const& [id, entry] : state.approvals)
    {
        (void)id;
        if (!entry)
        {
            continue;
        }
        const bool wasPending = entry->pending;
        CloseAssistantApproval(
            *entry,
            status,
            wasPending || (replaceResolvedStatus && !entry->terminal));
    }
}

void MarkAssistantCancellationRequested(AssistantUiState& state) noexcept
{
    state.cancelRequested = true;
    CloseAssistantApprovals(
        state,
        L"Cancellation requested · approval closed.",
        true);
    for (auto const& [toolCallId, entry] : state.toolCards)
    {
        (void)toolCallId;
        if (!entry || entry->terminal || !entry->started)
        {
            continue;
        }
        if (entry->mutation)
        {
            SetAssistantToolStatus(
                *entry,
                L"Cancellation requested · result may be unknown. Verify the LookDev state.",
                ColorHelper::FromArgb(38, 255, 170, 0));
        }
        else
        {
            SetAssistantToolStatus(
                *entry,
                L"Cancellation requested · waiting for the tool outcome.",
                ColorHelper::FromArgb(24, 128, 128, 128));
        }
    }
}

void FinishAssistantTurnUi(
    AssistantUiState& state,
    lookdevpt::winui::RendererController* controller,
    std::wstring const& approvalStatus,
    std::wstring const& interruptedStatus) noexcept
{
    if (controller)
    {
        controller->SetAssistantInferenceActive(false);
    }
    CloseAssistantApprovals(state, approvalStatus, true);
    for (auto const& [toolCallId, entry] : state.toolCards)
    {
        (void)toolCallId;
        if (!entry || entry->terminal || !entry->started)
        {
            continue;
        }
        SetAssistantToolStatus(
            *entry,
            entry->mutation
                ? L"Result unknown · the turn ended after this change started. Verify the LookDev state."
                : interruptedStatus,
            entry->mutation
                ? ColorHelper::FromArgb(38, 255, 170, 0)
                : ColorHelper::FromArgb(28, 220, 64, 64));
    }
    state.approvals.clear();
    state.approvalByToolCallId.clear();
    state.toolCards.clear();
    state.cancelRequested = false;
}

std::wstring FormatAssistantDuration(double milliseconds)
{
    std::wostringstream text;
    if (milliseconds >= 1000.0)
    {
        text << std::fixed << std::setprecision(1)
             << milliseconds / 1000.0 << L" s";
    }
    else
    {
        text << static_cast<unsigned long long>(
            (std::max)(milliseconds, 0.0) + 0.5) << L" ms";
    }
    return text.str();
}

std::wstring FormatAssistantTurnTiming(const cld::JsonValue& payload)
{
    const double total = cld::JsonNumberOr(
        payload, "totalMilliseconds", 0.0);
    if (total <= 0.0)
    {
        return {};
    }

    const double runtime = cld::JsonNumberOr(
        payload, "runtimeSetupMilliseconds", 0.0);
    const double initial = cld::JsonNumberOr(
        payload, "initialInferenceMilliseconds", 0.0);
    const double continuation = cld::JsonNumberOr(
        payload, "continuationInferenceMilliseconds", 0.0);
    const double tools = cld::JsonNumberOr(
        payload, "toolMilliseconds", 0.0);

    std::wstring summary = L"Last AI turn: " +
        FormatAssistantDuration(total) + L" total · runtime " +
        FormatAssistantDuration(runtime) + L" · model " +
        FormatAssistantDuration(initial + continuation);
    if (continuation > 0.0)
    {
        summary += L" (initial " + FormatAssistantDuration(initial) +
            L" + after tools " + FormatAssistantDuration(continuation) + L")";
    }
    if (tools > 0.0)
    {
        summary += L" · tools " + FormatAssistantDuration(tools);
    }
    return summary;
}

std::shared_ptr<AssistantToolUiEntry> EnsureAssistantToolCard(
    AssistantUiState& state,
    ListView const& transcript,
    std::string const& turnId,
    std::string const& toolCallId,
    std::string const& toolName,
    bool mutation)
{
    if (const auto found = state.toolCards.find(toolCallId);
        found != state.toolCards.end())
    {
        if (!found->second || found->second->turnId != turnId ||
            found->second->toolName != toolName)
        {
            return {};
        }
        found->second->mutation = found->second->mutation || mutation;
        return found->second;
    }

    Border card;
    card.Padding(Thickness{ 10, 8, 10, 8 });
    card.Margin(Thickness{ 0, 0, 0, 7 });
    card.CornerRadius(CornerRadius{ 6 });
    card.HorizontalAlignment(HorizontalAlignment::Stretch);
    card.Background(SolidColorBrush(
        ColorHelper::FromArgb(24, 0, 120, 215)));
    StackPanel panel;
    panel.Spacing(4);
    TextBlock heading;
    heading.Text(L"LookDev tool · " + Utf8ToWide(toolName));
    heading.Opacity(0.78);
    TextBlock status;
    status.Text(L"Preparing…");
    status.TextWrapping(TextWrapping::Wrap);
    status.IsTextSelectionEnabled(true);
    panel.Children().Append(heading);
    panel.Children().Append(status);
    card.Child(panel);
    transcript.Items().Append(card);
    transcript.ScrollIntoView(card);

    auto entry = std::make_shared<AssistantToolUiEntry>();
    entry->turnId = turnId;
    entry->toolName = toolName;
    entry->card = card;
    entry->status = status;
    entry->mutation = mutation;
    state.toolCards.emplace(toolCallId, entry);
    return entry;
}

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
         !state->pendingConversationRequestId.empty() ||
         !state->pendingResetRequestId.empty() ||
         !state->pendingExportRequestId.empty());
    const bool setupActive = state && state->modelSetupActive;
    const bool turnActive = state && state->turnActive;
    const bool idle = ready && !state->turnActive && !requestPending &&
        !setupActive;
    const bool canSend = idle && !state->activeConversationId.empty();
    AiConversationCombo().IsEnabled(idle);
    AiNewConversationButton().IsEnabled(idle);
    AiResetConversationButton().IsEnabled(canSend);
    AiSaveConversationButton().IsEnabled(canSend);
    AiSendButton().IsEnabled(canSend);
    AiModelCombo().IsEnabled(idle);
    AiBackendCombo().IsEnabled(idle);
    AiLicenseConsentCheck().IsEnabled(idle);
    AiSetupButton().IsEnabled(
        idle &&
        unbox_value_or<bool>(
            AiLicenseConsentCheck().IsChecked(), false));
    AiSetupCancelButton().Visibility(
        setupActive ? Visibility::Visible : Visibility::Collapsed);
    AiSetupCancelButton().IsEnabled(setupActive);
    AiThinkingPanel().Visibility(
        turnActive ? Visibility::Visible : Visibility::Collapsed);
    AiThinkingProgress().IsActive(turnActive);
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
    AiLoadedModelText().Text(L"Loaded model: waiting for runtime");
    AiModelStatusText().Text(L"Model: waiting for runtime");
    AiSetupProgress().IsIndeterminate(true);
    AiSetupProgress().Visibility(Visibility::Visible);
    AiSetupProgressText().Visibility(Visibility::Collapsed);
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
    FinishAssistantTurnUi(
        *state,
        m_controller.get(),
        L"Assistant stopped · approval closed.",
        L"Assistant stopped before the tool outcome was received.");
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

    // A server restart invalidates every legacy MCP session, even when it
    // returns on the same endpoint with the same bearer capability. Restart
    // the private ChatHost so it negotiates a fresh dedicated session and
    // catalog before another tool call can be attempted.
    if ((m_assistant->initialized || m_assistant->initializing) &&
        m_assistant->mcpGeneration != 0 &&
        (!snapshot.mcpRunning ||
         m_assistant->mcpGeneration != snapshot.mcpGeneration))
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
        AiSetupProgress().IsIndeterminate(true);
        AiSetupProgress().Visibility(Visibility::Visible);
        break;
    case State::WaitingForConnection:
        AiRuntimeStatusText().Text(L"Waiting for local AI host\u2026");
        AiSetupProgress().IsIndeterminate(true);
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
        FinishAssistantTurnUi(
            *m_assistant,
            m_controller.get(),
            L"Assistant disconnected · approval closed.",
            L"Assistant disconnected before the tool outcome was received.");
        m_assistant->connected = false;
        m_assistant->initializing = false;
        m_assistant->initialized = false;
        m_assistant->turnActive = false;
        m_assistant->activeTurnId.clear();
        m_assistant->pendingCreateRequestId.clear();
        m_assistant->pendingConversationId.clear();
        m_assistant->pendingConversationRequestId.clear();
        m_assistant->pendingResetRequestId.clear();
        m_assistant->pendingExportRequestId.clear();
        m_assistant->modelSetupRequestId.clear();
        m_assistant->modelSetupActive = false;
        AiSetupProgress().Visibility(Visibility::Collapsed);
        AiRuntimeStatusText().Text(L"Local AI host disconnected.");
        AiLoadedModelText().Text(L"Loaded model: unavailable");
        AiModelStatusText().Text(
            L"Retry the private local connection.");
        AiRetryButton().IsEnabled(true);
        AiSendButton().IsEnabled(false);
        AiStopButton().IsEnabled(false);
        UpdateAssistantInteractionState();
        break;
    case State::Stopped:
        FinishAssistantTurnUi(
            *m_assistant,
            m_controller.get(),
            L"Assistant stopped · approval closed.",
            L"Assistant stopped before the tool outcome was received.");
        m_assistant->connected = false;
        m_assistant->initializing = false;
        m_assistant->initialized = false;
        m_assistant->turnActive = false;
        m_assistant->activeTurnId.clear();
        m_assistant->pendingCreateRequestId.clear();
        m_assistant->pendingConversationId.clear();
        m_assistant->pendingConversationRequestId.clear();
        m_assistant->pendingResetRequestId.clear();
        m_assistant->pendingExportRequestId.clear();
        m_assistant->modelSetupRequestId.clear();
        m_assistant->modelSetupActive = false;
        AiSetupProgress().Visibility(Visibility::Collapsed);
        AiLoadedModelText().Text(L"Loaded model: unavailable");
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

    const auto snapshot = m_controller
        ? m_controller->LatestSnapshot()
        : nullptr;
    if (!snapshot || !snapshot->mcpRunning)
    {
        return;
    }
    const std::string mcpEndpoint = WideToUtf8(snapshot->mcpEndpoint);
    ScopedSecret mcpBearerToken{
        WideToUtf8(snapshot->mcpToken) };
    if (!IsPrivateLookDevMcpEndpoint(mcpEndpoint) ||
        mcpBearerToken.value.empty())
    {
        AiRuntimeStatusText().Text(
            L"The private LookDev MCP connection is not ready.");
        return;
    }
    state->mcpGeneration = snapshot->mcpGeneration;

    std::ostringstream payload;
    payload << "{"
        << "\"instanceId\":\""
        << cld::EscapeJson(state->instanceId) << "\","
        << "\"projectContextKey\":\""
        << cld::EscapeJson(state->projectContextKey) << "\","
        << "\"mcpEndpoint\":\""
        << cld::EscapeJson(mcpEndpoint) << "\","
        << "\"mcpBearerToken\":\""
        << cld::EscapeJson(mcpBearerToken.value) << "\"}"
        ;
    state->initializing = true;
    AiRuntimeStatusText().Text(L"Initializing local assistant\u2026");
    AiLoadedModelText().Text(L"Loaded model: none");
    AiModelStatusText().Text(L"Runtime: connecting to ChatHost");
    AiSetupProgress().IsIndeterminate(true);
    AiSetupProgress().Visibility(Visibility::Visible);
    const bool posted = PostAssistantRequest("initialize", payload.str());
    SecureClear(mcpBearerToken.value);
    if (!posted)
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
        if (method == "conversation.reset" &&
            (state->pendingResetRequestId.empty() ||
             event.requestId != state->pendingResetRequestId))
        {
            return;
        }
        if (method == "conversation.exportMarkdown" &&
            (state->pendingExportRequestId.empty() ||
             event.requestId != state->pendingExportRequestId))
        {
            return;
        }
        if (method == "modelSetup.start" &&
            (state->modelSetupRequestId.empty() ||
             event.requestId != state->modelSetupRequestId))
        {
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
                FinishAssistantTurnUi(
                    *state,
                    m_controller.get(),
                    L"Turn rejected · approval closed.",
                    L"The turn was rejected before the tool outcome was received.");
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
            if (method == "conversation.reset")
            {
                state->pendingResetRequestId.clear();
                AiRuntimeStatusText().Text(L"Chat history could not be reset.");
            }
            if (method == "conversation.exportMarkdown")
            {
                state->pendingExportRequestId.clear();
                AiRuntimeStatusText().Text(L"The Markdown chat could not be saved.");
            }
            if (method == "modelSetup.start")
            {
                state->modelSetupRequestId.clear();
                state->modelSetupActive = false;
                AiSetupProgress().IsIndeterminate(false);
                AiSetupProgress().Value(0.0);
                AiSetupProgress().Visibility(Visibility::Visible);
                AiSetupProgressText().Text(code + L": " + message);
                AiSetupProgressText().Visibility(Visibility::Visible);
                AiRuntimeStatusText().Text(
                    L"Local model setup could not be started.");
            }
            else
            {
                AiSetupProgress().Visibility(Visibility::Collapsed);
            }
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

        if (method == "conversation.reset")
        {
            state->pendingResetRequestId.clear();
            const cld::JsonValue* conversation =
                ObjectMember(*payload, "conversation");
            const std::string id = conversation
                ? cld::JsonStringOr(*conversation, "id")
                : std::string{};
            if (id.empty() || id != state->activeConversationId)
            {
                AddTranscriptCard(
                    AiTranscriptList(),
                    L"Error",
                    L"The reset conversation response was invalid.",
                    true);
                UpdateAssistantInteractionState();
                return;
            }
            const auto found = std::find(
                state->conversationIds.begin(),
                state->conversationIds.end(),
                id);
            if (found != state->conversationIds.end())
            {
                const auto index = static_cast<std::uint32_t>(
                    std::distance(state->conversationIds.begin(), found));
                AiConversationCombo().Items().SetAt(
                    index,
                    box_value(to_hstring(cld::JsonStringOr(
                        *conversation, "title", "New chat"))));
            }
            AiTranscriptList().Items().Clear();
            state->messageBodies.clear();
            AiRuntimeStatusText().Text(L"Selected chat history was reset.");
            UpdateAssistantInteractionState();
            return;
        }

        if (method == "conversation.exportMarkdown")
        {
            state->pendingExportRequestId.clear();
            const auto messageCount = static_cast<unsigned long long>(
                (std::max)(0.0, cld::JsonNumberOr(
                    *payload, "messageCount", 0.0)));
            std::wostringstream status;
            status << L"Saved " << messageCount
                   << L" chat messages as Markdown.";
            AiRuntimeStatusText().Text(status.str());
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
                    auto body = AddTranscriptCard(
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

        if (method == "modelSetup.start")
        {
            return;
        }

        if (method == "modelSetup.cancel")
        {
            if (state->modelSetupActive)
            {
                AiSetupCancelButton().IsEnabled(false);
                AiSetupProgressText().Text(L"Cancellation requested\u2026");
                AiSetupProgressText().Visibility(Visibility::Visible);
            }
            return;
        }
    }

    if (kind != "event")
    {
        return;
    }
    if (method == "conversationUpdated")
    {
        const std::string turnId = cld::JsonStringOr(*payload, "turnId");
        const cld::JsonValue* conversation =
            ObjectMember(*payload, "conversation");
        if (!conversation ||
            state->activeTurnId.empty() ||
            turnId != state->activeTurnId)
        {
            return;
        }
        const std::string id = cld::JsonStringOr(*conversation, "id");
        if (id.empty() || id != state->activeConversationId)
        {
            return;
        }
        const auto found = std::find(
            state->conversationIds.begin(),
            state->conversationIds.end(),
            id);
        if (found != state->conversationIds.end())
        {
            const auto index = static_cast<std::uint32_t>(
                std::distance(state->conversationIds.begin(), found));
            AiConversationCombo().Items().SetAt(
                index,
                box_value(to_hstring(cld::JsonStringOr(
                    *conversation, "title", "New chat"))));
        }
        return;
    }
    if (method == "modelSetupProgress")
    {
        if (!state->modelSetupActive ||
            state->modelSetupRequestId.empty() ||
            event.requestId != state->modelSetupRequestId)
        {
            return;
        }

        const double received = (std::max)(0.0,
            cld::JsonNumberOr(*payload, "overallBytesReceived", 0.0));
        const double total = (std::max)(0.0,
            cld::JsonNumberOr(*payload, "overallTotalBytes", 0.0));
        const double percent = std::clamp(
            cld::JsonNumberOr(*payload, "percent", 0.0), 0.0, 100.0);
        const bool terminal =
            cld::JsonBoolOr(*payload, "terminal", false);
        const bool succeeded =
            cld::JsonBoolOr(*payload, "succeeded", false);
        const std::string stage =
            cld::JsonStringOr(*payload, "stage");
        const std::wstring artifact = Utf8ToWide(
            cld::JsonStringOr(*payload, "artifact"));
        std::wstring message = Utf8ToWide(
            cld::JsonStringOr(
                *payload, "message", "Preparing local model setup..."));

        AiSetupProgress().IsIndeterminate(false);
        AiSetupProgress().Value(percent);
        AiSetupProgress().Visibility(Visibility::Visible);
        if (total > 0.0)
        {
            std::wostringstream detail;
            detail << message << L"  "
                << FormatSetupBytes(received) << L" / "
                << FormatSetupBytes(total) << L" ("
                << std::fixed << std::setprecision(1)
                << percent << L"%)";
            message = detail.str();
        }
        AiSetupProgressText().Text(message);
        AiSetupProgressText().Visibility(Visibility::Visible);
        if (!artifact.empty())
        {
            AiModelStatusText().Text(L"Current item: " + artifact);
        }

        if (terminal)
        {
            state->modelSetupRequestId.clear();
            state->modelSetupActive = false;
            if (succeeded)
            {
                AiSetupProgress().Value(100.0);
                AiRuntimeStatusText().Text(
                    L"Local model setup completed.");
                AiLoadedModelText().Text(
                    L"Loaded model: will update on the next message");
                AiModelStatusText().Text(
                    L"The model will load when the next message is sent.");
            }
            else if (stage == "cancelled")
            {
                AiRuntimeStatusText().Text(
                    L"Local model setup was cancelled.");
            }
            else
            {
                AiRuntimeStatusText().Text(
                    L"Local model setup failed.");
            }
            UpdateAssistantInteractionState();
        }
        else
        {
            AiRuntimeStatusText().Text(
                L"Downloading and verifying local AI files\u2026");
        }
        return;
    }
    if (method == "runtimeState")
    {
        const std::string runtimeStatus =
            cld::JsonStringOr(*payload, "status", "ready");
        const std::string backend =
            cld::JsonStringOr(*payload, "backend", "local");
        const std::wstring modelId = Utf8ToWide(
            cld::JsonStringOr(*payload, "modelId"));
        const std::wstring modelDisplayName = Utf8ToWide(
            cld::JsonStringOr(*payload, "modelDisplayName"));
        const std::wstring modelName = !modelDisplayName.empty()
            ? modelDisplayName
            : modelId;
        AiRuntimeStatusText().Text(
            runtimeStatus == "ready"
                ? L"Local assistant is ready"
                : L"Assistant: " + Utf8ToWide(runtimeStatus));
        AiLoadedModelText().Text(
            modelName.empty()
                ? L"Loaded model: not reported"
                : L"Loaded model: " + modelName);
        AiModelStatusText().Text(
            L"Runtime backend: " + Utf8ToWide(backend));
        if (state->turnActive)
        {
            AiThinkingStatusText().Text(
                modelName.empty()
                    ? L"AI Assistant is thinking\u2026"
                    : modelName + L" is thinking\u2026");
        }
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
            SetTranscriptContent(*found->second, content);
        }
        else
        {
            auto body = AddTranscriptCard(
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
        AiThinkingStatusText().Text(L"Generating response\u2026");
        auto found = state->messageBodies.find(id);
        if (found == state->messageBodies.end())
        {
            auto body = AddTranscriptCard(
                AiTranscriptList(), L"AI Assistant", delta);
            if (!id.empty())
            {
                state->messageBodies.insert_or_assign(id, body);
            }
        }
        else
        {
            AppendTranscriptContent(*found->second, delta);
            AiTranscriptList().ScrollIntoView(found->second->card);
        }
        return;
    }
    if (method == "toolStarted")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        const std::string toolCallId =
            cld::JsonStringOr(*payload, "toolCallId");
        const std::string toolName =
            cld::JsonStringOr(*payload, "tool");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId ||
            !IsSafeAssistantProtocolText(
                toolCallId, MaximumAssistantToolCallIdBytes) ||
            !IsSafeAssistantProtocolText(
                toolName, MaximumAssistantToolNameBytes))
        {
            return;
        }

        bool mutation = false;
        if (const auto bindingId =
                state->approvalByToolCallId.find(toolCallId);
            bindingId != state->approvalByToolCallId.end())
        {
            const auto approval = state->approvals.find(bindingId->second);
            if (approval == state->approvals.end() || !approval->second ||
                approval->second->turnId != turnId ||
                approval->second->toolName != toolName ||
                !approval->second->allowed)
            {
                return;
            }
            mutation = true;
            approval->second->mutationStarted = true;
            CloseAssistantApproval(
                *approval->second,
                L"Allowed once · LookDev change is running.");
        }

        auto entry = EnsureAssistantToolCard(
            *state,
            AiTranscriptList(),
            turnId,
            toolCallId,
            toolName,
            mutation);
        if (!entry || entry->terminal)
        {
            return;
        }
        entry->started = true;
        AiThinkingStatusText().Text(
            L"Using LookDev tool: " + Utf8ToWide(toolName) + L"\u2026");
        if (state->cancelRequested)
        {
            SetAssistantToolStatus(
                *entry,
                mutation
                    ? L"Cancellation requested · result may be unknown. Verify the LookDev state."
                    : L"Cancellation requested · waiting for the tool outcome.",
                mutation
                    ? ColorHelper::FromArgb(38, 255, 170, 0)
                    : ColorHelper::FromArgb(24, 128, 128, 128));
        }
        else
        {
            SetAssistantToolStatus(
                *entry,
                mutation
                    ? L"Running approved LookDev change…"
                    : L"Reading the current LookDev state…",
                ColorHelper::FromArgb(24, 0, 120, 215));
        }
        return;
    }
    if (method == "toolCompleted")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        const std::string toolCallId =
            cld::JsonStringOr(*payload, "toolCallId");
        const std::string toolName =
            cld::JsonStringOr(*payload, "tool");
        const std::string status =
            cld::JsonStringOr(*payload, "status");
        const bool isError =
            cld::JsonBoolOr(*payload, "isError", false);
        const std::string code =
            cld::JsonStringOr(*payload, "code");
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId ||
            !IsSafeAssistantProtocolText(
                toolCallId, MaximumAssistantToolCallIdBytes) ||
            !IsSafeAssistantProtocolText(
                toolName, MaximumAssistantToolNameBytes) ||
            !IsToolCompletionStatus(status))
        {
            return;
        }
        AiThinkingStatusText().Text(L"Thinking about the tool result\u2026");
        if ((status == "succeeded" && (isError || !code.empty())) ||
            (status != "succeeded" && !isError) ||
            (status == "denied" && code != "user_denied") ||
            (status == "unknown" && code != "cancelled_after_start") ||
            (status == "failed" &&
                !IsSafeAssistantProtocolText(
                    code, MaximumAssistantToolCodeBytes)))
        {
            return;
        }

        std::shared_ptr<AssistantApprovalUiEntry> approval;
        if (const auto bindingId =
                state->approvalByToolCallId.find(toolCallId);
            bindingId != state->approvalByToolCallId.end())
        {
            if (const auto found = state->approvals.find(bindingId->second);
                found != state->approvals.end())
            {
                approval = found->second;
            }
            if (!approval || approval->turnId != turnId ||
                approval->toolName != toolName)
            {
                return;
            }
        }

        auto entry = EnsureAssistantToolCard(
            *state,
            AiTranscriptList(),
            turnId,
            toolCallId,
            toolName,
            approval != nullptr);
        if (!entry || entry->terminal ||
            ((status == "succeeded" || status == "unknown") &&
                !entry->started))
        {
            return;
        }
        entry->terminal = true;
        if (approval)
        {
            approval->terminal = true;
        }
        if (status == "succeeded")
        {
            SetAssistantToolStatus(
                *entry,
                entry->mutation
                    ? L"Completed · LookDev change applied."
                    : L"Completed.",
                ColorHelper::FromArgb(28, 20, 150, 80));
            if (approval)
            {
                CloseAssistantApproval(
                    *approval, L"Allowed once · completed.");
            }
        }
        else if (status == "denied")
        {
            SetAssistantToolStatus(
                *entry,
                L"Denied · the LookDev change was not started.",
                ColorHelper::FromArgb(24, 128, 128, 128));
            if (approval)
            {
                CloseAssistantApproval(*approval, L"Denied.");
            }
        }
        else if (status == "unknown")
        {
            SetAssistantToolStatus(
                *entry,
                L"Result unknown · cancellation occurred after start. Verify the LookDev state.",
                ColorHelper::FromArgb(38, 255, 170, 0));
            if (approval)
            {
                CloseAssistantApproval(
                    *approval,
                    L"Allowed once · result unknown after cancellation.");
            }
        }
        else if (entry->mutation && entry->started)
        {
            SetAssistantToolStatus(
                *entry,
                L"Result unknown · the call failed after this change started. Verify the LookDev state.",
                ColorHelper::FromArgb(38, 255, 170, 0));
            if (approval)
            {
                CloseAssistantApproval(
                    *approval,
                    L"Allowed once · result unknown after the call failed.");
            }
        }
        else
        {
            SetAssistantToolStatus(
                *entry,
                L"Failed · " + Utf8ToWide(code),
                ColorHelper::FromArgb(28, 220, 64, 64));
            if (approval)
            {
                CloseAssistantApproval(
                    *approval, L"Approved call failed · " + Utf8ToWide(code));
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
            for (auto const& [messageId, body] : state->messageBodies)
            {
                (void)messageId;
                if (body && body->renderPending)
                {
                    RenderTranscriptContent(*body);
                }
            }
            const std::string turnStatus =
                cld::JsonStringOr(*payload, "status", "completed");
            const std::wstring timing = FormatAssistantTurnTiming(*payload);
            if (!timing.empty())
            {
                AiRuntimeStatusText().Text(timing);
            }
            FinishAssistantTurnUi(
                *state,
                m_controller.get(),
                turnStatus == "cancelled"
                    ? L"Turn cancelled · approval closed."
                    : L"Turn ended · approval closed.",
                turnStatus == "cancelled"
                    ? L"Turn cancelled before the tool outcome was received."
                    : L"Turn ended before the tool outcome was received.");
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
        const std::wstring timing = FormatAssistantTurnTiming(*payload);
        if (!timing.empty())
        {
            AiRuntimeStatusText().Text(L"AI turn failed · " + timing);
        }
        FinishAssistantTurnUi(
            *state,
            m_controller.get(),
            L"Turn failed · approval closed.",
            L"The turn failed before the tool outcome was received.");
        state->turnActive = false;
        state->activeTurnId.clear();
        AiStopButton().IsEnabled(false);
        UpdateAssistantInteractionState();
        return;
    }
    if (method == "toolApprovalRequired")
    {
        const std::string turnId =
            cld::JsonStringOr(*payload, "turnId");
        const std::string approvalId =
            cld::JsonStringOr(*payload, "approvalId");
        const std::string toolCallId =
            cld::JsonStringOr(*payload, "toolCallId");
        const std::string toolName =
            cld::JsonStringOr(*payload, "tool");
        const std::string summaryText =
            cld::JsonStringOr(*payload, "summary");
        const std::string argumentsJson =
            cld::JsonStringOr(*payload, "argumentsJson");
        ScopedSecret mcpSessionId{
            cld::JsonStringOr(*payload, "mcpSessionId") };
        ScopedSecret argumentsHash{
            cld::JsonStringOr(*payload, "argumentsHash") };
        if (state->activeTurnId.empty() ||
            turnId != state->activeTurnId ||
            !IsSafeAssistantProtocolText(
                approvalId, MaximumAssistantApprovalIdBytes) ||
            !IsSafeAssistantProtocolText(
                toolCallId, MaximumAssistantToolCallIdBytes) ||
            !IsSafeAssistantProtocolText(
                toolName, MaximumAssistantToolNameBytes) ||
            !IsSafeAssistantProtocolText(
                summaryText, MaximumAssistantApprovalSummaryBytes) ||
            !IsSafeAssistantProtocolText(
                mcpSessionId.value, MaximumAssistantMcpSessionIdBytes) ||
            !ApprovalArgumentsMatchHash(
                argumentsJson, argumentsHash.value) ||
            state->approvals.contains(approvalId) ||
            state->approvalByToolCallId.contains(toolCallId))
        {
            return;
        }

        AiThinkingStatusText().Text(L"Waiting for your approval\u2026");

        const std::wstring tool = Utf8ToWide(toolName);
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
            Utf8ToWide(summaryText) +
            L"\nAllow once applies only to the exact arguments below and expires in 30 seconds.");
        detail.TextWrapping(TextWrapping::Wrap);
        TextBlock argumentsLabel;
        argumentsLabel.Text(L"Exact arguments");
        argumentsLabel.Opacity(0.72);
        TextBox arguments;
        arguments.Text(Utf8ToWide(argumentsJson));
        arguments.IsReadOnly(true);
        arguments.AcceptsReturn(true);
        arguments.TextWrapping(TextWrapping::Wrap);
        arguments.FontFamily(FontFamily{ L"Consolas" });
        arguments.IsSpellCheckEnabled(false);
        arguments.MaxHeight(180);
        ScrollViewer::SetHorizontalScrollBarVisibility(
            arguments, ScrollBarVisibility::Disabled);
        ScrollViewer::SetVerticalScrollBarVisibility(
            arguments, ScrollBarVisibility::Auto);
        TextBlock approvalStatus;
        approvalStatus.Text(L"Waiting for your decision.");
        approvalStatus.TextWrapping(TextWrapping::Wrap);
        approvalStatus.IsTextSelectionEnabled(true);
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(6);
        Button allow;
        allow.Content(box_value(L"Allow once"));
        Button deny;
        deny.Content(box_value(L"Deny"));

        auto binding = std::make_shared<AssistantApprovalBinding>();
        binding->approvalId = approvalId;
        binding->turnId = turnId;
        binding->toolName = toolName;
        binding->mcpSessionId = std::move(mcpSessionId.value);
        binding->argumentsHash = std::move(argumentsHash.value);
        auto entry = std::make_shared<AssistantApprovalUiEntry>();
        entry->turnId = turnId;
        entry->toolCallId = toolCallId;
        entry->toolName = toolName;
        entry->allow = allow;
        entry->deny = deny;
        entry->status = approvalStatus;
        entry->binding = std::move(binding);
        state->approvals.emplace(approvalId, entry);
        state->approvalByToolCallId.emplace(toolCallId, approvalId);

        const auto weakWindow = get_weak();
        const std::weak_ptr<AssistantUiState> weakState = state;
        const std::weak_ptr<AssistantApprovalUiEntry> weakApproval = entry;
        allow.Click([weakWindow, weakState, weakApproval](
            IInspectable const&, RoutedEventArgs const&)
        {
            auto session = weakState.lock();
            auto approval = weakApproval.lock();
            if (!session || !approval)
            {
                return;
            }
            const auto bindingId =
                session->approvalByToolCallId.find(approval->toolCallId);
            if (bindingId == session->approvalByToolCallId.end())
            {
                return;
            }
            const auto tracked = session->approvals.find(bindingId->second);
            if (tracked == session->approvals.end() ||
                tracked->second != approval)
            {
                return;
            }
            auto self = weakWindow.get();
            if (!self || self->m_assistant != session || self->m_closing ||
                !approval->pending || session->cancelRequested ||
                session->activeTurnId != approval->turnId ||
                !approval->binding)
            {
                return;
            }

            approval->pending = false;
            try
            {
                approval->allow.IsEnabled(false);
                approval->deny.IsEnabled(false);
                approval->status.Text(L"Issuing one-time approval…");
            }
            catch (...)
            {
                CloseAssistantApproval(
                    *approval, L"Approval controls are unavailable.");
                return;
            }

            try
            {
                ScopedSecret grant{
                    mcp::ApprovalGrantBroker::Instance().Issue(
                        approval->binding->mcpSessionId,
                        approval->binding->toolName,
                        approval->binding->argumentsHash) };
                const bool posted = self->PostAssistantRequest(
                    "approval.respond",
                    "{\"approvalId\":\"" +
                        cld::EscapeJson(approval->binding->approvalId) +
                        "\",\"decision\":\"allowOnce\"," +
                        "\"approvalGrant\":\"" + grant.value + "\"}");
                approval->allowed = posted;
                SecureClear(grant.value);
                CloseAssistantApproval(
                    *approval,
                    posted
                        ? L"Allowed once · waiting for the tool to start."
                        : L"Approval could not be sent.");
                if (!posted)
                {
                    AddTranscriptCard(
                        self->AiTranscriptList(), L"Approval",
                        L"The one-time approval could not be sent.", true);
                }
            }
            catch (...)
            {
                CloseAssistantApproval(
                    *approval, L"Approval could not be issued.");
                AddTranscriptCard(
                    self->AiTranscriptList(), L"Approval",
                    L"The one-time approval could not be issued.", true);
            }
        });
        deny.Click([weakWindow, weakState, weakApproval](
            IInspectable const&, RoutedEventArgs const&)
        {
            auto session = weakState.lock();
            auto approval = weakApproval.lock();
            if (!session || !approval)
            {
                return;
            }
            const auto bindingId =
                session->approvalByToolCallId.find(approval->toolCallId);
            if (bindingId == session->approvalByToolCallId.end())
            {
                return;
            }
            const auto tracked = session->approvals.find(bindingId->second);
            if (tracked == session->approvals.end() ||
                tracked->second != approval)
            {
                return;
            }
            auto self = weakWindow.get();
            if (!self || self->m_assistant != session || self->m_closing ||
                !approval->pending ||
                session->activeTurnId != approval->turnId ||
                !approval->binding)
            {
                return;
            }

            approval->pending = false;
            try
            {
                approval->allow.IsEnabled(false);
                approval->deny.IsEnabled(false);
                approval->status.Text(L"Denying…");
            }
            catch (...)
            {
                CloseAssistantApproval(
                    *approval, L"Approval controls are unavailable.");
                return;
            }
            const bool posted = self->PostAssistantRequest(
                "approval.respond",
                "{\"approvalId\":\"" +
                    cld::EscapeJson(approval->binding->approvalId) +
                    "\",\"decision\":\"deny\"}");
            CloseAssistantApproval(
                *approval,
                posted ? L"Denied." : L"Denial could not be sent.");
        });
        actions.Children().Append(allow);
        actions.Children().Append(deny);
        panel.Children().Append(heading);
        panel.Children().Append(detail);
        panel.Children().Append(argumentsLabel);
        panel.Children().Append(arguments);
        panel.Children().Append(approvalStatus);
        panel.Children().Append(actions);
        card.Child(panel);
        AiTranscriptList().Items().Append(card);
        AiTranscriptList().ScrollIntoView(card);

        if (state->cancelRequested)
        {
            entry->pending = false;
            const bool posted = PostAssistantRequest(
                "approval.respond",
                "{\"approvalId\":\"" +
                    cld::EscapeJson(entry->binding->approvalId) +
                    "\",\"decision\":\"deny\"}");
            CloseAssistantApproval(
                *entry,
                posted
                    ? L"Cancellation requested · denied automatically."
                    : L"Cancellation requested · approval closed.");
        }
        return;
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
    state->cancelRequested = false;
    if (m_controller)
    {
        m_controller->SetAssistantInferenceActive(true);
    }
    AiThinkingStatusText().Text(L"Starting the local model\u2026");
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
    MarkAssistantCancellationRequested(*m_assistant);
    if (PostAssistantRequest(
            "cancelTurn",
            "{\"turnId\":\"" +
                cld::EscapeJson(m_assistant->activeTurnId) +
                "\"}"))
    {
        AiStopButton().IsEnabled(false);
        AiRuntimeStatusText().Text(L"Stopping the current turn\u2026");
    }
    else
    {
        AddTranscriptCard(
            AiTranscriptList(),
            L"Error",
            L"The cancellation request could not be sent. Pending approvals remain closed.",
            true);
    }
}

void MainWindow::OnAiQuickPromptClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const std::wstring identifier = Tag(sender);
    SendAssistantTurn(QuickPrompt(identifier), identifier);
}

void MainWindow::OnAiSendAcceleratorInvoked(
    IInspectable const&,
    KeyboardAcceleratorInvokedEventArgs const& args)
{
    args.Handled(true);
    if (!AiSendButton().IsEnabled())
    {
        return;
    }
    SendAssistantTurn(AiPromptBox().Text().c_str());
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

void MainWindow::OnAiResetConversationClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    ConfirmResetAssistantConversation();
}

void MainWindow::OnAiSaveConversationClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    SaveAssistantConversationMarkdown();
}

IAsyncAction MainWindow::ConfirmResetAssistantConversation()
{
    auto lifetime = get_strong();
    auto state = m_assistant;
    if (!state || !state->initialized || state->turnActive ||
        state->activeConversationId.empty() ||
        !state->pendingResetRequestId.empty() ||
        !state->pendingExportRequestId.empty())
    {
        co_return;
    }
    const std::string conversationId = state->activeConversationId;

    ContentDialog dialog;
    dialog.XamlRoot(EditorRoot().XamlRoot());
    dialog.Title(box_value(L"Reset this chat history?"));
    dialog.Content(box_value(
        L"All messages in the selected chat will be permanently deleted. "
        L"The chat itself will remain available as a new empty chat."));
    dialog.PrimaryButtonText(L"Reset History");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
    {
        co_return;
    }
    if (m_closing || m_assistant != state || state->turnActive ||
        state->activeConversationId != conversationId)
    {
        co_return;
    }

    std::string requestId;
    if (PostAssistantRequest(
            "conversation.reset",
            "{\"conversationId\":\"" +
                cld::EscapeJson(conversationId) + "\"}",
            &requestId))
    {
        state->pendingResetRequestId = std::move(requestId);
        AiRuntimeStatusText().Text(L"Resetting the selected chat history…");
        UpdateAssistantInteractionState();
    }
}

IAsyncAction MainWindow::SaveAssistantConversationMarkdown()
{
    auto lifetime = get_strong();
    auto state = m_assistant;
    if (!state || !state->initialized || state->turnActive ||
        state->activeConversationId.empty() ||
        !state->pendingResetRequestId.empty() ||
        !state->pendingExportRequestId.empty())
    {
        co_return;
    }
    const std::string conversationId = state->activeConversationId;
    const hstring selectedTitle = unbox_value_or<hstring>(
        AiConversationCombo().SelectedItem(), L"lookdev-chat");

    FileSavePicker picker;
    check_hresult(
        picker.as<::IInitializeWithWindow>()->Initialize(
            WindowHandle()));
    picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
    picker.FileTypeChoices().Insert(
        L"Markdown",
        single_threaded_vector<hstring>({ L".md" }));
    picker.DefaultFileExtension(L".md");
    picker.SuggestedFileName(
        AssistantMarkdownFileName(selectedTitle.c_str()));
    StorageFile file = co_await picker.PickSaveFileAsync();
    if (!file || m_closing || m_assistant != state || state->turnActive ||
        state->activeConversationId != conversationId)
    {
        co_return;
    }
    const std::string path = WideToUtf8(file.Path().c_str());
    if (path.empty())
    {
        AiRuntimeStatusText().Text(L"The selected Markdown path is unavailable.");
        co_return;
    }

    std::string requestId;
    if (PostAssistantRequest(
            "conversation.exportMarkdown",
            "{\"conversationId\":\"" +
                cld::EscapeJson(conversationId) +
                "\",\"path\":\"" + cld::EscapeJson(path) + "\"}",
            &requestId))
    {
        state->pendingExportRequestId = std::move(requestId);
        AiRuntimeStatusText().Text(L"Saving the selected chat as Markdown…");
        UpdateAssistantInteractionState();
    }
}

void MainWindow::OnAiSetupClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    auto state = m_assistant;
    if (!state || !state->connected || !state->initialized ||
        state->modelSetupActive ||
        !unbox_value_or<bool>(
            AiLicenseConsentCheck().IsChecked(), false))
    {
        return;
    }

    const std::wstring modelId = SelectedComboTag(AiModelCombo());
    const std::wstring backend = SelectedComboTag(AiBackendCombo());
    if (modelId.empty() || backend.empty())
    {
        AiRuntimeStatusText().Text(
            L"Select a model and inference backend first.");
        return;
    }
    if (const auto flyout = AiSetupFlyoutButton().Flyout())
    {
        flyout.Hide();
    }

    state->modelSetupActive = true;
    AiSetupProgress().IsIndeterminate(false);
    AiSetupProgress().Value(0.0);
    AiSetupProgress().Visibility(Visibility::Visible);
    AiSetupProgressText().Text(L"Preparing download\u2026");
    AiSetupProgressText().Visibility(Visibility::Visible);
    AiRuntimeStatusText().Text(L"Preparing local model setup\u2026");
    UpdateAssistantInteractionState();

    const std::string payload =
        "{\"modelId\":\"" +
        cld::EscapeJson(WideToUtf8(modelId)) +
        "\",\"backend\":\"" +
        cld::EscapeJson(WideToUtf8(backend)) +
        "\",\"licenseAccepted\":true}";
    std::string requestId;
    if (PostAssistantRequest(
            "modelSetup.start", payload, &requestId))
    {
        state->modelSetupRequestId = std::move(requestId);
        return;
    }

    state->modelSetupActive = false;
    state->modelSetupRequestId.clear();
    AiSetupProgressText().Text(
        L"The setup request could not be queued.");
    UpdateAssistantInteractionState();
}

void MainWindow::OnAiSetupCancelClick(
    IInspectable const&,
    RoutedEventArgs const&)
{
    const auto state = m_assistant;
    if (!state || !state->modelSetupActive)
    {
        return;
    }
    if (PostAssistantRequest("modelSetup.cancel", "{}"))
    {
        AiSetupCancelButton().IsEnabled(false);
        AiSetupProgressText().Text(L"Cancellation requested\u2026");
        AiSetupProgressText().Visibility(Visibility::Visible);
    }
}

void MainWindow::OnAiLicenseClick(
    IInspectable const& sender,
    RoutedEventArgs const&)
{
    const auto link = sender.try_as<HyperlinkButton>();
    const std::wstring license = link
        ? unbox_value_or<hstring>(link.Tag(), L"").c_str()
        : std::wstring{};
    const wchar_t* url = license == L"llama.cpp"
        ? L"https://github.com/ggml-org/llama.cpp/blob/b10205/LICENSE"
        : L"https://ai.google.dev/gemma/apache_2";
    (void)Launcher::LaunchUriAsync(
        Uri{url});
}

void MainWindow::OnAiLicenseConsentChanged(
    IInspectable const&,
    RoutedEventArgs const&)
{
    UpdateAssistantInteractionState();
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
