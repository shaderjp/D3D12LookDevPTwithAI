using System.Diagnostics;
using D3D12LookDevPTwithAI.Chat.Core;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.ChatHost;

internal static class ChatHostShutdownBudget
{
    internal static readonly TimeSpan CleanupTimeout = TimeSpan.FromMilliseconds(750);
    internal static readonly TimeSpan HostTimeout = TimeSpan.FromSeconds(1);
}

public sealed class ChatHostWorker(
    CommandLineOptions options,
    NamedPipeConnection connection,
    PipeRequestRouter router,
    ChatCoordinator coordinator,
    IHostApplicationLifetime applicationLifetime) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await connection.ConnectAsync(stoppingToken).ConfigureAwait(false);
            _ = MonitorParentAsync(stoppingToken);
            while (!stoppingToken.IsCancellationRequested)
            {
                var request = await connection.ReadAsync(stoppingToken).ConfigureAwait(false);
                if (request is null) break;
                await router.HandleAsync(request, connection, stoppingToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { }
        catch (EndOfStreamException) { }
        catch (IOException) { }
        finally
        {
            await CompleteShutdownAsync(
                coordinator.StopAsync,
                applicationLifetime,
                ChatHostShutdownBudget.CleanupTimeout).ConfigureAwait(false);
        }
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        await coordinator.StopAsync(cancellationToken).ConfigureAwait(false);
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
    }

    internal static async Task CompleteShutdownAsync(
        Func<CancellationToken, Task> cleanupAsync,
        IHostApplicationLifetime applicationLifetime,
        TimeSpan cleanupTimeout)
    {
        ArgumentNullException.ThrowIfNull(cleanupAsync);
        ArgumentNullException.ThrowIfNull(applicationLifetime);
        if (cleanupTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(cleanupTimeout));

        using var timeout = new CancellationTokenSource(cleanupTimeout);
        try
        {
            await cleanupAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested)
        {
        }
        finally
        {
            applicationLifetime.StopApplication();
        }
    }

    private async Task MonitorParentAsync(CancellationToken cancellationToken)
    {
        try
        {
            using var parent = Process.GetProcessById(options.ParentProcessId);
            await parent.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            applicationLifetime.StopApplication();
        }
        catch (ArgumentException)
        {
            applicationLifetime.StopApplication();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
    }
}
