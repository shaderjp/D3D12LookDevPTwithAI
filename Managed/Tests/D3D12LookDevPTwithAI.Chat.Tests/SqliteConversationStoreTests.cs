using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;
using Microsoft.Data.Sqlite;

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
    public async Task Message_order_is_append_sequence_even_when_the_clock_moves_backwards()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var conversation = await store.CreateAsync("lookdev-project", "Ordering");
        var timestamp = DateTimeOffset.UtcNow;

        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "first", timestamp));
        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "second", timestamp.AddHours(-1)));
        await store.AppendMessageAsync("lookdev-project", Message(conversation.Id, "third", timestamp.AddHours(-2)));

        var messages = await store.GetMessagesAsync("lookdev-project", conversation.Id);
        Assert.Equal(["first", "second", "third"], messages.Select(message => message.Content));
    }

    [Fact]
    public async Task Message_pages_use_an_exclusive_sequence_cursor_and_keep_project_isolation()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var projectA = await store.CreateAsync("project-a", "A");
        var projectB = await store.CreateAsync("project-b", "B");
        var timestamp = DateTimeOffset.UtcNow;
        foreach (var content in new[] { "a-1", "a-2", "a-3", "a-4", "a-5" })
            await store.AppendMessageAsync("project-a", Message(projectA.Id, content, timestamp));
        await store.AppendMessageAsync("project-b", Message(projectB.Id, "only-b", timestamp));

        var latest = await store.ListMessagesBeforeAsync("project-a", projectA.Id, null, 3);
        Assert.Equal(["a-5", "a-4", "a-3"], latest.Select(item => item.Message.Content));
        var older = await store.ListMessagesBeforeAsync(
            "project-a",
            projectA.Id,
            latest[^1].Sequence,
            3);
        Assert.Equal(["a-2", "a-1"], older.Select(item => item.Message.Content));
        Assert.Empty(await store.ListMessagesBeforeAsync("project-b", projectA.Id, null, 3));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            store.ListMessagesBeforeAsync(
                "project-a",
                projectA.Id,
                PipeProtocol.MaximumExactJsonInteger + 1,
                3));
    }

    [Fact]
    public async Task Version_one_database_migrates_to_sequence_index_version_two()
    {
        await using var database = new TemporaryDatabase();
        await database.CreateVersionOneSchemaAsync();

        var store = database.CreateStore();
        await store.InitializeAsync();
        Assert.Equal(
            ["legacy-first", "legacy-second"],
            (await store.GetMessagesAsync(
                "legacy-project",
                database.VersionOneConversationId)).Select(message => message.Content));

        await using var connection = new SqliteConnection(
            $"Data Source={database.DatabasePath};Pooling=False");
        await connection.OpenAsync();
        var versionCommand = connection.CreateCommand();
        versionCommand.CommandText = "PRAGMA user_version;";
        Assert.Equal(2L, (long)(await versionCommand.ExecuteScalarAsync())!);

        var indexCommand = connection.CreateCommand();
        indexCommand.CommandText = """
            SELECT name
            FROM sqlite_master
            WHERE type = 'index' AND name LIKE 'ix_messages_context_conversation_%'
            ORDER BY name
            """;
        var indexes = new List<string>();
        await using var reader = await indexCommand.ExecuteReaderAsync();
        while (await reader.ReadAsync()) indexes.Add(reader.GetString(0));
        Assert.Equal(["ix_messages_context_conversation_sequence"], indexes);

        var indexInfo = connection.CreateCommand();
        indexInfo.CommandText =
            "PRAGMA index_xinfo('ix_messages_context_conversation_sequence');";
        var indexedColumns = new List<(string Name, long Descending)>();
        await using var indexReader = await indexInfo.ExecuteReaderAsync();
        while (await indexReader.ReadAsync())
        {
            if (indexReader.GetInt64(5) != 0 && indexReader.GetInt64(1) >= 0)
                indexedColumns.Add((indexReader.GetString(2), indexReader.GetInt64(3)));
        }
        Assert.Equal(
            [
                ("project_context_key", 0L),
                ("conversation_id", 0L),
                ("message_sequence", 1L),
            ],
            indexedColumns);
    }

    [Fact]
    public async Task Multiple_store_instances_can_migrate_version_one_concurrently()
    {
        await using var database = new TemporaryDatabase();
        await database.CreateVersionOneSchemaAsync();
        var stores = Enumerable.Range(0, 8).Select(_ => database.CreateStore()).ToList();

        await Task.WhenAll(stores.Select(store => store.InitializeAsync()));

        Assert.Equal(
            ["legacy-first", "legacy-second"],
            (await stores[0].GetMessagesAsync(
                "legacy-project",
                database.VersionOneConversationId)).Select(message => message.Content));
    }

    [Fact]
    public async Task Oversized_message_is_rejected_without_changing_history_or_conversation_order()
    {
        await using var database = new TemporaryDatabase();
        var store = database.CreateStore();
        var conversation = await store.CreateAsync("lookdev-project", "Bounded history");
        var oversized = Message(
            conversation.Id,
            new string('x', PipeProtocol.MaximumConversationMessageCharacters + 1),
            conversation.UpdatedAt.AddDays(1));

        await Assert.ThrowsAsync<ArgumentException>(() =>
            store.AppendMessageAsync("lookdev-project", oversized));

        Assert.Empty(await store.GetMessagesAsync("lookdev-project", conversation.Id));
        Assert.Equal(
            conversation.UpdatedAt,
            Assert.Single(await store.ListAsync("lookdev-project")).UpdatedAt);
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

        public string DatabasePath => Path.Combine(_dataDirectory, "chat-history.sqlite3");
        public Guid VersionOneConversationId { get; } = Guid.NewGuid();

        public async Task CreateVersionOneSchemaAsync()
        {
            Directory.CreateDirectory(_dataDirectory);
            await using var connection = new SqliteConnection(
                $"Data Source={DatabasePath};Pooling=False");
            await connection.OpenAsync();
            var command = connection.CreateCommand();
            command.CommandText = """
                CREATE TABLE conversations (
                    project_context_key TEXT NOT NULL,
                    id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    PRIMARY KEY(project_context_key, id)
                );
                CREATE TABLE messages (
                    message_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                    project_context_key TEXT NOT NULL,
                    id TEXT NOT NULL,
                    conversation_id TEXT NOT NULL,
                    role TEXT NOT NULL,
                    content TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    is_error INTEGER NOT NULL DEFAULT 0,
                    UNIQUE(project_context_key, id),
                    FOREIGN KEY(project_context_key, conversation_id)
                        REFERENCES conversations(project_context_key, id)
                        ON DELETE CASCADE
                );
                CREATE INDEX ix_conversations_context_updated
                    ON conversations(project_context_key, updated_at DESC);
                CREATE INDEX ix_messages_context_conversation_order
                    ON messages(project_context_key, conversation_id, created_at, message_sequence);
                PRAGMA user_version = 1;
                """;
            await command.ExecuteNonQueryAsync();

            var seed = connection.CreateCommand();
            seed.CommandText = """
                INSERT INTO conversations(project_context_key, id, title, created_at, updated_at)
                VALUES($context, $conversation, 'Legacy', $created, $created);
                INSERT INTO messages(project_context_key, id, conversation_id, role, content, created_at, is_error)
                VALUES($context, $firstId, $conversation, 'user', 'legacy-first', $created, 0);
                INSERT INTO messages(project_context_key, id, conversation_id, role, content, created_at, is_error)
                VALUES($context, $secondId, $conversation, 'assistant', 'legacy-second', $older, 0);
                """;
            seed.Parameters.AddWithValue("$context", "legacy-project");
            seed.Parameters.AddWithValue("$conversation", VersionOneConversationId.ToString("D"));
            seed.Parameters.AddWithValue("$firstId", Guid.NewGuid().ToString("D"));
            seed.Parameters.AddWithValue("$secondId", Guid.NewGuid().ToString("D"));
            seed.Parameters.AddWithValue("$created", DateTimeOffset.UtcNow.ToString("O"));
            seed.Parameters.AddWithValue("$older", DateTimeOffset.UtcNow.AddDays(-1).ToString("O"));
            await seed.ExecuteNonQueryAsync();
        }

        public ValueTask DisposeAsync()
        {
            if (Directory.Exists(_dataDirectory)) Directory.Delete(_dataDirectory, recursive: true);
            return ValueTask.CompletedTask;
        }
    }
}
