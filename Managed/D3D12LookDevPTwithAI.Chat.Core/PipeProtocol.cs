using System.Buffers.Binary;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace D3D12LookDevPTwithAI.Chat.Core;

public enum PipeMessageKind
{
    Request,
    Response,
    Event,
}

public sealed record PipeError(
    string Code,
    string Message,
    bool Retryable = false);

public sealed record PipeEnvelope
{
    public int ProtocolVersion { get; init; } = PipeProtocol.Version;
    public PipeMessageKind Kind { get; init; }
    public Guid RequestId { get; init; }
    public long Sequence { get; init; }
    public string Method { get; init; } = string.Empty;
    public JsonElement Payload { get; init; } = PipeJson.EmptyObject;
    public PipeError? Error { get; init; }
}

public static class PipeProtocol
{
    public const int Version = 1;
    public const int MaximumFrameBytes = 4 * 1024 * 1024;
    public const int ConversationSelectEnvelopeReserveBytes = 16 * 1024;
    public const int MaximumConversationSelectPayloadBytes =
        MaximumFrameBytes - ConversationSelectEnvelopeReserveBytes;
    public const int DefaultConversationPageSize = 100;
    public const int MaximumConversationPageSize = 200;
    public const long MaximumExactJsonInteger = (1L << 53) - 1;
    public const int MaximumConversationTitleCharacters = 200;
    public const int MaximumConversationMessageCharacters = 128 * 1024;
}

public static class PipeJson
{
    public static JsonSerializerOptions SerializerOptions { get; } = CreateOptions();
    public static JsonElement EmptyObject { get; } = JsonSerializer.SerializeToElement(
        new Dictionary<string, object?>(), SerializerOptions);

    public static JsonElement ToElement<T>(T value) =>
        JsonSerializer.SerializeToElement(value, SerializerOptions);

    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web)
        {
            MaxDepth = 64,
            PropertyNameCaseInsensitive = false,
        };
        options.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return options;
    }
}

public sealed class PipeProtocolException(string message, Exception? innerException = null)
    : IOException(message, innerException);

public static class PipeFraming
{
    public static async ValueTask<PipeEnvelope?> ReadAsync(
        Stream stream,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        var prefix = new byte[sizeof(uint)];
        var firstRead = await stream.ReadAsync(prefix.AsMemory(0, 1), cancellationToken).ConfigureAwait(false);
        if (firstRead == 0) return null;
        await ReadExactlyAsync(stream, prefix.AsMemory(1), cancellationToken).ConfigureAwait(false);

        var length = BinaryPrimitives.ReadUInt32LittleEndian(prefix);
        if (length == 0 || length > PipeProtocol.MaximumFrameBytes)
            throw new PipeProtocolException($"Invalid pipe frame length: {length}.");

        var payload = new byte[(int)length];
        await ReadExactlyAsync(stream, payload, cancellationToken).ConfigureAwait(false);

        PipeEnvelope? envelope;
        try
        {
            envelope = JsonSerializer.Deserialize<PipeEnvelope>(payload, PipeJson.SerializerOptions);
        }
        catch (JsonException exception)
        {
            throw new PipeProtocolException("Pipe frame contains invalid JSON.", exception);
        }

        Validate(envelope);
        return envelope;
    }

