using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class LlamaServerChatInferenceRuntimeTests
{
    [Fact]
    public async Task Streams_sse_with_authorization_and_expected_request_shape()
    {
        string? requestBody = null;
        var handler = new DelegateHandler(async (request, cancellationToken) =>
        {
            Assert.Equal(HttpMethod.Post, request.Method);
            Assert.Equal(
                "http://127.0.0.1:53123/v1/chat/completions",
                request.RequestUri?.AbsoluteUri);
            Assert.Equal("Bearer", request.Headers.Authorization?.Scheme);
            Assert.Equal("test-api-key", request.Headers.Authorization?.Parameter);
            Assert.Equal("application/json", request.Content?.Headers.ContentType?.MediaType);
            requestBody = await request.Content!.ReadAsStringAsync(cancellationToken);
            return SseResponse(
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"こん\"}}]}\n\n" +
                "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"にちは\"}}]}\n\n" +
                Data(StopPayload()) +
                "data: [DONE]\n\n");
        });
        using var runtime = Runtime(handler);
        var request = Request(
            "現在の質問",
            [
                new ChatInferenceMessage(ChatInferenceRole.System, "system instruction"),
                new ChatInferenceMessage(ChatInferenceRole.Assistant, "前の回答"),
            ],
            allowToolCalls: false);

        var chunks = await CollectAsync(runtime.StreamAsync(request));
        var status = await runtime.GetStatusAsync();

        Assert.Equal("こんにちは", string.Concat(chunks));
        Assert.Equal("llama-server-cuda", status.RuntimeId);
        Assert.Equal("ready", status.State);
        Assert.True(status.IsReady);
        Assert.True(status.RuntimeId.Length <= ChatInferenceLimits.MaximumRuntimeIdentifierCharacters);
        Assert.True(status.State.Length <= ChatInferenceLimits.MaximumRuntimeStateCharacters);

        Assert.NotNull(requestBody);
        Assert.DoesNotContain("test-api-key", requestBody, StringComparison.Ordinal);
        using var document = JsonDocument.Parse(requestBody);
        var root = document.RootElement;
        Assert.Equal("gemma-test", root.GetProperty("model").GetString());
        Assert.Equal(1, root.GetProperty("n").GetInt32());
        Assert.True(root.GetProperty("stream").GetBoolean());
        Assert.True(root.GetProperty("stream_options").GetProperty("include_usage").GetBoolean());
        Assert.Equal(0.25, root.GetProperty("temperature").GetDouble());
        Assert.Equal(512, root.GetProperty("max_tokens").GetInt32());
        Assert.Equal("none", root.GetProperty("reasoning_effort").GetString());
        Assert.False(root.GetProperty("chat_template_kwargs").GetProperty("enable_thinking").GetBoolean());
        Assert.False(root.TryGetProperty("tools", out _));
        Assert.False(root.TryGetProperty("tool_choice", out _));
        Assert.False(root.TryGetProperty("parse_tool_calls", out _));
        Assert.False(root.TryGetProperty("parallel_tool_calls", out _));

        var messages = root.GetProperty("messages");
        Assert.Equal(4, messages.GetArrayLength());
        Assert.Equal("system", messages[0].GetProperty("role").GetString());
        Assert.Equal(
            LlamaServerChatInferenceRuntime.BuiltInSystemPrompt,
            messages[0].GetProperty("content").GetString());
        Assert.Contains("scene", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("material", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("light", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("camera", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("rendering", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("tool result", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("untrusted data", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("not instructions", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("live tools", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Contains("never claim", messages[0].GetProperty("content").GetString(), StringComparison.Ordinal);
        Assert.Equal("system", messages[1].GetProperty("role").GetString());
        Assert.Equal("system instruction", messages[1].GetProperty("content").GetString());
        Assert.Equal("assistant", messages[2].GetProperty("role").GetString());
        Assert.Equal("前の回答", messages[2].GetProperty("content").GetString());
        Assert.Equal("user", messages[3].GetProperty("role").GetString());
        Assert.Equal("現在の質問", messages[3].GetProperty("content").GetString());
    }

    [Fact]
    public async Task Serializes_tools_and_a_tool_result_continuation_in_openai_format()
    {
        string? requestBody = null;
        var handler = new DelegateHandler(async (request, cancellationToken) =>
        {
            requestBody = await request.Content!.ReadAsStringAsync(cancellationToken);
            return SseResponse(Data(StopPayload()) + "data: [DONE]\n\n");
        });
        using var runtime = Runtime(handler);
        var call = new ChatInferenceToolCall(
            "call_camera",
            "set_camera",
            "{\"fov\":45}");
        var tool = new ChatInferenceToolDefinition(
            "set_camera",
            "カメラを更新します",
            "{\"type\":\"object\",\"properties\":{\"fov\":{\"type\":\"number\"}}}");
        var request = Request(
            string.Empty,
            [
                new ChatInferenceMessage(ChatInferenceRole.User, "FOVを変えて"),
                new ChatInferenceMessage(
                    ChatInferenceRole.Assistant,
                    string.Empty,
                    ToolCalls: [call]),
                new ChatInferenceMessage(
                    ChatInferenceRole.Tool,
                    "{\"ok\":true}",
                    "set_camera",
                    "call_camera"),
            ],
            [tool],
            appendUserMessage: false);

        var chunks = await CollectChunksAsync(runtime.StreamAsync(request));

        Assert.Empty(chunks);
        Assert.NotNull(requestBody);
        Assert.DoesNotContain("test-api-key", requestBody, StringComparison.Ordinal);
        using var document = JsonDocument.Parse(requestBody);
        var root = document.RootElement;
        Assert.Equal("auto", root.GetProperty("tool_choice").GetString());
        Assert.True(root.GetProperty("parse_tool_calls").GetBoolean());
        Assert.False(root.GetProperty("parallel_tool_calls").GetBoolean());
        var tools = root.GetProperty("tools");
        Assert.Single(tools.EnumerateArray());
        Assert.Equal("function", tools[0].GetProperty("type").GetString());
        var function = tools[0].GetProperty("function");
        Assert.Equal("set_camera", function.GetProperty("name").GetString());
        Assert.Equal("カメラを更新します", function.GetProperty("description").GetString());
        Assert.Equal("object", function.GetProperty("parameters").GetProperty("type").GetString());

        var messages = root.GetProperty("messages");
        Assert.Equal(4, messages.GetArrayLength());
        Assert.Equal("user", messages[1].GetProperty("role").GetString());
        Assert.Equal("assistant", messages[2].GetProperty("role").GetString());
        Assert.Equal(JsonValueKind.Null, messages[2].GetProperty("content").ValueKind);
        var serializedCall = messages[2].GetProperty("tool_calls")[0];
        Assert.Equal("call_camera", serializedCall.GetProperty("id").GetString());
        Assert.Equal("function", serializedCall.GetProperty("type").GetString());
        Assert.Equal(
            "set_camera",
            serializedCall.GetProperty("function").GetProperty("name").GetString());
        Assert.Equal(
            "{\"fov\":45}",
            serializedCall.GetProperty("function").GetProperty("arguments").GetString());
        Assert.Equal("tool", messages[3].GetProperty("role").GetString());
        Assert.Equal("call_camera", messages[3].GetProperty("tool_call_id").GetString());
        Assert.Equal("set_camera", messages[3].GetProperty("name").GetString());
        Assert.Equal("{\"ok\":true}", messages[3].GetProperty("content").GetString());
        Assert.DoesNotContain(
            "{\"fov\":45}",
            call.ToString(),
            StringComparison.Ordinal);
        Assert.DoesNotContain("properties", tool.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Disabled_tool_calls_keep_the_catalog_but_reject_returned_calls()
    {
        string? requestBody = null;
        var call = ToolCallPayload("call_a", "set_camera", "{}");
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        var handler = new DelegateHandler(async (request, cancellationToken) =>
        {
            requestBody = await request.Content!.ReadAsStringAsync(cancellationToken);
            return SseResponse(Data(call) + Data(finish) + "data: [DONE]\n\n");
        });
        using var runtime = Runtime(handler);
        var request = Request(
            "summarize",
            tools:
            [
                new ChatInferenceToolDefinition(
                    "set_camera",
                    null,
                    "{\"type\":\"object\"}"),
            ],
            allowToolCalls: false);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(request)));

        Assert.Equal("inference_protocol_error", exception.Code);
        Assert.NotNull(requestBody);
        using var document = JsonDocument.Parse(requestBody);
        Assert.Equal("none", document.RootElement.GetProperty("tool_choice").GetString());
        Assert.True(document.RootElement.GetProperty("parse_tool_calls").GetBoolean());
        Assert.False(document.RootElement.GetProperty("parallel_tool_calls").GetBoolean());
        Assert.Single(document.RootElement.GetProperty("tools").EnumerateArray());
    }

    [Fact]
    public async Task Assembles_fragmented_parallel_calls_and_emits_them_only_after_done()
    {
        var first = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        role = "assistant",
                        content = "調整します。",
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "call_",
                                type = "function",
                                function = new { name = "set_", arguments = "{\"fov\":" },
                            },
                            new
                            {
                                index = 1,
                                id = "read_",
                                type = "function",
                                function = new { name = "get_", arguments = "{" },
                            },
                        },
                    },
                    finish_reason = (string?)null,
                },
            },
        };
        var second = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "camera",
                                type = "function",
                                function = new { name = "camera", arguments = "45}" },
                            },
                            new
                            {
                                index = 1,
                                id = "state",
                                type = "function",
                                function = new { name = "state", arguments = "}" },
                            },
                        },
                    },
                    finish_reason = (string?)null,
                },
            },
        };
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        var usage = new { choices = Array.Empty<object>(), usage = new { completion_tokens = 8 } };
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            Data(first) + Data(second) + Data(finish) + Data(usage) + "data: [DONE]\n\n")));
        using var runtime = Runtime(handler);

        var chunks = await CollectChunksAsync(runtime.StreamAsync(Request("adjust")));

        Assert.Equal(2, chunks.Count);
        Assert.Equal("調整します。", chunks[0].Text);
        Assert.Null(chunks[0].ToolCalls);
        Assert.Equal(string.Empty, chunks[1].Text);
        Assert.NotNull(chunks[1].ToolCalls);
        Assert.Collection(
            chunks[1].ToolCalls!,
            call =>
            {
                Assert.Equal("call_camera", call.Id);
                Assert.Equal("set_camera", call.Name);
                Assert.Equal("{\"fov\":45}", call.ArgumentsJson);
            },
            call =>
            {
                Assert.Equal("read_state", call.Id);
                Assert.Equal("get_state", call.Name);
                Assert.Equal("{}", call.ArgumentsJson);
            });
    }

    [Fact]
    public async Task Finished_or_truncated_partial_calls_are_never_exposed()
    {
        var fragment = ToolCallPayload("call_partial", "set_camera", "{\"fov\":");
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        var blockingStream = new BlockingSseStream(Data(fragment) + Data(finish));
        var blockingContent = new StreamContent(blockingStream);
        blockingContent.Headers.ContentType = new MediaTypeHeaderValue("text/event-stream");
        using var blockingRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.OK) { Content = blockingContent })));
        using var cancellation = new CancellationTokenSource();
        await using var enumerator = blockingRuntime.StreamAsync(
            Request("adjust"),
            cancellation.Token).GetAsyncEnumerator();
        var moveNext = enumerator.MoveNextAsync().AsTask();
        await blockingStream.BlockingReadStarted.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => moveNext);

        using var truncatedRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(fragment) + Data(finish)))));
        var truncated = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(truncatedRuntime.StreamAsync(Request("adjust"))));
        Assert.Equal("inference_stream_truncated", truncated.Code);
    }

    [Fact]
    public async Task Rejects_conflicting_or_incomplete_tool_call_streams_safely()
    {
        var duplicateIndex = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "call_a",
                                type = "function",
                                function = new { name = "set_camera", arguments = "{}" },
                            },
                            new
                            {
                                index = 0,
                                id = "call_b",
                                type = "function",
                                function = new { name = "set_camera", arguments = "{}" },
                            },
                        },
                    },
                },
            },
        };
        var wrongType = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "call_a",
                                type = "command",
                                function = new { name = "set_camera", arguments = "{}" },
                            },
                        },
                    },
                },
            },
        };
        var missingType = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "call_a",
                                function = new { name = "set_camera", arguments = "{}" },
                            },
                        },
                    },
                },
            },
        };
        var toolFinish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        foreach (var payload in new[] { Data(duplicateIndex), Data(wrongType), Data(missingType) })
        {
            using var runtime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
                SseResponse(payload + Data(toolFinish) + "data: [DONE]\n\n"))));
            var exception = await Assert.ThrowsAsync<ChatInferenceException>(
                () => CollectChunksAsync(runtime.StreamAsync(Request("adjust"))));
            Assert.Equal("inference_protocol_error", exception.Code);
            Assert.DoesNotContain("call_a", exception.ToString(), StringComparison.Ordinal);
        }

        var noFinish = ToolCallPayload("call_a", "set_camera", "{}");
        using var unfinishedRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(noFinish) + "data: [DONE]\n\n"))));
        var unfinished = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(unfinishedRuntime.StreamAsync(Request("adjust"))));
        Assert.Equal("inference_protocol_error", unfinished.Code);

        var stop = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "stop" },
            },
        };
        using var mismatchedRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(noFinish) + Data(stop) + "data: [DONE]\n\n"))));
        var mismatched = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(mismatchedRuntime.StreamAsync(Request("adjust"))));
        Assert.Equal("inference_protocol_error", mismatched.Code);
    }

    [Theory]
    [InlineData("[]")]
    [InlineData("{\"x\":1,\"x\":2}")]
    [InlineData("{\"x\":")]
    public async Task Completed_tool_arguments_must_be_one_unambiguous_json_object(
        string arguments)
    {
        var call = ToolCallPayload("call_a", "set_camera", arguments);
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        using var runtime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(call) + Data(finish) + "data: [DONE]\n\n"))));

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(Request("adjust"))));

        Assert.Equal("inference_protocol_error", exception.Code);
        Assert.DoesNotContain(arguments, exception.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Length_finish_reason_does_not_publish_a_partial_tool_call()
    {
        var call = ToolCallPayload("call_a", "set_camera", "{\"fov\":");
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "length" },
            },
        };
        using var runtime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(call) + Data(finish) + "data: [DONE]\n\n"))));

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(Request("adjust"))));

        Assert.Equal("inference_stream_truncated", exception.Code);
        Assert.True(exception.Retryable);
    }

    [Fact]
    public async Task Duplicate_ids_and_excessive_parallel_call_indices_are_rejected()
    {
        var duplicateIds = new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index = 0,
                                id = "duplicate_id",
                                type = "function",
                                function = new { name = "set_camera", arguments = "{}" },
                            },
                            new
                            {
                                index = 1,
                                id = "duplicate_id",
                                type = "function",
                                function = new { name = "get_state", arguments = "{}" },
                            },
                        },
                    },
                },
            },
        };
        var finish = new
        {
            choices = new[]
            {
                new { index = 0, delta = new { }, finish_reason = "tool_calls" },
            },
        };
        using var duplicateRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(duplicateIds) + Data(finish) + "data: [DONE]\n\n"))));
        var duplicateException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(duplicateRuntime.StreamAsync(Request("adjust"))));
        Assert.Equal("inference_protocol_error", duplicateException.Code);

        var excessiveIndex = ToolCallPayload(
            "call_too_many",
            "set_camera",
            "{}",
            ChatInferenceLimits.MaximumToolCalls);
        using var excessiveRuntime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(excessiveIndex) + "data: [DONE]\n\n"))));
        var excessiveException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(excessiveRuntime.StreamAsync(Request("adjust"))));
        Assert.Equal("inference_protocol_error", excessiveException.Code);
    }

    [Fact]
    public async Task Tool_argument_output_budget_is_enforced_before_call_exposure()
    {
        var oversizedArguments = "{\"value\":\"" +
            new string('x', ChatInferenceLimits.MaximumOutputCharacters) +
            "\"}";
        var payload = ToolCallPayload("call_a", "set_camera", oversizedArguments);
        using var runtime = Runtime(new DelegateHandler((_, _) => Task.FromResult(
            SseResponse(Data(payload) + "data: [DONE]\n\n"))));

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(Request("adjust"))));

        Assert.Equal("inference_output_too_large", exception.Code);
        Assert.DoesNotContain(oversizedArguments, exception.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Rejects_invalid_catalog_and_mismatched_tool_result_before_http()
    {
        const string malformedSchema = "{\"secret-schema-marker\":1,\"secret-schema-marker\":2}";
        var handler = new DelegateHandler((_, _) =>
            throw new InvalidOperationException("HTTP must not be attempted."));
        using var runtime = Runtime(handler);
        var invalidCatalog = Request(
            "question",
            tools:
            [
                new ChatInferenceToolDefinition("set_camera", null, malformedSchema),
            ]);
        var call = new ChatInferenceToolCall("call_a", "set_camera", "{}");
        var mismatchedResult = Request(
            string.Empty,
            [
                new ChatInferenceMessage(ChatInferenceRole.User, "question"),
                new ChatInferenceMessage(
                    ChatInferenceRole.Assistant,
                    string.Empty,
                    ToolCalls: [call]),
                new ChatInferenceMessage(
                    ChatInferenceRole.Tool,
                    "{}",
                    "another_tool",
                    "call_a"),
            ],
            [new ChatInferenceToolDefinition("set_camera", null, "{\"type\":\"object\"}")],
            appendUserMessage: false);

        var catalogException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(invalidCatalog)));
        var resultException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(mismatchedResult)));

        Assert.Equal("invalid_inference_request", catalogException.Code);
        Assert.Equal("invalid_inference_request", resultException.Code);
        Assert.DoesNotContain(malformedSchema, catalogException.ToString(), StringComparison.Ordinal);
        Assert.Equal(0, handler.RequestCount);
    }

    [Fact]
    public async Task Inference_catalog_caps_count_fields_and_aggregate_utf8_bytes()
    {
        Assert.Equal(128, ChatInferenceLimits.MaximumTools);
        Assert.Equal(4 * 1024, ChatInferenceLimits.MaximumToolDescriptionCharacters);
        Assert.Equal(64 * 1024, ChatInferenceLimits.MaximumToolSchemaCharacters);
        Assert.Equal(512 * 1024, ChatInferenceLimits.MaximumToolCatalogBytes);
        var handler = new DelegateHandler((_, _) =>
            throw new InvalidOperationException("HTTP must not be attempted."));
        using var runtime = Runtime(handler);
        var tooMany = Enumerable.Range(0, ChatInferenceLimits.MaximumTools + 1)
            .Select(index => new ChatInferenceToolDefinition(
                $"tool_{index}",
                null,
                "{\"type\":\"object\"}"))
            .ToArray();
        var longDescription = new ChatInferenceToolDefinition(
            "long_description",
            new string('d', ChatInferenceLimits.MaximumToolDescriptionCharacters + 1),
            "{\"type\":\"object\"}");
        var oversizedSchema = new ChatInferenceToolDefinition(
            "large_schema",
            null,
            "{\"description\":\"" +
                new string('s', ChatInferenceLimits.MaximumToolSchemaCharacters) +
                "\"}");
        var utf8HeavyCatalog = Enumerable.Range(0, 43)
            .Select(index => new ChatInferenceToolDefinition(
                $"unicode_tool_{index}",
                new string('界', ChatInferenceLimits.MaximumToolDescriptionCharacters),
                "{\"type\":\"object\"}"))
            .ToArray();
        var invalidRequests = new[]
        {
            Request("question", tools: tooMany),
            Request("question", tools: [longDescription]),
            Request("question", tools: [oversizedSchema]),
            Request("question", tools: utf8HeavyCatalog),
        };

        foreach (var request in invalidRequests)
        {
            var exception = await Assert.ThrowsAsync<ChatInferenceException>(
                () => CollectChunksAsync(runtime.StreamAsync(request)));
            Assert.Equal("invalid_inference_request", exception.Code);
        }
        Assert.Equal(0, handler.RequestCount);
    }

    [Fact]
    public void Production_handler_disables_proxy_redirects_and_cookies()
    {
        using var handler = Assert.IsType<SocketsHttpHandler>(
            LlamaServerChatInferenceRuntime.CreateDefaultHandler());
        Assert.False(handler.UseProxy);
        Assert.False(handler.AllowAutoRedirect);
        Assert.False(handler.UseCookies);
        Assert.NotNull(handler.ConnectCallback);
    }

    [Fact]
    public async Task Production_handler_sends_http_only_over_a_verified_process_connection()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        using var platform = new SystemLlamaServerPlatform();
        using var registration = platform.RegisterTrustedEndpoint(port, Environment.ProcessId);
        using var handler = LlamaServerChatInferenceRuntime.CreateDefaultHandler();
        using var client = new HttpClient(handler, disposeHandler: false)
        {
            Timeout = TimeSpan.FromSeconds(5),
        };
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var responseTask = client.GetAsync(
            new Uri($"http://127.0.0.1:{port}/health"),
            cancellation.Token);
        using var accepted = await listener.AcceptSocketAsync(cancellation.Token);
        using var stream = new NetworkStream(accepted, ownsSocket: false);
        var requestBuffer = new byte[4096];

        var requestBytes = await stream.ReadAsync(requestBuffer, cancellation.Token);
        Assert.True(requestBytes > 0);
        await stream.WriteAsync(
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"u8.ToArray(),
            cancellation.Token);
        using var response = await responseTask;

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
    }

    [Fact]
    public async Task Cancellation_interrupts_an_open_sse_stream()
    {
        var stream = new BlockingSseStream(
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\"}}]}\n\n");
        var streamContent = new StreamContent(stream);
        streamContent.Headers.ContentType = new MediaTypeHeaderValue("text/event-stream");
        var handler = new DelegateHandler((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.OK) { Content = streamContent }));
        using var runtime = Runtime(handler);
        using var cancellation = new CancellationTokenSource();
        await using var enumerator = runtime.StreamAsync(
            Request("cancel me"),
            cancellation.Token).GetAsyncEnumerator();

        Assert.True(await enumerator.MoveNextAsync());
        Assert.Equal("partial", enumerator.Current.Text);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => enumerator.MoveNextAsync().AsTask());
    }

    [Fact]
    public async Task End_of_stream_before_done_is_reported_as_truncated()
    {
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\"}}]}\n\n")));
        using var runtime = Runtime(handler);
        await using var enumerator = runtime.StreamAsync(Request("question")).GetAsyncEnumerator();

        Assert.True(await enumerator.MoveNextAsync());
        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => enumerator.MoveNextAsync().AsTask());

        Assert.Equal("inference_stream_truncated", exception.Code);
        Assert.True(exception.Retryable);
    }

    [Fact]
    public async Task Done_without_an_explicit_finish_reason_is_a_protocol_error()
    {
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            "data: [DONE]\n\n")));
        using var runtime = Runtime(handler);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectChunksAsync(runtime.StreamAsync(Request("question"))));

        Assert.Equal("inference_protocol_error", exception.Code);
    }

    [Fact]
    public async Task Http_error_does_not_expose_api_key_endpoint_or_server_body()
    {
        const string sensitiveBody = "private-server-detail test-api-key";
        var handler = new DelegateHandler((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.BadGateway)
        {
            Content = new StringContent(sensitiveBody),
        }));
        using var runtime = Runtime(handler);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("question"))));

        Assert.Equal("inference_http_error", exception.Code);
        Assert.True(exception.Retryable);
        Assert.DoesNotContain(sensitiveBody, exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("test-api-key", exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("53123", exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(sensitiveBody, exception.ToString(), StringComparison.Ordinal);
        Assert.DoesNotContain("test-api-key", exception.ToString(), StringComparison.Ordinal);
        Assert.DoesNotContain("53123", exception.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Malformed_sse_payload_is_a_safe_protocol_error()
    {
        const string sensitivePayload = "secret-invalid-json";
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            $"data: {{{sensitivePayload}}}\n\ndata: [DONE]\n\n")));
        using var runtime = Runtime(handler);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("question"))));

        Assert.Equal("inference_protocol_error", exception.Code);
        Assert.DoesNotContain(sensitivePayload, exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(sensitivePayload, exception.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Oversized_sse_line_is_rejected_before_json_parsing()
    {
        const string sensitiveMarker = "oversized-secret-marker";
        var oversizedLine = "data: " + sensitiveMarker + new string('x', 512 * 1024);
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            oversizedLine + "\ndata: [DONE]\n\n")));
        using var runtime = Runtime(handler);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("question"))));

        Assert.Equal("inference_protocol_error", exception.Code);
        Assert.DoesNotContain(sensitiveMarker, exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(sensitiveMarker, exception.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Explicit_non_sse_content_type_is_rejected()
    {
        var handler = new DelegateHandler((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("data: [DONE]\n\n", Encoding.UTF8, "application/json"),
        }));
        using var runtime = Runtime(handler);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("question"))));

        Assert.Equal("inference_protocol_error", exception.Code);
    }

    [Fact]
    public async Task Missing_content_type_is_rejected_and_idle_stream_is_bounded()
    {
        var missingTypeHandler = new DelegateHandler((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new ByteArrayContent(Encoding.UTF8.GetBytes("data: [DONE]\n\n")),
            }));
        using var missingTypeRuntime = Runtime(missingTypeHandler);
        var missingTypeException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(missingTypeRuntime.StreamAsync(Request("question"))));
        Assert.Equal("inference_protocol_error", missingTypeException.Code);

        var blockingContent = new StreamContent(new BlockingSseStream(string.Empty));
        blockingContent.Headers.ContentType = new MediaTypeHeaderValue("text/event-stream");
        var idleHandler = new DelegateHandler((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.OK) { Content = blockingContent }));
        using var idleRuntime = new LlamaServerChatInferenceRuntime(
            new FakeSessionProvider(Session()),
            idleHandler,
            TimeSpan.FromMilliseconds(50));
        var idleException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(idleRuntime.StreamAsync(Request("question"))));
        Assert.Equal("inference_stream_timeout", idleException.Code);
        Assert.True(idleException.Retryable);
    }

    [Theory]
    [InlineData("https://127.0.0.1:53123/")]
    [InlineData("http://example.com:53123/")]
    [InlineData("http://192.168.1.10:53123/")]
    public async Task Rejects_non_http_or_non_loopback_session_endpoints(string endpoint)
    {
        var handler = new DelegateHandler((_, _) =>
            throw new InvalidOperationException("HTTP must not be attempted."));
        using var runtime = Runtime(handler, Session(endpoint: new Uri(endpoint)));

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => runtime.GetStatusAsync().AsTask());

        Assert.Equal("invalid_inference_session", exception.Code);
        Assert.Equal(0, handler.RequestCount);
    }

    [Fact]
    public async Task Localhost_session_is_accepted_as_loopback()
    {
        using var runtime = Runtime(
            new DelegateHandler((_, _) => throw new InvalidOperationException()),
            Session(endpoint: new Uri("http://localhost:53123/"), backend: "vulkan"));

        var status = await runtime.GetStatusAsync();

        Assert.True(status.IsReady);
        Assert.Equal("llama-server-vulkan", status.RuntimeId);
    }

    [Fact]
    public async Task Missing_session_reports_not_ready_and_stream_fails_safely()
    {
        var handler = new DelegateHandler((_, _) => throw new InvalidOperationException());
        using var runtime = new LlamaServerChatInferenceRuntime(
            new FakeSessionProvider(null),
            handler);

        var status = await runtime.GetStatusAsync();
        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("question"))));

        Assert.False(status.IsReady);
        Assert.Equal("not_ready", status.State);
        Assert.Equal("inference_runtime_not_ready", exception.Code);
        Assert.True(exception.Retryable);
        Assert.Equal(0, handler.RequestCount);
    }

    [Fact]
    public async Task Provider_failure_and_invalid_api_key_are_redacted()
    {
        const string providerSecret = "provider-secret-api-key";
        using var providerFailureRuntime = new LlamaServerChatInferenceRuntime(
            new ThrowingSessionProvider(providerSecret),
            new DelegateHandler((_, _) => throw new InvalidOperationException()));

        var providerException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => providerFailureRuntime.GetStatusAsync().AsTask());

        const string invalidApiKey = "invalid secret,key";
        var invalidSession = Session() with { ApiKey = invalidApiKey };
        var handler = new DelegateHandler((_, _) =>
            throw new InvalidOperationException("HTTP must not be attempted."));
        using var invalidSessionRuntime = Runtime(handler, invalidSession);
        var sessionException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => invalidSessionRuntime.GetStatusAsync().AsTask());

        Assert.Equal("inference_session_failed", providerException.Code);
        Assert.DoesNotContain(providerSecret, providerException.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(providerSecret, providerException.ToString(), StringComparison.Ordinal);
        Assert.Equal("invalid_inference_session", sessionException.Code);
        Assert.DoesNotContain(invalidApiKey, sessionException.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(invalidApiKey, sessionException.ToString(), StringComparison.Ordinal);
        Assert.Equal(0, handler.RequestCount);
        Assert.Equal(nameof(LlamaServerSession), Session().ToString());
        Assert.DoesNotContain("test-api-key", Session().ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Input_and_output_character_limits_are_enforced_before_exposure()
    {
        var oversizedDelta = new string(
            'x',
            ChatInferenceLimits.MaximumOutputCharacters + 1);
        var payload = JsonSerializer.Serialize(new
        {
            choices = new[] { new { index = 0, delta = new { content = oversizedDelta } } },
        });
        var handler = new DelegateHandler((_, _) => Task.FromResult(SseResponse(
            $"data: {payload}\n\ndata: [DONE]\n\n")));
        using var runtime = Runtime(handler);

        var inputException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request(new string(
                'u',
                ChatInferenceLimits.MaximumInputCharacters + 1)))));
        var outputException = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request("valid"))));

        Assert.Equal("invalid_inference_request", inputException.Code);
        Assert.Equal("inference_output_too_large", outputException.Code);
        Assert.Equal(1, handler.RequestCount);
    }

    private static LlamaServerChatInferenceRuntime Runtime(
        HttpMessageHandler handler,
        LlamaServerSession? session = null) => new(
            new FakeSessionProvider(session ?? Session()),
            handler);

    private static LlamaServerSession Session(
        Uri? endpoint = null,
        string backend = "cuda") => new(
            endpoint ?? new Uri("http://127.0.0.1:53123/"),
            "test-api-key",
            backend,
            "gemma-test",
            Temperature: 0.25,
            MaxTokens: 512);

    private static ChatInferenceRequest Request(
        string text,
        IReadOnlyList<ChatInferenceMessage>? history = null,
        IReadOnlyList<ChatInferenceToolDefinition>? tools = null,
        bool appendUserMessage = true,
        bool allowToolCalls = true) => new(
            Guid.NewGuid(),
            "test-project",
            history ?? Array.Empty<ChatInferenceMessage>(),
            text,
            tools,
            appendUserMessage,
            allowToolCalls);

    private static string Data(object payload) =>
        $"data: {JsonSerializer.Serialize(payload)}\n\n";

    private static object StopPayload() => new
    {
        choices = new[]
        {
            new { index = 0, delta = new { }, finish_reason = "stop" },
        },
    };

    private static object ToolCallPayload(
        string id,
        string name,
        string arguments,
        int index = 0) => new
        {
            choices = new[]
            {
                new
                {
                    index = 0,
                    delta = new
                    {
                        tool_calls = new[]
                        {
                            new
                            {
                                index,
                                id,
                                type = "function",
                                function = new { name, arguments },
                            },
                        },
                    },
                    finish_reason = (string?)null,
                },
            },
        };

    private static HttpResponseMessage SseResponse(string content) => new(HttpStatusCode.OK)
    {
        Content = new StringContent(content, Encoding.UTF8, "text/event-stream"),
    };

    private static async Task<List<string>> CollectAsync(
        IAsyncEnumerable<ChatInferenceChunk> source)
    {
        var result = new List<string>();
        await foreach (var chunk in source)
            result.Add(chunk.Text);
        return result;
    }

    private static async Task<List<ChatInferenceChunk>> CollectChunksAsync(
        IAsyncEnumerable<ChatInferenceChunk> source)
    {
        var result = new List<ChatInferenceChunk>();
        await foreach (var chunk in source)
            result.Add(chunk);
        return result;
    }

    private sealed class FakeSessionProvider(LlamaServerSession? session)
        : ILlamaServerSessionProvider
    {
        public ValueTask<LlamaServerSession?> GetSessionAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(session);
        }
    }

    private sealed class ThrowingSessionProvider(string message)
        : ILlamaServerSessionProvider
    {
        public ValueTask<LlamaServerSession?> GetSessionAsync(
            CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException(message);
    }

    private sealed class DelegateHandler(
        Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send)
        : HttpMessageHandler
    {
        private int _requestCount;

        public int RequestCount => Volatile.Read(ref _requestCount);

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref _requestCount);
            return send(request, cancellationToken);
        }
    }

    private sealed class BlockingSseStream(string initialContent) : Stream
    {
        private readonly byte[] _initialBytes = Encoding.UTF8.GetBytes(initialContent);
        private readonly TaskCompletionSource _blockingReadStarted = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private int _offset;

        public Task BlockingReadStarted => _blockingReadStarted.Task;

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public override int Read(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();

        public override Task<int> ReadAsync(
            byte[] buffer,
            int offset,
            int count,
            CancellationToken cancellationToken) =>
            ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            if (_offset < _initialBytes.Length)
            {
                var count = Math.Min(buffer.Length, _initialBytes.Length - _offset);
                _initialBytes.AsMemory(_offset, count).CopyTo(buffer);
                _offset += count;
                return count;
            }

            _blockingReadStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();
        public override void SetLength(long value) =>
            throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();
    }
}
