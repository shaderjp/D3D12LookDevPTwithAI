using System.Globalization;
using D3D12LookDevPTwithAI.Chat.Core;
using Microsoft.Data.Sqlite;

namespace D3D12LookDevPTwithAI.Chat.Infrastructure;

public sealed class SqliteConversationStore(IAppPaths paths) : IConversationStore
{
    private const int CurrentSchemaVersion = 1;
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

            if (version == 0)
                await ApplyVersionOneAsync(connection, cancellationToken).ConfigureAwait(false);

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
            ORDER BY created_at, message_sequence
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

    public async Task AppendMessageAsync(
        string projectContextKey,
        ConversationMessage message,
        CancellationToken cancellationToken = default)
    {
        ValidateProjectContextKey(projectContextKey);
        ArgumentNullException.ThrowIfNull(message);
        await InitializeAsync(cancellationToken).ConfigureAwait(false);

        await using var connection = CreateConnection();
        await connection.OpenAsync(cancellationToken).ConfigureAwait(false);
        await using var transaction = await connection.BeginTransactionAsync(cancellationToken).ConfigureAwait(false);
        var insert = connection.CreateCommand();
        insert.Transaction = (SqliteTransaction)transaction;
        insert.CommandText = """
            INSERT INTO messages(project_context_key, id, conversation_id, role, content, created_at, is_error)
            VALUES($context, $id, $conversation, $role, $content, $created, $error);
            UPDATE conversations
            SET updated_at = CASE WHEN updated_at < $updated THEN $updated ELSE updated_at END
            WHERE project_context_key = $context AND id = $conversation;
            """;
        insert.Parameters.AddWithValue("$context", projectContextKey);
        insert.Parameters.AddWithValue("$id", message.Id.ToString("D"));
        insert.Parameters.AddWithValue("$conversation", message.ConversationId.ToString("D"));
        insert.Parameters.AddWithValue("$role", message.Role);
        insert.Parameters.AddWithValue("$content", message.Content);
        insert.Parameters.AddWithValue("$created", FormatTimestamp(message.CreatedAt));
        insert.Parameters.AddWithValue("$updated", FormatTimestamp(message.CreatedAt));
        insert.Parameters.AddWithValue("$error", message.IsError ? 1 : 0);
        await insert.ExecuteNonQueryAsync(cancellationToken).ConfigureAwait(false);
        await transaction.CommitAsync(cancellationToken).ConfigureAwait(false);
    }

    private static async Task ApplyVersionOneAsync(
        SqliteConnection connection,
        CancellationToken cancellationToken)
    {
        await using var transaction = await connection.BeginTransactionAsync(cancellationToken).ConfigureAwait(false);
        var command = connection.CreateCommand();
        command.Transaction = (SqliteTransaction)transaction;
        command.CommandText = """
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
            CREATE INDEX IF NOT EXISTS ix_messages_context_conversation_order
                ON messages(project_context_key, conversation_id, created_at, message_sequence);

            PRAGMA user_version = 1;
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
