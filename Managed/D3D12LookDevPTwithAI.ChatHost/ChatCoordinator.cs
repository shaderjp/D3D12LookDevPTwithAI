using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.ChatHost.Inference;
using D3D12LookDevPTwithAI.ChatHost.Mcp;

namespace D3D12LookDevPTwithAI.ChatHost;

public sealed class ChatCoordinator(
    IConversationStore conversationStore,
    IChatInferenceRuntime inferenceRuntime,
    ISameInstanceMcpClientFactory? mcpClientFactory = null)
{
    private static readonly UTF8Encoding StrictUtf8 = new(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);
    private const int InferenceHistoryMessageLimit = 64;
    private const int MaximumToolRoundsPerTurn = 4;
    private const int MaximumToolCallsPerRound = 4;
    private const int MaximumToolCallsPerTurn = 8;
    private const int MaximumMutationApprovalsPerTurn = 4;
    private const int MaximumToolArgumentsBytes = 16 * 1024;
    private const int MaximumToolArgumentsPerTurnBytes = 64 * 1024;
    private const int MaximumApprovalArgumentsBytes = 8 * 1024;
    private const int MaximumToolResultBytes = 32 * 1024;
    private const int MaximumToolResultsPerTurnBytes = 96 * 1024;
    private const int MaximumToolRoundTextCharacters = 32 * 1024;
    private const int MaximumToolRoundTextPerTurnCharacters = 64 * 1024;
    private const int MaximumAutomaticTitleCharacters = 48;
    private const string DefaultConversationTitle = "新しいチャット";
    private const string DirectDiagnosticsToolName = "lookdevpt.get_diagnostics";
    private static readonly TimeSpan CancelledPartialPersistenceTimeout =
        TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan TerminalEventWriteTimeout =
        TimeSpan.FromMilliseconds(250);
    private readonly object _gate = new();
    private readonly ConcurrentDictionary<Guid, PendingApproval> _approvals = new();
    private ActiveTurn? _activeTurn;
    private Guid? _activeConversationId;
    private string? _instanceId;
    private string? _projectContextKey;
    private ISameInstanceMcpClient? _mcpClient;
    private IReadOnlyDictionary<string, SameInstanceMcpTool> _mcpTools =
        new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
    private IReadOnlyList<ChatInferenceToolDefinition> _inferenceTools = [];

    public async Task<InitializeResult> InitializeAsync(
        InitializeRequest request,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(request.InstanceId) || request.InstanceId.Length > 128)
            throw new ChatRequestException("invalid_initialize", "instanceId is required and must not exceed 128 characters.");
        if (string.IsNullOrWhiteSpace(request.ProjectContextKey) || request.ProjectContextKey.Length > 256)
            throw new ChatRequestException("invalid_initialize", "projectContextKey is required and must not exceed 256 characters.");

        lock (_gate)
        {
            if (_instanceId is not null)
                throw new ChatRequestException("already_initialized", "ChatHost is already initialized.");
            _instanceId = request.InstanceId;
            _projectContextKey = request.ProjectContextKey;
        }

        ISameInstanceMcpClient? candidateMcpClient = null;
        IReadOnlyDictionary<string, SameInstanceMcpTool> candidateMcpTools =
            new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
        IReadOnlyList<ChatInferenceToolDefinition> candidateInferenceTools = [];
        try
        {
            if (mcpClientFactory is not null)
            {
                try
                {
                    if (string.IsNullOrWhiteSpace(request.McpEndpoint) ||
                        string.IsNullOrWhiteSpace(request.McpBearerToken))
                    {
                        throw new InvalidOperationException("Missing private MCP capability.");
                    }
                    candidateMcpClient = mcpClientFactory.Create(
                        request.McpEndpoint,
                        request.McpBearerToken);
                    var tools = await candidateMcpClient.GetToolsAsync(cancellationToken).ConfigureAwait(false);
                    (candidateMcpTools, candidateInferenceTools) = BuildToolCatalog(tools);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch
                {
                    if (candidateMcpClient is not null)
                    {
                        await DisposeMcpClientQuietlyAsync(candidateMcpClient).ConfigureAwait(false);
                        candidateMcpClient = null;
                    }
                    throw new ChatRequestException(
                        "mcp_initialization_failed",
                        "The private LookDev tool connection could not be initialized.",
                        retryable: true);
                }
            }
            else if (request.McpEndpoint is not null || request.McpBearerToken is not null)
            {
                throw new ChatRequestException(
                    "mcp_initialization_failed",
                    "The private LookDev tool connection could not be initialized.",
                    retryable: true);
            }

            await conversationStore.InitializeAsync(cancellationToken).ConfigureAwait(false);
            IReadOnlyList<ConversationSummary> conversations =
                await conversationStore.ListAsync(request.ProjectContextKey, cancellationToken).ConfigureAwait(false);
            if (conversations.Count == 0)
            {
                var created = await conversationStore.CreateAsync(
                    request.ProjectContextKey,
                    DefaultConversationTitle,
                    cancellationToken).ConfigureAwait(false);
                conversations = [created];
            }
            else
            {
                conversations = await EnsureConversationTitlesAsync(
                    request.ProjectContextKey,
                    conversations,
                    cancellationToken).ConfigureAwait(false);
            }

            lock (_gate)
            {
                _activeConversationId = conversations[0].Id;
                _mcpClient = candidateMcpClient;
                _mcpTools = candidateMcpTools;
                _inferenceTools = candidateInferenceTools;
                candidateMcpClient = null;
            }
            return new InitializeResult(
                typeof(ChatCoordinator).Assembly.GetName().Version?.ToString() ?? "1.0.0",
                conversations[0].Id,
                conversations);
        }
        catch
        {
            if (candidateMcpClient is not null)
                await DisposeMcpClientQuietlyAsync(candidateMcpClient).ConfigureAwait(false);
            lock (_gate)
            {
                _instanceId = null;
                _projectContextKey = null;
                _activeConversationId = null;
                _mcpTools = new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
                _inferenceTools = [];
            }
            throw;
        }
    }

    public async Task<ConversationListResult> ListConversationsAsync(
        CancellationToken cancellationToken = default)
    {
        var (projectContextKey, activeConversationId) = GetInitializedState();
        var conversations = await conversationStore.ListAsync(projectContextKey, cancellationToken).ConfigureAwait(false);
        conversations = await EnsureConversationTitlesAsync(
            projectContextKey,
            conversations,
            cancellationToken).ConfigureAwait(false);
        return new ConversationListResult(activeConversationId, conversations);
    }

    public async Task<ConversationCreateResult> CreateConversationAsync(
        ConversationCreateRequest request,
        CancellationToken cancellationToken = default)
    {
        var (projectContextKey, _) = GetInitializedState();
        var title = string.IsNullOrWhiteSpace(request.Title) ? DefaultConversationTitle : request.Title.Trim();
        if (title.Length > PipeProtocol.MaximumConversationTitleCharacters)
            throw new ChatRequestException(
                "invalid_title",
                $"Conversation title must not exceed {PipeProtocol.MaximumConversationTitleCharacters} characters.");

        var conversation = await conversationStore.CreateAsync(
            projectContextKey,
            title,
            cancellationToken).ConfigureAwait(false);
        lock (_gate) _activeConversationId = conversation.Id;
        return new ConversationCreateResult(conversation);
    }

    public async Task<ConversationResetResult> ResetConversationAsync(
        ConversationResetRequest request,
        CancellationToken cancellationToken = default)
    {
        if (request.ConversationId == Guid.Empty)
            throw new ChatRequestException("invalid_conversation", "conversationId is required.");
        var (projectContextKey, activeConversationId) = GetInitializedState();
        if (activeConversationId != request.ConversationId)
            throw new ChatRequestException("conversation_not_selected", "Select the conversation before resetting it.");
        lock (_gate)
        {
            if (_activeTurn is not null)
                throw new ChatRequestException("turn_busy", "The active turn must finish before resetting history.", retryable: true);
        }

        try
        {
            var conversation = await conversationStore.ResetAsync(
                projectContextKey,
                request.ConversationId,
                DefaultConversationTitle,
                cancellationToken).ConfigureAwait(false);
            return new ConversationResetResult(conversation);
        }
        catch (KeyNotFoundException)
        {
            throw new ChatRequestException("conversation_not_found", "The requested conversation does not exist.");
        }
    }

    public async Task<ConversationExportMarkdownResult> ExportConversationMarkdownAsync(
        ConversationExportMarkdownRequest request,
        CancellationToken cancellationToken = default)
    {
        if (request.ConversationId == Guid.Empty)
            throw new ChatRequestException("invalid_conversation", "conversationId is required.");
        var (projectContextKey, activeConversationId) = GetInitializedState();
        if (activeConversationId != request.ConversationId)
            throw new ChatRequestException("conversation_not_selected", "Select the conversation before exporting it.");
        lock (_gate)
        {
            if (_activeTurn is not null)
                throw new ChatRequestException("turn_busy", "The active turn must finish before exporting history.", retryable: true);
        }

        string fullPath;
        try
        {
            if (string.IsNullOrWhiteSpace(request.Path) ||
                request.Path.Length > short.MaxValue ||
                !Path.IsPathFullyQualified(request.Path))
            {
                throw new ArgumentException();
            }
            fullPath = Path.GetFullPath(request.Path);
            if (!string.Equals(Path.GetExtension(fullPath), ".md", StringComparison.OrdinalIgnoreCase) ||
                string.IsNullOrWhiteSpace(Path.GetDirectoryName(fullPath)))
            {
                throw new ArgumentException();
            }
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            throw new ChatRequestException("invalid_export_path", "Choose a valid absolute Markdown file path.");
        }

        var conversation = await conversationStore.GetAsync(
            projectContextKey,
            request.ConversationId,
            cancellationToken).ConfigureAwait(false);
        if (conversation is null)
            throw new ChatRequestException("conversation_not_found", "The requested conversation does not exist.");
        var messages = await conversationStore.GetMessagesAsync(
            projectContextKey,
            request.ConversationId,
            cancellationToken).ConfigureAwait(false);
        var markdown = BuildConversationMarkdown(conversation, messages);
        try
        {
            await File.WriteAllTextAsync(
                fullPath,
                markdown,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            throw new ChatRequestException(
                "conversation_export_failed",
                "The Markdown file could not be saved.",
                retryable: true);
        }
        return new ConversationExportMarkdownResult(request.ConversationId, messages.Count);
    }

    public async Task<ConversationSelectResult> SelectConversationAsync(
        ConversationSelectRequest request,
        CancellationToken cancellationToken = default)
    {
        if (request.ConversationId == Guid.Empty)
            throw new ChatRequestException("invalid_conversation", "conversationId is required.");
        if (request.BeforeMessageSequence is <= 0 or > PipeProtocol.MaximumExactJsonInteger)
            throw new ChatRequestException(
                "invalid_history_cursor",
                $"beforeMessageSequence must be between 1 and {PipeProtocol.MaximumExactJsonInteger}.");
        var pageSize = request.PageSize ?? PipeProtocol.DefaultConversationPageSize;
        if (pageSize <= 0 || pageSize > PipeProtocol.MaximumConversationPageSize)
            throw new ChatRequestException(
                "invalid_page_size",
                $"pageSize must be between 1 and {PipeProtocol.MaximumConversationPageSize}.");

        var (projectContextKey, _) = GetInitializedState();
        var conversation = await conversationStore.GetAsync(
            projectContextKey,
            request.ConversationId,
            cancellationToken).ConfigureAwait(false);
        if (conversation is null)
            throw new ChatRequestException("conversation_not_found", "The requested conversation does not exist.");

        var candidates = await conversationStore.ListMessagesBeforeAsync(
            projectContextKey,
            request.ConversationId,
            request.BeforeMessageSequence,
            checked(pageSize + 1),
            cancellationToken).ConfigureAwait(false);
        var page = BuildConversationPage(conversation, candidates, pageSize);
        lock (_gate) _activeConversationId = conversation.Id;
        return page;
    }

    public async Task<PreparedTurn> PrepareTurnAsync(
        SendTurnRequest request,
        Guid requestId,
        IPipePeer peer,
        CancellationToken hostCancellationToken)
    {
        ArgumentNullException.ThrowIfNull(peer);
        if (request.TurnId == Guid.Empty)
            throw new ChatRequestException("invalid_turn", "turnId is required.");
        if (string.IsNullOrWhiteSpace(request.Text))
            throw new ChatRequestException("invalid_turn", "Turn text is required.");
        if (request.Text.Length > ChatInferenceLimits.MaximumInputCharacters)
            throw new ChatRequestException(
                "invalid_turn",
                $"Turn text must not exceed {ChatInferenceLimits.MaximumInputCharacters} characters.");

        var (projectContextKey, activeConversationId) = GetInitializedState();
        if (activeConversationId != request.ConversationId)
            throw new ChatRequestException("conversation_not_selected", "Select the conversation before sending a turn.");

        lock (_gate)
        {
            if (_activeTurn is not null)
                throw new ChatRequestException("turn_busy", "Another turn is already active.", retryable: true);
        }

        var conversation = await conversationStore.GetAsync(
            projectContextKey,
            request.ConversationId,
            hostCancellationToken).ConfigureAwait(false);
        if (conversation is null)
            throw new ChatRequestException("conversation_not_found", "The requested conversation does not exist.");

        lock (_gate)
        {
            if (_activeTurn is not null)
                throw new ChatRequestException("turn_busy", "Another turn is already active.", retryable: true);
            if (_activeConversationId != request.ConversationId)
                throw new ChatRequestException("conversation_not_selected", "Select the conversation before sending a turn.");

            var turnMcpClient = _mcpClient;
            var turnMcpTools = _mcpTools;
            var turnInferenceTools = _inferenceTools;
            var cancellation = CancellationTokenSource.CreateLinkedTokenSource(hostCancellationToken);
            var activeTurn = new ActiveTurn(
                request.TurnId,
                cancellation,
                turnCancellationToken => RunInferenceTurnAsync(
                    projectContextKey,
                    conversation,
                    request,
                    requestId,
                    peer,
                    turnMcpClient,
                    turnMcpTools,
                    turnInferenceTools,
                    turnCancellationToken),
                () => SendWithoutTurnCancellationAsync(
                    peer,
                    requestId,
                    "completed",
                    new TurnCompletedEvent(request.TurnId, "cancelled")),
                completedTurn =>
                {
                    lock (_gate)
                    {
                        if (ReferenceEquals(_activeTurn, completedTurn)) _activeTurn = null;
                    }
                });
            _activeTurn = activeTurn;
            return new PreparedTurn(
                start: activeTurn.Start,
                abort: () =>
                {
                    lock (_gate)
                    {
                        if (ReferenceEquals(_activeTurn, activeTurn)) _activeTurn = null;
                    }
                    activeTurn.Abort();
                });
        }
    }

    public PreparedCancelTurn PrepareCancelTurn(CancelTurnRequest request)
    {
        _ = GetInitializedState();
        ActiveTurn? active;
        lock (_gate) active = _activeTurn?.TurnId == request.TurnId ? _activeTurn : null;
        Func<bool> commit = active is null
            ? static () => false
            : active.TryCancel;
        return new PreparedCancelTurn(
            new CancelTurnResult(request.TurnId, active is not null),
            commit);
    }

    public async Task<ApprovalResolution> WaitForApprovalAsync(
        Guid approvalId,
        CancellationToken cancellationToken = default)
    {
        if (approvalId == Guid.Empty)
            throw new ArgumentException("approvalId is required.", nameof(approvalId));
        var pending = new PendingApproval();
        if (!_approvals.TryAdd(approvalId, pending))
            throw new InvalidOperationException("The approval identifier is already pending.");
        try
        {
            return await pending.Completion.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _approvals.TryRemove(approvalId, out _);
        }
    }

    public PreparedApprovalResponse PrepareApprovalResponse(ApprovalRespondRequest request)
    {
        _ = GetInitializedState();
        if (request.ApprovalId == Guid.Empty)
            throw new ChatRequestException("invalid_approval", "approvalId is required.");
        var allowed = string.Equals(request.Decision, "allowOnce", StringComparison.Ordinal);
        var denied = string.Equals(request.Decision, "deny", StringComparison.Ordinal);
        if (!allowed && !denied)
            throw new ChatRequestException("invalid_approval", "decision must be allowOnce or deny.");
        if (allowed && !IsOneTimeApprovalGrant(request.ApprovalGrant))
            throw new ChatRequestException(
                "invalid_approval",
                "allowOnce requires a valid one-time approvalGrant.");
        if (!_approvals.TryGetValue(request.ApprovalId, out var pending) ||
            !pending.TryPrepare())
        {
            return PreparedApprovalResponse.NotAccepted(request.ApprovalId);
        }

        var resolution = new ApprovalResolution(
            allowed,
            allowed ? request.ApprovalGrant : null);
        return new PreparedApprovalResponse(
            new ApprovalRespondResult(request.ApprovalId, true),
            () => pending.Commit(resolution),
            pending.Abort);
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        ActiveTurn? active;
        ISameInstanceMcpClient? mcpClient;
        lock (_gate)
        {
            active = _activeTurn;
            mcpClient = _mcpClient;
            _mcpClient = null;
            _mcpTools = new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
            _inferenceTools = [];
        }
        active?.TryCancel();
        foreach (var approval in _approvals.ToArray())
        {
            if (_approvals.TryRemove(approval.Key, out var pending))
                pending.Stop();
        }
        if (active is not null)
        {
            try { await active.Completion.WaitAsync(cancellationToken).ConfigureAwait(false); }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested || active.IsCancellationRequested) { }
        }
        if (mcpClient is not null)
            await mcpClient.DisposeAsync().ConfigureAwait(false);
    }

    private async Task RunInferenceTurnAsync(
        string projectContextKey,
        ConversationSummary conversation,
        SendTurnRequest request,
        Guid requestId,
        IPipePeer peer,
        ISameInstanceMcpClient? mcpClient,
        IReadOnlyDictionary<string, SameInstanceMcpTool> mcpTools,
        IReadOnlyList<ChatInferenceToolDefinition> inferenceTools,
        CancellationToken cancellationToken)
    {
        await Task.Yield();
        var timings = new TurnTimingTracker();
        var assistantMessageId = Guid.NewGuid();
        var visibleResponse = new StringBuilder();
        var assistantStored = false;
        async Task EmitVisibleTextAsync(string text)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (visibleResponse.Length + text.Length > ChatInferenceLimits.MaximumOutputCharacters)
            {
                throw new ChatInferenceException(
                    "inference_output_too_large",
                    $"The local response exceeded {ChatInferenceLimits.MaximumOutputCharacters} characters.");
            }
            visibleResponse.Append(text);
            await peer.SendEventAsync(
                requestId,
                "textDelta",
                new TextDeltaEvent(request.TurnId, assistantMessageId, text),
                cancellationToken).ConfigureAwait(false);
        }
        try
        {
            var persistedHistory = await conversationStore.ListMessagesBeforeAsync(
                projectContextKey,
                request.ConversationId,
                beforeMessageSequence: null,
                InferenceHistoryMessageLimit,
                cancellationToken).ConfigureAwait(false);
            var inferenceHistory = BuildInferenceHistory(persistedHistory);
            var userText = request.Text.Trim();
            var userMessage = new ConversationMessage(
                Guid.NewGuid(),
                request.ConversationId,
                "user",
                userText,
                DateTimeOffset.UtcNow);
            await conversationStore.AppendMessageAsync(
                projectContextKey,
                userMessage,
                cancellationToken).ConfigureAwait(false);
            if (IsDefaultConversationTitle(conversation.Title) &&
                !persistedHistory.Any(message =>
                    string.Equals(message.Message.Role, "user", StringComparison.Ordinal)))
            {
                ConversationSummary? titledConversation = null;
                try
                {
                    titledConversation = await conversationStore.UpdateTitleAsync(
                        projectContextKey,
                        request.ConversationId,
                        BuildAutomaticConversationTitle(userText),
                        cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (Exception)
                {
                    // Title generation is a convenience; a failed metadata
                    // update must not discard an otherwise valid chat turn.
                }
                if (titledConversation is not null)
                {
                    await peer.SendEventAsync(
                        requestId,
                        "conversationUpdated",
                        new ConversationUpdatedEvent(request.TurnId, titledConversation),
                        cancellationToken).ConfigureAwait(false);
                }
            }
            await peer.SendEventAsync(
                requestId,
                "messageAdded",
                new MessageAddedEvent(request.TurnId, userMessage),
                cancellationToken).ConfigureAwait(false);
            ChatInferenceRuntimeStatus runtimeStatus;
            var runtimeSetupTimer = Stopwatch.StartNew();
            try
            {
                runtimeStatus = await inferenceRuntime.GetStatusAsync(
                    cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                timings.AddRuntimeSetup(runtimeSetupTimer.ElapsedMilliseconds);
            }
            if (!IsSafeRuntimeToken(
                    runtimeStatus.RuntimeId,
                    ChatInferenceLimits.MaximumRuntimeIdentifierCharacters) ||
                !IsSafeRuntimeToken(
                    runtimeStatus.State,
                    ChatInferenceLimits.MaximumRuntimeStateCharacters) ||
                (!string.IsNullOrEmpty(runtimeStatus.ModelId) &&
                 !IsSafeRuntimeToken(
                     runtimeStatus.ModelId,
                     ChatInferenceLimits.MaximumNameCharacters)) ||
                !IsSafeRuntimeDisplayName(runtimeStatus.ModelDisplayName))
            {
                throw new ChatInferenceException(
                    "invalid_inference_status",
                    "The local inference runtime returned an invalid status.");
            }
            if (!runtimeStatus.IsReady)
            {
                throw new ChatInferenceException(
                    "inference_runtime_not_ready",
                    "The local inference runtime is not ready.",
                    retryable: true);
            }
            await peer.SendEventAsync(
                requestId,
                "runtimeState",
                new RuntimeStateEvent(
                    runtimeStatus.State,
                    runtimeStatus.RuntimeId,
                    runtimeStatus.ModelId,
                    runtimeStatus.ModelDisplayName),
                cancellationToken).ConfigureAwait(false);

            var toolsAvailable = mcpClient is not null &&
                mcpTools.Count != 0 &&
                inferenceTools.Count != 0;
            var offeredTools = toolsAvailable ? inferenceTools : null;
            var allowToolCalls = toolsAvailable;
            var continuationHistory = new List<ChatInferenceMessage>(inferenceHistory.Count + 2);
            continuationHistory.AddRange(inferenceHistory);
            var removableHistoryMessages = continuationHistory.Count;
            continuationHistory.Add(new ChatInferenceMessage(ChatInferenceRole.User, userText));
            var toolState = new ToolTurnState();
            ChatInferenceRequest inferenceRequest;
            if (toolsAvailable &&
                mcpClient is not null &&
                mcpTools.TryGetValue(DirectDiagnosticsToolName, out var diagnosticsTool) &&
                diagnosticsTool.IsReadOnly &&
                RequestsExplicitDiagnosticsInvocation(userText))
            {
                var directCall = new ChatInferenceToolCall(
                    $"direct-{Guid.NewGuid():N}",
                    DirectDiagnosticsToolName,
                    "{}");
                var preparedCall = PrepareToolBatch([directCall], mcpTools, toolState).Single();
                toolState.ToolRounds = 1;
                toolState.ToolCalls = 1;
                continuationHistory.Add(new ChatInferenceMessage(
                    ChatInferenceRole.Assistant,
                    string.Empty,
                    ToolCalls: [directCall]));

                ChatInferenceMessage toolResult;
                var toolTimer = Stopwatch.StartNew();
                try
                {
                    toolResult = await ExecuteToolCallAsync(
                        request.TurnId,
                        requestId,
                        peer,
                        mcpClient,
                        preparedCall,
                        toolState,
                        cancellationToken).ConfigureAwait(false);
                }
                finally
                {
                    timings.AddTool(toolTimer.ElapsedMilliseconds);
                }
                continuationHistory.Add(toolResult);
                removableHistoryMessages = TrimContinuationHistory(
                    continuationHistory,
                    removableHistoryMessages);
                allowToolCalls = false;
                inferenceRequest = new ChatInferenceRequest(
                    request.ConversationId,
                    projectContextKey,
                    continuationHistory.ToArray(),
                    string.Empty,
                    offeredTools,
                    AppendUserMessage: false,
                    AllowToolCalls: false);
            }
            else
            {
                inferenceRequest = new ChatInferenceRequest(
                    request.ConversationId,
                    projectContextKey,
                    inferenceHistory,
                    userText,
                    offeredTools,
                    AllowToolCalls: allowToolCalls);
            }

            while (true)
            {
                InferenceRound round;
                var initialInference = toolState.ToolCalls == 0 && timings.InferenceRounds == 0;
                var inferenceTimer = Stopwatch.StartNew();
                try
                {
                    round = await ReadInferenceRoundAsync(
                        inferenceRequest,
                        !allowToolCalls ? EmitVisibleTextAsync : null,
                        cancellationToken).ConfigureAwait(false);
                }
                finally
                {
                    timings.AddInference(
                        initialInference,
                        inferenceTimer.ElapsedMilliseconds);
                }
                if (round.ToolCalls is null)
                {
                    if (round.TextChunks.Count == 0)
                    {
                        throw new ChatInferenceException(
                            "empty_inference_response",
                            "The local inference runtime returned no response.");
                    }
                    if (!round.TextWasEmitted)
                    {
                        foreach (var text in round.TextChunks)
                            await EmitVisibleTextAsync(text).ConfigureAwait(false);
                    }
                    break;
                }

                if (!allowToolCalls || offeredTools is null || mcpClient is null)
                {
                    throw new ChatInferenceException(
                        "invalid_tool_calls",
                        "The local inference runtime returned tool calls when tools were disabled.");
                }
                toolState.ToolRounds++;
                if (toolState.ToolRounds > MaximumToolRoundsPerTurn ||
                    round.ToolCalls.Count > MaximumToolCallsPerRound ||
                    toolState.ToolCalls + round.ToolCalls.Count > MaximumToolCallsPerTurn)
                {
                    throw new ChatInferenceException(
                        "tool_call_limit_exceeded",
                        "The local inference runtime exceeded the tool call limit.");
                }

                var preparedCalls = PrepareToolBatch(round.ToolCalls, mcpTools, toolState);
                toolState.ToolCalls += preparedCalls.Count;
                toolState.ToolRoundTextCharacters += round.Text.Length;
                if (round.Text.Length > MaximumToolRoundTextCharacters ||
                    toolState.ToolRoundTextCharacters > MaximumToolRoundTextPerTurnCharacters)
                {
                    throw new ChatInferenceException(
                        "invalid_tool_calls",
                        "The local inference tool-call prelude exceeded the supported size.");
                }
                continuationHistory.Add(new ChatInferenceMessage(
                    ChatInferenceRole.Assistant,
                    round.Text,
                    ToolCalls: round.ToolCalls));

                foreach (var preparedCall in preparedCalls)
                {
                    ChatInferenceMessage toolResult;
                    var toolTimer = Stopwatch.StartNew();
                    try
                    {
                        toolResult = await ExecuteToolCallAsync(
                            request.TurnId,
                            requestId,
                            peer,
                            mcpClient,
                            preparedCall,
                            toolState,
                            cancellationToken).ConfigureAwait(false);
                    }
                    finally
                    {
                        timings.AddTool(toolTimer.ElapsedMilliseconds);
                    }
                    continuationHistory.Add(toolResult);
                }

                removableHistoryMessages = TrimContinuationHistory(
                    continuationHistory,
                    removableHistoryMessages);
                allowToolCalls =
                    toolState.ToolRounds < MaximumToolRoundsPerTurn &&
                    toolState.ToolCalls < MaximumToolCallsPerTurn;
                inferenceRequest = new ChatInferenceRequest(
                    request.ConversationId,
                    projectContextKey,
                    continuationHistory.ToArray(),
                    string.Empty,
                    offeredTools,
                    AppendUserMessage: false,
                    AllowToolCalls: allowToolCalls);
            }

            var assistantMessage = new ConversationMessage(
                assistantMessageId,
                request.ConversationId,
                "assistant",
                visibleResponse.ToString(),
                DateTimeOffset.UtcNow);
            await conversationStore.AppendMessageAsync(
                projectContextKey,
                assistantMessage,
                cancellationToken).ConfigureAwait(false);
            assistantStored = true;
            await peer.SendEventAsync(
                requestId,
                "messageAdded",
                new MessageAddedEvent(request.TurnId, assistantMessage),
                cancellationToken).ConfigureAwait(false);
            await peer.SendEventAsync(
                requestId,
                "completed",
                timings.Completion(request.TurnId, "completed"),
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            if (visibleResponse.Length > 0 && !assistantStored)
            {
                try
                {
                    using var persistenceTimeout = new CancellationTokenSource(
                        CancelledPartialPersistenceTimeout);
                    await conversationStore.AppendMessageAsync(
                        projectContextKey,
                        new ConversationMessage(
                            assistantMessageId,
                            request.ConversationId,
                            "assistant",
                            visibleResponse.ToString(),
                            DateTimeOffset.UtcNow),
                        persistenceTimeout.Token).ConfigureAwait(false);
                }
                catch (Exception)
                {
                    await SendWithoutTurnCancellationAsync(
                        peer,
                        requestId,
                        "error",
                        new ErrorEvent(request.TurnId, "history_write_failed", "The cancelled response could not be saved.")).ConfigureAwait(false);
                }
            }
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "completed",
                timings.Completion(request.TurnId, "cancelled")).ConfigureAwait(false);
        }
        catch (ChatInferenceException exception)
        {
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "error",
                BuildPublicInferenceError(request.TurnId, exception, timings)).ConfigureAwait(false);
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "completed",
                timings.Completion(request.TurnId, "failed")).ConfigureAwait(false);
        }
        catch (Exception)
        {
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "error",
                timings.Error(
                    request.TurnId,
                    "turn_failed",
                    "The local chat turn failed.")).ConfigureAwait(false);
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "completed",
                timings.Completion(request.TurnId, "failed")).ConfigureAwait(false);
        }
    }

    private async Task<InferenceRound> ReadInferenceRoundAsync(
        ChatInferenceRequest inferenceRequest,
        Func<string, Task>? emitText,
        CancellationToken cancellationToken)
    {
        var text = new StringBuilder();
        var textChunks = new List<string>();
        IReadOnlyList<ChatInferenceToolCall>? toolCalls = null;
        await foreach (var chunk in inferenceRuntime.StreamAsync(
            inferenceRequest,
            cancellationToken).WithCancellation(cancellationToken).ConfigureAwait(false))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (toolCalls is not null)
            {
                throw new ChatInferenceException(
                    "invalid_tool_calls",
                    "The local inference runtime returned output after its tool calls.");
            }
            if (!string.IsNullOrEmpty(chunk.Text))
            {
                if (text.Length + chunk.Text.Length > ChatInferenceLimits.MaximumOutputCharacters)
                {
                    throw new ChatInferenceException(
                        "inference_output_too_large",
                        $"The local response exceeded {ChatInferenceLimits.MaximumOutputCharacters} characters.");
                }
                text.Append(chunk.Text);
                textChunks.Add(chunk.Text);
                if (emitText is not null)
                    await emitText(chunk.Text).ConfigureAwait(false);
            }
            if (chunk.ToolCalls is { Count: > 0 })
            {
                toolCalls = chunk.ToolCalls.ToArray();
            }
            else if (string.IsNullOrEmpty(chunk.Text))
            {
                throw new ChatInferenceException(
                    "invalid_inference_output",
                    "The local inference runtime returned an empty output chunk.");
            }
        }
        return new InferenceRound(
            text.ToString(),
            textChunks,
            toolCalls,
            TextWasEmitted: emitText is not null);
    }

    private static IReadOnlyList<PreparedToolCall> PrepareToolBatch(
        IReadOnlyList<ChatInferenceToolCall> calls,
        IReadOnlyDictionary<string, SameInstanceMcpTool> tools,
        ToolTurnState state)
    {
        if (calls.Count == 0)
            throw InvalidToolCalls();

        var prepared = new List<PreparedToolCall>(calls.Count);
        var batchIds = new HashSet<string>(StringComparer.Ordinal);
        var batchArgumentBytes = 0;
        foreach (var call in calls)
        {
            if (!IsSafeProtocolValue(
                    call.Id,
                    ChatInferenceLimits.MaximumToolCallIdCharacters) ||
                !batchIds.Add(call.Id) ||
                state.ToolCallIds.Contains(call.Id) ||
                !IsSafeProtocolValue(
                    call.Name,
                    ChatInferenceLimits.MaximumToolNameCharacters) ||
                call.ArgumentsJson is null)
            {
                throw InvalidToolCalls();
            }

            var argumentBytes = Encoding.UTF8.GetByteCount(call.ArgumentsJson);
            if (argumentBytes > MaximumToolArgumentsBytes ||
                batchArgumentBytes + argumentBytes > MaximumToolArgumentsPerTurnBytes - state.ToolArgumentBytes)
            {
                throw new ChatInferenceException(
                    "tool_call_limit_exceeded",
                    "The local inference runtime exceeded the tool argument limit.");
            }

            JsonElement arguments;
            try
            {
                using var document = JsonDocument.Parse(
                    call.ArgumentsJson,
                    new JsonDocumentOptions
                    {
                        AllowTrailingCommas = false,
                        CommentHandling = JsonCommentHandling.Disallow,
                        MaxDepth = 64,
                    });
                if (document.RootElement.ValueKind != JsonValueKind.Object)
                    throw new JsonException();
                arguments = document.RootElement.Clone();
            }
            catch (JsonException)
            {
                throw InvalidToolCalls();
            }

            string canonicalArguments;
            try
            {
                canonicalArguments = SameInstanceMcpArgumentHash.Canonicalize(arguments);
            }
            catch (ArgumentException)
            {
                throw InvalidToolCalls();
            }

            if (!tools.TryGetValue(call.Name, out var tool))
                throw InvalidToolCalls();
            prepared.Add(new PreparedToolCall(
                call,
                tool,
                arguments,
                canonicalArguments));
            batchArgumentBytes += argumentBytes;
        }

        foreach (var id in batchIds) state.ToolCallIds.Add(id);
        state.ToolArgumentBytes += batchArgumentBytes;
        return prepared;
    }

    private async Task<ChatInferenceMessage> ExecuteToolCallAsync(
        Guid turnId,
        Guid requestId,
        IPipePeer peer,
        ISameInstanceMcpClient mcpClient,
        PreparedToolCall call,
        ToolTurnState state,
        CancellationToken cancellationToken)
    {
        if (call.Tool is null)
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                "unknown",
                "failed",
                "unknown_tool",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "unknown_tool");
        }

        if (call.Tool.IsReadOnly)
        {
            return await CallMcpToolAsync(
                turnId,
                requestId,
                peer,
                mcpClient,
                call,
                state,
                approvalGrant: null,
                cancellationToken).ConfigureAwait(false);
        }

        var argumentsBytes = Encoding.UTF8.GetByteCount(call.CanonicalArguments);
        if (argumentsBytes > MaximumApprovalArgumentsBytes)
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "approval_arguments_too_large",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "approval_arguments_too_large");
        }

        var expectedHash = SameInstanceMcpArgumentHash.Compute(call.Arguments);
        var deniedFingerprint = call.Tool.Name + "\0" + expectedHash;
        if (state.DeniedMutations.Contains(deniedFingerprint))
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "denied",
                "user_denied",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "user_denied");
        }
        if (state.MutationApprovals >= MaximumMutationApprovalsPerTurn)
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "approval_limit_exceeded",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "approval_limit_exceeded");
        }

        SameInstanceMcpApprovalBinding binding;
        try
        {
            binding = await mcpClient.CreateApprovalBindingAsync(
                call.Tool.Name,
                call.Arguments,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (SameInstanceMcpException exception)
        {
            var code = IsApprovalSessionExpired(exception.Code)
                ? "approval_session_expired"
                : "approval_binding_failed";
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                code,
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, code);
        }
        catch (Exception)
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "approval_binding_failed",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "approval_binding_failed");
        }

        if (!string.Equals(binding.Tool, call.Tool.Name, StringComparison.Ordinal) ||
            !string.Equals(binding.ArgumentsHash, expectedHash, StringComparison.Ordinal) ||
            !IsSafeProtocolValue(
                binding.McpSessionId,
                SameInstanceMcpLimits.MaximumSessionIdCharacters))
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "approval_binding_failed",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "approval_binding_failed");
        }

        state.MutationApprovals++;
        var approvalId = Guid.NewGuid();
        using var approvalCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var approvalTask = WaitForApprovalAsync(approvalId, approvalCancellation.Token);
        try
        {
            await peer.SendEventAsync(
                requestId,
                "toolApprovalRequired",
                new ToolApprovalRequiredEvent(
                    approvalId,
                    turnId,
                    call.Call.Id,
                    call.Tool.Name,
                    $"Run {call.Tool.Name}",
                    binding.McpSessionId,
                    binding.ArgumentsHash,
                    call.CanonicalArguments),
                cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            approvalCancellation.Cancel();
            try { _ = await approvalTask.ConfigureAwait(false); }
            catch (OperationCanceledException) when (approvalCancellation.IsCancellationRequested) { }
            throw;
        }

        var resolution = await approvalTask.ConfigureAwait(false);
        if (!resolution.Allowed)
        {
            state.DeniedMutations.Add(deniedFingerprint);
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "denied",
                "user_denied",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "user_denied");
        }
        if (!IsOneTimeApprovalGrant(resolution.ApprovalGrant))
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "invalid_approval_grant",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "invalid_approval_grant");
        }

        return await CallMcpToolAsync(
            turnId,
            requestId,
            peer,
            mcpClient,
            call,
            state,
            resolution.ApprovalGrant,
            cancellationToken).ConfigureAwait(false);
    }

    private static async Task<ChatInferenceMessage> CallMcpToolAsync(
        Guid turnId,
        Guid requestId,
        IPipePeer peer,
        ISameInstanceMcpClient mcpClient,
        PreparedToolCall call,
        ToolTurnState state,
        string? approvalGrant,
        CancellationToken cancellationToken)
    {
        await peer.SendEventAsync(
            requestId,
            "toolStarted",
            new ToolStartedEvent(turnId, call.Call.Id, call.Tool!.Name),
            cancellationToken).ConfigureAwait(false);
        try
        {
            var result = await mcpClient.CallToolAsync(
                call.Tool.Name,
                call.Arguments,
                approvalGrant,
                cancellationToken).ConfigureAwait(false);
            if (!string.Equals(result.ToolName, call.Tool.Name, StringComparison.Ordinal))
            {
                await SendToolCompletedAsync(
                    peer,
                    requestId,
                    turnId,
                    call.Call.Id,
                    call.Tool.Name,
                    "failed",
                    "tool_result_unavailable",
                    cancellationToken).ConfigureAwait(false);
                return BuildSyntheticToolResult(call.Call, "tool_result_unavailable");
            }
            var shapedResult = ShapeToolResultForModel(result.Result);
            if (shapedResult is null)
            {
                await SendToolCompletedAsync(
                    peer,
                    requestId,
                    turnId,
                    call.Call.Id,
                    call.Tool.Name,
                    "failed",
                    "tool_result_unavailable",
                    cancellationToken).ConfigureAwait(false);
                return BuildSyntheticToolResult(call.Call, "tool_result_unavailable");
            }
            var resultJson = shapedResult;
            var resultBytes = Encoding.UTF8.GetByteCount(resultJson);
            if (resultBytes > MaximumToolResultBytes ||
                state.ToolResultBytes + resultBytes > MaximumToolResultsPerTurnBytes)
            {
                await SendToolCompletedAsync(
                    peer,
                    requestId,
                    turnId,
                    call.Call.Id,
                    call.Tool.Name,
                    "failed",
                    "tool_result_too_large",
                    cancellationToken).ConfigureAwait(false);
                return BuildSyntheticToolResult(call.Call, "tool_result_too_large");
            }

            state.ToolResultBytes += resultBytes;
            var code = result.IsError ? "tool_error" : null;
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                result.IsError ? "failed" : "succeeded",
                code,
                cancellationToken,
                isError: result.IsError).ConfigureAwait(false);
            return new ChatInferenceMessage(
                ChatInferenceRole.Tool,
                resultJson,
                Name: call.Call.Name,
                ToolCallId: call.Call.Id);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "toolCompleted",
                new ToolCompletedEvent(
                    turnId,
                    call.Call.Id,
                    call.Tool.Name,
                    "unknown",
                    IsError: true,
                    Code: "cancelled_after_start")).ConfigureAwait(false);
            throw;
        }
        catch (SameInstanceMcpException exception)
        {
            var code = IsApprovalSessionExpired(exception.Code)
                ? "approval_session_expired"
                : "tool_failed";
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                code,
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, code);
        }
        catch (Exception)
        {
            await SendToolCompletedAsync(
                peer,
                requestId,
                turnId,
                call.Call.Id,
                call.Tool.Name,
                "failed",
                "tool_failed",
                cancellationToken).ConfigureAwait(false);
            return BuildSyntheticToolResult(call.Call, "tool_failed");
        }
    }

    private static Task SendToolCompletedAsync(
        IPipePeer peer,
        Guid requestId,
        Guid turnId,
        string toolCallId,
        string tool,
        string status,
        string? code,
        CancellationToken cancellationToken,
        bool? isError = null) => peer.SendEventAsync(
            requestId,
            "toolCompleted",
            new ToolCompletedEvent(
                turnId,
                toolCallId,
                tool,
                status,
                isError ?? true,
                code),
            cancellationToken);

    private static ChatInferenceMessage BuildSyntheticToolResult(
        ChatInferenceToolCall call,
        string code) => new(
            ChatInferenceRole.Tool,
            $"{{\"ok\":false,\"code\":\"{code}\"}}",
            Name: call.Name,
            ToolCallId: call.Id);

    private static string? ShapeToolResultForModel(JsonElement result)
    {
        if (result.ValueKind != JsonValueKind.Object) return null;
        if (result.TryGetProperty("structuredContent", out var structuredContent) &&
            structuredContent.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined))
        {
            return JsonSerializer.Serialize(structuredContent, PipeJson.SerializerOptions);
        }

        if (!result.TryGetProperty("content", out var content) ||
            content.ValueKind != JsonValueKind.Array)
        {
            return null;
        }
        var textItems = new List<string>();
        foreach (var item in content.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.Object &&
                item.TryGetProperty("type", out var type) &&
                string.Equals(type.GetString(), "text", StringComparison.Ordinal) &&
                item.TryGetProperty("text", out var text) &&
                text.ValueKind == JsonValueKind.String)
            {
                textItems.Add(text.GetString() ?? string.Empty);
            }
        }
        return textItems.Count == 0
            ? null
            : JsonSerializer.Serialize(new { text = textItems }, PipeJson.SerializerOptions);
    }

    private static int TrimContinuationHistory(
        List<ChatInferenceMessage> history,
        int removableMessages)
    {
        while (history.Count > ChatInferenceLimits.MaximumHistoryMessages ||
               MeasureInferenceHistory(history) > ChatInferenceLimits.MaximumHistoryCharacters)
        {
            if (removableMessages == 0)
            {
                throw new ChatInferenceException(
                    "tool_context_too_large",
                    "The local tool context exceeded the supported size.");
            }
            history.RemoveAt(0);
            removableMessages--;
        }
        return removableMessages;
    }

    private static long MeasureInferenceHistory(IEnumerable<ChatInferenceMessage> history)
    {
        long characters = 0;
        foreach (var message in history)
        {
            characters += message.Content.Length;
            characters += message.Name?.Length ?? 0;
            characters += message.ToolCallId?.Length ?? 0;
            if (message.ToolCalls is null) continue;
            foreach (var call in message.ToolCalls)
                characters += call.Id.Length + call.Name.Length + call.ArgumentsJson.Length;
        }
        return characters;
    }

    private static bool IsSafeProtocolValue(string? value, int maximumCharacters) =>
        !string.IsNullOrWhiteSpace(value) &&
        value.Length <= maximumCharacters &&
        !value.Any(char.IsControl);

    private static bool RequestsExplicitDiagnosticsInvocation(string userText)
    {
        if (string.IsNullOrWhiteSpace(userText)) return false;

        var trimmed = userText.TrimStart();
        if (!trimmed.StartsWith(DirectDiagnosticsToolName, StringComparison.Ordinal))
            return false;
        if (trimmed.Length > DirectDiagnosticsToolName.Length)
        {
            var next = trimmed[DirectDiagnosticsToolName.Length];
            if ((next >= 'a' && next <= 'z') ||
                (next >= 'A' && next <= 'Z') ||
                (next >= '0' && next <= '9') ||
                next is '_' or '.')
            {
                return false;
            }
        }

        var lower = trimmed.ToLowerInvariant();
        if (trimmed.Contains("実行しない", StringComparison.Ordinal) ||
            trimmed.Contains("実行せず", StringComparison.Ordinal) ||
            trimmed.Contains("呼び出さない", StringComparison.Ordinal) ||
            trimmed.Contains("取得しない", StringComparison.Ordinal) ||
            lower.Contains("do not ", StringComparison.Ordinal) ||
            lower.Contains("don't ", StringComparison.Ordinal) ||
            lower.Contains("without running", StringComparison.Ordinal) ||
            lower.Contains("without calling", StringComparison.Ordinal))
        {
            return false;
        }

        return trimmed.Contains("実行", StringComparison.Ordinal) ||
            trimmed.Contains("呼び出", StringComparison.Ordinal) ||
            trimmed.Contains("取得", StringComparison.Ordinal) ||
            lower.Contains("run", StringComparison.Ordinal) ||
            lower.Contains("execute", StringComparison.Ordinal) ||
            lower.Contains("call", StringComparison.Ordinal) ||
            lower.Contains("invoke", StringComparison.Ordinal);
    }

    private static bool IsApprovalSessionExpired(string? code) =>
        string.Equals(code, "approval_session_expired", StringComparison.Ordinal) ||
        string.Equals(code, "session_expired", StringComparison.Ordinal);

    private static ChatInferenceException InvalidToolCalls() => new(
        "invalid_tool_calls",
        "The local inference runtime returned invalid tool calls.");

    private static ErrorEvent BuildPublicInferenceError(
        Guid turnId,
        ChatInferenceException exception,
        TurnTimingTracker timings)
    {
        var publicError = exception.Code switch
        {
            "invalid_inference_status" => new ErrorEvent(
                turnId,
                "invalid_inference_status",
                "The local inference runtime returned an invalid status."),
            "inference_runtime_not_ready" => new ErrorEvent(
                turnId,
                "inference_runtime_not_ready",
                "The local inference runtime is not ready."),
            "invalid_inference_output" => new ErrorEvent(
                turnId,
                "invalid_inference_output",
                "The local inference runtime returned invalid output."),
            "inference_output_too_large" => new ErrorEvent(
                turnId,
                "inference_output_too_large",
                "The local inference response exceeded the supported size."),
            "empty_inference_response" => new ErrorEvent(
                turnId,
                "empty_inference_response",
                "The local inference runtime returned no response."),
            "invalid_inference_request" => new ErrorEvent(
                turnId,
                "invalid_inference_request",
                "The local inference request is invalid."),
            "invalid_tool_calls" => new ErrorEvent(
                turnId,
                "invalid_tool_calls",
                "The local inference runtime returned invalid tool calls."),
            "tool_call_limit_exceeded" => new ErrorEvent(
                turnId,
                "tool_call_limit_exceeded",
                "The local inference runtime exceeded the tool call limit."),
            "tool_context_too_large" => new ErrorEvent(
                turnId,
                "tool_context_too_large",
                "The local tool context exceeded the supported size."),
            "inference_stream_timeout" => new ErrorEvent(
                turnId,
                "inference_stream_timeout",
                "The local model did not produce stream output before the timeout."),
            "inference_http_error" => new ErrorEvent(
                turnId,
                "inference_http_error",
                "The local inference server rejected the request."),
            "inference_transport_failed" => new ErrorEvent(
                turnId,
                "inference_transport_failed",
                "The local inference server could not be reached."),
            "inference_stream_failed" => new ErrorEvent(
                turnId,
                "inference_stream_failed",
                "The local inference response stream failed."),
            "inference_stream_truncated" => new ErrorEvent(
                turnId,
                "inference_stream_truncated",
                "The local inference response ended before completion."),
            "inference_session_failed" => new ErrorEvent(
                turnId,
                "inference_session_failed",
                "The local inference session is unavailable."),
            _ => new ErrorEvent(
                turnId,
                "inference_runtime_failed",
                "The local inference runtime failed."),
        };
        return timings.WithTiming(publicError);
    }

    private static bool IsSafeRuntimeToken(string? value, int maximumCharacters)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > maximumCharacters)
            return false;
        foreach (var character in value)
        {
            if ((character is >= 'a' and <= 'z') ||
                (character is >= 'A' and <= 'Z') ||
                (character is >= '0' and <= '9') ||
                character is '-' or '_' or '.')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    private static bool IsSafeRuntimeDisplayName(string? value)
    {
        if (value is null || value.Length > ChatInferenceLimits.MaximumNameCharacters)
            return false;
        return value.All(character => !char.IsControl(character));
    }

    private static bool IsOneTimeApprovalGrant(string? value)
    {
        if (value is not { Length: 64 }) return false;
        foreach (var character in value)
        {
            if (character is not (>= '0' and <= '9') and
                not (>= 'a' and <= 'f'))
            {
                return false;
            }
        }
        return true;
    }

    private static (
        IReadOnlyDictionary<string, SameInstanceMcpTool> McpTools,
        IReadOnlyList<ChatInferenceToolDefinition> InferenceTools)
        BuildToolCatalog(IReadOnlyList<SameInstanceMcpTool> tools)
    {
        if (tools.Count > ChatInferenceLimits.MaximumTools)
            throw new InvalidOperationException("The private MCP tool catalog is too large.");

        var mcpTools = new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
        var inferenceTools = new List<ChatInferenceToolDefinition>(tools.Count);
        long catalogBytes = 0;
        foreach (var tool in tools)
        {
            if (!IsSafeProtocolValue(
                    tool.Name,
                    ChatInferenceLimits.MaximumToolNameCharacters) ||
                !mcpTools.TryAdd(tool.Name, tool) ||
                tool.Description.Length > ChatInferenceLimits.MaximumToolDescriptionCharacters ||
                tool.InputSchema.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidOperationException("The private MCP tool catalog is invalid.");
            }

            var schemaJson = JsonSerializer.Serialize(tool.InputSchema, PipeJson.SerializerOptions);
            if (schemaJson.Length > ChatInferenceLimits.MaximumToolSchemaCharacters)
                throw new InvalidOperationException("The private MCP tool catalog is too large.");
            EnsureNoDuplicateJsonProperties(tool.InputSchema);
            try
            {
                catalogBytes += StrictUtf8.GetByteCount(tool.Name) +
                    StrictUtf8.GetByteCount(tool.Description) +
                    StrictUtf8.GetByteCount(schemaJson);
            }
            catch (EncoderFallbackException exception)
            {
                throw new InvalidOperationException(
                    "The private MCP tool catalog is invalid.",
                    exception);
            }
            if (catalogBytes > ChatInferenceLimits.MaximumToolCatalogBytes)
                throw new InvalidOperationException("The private MCP tool catalog is too large.");

            inferenceTools.Add(new ChatInferenceToolDefinition(
                tool.Name,
                tool.Description,
                schemaJson));
        }
        return (mcpTools, inferenceTools);
    }

    private static void EnsureNoDuplicateJsonProperties(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in value.EnumerateObject())
            {
                if (!names.Add(property.Name))
                    throw new InvalidOperationException("The private MCP tool schema is invalid.");
                EnsureNoDuplicateJsonProperties(property.Value);
            }
        }
        else if (value.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in value.EnumerateArray())
                EnsureNoDuplicateJsonProperties(item);
        }
    }

    private async Task<IReadOnlyList<ConversationSummary>> EnsureConversationTitlesAsync(
        string projectContextKey,
        IReadOnlyList<ConversationSummary> conversations,
        CancellationToken cancellationToken)
    {
        var result = conversations.ToArray();
        for (var index = 0; index < result.Length; index++)
        {
            if (!IsDefaultConversationTitle(result[index].Title)) continue;
            try
            {
                var messages = await conversationStore.GetMessagesAsync(
                    projectContextKey,
                    result[index].Id,
                    cancellationToken).ConfigureAwait(false);
                var firstUserMessage = messages.FirstOrDefault(message =>
                    !message.IsError &&
                    string.Equals(message.Role, "user", StringComparison.Ordinal) &&
                    !string.IsNullOrWhiteSpace(message.Content));
                if (firstUserMessage is null) continue;
                result[index] = await conversationStore.UpdateTitleAsync(
                    projectContextKey,
                    result[index].Id,
                    BuildAutomaticConversationTitle(firstUserMessage.Content),
                    cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception)
            {
                // Old histories remain usable even if a best-effort title
                // backfill cannot update their metadata.
            }
        }
        return result;
    }

    private static bool IsDefaultConversationTitle(string? title) =>
        string.IsNullOrWhiteSpace(title) ||
        string.Equals(title.Trim(), DefaultConversationTitle, StringComparison.Ordinal) ||
        string.Equals(title.Trim(), "New chat", StringComparison.OrdinalIgnoreCase);

    private static string BuildAutomaticConversationTitle(string text)
    {
        var normalized = new StringBuilder(Math.Min(text.Length, MaximumAutomaticTitleCharacters * 2));
        var pendingSpace = false;
        foreach (var character in text.Trim())
        {
            if (char.IsWhiteSpace(character))
            {
                pendingSpace = normalized.Length != 0;
                continue;
            }
            if (character is '。' or '！' or '？' or '!' or '?') break;
            if (pendingSpace)
            {
                normalized.Append(' ');
                pendingSpace = false;
            }
            normalized.Append(character);
        }

        var title = normalized.ToString().Trim(' ', '#', '-', '*', '`', '"', '\'');
        if (string.IsNullOrWhiteSpace(title)) return DefaultConversationTitle;
        if (title.Length <= MaximumAutomaticTitleCharacters) return title;

        var length = MaximumAutomaticTitleCharacters - 1;
        if (length > 0 && char.IsHighSurrogate(title[length - 1])) length--;
        var shortened = title[..length].TrimEnd();
        var lastSpace = shortened.LastIndexOf(' ');
        if (lastSpace >= MaximumAutomaticTitleCharacters / 2)
            shortened = shortened[..lastSpace].TrimEnd();
        return shortened + "…";
    }

    private static string BuildConversationMarkdown(
        ConversationSummary conversation,
        IReadOnlyList<ConversationMessage> messages)
    {
        var markdown = new StringBuilder();
        markdown.Append("# ").AppendLine(CollapseMarkdownHeading(conversation.Title));
        markdown.AppendLine();
        markdown.Append("- Created: ").AppendLine(
            conversation.CreatedAt.ToString("yyyy-MM-dd HH:mm:ss zzz", CultureInfo.InvariantCulture));
        markdown.Append("- Updated: ").AppendLine(
            conversation.UpdatedAt.ToString("yyyy-MM-dd HH:mm:ss zzz", CultureInfo.InvariantCulture));
        markdown.Append("- Messages: ").AppendLine(
            messages.Count.ToString(CultureInfo.InvariantCulture));
        markdown.AppendLine();
        markdown.AppendLine("---");
        markdown.AppendLine();
        foreach (var message in messages)
        {
            var role = message.IsError
                ? "Error"
                : message.Role switch
                {
                    "user" => "You",
                    "assistant" => "AI Assistant",
                    "system" => "System",
                    _ => "Message",
                };
            markdown.Append("## ").AppendLine(role);
            markdown.AppendLine();
            var content = message.Content
                .Replace("\r\n", "\n", StringComparison.Ordinal)
                .Replace('\r', '\n')
                .Trim();
            markdown.AppendLine(string.IsNullOrEmpty(content) ? "_Empty message_" : content);
            markdown.AppendLine();
            markdown.AppendLine("---");
            markdown.AppendLine();
        }
        return markdown.ToString();
    }

    private static string CollapseMarkdownHeading(string title)
    {
        var normalized = new StringBuilder(title.Length);
        var pendingSpace = false;
        foreach (var character in title)
        {
            if (char.IsWhiteSpace(character))
            {
                pendingSpace = normalized.Length != 0;
                continue;
            }
            if (pendingSpace)
            {
                normalized.Append(' ');
                pendingSpace = false;
            }
            if (character is '#' or '\\') normalized.Append('\\');
            normalized.Append(character);
        }
        return normalized.Length == 0 ? DefaultConversationTitle : normalized.ToString();
    }

    private static IReadOnlyList<ChatInferenceMessage> BuildInferenceHistory(
        IReadOnlyList<SequencedConversationMessage> newestFirst)
    {
        var selectedNewestFirst = new List<ChatInferenceMessage>(newestFirst.Count);
        var historyCharacters = 0;
        foreach (var sequenced in newestFirst)
        {
            var message = sequenced.Message;
            if (message.IsError)
                continue;
            var role = message.Role switch
            {
                "system" => ChatInferenceRole.System,
                "user" => ChatInferenceRole.User,
                "assistant" => ChatInferenceRole.Assistant,
                _ => (ChatInferenceRole?)null,
            };
            if (!role.HasValue || string.IsNullOrEmpty(message.Content))
                continue;
            if (historyCharacters + message.Content.Length >
                ChatInferenceLimits.MaximumHistoryCharacters)
            {
                break;
            }
            selectedNewestFirst.Add(new ChatInferenceMessage(
                role.Value,
                message.Content));
            historyCharacters += message.Content.Length;
        }
        selectedNewestFirst.Reverse();
        return selectedNewestFirst;
    }

    private static ConversationSelectResult BuildConversationPage(
        ConversationSummary conversation,
        IReadOnlyList<SequencedConversationMessage> candidatesNewestFirst,
        int pageSize)
    {
        var sizingResult = new ConversationSelectResult(
            conversation,
            Array.Empty<ConversationMessage>(),
            PipeProtocol.MaximumExactJsonInteger,
            HasMoreMessages: false);
        var fixedPayloadBytes = JsonSerializer.SerializeToUtf8Bytes(
            sizingResult,
            PipeJson.SerializerOptions).Length - 2;
        if (fixedPayloadBytes + 2 > PipeProtocol.MaximumConversationSelectPayloadBytes)
            throw new ChatRequestException(
                "history_page_too_large",
                "The conversation metadata exceeds the supported history page size.");

        var selectedNewestFirst = new List<SequencedConversationMessage>(
            Math.Min(pageSize, candidatesNewestFirst.Count));
        var messageArrayBytes = 2;
        foreach (var candidate in candidatesNewestFirst.Take(pageSize))
        {
            if (candidate.Sequence <= 0 || candidate.Sequence > PipeProtocol.MaximumExactJsonInteger)
                throw new ChatRequestException(
                    "history_cursor_out_of_range",
                    "The stored history sequence exceeds the supported cursor range.");

            var messageBytes = JsonSerializer.SerializeToUtf8Bytes(
                candidate.Message,
                PipeJson.SerializerOptions).Length;
            var separatorBytes = selectedNewestFirst.Count == 0 ? 0 : 1;
            if (fixedPayloadBytes + messageArrayBytes + separatorBytes + messageBytes >
                PipeProtocol.MaximumConversationSelectPayloadBytes)
            {
                break;
            }
            selectedNewestFirst.Add(candidate);
            messageArrayBytes += separatorBytes + messageBytes;
        }

        if (selectedNewestFirst.Count == 0 && candidatesNewestFirst.Count != 0)
            throw new ChatRequestException(
                "history_message_too_large",
                "A stored conversation message exceeds the supported history page size.");

        var hasMoreMessages = selectedNewestFirst.Count < candidatesNewestFirst.Count;
        var olderBeforeMessageSequence = hasMoreMessages
            ? selectedNewestFirst[^1].Sequence
            : (long?)null;
        var messages = selectedNewestFirst
            .AsEnumerable()
            .Reverse()
            .Select(candidate => candidate.Message)
            .ToArray();
        var result = new ConversationSelectResult(
            conversation,
            messages,
            olderBeforeMessageSequence,
            hasMoreMessages);
        if (JsonSerializer.SerializeToUtf8Bytes(result, PipeJson.SerializerOptions).Length >
            PipeProtocol.MaximumConversationSelectPayloadBytes)
        {
            throw new ChatRequestException(
                "history_page_too_large",
                "The conversation history page exceeds the supported response size.");
        }
        return result;
    }

    private static async Task SendWithoutTurnCancellationAsync(
        IPipePeer peer,
        Guid requestId,
        string method,
        object payload)
    {
        using var timeout = new CancellationTokenSource(TerminalEventWriteTimeout);
        try { await peer.SendEventAsync(requestId, method, payload, timeout.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested) { }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }

    private static async ValueTask DisposeMcpClientQuietlyAsync(ISameInstanceMcpClient client)
    {
        try { await client.DisposeAsync().ConfigureAwait(false); }
        catch { }
    }

    private (string ProjectContextKey, Guid? ActiveConversationId) GetInitializedState()
    {
        lock (_gate)
        {
            if (_instanceId is null || _projectContextKey is null)
                throw new ChatRequestException("not_initialized", "initialize must be called first.");
            return (_projectContextKey, _activeConversationId);
        }
    }

    private sealed class ActiveTurn
    {
        private const int Prepared = 0;
        private const int Started = 1;
        private const int Aborted = 2;
        private const int CancelledBeforeStart = 3;
        private readonly CancellationTokenSource _cancellation;
        private readonly CancellationToken _cancellationToken;
        private readonly Func<Task> _cancelledBeforeStartOperation;
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly Func<CancellationToken, Task> _operation;
        private readonly Action<ActiveTurn> _onCompleted;
        private int _disposition;
        private int _completionSignaled;

        public ActiveTurn(
            Guid turnId,
            CancellationTokenSource cancellation,
            Func<CancellationToken, Task> operation,
            Func<Task> cancelledBeforeStartOperation,
            Action<ActiveTurn> onCompleted)
        {
            TurnId = turnId;
            _cancellation = cancellation;
            _cancellationToken = cancellation.Token;
            _operation = operation;
            _cancelledBeforeStartOperation = cancelledBeforeStartOperation;
            _onCompleted = onCompleted;
        }

        public Guid TurnId { get; }
        public Task Completion => _completion.Task;
        public bool IsCancellationRequested => _cancellationToken.IsCancellationRequested;

        public void Start()
        {
            var previous = Interlocked.CompareExchange(ref _disposition, Started, Prepared);
            var operationFactory = _operation;
            if (previous == CancelledBeforeStart)
            {
                if (Interlocked.CompareExchange(
                        ref _disposition,
                        Started,
                        CancelledBeforeStart) != CancelledBeforeStart)
                {
                    throw new InvalidOperationException("The active turn has already been handled.");
                }
                operationFactory = _ => _cancelledBeforeStartOperation();
            }
            else if (previous != Prepared)
            {
                throw new InvalidOperationException("The active turn has already been handled.");
            }

            Task operation;
            try
            {
                operation = operationFactory(_cancellationToken);
            }
            catch (Exception exception)
            {
                operation = Task.FromException(exception);
            }
            _ = ObserveOperationAsync(operation);
        }

        public void Abort()
        {
            while (true)
            {
                var current = Volatile.Read(ref _disposition);
                if (current is Started or Aborted)
                    return;
                if (current is not (Prepared or CancelledBeforeStart))
                    throw new InvalidOperationException("The active turn has already been handled.");
                if (Interlocked.CompareExchange(ref _disposition, Aborted, current) != current)
                    continue;

                TryCancel();
                CompleteSuccessfully();
                return;
            }
        }

        public bool TryCancel()
        {
            try
            {
                _cancellation.Cancel();
                // The sendTurn response decides whether this prepared turn was
                // accepted. Start emits the cancellation terminal; Abort
                // completes without one when the response failed.
                Interlocked.CompareExchange(
                    ref _disposition,
                    CancelledBeforeStart,
                    Prepared);
                return true;
            }
            catch (ObjectDisposedException)
            {
                return false;
            }
        }

        private async Task ObserveOperationAsync(Task operation)
        {
            try
            {
                await operation.ConfigureAwait(false);
                CompleteSuccessfully();
            }
            catch (OperationCanceledException) when (_cancellationToken.IsCancellationRequested)
            {
                CompleteCanceled();
            }
            catch (Exception exception)
            {
                CompleteWithException(exception);
            }
        }

        private void CompleteSuccessfully() => Complete(() => _completion.TrySetResult());

        private void CompleteCanceled() =>
            Complete(() => _completion.TrySetCanceled(_cancellationToken));

        private void CompleteWithException(Exception exception) =>
            Complete(() => _completion.TrySetException(exception));

        private void Complete(Action signalCompletion)
        {
            if (Interlocked.Exchange(ref _completionSignaled, 1) != 0)
                return;
            _onCompleted(this);
            _cancellation.Dispose();
            signalCompletion();
        }
    }

    private sealed record InferenceRound(
        string Text,
        IReadOnlyList<string> TextChunks,
        IReadOnlyList<ChatInferenceToolCall>? ToolCalls,
        bool TextWasEmitted);

    private sealed class TurnTimingTracker
    {
        private readonly Stopwatch _total = Stopwatch.StartNew();

        public long RuntimeSetupMilliseconds { get; private set; }
        public long InitialInferenceMilliseconds { get; private set; }
        public long ContinuationInferenceMilliseconds { get; private set; }
        public long ToolMilliseconds { get; private set; }
        public int InferenceRounds { get; private set; }
        public int ToolCalls { get; private set; }

        public void AddRuntimeSetup(long elapsedMilliseconds) =>
            RuntimeSetupMilliseconds += elapsedMilliseconds;

        public void AddInference(bool initial, long elapsedMilliseconds)
        {
            if (initial)
                InitialInferenceMilliseconds += elapsedMilliseconds;
            else
                ContinuationInferenceMilliseconds += elapsedMilliseconds;
            InferenceRounds++;
        }

        public void AddTool(long elapsedMilliseconds)
        {
            ToolMilliseconds += elapsedMilliseconds;
            ToolCalls++;
        }

        public TurnCompletedEvent Completion(Guid turnId, string status) => new(
            turnId,
            status,
            _total.ElapsedMilliseconds,
            RuntimeSetupMilliseconds,
            InitialInferenceMilliseconds,
            ContinuationInferenceMilliseconds,
            ToolMilliseconds,
            InferenceRounds,
            ToolCalls);

        public ErrorEvent Error(Guid? turnId, string code, string message) => new(
            turnId,
            code,
            message,
            _total.ElapsedMilliseconds,
            RuntimeSetupMilliseconds,
            InitialInferenceMilliseconds,
            ContinuationInferenceMilliseconds,
            ToolMilliseconds,
            InferenceRounds,
            ToolCalls);

        public ErrorEvent WithTiming(ErrorEvent error) => error with
        {
            TotalMilliseconds = _total.ElapsedMilliseconds,
            RuntimeSetupMilliseconds = RuntimeSetupMilliseconds,
            InitialInferenceMilliseconds = InitialInferenceMilliseconds,
            ContinuationInferenceMilliseconds = ContinuationInferenceMilliseconds,
            ToolMilliseconds = ToolMilliseconds,
            InferenceRounds = InferenceRounds,
            ToolCalls = ToolCalls,
        };
    }

    private sealed record PreparedToolCall(
        ChatInferenceToolCall Call,
        SameInstanceMcpTool? Tool,
        JsonElement Arguments,
        string CanonicalArguments);

    private sealed class ToolTurnState
    {
        public int ToolRounds { get; set; }
        public int ToolCalls { get; set; }
        public int MutationApprovals { get; set; }
        public int ToolArgumentBytes { get; set; }
        public int ToolResultBytes { get; set; }
        public int ToolRoundTextCharacters { get; set; }
        public HashSet<string> ToolCallIds { get; } = new(StringComparer.Ordinal);
        public HashSet<string> DeniedMutations { get; } = new(StringComparer.Ordinal);
    }

    public sealed class PreparedTurn(Action start, Action abort)
    {
        private int _state;

        public void Start()
        {
            if (Interlocked.CompareExchange(ref _state, 1, 0) != 0)
                throw new InvalidOperationException("The prepared turn has already been handled.");
            start();
        }

        public void Abort()
        {
            if (Interlocked.CompareExchange(ref _state, 2, 0) == 0) abort();
        }
    }

    public sealed class PreparedCancelTurn(
        CancelTurnResult result,
        Func<bool> commit)
    {
        private int _state;

        public CancelTurnResult Result { get; } = result;

        public void Commit()
        {
            if (Interlocked.CompareExchange(ref _state, 1, 0) != 0)
                throw new InvalidOperationException("The prepared cancellation has already been handled.");
            _ = commit();
        }

        public void Abort() => Interlocked.CompareExchange(ref _state, 2, 0);
    }

    private sealed class PendingApproval
    {
        private const int Pending = 0;
        private const int Prepared = 1;
        private const int Completed = 2;
        private readonly TaskCompletionSource<ApprovalResolution> _completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _state;

        public Task<ApprovalResolution> Completion => _completion.Task;

        public bool TryPrepare() =>
            Interlocked.CompareExchange(ref _state, Prepared, Pending) == Pending;

        public void Commit(ApprovalResolution resolution)
        {
            if (Interlocked.CompareExchange(ref _state, Completed, Prepared) == Prepared)
                _completion.TrySetResult(resolution);
        }

        public void Abort()
        {
            if (Interlocked.CompareExchange(ref _state, Completed, Prepared) == Prepared)
                _completion.TrySetResult(new ApprovalResolution(false, null));
        }

        public void Stop()
        {
            var previous = Interlocked.Exchange(ref _state, Completed);
            if (previous is Pending or Prepared)
                _completion.TrySetResult(new ApprovalResolution(false, null));
        }
    }

    public sealed class PreparedApprovalResponse
    {
        private readonly Action _commit;
        private readonly Action _abort;
        private int _state;

        private PreparedApprovalResponse(ApprovalRespondResult result)
            : this(result, static () => { }, static () => { })
        {
        }

        internal PreparedApprovalResponse(
            ApprovalRespondResult result,
            Action commit,
            Action abort)
        {
            Result = result;
            _commit = commit;
            _abort = abort;
        }

        public ApprovalRespondResult Result { get; }

        internal static PreparedApprovalResponse NotAccepted(Guid approvalId) =>
            new(new ApprovalRespondResult(approvalId, false));

        public void Commit()
        {
            if (Interlocked.CompareExchange(ref _state, 1, 0) != 0)
                throw new InvalidOperationException("The prepared approval response has already been handled.");
            _commit();
        }

        public void Abort()
        {
            if (Interlocked.CompareExchange(ref _state, 2, 0) == 0) _abort();
        }
    }
}

public sealed class ChatRequestException(
    string code,
    string message,
    bool retryable = false) : Exception(message)
{
    public string Code { get; } = code;
    public bool Retryable { get; } = retryable;
}
