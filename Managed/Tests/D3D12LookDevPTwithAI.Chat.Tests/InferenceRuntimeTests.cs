using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class InferenceRuntimeTests
{
    [Fact]
    public async Task Deterministic_runtime_streams_nonempty_ordered_chunks()
    {
        var runtime = new DeterministicChatInferenceRuntime(
            chunkCharacters: 7,
            chunkDelay: TimeSpan.Zero);

        var chunks = await CollectAsync(runtime.StreamAsync(Request("inspect this scene")));

        Assert.NotEmpty(chunks);
        Assert.All(chunks, chunk => Assert.InRange(chunk.Length, 1, 7));
        Assert.Equal(
            "[Local ChatHost placeholder] inspect this scene",
            string.Concat(chunks));
    }

    [Fact]
    public async Task Deterministic_runtime_reports_local_ready_status()
    {
        var runtime = new DeterministicChatInferenceRuntime();

        var status = await runtime.GetStatusAsync();

        Assert.Equal(DeterministicChatInferenceRuntime.RuntimeId, status.RuntimeId);
        Assert.True(status.IsReady);
        Assert.Equal("ready", status.State);
    }

    [Fact]
    public async Task Deterministic_runtime_observes_stream_cancellation()
    {
        var runtime = new DeterministicChatInferenceRuntime(
            chunkCharacters: 1,
            chunkDelay: TimeSpan.FromSeconds(30));
        using var cancellation = new CancellationTokenSource();
        await using var enumerator = runtime.StreamAsync(
            Request("cancel this response"),
            cancellation.Token).GetAsyncEnumerator();

        Assert.True(await enumerator.MoveNextAsync());
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await enumerator.MoveNextAsync().AsTask());
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public async Task Deterministic_runtime_rejects_empty_input(string input)
    {
        var runtime = new DeterministicChatInferenceRuntime(chunkDelay: TimeSpan.Zero);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(Request(input))));

        Assert.Equal("invalid_inference_request", exception.Code);
        Assert.False(exception.Retryable);
    }

    [Fact]
    public async Task Deterministic_runtime_validates_tool_history_names()
    {
        var runtime = new DeterministicChatInferenceRuntime(chunkDelay: TimeSpan.Zero);
        var request = Request("continue") with
        {
            History =
            [
                new ChatInferenceMessage(
                    ChatInferenceRole.Tool,
                    "tool result",
                    Name: null),
            ],
        };

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(request)));

        Assert.Equal("invalid_inference_request", exception.Code);
    }

    [Fact]
    public async Task Deterministic_runtime_rejects_an_unbounded_history_context()
    {
        var runtime = new DeterministicChatInferenceRuntime(chunkDelay: TimeSpan.Zero);
        var request = Request("continue") with
        {
            History = Enumerable.Range(0, 5)
                .Select(_ => new ChatInferenceMessage(
                    ChatInferenceRole.User,
                    new string('h', ChatInferenceLimits.MaximumHistoryMessageCharacters)))
                .ToArray(),
        };

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => CollectAsync(runtime.StreamAsync(request)));

        Assert.Equal("invalid_inference_request", exception.Code);
    }

    private static ChatInferenceRequest Request(string input) =>
        new(
            Guid.Parse("11111111-1111-1111-1111-111111111111"),
            "inference-runtime-tests",
            [
                new ChatInferenceMessage(
                    ChatInferenceRole.User,
                    "earlier question"),
                new ChatInferenceMessage(
                    ChatInferenceRole.Assistant,
                    "earlier answer"),
            ],
            input);

    private static async Task<IReadOnlyList<string>> CollectAsync(
        IAsyncEnumerable<ChatInferenceChunk> chunks)
    {
        var result = new List<string>();
        await foreach (var chunk in chunks)
            result.Add(chunk.Text);
        return result;
    }
}
