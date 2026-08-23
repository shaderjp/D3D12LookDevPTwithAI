using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading.Channels;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;
using D3D12LookDevPTwithAI.ChatHost;
using D3D12LookDevPTwithAI.ChatHost.Inference;
using D3D12LookDevPTwithAI.ChatHost.Mcp;
using Microsoft.Data.Sqlite;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class PipeRequestRouterTests
{
    [Fact]
    public async Task Initialize_validates_private_mcp_catalog_and_stop_disposes_the_client()
    {
        var mcpClient = new FakeSameInstanceMcpClient();
        var factory = new FakeSameInstanceMcpClientFactory(_ => mcpClient);
        var fixture = new RouterFixture(mcpClientFactory: factory);
        try
        {
            await fixture.Router.HandleAsync(
                fixture.Request(
                    "initialize",
                    new InitializeRequest(
                        "instance-1",
                        "project-1",
                        "http://127.0.0.1:43123/mcp",
                        "private-token")),
                fixture.Peer);

            var response = await fixture.Peer.ReadAsync();
            Assert.Null(response.Error);
            Assert.Equal(1, mcpClient.GetToolsCallCount);
            Assert.Equal("http://127.0.0.1:43123/mcp", factory.Endpoint);
            Assert.Equal("private-token", factory.BearerToken);

            await fixture.Coordinator.StopAsync();
            Assert.True(mcpClient.IsDisposed);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task Failed_private_mcp_initialize_is_sanitized_disposed_and_can_retry()
    {
        const string endpoint = "http://127.0.0.1:43124/mcp";
        const string bearerToken = "private-token-marker";
        const string privatePath = "C:\\private\\scene-marker.gltf";
        var failedClient = new FakeSameInstanceMcpClient(
            new InvalidOperationException(endpoint + bearerToken + privatePath));
        var succeedingClient = new FakeSameInstanceMcpClient();
        var clients = new Queue<ISameInstanceMcpClient>([failedClient, succeedingClient]);
        var factory = new FakeSameInstanceMcpClientFactory(_ => clients.Dequeue());
        await using var fixture = new RouterFixture(mcpClientFactory: factory);
        var request = new InitializeRequest("instance-1", "project-1", endpoint, bearerToken);

        await fixture.Router.HandleAsync(fixture.Request("initialize", request), fixture.Peer);
        var failed = await fixture.Peer.ReadAsync();

        Assert.Equal("mcp_initialization_failed", failed.Error?.Code);
        Assert.True(failed.Error?.Retryable);
        var serialized = JsonSerializer.Serialize(failed, PipeJson.SerializerOptions);
        Assert.DoesNotContain(endpoint, serialized, StringComparison.Ordinal);
        Assert.DoesNotContain(bearerToken, serialized, StringComparison.Ordinal);
        Assert.DoesNotContain(privatePath, serialized, StringComparison.Ordinal);
        Assert.True(failedClient.IsDisposed);

        await fixture.Router.HandleAsync(fixture.Request("initialize", request), fixture.Peer);
        var succeeded = await fixture.Peer.ReadAsync();
        Assert.Null(succeeded.Error);
        Assert.Equal(1, succeedingClient.GetToolsCallCount);
    }

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
    public async Task Model_setup_requires_explicit_license_acceptance()
    {
        await using var fixture = new RouterFixture();
        _ = await fixture.InitializeAsync();

        await fixture.Router.HandleAsync(
            fixture.Request(
                "modelSetup.start",
                new ModelSetupStartRequest(
                    "gemma-4-e2b-it-q4",
                    "cpu",
                    LicenseAccepted: false)),
            fixture.Peer);

        var rejected = await fixture.Peer.ReadAsync();
        Assert.Equal(PipeMessageKind.Response, rejected.Kind);
        Assert.Equal("model_license_not_accepted", rejected.Error?.Code);
    }

    [Fact]
    public async Task Model_setup_reports_progress_and_can_be_cancelled()
    {
        var setup = new BlockingModelSetupService();
        await using var fixture = new RouterFixture(modelSetupService: setup);
        _ = await fixture.InitializeAsync();
        var start = fixture.Request(
            "modelSetup.start",
            new ModelSetupStartRequest(
                "gemma-4-e2b-it-q4",
                "vulkan",
                LicenseAccepted: true));

        await fixture.Router.HandleAsync(start, fixture.Peer);
        var first = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        var second = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        var accepted = new[] { first, second }.Single(message =>
            message.Kind == PipeMessageKind.Response);
        var progress = new[] { first, second }.Single(message =>
            message.Method == "modelSetupProgress");

        Assert.Null(accepted.Error);
        Assert.Equal(start.RequestId, progress.RequestId);
        Assert.Equal(
            42,
            progress.Payload.GetProperty("percent").GetDouble());
        Assert.Equal("vulkan", (await setup.Started.Task.WaitAsync(
            TimeSpan.FromSeconds(3))).Backend);

        await fixture.Router.HandleAsync(
            fixture.Request("modelSetup.cancel", new { }),
            fixture.Peer);
        PipeEnvelope? cancelled = null;
        PipeEnvelope? cancelResponse = null;
        while (cancelled is null || cancelResponse is null)
        {
            var message = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            if (message.Kind == PipeMessageKind.Response &&
                message.Method == "modelSetup.cancel")
            {
                cancelResponse = message;
            }
            else if (message.Method == "modelSetupProgress" &&
                     message.Payload.GetProperty("terminal").GetBoolean())
            {
                cancelled = message;
            }
        }

        Assert.True(cancelResponse.Payload
            .Deserialize<ModelSetupCancelResult>(PipeJson.SerializerOptions)!
            .CancelRequested);
        Assert.Equal(
            "cancelled",
            cancelled.Payload.GetProperty("stage").GetString());
        await setup.Cancelled.Task.WaitAsync(TimeSpan.FromSeconds(3));
    }

    [Fact]
    public async Task Send_turn_streams_the_injected_inference_runtime()
    {
        var runtime = new ScriptedInferenceRuntime("alpha ", "beta");
        await using var fixture = new RouterFixture(runtime);
        var conversationId = await fixture.InitializeAsync();
        var turnId = Guid.NewGuid();
        var send = fixture.Request(
            "sendTurn",
            new SendTurnRequest(turnId, conversationId, "use the runtime"));

        await fixture.Router.HandleAsync(send, fixture.Peer);
        var accepted = await fixture.Peer.ReadAsync();
        var deltas = new List<string>();
        string? backend = null;
        string? modelId = null;
        string? modelDisplayName = null;
        PipeEnvelope envelope;
        do
        {
            envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            if (envelope.Method == "textDelta")
                deltas.Add(envelope.Payload.GetProperty("delta").GetString()!);
            if (envelope.Method == "runtimeState")
            {
                backend = envelope.Payload.GetProperty("backend").GetString();
                modelId = envelope.Payload.GetProperty("modelId").GetString();
                modelDisplayName = envelope.Payload
                    .GetProperty("modelDisplayName")
                    .GetString();
            }
        }
        while (envelope.Method != "completed");

        Assert.Null(accepted.Error);
        Assert.Equal("alpha beta", string.Concat(deltas));
        Assert.Equal(ScriptedInferenceRuntime.Id, backend);
        Assert.Equal(ScriptedInferenceRuntime.ModelId, modelId);
        Assert.Equal(ScriptedInferenceRuntime.ModelDisplayName, modelDisplayName);
        var request = Assert.Single(runtime.Requests);
        Assert.Equal(conversationId, request.ConversationId);
        Assert.Equal("use the runtime", request.UserText);
        Assert.Empty(request.History);
    }

    [Fact]
    public async Task Inference_runtime_receives_prior_history_in_database_sequence_order()
    {
        var runtime = new ScriptedInferenceRuntime("runtime reply");
        await using var fixture = new RouterFixture(runtime);
        var conversationId = await fixture.InitializeAsync();

        async Task SendAndDrainAsync(string text)
        {
            await fixture.Router.HandleAsync(
                fixture.Request(
                    "sendTurn",
                    new SendTurnRequest(Guid.NewGuid(), conversationId, text)),
                fixture.Peer);
            Assert.Null((await fixture.Peer.ReadAsync()).Error);
            PipeEnvelope envelope;
            do
            {
                envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            }
            while (envelope.Method != "completed");
        }

        await SendAndDrainAsync("first question");
        await SendAndDrainAsync("second question");

        Assert.Equal(2, runtime.Requests.Count);
        var history = runtime.Requests[1].History;
        Assert.Collection(
            history,
            message =>
            {
                Assert.Equal(ChatInferenceRole.User, message.Role);
                Assert.Equal("first question", message.Content);
            },
            message =>
            {
                Assert.Equal(ChatInferenceRole.Assistant, message.Role);
                Assert.Equal("runtime reply", message.Content);
            });
    }

    [Fact]
    public async Task Inference_runtime_exception_details_do_not_cross_the_pipe_boundary()
    {
        var runtime = new ScriptedInferenceRuntime
        {
            Failure = new ChatInferenceException(
                "private_runtime_failure_code",
                ScriptedInferenceRuntime.SensitiveFailureMarker),
        };
        await using var fixture = new RouterFixture(runtime);
        var conversationId = await fixture.InitializeAsync();
        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "trigger runtime failure")),
            fixture.Peer);

        Assert.Null((await fixture.Peer.ReadAsync()).Error);
        PipeEnvelope? failure = null;
        PipeEnvelope envelope;
        do
        {
            envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            if (envelope.Method == "error") failure = envelope;
        }
        while (envelope.Method != "completed");

        Assert.NotNull(failure);
        Assert.Equal(
            "inference_runtime_failed",
            failure.Payload.GetProperty("code").GetString());
        Assert.DoesNotContain(
            ScriptedInferenceRuntime.SensitiveFailureMarker,
            failure.Payload.GetProperty("message").GetString(),
            StringComparison.Ordinal);
        Assert.Equal("failed", envelope.Payload.GetProperty("status").GetString());
    }

    [Fact]
    public async Task Unsafe_runtime_status_is_rejected_before_it_crosses_the_pipe_boundary()
    {
        var runtime = new ScriptedInferenceRuntime("unused")
        {
            Status = new ChatInferenceRuntimeStatus(
                ScriptedInferenceRuntime.SensitiveFailureMarker,
                "Unsafe test runtime",
                IsReady: true,
                State: "ready"),
        };
        await using var fixture = new RouterFixture(runtime);
        var conversationId = await fixture.InitializeAsync();
        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "trigger unsafe status")),
            fixture.Peer);

        Assert.Null((await fixture.Peer.ReadAsync()).Error);
        var methods = new List<string>();
        PipeEnvelope? failure = null;
        PipeEnvelope envelope;
        do
        {
            envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
            methods.Add(envelope.Method!);
            if (envelope.Method == "error") failure = envelope;
        }
        while (envelope.Method != "completed");

        Assert.DoesNotContain("runtimeState", methods);
        Assert.NotNull(failure);
        Assert.Equal(
            "invalid_inference_status",
            failure.Payload.GetProperty("code").GetString());
        Assert.DoesNotContain(
            ScriptedInferenceRuntime.SensitiveFailureMarker,
            failure.Payload.GetProperty("message").GetString(),
            StringComparison.Ordinal);
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
    public async Task Accepted_turn_reports_a_terminal_failure_when_history_loading_fails()
    {
        await using var fixture = new RouterFixture(failHistoryRead: true);
        var conversationId = await fixture.InitializeAsync();
        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "trigger history failure")),
            fixture.Peer);

        var accepted = await fixture.Peer.ReadAsync();
        Assert.Null(accepted.Error);
        Assert.True(accepted.Payload.GetProperty("accepted").GetBoolean());

        var error = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        var completed = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        Assert.Equal("error", error.Method);
        Assert.Equal("turn_failed", error.Payload.GetProperty("code").GetString());
        Assert.DoesNotContain(
            FailingHistoryConversationStore.SensitiveFailureMarker,
            error.Payload.GetProperty("message").GetString(),
            StringComparison.Ordinal);
        Assert.Equal("completed", completed.Method);
        Assert.Equal("failed", completed.Payload.GetProperty("status").GetString());
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
        var approvalGrant = new string('a', 64);
        var pending = fixture.Coordinator.WaitForApprovalAsync(approvalId);

        await fixture.Router.HandleAsync(
            fixture.Request("approval.respond", new ApprovalRespondRequest(approvalId, "allowOnce", approvalGrant)),
            fixture.Peer);

        var response = await fixture.Peer.ReadAsync();
        var resolution = await pending;
        Assert.True(response.Payload.GetProperty("accepted").GetBoolean());
        Assert.True(resolution.Allowed);
        Assert.Equal(approvalGrant, resolution.ApprovalGrant);

        await fixture.Router.HandleAsync(
            fixture.Request("approval.respond", new ApprovalRespondRequest(approvalId, "allowOnce", approvalGrant)),
            fixture.Peer);
        var replay = await fixture.Peer.ReadAsync();
        Assert.False(replay.Payload.GetProperty("accepted").GetBoolean());
    }

    [Fact]
    public async Task Approval_allow_once_rejects_malformed_grants_without_resolving_the_prompt()
    {
        await using var fixture = new RouterFixture();
        _ = await fixture.InitializeAsync();

        foreach (var invalidGrant in new[]
        {
            string.Empty,
            new string('a', 63),
            new string('a', 65),
            new string('A', 64),
            new string('g', 64),
        })
        {
            var approvalId = Guid.NewGuid();
            using var cancellation = new CancellationTokenSource();
            var pending = fixture.Coordinator.WaitForApprovalAsync(
                approvalId,
                cancellation.Token);

            await fixture.Router.HandleAsync(
                fixture.Request(
                    "approval.respond",
                    new ApprovalRespondRequest(approvalId, "allowOnce", invalidGrant)),
                fixture.Peer);
            var response = await fixture.Peer.ReadAsync();
            Assert.Equal("invalid_approval", response.Error?.Code);
            Assert.False(pending.IsCompleted);

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        }
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

        Assert.True(
            stopwatch.Elapsed < TimeSpan.FromSeconds(1),
            $"Coordinator shutdown took {stopwatch.Elapsed}.");
    }

    [Fact]
    public async Task Failed_turn_terminal_writes_are_bounded_per_event()
    {
        var runtime = new ScriptedInferenceRuntime
        {
            Failure = new ChatInferenceException(
                "simulated_failure",
                "Simulated failure for terminal timeout coverage."),
        };
        await using var fixture = new RouterFixture(runtime);
        var conversationId = await fixture.InitializeAsync();
        var blockedPeer = new BlockingTerminalEventPipePeer();

        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "block terminal writes")),
            blockedPeer);
        await blockedPeer.FirstTerminalWriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));

        var stopwatch = Stopwatch.StartNew();
        await fixture.Coordinator.StopAsync().WaitAsync(TimeSpan.FromSeconds(2));
        stopwatch.Stop();

        Assert.Equal(2, blockedPeer.TerminalWriteCount);
        Assert.True(
            stopwatch.Elapsed < TimeSpan.FromSeconds(1),
            $"Two terminal writes took {stopwatch.Elapsed}.");
    }

    [Fact]
    public async Task Normal_turn_history_writes_receive_the_turn_cancellation_token()
    {
        AppendTokenRecordingConversationStore? recordingStore = null;
        await using var fixture = new RouterFixture(
            decorateConversationStore: inner =>
                recordingStore = new AppendTokenRecordingConversationStore(inner));
        var conversationId = await fixture.InitializeAsync();

        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "record append tokens")),
            fixture.Peer);
        Assert.Null((await fixture.Peer.ReadAsync()).Error);

        PipeEnvelope envelope;
        do
        {
            envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        }
        while (envelope.Method != "completed");

        var appendCalls = Assert.IsType<AppendTokenRecordingConversationStore>(
            recordingStore).AppendCalls;
        Assert.Collection(
            appendCalls,
            call =>
            {
                Assert.Equal("user", call.Role);
                Assert.True(call.TokenCanBeCanceled);
            },
            call =>
            {
                Assert.Equal("assistant", call.Role);
                Assert.True(call.TokenCanBeCanceled);
            });
    }

    [Fact]
    public async Task Cancelled_partial_history_write_uses_an_independent_bounded_token()
    {
        CancelledPartialPersistenceStore? persistenceStore = null;
        await using var fixture = new RouterFixture(
            new BlockingAfterFirstChunkInferenceRuntime(),
            decorateConversationStore: inner =>
                persistenceStore = new CancelledPartialPersistenceStore(inner));
        var conversationId = await fixture.InitializeAsync();

        await fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "cancel after a partial")),
            fixture.Peer);
        Assert.Null((await fixture.Peer.ReadAsync()).Error);
        PipeEnvelope envelope;
        do
        {
            envelope = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(3));
        }
        while (envelope.Method != "textDelta");

        var stopwatch = Stopwatch.StartNew();
        await fixture.Coordinator.StopAsync().WaitAsync(TimeSpan.FromSeconds(2));
        stopwatch.Stop();

        var store = Assert.IsType<CancelledPartialPersistenceStore>(persistenceStore);
        await store.PersistenceCancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.True(store.TokenCanBeCanceled);
        Assert.False(store.TokenWasAlreadyCancelled);
        Assert.True(
            stopwatch.Elapsed < TimeSpan.FromSeconds(1),
            $"Cancelled partial persistence took {stopwatch.Elapsed}.");
    }

    [Fact]
    public async Task Stop_after_acceptance_emits_cancelled_terminal_without_late_history()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var acceptancePeer = new GatedAcceptancePipePeer();
        var routing = fixture.Router.HandleAsync(
            fixture.Request(
                "sendTurn",
                new SendTurnRequest(Guid.NewGuid(), conversationId, "acceptance race")),
            acceptancePeer);
        await acceptancePeer.AcceptanceObserved.Task.WaitAsync(TimeSpan.FromSeconds(3));

        var stopping = fixture.Coordinator.StopAsync();
        try
        {
            await Task.Yield();
            Assert.False(stopping.IsCompleted);
        }
        finally
        {
            acceptancePeer.ReleaseAcceptance.TrySetResult();
        }
        await routing.WaitAsync(TimeSpan.FromSeconds(3));
        await stopping.WaitAsync(TimeSpan.FromSeconds(3));

        Assert.Empty(await fixture.Store.GetMessagesAsync("project-1", conversationId));
        var completed = await acceptancePeer.Completed.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.Equal("cancelled", completed.Status);
    }

    [Fact]
    public async Task Conversation_select_validates_page_size_and_exact_sequence_cursor()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var invalidRequests = new[]
        {
            new ConversationSelectRequest(Guid.Empty),
            new ConversationSelectRequest(conversationId, BeforeMessageSequence: 0),
            new ConversationSelectRequest(
                conversationId,
                BeforeMessageSequence: PipeProtocol.MaximumExactJsonInteger + 1),
            new ConversationSelectRequest(conversationId, PageSize: 0),
            new ConversationSelectRequest(
                conversationId,
                PageSize: PipeProtocol.MaximumConversationPageSize + 1),
        };

        foreach (var invalidRequest in invalidRequests)
        {
            await fixture.Router.HandleAsync(
                fixture.Request("conversation.select", invalidRequest),
                fixture.Peer);
            var response = await fixture.Peer.ReadAsync();
            Assert.Contains(
                response.Error?.Code,
                new[] { "invalid_conversation", "invalid_history_cursor", "invalid_page_size" });
        }
    }

    [Fact]
    public async Task Conversation_select_pages_all_history_within_the_four_mib_frame_limit()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        var expectedIds = new List<Guid>();
        var maximumEscapedContent = new string(
            '\u0001',
            PipeProtocol.MaximumConversationMessageCharacters);
        for (var index = 0; index < 8; index++)
        {
            var message = new ConversationMessage(
                Guid.NewGuid(),
                conversationId,
                "assistant",
                maximumEscapedContent,
                DateTimeOffset.UtcNow.AddMinutes(-index));
            expectedIds.Add(message.Id);
            await fixture.Store.AppendMessageAsync("project-1", message);
        }

        long? cursor = null;
        var reconstructedChronologicalIds = new List<Guid>();
        var pageCount = 0;
        do
        {
            await fixture.Router.HandleAsync(
                fixture.Request(
                    "conversation.select",
                    new ConversationSelectRequest(
                        conversationId,
                        cursor,
                        PipeProtocol.MaximumConversationPageSize)),
                fixture.Peer);
            var response = await fixture.Peer.ReadAsync(TimeSpan.FromSeconds(10));
            Assert.Null(response.Error);
            var page = response.Payload.Deserialize<ConversationSelectResult>(
                PipeJson.SerializerOptions)!;
            Assert.NotEmpty(page.Messages);
            Assert.True(
                JsonSerializer.SerializeToUtf8Bytes(page, PipeJson.SerializerOptions).Length <=
                PipeProtocol.MaximumConversationSelectPayloadBytes);
            await using var frame = new MemoryStream();
            await PipeFraming.WriteAsync(
                frame,
                response with { Sequence = long.MaxValue });
            Assert.InRange(
                frame.Length - sizeof(uint),
                1,
                PipeProtocol.MaximumFrameBytes);

            reconstructedChronologicalIds.InsertRange(
                0,
                page.Messages.Select(message => message.Id));
            pageCount++;
            Assert.Equal(page.HasMoreMessages, page.OlderBeforeMessageSequence.HasValue);
            if (pageCount == 1)
            {
                await fixture.Store.AppendMessageAsync(
                    "project-1",
                    new ConversationMessage(
                        Guid.NewGuid(),
                        conversationId,
                        "assistant",
                        "inserted after the latest-page cursor",
                        DateTimeOffset.UtcNow));
            }
            if (!page.HasMoreMessages)
            {
                Assert.Null(page.OlderBeforeMessageSequence);
                break;
            }
            cursor = Assert.IsType<long>(page.OlderBeforeMessageSequence);
        }
        while (true);

        Assert.True(pageCount > 1);
        Assert.Equal(expectedIds, reconstructedChronologicalIds);
    }

    [Fact]
    public async Task Legacy_oversized_history_returns_a_small_error_and_keeps_the_connection_usable()
    {
        await using var fixture = new RouterFixture();
        var conversationId = await fixture.InitializeAsync();
        await fixture.SeedLegacyMessageAsync(
            conversationId,
            new string(
                '\u0001',
                PipeProtocol.MaximumConversationSelectPayloadBytes / 6 + 1024));

        await fixture.Router.HandleAsync(
            fixture.Request(
                "conversation.select",
                new ConversationSelectRequest(conversationId)),
            fixture.Peer);
        var rejected = await fixture.Peer.ReadAsync();

        Assert.Equal("history_message_too_large", rejected.Error?.Code);
        Assert.True(
            JsonSerializer.SerializeToUtf8Bytes(
                rejected,
                PipeJson.SerializerOptions).Length < 16 * 1024);

        await fixture.Router.HandleAsync(
            fixture.Request("conversation.list", new { }),
            fixture.Peer);
        var list = await fixture.Peer.ReadAsync();
        Assert.Null(list.Error);
        Assert.Equal(
            conversationId,
            list.Payload.Deserialize<ConversationListResult>(
                PipeJson.SerializerOptions)!.ActiveConversationId);
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
        private readonly LocalModelSetupCoordinator _modelSetup;

        public RouterFixture(
            IChatInferenceRuntime? inferenceRuntime = null,
            bool failHistoryRead = false,
            Func<IConversationStore, IConversationStore>? decorateConversationStore = null,
            ISameInstanceMcpClientFactory? mcpClientFactory = null,
            ILocalModelSetupService? modelSetupService = null)
        {
            var paths = new AppPaths(_dataDirectory);
            Store = new SqliteConversationStore(paths);
            IConversationStore coordinatorStore = failHistoryRead
                ? new FailingHistoryConversationStore(Store)
                : Store;
            if (decorateConversationStore is not null)
                coordinatorStore = decorateConversationStore(coordinatorStore);
            Coordinator = new ChatCoordinator(
                coordinatorStore,
                inferenceRuntime ?? new DeterministicChatInferenceRuntime(),
                mcpClientFactory);
            _modelSetup = new LocalModelSetupCoordinator(
                modelSetupService ?? new LocalModelSetupService(paths));
            Router = new PipeRequestRouter(Coordinator, _modelSetup, _lifetime);
        }

        public SqliteConversationStore Store { get; }
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

        public async Task SeedLegacyMessageAsync(
            Guid conversationId,
            string content)
        {
            await using var connection = new SqliteConnection(
                new SqliteConnectionStringBuilder
                {
                    DataSource = Path.Combine(
                        _dataDirectory,
                        "chat-history.sqlite3"),
                    Mode = SqliteOpenMode.ReadWrite,
                    Pooling = false,
                }.ToString());
            await connection.OpenAsync();
            var command = connection.CreateCommand();
            command.CommandText = """
                INSERT INTO messages(
                    project_context_key,
                    id,
                    conversation_id,
                    role,
                    content,
                    created_at,
                    is_error)
                VALUES($context, $id, $conversation, 'assistant', $content, $created, 0)
                """;
            command.Parameters.AddWithValue("$context", "project-1");
            command.Parameters.AddWithValue("$id", Guid.NewGuid().ToString("D"));
            command.Parameters.AddWithValue(
                "$conversation",
                conversationId.ToString("D"));
            command.Parameters.AddWithValue("$content", content);
            command.Parameters.AddWithValue(
                "$created",
                DateTimeOffset.UtcNow.ToString("O"));
            await command.ExecuteNonQueryAsync();
        }

        public async ValueTask DisposeAsync()
        {
            await _modelSetup.DisposeAsync();
            await Coordinator.StopAsync();
            if (Directory.Exists(_dataDirectory)) Directory.Delete(_dataDirectory, recursive: true);
        }
    }

    private sealed class FakeSameInstanceMcpClientFactory(
        Func<(string Endpoint, string BearerToken), ISameInstanceMcpClient> create)
        : ISameInstanceMcpClientFactory
    {
        public string? Endpoint { get; private set; }
        public string? BearerToken { get; private set; }

        public ISameInstanceMcpClient Create(string endpoint, string bearerToken)
        {
            Endpoint = endpoint;
            BearerToken = bearerToken;
            return create((endpoint, bearerToken));
        }
    }

    private sealed class BlockingModelSetupService : ILocalModelSetupService
    {
        public TaskCompletionSource<ModelSetupStartRequest> Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Cancelled { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task InstallAsync(
            ModelSetupStartRequest request,
            Func<ModelSetupProgressEvent, CancellationToken, Task> reportAsync,
            CancellationToken cancellationToken)
        {
            Started.TrySetResult(request);
            await reportAsync(
                new ModelSetupProgressEvent(
                    "downloading",
                    "test artifact",
                    42,
                    100,
                    42,
                    100,
                    42,
                    "Downloading…"),
                cancellationToken);
            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                Cancelled.TrySetResult();
                throw;
            }
        }

        public void Dispose()
        {
        }
    }

    private sealed class FakeSameInstanceMcpClient(Exception? catalogFailure = null)
        : ISameInstanceMcpClient
    {
        public int GetToolsCallCount { get; private set; }
        public bool IsDisposed { get; private set; }

        public Task<IReadOnlyList<SameInstanceMcpTool>> GetToolsAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            GetToolsCallCount++;
            return catalogFailure is null
                ? Task.FromResult<IReadOnlyList<SameInstanceMcpTool>>([])
                : Task.FromException<IReadOnlyList<SameInstanceMcpTool>>(catalogFailure);
        }

        public Task<SameInstanceMcpToolResult> CallToolAsync(
            string toolName,
            JsonElement arguments,
            string? approvalGrant = null,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task<SameInstanceMcpApprovalBinding> CreateApprovalBindingAsync(
            string toolName,
            JsonElement arguments,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ScriptedInferenceRuntime(params string[] chunks) : IChatInferenceRuntime
    {
        public const string Id = "scripted-test-runtime";
        public const string ModelId = "scripted-model-q4";
        public const string ModelDisplayName = "Scripted Model Q4";
        public const string SensitiveFailureMarker =
            "C:\\private\\model-path\\sensitive-runtime-marker.gguf";
        public List<ChatInferenceRequest> Requests { get; } = [];
        public ChatInferenceRuntimeStatus Status { get; init; } = new(
            Id,
            "Scripted test runtime",
            IsReady: true,
            State: "ready",
            ModelId,
            ModelDisplayName);
        public ChatInferenceException? Failure { get; init; }

        public ValueTask<ChatInferenceRuntimeStatus> GetStatusAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(Status);
        }

        public async IAsyncEnumerable<ChatInferenceChunk> StreamAsync(
            ChatInferenceRequest request,
            [EnumeratorCancellation] CancellationToken cancellationToken = default)
        {
            Requests.Add(request);
            if (Failure is not null) throw Failure;
            foreach (var chunk in chunks)
            {
                cancellationToken.ThrowIfCancellationRequested();
                yield return new ChatInferenceChunk(chunk);
                await Task.Yield();
            }
        }
    }

    private sealed class BlockingAfterFirstChunkInferenceRuntime : IChatInferenceRuntime
    {
        public ValueTask<ChatInferenceRuntimeStatus> GetStatusAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new ChatInferenceRuntimeStatus(
                "blocking-after-chunk",
                "Blocking after chunk test runtime",
                IsReady: true,
                State: "ready"));
        }

        public async IAsyncEnumerable<ChatInferenceChunk> StreamAsync(
            ChatInferenceRequest request,
            [EnumeratorCancellation] CancellationToken cancellationToken = default)
        {
            yield return new ChatInferenceChunk("partial response");
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
        }
    }

    private sealed class AppendTokenRecordingConversationStore(IConversationStore inner) :
        DelegatingConversationStore(inner)
    {
        public List<AppendCall> AppendCalls { get; } = [];

        public override Task AppendMessageAsync(
            string projectContextKey,
            ConversationMessage message,
            CancellationToken cancellationToken = default)
        {
            AppendCalls.Add(new AppendCall(message.Role, cancellationToken.CanBeCanceled));
            return base.AppendMessageAsync(projectContextKey, message, cancellationToken);
        }

        public sealed record AppendCall(string Role, bool TokenCanBeCanceled);
    }

    private sealed class CancelledPartialPersistenceStore(IConversationStore inner) :
        DelegatingConversationStore(inner)
    {
        public TaskCompletionSource PersistenceCancellationObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool TokenCanBeCanceled { get; private set; }
        public bool TokenWasAlreadyCancelled { get; private set; }

        public override async Task AppendMessageAsync(
            string projectContextKey,
            ConversationMessage message,
            CancellationToken cancellationToken = default)
        {
            if (!string.Equals(message.Role, "assistant", StringComparison.Ordinal))
            {
                await base.AppendMessageAsync(
                    projectContextKey,
                    message,
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            TokenCanBeCanceled = cancellationToken.CanBeCanceled;
            TokenWasAlreadyCancelled = cancellationToken.IsCancellationRequested;
            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                PersistenceCancellationObserved.TrySetResult();
                throw;
            }
        }
    }

    private abstract class DelegatingConversationStore(IConversationStore inner) :
        IConversationStore
    {
        public Task InitializeAsync(CancellationToken cancellationToken = default) =>
            inner.InitializeAsync(cancellationToken);

        public Task<IReadOnlyList<ConversationSummary>> ListAsync(
            string projectContextKey,
            CancellationToken cancellationToken = default) =>
            inner.ListAsync(projectContextKey, cancellationToken);

        public Task<ConversationSummary?> GetAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default) =>
            inner.GetAsync(projectContextKey, conversationId, cancellationToken);

        public Task<ConversationSummary> CreateAsync(
            string projectContextKey,
            string title,
            CancellationToken cancellationToken = default) =>
            inner.CreateAsync(projectContextKey, title, cancellationToken);

        public Task<IReadOnlyList<ConversationMessage>> GetMessagesAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default) =>
            inner.GetMessagesAsync(projectContextKey, conversationId, cancellationToken);

        public Task<IReadOnlyList<SequencedConversationMessage>> ListMessagesBeforeAsync(
            string projectContextKey,
            Guid conversationId,
            long? beforeMessageSequence,
            int maximumMessages,
            CancellationToken cancellationToken = default) =>
            inner.ListMessagesBeforeAsync(
                projectContextKey,
                conversationId,
                beforeMessageSequence,
                maximumMessages,
                cancellationToken);

        public virtual Task AppendMessageAsync(
            string projectContextKey,
            ConversationMessage message,
            CancellationToken cancellationToken = default) =>
            inner.AppendMessageAsync(projectContextKey, message, cancellationToken);
    }

    private sealed class FailingHistoryConversationStore(IConversationStore inner) : IConversationStore
    {
        public const string SensitiveFailureMarker = "sensitive-history-failure-marker";

        public Task InitializeAsync(CancellationToken cancellationToken = default) =>
            inner.InitializeAsync(cancellationToken);

        public Task<IReadOnlyList<ConversationSummary>> ListAsync(
            string projectContextKey,
            CancellationToken cancellationToken = default) =>
            inner.ListAsync(projectContextKey, cancellationToken);

        public Task<ConversationSummary?> GetAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default) =>
            inner.GetAsync(projectContextKey, conversationId, cancellationToken);

        public Task<ConversationSummary> CreateAsync(
            string projectContextKey,
            string title,
            CancellationToken cancellationToken = default) =>
            inner.CreateAsync(projectContextKey, title, cancellationToken);

        public Task<IReadOnlyList<ConversationMessage>> GetMessagesAsync(
            string projectContextKey,
            Guid conversationId,
            CancellationToken cancellationToken = default) =>
            inner.GetMessagesAsync(projectContextKey, conversationId, cancellationToken);

        public Task<IReadOnlyList<SequencedConversationMessage>> ListMessagesBeforeAsync(
            string projectContextKey,
            Guid conversationId,
            long? beforeMessageSequence,
            int maximumMessages,
            CancellationToken cancellationToken = default) =>
            Task.FromException<IReadOnlyList<SequencedConversationMessage>>(
                new InvalidOperationException(SensitiveFailureMarker));

        public Task AppendMessageAsync(
            string projectContextKey,
            ConversationMessage message,
            CancellationToken cancellationToken = default) =>
            inner.AppendMessageAsync(projectContextKey, message, cancellationToken);
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

    private sealed class BlockingTerminalEventPipePeer : IPipePeer
    {
        private int _terminalWriteCount;

        public TaskCompletionSource FirstTerminalWriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int TerminalWriteCount => Volatile.Read(ref _terminalWriteCount);

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
            if (method is not ("error" or "completed"))
                return;

            if (Interlocked.Increment(ref _terminalWriteCount) == 1)
                FirstTerminalWriteStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
        }
    }

    private sealed class GatedAcceptancePipePeer : IPipePeer
    {
        public TaskCompletionSource AcceptanceObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseAcceptance { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<TurnCompletedEvent> Completed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async Task SendResponseAsync(
            PipeEnvelope request,
            object payload,
            PipeError? error = null,
            CancellationToken cancellationToken = default)
        {
            AcceptanceObserved.TrySetResult();
            await ReleaseAcceptance.Task.WaitAsync(cancellationToken);
        }

        public Task SendEventAsync(
            Guid requestId,
            string method,
            object payload,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (method == "completed" && payload is TurnCompletedEvent completed)
                Completed.TrySetResult(completed);
            return Task.CompletedTask;
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
