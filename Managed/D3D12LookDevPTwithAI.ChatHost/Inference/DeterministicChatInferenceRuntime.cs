using System.Runtime.CompilerServices;

namespace D3D12LookDevPTwithAI.ChatHost.Inference;

public sealed class DeterministicChatInferenceRuntime : IChatInferenceRuntime
{
    public const string RuntimeId = "deterministic-placeholder";
    private const int DefaultChunkCharacters = 12;
    private static readonly TimeSpan DefaultChunkDelay = TimeSpan.FromMilliseconds(15);
    private readonly int _chunkCharacters;
    private readonly TimeSpan _chunkDelay;

    public DeterministicChatInferenceRuntime(
        int chunkCharacters = DefaultChunkCharacters,
        TimeSpan? chunkDelay = null)
    {
        if (chunkCharacters <= 0 || chunkCharacters > 4096)
            throw new ArgumentOutOfRangeException(nameof(chunkCharacters));
        if (chunkDelay is { } delay && delay < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(chunkDelay));

        _chunkCharacters = chunkCharacters;
        _chunkDelay = chunkDelay ?? DefaultChunkDelay;
    }

    public ValueTask<ChatInferenceRuntimeStatus> GetStatusAsync(
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ChatInferenceRuntimeStatus(
            RuntimeId,
            "Deterministic placeholder",
            IsReady: true,
            State: "ready",
            ModelId: RuntimeId,
            ModelDisplayName: "Deterministic placeholder"));
    }

    public async IAsyncEnumerable<ChatInferenceChunk> StreamAsync(
        ChatInferenceRequest request,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        Validate(request);
        var response = $"[Local ChatHost placeholder] {request.UserText.Trim()}";

        for (var offset = 0; offset < response.Length; offset += _chunkCharacters)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var length = Math.Min(_chunkCharacters, response.Length - offset);
            yield return new ChatInferenceChunk(response.Substring(offset, length));

            if (offset + length < response.Length && _chunkDelay > TimeSpan.Zero)
                await Task.Delay(_chunkDelay, cancellationToken).ConfigureAwait(false);
        }
    }

    private static void Validate(ChatInferenceRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.ConversationId == Guid.Empty)
            throw Invalid("conversationId is required.");
        if (string.IsNullOrWhiteSpace(request.ContextKey) ||
            request.ContextKey.Length > ChatInferenceLimits.MaximumContextKeyCharacters)
        {
            throw Invalid(
                $"contextKey is required and must not exceed {ChatInferenceLimits.MaximumContextKeyCharacters} characters.");
        }
        if (string.IsNullOrWhiteSpace(request.UserText) ||
            request.UserText.Length > ChatInferenceLimits.MaximumInputCharacters)
        {
            throw Invalid(
                $"userText is required and must not exceed {ChatInferenceLimits.MaximumInputCharacters} characters.");
        }
        if (request.History is null)
            throw Invalid("history is required.");
        if (request.History.Count > ChatInferenceLimits.MaximumHistoryMessages)
        {
            throw Invalid(
                $"history must not exceed {ChatInferenceLimits.MaximumHistoryMessages} messages.");
        }

        var historyCharacters = 0L;
        foreach (var message in request.History)
        {
            if (message is null || string.IsNullOrEmpty(message.Content) ||
                message.Content.Length > ChatInferenceLimits.MaximumHistoryMessageCharacters)
            {
                throw Invalid(
                    $"Each history message requires content no longer than {ChatInferenceLimits.MaximumHistoryMessageCharacters} characters.");
            }
            if (message.Name is { Length: > ChatInferenceLimits.MaximumNameCharacters })
            {
                throw Invalid(
                    $"History message names must not exceed {ChatInferenceLimits.MaximumNameCharacters} characters.");
            }
            if (message.Role == ChatInferenceRole.Tool &&
                string.IsNullOrWhiteSpace(message.Name))
            {
                throw Invalid("Tool history messages require a name.");
            }
            historyCharacters += message.Content.Length;
            if (historyCharacters > ChatInferenceLimits.MaximumHistoryCharacters)
            {
                throw Invalid(
                    $"history must not exceed {ChatInferenceLimits.MaximumHistoryCharacters} characters in total.");
            }
        }
    }

    private static ChatInferenceException Invalid(string message) =>
        new("invalid_inference_request", message);
}
