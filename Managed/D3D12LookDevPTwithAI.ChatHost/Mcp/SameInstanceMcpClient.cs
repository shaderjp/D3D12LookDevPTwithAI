using System.Buffers;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.ChatHost.Mcp;

public static class SameInstanceMcpLimits
{
    public const int MaximumBearerTokenBytes = 4 * 1024;
    public const int MaximumResponseBytes = 4 * 1024 * 1024;
    public const int MaximumTools = 512;
    public const int MaximumToolNameCharacters = 256;
    public const int MaximumToolDescriptionCharacters = 64 * 1024;
    public const int MaximumSessionIdCharacters = 256;
    public const int MaximumPages = 64;
    public const int MaximumUsedApprovalGrants = 512;
}

/// <summary>
/// A process-memory-only capability received from the authenticated native
/// parent. The bearer token is copied into a wipeable buffer and is never
/// exposed through ToString. The caller should dispose its source DTO as soon
/// as the capability has been created.
/// </summary>
public sealed class SameInstanceMcpCapability : IDisposable
{
    private byte[]? _bearerToken;

    private SameInstanceMcpCapability(Uri endpoint, byte[] bearerToken)
    {
        Endpoint = endpoint;
        _bearerToken = bearerToken;
    }

    internal Uri Endpoint { get; }

    public static SameInstanceMcpCapability Create(string endpoint, string bearerToken)
    {
        if (!Uri.TryCreate(endpoint, UriKind.Absolute, out var uri) ||
            uri.Scheme != Uri.UriSchemeHttp ||
            !string.Equals(uri.Host, IPAddress.Loopback.ToString(), StringComparison.Ordinal) ||
            uri.Port is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort ||
            !string.Equals(uri.AbsolutePath, "/mcp", StringComparison.Ordinal) ||
            !string.IsNullOrEmpty(uri.UserInfo) ||
            !string.IsNullOrEmpty(uri.Query) ||
            !string.IsNullOrEmpty(uri.Fragment))
        {
            throw new ArgumentException(
                "The same-instance MCP endpoint must be an exact http://127.0.0.1:<port>/mcp URL.",
                nameof(endpoint));
        }
        if (string.IsNullOrWhiteSpace(bearerToken) ||
            bearerToken.Any(char.IsControl))
        {
            throw new ArgumentException("The same-instance MCP bearer token is invalid.", nameof(bearerToken));
        }

        var tokenBytes = Encoding.UTF8.GetBytes(bearerToken);
        if (tokenBytes.Length > SameInstanceMcpLimits.MaximumBearerTokenBytes)
        {
            CryptographicOperations.ZeroMemory(tokenBytes);
            throw new ArgumentException("The same-instance MCP bearer token is too large.", nameof(bearerToken));
        }
        return new SameInstanceMcpCapability(uri, tokenBytes);
    }

    internal byte[] CopyBearerToken()
    {
        ObjectDisposedException.ThrowIf(_bearerToken is null, this);
        return _bearerToken.ToArray();
    }

    public void Dispose()
    {
        var token = Interlocked.Exchange(ref _bearerToken, null);
        if (token is not null) CryptographicOperations.ZeroMemory(token);
    }

    public override string ToString() => "[same-instance MCP capability]";
}

public sealed record SameInstanceMcpTool(
    string Name,
    string Description,
    JsonElement InputSchema,
    bool IsReadOnly);

public sealed record SameInstanceMcpToolResult(
    string ToolName,
    JsonElement Result,
    bool IsError);

public sealed class SameInstanceMcpApprovalBinding(
    string mcpSessionId,
    string tool,
    string argumentsHash)
{
    public string McpSessionId { get; } = mcpSessionId;
    public string Tool { get; } = tool;
    public string ArgumentsHash { get; } = argumentsHash;

    public override string ToString() => "[same-instance MCP approval binding]";
}

public interface ISameInstanceMcpClient : IAsyncDisposable
{
    Task<IReadOnlyList<SameInstanceMcpTool>> GetToolsAsync(
        CancellationToken cancellationToken = default);

