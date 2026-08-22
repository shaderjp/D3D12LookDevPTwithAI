using System.Collections.Concurrent;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.ChatHost;

public sealed class ChatCoordinator(
    IConversationStore conversationStore,
    IChatInferenceRuntime inferenceRuntime)
{
    private const int InferenceHistoryMessageLimit = 64;
    private static readonly TimeSpan CancelledPartialPersistenceTimeout =
        TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan TerminalEventWriteTimeout =
        TimeSpan.FromMilliseconds(250);
    private readonly object _gate = new();
    private readonly ConcurrentDictionary<Guid, TaskCompletionSource<ApprovalResolution>> _approvals = new();
    private ActiveTurn? _activeTurn;
    private Guid? _activeConversationId;
    private string? _instanceId;
    private string? _projectContextKey;

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
            if (_instanceId is not null && !string.Equals(_instanceId, request.InstanceId, StringComparison.Ordinal))
                throw new ChatRequestException("already_initialized", "ChatHost is already initialized for another native instance.");
            _instanceId = request.InstanceId;
            _projectContextKey = request.ProjectContextKey;
        }

        try
        {
            await conversationStore.InitializeAsync(cancellationToken).ConfigureAwait(false);
            IReadOnlyList<ConversationSummary> conversations =
                await conversationStore.ListAsync(request.ProjectContextKey, cancellationToken).ConfigureAwait(false);
            if (conversations.Count == 0)
            {
                var created = await conversationStore.CreateAsync(
                    request.ProjectContextKey,
                    "新しいチャット",
                    cancellationToken).ConfigureAwait(false);
                conversations = [created];
            }

            lock (_gate) _activeConversationId = conversations[0].Id;
            return new InitializeResult(
                typeof(ChatCoordinator).Assembly.GetName().Version?.ToString() ?? "1.0.0",
                conversations[0].Id,
                conversations);
        }
        catch
        {
            lock (_gate)
            {
                _instanceId = null;
                _projectContextKey = null;
                _activeConversationId = null;
            }
            throw;
        }
    }

    public async Task<ConversationListResult> ListConversationsAsync(
        CancellationToken cancellationToken = default)
    {
        var (projectContextKey, activeConversationId) = GetInitializedState();
        var conversations = await conversationStore.ListAsync(projectContextKey, cancellationToken).ConfigureAwait(false);
        return new ConversationListResult(activeConversationId, conversations);
    }

    public async Task<ConversationCreateResult> CreateConversationAsync(
        ConversationCreateRequest request,
        CancellationToken cancellationToken = default)
    {
        var (projectContextKey, _) = GetInitializedState();
        var title = string.IsNullOrWhiteSpace(request.Title) ? "新しいチャット" : request.Title.Trim();
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

            var cancellation = CancellationTokenSource.CreateLinkedTokenSource(hostCancellationToken);
            var activeTurn = new ActiveTurn(
                request.TurnId,
                cancellation,
                turnCancellationToken => RunInferenceTurnAsync(
                    projectContextKey,
                    request,
                    requestId,
                    peer,
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

    public CancelTurnResult CancelTurn(CancelTurnRequest request)
    {
        _ = GetInitializedState();
        ActiveTurn? active;
        lock (_gate) active = _activeTurn?.TurnId == request.TurnId ? _activeTurn : null;
        var cancelled = active?.TryCancel() == true;
        return new CancelTurnResult(request.TurnId, cancelled);
    }

    public async Task<ApprovalResolution> WaitForApprovalAsync(
        Guid approvalId,
        CancellationToken cancellationToken = default)
    {
        if (approvalId == Guid.Empty)
            throw new ArgumentException("approvalId is required.", nameof(approvalId));
        var completion = new TaskCompletionSource<ApprovalResolution>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_approvals.TryAdd(approvalId, completion))
            throw new InvalidOperationException("The approval identifier is already pending.");
        try
        {
            return await completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _approvals.TryRemove(approvalId, out _);
        }
    }

    public ApprovalRespondResult RespondToApproval(ApprovalRespondRequest request)
    {
        _ = GetInitializedState();
        var allowed = string.Equals(request.Decision, "allowOnce", StringComparison.Ordinal);
        var denied = string.Equals(request.Decision, "deny", StringComparison.Ordinal);
        if (!allowed && !denied)
            throw new ChatRequestException("invalid_approval", "decision must be allowOnce or deny.");
        if (allowed && string.IsNullOrWhiteSpace(request.ApprovalGrant))
            throw new ChatRequestException("invalid_approval", "allowOnce requires an approvalGrant.");
        if (!_approvals.TryRemove(request.ApprovalId, out var completion))
            return new ApprovalRespondResult(request.ApprovalId, false);

        completion.TrySetResult(new ApprovalResolution(allowed, allowed ? request.ApprovalGrant : null));
        return new ApprovalRespondResult(request.ApprovalId, true);
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        ActiveTurn? active;
        lock (_gate) active = _activeTurn;
        active?.TryCancel();
        foreach (var approval in _approvals.ToArray())
        {
            if (_approvals.TryRemove(approval.Key, out var completion))
                completion.TrySetResult(new ApprovalResolution(false, null));
        }
        if (active is not null)
        {
            try { await active.Completion.WaitAsync(cancellationToken).ConfigureAwait(false); }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested || active.IsCancellationRequested) { }
        }
    }

    private async Task RunInferenceTurnAsync(
        string projectContextKey,
        SendTurnRequest request,
        Guid requestId,
        IPipePeer peer,
        CancellationToken cancellationToken)
    {
        await Task.Yield();
        var assistantMessageId = Guid.NewGuid();
        var visibleResponse = new StringBuilder();
        var assistantStored = false;
        try
        {
            var persistedHistory = await conversationStore.ListMessagesBeforeAsync(
                projectContextKey,
                request.ConversationId,
                beforeMessageSequence: null,
                InferenceHistoryMessageLimit,
                cancellationToken).ConfigureAwait(false);
            var inferenceRequest = new ChatInferenceRequest(
                request.ConversationId,
                projectContextKey,
                BuildInferenceHistory(persistedHistory),
                request.Text.Trim());
            var userMessage = new ConversationMessage(
                Guid.NewGuid(),
                request.ConversationId,
                "user",
                request.Text.Trim(),
                DateTimeOffset.UtcNow);
            await conversationStore.AppendMessageAsync(
                projectContextKey,
                userMessage,
                cancellationToken).ConfigureAwait(false);
            await peer.SendEventAsync(
                requestId,
                "messageAdded",
                new MessageAddedEvent(request.TurnId, userMessage),
                cancellationToken).ConfigureAwait(false);
            var runtimeStatus = await inferenceRuntime.GetStatusAsync(
                cancellationToken).ConfigureAwait(false);
            if (!IsSafeRuntimeToken(
                    runtimeStatus.RuntimeId,
                    ChatInferenceLimits.MaximumRuntimeIdentifierCharacters) ||
                !IsSafeRuntimeToken(
                    runtimeStatus.State,
                    ChatInferenceLimits.MaximumRuntimeStateCharacters))
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
                new RuntimeStateEvent(runtimeStatus.State, runtimeStatus.RuntimeId),
                cancellationToken).ConfigureAwait(false);

            await foreach (var chunk in inferenceRuntime.StreamAsync(
                inferenceRequest,
                cancellationToken).WithCancellation(cancellationToken).ConfigureAwait(false))
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (string.IsNullOrEmpty(chunk.Text))
                {
                    throw new ChatInferenceException(
                        "invalid_inference_output",
                        "The local inference runtime returned an empty text chunk.");
                }
                if (visibleResponse.Length + chunk.Text.Length >
                    ChatInferenceLimits.MaximumOutputCharacters)
                {
                    throw new ChatInferenceException(
                        "inference_output_too_large",
                        $"The local response exceeded {ChatInferenceLimits.MaximumOutputCharacters} characters.");
                }
                visibleResponse.Append(chunk.Text);
                await peer.SendEventAsync(
                    requestId,
                    "textDelta",
                    new TextDeltaEvent(request.TurnId, assistantMessageId, chunk.Text),
                    cancellationToken).ConfigureAwait(false);
            }
            if (visibleResponse.Length == 0)
            {
                throw new ChatInferenceException(
                    "empty_inference_response",
                    "The local inference runtime returned no response.");
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
                new TurnCompletedEvent(request.TurnId, "completed"),
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
                new TurnCompletedEvent(request.TurnId, "cancelled")).ConfigureAwait(false);
        }
        catch (ChatInferenceException exception)
        {
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "error",
                BuildPublicInferenceError(request.TurnId, exception)).ConfigureAwait(false);
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "completed",
                new TurnCompletedEvent(request.TurnId, "failed")).ConfigureAwait(false);
        }
        catch (Exception)
        {
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "error",
                new ErrorEvent(request.TurnId, "turn_failed", "The local chat turn failed.")).ConfigureAwait(false);
            await SendWithoutTurnCancellationAsync(
                peer,
                requestId,
                "completed",
                new TurnCompletedEvent(request.TurnId, "failed")).ConfigureAwait(false);
        }
    }

    private static ErrorEvent BuildPublicInferenceError(
        Guid turnId,
        ChatInferenceException exception) => exception.Code switch
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
            _ => new ErrorEvent(
                turnId,
                "inference_runtime_failed",
                "The local inference runtime failed."),
        };

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
}

public sealed class ChatRequestException(
    string code,
    string message,
    bool retryable = false) : Exception(message)
{
    public string Code { get; } = code;
    public bool Retryable { get; } = retryable;
}
