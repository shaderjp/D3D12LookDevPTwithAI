namespace D3D12LookDevPTwithAI.Chat.Core;

public interface IAppPaths
{
    string DataDirectory { get; }
    string DatabasePath { get; }
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

    Task AppendMessageAsync(
        string projectContextKey,
        ConversationMessage message,
        CancellationToken cancellationToken = default);
}
