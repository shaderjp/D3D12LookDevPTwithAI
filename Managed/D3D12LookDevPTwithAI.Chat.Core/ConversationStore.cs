namespace D3D12LookDevPTwithAI.Chat.Core;

public interface IAppPaths
{
    string DataDirectory { get; }
    string DatabasePath { get; }
    string ModelsDirectory { get; }
    string RuntimesDirectory { get; }
    string InferenceSettingsPath { get; }
    void EnsureCreated();
}

public interface IConversationStore
{
    Task InitializeAsync(CancellationToken cancellationToken = default);

    Task<IReadOnlyList<ConversationSummary>> ListAsync(
        string projectContextKey,
        CancellationToken cancellationToken = default);

    Task<ConversationSummary?> GetAsync(
        string projectContextKey,
        Guid conversationId,
        CancellationToken cancellationToken = default);

    Task<ConversationSummary> CreateAsync(
        string projectContextKey,
        string title,
        CancellationToken cancellationToken = default);

    Task<IReadOnlyList<ConversationMessage>> GetMessagesAsync(
        string projectContextKey,
        Guid conversationId,
        CancellationToken cancellationToken = default);

    // Returns messages newest-first so callers can build a latest-history page
    // without loading the complete conversation. beforeMessageSequence is an
    // exclusive, stable database cursor.
    Task<IReadOnlyList<SequencedConversationMessage>> ListMessagesBeforeAsync(
        string projectContextKey,
        Guid conversationId,
        long? beforeMessageSequence,
        int maximumMessages,
        CancellationToken cancellationToken = default);

    Task AppendMessageAsync(
        string projectContextKey,
        ConversationMessage message,
        CancellationToken cancellationToken = default);
}

public sealed record SequencedConversationMessage(
    long Sequence,
    ConversationMessage Message);
