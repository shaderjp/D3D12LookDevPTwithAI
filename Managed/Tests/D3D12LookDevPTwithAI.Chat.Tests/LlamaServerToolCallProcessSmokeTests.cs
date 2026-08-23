using System.Diagnostics;
using System.Globalization;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class LlamaServerToolCallProcessSmokeTests
{
    private static readonly TimeSpan ProcessTimeout = TimeSpan.FromSeconds(15);

    [Fact]
    public async Task Production_adapter_accepts_fragmented_b10205_tool_call_over_process_owned_socket()
    {
        if (!OperatingSystem.IsWindows()) return;

        var shellPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell",
            "v1.0",
            "powershell.exe");
        Assert.True(File.Exists(shellPath));

        var fixturePath = Path.Combine(
            AppContext.BaseDirectory,
            "Fixtures",
            "LlamaServerToolCallSseFixture.ps1");
        Assert.True(File.Exists(fixturePath));

        var temporaryDirectory = Path.Combine(
            Path.GetTempPath(),
            $"D3D12LookDevPTwithAI-LlamaToolSmoke-{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        var readyFile = Path.Combine(temporaryDirectory, "ready.txt");

        using var process = new Process
        {
            StartInfo = CreateFixtureStartInfo(shellPath, fixturePath, readyFile),
        };
        var processStarted = false;
        Task<string>? stdoutTask = null;
        Task<string>? stderrTask = null;
        try
        {
            Assert.True(process.Start());
            processStarted = true;
            using var timeout = new CancellationTokenSource(ProcessTimeout);
            stdoutTask = ReadBoundedAsync(process.StandardOutput, timeout.Token);
            stderrTask = ReadBoundedAsync(process.StandardError, timeout.Token);
            var port = await WaitForReadyPortAsync(
                readyFile,
                process,
                timeout.Token);

            using var platform = new SystemLlamaServerPlatform();
            Assert.True(platform.IsLoopbackPortOwnedByProcess(port, process.Id));
            using var registration = VerifiedLoopbackProcessRegistry.Register(
                port,
                process.Id);
            using var runtime = new LlamaServerChatInferenceRuntime(
                new FixtureSessionProvider(new LlamaServerSession(
                    new Uri($"http://127.0.0.1:{port}/"),
                    "fixture-token",
                    "cpu",
                    "fixture-model",
                    Temperature: 0,
                    MaxTokens: 128)));

            var chunks = new List<ChatInferenceChunk>();
            await foreach (var chunk in runtime.StreamAsync(new ChatInferenceRequest(
                Guid.NewGuid(),
                "fixture-scene",
                Array.Empty<ChatInferenceMessage>(),
                "Use the live scene-state tool.",
                [new ChatInferenceToolDefinition(
                    "lookdev.get_scene_state",
                    "Read the active scene state.",
                    "{\"type\":\"object\",\"additionalProperties\":false}")]),
                timeout.Token))
            {
                chunks.Add(chunk);
            }

            await process.WaitForExitAsync(timeout.Token);
            Assert.Equal(0, process.ExitCode);
            Assert.Equal(string.Empty, await stdoutTask);
            Assert.Equal(string.Empty, await stderrTask);
            Assert.Equal(2, chunks.Count);
            Assert.Equal("Checking ", chunks[0].Text);
            Assert.Null(chunks[0].ToolCalls);

            var toolCall = Assert.Single(chunks[1].ToolCalls!);
            Assert.Equal(string.Empty, chunks[1].Text);
            Assert.Equal("call_scene_state", toolCall.Id);
            Assert.Equal("lookdev.get_scene_state", toolCall.Name);
            Assert.Equal("{\"scope\":\"active_scene\"}", toolCall.ArgumentsJson);
        }
        finally
        {
            if (processStarted && !process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
            }
            if (stdoutTask is not null)
                _ = await IgnoreFailureAsync(stdoutTask);
            if (stderrTask is not null)
                _ = await IgnoreFailureAsync(stderrTask);
            Directory.Delete(temporaryDirectory, recursive: true);
        }
    }

    private static ProcessStartInfo CreateFixtureStartInfo(
        string shellPath,
        string fixturePath,
        string readyFile)
    {
        var startInfo = new ProcessStartInfo(shellPath)
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false,
        };
        startInfo.ArgumentList.Add("-NoLogo");
        startInfo.ArgumentList.Add("-NoProfile");
        startInfo.ArgumentList.Add("-NonInteractive");
        startInfo.ArgumentList.Add("-ExecutionPolicy");
        startInfo.ArgumentList.Add("Bypass");
        startInfo.ArgumentList.Add("-File");
        startInfo.ArgumentList.Add(fixturePath);
        startInfo.ArgumentList.Add("-ReadyFile");
        startInfo.ArgumentList.Add(readyFile);
        return startInfo;
    }

    private static async Task<int> WaitForReadyPortAsync(
        string readyFile,
        Process process,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (process.HasExited)
                throw new InvalidOperationException(
                    "The llama-server fixture exited before becoming ready.");
            try
            {
                if (File.Exists(readyFile))
                {
                    var value = await File.ReadAllTextAsync(
                        readyFile,
                        cancellationToken);
                    if (value.Length <= 5 &&
                        int.TryParse(
                            value,
                            NumberStyles.None,
                            CultureInfo.InvariantCulture,
                            out var port) &&
                        port is > 0 and <= 65_535)
                    {
                        return port;
                    }
                }
            }
            catch (IOException)
            {
                // The fixture can briefly hold the file while publishing it.
            }
            await Task.Delay(TimeSpan.FromMilliseconds(20), cancellationToken);
        }
    }

    private static async Task<string> ReadBoundedAsync(
        StreamReader reader,
        CancellationToken cancellationToken)
    {
        const int maximumCharacters = 8 * 1024;
        var result = new char[maximumCharacters + 1];
        var offset = 0;
        while (offset < result.Length)
        {
            var read = await reader.ReadAsync(
                result.AsMemory(offset, result.Length - offset),
                cancellationToken);
            if (read == 0) break;
            offset += read;
        }
        if (offset > maximumCharacters)
            throw new InvalidOperationException(
                "The llama-server fixture emitted too much diagnostic output.");
        return new string(result, 0, offset);
    }

    private static async Task<bool> IgnoreFailureAsync(Task task)
    {
        try
        {
            await task;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private sealed class FixtureSessionProvider(LlamaServerSession session)
        : ILlamaServerSessionProvider
    {
        public ValueTask<LlamaServerSession?> GetSessionAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult<LlamaServerSession?>(session);
        }
    }
}
