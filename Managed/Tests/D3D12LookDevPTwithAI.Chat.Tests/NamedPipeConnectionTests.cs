using System.IO.Pipes;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.ChatHost;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class NamedPipeConnectionTests
{
    [Fact]
    public async Task Concurrent_writers_emit_strictly_increasing_wire_sequences()
    {
        const int messageCount = 128;
        var pipeName = $"LookDev.Chat.Tests.{Guid.NewGuid():N}";
        await using var server = new NamedPipeServerStream(
            pipeName,
            PipeDirection.InOut,
            1,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous);
        await using var connection = new NamedPipeConnection(
            new CommandLineOptions(pipeName, Environment.ProcessId));
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        var accepting = server.WaitForConnectionAsync(timeout.Token);
        await connection.ConnectAsync(timeout.Token);
        await accepting;

        var reader = Task.Run(async () =>
        {
            var sequences = new List<long>(messageCount);
            for (var index = 0; index < messageCount; index++)
            {
                var envelope = await PipeFraming.ReadAsync(server, timeout.Token);
                sequences.Add(Assert.IsType<PipeEnvelope>(envelope).Sequence);
            }
            return sequences;
        }, timeout.Token);

        var writers = Enumerable.Range(0, messageCount)
            .Select(index => Task.Run(
                () => connection.SendEventAsync(Guid.NewGuid(), "concurrent", new { index }, timeout.Token),
                timeout.Token))
            .ToList();
        await Task.WhenAll(writers);

        Assert.Equal(
            Enumerable.Range(1, messageCount).Select(sequence => (long)sequence),
            await reader);
    }
}
