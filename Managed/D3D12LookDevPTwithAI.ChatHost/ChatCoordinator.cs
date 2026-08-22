using System.Collections.Concurrent;
using System.Text;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.ChatHost;

public sealed class ChatCoordinator(IConversationStore conversationStore)
{
    private const int MaximumInputCharacters = 64 * 1024;
    private const int PlaceholderChunkCharacters = 12;
    private static readonly TimeSpan TerminalEventWriteTimeout = TimeSpan.FromSeconds(2);
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
        if (title.Length > 200)
            throw new ChatRequestException("invalid_title", "Conversation title must not exceed 200 characters.");

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
        var (projectContextKey, _) = GetInitializedState();
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
        lock (_gate) _activeConversationId = conversation.Id;
        return new ConversationSelectResult(conversation, messages);
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
        if (request.Text.Length > MaximumInputCharacters)
            throw new ChatRequestException("invalid_turn", $"Turn text must not exceed {MaximumInputCharacters} characters.");

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
            var activeTurn = new ActiveTurn(request.TurnId, cancellation);
            _activeTurn = activeTurn;
            return new PreparedTurn(
                start: () =>
                {
                    var task = RunPlaceholderTurnAsync(
                        activeTurn,
                        projectContextKey,
                        request,
                        requestId,
                        peer);
                    activeTurn.Task = task;
                },
                abort: () =>
                {
                    lock (_gate)
                    {
                        if (ReferenceEquals(_activeTurn, activeTurn)) _activeTurn = null;
                    }
                    activeTurn.Cancellation.Cancel();
                    activeTurn.Cancellation.Dispose();
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
            try { await active.Task.WaitAsync(cancellationToken).ConfigureAwait(false); }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested || active.Cancellation.IsCancellationRequested) { }
        }
    }

    private async Task RunPlaceholderTurnAsync(
        ActiveTurn activeTurn,
        string projectContextKey,
        SendTurnRequest request,
        Guid requestId,
        IPipePeer peer)
    {
        await Task.Yield();
        var cancellationToken = activeTurn.Cancellation.Token;
        var userMessage = new ConversationMessage(
            Guid.NewGuid(),
            request.ConversationId,
            "user",
            request.Text.Trim(),
            DateTimeOffset.UtcNow);
        var assistantMessageId = Guid.NewGuid();
        var response = $"[Local ChatHost placeholder] {request.Text.Trim()}";
        var visibleResponse = new StringBuilder();
        var assistantStored = false;
        try
        {
            await conversationStore.AppendMessageAsync(
                projectContextKey,
                userMessage,
                CancellationToken.None).ConfigureAwait(false);
            await peer.SendEventAsync(
                requestId,
                "messageAdded",
                new MessageAddedEvent(request.TurnId, userMessage),
                cancellationToken).ConfigureAwait(false);
            await peer.SendEventAsync(
                requestId,
                "runtimeState",
                new RuntimeStateEvent("ready"),
                cancellationToken).ConfigureAwait(false);

            for (var offset = 0; offset < response.Length; offset += PlaceholderChunkCharacters)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var length = Math.Min(PlaceholderChunkCharacters, response.Length - offset);
                var delta = response.Substring(offset, length);
                visibleResponse.Append(delta);
                await peer.SendEventAsync(
                    requestId,
                    "textDelta",
                    new TextDeltaEvent(request.TurnId, assistantMessageId, delta),
                    cancellationToken).ConfigureAwait(false);
                await Task.Delay(15, cancellationToken).ConfigureAwait(false);
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
                CancellationToken.None).ConfigureAwait(false);
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
                    await conversationStore.AppendMessageAsync(
                        projectContextKey,
                        new ConversationMessage(
                            assistantMessageId,
                            request.ConversationId,
                            "assistant",
                            visibleResponse.ToString(),
                            DateTimeOffset.UtcNow),
                        CancellationToken.None).ConfigureAwait(false);
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
        finally
        {
            lock (_gate)
            {
                if (ReferenceEquals(_activeTurn, activeTurn)) _activeTurn = null;
            }
            activeTurn.Cancellation.Dispose();
        }
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

    private sealed class ActiveTurn(Guid turnId, CancellationTokenSource cancellation)
    {
        public Guid TurnId { get; } = turnId;
        public CancellationTokenSource Cancellation { get; } = cancellation;
        public Task Task { get; set; } = Task.CompletedTask;

        public bool TryCancel()
        {
            try
            {
                Cancellation.Cancel();
                return true;
            }
            catch (ObjectDisposedException)
            {
                return false;
            }
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