    Task<SameInstanceMcpToolResult> CallToolAsync(
        string toolName,
        JsonElement arguments,
        string? approvalGrant = null,
        CancellationToken cancellationToken = default);

    Task<SameInstanceMcpApprovalBinding> CreateApprovalBindingAsync(
        string toolName,
        JsonElement arguments,
        CancellationToken cancellationToken = default);
}

public interface ISameInstanceMcpClientFactory
{
    ISameInstanceMcpClient Create(string endpoint, string bearerToken);
}

public sealed class SameInstanceMcpClientFactory(int expectedServerProcessId)
    : ISameInstanceMcpClientFactory
{
    public ISameInstanceMcpClient Create(string endpoint, string bearerToken) =>
        new SameInstanceMcpClient(
            SameInstanceMcpCapability.Create(endpoint, bearerToken),
            expectedServerProcessId);
}

/// <summary>
/// Minimal MCP 2025-11-25 client for the LookDev server owned by the native
/// parent process. It intentionally has no profile, discovery, persistence,
/// stdio, remote HTTP, redirect, proxy, cookie, or logging surface.
/// </summary>
public sealed class SameInstanceMcpClient : ISameInstanceMcpClient
{
    private const string ProtocolVersion = "2025-11-25";
    private const string ProtocolVersionHeader = "MCP-Protocol-Version";
    private const string SessionIdHeader = "MCP-Session-Id";
    private const string ApprovalTokenProperty = "shaderjp.lookdevpt/approvalToken";
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan SessionDeleteTimeout = TimeSpan.FromMilliseconds(250);
    private readonly SameInstanceMcpCapability _capability;
    private readonly HttpClient _httpClient;
    private readonly SystemLlamaServerPlatform? _ownershipPlatform;
    private readonly int _expectedServerProcessId;
    private readonly bool _verifyProcessOwnership;
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly HashSet<string> _usedApprovalGrantHashes = new(StringComparer.Ordinal);
    private IReadOnlyDictionary<string, SameInstanceMcpTool>? _tools;
    private string? _sessionId;
    private long _nextRequestId;
    private int _disposed;

    public SameInstanceMcpClient(
        SameInstanceMcpCapability capability,
        int expectedServerProcessId,
        HttpMessageHandler? handler = null)
    {
        ArgumentNullException.ThrowIfNull(capability);
        if (expectedServerProcessId <= 0)
            throw new ArgumentOutOfRangeException(nameof(expectedServerProcessId));

        _capability = capability;
        _expectedServerProcessId = expectedServerProcessId;
        _verifyProcessOwnership = handler is null;
        _ownershipPlatform = handler is null ? new SystemLlamaServerPlatform() : null;
        _httpClient = new HttpClient(
            handler ?? CreateProductionHandler(capability.Endpoint.Port, expectedServerProcessId),
            disposeHandler: true)
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
    }

