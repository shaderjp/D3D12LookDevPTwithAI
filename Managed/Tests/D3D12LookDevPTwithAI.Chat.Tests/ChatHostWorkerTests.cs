using D3D12LookDevPTwithAI.ChatHost;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class ChatHostWorkerTests
{
    [Fact]
    public async Task Cleanup_timeout_is_swallowed_and_host_stop_is_always_requested()
    {
        var lifetime = new TestApplicationLifetime();

        await ChatHostWorker.CompleteShutdownAsync(
            cancellationToken => Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken),
            lifetime,
            TimeSpan.FromMilliseconds(25));

        Assert.True(lifetime.ApplicationStopping.IsCancellationRequested);
    }

    private sealed class TestApplicationLifetime : IHostApplicationLifetime
    {
        private readonly CancellationTokenSource _started = new();
        private readonly CancellationTokenSource _stopping = new();
        private readonly CancellationTokenSource _stopped = new();

        public CancellationToken ApplicationStarted => _started.Token;
        public CancellationToken ApplicationStopping => _stopping.Token;
        public CancellationToken ApplicationStopped => _stopped.Token;
        public void StopApplication() => _stopping.Cancel();
    }
}
