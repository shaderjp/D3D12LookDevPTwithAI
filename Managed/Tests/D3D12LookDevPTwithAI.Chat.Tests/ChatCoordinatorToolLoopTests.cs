using System.IO.Pipes;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading.Channels;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.ChatHost;
using D3D12LookDevPTwithAI.ChatHost.Inference;
using D3D12LookDevPTwithAI.ChatHost.Mcp;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class ChatCoordinatorToolLoopTests
{
    private const string ApprovalGrant =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    [Fact]
    public async Task Oversized_catalog_fails_initialization_transactionally()
    {
        var tools = Enumerable.Range(0, ChatInferenceLimits.MaximumTools + 1)
            .Select(index => Tool($"scene.get.{index}", isReadOnly: true))
            .ToArray();
        var mcp = new FakeMcpClient(tools);
        await using var fixture = new ToolLoopFixture(
            new ScriptedToolRuntime(TextRound("unused")),
            mcp);
        var initialize = fixture.Request(
            "initialize",
            new InitializeRequest(
                "instance-tools",
                "project-tools",
                "http://127.0.0.1:43123/mcp",
                "private-token"));

        await fixture.Router.HandleAsync(initialize, fixture.Peer);
        var response = await fixture.Peer.ReadAsync();

        Assert.Equal("mcp_initialization_failed", response.Error?.Code);
        Assert.True(response.Error?.Retryable);
        Assert.True(mcp.IsDisposed);
    }

    [Fact]
    public async Task Read_only_tool_runs_automatically_and_only_final_text_is_visible_and_persisted()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound("private prelude", ToolCall("call-1", "scene.get", "{\"path\":\"camera\"}")),
            TextRound("最終回答"));
        var mcp = new FakeMcpClient(Tool("scene.get", isReadOnly: true));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();

        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(turnId, conversationId, "カメラを確認"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        var response = await fixture.Peer.ReadAsync();
        Assert.Equal(PipeMessageKind.Response, response.Kind);
        Assert.Equal(sendRequest.RequestId, response.RequestId);

        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);
        Assert.Equal(
            ["messageAdded", "runtimeState", "toolStarted", "toolCompleted", "textDelta", "messageAdded", "completed"],
            events.Select(message => message.Method));
        Assert.DoesNotContain("private prelude", Serialize(events));
        Assert.Equal(
            "最終回答",
            events.Single(message => message.Method == "textDelta")
                .Payload.Deserialize<TextDeltaEvent>(PipeJson.SerializerOptions)!.Delta);

        Assert.Single(mcp.Calls);
        Assert.Null(mcp.Calls[0].ApprovalGrant);
        Assert.Equal("scene.get", mcp.Calls[0].Tool);
        Assert.Equal(2, runtime.Requests.Count);
        Assert.Single(runtime.Requests[0].Tools!);
        Assert.True(runtime.Requests[0].AppendUserMessage);
        Assert.False(runtime.Requests[1].AppendUserMessage);
        Assert.Equal(string.Empty, runtime.Requests[1].UserText);
        Assert.Collection(
            runtime.Requests[1].History.TakeLast(3),
            message => Assert.Equal(ChatInferenceRole.User, message.Role),
            message =>
            {
                Assert.Equal(ChatInferenceRole.Assistant, message.Role);
                Assert.Equal("private prelude", message.Content);
                Assert.Single(message.ToolCalls!);
            },
            message =>
            {
                Assert.Equal(ChatInferenceRole.Tool, message.Role);
                Assert.Equal("call-1", message.ToolCallId);
                Assert.Equal("scene.get", message.Name);
                Assert.Equal("{\"ok\":true}", message.Content);
                Assert.DoesNotContain("private-base64-marker", message.Content);
            });

        var stored = await fixture.Store.GetMessagesAsync("project-tools", conversationId);
        Assert.Equal(["user", "assistant"], stored.Select(message => message.Role));
        Assert.Equal("最終回答", stored[^1].Content);
        Assert.DoesNotContain(stored, message => message.Content.Contains("private prelude", StringComparison.Ordinal));
    }

    [Fact]
    public async Task Mutation_waits_for_approval_response_before_start_and_uses_grant_once()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("mutate-1", "material.set", "{\"z\":2,\"a\":1}")),
            TextRound("変更しました"));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(turnId, conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();

        var beforeApproval = await fixture.Peer.ReadUntilAsync(
            sendRequest.RequestId,
            "toolApprovalRequired");
        var approvalEvent = beforeApproval[^1].Payload.Deserialize<ToolApprovalRequiredEvent>(
            PipeJson.SerializerOptions)!;
        Assert.Equal("Run material.set", approvalEvent.Summary);
        Assert.Equal("{\"a\":1e0,\"z\":2e0}", approvalEvent.ArgumentsJson);
        Assert.Equal(64, approvalEvent.ArgumentsHash.Length);
        Assert.Empty(mcp.Calls);

        var gatedPeer = new GatedResponsePeer(fixture.Peer);
        var approvalRouting = fixture.Router.HandleAsync(
            fixture.Request(
                "approval.respond",
                new ApprovalRespondRequest(
                    approvalEvent.ApprovalId,
                    "allowOnce",
                    ApprovalGrant)),
            gatedPeer);
        await gatedPeer.ResponseStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await Task.Delay(50);
        Assert.Empty(mcp.Calls);
        Assert.DoesNotContain(fixture.Peer.Snapshot(), message => message.Method == "toolStarted");

        gatedPeer.ReleaseResponse.TrySetResult();
        await approvalRouting;
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);
        Assert.Equal("toolStarted", events.First(message => message.Method == "toolStarted").Method);
        Assert.Single(mcp.Calls);
        Assert.Equal(ApprovalGrant, mcp.Calls[0].ApprovalGrant);
        Assert.False(runtime.Requests[^1].AppendUserMessage);
    }

    [Fact]
    public async Task Denial_returns_fixed_tool_result_without_calling_mcp()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("mutate-deny", "material.set", "{}")),
            TextRound("変更しませんでした"));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var approvalMessages = await fixture.Peer.ReadUntilAsync(
            sendRequest.RequestId,
            "toolApprovalRequired");
        var approval = approvalMessages[^1].Payload.Deserialize<ToolApprovalRequiredEvent>(
            PipeJson.SerializerOptions)!;

        await fixture.Router.HandleAsync(
            fixture.Request(
                "approval.respond",
                new ApprovalRespondRequest(approval.ApprovalId, "deny")),
            fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Empty(mcp.Calls);
        Assert.DoesNotContain(events, message => message.Method == "toolStarted");
        var completed = events.Single(message => message.Method == "toolCompleted")
            .Payload.Deserialize<ToolCompletedEvent>(PipeJson.SerializerOptions)!;
        Assert.Equal("denied", completed.Status);
        Assert.Equal("user_denied", completed.Code);
        Assert.Equal(
            "{\"ok\":false,\"code\":\"user_denied\"}",
            runtime.Requests[1].History[^1].Content);
    }

    [Fact]
    public async Task Cancelling_while_waiting_for_approval_never_calls_the_tool()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("mutate-cancel", "material.set", "{}")));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(turnId, conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var approvalMessages = await fixture.Peer.ReadUntilAsync(
            sendRequest.RequestId,
            "toolApprovalRequired");
        var approval = approvalMessages[^1].Payload.Deserialize<ToolApprovalRequiredEvent>(
            PipeJson.SerializerOptions)!;

        var cancel = fixture.Request("cancelTurn", new CancelTurnRequest(turnId));
        await fixture.Router.HandleAsync(cancel, fixture.Peer);
        var cancelResponse = await fixture.Peer.ReadAsync();
        Assert.Equal(cancel.RequestId, cancelResponse.RequestId);
        var terminal = await fixture.Peer.ReadUntilAsync(sendRequest.RequestId, "completed");
        Assert.Equal(
            "cancelled",
            terminal[^1].Payload.Deserialize<TurnCompletedEvent>(PipeJson.SerializerOptions)!.Status);
        Assert.Empty(mcp.Calls);

        var lateResponse = fixture.Request(
            "approval.respond",
            new ApprovalRespondRequest(approval.ApprovalId, "allowOnce", ApprovalGrant));
        await fixture.Router.HandleAsync(lateResponse, fixture.Peer);
        var response = await fixture.Peer.ReadAsync();
        Assert.False(response.Payload.Deserialize<ApprovalRespondResult>(PipeJson.SerializerOptions)!.Accepted);
    }

    [Fact]
    public async Task Cancellation_after_tool_start_reports_unknown_execution_outcome()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("read-cancel", "scene.get", "{}")));
        var mcp = new FakeMcpClient(Tool("scene.get", isReadOnly: true))
        {
            BlockCallUntilCancelled = true,
        };
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(turnId, conversationId, "調べて"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        _ = await fixture.Peer.ReadUntilAsync(sendRequest.RequestId, "toolStarted");
        await mcp.CallStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

        var cancel = fixture.Request("cancelTurn", new CancelTurnRequest(turnId));
        await fixture.Router.HandleAsync(cancel, fixture.Peer);
        var cancelResponse = await fixture.Peer.ReadAsync();
        Assert.Equal(cancel.RequestId, cancelResponse.RequestId);
        var terminal = await fixture.Peer.ReadUntilAsync(sendRequest.RequestId, "completed");

        Assert.Single(mcp.Calls);
        var toolCompleted = terminal.Single(message => message.Method == "toolCompleted")
            .Payload.Deserialize<ToolCompletedEvent>(PipeJson.SerializerOptions)!;
        Assert.Equal("unknown", toolCompleted.Status);
        Assert.Equal("cancelled_after_start", toolCompleted.Code);
        Assert.Equal(
            "cancelled",
            terminal[^1].Payload.Deserialize<TurnCompletedEvent>(PipeJson.SerializerOptions)!.Status);
    }

    [Fact]
    public async Task Tool_failure_and_approval_session_expiry_are_sanitized_and_never_retried()
    {
        const string sensitiveMarker = "C:\\secret\\scene-marker.gltf";
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("mutate-expired", "material.set", "{}")),
            TextRound("再承認が必要です"));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false))
        {
            CallFailure = new SameInstanceMcpException(
                "approval_session_expired",
                sensitiveMarker),
        };
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var approvalMessages = await fixture.Peer.ReadUntilAsync(
            sendRequest.RequestId,
            "toolApprovalRequired");
        var approval = approvalMessages[^1].Payload.Deserialize<ToolApprovalRequiredEvent>(
            PipeJson.SerializerOptions)!;
        await fixture.Router.HandleAsync(
            fixture.Request(
                "approval.respond",
                new ApprovalRespondRequest(approval.ApprovalId, "allowOnce", ApprovalGrant)),
            fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Single(mcp.Calls);
        Assert.Equal(
            "{\"ok\":false,\"code\":\"approval_session_expired\"}",
            runtime.Requests[1].History[^1].Content);
        Assert.DoesNotContain(sensitiveMarker, Serialize(events));
        Assert.DoesNotContain(sensitiveMarker, Serialize(runtime.Requests));
    }

    [Fact]
    public async Task Generic_tool_failure_is_a_fixed_model_result_and_public_event()
    {
        const string sensitiveMarker = "private-tool-exception-marker";
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("read-fail", "scene.get", "{}")),
            TextRound("取得できませんでした"));
        var mcp = new FakeMcpClient(Tool("scene.get", isReadOnly: true))
        {
            CallFailure = new InvalidOperationException(sensitiveMarker),
        };
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "調べて"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Single(mcp.Calls);
        Assert.Equal(
            "{\"ok\":false,\"code\":\"tool_failed\"}",
            runtime.Requests[1].History[^1].Content);
        var completed = events.Single(message => message.Method == "toolCompleted")
            .Payload.Deserialize<ToolCompletedEvent>(PipeJson.SerializerOptions)!;
        Assert.Equal("failed", completed.Status);
        Assert.Equal("tool_failed", completed.Code);
        Assert.DoesNotContain(sensitiveMarker, Serialize(events));
    }

    [Fact]
    public async Task Four_tool_rounds_are_bounded_and_the_final_request_disables_tools()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("read-1", "scene.get", "{}")),
            ToolRound(string.Empty, ToolCall("read-2", "scene.get", "{}")),
            ToolRound(string.Empty, ToolCall("read-3", "scene.get", "{}")),
            ToolRound(string.Empty, ToolCall("read-4", "scene.get", "{}")),
            TextRound("完了"));
        var mcp = new FakeMcpClient(Tool("scene.get", isReadOnly: true));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "調べて"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        _ = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Equal(4, mcp.Calls.Count);
        Assert.Equal(5, runtime.Requests.Count);
        Assert.NotNull(runtime.Requests[3].Tools);
        Assert.NotNull(runtime.Requests[4].Tools);
        Assert.False(runtime.Requests[4].AllowToolCalls);
        Assert.False(runtime.Requests[4].AppendUserMessage);
    }

    [Fact]
    public async Task Unknown_tool_makes_the_entire_batch_side_effect_free()
    {
        var runtime = new ScriptedToolRuntime(
            ToolRound(
                string.Empty,
                ToolCall("valid-mutation", "material.set", "{}"),
                ToolCall("unknown-call", "unknown.tool", "{}")));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Empty(mcp.Calls);
        Assert.Equal(0, mcp.BindingCallCount);
        Assert.DoesNotContain(events, message =>
            message.Method is "toolApprovalRequired" or "toolStarted");
        var error = events.Single(message => message.Method == "error")
            .Payload.Deserialize<ErrorEvent>(PipeJson.SerializerOptions)!;
        Assert.Equal("invalid_tool_calls", error.Code);
    }

    [Fact]
    public async Task Per_round_call_cap_is_side_effect_free()
    {
        var calls = Enumerable.Range(0, 5)
            .Select(index => ToolCall($"read-{index}", "scene.get", "{}"))
            .ToArray();
        var runtime = new ScriptedToolRuntime(ToolRound(string.Empty, calls));
        var mcp = new FakeMcpClient(Tool("scene.get", isReadOnly: true));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "調べて"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Empty(mcp.Calls);
        Assert.DoesNotContain(events, message => message.Method == "toolStarted");
        Assert.Equal(
            "tool_call_limit_exceeded",
            events.Single(message => message.Method == "error")
                .Payload.Deserialize<ErrorEvent>(PipeJson.SerializerOptions)!.Code);
    }

    [Fact]
    public async Task Mutation_arguments_over_approval_display_limit_fail_without_binding_or_prompt()
    {
        var arguments = "{\"value\":\"" + new string('x', 9 * 1024) + "\"}";
        var runtime = new ScriptedToolRuntime(
            ToolRound(string.Empty, ToolCall("too-large", "material.set", arguments)),
            TextRound("引数が大きすぎます"));
        var mcp = new FakeMcpClient(Tool("material.set", isReadOnly: false));
        await using var fixture = new ToolLoopFixture(runtime, mcp);
        var conversationId = await fixture.InitializeAsync();
        var sendRequest = fixture.Request(
            "sendTurn",
            new SendTurnRequest(Guid.NewGuid(), conversationId, "変更して"));
        await fixture.Router.HandleAsync(sendRequest, fixture.Peer);
        _ = await fixture.Peer.ReadAsync();
        var events = await fixture.Peer.ReadTurnEventsUntilCompletedAsync(sendRequest.RequestId);

        Assert.Equal(0, mcp.BindingCallCount);
        Assert.Empty(mcp.Calls);
        Assert.DoesNotContain(events, message => message.Method == "toolApprovalRequired");
        var completed = events.Single(message => message.Method == "toolCompleted")
            .Payload.Deserialize<ToolCompletedEvent>(PipeJson.SerializerOptions)!;
        Assert.Equal("approval_arguments_too_large", completed.Code);
    }

    [Fact]
    public async Task Real_named_pipe_orders_approval_response_before_tool_started_event()
    {
        var pipeName = "lookdev-approval-order-" + Guid.NewGuid().ToString("N");
        await using var server = new NamedPipeServerStream(
            pipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        await using var client = new NamedPipeClientStream(
            ".",
            pipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous);
        var accept = server.WaitForConnectionAsync();
        await client.ConnectAsync();
        await accept;

        var store = new MemoryConversationStore();
        var coordinator = new ChatCoordinator(store, new ScriptedToolRuntime(TextRound("unused")));
        await using var modelSetup = new LocalModelSetupCoordinator(
            new NoopModelSetupService());
        var router = new PipeRequestRouter(coordinator, modelSetup, new TestLifetime());
        await using var peer = new FramedStreamPeer(client);
        long requestSequence = 0;
        PipeEnvelope Request<T>(string method, T payload) => new()
        {
            Kind = PipeMessageKind.Request,
            RequestId = Guid.NewGuid(),
            Sequence = Interlocked.Increment(ref requestSequence),
            Method = method,
            Payload = PipeJson.ToElement(payload),
        };

        var initializeRead = PipeFraming.ReadAsync(server).AsTask();
        await router.HandleAsync(
            Request("initialize", new InitializeRequest("instance-pipe", "project-pipe")),
            peer);
        _ = await initializeRead.WaitAsync(TimeSpan.FromSeconds(5));
        var approvalId = Guid.NewGuid();
        var pending = coordinator.WaitForApprovalAsync(approvalId);
        var syntheticRequestId = Guid.NewGuid();
        var eventWrite = Task.Run(async () =>
        {
            _ = await pending;
            await peer.SendEventAsync(
                syntheticRequestId,
                "toolStarted",
                new ToolStartedEvent(Guid.NewGuid(), "pipe-call", "scene.get"));
        });

        var orderedFrames = ReadFramesAsync(server, 2);
        await router.HandleAsync(
            Request(
                "approval.respond",
                new ApprovalRespondRequest(approvalId, "allowOnce", ApprovalGrant)),
            peer);
        var frames = await orderedFrames.WaitAsync(TimeSpan.FromSeconds(5));
        await eventWrite.WaitAsync(TimeSpan.FromSeconds(5));
        var first = frames[0];
        var second = frames[1];

        Assert.NotNull(first);
        Assert.NotNull(second);
        Assert.Equal(PipeMessageKind.Response, first.Kind);
        Assert.Equal("approval.respond", first.Method);
        Assert.Equal(PipeMessageKind.Event, second.Kind);
        Assert.Equal("toolStarted", second.Method);
        Assert.True(first.Sequence < second.Sequence);
        await coordinator.StopAsync();
    }

    private static async Task<IReadOnlyList<PipeEnvelope>> ReadFramesAsync(
        Stream stream,
        int count)
    {
        var frames = new List<PipeEnvelope>(count);
        while (frames.Count < count)
        {
            var frame = await PipeFraming.ReadAsync(stream);
            Assert.NotNull(frame);
            frames.Add(frame);
        }
        return frames;
    }

    private static SameInstanceMcpTool Tool(string name, bool isReadOnly) => new(
        name,
        "test tool",
        Json("{\"type\":\"object\",\"additionalProperties\":true}"),
        isReadOnly);

    private static ChatInferenceToolCall ToolCall(string id, string name, string arguments) =>
        new(id, name, arguments);

    private static IReadOnlyList<ChatInferenceChunk> ToolRound(
        string text,
        params ChatInferenceToolCall[] calls) =>
        string.IsNullOrEmpty(text)
            ? [new ChatInferenceChunk(string.Empty, calls)]
            : [new ChatInferenceChunk(text), new ChatInferenceChunk(string.Empty, calls)];

    private static IReadOnlyList<ChatInferenceChunk> TextRound(params string[] text) =>
        text.Select(value => new ChatInferenceChunk(value)).ToArray();

    private static JsonElement Json(string value)
    {
        using var document = JsonDocument.Parse(value);
        return document.RootElement.Clone();
    }

    private static string Serialize<T>(T value) =>
        JsonSerializer.Serialize(value, PipeJson.SerializerOptions);

    private sealed class ToolLoopFixture : IAsyncDisposable
    {
        private long _requestSequence;
        private readonly LocalModelSetupCoordinator _modelSetup;

        public ToolLoopFixture(ScriptedToolRuntime runtime, FakeMcpClient mcp)
        {
            Runtime = runtime;
            Mcp = mcp;
            Store = new MemoryConversationStore();
            Coordinator = new ChatCoordinator(
                Store,
                Runtime,
                new FakeMcpClientFactory(Mcp));
            _modelSetup = new LocalModelSetupCoordinator(
                new NoopModelSetupService());
            Router = new PipeRequestRouter(
                Coordinator,
                _modelSetup,
                new TestLifetime());
        }

        public ScriptedToolRuntime Runtime { get; }
        public FakeMcpClient Mcp { get; }
        public MemoryConversationStore Store { get; }
        public ChatCoordinator Coordinator { get; }
        public PipeRequestRouter Router { get; }
        public RecordingPeer Peer { get; } = new();

        public PipeEnvelope Request<T>(string method, T payload) => new()
        {
            Kind = PipeMessageKind.Request,
            RequestId = Guid.NewGuid(),
            Sequence = Interlocked.Increment(ref _requestSequence),
            Method = method,
            Payload = PipeJson.ToElement(payload),
        };

        public async Task<Guid> InitializeAsync()
        {
            await Router.HandleAsync(
                Request(
                    "initialize",
                    new InitializeRequest(
                        "instance-tools",
                        "project-tools",
                        "http://127.0.0.1:43123/mcp",
                        "private-token")),
                Peer);
            var response = await Peer.ReadAsync();
            Assert.Null(response.Error);
            return response.Payload.Deserialize<InitializeResult>(PipeJson.SerializerOptions)!
                .ActiveConversationId;
        }

        public async ValueTask DisposeAsync()
        {
            await _modelSetup.DisposeAsync();
            await Coordinator.StopAsync();
        }
    }

    private sealed class ScriptedToolRuntime(
        params IReadOnlyList<ChatInferenceChunk>[] rounds) : IChatInferenceRuntime
    {
        private readonly Queue<IReadOnlyList<ChatInferenceChunk>> _rounds = new(rounds);
        public List<ChatInferenceRequest> Requests { get; } = [];

        public ValueTask<ChatInferenceRuntimeStatus> GetStatusAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new ChatInferenceRuntimeStatus(
                "scripted-tools",
                "Scripted tools",
                IsReady: true,
                State: "ready"));
        }

        public async IAsyncEnumerable<ChatInferenceChunk> StreamAsync(
            ChatInferenceRequest request,
            [EnumeratorCancellation] CancellationToken cancellationToken = default)
        {
            Requests.Add(request);
            if (_rounds.Count == 0) throw new InvalidOperationException("No scripted round remains.");
            foreach (var chunk in _rounds.Dequeue())
            {
                cancellationToken.ThrowIfCancellationRequested();
                yield return chunk;
                await Task.Yield();
            }
        }
    }

    private sealed class FakeMcpClient(params SameInstanceMcpTool[] tools) : ISameInstanceMcpClient
    {
        public List<McpCall> Calls { get; } = [];
        public Exception? CallFailure { get; init; }
        public bool BlockCallUntilCancelled { get; init; }
        public TaskCompletionSource CallStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int BindingCallCount { get; private set; }
        public bool IsDisposed { get; private set; }

        public Task<IReadOnlyList<SameInstanceMcpTool>> GetToolsAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return Task.FromResult<IReadOnlyList<SameInstanceMcpTool>>(tools);
        }

        public async Task<SameInstanceMcpToolResult> CallToolAsync(
            string toolName,
            JsonElement arguments,
            string? approvalGrant = null,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Calls.Add(new McpCall(toolName, arguments.GetRawText(), approvalGrant));
            CallStarted.TrySetResult();
            if (BlockCallUntilCancelled)
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            if (CallFailure is not null) throw CallFailure;
            return new SameInstanceMcpToolResult(
                toolName,
                Json("{\"structuredContent\":{\"ok\":true},\"content\":[{\"type\":\"image\",\"data\":\"private-base64-marker\",\"mimeType\":\"image/png\"}]}"),
                IsError: false);
        }

        public Task<SameInstanceMcpApprovalBinding> CreateApprovalBindingAsync(
            string toolName,
            JsonElement arguments,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            BindingCallCount++;
            return Task.FromResult(new SameInstanceMcpApprovalBinding(
                "legacy-session-1",
                toolName,
                SameInstanceMcpArgumentHash.Compute(arguments)));
        }

        public ValueTask DisposeAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }

        public sealed record McpCall(string Tool, string ArgumentsJson, string? ApprovalGrant);
    }

    private sealed class FakeMcpClientFactory(FakeMcpClient client) : ISameInstanceMcpClientFactory
    {
        public ISameInstanceMcpClient Create(string endpoint, string bearerToken) => client;
    }

    private sealed class RecordingPeer : IPipePeer
    {
        private readonly Channel<PipeEnvelope> _messages = Channel.CreateUnbounded<PipeEnvelope>();
        private readonly List<PipeEnvelope> _snapshot = [];
        private readonly object _gate = new();
        private long _sequence;

        public Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default) =>
            WriteAsync(new PipeEnvelope
            {
                Kind = PipeMessageKind.Response,
                RequestId = request.RequestId,
                Sequence = Interlocked.Increment(ref _sequence),
                Method = request.Method,
                Payload = PipeJson.ToElement(payload),
                Error = error,
            }, cancellationToken);

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default) =>
            WriteAsync(new PipeEnvelope
            {
                Kind = PipeMessageKind.Event,
                RequestId = requestId,
                Sequence = Interlocked.Increment(ref _sequence),
                Method = method,
                Payload = PipeJson.ToElement(payload),
            }, cancellationToken);

        public IReadOnlyList<PipeEnvelope> Snapshot()
        {
            lock (_gate) return _snapshot.ToArray();
        }

        public Task<PipeEnvelope> ReadAsync(CancellationToken cancellationToken = default) =>
            _messages.Reader.ReadAsync(cancellationToken).AsTask();

        public async Task<IReadOnlyList<PipeEnvelope>> ReadUntilAsync(
            Guid requestId,
            string method)
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            var selected = new List<PipeEnvelope>();
            while (true)
            {
                var message = await ReadAsync(timeout.Token);
                if (message.RequestId != requestId) continue;
                selected.Add(message);
                if (message.Method == method) return selected;
            }
        }

        public Task<IReadOnlyList<PipeEnvelope>> ReadTurnEventsUntilCompletedAsync(Guid requestId) =>
            ReadUntilAsync(requestId, "completed");

        private Task WriteAsync(PipeEnvelope message, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate) _snapshot.Add(message);
            return _messages.Writer.WriteAsync(message, cancellationToken).AsTask();
        }
    }

    private sealed class GatedResponsePeer(RecordingPeer events) : IPipePeer
    {
        public TaskCompletionSource ResponseStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseResponse { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default)
        {
            ResponseStarted.TrySetResult();
            await ReleaseResponse.Task.WaitAsync(cancellationToken);
            await events.SendResponseAsync(request, payload, error, cancellationToken);
        }

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default) =>
            events.SendEventAsync(requestId, method, payload, cancellationToken);
    }

    private sealed class FramedStreamPeer(Stream stream) : IPipePeer, IAsyncDisposable
    {
        private readonly SemaphoreSlim _writeGate = new(1, 1);
        private long _sequence;

        public Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default) =>
            WriteAsync(PipeMessageKind.Response, request.RequestId, request.Method, payload, error, cancellationToken);

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default) =>
            WriteAsync(PipeMessageKind.Event, requestId, method, payload, null, cancellationToken);

        private async Task WriteAsync(
            PipeMessageKind kind,
            Guid requestId,
            string method,
            object payload,
            PipeError? error,
            CancellationToken cancellationToken)
        {
            await _writeGate.WaitAsync(cancellationToken);
            try
            {
                await PipeFraming.WriteAsync(
                    stream,
                    new PipeEnvelope
                    {
                        Kind = kind,
                        RequestId = requestId,
                        Sequence = ++_sequence,
                        Method = method,
                        Payload = PipeJson.ToElement(payload),
                        Error = error,
                    },
                    cancellationToken);
            }
            finally
            {
                _writeGate.Release();
            }
        }

        public ValueTask DisposeAsync()
        {
            _writeGate.Dispose();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class MemoryConversationStore : IConversationStore
    {
        private readonly object _gate = new();
        private readonly List<ConversationMessage> _messages = [];
        private ConversationSummary? _conversation;
        private long _sequence;

        public Task InitializeAsync(CancellationToken cancellationToken = default) => Task.CompletedTask;

        public Task<IReadOnlyList<ConversationSummary>> ListAsync(
            string projectContextKey,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
                return Task.FromResult<IReadOnlyList<ConversationSummary>>(
                    _conversation is null ? [] : [_conversation]);
        }

        public Task<ConversationSummary?> GetAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
                return Task.FromResult(
                    _conversation?.Id == conversationId ? _conversation : null);
        }

        public Task<ConversationSummary> CreateAsync(
            string projectContextKey,
            string title,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
            {
                var now = DateTimeOffset.UtcNow;
                _conversation = new ConversationSummary(Guid.NewGuid(), title, now, now);
                return Task.FromResult(_conversation);
            }
        }

        public Task<IReadOnlyList<ConversationMessage>> GetMessagesAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
                return Task.FromResult<IReadOnlyList<ConversationMessage>>(
                    _messages.Where(message => message.ConversationId == conversationId).ToArray());
        }

        public Task<IReadOnlyList<SequencedConversationMessage>> ListMessagesBeforeAsync(
            string projectContextKey,
            Guid conversationId,
            long? beforeMessageSequence,
            int maximumMessages,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
            {
                var result = _messages
                    .Select((message, index) => new SequencedConversationMessage(index + 1, message))
                    .Where(message => message.Message.ConversationId == conversationId &&
                        (!beforeMessageSequence.HasValue || message.Sequence < beforeMessageSequence.Value))
                    .OrderByDescending(message => message.Sequence)
                    .Take(maximumMessages)
                    .ToArray();
                return Task.FromResult<IReadOnlyList<SequencedConversationMessage>>(result);
            }
        }

        public Task AppendMessageAsync(
            string projectContextKey,
            ConversationMessage message,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_gate)
            {
                _messages.Add(message);
                _sequence++;
                if (_conversation is not null)
                    _conversation = _conversation with { UpdatedAt = message.CreatedAt };
            }
            return Task.CompletedTask;
        }
    }

    private sealed class TestLifetime : IHostApplicationLifetime
    {
        private readonly CancellationTokenSource _started = new();
        private readonly CancellationTokenSource _stopping = new();
        private readonly CancellationTokenSource _stopped = new();

        public CancellationToken ApplicationStarted => _started.Token;
        public CancellationToken ApplicationStopping => _stopping.Token;
        public CancellationToken ApplicationStopped => _stopped.Token;
        public void StopApplication() => _stopping.Cancel();
    }

    private sealed class NoopModelSetupService : ILocalModelSetupService
    {
        public Task InstallAsync(
            ModelSetupStartRequest request,
            Func<ModelSetupProgressEvent, CancellationToken, Task> reportAsync,
            CancellationToken cancellationToken) =>
            Task.CompletedTask;

        public void Dispose()
        {
        }
    }
}
