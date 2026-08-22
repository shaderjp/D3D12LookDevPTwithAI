using System.Collections.Concurrent;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace D3D12LookDevPTwithAI.ChatHost.Inference;

internal sealed record LlamaServerSession(
    Uri Endpoint,
    string ApiKey,
    string Backend,
    string ModelId,
    double Temperature,
    int MaxTokens)
{
    public override string ToString() => nameof(LlamaServerSession);
}

internal interface ILlamaServerSessionProvider
{
    ValueTask<LlamaServerSession?> GetSessionAsync(
        CancellationToken cancellationToken = default);
}

internal static class VerifiedLoopbackProcessRegistry
{
    private static readonly ConcurrentDictionary<int, int> ProcessesByPort = [];

    internal static IDisposable Register(int port, int processId)
    {
        if (port is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort ||
            processId <= 0 ||
            !ProcessesByPort.TryAdd(port, processId))
        {
            throw new InvalidOperationException("The loopback process could not be registered.");
        }
        return new Registration(port, processId);
    }

    internal static bool TryGetProcessId(int port, out int processId) =>
        ProcessesByPort.TryGetValue(port, out processId);

    private sealed class Registration(int port, int processId) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            ((ICollection<KeyValuePair<int, int>>)ProcessesByPort).Remove(
                new KeyValuePair<int, int>(port, processId));
        }

        public override string ToString() => nameof(Registration);
    }
}

public sealed class LlamaServerChatInferenceRuntime : IChatInferenceRuntime, IDisposable
{
    internal const string BuiltInSystemPrompt =
        "You are the assistant built into D3D12LookDevPTwithAI. " +
        "Help with scene, material, light, camera, and rendering workflows. " +
        "Unless a tool result explicitly confirms it, never claim that you observed or changed the live scene.";

    private const string BaseRuntimeId = "llama-server";
    private const string DisplayName = "llama.cpp local server";
    private const int MaximumSseLineCharacters = 512 * 1024;
    private const int MaximumApiKeyCharacters = 4096;
    private const int MaximumModelIdCharacters = 128;
    private static readonly int MaximumBackendCharacters =
        ChatInferenceLimits.MaximumRuntimeIdentifierCharacters - BaseRuntimeId.Length - 1;
    private const int MaximumMaxTokens = 32 * 1024;
    private const int MaximumSseLines = 100_000;
    private const long MaximumSseCharacters = 4L * 1024 * 1024;
    private static readonly TimeSpan DefaultStreamIdleTimeout = TimeSpan.FromSeconds(30);

    private readonly ILlamaServerSessionProvider _sessionProvider;
    private readonly HttpClient _httpClient;
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    private readonly TimeSpan _streamIdleTimeout;
    private int _disposed;

    internal LlamaServerChatInferenceRuntime(
        ILlamaServerSessionProvider sessionProvider)
        : this(sessionProvider, CreateDefaultHandler(), disposeHandler: true)
    {
    }

    internal LlamaServerChatInferenceRuntime(
        ILlamaServerSessionProvider sessionProvider,
        HttpMessageHandler handler,
        TimeSpan? streamIdleTimeout = null)
        : this(
            sessionProvider,
            handler,
            disposeHandler: false,
            streamIdleTimeout ?? DefaultStreamIdleTimeout)
    {
    }

    private LlamaServerChatInferenceRuntime(
        ILlamaServerSessionProvider sessionProvider,
        HttpMessageHandler handler,
        bool disposeHandler,
        TimeSpan? streamIdleTimeout = null)
    {
        ArgumentNullException.ThrowIfNull(sessionProvider);
        ArgumentNullException.ThrowIfNull(handler);
        _sessionProvider = sessionProvider;
        _streamIdleTimeout = streamIdleTimeout ?? DefaultStreamIdleTimeout;
        if (_streamIdleTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(streamIdleTimeout));
        _httpClient = new HttpClient(handler, disposeHandler)
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
    }

