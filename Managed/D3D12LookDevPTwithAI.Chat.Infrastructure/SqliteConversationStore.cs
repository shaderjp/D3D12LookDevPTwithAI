using System.Globalization;
using D3D12LookDevPTwithAI.Chat.Core;
using Microsoft.Data.Sqlite;

namespace D3D12LookDevPTwithAI.Chat.Infrastructure;

public sealed class SqliteConversationStore(IAppPaths paths) : IConversationStore
{
    private const int CurrentSchemaVersion = 2;
    private readonly SemaphoreSlim _initializeGate = new(1, 1);
    private volatile bool _initialized;

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        if (_initialized) return;

        await _initializeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_initialized) return;

            paths.EnsureCreated();
            await using var connection = CreateConnection();
            await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
            await ExecuteNonQueryAsync(
                connection,
                "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;",
                cancellationToken).ConfigureAwait(false);

            var versionCommand = connection.CreateCommand();
            versionCommand.CommandText = "PRAGMA user_version;";
            var version = Convert.ToInt32(
                await versionCommand.ExecuteScalarAsync(cancellationToken).ConfigureAwait(false),
                CultureInfo.InvariantCulture);

            if (version > CurrentSchemaVersion)
                throw new InvalidOperationException($"The chat history schema version {version} is newer than supported version {CurrentSchemaVersion}.");

            if (version < CurrentSchemaVersion)
                await ApplyMigrationsAsync(connection, version, cancellationToken).ConfigureAwait(false);

            _initialized = true;
        }
        finally
        {
            _initializeGate.Release();
        }
    }

    public async Task<IReadOnlyList<ConversationSummary>> ListAsync(
        string projectContextKey,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        var result = new List<ConversationSummary>();
        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.CommandText = """
            SELECT id, title, created_at, updated_at
            FROM conversations
            WHERE project_context_key = $context
            ORDER BY updated_at DESC, created_at DESC, id
            """;
        command.Parameters.AddWithValue("$context", projectContextKey);
        await using var reader = await command.ExecuteReaderAsync(cancellationToken).ConfigureAwait(false);
        while (await reader.ReadAsync(cancellationToken).ConfigureAwait(false))
            result.Add(ReadConversation(reader));
        return result;
    }

    public async Task<ConversationSummary?> GetAsync(
        string projectContextKey,
        Guid conversationId,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.CommandText = """
            SELECT id, title, created_at, updated_at
            FROM conversations
            WHERE project_context_key = $context AND id = $id
            """;
        command.Parameters.AddWithValue("$context", projectContextKey);
        command.Parameters.AddWithValue("$id", conversationId.ToString("D"));
        await using var reader = await command.ExecuteReaderAsync(cancellationToken).ConfigureAwait(false);
        return await reader.ReadAsync(cancellationToken).ConfigureAwait(false)
            ? ReadConversation(reader)
            : null;
    }

    public async Task<ConversationSummary> CreateAsync(
        string projectContextKey,
        string title,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        var now = DateTimeOffset.UtcNow;
        var conversation = new ConversationSummary(
            Guid.NewGuid(),
            string.IsNullOrWhiteSpace(title) ? "新しいチャット" : title.Trim(),
            now,
            now);
        if (conversation.Title.Length > PipeProtocol.MaximumConversationTitleCharacters)
            throw new ArgumentException(
                $"Conversation title must not exceed {PipeProtocol.MaximumConversationTitleCharacters} characters.",
                nameof(title));
        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.CommandText = """
            INSERT INTO conversations(project_context_key, id, title, created_at, updated_at)
            VALUES($context, $id, $title, $created, $updated)
            """;
        command.Parameters.AddWithValue("$context", projectContextKey);
        command.Parameters.AddWithValue("$id", conversation.Id.ToString("D"));
        command.Parameters.AddWithValue("$title", conversation.Title);
        command.Parameters.AddWithValue("$created", FormatTimestamp(conversation.CreatedAt));
        command.Parameters.AddWithValue("$updated", FormatTimestamp(conversation.UpdatedAt));
        await command.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);
        return conversation;
    }

    public async Task<IReadOnlyList<ConversationMessage>> GetMessagesAsync(
        string projectContextKey,
        Guid conversationId,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        var result = new List<ConversationMessage>();
        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.CommandText = """
            SELECT id, conversation_id, role, content, created_at, is_error
            FROM messages
            WHERE project_context_key = $context AND conversation_id = $conversation
            ORDER BY message_sequence
            """;
        command.Parameters.AddWithValue("$context", projectContextKey);
        command.Parameters.AddWithValue("$conversation", conversationId.ToString("D"));
        await using var reader = await command.ExecuteReaderAsync(cancellationToken).ConfigureAwait(false);
        while (await reader.ReadAsync(cancellationToken).ConfigureAwait(false))
        {
            result.Add(new ConversationMessage(
                Guid.Parse(reader.GetString(0)),
                Guid.Parse(reader.GetString(1)),
                reader.GetString(2),
                reader.GetString(3),
                ParseTimestamp(reader.GetString(4)),
                reader.GetInt32(5) != 0));
        }
        return result;
    }

    public async Task<IReadOnlyList<SequencedConversationMessage>> ListMessagesBeforeAsync(
        string projectContextKey,
        Guid conversationId,
        long? beforeMessageSequence,
        int maximumMessages,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        if (conversationId == Guid.Empty)
            throw new ArgumentException("A conversation identifier is required.", nameof(conversationId));
        if (beforeMessageSequence is <= 0 or > PipeProtocol.MaximumExactJsonInteger)
            throw new ArgumentOutOfRangeException(
                nameof(beforeMessageSequence),
                $"The message sequence cursor must be between 1 and {PipeProtocol.MaximumExactJsonInteger}.");
        if (maximumMessages <= 0 ||
            maximumMessages > PipeProtocol.MaximumConversationPageSize + 1)
        {
            throw new ArgumentOutOfRangeException(
                nameof(maximumMessages),
                $"The message page query must request between 1 and {PipeProtocol.MaximumConversationPageSize + 1} messages.");
        }
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        var result = new List<SequencedConversationMessage>();
        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.CommandText = beforeMessageSequence.HasValue
            ? """
                SELECT message_sequence, id, conversation_id, role, content, created_at, is_error
                FROM messages
                WHERE project_context_key = $context AND conversation_id = $conversation
                    AND message_sequence < $before
                ORDER BY message_sequence DESC
                LIMIT $limit
                """
            : """
                SELECT message_sequence, id, conversation_id, role, content, created_at, is_error
                FROM messages
                WHERE project_context_key = $context AND conversation_id = $conversation
                ORDER BY message_sequence DESC
                LIMIT $limit
                """;
        command.Parameters.AddWithValue("$context", projectContextKey);
        command.Parameters.AddWithValue("$conversation", conversationId.ToString("D"));
        command.Parameters.AddWithValue("$limit", maximumMessages);
        if (beforeMessageSequence.HasValue)
            command.Parameters.AddWithValue("$before", beforeMessageSequence.Value);
        await using var reader = await command.ExecuteReaderAsync(cancellationToken).ConfigureAwait(false);
        while (await reader.ReadAsync(cancellationToken).ConfigureAwait(false))
        {
            result.Add(new SequencedConversationMessage(
                reader.GetInt64(0),
                new ConversationMessage(
                    Guid.Parse(reader.GetString(1)),
                    Guid.Parse(reader.GetString(2)),
                    reader.GetString(3),
                    reader.GetString(4),
                    ParseTimestamp(reader.GetString(5)),
                    reader.GetInt32(6) != 0)));
        }
        return result;
    }

    public async Task AppendMessageAsync(
        string projectContextKey,
        ConversationMessage message,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        ArgumentNullException.ThrowIfNull(message);
        if (message.Id == Guid.Empty)
            throw new ArgumentException("A message identifier is required.", nameof(message));
        if (message.ConversationId == Guid.Empty)
            throw new ArgumentException("A conversation identifier is required.", nameof(message));
        if (string.IsNullOrWhiteSpace(message.Role) || message.Role.Length > 32)
            throw new ArgumentException("Message role is required and must not exceed 32 characters.", nameof(message));
        if (message.Content is null ||
            message.Content.Length > PipeProtocol.MaximumConversationMessageCharacters)
        {
            throw new ArgumentException(
                $"Message content must not exceed {PipeProtocol.MaximumConversationMessageCharacters} characters.",
                nameof(message));
        }
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        await using var transaction = await connection.BeginTransactionAsync(cancellationToken).ConfigureAwait(false);
        var insert = connection.CreateCommand();
        insert.Transaction = (SqliteTransaction)transaction;
        insert.CommandText = """
            INSERT INTO messages(project_context_key, id, conversation_id, role, content, created_at, is_error)
            VALUES($context, $id, $conversation, $role, $content, $created, $error)
            """;
        insert.Parameters.AddWithValue("$context", projectContextKey);
        insert.Parameters.AddWithValue("$id", message.Id.ToString("D"));
        insert.Parameters.AddWithValue("$conversation", message.ConversationId.ToString("D"));
        insert.Parameters.AddWithValue("$role", message.Role);
        insert.Parameters.AddWithValue("$content", message.Content);
        insert.Parameters.AddWithValue("$created", FormatTimestamp(message.CreatedAt));
        insert.Parameters.AddWithValue("$error", message.IsError ? 1 : 0);
        await insert.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);

        var sequenceCommand = connection.CreateCommand();
        sequenceCommand.Transaction = (SqliteTransaction)transaction;
        sequenceCommand.CommandText = "SELECT last_insert_rowid();";
        var insertedSequence = Convert.ToInt64(
            await sequenceCommand.ExecuteScalarAsync(cancellationToken).ConfigureAwait(false),
            CultureInfo.InvariantCulture);
        if (insertedSequence <= 0 || insertedSequence > PipeProtocol.MaximumExactJsonInteger)
            throw new InvalidOperationException("The chat history sequence exceeds the supported cursor range.");

        var update = connection.CreateCommand();
        update.Transaction = (SqliteTransaction)transaction;
        update.CommandText = """
            UPDATE conversations
            SET updated_at = CASE WHEN updated_at < $updated THEN $updated ELSE updated_at END
            WHERE project_context_key = $context AND id = $conversation
            """;
        update.Parameters.AddWithValue("$updated", FormatTimestamp(message.CreatedAt));
        update.Parameters.AddWithValue("$context", projectContextKey);
        update.Parameters.AddWithValue("$conversation", message.ConversationId.ToString("D"));
        await update.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);
        await transaction.CommitAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async Task ApplyMigrationsAsync(
        SqliteConnection connection,
        int version,
        CancellationToken cancellationToken)
    {
        await using var transaction = await connection.BeginTransactionAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.Transaction = (SqliteTransaction)transaction;
        command.CommandText = version == 0
            ? """
                CREATE TABLE IF NOT EXISTS conversations (
                    project_context_key TEXT NOT NULL,
                    id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    PRIMARY KEY(project_context_key, id)
                );

                CREATE TABLE IF NOT EXISTS messages (
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

                CREATE INDEX IF NOT EXISTS ix_conversations_context_updated
                    ON conversations(project_context_key, updated_at DESC);
                DROP INDEX IF EXISTS ix_messages_context_conversation_order;
                CREATE INDEX IF NOT EXISTS ix_messages_context_conversation_sequence
                    ON messages(project_context_key, conversation_id, message_sequence DESC);
                PRAGMA user_version = 2;
                """
            : """
                DROP INDEX IF EXISTS ix_messages_context_conversation_order;
                CREATE INDEX IF NOT EXISTS ix_messages_context_conversation_sequence
                    ON messages(project_context_key, conversation_id, message_sequence DESC);
                PRAGMA user_version = 2;
                """;
        await command.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);
        await transaction.CommitAsync(cancellationToken).ConfigureAwait(false);
    }

    private SqliteConnection CreateConnection()
    {
        var connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = paths.DatabasePath,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Cache = SqliteCacheMode.Shared,
            ForeignKeys = true,
            Pooling = false,
            DefaultTimeout = 5,
        };
        return new SqliteConnection(connectionString.ToString());
    }

    private static async Task<int> ExecuteNonQueryAsync(
        SqliteConnection connection,
        string commandText,
        CancellationToken cancellationToken)
    {
        await using var command = connection.CreateCommand();
        command.CommandText = commandText;
        return await command.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);
    }

    private static ConversationSummary ReadConversation(SqliteDataReader reader) => new(
        Guid.Parse(reader.GetString(0)),
        reader.GetString(1),
        ParseTimestamp(reader.GetString(2)),
        ParseTimestamp(reader.GetString(3)));

    private static string FormatTimestamp(DateTimeOffset timestamp) =>
        timestamp.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture);

    private static DateTimeOffset ParseTimestamp(string value) =>
        DateTimeOffset.Parse(value, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind);

    private static void ValidateProjectContextKey(string projectContextKey)
    {
        if (string.IsNullOrWhiteSpace(projectContextKey))
            throw new ArgumentException("A project context key is required.", nameof(projectContextKey));
    }
}