    public async Task<IReadOnlyList<SameInstanceMcpTool>> GetToolsAsync(
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        await EnterGateAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await RunWithOneSessionRecoveryAsync(
                async () =>
                {
                    await EnsureInitializedAsync(cancellationToken).ConfigureAwait(false);
                    await EnsureToolsLoadedAsync(cancellationToken).ConfigureAwait(false);
                    return _tools!.Values
                        .OrderBy(tool => tool.Name, StringComparer.Ordinal)
                        .ToArray();
                }).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task<SameInstanceMcpToolResult> CallToolAsync(
        string toolName,
        JsonElement arguments,
        string? approvalGrant = null,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        if (string.IsNullOrWhiteSpace(toolName) || toolName.Length > SameInstanceMcpLimits.MaximumToolNameCharacters)
            throw new ArgumentException("The MCP tool name is invalid.", nameof(toolName));
        if (arguments.ValueKind != JsonValueKind.Object)
            throw new ArgumentException("MCP tool arguments must be a JSON object.", nameof(arguments));

        await EnterGateAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            for (var attempt = 0; ; attempt++)
            {
                await EnsureInitializedAsync(cancellationToken).ConfigureAwait(false);
                await EnsureToolsLoadedAsync(cancellationToken).ConfigureAwait(false);
                if (!_tools!.TryGetValue(toolName, out var tool))
                    throw new SameInstanceMcpException("unknown_tool", "The requested LookDev tool is not available.");

                if (tool.IsReadOnly)
                {
                    if (approvalGrant is not null)
                        throw new SameInstanceMcpException("unexpected_approval", "Read-only tools do not accept approval grants.");
                }
                else
                {
                    if (!IsLowerHexSha256(approvalGrant))
                        throw new SameInstanceMcpException("approval_required", "This LookDev tool requires one-time approval.");
                    var grantHash = Convert.ToHexString(
                        SHA256.HashData(Encoding.UTF8.GetBytes(approvalGrant!)));
                    if (_usedApprovalGrantHashes.Count >= SameInstanceMcpLimits.MaximumUsedApprovalGrants)
                        throw new SameInstanceMcpException("approval_limit", "The one-time approval budget has been exhausted.");
                    if (!_usedApprovalGrantHashes.Add(grantHash))
                        throw new SameInstanceMcpException("approval_reused", "The one-time approval grant has already been used.");
                }

                try
                {
                    using var response = await SendRequestAsync(
                        "tools/call",
                        writer =>
                        {
                            writer.WriteStartObject();
                            writer.WriteString("name", tool.Name);
                            writer.WritePropertyName("arguments");
                            arguments.WriteTo(writer);
                            if (approvalGrant is not null)
                            {
                                writer.WritePropertyName("_meta");
                                writer.WriteStartObject();
                                writer.WriteString(ApprovalTokenProperty, approvalGrant);
                                writer.WriteEndObject();
                            }
                            writer.WriteEndObject();
                        },
                        cancellationToken).ConfigureAwait(false);
                    var result = GetResult(response.Document.RootElement);
                    var isError = result.TryGetProperty("isError", out var errorElement) &&
                        errorElement.ValueKind == JsonValueKind.True;
                    return new SameInstanceMcpToolResult(tool.Name, result.Clone(), isError);
                }
                catch (SameInstanceMcpException exception) when (
                    exception.Code == "session_expired")
                {
                    ResetSession();
                    if (!tool.IsReadOnly)
                    {
                        throw new SameInstanceMcpException(
                            "approval_session_expired",
                            "The private LookDev tool session changed; approve a fresh tool call.");
                    }
                    if (attempt != 0)
                    {
                        throw new SameInstanceMcpException(
                            "session_expired",
                            "The private LookDev tool session could not be renewed.");
                    }
                }
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task<SameInstanceMcpApprovalBinding> CreateApprovalBindingAsync(
        string toolName,
        JsonElement arguments,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        if (!IsSafeToolName(toolName))
            throw new ArgumentException("The MCP tool name is invalid.", nameof(toolName));
        if (arguments.ValueKind != JsonValueKind.Object)
            throw new ArgumentException("MCP tool arguments must be a JSON object.", nameof(arguments));

        await EnterGateAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await RunWithOneSessionRecoveryAsync(
                async () =>
                {
                    await EnsureInitializedAsync(cancellationToken).ConfigureAwait(false);
                    await EnsureToolsLoadedAsync(cancellationToken).ConfigureAwait(false);
                    if (!_tools!.TryGetValue(toolName, out var tool))
                        throw new SameInstanceMcpException("unknown_tool", "The requested LookDev tool is not available.");
                    if (tool.IsReadOnly)
                        throw new SameInstanceMcpException("approval_not_required", "The requested LookDev tool is read-only.");
                    await PingSessionAsync(cancellationToken).ConfigureAwait(false);
                    return new SameInstanceMcpApprovalBinding(
                        _sessionId!,
                        tool.Name,
                        SameInstanceMcpArgumentHash.Compute(arguments));
                }).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task EnsureInitializedAsync(CancellationToken cancellationToken)
    {
        if (_sessionId is not null) return;

        using var response = await SendRequestAsync(
            "initialize",
            writer =>
            {
                writer.WriteStartObject();
                writer.WriteString("protocolVersion", ProtocolVersion);
                writer.WritePropertyName("capabilities");
                writer.WriteStartObject();
                writer.WriteEndObject();
                writer.WritePropertyName("clientInfo");
                writer.WriteStartObject();
                writer.WriteString("name", "D3D12LookDevPTwithAI.ChatHost");
                writer.WriteString("version", "1");
                writer.WriteEndObject();
                writer.WriteEndObject();
            },
            cancellationToken,
            includeSession: false).ConfigureAwait(false);

        var result = GetResult(response.Document.RootElement);
        if (!result.TryGetProperty("protocolVersion", out var version) ||
            !string.Equals(version.GetString(), ProtocolVersion, StringComparison.Ordinal))
        {
            throw new SameInstanceMcpException("protocol_mismatch", "The LookDev MCP protocol negotiation failed.");
        }
        if (!response.Headers.TryGetValue(SessionIdHeader, out var values))
            throw new SameInstanceMcpException("missing_session", "The LookDev MCP session was not created.");
        var sessionId = values.SingleOrDefault();
        if (!IsSafeHeaderValue(sessionId, SameInstanceMcpLimits.MaximumSessionIdCharacters))
            throw new SameInstanceMcpException("invalid_session", "The LookDev MCP session identifier is invalid.");
        _sessionId = sessionId;

        await SendNotificationAsync("notifications/initialized", cancellationToken).ConfigureAwait(false);
        VerifyListenerOwnership();
    }

    private async Task PingSessionAsync(CancellationToken cancellationToken)
    {
        using var response = await SendRequestAsync(
            "ping",
            writer =>
            {
                writer.WriteStartObject();
                writer.WriteEndObject();
            },
            cancellationToken).ConfigureAwait(false);
        _ = GetResult(response.Document.RootElement);
    }

    private async Task<T> RunWithOneSessionRecoveryAsync<T>(Func<Task<T>> operation)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return await operation().ConfigureAwait(false);
            }
            catch (SameInstanceMcpException exception) when (
                exception.Code == "session_expired" && attempt == 0)
            {
                ResetSession();
            }
        }
    }

    private void ResetSession()
    {
        _sessionId = null;
        _tools = null;
    }

    private async Task EnterGateAsync(CancellationToken cancellationToken)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        try
        {
            await _gate.WaitAsync(linked.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (
            _lifetimeCancellation.IsCancellationRequested &&
            !cancellationToken.IsCancellationRequested)
        {
            throw new ObjectDisposedException(nameof(SameInstanceMcpClient));
        }
    }

    private async Task EnsureToolsLoadedAsync(CancellationToken cancellationToken)
    {
        if (_tools is not null) return;
        _tools = await LoadToolsAsync(cancellationToken).ConfigureAwait(false);
        VerifyListenerOwnership();
    }

    private async Task<IReadOnlyDictionary<string, SameInstanceMcpTool>> LoadToolsAsync(
        CancellationToken cancellationToken)
    {
        var tools = new Dictionary<string, SameInstanceMcpTool>(StringComparer.Ordinal);
        string? cursor = null;
        for (var page = 0; page < SameInstanceMcpLimits.MaximumPages; page++)
        {
            using var response = await SendRequestAsync(
                "tools/list",
                writer =>
                {
                    writer.WriteStartObject();
                    if (cursor is not null) writer.WriteString("cursor", cursor);
                    writer.WriteEndObject();
                },
                cancellationToken).ConfigureAwait(false);
            var result = GetResult(response.Document.RootElement);
            if (!result.TryGetProperty("tools", out var toolArray) || toolArray.ValueKind != JsonValueKind.Array)
                throw new SameInstanceMcpException("invalid_tools", "The LookDev MCP tool catalog is invalid.");
            foreach (var item in toolArray.EnumerateArray())
            {
                if (tools.Count >= SameInstanceMcpLimits.MaximumTools)
                    throw new SameInstanceMcpException("too_many_tools", "The LookDev MCP tool catalog is too large.");
                var tool = ParseTool(item);
                if (!tools.TryAdd(tool.Name, tool))
                    throw new SameInstanceMcpException("duplicate_tool", "The LookDev MCP tool catalog contains duplicate names.");
            }

            cursor = result.TryGetProperty("nextCursor", out var nextCursor) &&
                nextCursor.ValueKind == JsonValueKind.String
                ? nextCursor.GetString()
                : null;
            if (cursor is null) return tools;
            if (!IsSafeHeaderValue(cursor, SameInstanceMcpLimits.MaximumSessionIdCharacters))
                throw new SameInstanceMcpException("invalid_cursor", "The LookDev MCP tool cursor is invalid.");
        }
        throw new SameInstanceMcpException("too_many_pages", "The LookDev MCP tool catalog has too many pages.");
    }

    private static SameInstanceMcpTool ParseTool(JsonElement item)
    {
        if (item.ValueKind != JsonValueKind.Object ||
            !item.TryGetProperty("name", out var nameElement) ||
            nameElement.ValueKind != JsonValueKind.String)
        {
            throw new SameInstanceMcpException("invalid_tool", "The LookDev MCP tool definition is invalid.");
        }
        var name = nameElement.GetString();
        if (!IsSafeToolName(name))
            throw new SameInstanceMcpException("invalid_tool", "The LookDev MCP tool name is invalid.");
        var description = item.TryGetProperty("description", out var descriptionElement) &&
            descriptionElement.ValueKind == JsonValueKind.String
            ? descriptionElement.GetString() ?? string.Empty
            : string.Empty;
        if (description.Length > SameInstanceMcpLimits.MaximumToolDescriptionCharacters)
            throw new SameInstanceMcpException("invalid_tool", "The LookDev MCP tool description is too large.");
        if (!item.TryGetProperty("inputSchema", out var inputSchema) || inputSchema.ValueKind != JsonValueKind.Object)
            throw new SameInstanceMcpException("invalid_tool", "The LookDev MCP tool schema is invalid.");
        var readOnly = item.TryGetProperty("annotations", out var annotations) &&
            annotations.ValueKind == JsonValueKind.Object &&
            annotations.TryGetProperty("readOnlyHint", out var readOnlyHint) &&
            readOnlyHint.ValueKind == JsonValueKind.True;
        return new SameInstanceMcpTool(name!, description, inputSchema.Clone(), readOnly);
    }

    private async Task<JsonRpcResponse> SendRequestAsync(
        string method,
        Action<Utf8JsonWriter> writeParameters,
        CancellationToken cancellationToken,
        bool includeSession = true)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        timeout.CancelAfter(RequestTimeout);
        var requestId = Interlocked.Increment(ref _nextRequestId);
        var payload = new ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(payload))
        {
            writer.WriteStartObject();
            writer.WriteString("jsonrpc", "2.0");
            writer.WriteNumber("id", requestId);
            writer.WriteString("method", method);
            writer.WritePropertyName("params");
            writeParameters(writer);
            writer.WriteEndObject();
        }

        HttpResponseMessage response;
        try
        {
            response = await SendAsync(payload.WrittenMemory, includeSession, timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new SameInstanceMcpException("timeout", "The LookDev MCP request timed out.");
        }
        JsonDocument? document = null;
        try
        {
            byte[] body;
            try
            {
                body = await ReadBoundedBodyAsync(response, timeout.Token).ConfigureAwait(false);
                document = JsonDocument.Parse(body);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                throw new SameInstanceMcpException("timeout", "The LookDev MCP request timed out.");
            }
            catch (JsonException)
            {
                throw new SameInstanceMcpException(
                    "invalid_response",
                    "The LookDev MCP response JSON is invalid.");
            }
            ValidateResponse(document.RootElement, requestId);
            var headers = response.Headers
                .Concat(response.Content.Headers)
                .ToDictionary(
                    header => header.Key,
                    header => (IReadOnlyList<string>)header.Value.ToArray(),
                    StringComparer.OrdinalIgnoreCase);
            response.Dispose();
            var result = new JsonRpcResponse(headers, document);
            document = null;
            return result;
        }
        catch
        {
            document?.Dispose();
            response.Dispose();
            throw;
        }
    }

    private async Task SendNotificationAsync(string method, CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        timeout.CancelAfter(RequestTimeout);
        var payload = new ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(payload))
        {
            writer.WriteStartObject();
            writer.WriteString("jsonrpc", "2.0");
            writer.WriteString("method", method);
            writer.WriteEndObject();
        }
        HttpResponseMessage response;
        try
        {
            response = await SendAsync(payload.WrittenMemory, includeSession: true, timeout.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new SameInstanceMcpException("timeout", "The LookDev MCP request timed out.");
        }
        using (response)
            if (response.StatusCode != HttpStatusCode.Accepted)
                throw new SameInstanceMcpException("notification_failed", "The LookDev MCP initialization notification failed.");
    }

    private async Task<HttpResponseMessage> SendAsync(
        ReadOnlyMemory<byte> payload,
        bool includeSession,
        CancellationToken cancellationToken)
    {
        VerifyListenerOwnership();
        using var request = new HttpRequestMessage(HttpMethod.Post, _capability.Endpoint)
        {
            Content = new ByteArrayContent(payload.ToArray()),
        };
        request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
        request.Headers.TryAddWithoutValidation(ProtocolVersionHeader, ProtocolVersion);
        if (includeSession && _sessionId is not null)
            request.Headers.TryAddWithoutValidation(SessionIdHeader, _sessionId);

        var token = _capability.CopyBearerToken();
        try
        {
            request.Headers.TryAddWithoutValidation(
                "Authorization",
                "Bearer " + Encoding.UTF8.GetString(token));
        }
        finally
        {
            CryptographicOperations.ZeroMemory(token);
        }

        HttpResponseMessage response;
        try
        {
            response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
        }
        catch (HttpRequestException)
        {
            throw new SameInstanceMcpException("connection_failed", "The LookDev MCP connection failed.");
        }
        if (!response.IsSuccessStatusCode)
        {
            var statusCode = response.StatusCode;
            response.Dispose();
            if (includeSession && statusCode == HttpStatusCode.NotFound)
            {
                throw new SameInstanceMcpException(
                    "session_expired",
                    "The private LookDev tool session expired.");
            }
            throw new SameInstanceMcpException("http_error", "The LookDev MCP server rejected the request.");
        }
        return response;
    }

    private void VerifyListenerOwnership()
    {
        if (!_verifyProcessOwnership) return;
        if (_ownershipPlatform is null ||
            !_ownershipPlatform.IsLoopbackPortOwnedByProcess(
                _capability.Endpoint.Port,
                _expectedServerProcessId))
        {
            throw new SameInstanceMcpException(
                "connection_owner_mismatch",
                "The LookDev MCP endpoint is not owned by the native parent process.");
        }
    }

    private static async Task<byte[]> ReadBoundedBodyAsync(
        HttpResponseMessage response,
        CancellationToken cancellationToken)
    {
        var mediaType = response.Content.Headers.ContentType?.MediaType;
        if (!string.Equals(mediaType, "application/json", StringComparison.OrdinalIgnoreCase))
            throw new SameInstanceMcpException("invalid_content_type", "The LookDev MCP response type is invalid.");
        if (response.Content.Headers.ContentLength > SameInstanceMcpLimits.MaximumResponseBytes)
            throw new SameInstanceMcpException("response_too_large", "The LookDev MCP response is too large.");

        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        using var buffer = new MemoryStream();
        var chunk = ArrayPool<byte>.Shared.Rent(16 * 1024);
        try
        {
            while (true)
            {
                var read = await stream.ReadAsync(chunk.AsMemory(), cancellationToken).ConfigureAwait(false);
                if (read == 0) break;
                if (buffer.Length + read > SameInstanceMcpLimits.MaximumResponseBytes)
                    throw new SameInstanceMcpException("response_too_large", "The LookDev MCP response is too large.");
                buffer.Write(chunk, 0, read);
            }
            return buffer.ToArray();
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(chunk, clearArray: true);
        }
    }

    private static void ValidateResponse(JsonElement root, long requestId)
    {
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("jsonrpc", out var version) ||
            !string.Equals(version.GetString(), "2.0", StringComparison.Ordinal) ||
            !root.TryGetProperty("id", out var id) ||
            !id.TryGetInt64(out var responseId) ||
            responseId != requestId)
        {
            throw new SameInstanceMcpException("invalid_response", "The LookDev MCP response envelope is invalid.");
        }
        if (root.TryGetProperty("error", out _))
            throw new SameInstanceMcpException("rpc_error", "The LookDev MCP request was rejected.");
    }