    public async ValueTask<ChatInferenceRuntimeStatus> GetStatusAsync(
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        using var operationCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        var session = await GetSessionSafelyAsync(operationCancellation.Token)
            .ConfigureAwait(false);
        if (session is null)
        {
            return new ChatInferenceRuntimeStatus(
                BaseRuntimeId,
                DisplayName,
                IsReady: false,
                State: "not_ready");
        }

        ValidateSession(session);
        return new ChatInferenceRuntimeStatus(
            CreateRuntimeId(session.Backend),
            DisplayName,
            IsReady: true,
            State: "ready");
    }

    public async IAsyncEnumerable<ChatInferenceChunk> StreamAsync(
        ChatInferenceRequest request,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        ValidateRequest(request);

        using var operationCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeCancellation.Token);
        var operationToken = operationCancellation.Token;

        var session = await GetSessionSafelyAsync(operationToken).ConfigureAwait(false);
        if (session is null)
            throw SafeFailure(
                "inference_runtime_not_ready",
                "The local inference runtime is not ready.",
                retryable: true);
        ValidateSession(session);

        using var message = new HttpRequestMessage(
            HttpMethod.Post,
            new Uri(session.Endpoint, "/v1/chat/completions"))
        {
            Content = JsonContent.Create(CreateRequestBody(request, session)),
        };
        message.Headers.Authorization = new AuthenticationHeaderValue(
            "Bearer",
            session.ApiKey);

        using var response = await SendAsync(message, operationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            throw SafeFailure(
                "inference_http_error",
                "The local inference server rejected the request.",
                IsRetryable(response.StatusCode));
        }
        var mediaType = response.Content.Headers.ContentType?.MediaType;
        if (!string.Equals(mediaType, "text/event-stream", StringComparison.OrdinalIgnoreCase))
        {
            throw SafeFailure(
                "inference_protocol_error",
                "The local inference server returned an invalid stream response.");
        }

        await using var responseStream = await ReadResponseStreamAsync(
            response,
            operationToken).ConfigureAwait(false);
        using var reader = new StreamReader(
            responseStream,
            Encoding.UTF8,
            detectEncodingFromByteOrderMarks: false,
            leaveOpen: false);
        var lineReader = new BoundedLineReader(reader, MaximumSseLineCharacters);

        var receivedDone = false;
        var outputCharacters = 0L;
        var sseCharacters = 0L;
        var sseLines = 0;
        while (true)
        {
            using var idleCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                operationToken);
            idleCancellation.CancelAfter(_streamIdleTimeout);
            string? line;
            try
            {
                line = await lineReader.ReadLineAsync(idleCancellation.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!operationToken.IsCancellationRequested)
            {
                throw SafeFailure(
                    "inference_stream_timeout",
                    "The local inference stream stopped responding.",
                    retryable: true);
            }
            if (line is null) break;
            operationToken.ThrowIfCancellationRequested();
            sseCharacters += line.Length;
            if (++sseLines > MaximumSseLines || sseCharacters > MaximumSseCharacters)
            {
                throw SafeFailure(
                    "inference_protocol_error",
                    "The local inference stream exceeded the supported size.");
            }
            if (!line.StartsWith("data:", StringComparison.Ordinal))
                continue;

            var payload = line[5..].TrimStart();
            if (payload.Length == 0)
                continue;
            if (string.Equals(payload, "[DONE]", StringComparison.Ordinal))
            {
                receivedDone = true;
                break;
            }

            foreach (var delta in ParseTextDeltas(payload))
            {
                outputCharacters += delta.Length;
                if (outputCharacters > ChatInferenceLimits.MaximumOutputCharacters)
                {
                    throw SafeFailure(
                        "inference_output_too_large",
                        "The local inference response exceeded the supported size.");
                }
                yield return new ChatInferenceChunk(delta);
            }
        }

