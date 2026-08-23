using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.ChatHost.Mcp;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class SameInstanceMcpClientTests
{
    [Theory]
    [InlineData("http://localhost:8777/mcp")]
    [InlineData("http://[::1]:8777/mcp")]
    [InlineData("https://127.0.0.1:8777/mcp")]
    [InlineData("http://127.0.0.1:8777/")]
    [InlineData("http://user@127.0.0.1:8777/mcp")]
    [InlineData("http://127.0.0.1:8777/mcp?secret=value")]
    [InlineData("http://127.0.0.1:8777/mcp#fragment")]
    public void Capability_rejects_everything_except_the_exact_ipv4_endpoint(string endpoint)
    {
        Assert.Throws<ArgumentException>(() => SameInstanceMcpCapability.Create(endpoint, "secret"));
    }

    [Fact]
    public void Capability_redacts_itself_and_rejects_header_injection()
    {
        using var capability = SameInstanceMcpCapability.Create(
            "http://127.0.0.1:8777/mcp",
            "not-printed");

        Assert.Equal("[same-instance MCP capability]", capability.ToString());
        Assert.DoesNotContain("not-printed", capability.ToString(), StringComparison.Ordinal);
        Assert.Throws<ArgumentException>(() => SameInstanceMcpCapability.Create(
            "http://127.0.0.1:8777/mcp",
            "secret\r\nInjected: true"));
    }

    [Fact]
    public async Task Dispose_cancels_a_response_body_that_stalls_after_headers()
    {
        var content = new BlockingContent();
        var handler = new ScriptedHandler(_ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = content,
        });
        var client = CreateClient(handler);
        var catalog = client.GetToolsAsync();
        await content.ReadStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));

        await client.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        var failure = await Assert.ThrowsAsync<SameInstanceMcpException>(() => catalog);
        Assert.Equal("timeout", failure.Code);
    }

    [Fact]
    public async Task Dispose_deletes_the_dedicated_legacy_session()
    {
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.get_state","description":"state","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]}"""),
            request =>
            {
                Assert.Equal(HttpMethod.Delete, request.Method);
                Assert.Equal(
                    "dedicated-session",
                    request.Headers.GetValues("MCP-Session-Id").Single());
                Assert.Null(request.Content);
                return new HttpResponseMessage(HttpStatusCode.Accepted);
            });
        var client = CreateClient(handler);

        _ = await client.GetToolsAsync();
        await client.DisposeAsync();

        Assert.Equal(4, handler.RequestCount);
    }

    [Fact]
    public async Task Catalog_preserves_external_ids_and_read_only_annotations_across_pages()
    {
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.get_state","description":"state","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}],"nextCursor":"page-2"}"""),
            request =>
            {
                using var body = ParseRequest(request);
                Assert.Equal("page-2", body.RootElement.GetProperty("params").GetProperty("cursor").GetString());
                return JsonResponse(
                    request,
                    3,
                    """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"}}]}""");
            });
        await using var client = CreateClient(handler);

        var tools = await client.GetToolsAsync();

        Assert.Collection(
            tools,
            tool =>
            {
                Assert.Equal("lookdevpt.get_state", tool.Name);
                Assert.True(tool.IsReadOnly);
            },
            tool =>
            {
                Assert.Equal("lookdevpt.set_camera", tool.Name);
                Assert.False(tool.IsReadOnly);
            });
        Assert.All(handler.AuthorizationValues, value => Assert.Equal("Bearer memory-only-token", value));
        Assert.Equal(ProtocolVersion, handler.ProtocolVersions.Distinct(StringComparer.Ordinal).Single());
    }

    [Fact]
    public async Task Read_only_tool_executes_without_approval_metadata()
    {
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.get_state","description":"state","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]}"""),
            request =>
            {
                using var body = ParseRequest(request);
                var parameters = body.RootElement.GetProperty("params");
                Assert.Equal("lookdevpt.get_state", parameters.GetProperty("name").GetString());
                Assert.False(parameters.TryGetProperty("_meta", out _));
                return JsonResponse(request, 3, """{"content":[{"type":"text","text":"ok"}]}""");
            });
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("{}");

        var result = await client.CallToolAsync("lookdevpt.get_state", arguments.RootElement);

        Assert.False(result.IsError);
        Assert.Equal("lookdevpt.get_state", result.ToolName);
        Assert.Equal(4, handler.RequestCount);
    }

    [Fact]
    public async Task Mutation_requires_one_time_grant_and_sends_it_only_once()
    {
        const string grant = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":false}}]}"""),
            request =>
            {
                using var body = ParseRequest(request);
                var parameters = body.RootElement.GetProperty("params");
                Assert.Equal(grant, parameters.GetProperty("_meta")
                    .GetProperty("shaderjp.lookdevpt/approvalToken").GetString());
                Assert.Equal("dedicated-session", request.Headers.GetValues("MCP-Session-Id").Single());
                return JsonResponse(request, 3, """{"content":[{"type":"text","text":"changed"}]}""");
            });
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("""{"yaw":20,"pitch":-5}""");

        var missing = await Assert.ThrowsAsync<SameInstanceMcpException>(() =>
            client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement));
        Assert.Equal("approval_required", missing.Code);

        var result = await client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant);
        Assert.False(result.IsError);

        var reused = await Assert.ThrowsAsync<SameInstanceMcpException>(() =>
            client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant));
        Assert.Equal("approval_reused", reused.Code);
        Assert.Equal(4, handler.RequestCount);
    }

    [Fact]
    public async Task Used_grant_ledger_expires_without_exhausting_a_long_running_exhibition()
    {
        const string grant =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        var timeProvider = new ManualTimeProvider();
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"}}]}"""),
            request => JsonResponse(request, 3, """{"content":[{"type":"text","text":"changed"}]}"""),
            request => JsonResponse(request, 4, """{"content":[{"type":"text","text":"changed again"}]}"""));
        await using var client = CreateClient(handler, timeProvider);
        using var arguments = JsonDocument.Parse("""{"yaw":20}""");

        _ = await client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant);
        var reused = await Assert.ThrowsAsync<SameInstanceMcpException>(() =>
            client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant));
        Assert.Equal("approval_reused", reused.Code);

        timeProvider.Advance(TimeSpan.FromMinutes(1));
        _ = await client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant);

        Assert.Equal(5, handler.RequestCount);
    }

    [Fact]
    public async Task Approval_binding_uses_dedicated_session_external_tool_id_and_canonical_hash()
    {
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "dedicated-session"),
            NotificationResponse,
            request => JsonResponse(
                request,
                2,
                """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"}}]}"""),
            request => JsonResponse(request, 3, "{}"));
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("""{"yaw":20,"pitch":-5}""");

        var binding = await client.CreateApprovalBindingAsync(
            "lookdevpt.set_camera",
            arguments.RootElement);

        Assert.Equal("dedicated-session", binding.McpSessionId);
        Assert.Equal("lookdevpt.set_camera", binding.Tool);
        Assert.Equal(SameInstanceMcpArgumentHash.Compute(arguments.RootElement), binding.ArgumentsHash);
        Assert.DoesNotContain("dedicated-session", binding.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Approval_binding_renegotiates_an_expired_session_before_returning()
    {
        const string tools =
            """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"}}]}""";
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "expired-session"),
            request => NotificationResponse(request, "expired-session"),
            request => JsonResponse(request, 2, tools),
            request =>
            {
                AssertRequest(request, 3, "ping", "expired-session");
                return new HttpResponseMessage(HttpStatusCode.NotFound);
            },
            request => JsonResponse(
                request,
                4,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "renewed-session"),
            request => NotificationResponse(request, "renewed-session"),
            request => JsonResponse(request, 5, tools),
            request => JsonResponse(request, 6, "{}"));
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("""{"yaw":20}""");

        var binding = await client.CreateApprovalBindingAsync(
            "lookdevpt.set_camera",
            arguments.RootElement);

        Assert.Equal("renewed-session", binding.McpSessionId);
        Assert.Equal(8, handler.RequestCount);
    }

    [Fact]
    public async Task Read_only_tool_renegotiates_and_retries_once_after_session_expiry()
    {
        const string tools =
            """{"tools":[{"name":"lookdevpt.get_state","description":"state","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]}""";
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "expired-session"),
            request => NotificationResponse(request, "expired-session"),
            request => JsonResponse(request, 2, tools),
            request =>
            {
                AssertRequest(request, 3, "tools/call", "expired-session");
                return new HttpResponseMessage(HttpStatusCode.NotFound);
            },
            request => JsonResponse(
                request,
                4,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "renewed-session"),
            request => NotificationResponse(request, "renewed-session"),
            request => JsonResponse(request, 5, tools),
            request =>
            {
                AssertRequest(request, 6, "tools/call", "renewed-session");
                return JsonResponse(request, 6, """{"content":[{"type":"text","text":"ok"}]}""");
            });
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("{}");

        var result = await client.CallToolAsync(
            "lookdevpt.get_state",
            arguments.RootElement);

        Assert.False(result.IsError);
        Assert.Equal(8, handler.RequestCount);
    }

    [Fact]
    public async Task Mutation_never_retries_or_reuses_a_grant_after_session_expiry()
    {
        const string grant =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const string tools =
            """{"tools":[{"name":"lookdevpt.set_camera","description":"camera","inputSchema":{"type":"object"}}]}""";
        var handler = new ScriptedHandler(
            request => JsonResponse(
                request,
                1,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "expired-session"),
            request => NotificationResponse(request, "expired-session"),
            request => JsonResponse(request, 2, tools),
            request =>
            {
                AssertRequest(request, 3, "tools/call", "expired-session");
                return new HttpResponseMessage(HttpStatusCode.NotFound);
            },
            request => JsonResponse(
                request,
                4,
                """{"protocolVersion":"2025-11-25"}""",
                sessionId: "renewed-session"),
            request => NotificationResponse(request, "renewed-session"),
            request => JsonResponse(request, 5, tools));
        await using var client = CreateClient(handler);
        using var arguments = JsonDocument.Parse("""{"yaw":20}""");

        var expired = await Assert.ThrowsAsync<SameInstanceMcpException>(() =>
            client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant));
        Assert.Equal("approval_session_expired", expired.Code);
        Assert.Equal(4, handler.RequestCount);

        var reused = await Assert.ThrowsAsync<SameInstanceMcpException>(() =>
            client.CallToolAsync("lookdevpt.set_camera", arguments.RootElement, grant));
        Assert.Equal("approval_reused", reused.Code);
        Assert.Equal(7, handler.RequestCount);
    }

    [Theory]
    [InlineData("{}", "{}", "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a")]
    [InlineData("{\"b\":1,\"a\":-0}", "{\"a\":0,\"b\":1e0}", "2f954b957c86ae054ee0935643ad1f0dd7522789a6490bd06a116978447b012b")]
    [InlineData("{\"text\":\"line\\n日本語\",\"values\":[true,null,0.5]}", "{\"text\":\"line\\n日本語\",\"values\":[true,null,5e-1]}", "bc39897afca6235bae005c4ac4eee62f819ab375661018e5e5c500da29712546")]
    [InlineData("{\"tiny\":1e-8,\"large\":1e20,\"small\":1e-7}", "{\"large\":1e20,\"small\":9.9999999999999995e-8,\"tiny\":1e-8}", "ffd94ad98d15b3ef7f3ba1bd7c3cebd60062be66388df869d700b60de5f5504c")]
    [InlineData("{\"😀\":2,\"\":1}", "{\"\":1e0,\"😀\":2e0}", "0c17a92c27cd16a347789cf9fe4b62e7d020ae5e2c3d8371cf43ac41f8ed30ef")]
    [InlineData("{\"v\":1000000}", "{\"v\":1e6}", "ce11dcd09748a235500ecf8323fa48697351a2ba66a557ebc5b3478993706729")]
    [InlineData("{\"v\":1e15}", "{\"v\":1e15}", "363ed7a10f2aaa15a2dfbd110ff2cd57f46ac77b5ee3834d1a2fd73f93490c88")]
    [InlineData("{\"v\":1e16}", "{\"v\":1e16}", "80c8d25e04ccc932a94aa6fdcc0c90ee36f23665b313b0a3dad06fe333f8d606")]
    [InlineData("{\"v\":1e17}", "{\"v\":1e17}", "031294a3c1bd126b83c6e02a51cb3accdb3e39b5ec31da5f585d3b655c1374ae")]
    [InlineData("{\"v\":1e-4}", "{\"v\":1e-4}", "9c3f563e731898097d62d45d3c9cab62b7804c3b0e3fd3892a3cff32a222e475")]
    [InlineData("{\"v\":1e-5}", "{\"v\":1.0000000000000001e-5}", "d87d46e7098aedb95fb6cca01d36d8da61136e1f5a51291916cb5abf7262f5b1")]
    [InlineData("{\"v\":1.0000000000000002}", "{\"v\":1.0000000000000002e0}", "c9d17a945f8b692b317d3d7b86dd11afafdace37e8e20ebbac3596043312d1bc")]
    [InlineData("{\"v\":0.9999999999999999}", "{\"v\":9.9999999999999989e-1}", "490716aaf54aa8133e2ee995bc7dcd6e80caa0ab18aa5d262731ae0eb7a1c939")]
    [InlineData("{\"v\":1.7976931348623157e308}", "{\"v\":1.7976931348623157e308}", "ee16b8e5a24a3e4a3e566dc4da9a4afa993274e80d3744e71987b3bd32312f50")]
    [InlineData("{\"v\":5e-324}", "{\"v\":4.9406564584124654e-324}", "e78ddea1a7937dbd43be5cceeae312be7fb38ccf3b908835ac066e6174b658b7")]
    [InlineData("{\"v\":-1000000}", "{\"v\":-1e6}", "be4dcb04db2f33d2433b16a150d26802dad440a10f474e378b542eff8da5b21b")]
    public void Canonical_argument_hash_matches_cross_language_fixtures(
        string json,
        string canonical,
        string expectedSha256)
    {
        using var document = JsonDocument.Parse(json);

        Assert.Equal(canonical, SameInstanceMcpArgumentHash.Canonicalize(document.RootElement));
        Assert.Equal(expectedSha256, SameInstanceMcpArgumentHash.Compute(document.RootElement));
    }

    [Fact]
    public void Canonical_argument_hash_rejects_duplicate_object_properties()
    {
        using var document = JsonDocument.Parse("""{"value":1,"value":2}""");

        Assert.Throws<ArgumentException>(() =>
            SameInstanceMcpArgumentHash.Canonicalize(document.RootElement));
        Assert.Throws<ArgumentException>(() =>
            SameInstanceMcpArgumentHash.Compute(document.RootElement));
    }

    private const string ProtocolVersion = "2025-11-25";

    private static SameInstanceMcpClient CreateClient(
        HttpMessageHandler handler,
        TimeProvider? timeProvider = null) => new(
        SameInstanceMcpCapability.Create("http://127.0.0.1:8777/mcp", "memory-only-token"),
        Environment.ProcessId,
        handler,
        timeProvider);

    private static HttpResponseMessage NotificationResponse(HttpRequestMessage request)
        => NotificationResponse(request, "dedicated-session");

    private static HttpResponseMessage NotificationResponse(
        HttpRequestMessage request,
        string sessionId)
    {
        using var body = ParseRequest(request);
        Assert.Equal("notifications/initialized", body.RootElement.GetProperty("method").GetString());
        Assert.False(body.RootElement.TryGetProperty("id", out _));
        Assert.Equal(sessionId, request.Headers.GetValues("MCP-Session-Id").Single());
        return new HttpResponseMessage(HttpStatusCode.Accepted);
    }

    private static void AssertRequest(
        HttpRequestMessage request,
        long id,
        string method,
        string sessionId)
    {
        using var body = ParseRequest(request);
        Assert.Equal(id, body.RootElement.GetProperty("id").GetInt64());
        Assert.Equal(method, body.RootElement.GetProperty("method").GetString());
        Assert.Equal(sessionId, request.Headers.GetValues("MCP-Session-Id").Single());
    }

    private static HttpResponseMessage JsonResponse(
        HttpRequestMessage request,
        long id,
        string result,
        string? sessionId = null)
    {
        using var body = ParseRequest(request);
        Assert.Equal(id, body.RootElement.GetProperty("id").GetInt64());
        var response = new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent(
                $$"""{"jsonrpc":"2.0","id":{{id}},"result":{{result}}}""",
                Encoding.UTF8,
                "application/json"),
        };
        if (sessionId is not null) response.Headers.TryAddWithoutValidation("MCP-Session-Id", sessionId);
        return response;
    }

    private static JsonDocument ParseRequest(HttpRequestMessage request) =>
        JsonDocument.Parse(request.Content!.ReadAsStringAsync().GetAwaiter().GetResult());

    private sealed class ScriptedHandler(params Func<HttpRequestMessage, HttpResponseMessage>[] responses)
        : HttpMessageHandler
    {
        private readonly Queue<Func<HttpRequestMessage, HttpResponseMessage>> _responses = new(responses);

        public List<string> AuthorizationValues { get; } = [];
        public List<string> ProtocolVersions { get; } = [];
        public int RequestCount { get; private set; }

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            RequestCount++;
            AuthorizationValues.Add(request.Headers.GetValues("Authorization").Single());
            ProtocolVersions.Add(request.Headers.GetValues("MCP-Protocol-Version").Single());
            Assert.Equal("127.0.0.1", request.RequestUri!.Host);
            Assert.Equal("/mcp", request.RequestUri.AbsolutePath);
            Assert.NotEmpty(_responses);
            return Task.FromResult(_responses.Dequeue()(request));
        }
    }

    private sealed class ManualTimeProvider : TimeProvider
    {
        private long _timestamp;

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => Volatile.Read(ref _timestamp);

        public void Advance(TimeSpan elapsed) =>
            Interlocked.Add(ref _timestamp, elapsed.Ticks);
    }

    private sealed class BlockingContent : HttpContent
    {
        public BlockingContent()
        {
            Headers.ContentType = new MediaTypeHeaderValue("application/json");
        }

        public TaskCompletionSource ReadStarted { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        protected override Task SerializeToStreamAsync(Stream stream, TransportContext? context) =>
            Task.CompletedTask;

        protected override bool TryComputeLength(out long length)
        {
            length = 0;
            return false;
        }

        protected override Task<Stream> CreateContentReadStreamAsync() =>
            Task.FromResult<Stream>(new BlockingReadStream(ReadStarted));

        protected override Task<Stream> CreateContentReadStreamAsync(CancellationToken cancellationToken) =>
            Task.FromResult<Stream>(new BlockingReadStream(ReadStarted));
    }

    private sealed class BlockingReadStream(TaskCompletionSource readStarted) : Stream
    {
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

        public override async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default)
        {
            readStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