    private static JsonElement GetResult(JsonElement root)
    {
        if (!root.TryGetProperty("result", out var result) || result.ValueKind != JsonValueKind.Object)
            throw new SameInstanceMcpException("invalid_response", "The LookDev MCP result is invalid.");
        return result;
    }

    private static bool IsSafeToolName(string? value) =>
        !string.IsNullOrWhiteSpace(value) &&
        value.Length <= SameInstanceMcpLimits.MaximumToolNameCharacters &&
        !value.Any(char.IsControl);

    private static bool IsLowerHexSha256(string? value) =>
        value is { Length: 64 } && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static bool IsSafeHeaderValue(string? value, int maximumCharacters) =>
        !string.IsNullOrWhiteSpace(value) &&
        value.Length <= maximumCharacters &&
        value.All(character => character is >= '!' and <= '~');

    private static SocketsHttpHandler CreateProductionHandler(int port, int expectedServerProcessId) => new()
    {
        AllowAutoRedirect = false,
        UseCookies = false,
        UseProxy = false,
        PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        ConnectCallback = async (_, cancellationToken) =>
        {
            var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
            {
                NoDelay = true,
            };
            try
            {
                await socket.ConnectAsync(IPAddress.Loopback, port, cancellationToken).ConfigureAwait(false);
                var clientPort = ((IPEndPoint?)socket.LocalEndPoint)?.Port ?? 0;
                for (var attempt = 0; attempt < 25; attempt++)
                {
                    if (SystemLlamaServerPlatform.IsLoopbackConnectionOwnedByProcess(
                        port,
                        clientPort,
                        expectedServerProcessId))
                    {
                        return new NetworkStream(socket, ownsSocket: true);
                    }
                    await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken).ConfigureAwait(false);
                }
                throw new IOException("The LookDev MCP connection owner could not be verified.");
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        },
    };

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetimeCancellation.Cancel();
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            await TryDeleteSessionAsync().ConfigureAwait(false);
            _sessionId = null;
            _tools = null;
            _usedApprovalGrantHashes.Clear();
            _httpClient.Dispose();
            _ownershipPlatform?.Dispose();
            _capability.Dispose();
        }
        finally
        {
            _gate.Release();
            _gate.Dispose();
            _lifetimeCancellation.Dispose();
        }
    }

    private async Task TryDeleteSessionAsync()
    {
        if (_sessionId is null) return;

        try
        {
            VerifyListenerOwnership();
            using var timeout = new CancellationTokenSource(SessionDeleteTimeout);
            using var request = new HttpRequestMessage(HttpMethod.Delete, _capability.Endpoint);
            request.Headers.TryAddWithoutValidation(ProtocolVersionHeader, ProtocolVersion);
            request.Headers.TryAddWithoutValidation(SessionIdHeader, _sessionId);

            var token = _capability.CopyBearerToken();
            try
            {
                request.Headers.TryAddWithoutValidation(
                    "Authorization",
                    "Bearer " + Encoding.UTF8.GetString(token));
            }
            finally
            {
                CryptographicOperations.ZeroMemory(token);
            }

            using var response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                timeout.Token).ConfigureAwait(false);
        }
        catch
        {
            // Session cleanup is best effort. Native idle expiry remains the
            // fallback and disposal must stay bounded during app shutdown.
        }
    }

    private sealed class JsonRpcResponse(
        IReadOnlyDictionary<string, IReadOnlyList<string>> headers,
        JsonDocument document) : IDisposable
    {
        public IReadOnlyDictionary<string, IReadOnlyList<string>> Headers { get; } = headers;
        public JsonDocument Document { get; } = document;

        public void Dispose() => Document.Dispose();
    }
}

