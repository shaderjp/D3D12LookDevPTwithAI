using System.Diagnostics;
using System.Text.Json;
using System.Threading.Channels;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;
using D3D12LookDevPTwithAI.ChatHost;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class PipeRequestRouterTests
{
    [Fact]
    public async Task Initialize_and_conversation_requests_operate_on_the_persisted_store()
    {
        await using var fixture = new RouterFixture();
        var initialize = fixture.Request("initialize", new InitializeRequest("instance-1", "project-1"));
        await fixture.Router.HandleAsync(initialize, fixture.Peer);
        var initialized = await fixture.Peer.ReadAsync();
        var initializeResult = initialized.Payload.Deserialize<InitializeResult>(PipeJson.SerializerOptions)!;
        Assert.Null(initialized.Error);
        Assert.Single(initializeResult.Conversations);

        await fixture.Router.HandleAsync(
            fixture.Request("conversation.create", new ConversationCreateRequest("Lighting review")),
            fixture.Peer);
        var createdResponse = await fixture.Peer.ReadAsync();
        var created = createdResponse.Payload.Deserialize<ConversationCreateResult>(PipeJson.SerializerOptions)!;
        Assert.Equal("Lighting review", created.Conversation.Title);

        await fixture.Router.HandleAsync(
            fixture.Request("conversation.list", new { }),
            fixture.Peer);
        var listResponse = await fixture.Peer.ReadAsync();
        var list = listResponse.Payload.Deserialize<ConversationListResult>(PipeJson.SerializerOptions)!;
        Assert.Equal(2, list.Conversations.Count);
        Assert.Equal(created.Conversation.Id, list.ActiveConversationId);

        await fixture.Router.HandleAsync(
            fixture.Request("conversation.select", new ConversationSelectRequest(initializeResult.ActiveConversationId)),
            fixture.Peer);
        var selectedResponse = await fixture.Peer.ReadAsync();
        var selected = selectedResponse.Payload.Deserialize<ConversationSelectResult>(PipeJson.SerializerOptions)!;
        Assert.Equal(initializeResult.ActiveConversationId, selected.Conversation.Id);
        Assert.Empty(selected.Messages);
    }

    [Fact]
    public async Task Send_turn_responds_before_streaming_deterministic_background_events()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        var send = fixture.Request("sendTurn", new SendTurnRequest(turnId, conversationId, "露出を確認"));

        await fixture.Router.HandleAsync(send, fixture.Peer);

        var accepted = await fixture.Peer.ReadAsync();
        Assert.Equal(PipeMessageKind.Response, accepted.Kind);
        Assert.True(accepted.Payload.GetProperty("accepted").GetBoolean());

        var methods = new List<string>();
        PipeEnvelope completed;
        do
        {
            completed = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            methods.Add(completed.Method);
        }
        while (completed.Method != "completed");

        Assert.Contains("messageAdded", methods);
        Assert.Contains("runtimeState", methods);
        Assert.Contains("textDelta", methods);
        Assert.Equal("completed", completed.Payload.GetProperty("status").GetString());
    }

    [Fact]
    public async Task Failed_send_turn_acceptance_does_not_leave_the_coordinator_busy()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();

        await Assert.ThrowsAsync<IOException>(() => fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "first")),
            new AlwaysFailingPipePeer()));

        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "second")),
            fixture.Peer);
        var accepted = await fixture.Peer.ReadAsync();
        Assert.Null(accepted.Error);
        Assert.True(accepted.Payload.GetProperty("accepted").GetBoolean());
    }

    [Fact]
    public async Task Cancel_turn_remains_responsive_while_events_are_streaming()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        await fixture.Router.HandleAsync(
            fixture.Request("sendTurn", new SendTurnRequest(turnId, conversationId, new string('x', 2000))),
            fixture.Peer);
        _ = await fixture.Peer.ReadAsync();

        await fixture.Router.HandleAsync(
            fixture.Request("cancelTurn", new CancelTurnRequest(turnId)),
            fixture.Peer);

        var sawCancelResponse = false;
        var sawCancelledCompletion = false;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        while (!sawCancelResponse || !sawCancelledCompletion)
        {
            var envelope = await fixture.Peer.ReadAsync(timeout.Token);
            if (envelope.Kind == PipeMessageKind.Response && envelope.Method == "cancelTurn")
                sawCancelResponse = envelope.Payload.GetProperty("cancelRequested").GetBoolean();
            if (envelope.Kind == PipeMessageKind.Event && envelope.Method == "completed")
                sawCancelledCompletion = envelope.Payload.GetProperty("status").GetString() == "cancelled";
        }

        Assert.True(sawCancelResponse);
        Assert.True(sawCancelledCompletion);
    }

    [Fact]
    public async Task Approval_response_completes_only_a_registered_pending_approval()
    {
        await using var fixture = new RouterFixture();
        _ = await fixture.InitializeAsync();
        var approvalId = Guid.NewGuid();
        var pending = fixture.Coordinator.WaitForApprovalAsync(approvalId);

        await fixture.Router.HandleAsync(
            fixture.Request("approval.respond", new ApprovalRespondRequest(approvalId, "allowOnce", "one-time-grant")),
            fixture.Peer);

        var response = await fixture.Peer.ReadAsync();
        var resolution = await pending;
        Assert.True(response.Payload.GetProperty("accepted").GetBoolean());
        Assert.True(resolution.Allowed);
        Assert.Equal("one-time-grant", resolution.ApprovalGrant);
    }

    [Fact]
    public async Task Requests_before_initialize_return_a_protocol_error_response()
    {
        await using var fixture = new RouterFixture();

        await fixture.Router.HandleAsync(
            fixture.Request("conversation.list", new { }),
            fixture.Peer);

        var response = await fixture.Peer.ReadAsync();
        Assert.Equal("not_initialized", response.Error?.Code);
    }

    [Fact]
    public async Task Invalid_payload_response_does_not_expose_payload_values_or_json_paths()
    {
        await using var fixture = new RouterFixture();
        _ = await fixture.InitializeAsync();
        const string sensitiveMarker = "private-scene-path-marker";

        await fixture.Router.HandleAsync(
            fixture.Request(
                "conversation.select",
                new { conversationId = new { value = sensitiveMarker } }),
            fixture.Peer);

        var response = await fixture.Peer.ReadAsync();
        var error = Assert.IsType<PipeError>(response.Error);
        Assert.Equal("invalid_payload", error.Code);
        Assert.Equal("The request payload is invalid.", error.Message);
        Assert.DoesNotContain(sensitiveMarker, error.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("conversationId", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task Shutdown_acknowledges_before_requesting_host_stop()
    {
        await using var fixture = new RouterFixture();
        _ = await fixture.InitializeAsync();

        await fixture.Router.HandleAsync(fixture.Request("shutdown", new { }), fixture.Peer);

        var response = await fixture.Peer.ReadAsync();
        Assert.True(response.Payload.GetProperty("accepted").GetBoolean());
        Assert.True(fixture.Lifetime.ApplicationStopping.IsCancellationRequested);
    }

    [Fact]
    public async Task Stop_is_bounded_when_the_native_pipe_stops_reading_events()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var blockedPeer = new BlockingEventPipePeer();
        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "blocked event")),
            blockedPeer);
        await blockedPeer.EventWriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));

        var stopwatch = Stopwatch.StartNew();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await fixture.Coordinator.StopAsync(timeout.Token);

        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(3));
    }

    [Fact]
    public void Command_line_requires_a_simple_pipe_name_and_positive_parent_pid()
    {
        Assert.True(CommandLineOptions.TryParse(
            ["--pipe-name", "LookDev.Chat.123", "--parent-pid", "42"],
            out var options,
            out _));
        Assert.Equal(42, options!.ParentProcessId);

        Assert.False(CommandLineOptions.TryParse(
            ["--pipe-name", "..\\spoof", "--parent-pid", "42"],
            out _,
            out _));
    }

    private sealed class RouterFixture : IAsyncDisposable
    {
        private long _sequence;
        private readonly TestApplicationLifetime _lifetime = new();
        private readonly string _dataDirectory = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI.Chat.Tests",
            Guid.NewGuid().ToString("N"));

        public RouterFixture()
        {
            Coordinator = new ChatCoordinator(new SqliteConversationStore(new AppPaths(_dataDirectory)));
            Router = new PipeRequestRouter(Coordinator, _lifetime);
        }

        public ChatCoordinator Coordinator { get; }
        public PipeRequestRouter Router { get; }
        public RecordingPipePeer Peer { get; } = new();
        public TestApplicationLifetime Lifetime => _lifetime;

        public PipeEnvelope Request<T>(string method, T payload) => new()
        {
            Kind = PipeMessageKind.Request,
            RequestId = Guid.NewGuid(),
            Sequence = Interlocked.Increment(ref _sequence),
            Method = method,
            Payload = PipeJson.ToElement(payload),
        };

        public async Task<Guid> InitializeAsync()
        {
            await Router.HandleAsync(
                Request("initialize", new InitializeRequest("instance-1", "project-1")),
                Peer);
            var response = await Peer.ReadAsync();
            return response.Payload.Deserialize<InitializeResult>(PipeJson.SerializerOptions)!.ActiveConversationId;
        }

        public async ValueTask DisposeAsync()
        {
            await Coordinator.StopAsync();
            if (Directory.Exists(_dataDirectory)) Directory.Delete(_dataDirectory, recursive: true);
        }
    }

    private sealed class AlwaysFailingPipePeer : IPipePeer
    {
        public Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default) =>
            Task.FromException(new IOException("Simulated disconnected pipe."));

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default) =>
            Task.FromException(new IOException("Simulated disconnected pipe."));
    }

    private sealed class BlockingEventPipePeer : IPipePeer
    {
        public TaskCompletionSource EventWriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default) =>
            Task.CompletedTask;

        public async Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default)
        {
            EventWriteStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
        }
    }

    private sealed class RecordingPipePeer : IPipePeer
    {
        private readonly Channel<PipeEnvelope> _messages = Channel.CreateUnbounded<PipeEnvelope>();
        private long _sequence;

        public Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default)
        {
            var response = new PipeEnvelope
            {
                Kind = PipeMessageKind.Response,
                RequestId = request.RequestId,
                Sequence = Interlocked.Increment(ref _sequence),
                Method = request.Method,
                Payload = PipeJson.ToElement(payload),
                Error = error,
            };
            return _messages.Writer.WriteAsync(response, cancellationToken).AsTask();
        }

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default)
        {
            var eventEnvelope = new PipeEnvelope
            {
                Kind = PipeMessageKind.Event,
                RequestId = requestId,
                Sequence = Interlocked.Increment(ref _sequence),
                Method = method,
                Payload = PipeJson.ToElement(payload),
            };
            return _messages.Writer.WriteAsync(eventEnvelope, cancellationToken).AsTask();
        }

        public Task<PipeEnvelope> ReadAsync(CancellationToken cancellationToken = default) =>
            _messages.Reader.ReadAsync(cancellationToken).AsTask();

        public async Task<PipeEnvelope> ReadAsync(TimeSpan timeout)
        {
            using var cancellation = new CancellationTokenSource(timeout);
            return await ReadAsync(cancellation.Token);
        }
    }

    private sealed class TestApplicationLifetime : IHostApplicationLifetime
    {
        private readonly CancellationTokenSource _started = new();
        private readonly CancellationTokenSource _stopping = new();
        private readonly CancellationTokenSource _stopped = new();

        public CancellationToken ApplicationStarted => _started.Token;
        public CancellationToken ApplicationStopping => _stopping.Token;
        public CancellationToken ApplicationStopped => _stopped.Token;
        public void StopApplication() => _stopping.Cancel();
    }
}