    public static async ValueTask WriteAsync(
        Stream stream,
        PipeEnvelope envelope,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        Validate(envelope);

        var payload = JsonSerializer.SerializeToUtf8Bytes(envelope, PipeJson.SerializerOptions);
        if (payload.Length == 0 || payload.Length > PipeProtocol.MaximumFrameBytes)
            throw new PipeProtocolException($"Serialized pipe frame is {payload.Length} bytes; the limit is {PipeProtocol.MaximumFrameBytes}.");

        var prefix = new byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(prefix, (uint)payload.Length);
        await stream.WriteAsync(prefix, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    private static void Validate(PipeEnvelope? envelope)
    {
        if (envelope is null) throw new PipeProtocolException("Pipe frame deserialized to null.");
        if (envelope.ProtocolVersion != PipeProtocol.Version)
            throw new PipeProtocolException($"Unsupported pipe protocol version: {envelope.ProtocolVersion}.");
        if (envelope.RequestId == Guid.Empty) throw new PipeProtocolException("requestId is required.");
        if (envelope.Sequence <= 0) throw new PipeProtocolException("sequence must be positive.");
        if (string.IsNullOrWhiteSpace(envelope.Method)) throw new PipeProtocolException("method is required.");
        if (envelope.Payload.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null)
            throw new PipeProtocolException("payload must be present.");
        if (envelope.Kind != PipeMessageKind.Response && envelope.Error is not null)
            throw new PipeProtocolException("Only response frames may contain an error.");
    }

    private static async ValueTask ReadExactlyAsync(
        Stream stream,
        Memory<byte> destination,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < destination.Length)
        {
            var count = await stream.ReadAsync(destination[offset..], cancellationToken).ConfigureAwait(false);
            if (count == 0) throw new EndOfStreamException("The named pipe closed in the middle of a frame.");
            offset += count;
        }
    }
}

public sealed record InitializeRequest(
    string InstanceId,
    string ProjectContextKey,
    string? McpEndpoint = null,
    string? McpBearerToken = null);
public sealed record InitializeResult(
    string HostVersion,
    Guid ActiveConversationId,
    IReadOnlyList<ConversationSummary> Conversations);

public sealed record ModelSetupStartRequest(
    string ModelId,
    string Backend,
    bool LicenseAccepted);

public sealed record ModelSetupStartResult(bool Accepted);

public sealed record ModelSetupCancelResult(bool CancelRequested);

public sealed record ModelSetupProgressEvent(
    string Stage,
    string Artifact,
    long BytesReceived,
    long TotalBytes,
    long OverallBytesReceived,
    long OverallTotalBytes,
    double Percent,
    string Message,
    bool Terminal = false,
    bool Succeeded = false);

public sealed record ConversationSummary(
    Guid Id,
    string Title,
    DateTimeOffset CreatedAt,
    DateTimeOffset UpdatedAt);

public sealed record ConversationMessage(
    Guid Id,
    Guid ConversationId,
    string Role,
    string Content,
    DateTimeOffset CreatedAt,
    bool IsError = false);

public sealed record ConversationListResult(
    Guid? ActiveConversationId,
    IReadOnlyList<ConversationSummary> Conversations);

public sealed record ConversationCreateRequest(string? Title = null);
public sealed record ConversationCreateResult(ConversationSummary Conversation);
public sealed record ConversationSelectRequest(
    Guid ConversationId,
    long? BeforeMessageSequence = null,
    int? PageSize = null);
public sealed record ConversationSelectResult(
    ConversationSummary Conversation,
    IReadOnlyList<ConversationMessage> Messages,
    long? OlderBeforeMessageSequence = null,
    bool HasMoreMessages = false);

public sealed record SendTurnRequest(Guid TurnId, Guid ConversationId, string Text);
public sealed record SendTurnResult(Guid TurnId, bool Accepted);
public sealed record CancelTurnRequest(Guid TurnId);
public sealed record CancelTurnResult(Guid TurnId, bool CancelRequested);

public sealed record ApprovalRespondRequest(
    Guid ApprovalId,
    string Decision,
    string? ApprovalGrant = null);

public sealed record ApprovalRespondResult(Guid ApprovalId, bool Accepted);
public sealed record ApprovalResolution(bool Allowed, string? ApprovalGrant);

public sealed record RuntimeStateEvent(
    string Status,
    string Backend = "placeholder",
    string ModelId = "",
    string ModelDisplayName = "");
public sealed record MessageAddedEvent(Guid TurnId, ConversationMessage Message);
public sealed record TextDeltaEvent(Guid TurnId, Guid MessageId, string Delta);
public sealed record ToolApprovalRequiredEvent(
    Guid ApprovalId,
    Guid TurnId,
    string ToolCallId,
    string Tool,
    string Summary,
    string McpSessionId,
    string ArgumentsHash,
    string ArgumentsJson);
public sealed record ToolStartedEvent(Guid TurnId, string ToolCallId, string Tool);
public sealed record ToolCompletedEvent(
    Guid TurnId,
    string ToolCallId,
    string Tool,
    string Status,
    bool IsError,
    string? Code = null);
public sealed record TurnCompletedEvent(Guid TurnId, string Status);
public sealed record ErrorEvent(Guid? TurnId, string Code, string Message);