public static class SameInstanceMcpArgumentHash
{
    public static string Compute(JsonElement arguments)
    {
        if (arguments.ValueKind != JsonValueKind.Object)
            throw new ArgumentException("MCP tool arguments must be a JSON object.", nameof(arguments));
        var canonical = new StringBuilder();
        AppendCanonical(arguments, canonical);
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(canonical.ToString())))
            .ToLowerInvariant();
    }

    internal static string Canonicalize(JsonElement value)
    {
        var canonical = new StringBuilder();
        AppendCanonical(value, canonical);
        return canonical.ToString();
    }

    private static void AppendCanonical(JsonElement value, StringBuilder output)
    {
        switch (value.ValueKind)
        {
            case JsonValueKind.Null:
                output.Append("null");
                return;
            case JsonValueKind.True:
                output.Append("true");
                return;
            case JsonValueKind.False:
                output.Append("false");
                return;
            case JsonValueKind.Number:
                var number = value.GetDouble();
                if (!double.IsFinite(number))
                    throw new ArgumentException("MCP tool arguments contain a non-finite number.", nameof(value));
                AppendCanonicalNumber(number, output);
                return;
            case JsonValueKind.String:
                AppendEscapedString(value.GetString() ?? string.Empty, output);
                return;
            case JsonValueKind.Array:
                output.Append('[');
                var firstItem = true;
                foreach (var item in value.EnumerateArray())
                {
                    if (!firstItem) output.Append(',');
                    firstItem = false;
                    AppendCanonical(item, output);
                }
                output.Append(']');
                return;
            case JsonValueKind.Object:
                output.Append('{');
                var properties = value.EnumerateObject().ToArray();
                Array.Sort(properties, static (left, right) => CompareUtf8(left.Name, right.Name));
                for (var index = 0; index < properties.Length; index++)
                {
                    if (index != 0) output.Append(',');
                    AppendEscapedString(properties[index].Name, output);
                    output.Append(':');
                    AppendCanonical(properties[index].Value, output);
                }
                output.Append('}');
                return;
            default:
                throw new ArgumentException("MCP tool arguments contain an unsupported JSON value.", nameof(value));
        }
    }

    private static void AppendCanonicalNumber(double number, StringBuilder output)
    {
        if (number == 0)
        {
            output.Append('0');
            return;
        }

        // Fix the representation across the MSVC and .NET formatter
        // thresholds: 17 significant digits, scientific notation, no
        // redundant mantissa zeroes, plus sign, or exponent zeroes.
        var scientific = number.ToString("E16", CultureInfo.InvariantCulture);
        var exponentIndex = scientific.IndexOf('E');
        var mantissaEnd = exponentIndex;
        while (mantissaEnd > 0 && scientific[mantissaEnd - 1] == '0')
            mantissaEnd--;
        if (mantissaEnd > 0 && scientific[mantissaEnd - 1] == '.')
            mantissaEnd--;
        var exponent = int.Parse(
            scientific.AsSpan(exponentIndex + 1),
            NumberStyles.AllowLeadingSign,
            CultureInfo.InvariantCulture);
        output.Append(scientific.AsSpan(0, mantissaEnd));
        output.Append('e');
        output.Append(exponent.ToString(CultureInfo.InvariantCulture));
    }

    private static int CompareUtf8(string left, string right)
    {
        var leftBytes = Encoding.UTF8.GetBytes(left);
        var rightBytes = Encoding.UTF8.GetBytes(right);
        return leftBytes.AsSpan().SequenceCompareTo(rightBytes);
    }

    private static void AppendEscapedString(string value, StringBuilder output)
    {
        output.Append('"');
        foreach (var character in value)
        {
            switch (character)
            {
                case '\\': output.Append("\\\\"); break;
                case '"': output.Append("\\\""); break;
                case '\b': output.Append("\\b"); break;
                case '\f': output.Append("\\f"); break;
                case '\n': output.Append("\\n"); break;
                case '\r': output.Append("\\r"); break;
                case '\t': output.Append("\\t"); break;
                default:
                    if (character < 0x20)
                        output.Append("\\u").Append(((int)character).ToString("x4", CultureInfo.InvariantCulture));
                    else
                        output.Append(character);
                    break;
            }
        }
        output.Append('"');
    }
}

public sealed class SameInstanceMcpException(
    string code,
    string message,
    Exception? innerException = null) : Exception(message, innerException)
{
    public string Code { get; } = code;
}