        if (!receivedDone)
        {
            throw SafeFailure(
                "inference_stream_truncated",
                "The local inference stream ended before completion.",
                retryable: true);
        }
    }

    private async ValueTask<LlamaServerSession?> GetSessionSafelyAsync(
        CancellationToken cancellationToken)
    {
        try
        {
            return await _sessionProvider.GetSessionAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (ChatInferenceException exception) when (IsSafeSessionFailure(exception.Code))
        {
            throw;
        }
        catch (Exception)
        {
            throw SafeFailure(
                "inference_session_failed",
                "The local inference session is unavailable.",
                retryable: true);
        }
    }

    private async Task<HttpResponseMessage> SendAsync(
        HttpRequestMessage request,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is HttpRequestException or IOException or ObjectDisposedException)
        {
            throw SafeFailure(
                "inference_transport_failed",
                "The local inference server could not be reached.",
                retryable: true);
        }
    }

    private static async Task<Stream> ReadResponseStreamAsync(
        HttpResponseMessage response,
        CancellationToken cancellationToken)
    {
        try
        {
            return await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (
            exception is HttpRequestException or IOException or ObjectDisposedException)
        {
            throw SafeFailure(
                "inference_transport_failed",
                "The local inference stream could not be opened.",
                retryable: true);
        }
    }

    private static IReadOnlyList<string> ParseTextDeltas(string payload)
    {
        try
        {
            using var document = JsonDocument.Parse(
                payload,
                new JsonDocumentOptions { MaxDepth = 64 });
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new JsonException();
            if (!root.TryGetProperty("choices", out var choices))
                return Array.Empty<string>();
            if (choices.ValueKind != JsonValueKind.Array)
                throw new JsonException();

            var deltas = new List<string>();
            foreach (var choice in choices.EnumerateArray())
            {
                if (choice.ValueKind != JsonValueKind.Object ||
                    !choice.TryGetProperty("delta", out var delta) ||
                    delta.ValueKind != JsonValueKind.Object ||
                    !delta.TryGetProperty("content", out var content) ||
                    content.ValueKind == JsonValueKind.Null)
                {
                    continue;
                }
                if (content.ValueKind != JsonValueKind.String)
                    throw new JsonException();
                var text = content.GetString();
                if (!string.IsNullOrEmpty(text))
                    deltas.Add(text);
            }
            return deltas;
        }
        catch (JsonException)
        {
            throw SafeFailure(
                "inference_protocol_error",
                "The local inference server returned an invalid stream payload.");
        }
    }

    private static JsonObject CreateRequestBody(
        ChatInferenceRequest request,
        LlamaServerSession session)
    {
        var messages = new JsonArray();
        messages.Add(new JsonObject
        {
            ["role"] = "system",
            ["content"] = BuiltInSystemPrompt,
        });
        foreach (var historyMessage in request.History)
        {
            var message = new JsonObject
            {
                ["role"] = ToWireRole(historyMessage.Role),
                ["content"] = historyMessage.Content,
            };
            if (!string.IsNullOrWhiteSpace(historyMessage.Name))
                message["name"] = historyMessage.Name;
            messages.Add(message);
        }
        messages.Add(new JsonObject
        {
            ["role"] = "user",
            ["content"] = request.UserText,
        });

        return new JsonObject
        {
            ["model"] = session.ModelId,
            ["messages"] = messages,
            ["stream"] = true,
            ["stream_options"] = new JsonObject { ["include_usage"] = true },
            ["temperature"] = session.Temperature,
            ["max_tokens"] = session.MaxTokens,
            ["cache_prompt"] = true,
            ["reasoning_effort"] = "none",
            ["chat_template_kwargs"] = new JsonObject { ["enable_thinking"] = false },
        };
    }

    private static string ToWireRole(ChatInferenceRole role) => role switch
    {
        ChatInferenceRole.System => "system",
        ChatInferenceRole.User => "user",
        ChatInferenceRole.Assistant => "assistant",
        ChatInferenceRole.Tool => "tool",
        _ => throw SafeFailure(
            "invalid_inference_request",
            "The local inference request is invalid."),
    };

    private static void ValidateRequest(ChatInferenceRequest request)
    {
        if (request is null)
            throw SafeFailure(
                "invalid_inference_request",
                "The local inference request is invalid.");
        if (request.ConversationId == Guid.Empty ||
            string.IsNullOrWhiteSpace(request.ContextKey) ||
            request.ContextKey.Length > ChatInferenceLimits.MaximumContextKeyCharacters ||
            string.IsNullOrWhiteSpace(request.UserText) ||
            request.UserText.Length > ChatInferenceLimits.MaximumInputCharacters ||
            request.History is null ||
            request.History.Count > ChatInferenceLimits.MaximumHistoryMessages)
        {
            throw SafeFailure(
                "invalid_inference_request",
                "The local inference request is invalid.");
        }

        var historyCharacters = 0L;
        foreach (var message in request.History)
        {
            if (message is null ||
                string.IsNullOrEmpty(message.Content) ||
                message.Content.Length > ChatInferenceLimits.MaximumHistoryMessageCharacters ||
                message.Name is { Length: > ChatInferenceLimits.MaximumNameCharacters } ||
                message.Role == ChatInferenceRole.Tool && string.IsNullOrWhiteSpace(message.Name))
            {
                throw SafeFailure(
                    "invalid_inference_request",
                    "The local inference request is invalid.");
            }

            historyCharacters += message.Content.Length;
            if (historyCharacters > ChatInferenceLimits.MaximumHistoryCharacters)
            {
                throw SafeFailure(
                    "invalid_inference_request",
                    "The local inference request is invalid.");
            }
        }
    }

    private static void ValidateSession(LlamaServerSession session)
    {
        if (session.Endpoint is null ||
            !session.Endpoint.IsAbsoluteUri ||
            !string.Equals(session.Endpoint.Scheme, Uri.UriSchemeHttp, StringComparison.OrdinalIgnoreCase) ||
            !session.Endpoint.IsLoopback ||
            !string.IsNullOrEmpty(session.Endpoint.UserInfo) ||
            !string.IsNullOrEmpty(session.Endpoint.Query) ||
            !string.IsNullOrEmpty(session.Endpoint.Fragment) ||
            string.IsNullOrWhiteSpace(session.ApiKey) ||
            session.ApiKey.Length > MaximumApiKeyCharacters ||
            !IsSafeBearerToken(session.ApiKey) ||
            !IsSafeToken(session.Backend, MaximumBackendCharacters) ||
            string.IsNullOrWhiteSpace(session.ModelId) ||
            session.ModelId.Length > MaximumModelIdCharacters ||
            session.ModelId.Any(char.IsControl) ||
            !double.IsFinite(session.Temperature) ||
            session.Temperature is < 0 or > 2 ||
            session.MaxTokens is <= 0 or > MaximumMaxTokens)
        {
            throw SafeFailure(
                "invalid_inference_session",
                "The local inference session is invalid.");
        }
    }

    private static string CreateRuntimeId(string backend) =>
        BaseRuntimeId + "-" + backend.ToLowerInvariant();

    private static bool IsSafeToken(string? value, int maximumCharacters)
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

    private static bool IsSafeBearerToken(string value)
    {
        var paddingStarted = false;
        foreach (var character in value)
        {
            if (character == '=')
            {
                paddingStarted = true;
                continue;
            }
            if (paddingStarted ||
                !((character is >= 'a' and <= 'z') ||
                  (character is >= 'A' and <= 'Z') ||
                  (character is >= '0' and <= '9') ||
                  character is '-' or '.' or '_' or '~' or '+' or '/'))
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsRetryable(HttpStatusCode statusCode) =>
        statusCode == HttpStatusCode.RequestTimeout ||
        (int)statusCode == 429 ||
        (int)statusCode >= 500;

    private static bool IsSafeSessionFailure(string code) => code is
        "local_runtime_configuration_invalid" or
        "local_runtime_configuration_unavailable" or
        "local_runtime_artifact_invalid" or
        "local_runtime_start_failed" or
        "local_runtime_start_timeout";

    internal static HttpMessageHandler CreateDefaultHandler()
    {
        return new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            UseProxy = false,
            UseCookies = false,
            ConnectCallback = ConnectVerifiedLoopbackAsync,
        };
    }

    private static async ValueTask<Stream> ConnectVerifiedLoopbackAsync(
        SocketsHttpConnectionContext context,
        CancellationToken cancellationToken)
    {
        var port = context.DnsEndPoint.Port;
        if (!VerifiedLoopbackProcessRegistry.TryGetProcessId(port, out var processId))
            throw new HttpRequestException("The loopback inference endpoint is not registered.");

        var socket = new Socket(
            AddressFamily.InterNetwork,
            SocketType.Stream,
            ProtocolType.Tcp)
        {
            NoDelay = true,
        };
        try
        {
            await socket.ConnectAsync(
                new IPEndPoint(IPAddress.Loopback, port),
                cancellationToken).ConfigureAwait(false);
            var clientEndpoint = socket.LocalEndPoint as IPEndPoint;
            if (clientEndpoint is null) throw new HttpRequestException();

            var verified = false;
            for (var attempt = 0; attempt < 25 && !verified; ++attempt)
            {
                verified = SystemLlamaServerPlatform.IsLoopbackConnectionOwnedByProcess(
                    port,
                    clientEndpoint.Port,
                    processId);
                if (!verified)
                {
                    await Task.Delay(
                        TimeSpan.FromMilliseconds(10),
                        cancellationToken).ConfigureAwait(false);
                }
            }
            if (!verified)
                throw new HttpRequestException("The loopback inference peer is not trusted.");
            return new NetworkStream(socket, ownsSocket: true);
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private static ChatInferenceException SafeFailure(
        string code,
        string message,
        bool retryable = false) => new(code, message, retryable);

    private void ThrowIfDisposed() =>
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _disposed) != 0,
            this);

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            _lifetimeCancellation.Cancel();
            _httpClient.Dispose();
            _lifetimeCancellation.Dispose();
        }
    }

    private sealed class BoundedLineReader(
        StreamReader reader,
        int maximumLineCharacters)
    {
        private readonly char[] _buffer = new char[4096];
        private int _bufferLength;
        private int _bufferOffset;
        private bool _consumeLineFeed;

        public async ValueTask<string?> ReadLineAsync(
            CancellationToken cancellationToken)
        {
            StringBuilder? line = null;
            var lineCharacters = 0;
            while (true)
            {
                if (_bufferOffset == _bufferLength)
                {
                    _bufferLength = await ReadIntoBufferAsync(cancellationToken).ConfigureAwait(false);
                    _bufferOffset = 0;
                    if (_bufferLength == 0)
                        return line?.ToString();
                }

                if (_consumeLineFeed)
                {
                    _consumeLineFeed = false;
                    if (_buffer[_bufferOffset] == '\n')
                    {
                        _bufferOffset++;
                        if (_bufferOffset == _bufferLength)
                            continue;
                    }
                }

                var segmentStart = _bufferOffset;
                while (_bufferOffset < _bufferLength &&
                       _buffer[_bufferOffset] is not ('\r' or '\n'))
                {
                    _bufferOffset++;
                }

                var segmentLength = _bufferOffset - segmentStart;
                lineCharacters += segmentLength;
                if (lineCharacters > maximumLineCharacters)
                {
                    throw SafeFailure(
                        "inference_protocol_error",
                        "The local inference stream line exceeded the supported size.");
                }
                if (segmentLength > 0)
                {
                    line ??= new StringBuilder(Math.Min(maximumLineCharacters, 4096));
                    line.Append(_buffer, segmentStart, segmentLength);
                }

                if (_bufferOffset == _bufferLength)
                    continue;

                var terminator = _buffer[_bufferOffset++];
                if (terminator == '\r')
                {
                    if (_bufferOffset < _bufferLength)
                    {
                        if (_buffer[_bufferOffset] == '\n')
                            _bufferOffset++;
                    }
                    else
                    {
                        _consumeLineFeed = true;
                    }
                }
                return line?.ToString() ?? string.Empty;
            }
        }

        private async ValueTask<int> ReadIntoBufferAsync(
            CancellationToken cancellationToken)
        {
            try
            {
                return await reader.ReadAsync(
                    _buffer.AsMemory(),
                    cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception) when (
                exception is IOException or ObjectDisposedException or DecoderFallbackException)
            {
                throw SafeFailure(
                    "inference_stream_failed",
                    "The local inference stream failed.",
                    retryable: true);
            }
        }
    }
}
