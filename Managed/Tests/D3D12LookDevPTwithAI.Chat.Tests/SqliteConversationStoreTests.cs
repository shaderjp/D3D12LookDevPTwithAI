using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class SqliteConversationStoreTests
{
    [Fact]
    public async Task Conversations_and_messages_never_cross_project_contexts()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var projectAConversation = await store.CreateAsync("project-a", "A");
        var projectBConversation = await store.CreateAsync("project-b", "B");
        await store.AppendMessageAsync(
            "project-a",
            Message(projectAConversation.Id, "only-a", DateTimeOffset.UtcNow));

        var projectA = await store.ListAsync("project-a");
        var projectB = await store.ListAsync("project-b");

        Assert.Single(projectA);
        Assert.Equal(projectAConversation.Id, projectA[0].Id);
        Assert.Single(projectB);
        Assert.Equal(projectBConversation.Id, projectB[0].Id);
        Assert.Null(await store.GetAsync("project-b", projectAConversation.Id));
        Assert.Empty(await store.GetMessagesAsync("project-b", projectAConversation.Id));
        Assert.Equal("only-a", Assert.Single(await store.GetMessagesAsync("project-a", projectAConversation.Id)).Content);
    }

    [Fact]
    public async Task Conversations_and_messages_survive_store_reopen()
    {
        await using var database = new TemporaryDatabase();
        var firstStore = database.CreateStore();
        var conversation = await firstStore.CreateAsync("lookdev-project", "Material pass");
        var message = Message(conversation.Id, "persisted", DateTimeOffset.UtcNow);
        await firstStore.AppendMessageAsync("lookdev-project", message);

        var reopenedStore = database.CreateStore();
        var reopenedConversation = Assert.Single(await reopenedStore.ListAsync("lookdev-project"));
        var reopenedMessage = Assert.Single(await reopenedStore.GetMessagesAsync("lookdev-project", conversation.Id));

        Assert.Equal(conversation.Id, reopenedConversation.Id);
        Assert.Equal("Material pass", reopenedConversation.Title);
        Assert.Equal(message, reopenedMessage);
    }

    [Fact]
    public async Task Messages_with_equal_timestamps_keep_append_order()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var conversation = await store.CreateAsync("lookdev-project", "Ordering");
        var timestamp = DateTimeOffset.UtcNow;

        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "first", timestamp));
        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "second", timestamp));
        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "third", timestamp));

        var messages = await store.GetMessagesAsync("lookdev-project", conversation.Id);
        Assert.Equal(["first", "second", "third"], messages.Select(message => message.Content));
    }

    [Fact]
    public async Task Multiple_store_instances_can_initialize_the_same_database_concurrently()
    {
        await using var database = new TemporaryDatabase();
        var stores = Enumerable.Range(0, 8).Select(_ => database.CreateStore()).ToList();

        await Task.WhenAll(stores.Select(store => store.InitializeAsync()));

        var conversation = await stores[0].CreateAsync("lookdev-project", "Concurrent init");
        Assert.Equal(conversation.Id, Assert.Single(await stores[^1].ListAsync("lookdev-project")).Id);
    }

    [Fact]
    public async Task Concurrent_message_writes_are_serialized_without_losing_history()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var conversation = await store.CreateAsync("lookdev-project", "Concurrent writes");
        var timestamp = DateTimeOffset.UtcNow;
        var messages = Enumerable.Range(0, 32)
            .Select(index => Message(conversation.Id, index.ToString(), timestamp))
            .ToList();

        await Task.WhenAll(messages.Select(message =>
            store.AppendMessageAsync("lookdev-project", message)));

        var persisted = await store.GetMessagesAsync("lookdev-project", conversation.Id);
        Assert.Equal(messages.Count, persisted.Count);
        Assert.Equal(
            messages.Select(message => message.Id).Order(),
            persisted.Select(message => message.Id).Order());
    }

    private static ConversationMessage Message(
        Guid conversationId,
        string content,
        DateTimeOffset timestamp) =>
        new(Guid.NewGuid(), conversationId, "user", content, timestamp);

    private sealed class TemporaryDatabase : IAsyncDisposable
    {
        private readonly string _dataDirectory = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI.Chat.Tests",
            Guid.NewGuid().ToString("N"));

        public SqliteConversationStore CreateStore() =>
            new(new AppPaths(_dataDirectory));

        public ValueTask DisposeAsync()
        {
            if (Directory.Exists(_dataDirectory)) Directory.Delete(_dataDirectory, recursive: true);
            return ValueTask.CompletedTask;
        }
    }
}
