using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class LlamaServerProcessSessionProviderTests
{
    [Fact]
    public async Task Missing_settings_return_not_ready_without_starting_a_process()
    {
        var settingsProvider = new MutableSettingsProvider(null);
        var platform = new FakePlatform();
        await using var provider = new LlamaServerProcessSessionProvider(
            settingsProvider,
            platform);

        var session = await provider.GetSessionAsync();

        Assert.Null(session);
        Assert.Equal(0, platform.StartCount);
        Assert.Empty(platform.VerifiedArtifacts);
    }

    [Theory]
    [InlineData(LocalInferenceBackend.Cpu, "cpu", "0")]
    [InlineData(LocalInferenceBackend.Cuda, "cuda", "999")]
    [InlineData(LocalInferenceBackend.Vulkan, "vulkan", "999")]
    public async Task Launch_uses_hardened_start_info_and_expected_arguments(
        LocalInferenceBackend backend,
        string expectedBackend,
        string expectedGpuLayers)
    {
        var settings = CreateSettings(backend);
        var platform = new FakePlatform { Port = 23_451 };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(settings),
            platform);

        var session = Assert.IsType<LlamaServerSession>(
            await provider.GetSessionAsync());
        var request = Assert.Single(platform.StartRequests);

        Assert.Equal(expectedBackend, session.Backend);
        Assert.Equal(platform.Port, session.Endpoint.Port);
        Assert.Equal(settings.Model.FullPath, ArgumentValue(request, "--model"));
        Assert.Equal(settings.ModelId, ArgumentValue(request, "--alias"));
        Assert.Equal("127.0.0.1", ArgumentValue(request, "--host"));
        Assert.Equal(platform.Port.ToString(), ArgumentValue(request, "--port"));
        Assert.Equal(settings.ContextSize.ToString(), ArgumentValue(request, "--ctx-size"));
        Assert.Equal(settings.MaxTokens.ToString(), ArgumentValue(request, "--n-predict"));
        Assert.Equal(expectedGpuLayers, ArgumentValue(request, "--n-gpu-layers"));
        Assert.Contains("--no-ui", request.Arguments);
        Assert.Contains("--offline", request.Arguments);
        Assert.Contains("--jinja", request.Arguments);
        Assert.Contains("--no-cors-credentials", request.Arguments);
        Assert.Contains("--log-disable", request.Arguments);
        Assert.Equal("localhost", ArgumentValue(request, "--cors-origins"));

        var apiKeyFile = ArgumentValue(request, "--api-key-file");
        Assert.False(File.Exists(apiKeyFile));
        var capturedApiKey = Assert.Single(platform.CapturedApiKeys);
        Assert.Equal(64, capturedApiKey.Length);
        Assert.True(capturedApiKey.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f'));
        AssertSensitiveStringEqual(capturedApiKey, session.ApiKey);
        Assert.DoesNotContain(
            request.Arguments,
            argument => argument.Contains(capturedApiKey, StringComparison.Ordinal));

        var startInfo = SystemLlamaServerPlatform.CreateStartInfo(request);
        Assert.False(startInfo.UseShellExecute);
        Assert.True(startInfo.CreateNoWindow);
        Assert.True(startInfo.RedirectStandardOutput);
        Assert.True(startInfo.RedirectStandardError);
        Assert.False(startInfo.RedirectStandardInput);
        Assert.Equal(request.ExecutablePath, startInfo.FileName);
        Assert.Equal(request.Arguments.Count, startInfo.ArgumentList.Count);
        for (var index = 0; index < request.Arguments.Count; ++index)
        {
            AssertSensitiveStringEqual(
                request.Arguments[index],
                startInfo.ArgumentList[index]);
        }
        Assert.DoesNotContain(
            startInfo.Environment.Keys,
            name => name.Contains("TOKEN", StringComparison.OrdinalIgnoreCase) ||
                name.Contains("KEY", StringComparison.OrdinalIgnoreCase) ||
                name.Contains("SECRET", StringComparison.OrdinalIgnoreCase));
        Assert.Equal(Environment.SystemDirectory, startInfo.Environment["PATH"]);
    }

    [Fact]
    public async Task Concurrent_callers_share_one_startup()
    {
        var enteredHealth = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseHealth = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var healthCalls = 0;
        var platform = new FakePlatform
        {
            HealthHandler = async (_, cancellationToken) =>
            {
                if (Interlocked.Increment(ref healthCalls) == 1)
                {
                    enteredHealth.TrySetResult();
                    await releaseHealth.Task.WaitAsync(cancellationToken);
                }
                return true;
            },
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);

        var first = provider.GetSessionAsync().AsTask();
        await enteredHealth.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = provider.GetSessionAsync().AsTask();
        await Task.Yield();

        Assert.Equal(1, platform.StartCount);
        releaseHealth.TrySetResult();
        var sessions = await Task.WhenAll(first, second);

        Assert.Equal(1, platform.StartCount);
        Assert.True(ReferenceEquals(sessions[0], sessions[1]));
    }

    [Fact]
    public async Task Health_is_not_accepted_until_the_listener_pid_matches_the_child()
    {
        var ownerChecks = 0;
        var platform = new FakePlatform
        {
            PortOwnerHandler = (_, _) => Interlocked.Increment(ref ownerChecks) >= 2,
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);

        var session = await provider.GetSessionAsync();

        Assert.NotNull(session);
        Assert.Equal(1, platform.HealthCallCount);
        Assert.Equal(3, platform.PortOwnerCheckCount);
    }

    [Fact]
    public async Task Healthy_process_is_reused_for_the_same_configuration()
    {
        var platform = new FakePlatform();
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);

        var first = await provider.GetSessionAsync();
        var second = await provider.GetSessionAsync();

        Assert.NotNull(first);
        Assert.True(ReferenceEquals(first, second));
        Assert.Equal(1, platform.StartCount);
        Assert.Equal(2, platform.HealthCallCount);
        Assert.Equal(3, platform.VerifiedArtifacts.Count);
    }

    [Fact]
    public async Task Unhealthy_reused_process_is_replaced()
    {
        var results = new ConcurrentQueue<bool>([true, false, true]);
        var platform = new FakePlatform
        {
            HealthHandler = (_, _) =>
                ValueTask.FromResult(results.TryDequeue(out var result) && result),
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);

        var first = await provider.GetSessionAsync();
        var second = await provider.GetSessionAsync();

        Assert.NotNull(first);
        Assert.NotNull(second);
        Assert.False(ReferenceEquals(first, second));
        Assert.Equal(2, platform.StartCount);
        Assert.Equal(1, platform.Processes[0].KillCount);
    }

    [Fact]
    public async Task Configuration_change_restarts_with_new_backend()
    {
        var settingsProvider = new MutableSettingsProvider(CreateSettings());
        var platform = new FakePlatform();
        await using var provider = new LlamaServerProcessSessionProvider(
            settingsProvider,
            platform);

        var first = await provider.GetSessionAsync();
        settingsProvider.Settings = CreateSettings(LocalInferenceBackend.Cuda) with
        {
            ContextSize = 8192,
        };
        var second = await provider.GetSessionAsync();

        Assert.NotNull(first);
        Assert.NotNull(second);
        Assert.False(ReferenceEquals(first, second));
        Assert.Equal(2, platform.StartCount);
        Assert.Equal(1, platform.Processes[0].KillCount);
        Assert.Equal("999", ArgumentValue(platform.StartRequests[1], "--n-gpu-layers"));
    }

    [Fact]
    public async Task Hash_mismatch_is_a_fixed_safe_failure()
    {
        var settings = CreateSettings();
        var platform = new FakePlatform
        {
            ArtifactHandler = (artifact, _) =>
                ValueTask.FromResult(!ReferenceEquals(artifact, settings.Model)),
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(settings),
            platform);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => provider.GetSessionAsync().AsTask());

        Assert.Equal("local_runtime_artifact_invalid", exception.Code);
        Assert.Equal(
            "Local inference files failed integrity verification.",
            exception.Message);
        Assert.False(exception.Message.Contains(
            settings.Model.FullPath,
            StringComparison.OrdinalIgnoreCase));
        Assert.Equal(0, platform.StartCount);
    }

    [Fact]
    public async Task Artifact_verification_observes_cancellation()
    {
        var enteredVerification = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var platform = new FakePlatform
        {
            ArtifactHandler = async (_, cancellationToken) =>
            {
                enteredVerification.TrySetResult();
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                return true;
            },
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);
        using var cancellation = new CancellationTokenSource();

        var pending = provider.GetSessionAsync(cancellation.Token).AsTask();
        await enteredVerification.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        Assert.Equal(0, platform.StartCount);
    }

    [Fact]
    public async Task Cancellation_during_startup_kills_the_unready_process()
    {
        var enteredHealth = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var platform = new FakePlatform
        {
            HealthHandler = async (_, cancellationToken) =>
            {
                enteredHealth.TrySetResult();
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                return false;
            },
        };
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);
        using var cancellation = new CancellationTokenSource();

        var pending = provider.GetSessionAsync(cancellation.Token).AsTask();
        await enteredHealth.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        var process = Assert.Single(platform.Processes);
        Assert.Equal(1, process.KillCount);
        Assert.Equal(1, process.DisposeCount);
    }

    [Fact]
    public async Task Early_process_exit_has_no_path_or_api_key_details()
    {
        var platform = new FakePlatform
        {
            ProcessFactory = () => new FakeProcess(initiallyExited: true),
        };
        var settings = CreateSettings();
        await using var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(settings),
            platform);

        var exception = await Assert.ThrowsAsync<ChatInferenceException>(
            () => provider.GetSessionAsync().AsTask());

        Assert.Equal("local_runtime_start_failed", exception.Code);
        Assert.Equal(
            "The local inference runtime could not be started.",
            exception.Message);
        var request = Assert.Single(platform.StartRequests);
        var apiKeyFile = ArgumentValue(request, "--api-key-file");
        var apiKey = Assert.Single(platform.CapturedApiKeys);
        Assert.False(File.Exists(apiKeyFile));
        Assert.False(exception.Message.Contains(apiKey, StringComparison.Ordinal));
        Assert.False(exception.Message.Contains(
            settings.Runtime.FullPath,
            StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public async Task Dispose_kills_process_tree_immediately_and_is_idempotent()
    {
        var platform = new FakePlatform();
        var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);
        Assert.NotNull(await provider.GetSessionAsync());
        var process = Assert.Single(platform.Processes);

        await provider.DisposeAsync();
        await provider.DisposeAsync();

        Assert.Equal(1, process.KillCount);
        Assert.Equal(1, process.WaitCount);
        Assert.Equal(1, process.DisposeCount);
        Assert.Equal(1, platform.DisposeCount);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => provider.GetSessionAsync().AsTask());
    }

    [Fact]
    public async Task Dispose_wait_is_bounded_when_child_does_not_exit()
    {
        var platform = new FakePlatform
        {
            ProcessFactory = () => new FakeProcess(exitOnKill: false),
        };
        var provider = new LlamaServerProcessSessionProvider(
            new MutableSettingsProvider(CreateSettings()),
            platform);
        Assert.NotNull(await provider.GetSessionAsync());
        var process = Assert.Single(platform.Processes);

        var stopwatch = Stopwatch.StartNew();
        await provider.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        stopwatch.Stop();

        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(2));
        Assert.Equal(1, process.KillCount);
        Assert.Equal(1, process.WaitCount);
        Assert.Equal(1, process.DisposeCount);
    }

    [Fact]
    public async Task Production_hash_verifier_checks_size_hash_and_cancellation()
    {
        var directory = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-hash-tests",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "model.gguf");
        var bytes = Encoding.UTF8.GetBytes("bounded local inference fixture");
        await File.WriteAllBytesAsync(path, bytes);
        var sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var artifact = new LocalInferenceArtifact(
            "fixture/model.gguf",
            path,
            sha256,
            bytes.LongLength);
        using var platform = new SystemLlamaServerPlatform();

        try
        {
            await using var validLease = Assert.IsAssignableFrom<ILlamaArtifactLease>(
                await platform.AcquireVerifiedArtifactAsync(
                    artifact,
                    CancellationToken.None));
            Assert.Null(await platform.AcquireVerifiedArtifactAsync(
                artifact with { Sha256 = new string('0', 64) },
                CancellationToken.None));
            Assert.Null(await platform.AcquireVerifiedArtifactAsync(
                artifact with { ExpectedSize = bytes.LongLength + 1 },
                CancellationToken.None));

            using var cancellation = new CancellationTokenSource();
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => platform.AcquireVerifiedArtifactAsync(
                    artifact,
                    cancellation.Token).AsTask());
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task Production_runtime_bundle_requires_an_exact_manifest_and_invalidates_on_mutation()
    {
        var directory = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-runtime-bundle-tests",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var runtimePath = Path.Combine(directory, "llama-server.exe");
        var dependencyPath = Path.Combine(directory, "ggml.dll");
        var emptyPath = Path.Combine(directory, "notice.txt");
        await File.WriteAllTextAsync(runtimePath, "runtime fixture");
        await File.WriteAllTextAsync(dependencyPath, "dependency fixture");
        await File.WriteAllBytesAsync(emptyPath, []);
        var runtime = CreateArtifactFromFile("cpu/fixture/llama-server.exe", runtimePath);
        LocalInferenceArtifact[] dependencies =
        [
            CreateArtifactFromFile("cpu/fixture/ggml.dll", dependencyPath),
            CreateArtifactFromFile("cpu/fixture/notice.txt", emptyPath),
        ];
        using var platform = new SystemLlamaServerPlatform();

        try
        {
            await using var lease = Assert.IsAssignableFrom<ILlamaArtifactLease>(
                await platform.AcquireVerifiedRuntimeBundleAsync(
                    runtime,
                    dependencies,
                    CancellationToken.None));
            Assert.True(lease.IsValid);
            Assert.Throws<IOException>(() =>
                File.WriteAllText(dependencyPath, "tampered"));

            var invalidated = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            lease.RegisterInvalidationCallback(() => invalidated.TrySetResult());
            var extraPath = Path.Combine(directory, "unlisted.dll");
            await File.WriteAllTextAsync(extraPath, "unlisted");
            await invalidated.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(lease.IsValid);
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void Health_handler_disables_proxy_redirects_and_cookies()
    {
        using var handler = SystemLlamaServerPlatform.CreateHttpHandler();

        Assert.False(handler.UseProxy);
        Assert.False(handler.AllowAutoRedirect);
        Assert.False(handler.UseCookies);
    }

    [Fact]
    public async Task Production_health_probe_does_not_send_the_api_key()
    {
        var handler = new RecordingHealthHandler();
        using var platform = new SystemLlamaServerPlatform(handler);

        var healthy = await platform.IsHealthyAsync(
            new Uri("http://127.0.0.1:23452/"),
            CancellationToken.None);

        Assert.True(healthy);
        Assert.Equal(HttpMethod.Get, handler.Method);
        Assert.Equal("/health", handler.RequestUri?.AbsolutePath);
        Assert.Null(handler.AuthorizationScheme);
        Assert.Null(handler.AuthorizationParameter);
    }

    [Fact]
    public async Task Production_connection_owner_check_requires_the_exact_established_tuple()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var serverPort = ((IPEndPoint)listener.LocalEndpoint).Port;
        using var client = new Socket(
            AddressFamily.InterNetwork,
            SocketType.Stream,
            ProtocolType.Tcp);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var acceptedTask = listener.AcceptSocketAsync(cancellation.Token).AsTask();

        await client.ConnectAsync(
            new IPEndPoint(IPAddress.Loopback, serverPort),
            cancellation.Token);
        using var accepted = await acceptedTask;
        var clientPort = Assert.IsType<IPEndPoint>(client.LocalEndPoint).Port;

        Assert.True(SystemLlamaServerPlatform.IsLoopbackConnectionOwnedByProcess(
            serverPort,
            clientPort,
            Environment.ProcessId));
        Assert.False(SystemLlamaServerPlatform.IsLoopbackConnectionOwnedByProcess(
            serverPort,
            clientPort,
            Environment.ProcessId + 1));
        Assert.False(SystemLlamaServerPlatform.IsLoopbackConnectionOwnedByProcess(
            serverPort,
            clientPort == IPEndPoint.MaxPort ? clientPort - 1 : clientPort + 1,
            Environment.ProcessId));
    }

    [Fact]
    public void Production_port_owner_check_requires_the_exact_process_and_loopback_bind()
    {
        using var platform = new SystemLlamaServerPlatform();
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Assert.True(platform.IsLoopbackPortOwnedByProcess(port, Environment.ProcessId));
            Assert.False(platform.IsLoopbackPortOwnedByProcess(port, Environment.ProcessId + 1));
        }
        finally
        {
            listener.Stop();
        }
    }

    private static LocalInferenceSettings CreateSettings(
        LocalInferenceBackend backend = LocalInferenceBackend.Cpu)
    {
        var root = Path.Combine(Path.GetTempPath(), "D3D12LookDevPTwithAI-process-tests");
        return new LocalInferenceSettings(
            SchemaVersion: 1,
            ModelId: "gemma-test-q4",
            Backend: backend,
            ContextSize: 4096,
            MaxTokens: 512,
            Temperature: 0.25,
            Model: new LocalInferenceArtifact(
                "fixture/model.gguf",
                Path.Combine(root, "Models", "fixture", "model.gguf"),
                new string('a', 64),
                ExpectedSize: 1024),
            Runtime: new LocalInferenceArtifact(
                "cpu/fixture/llama-server.exe",
                Path.Combine(root, "Runtimes", "cpu", "fixture", "llama-server.exe"),
                new string('b', 64),
                ExpectedSize: 2048),
            RuntimeDependencies:
            [
                new LocalInferenceArtifact(
                    "cpu/fixture/ggml.dll",
                    Path.Combine(root, "Runtimes", "cpu", "fixture", "ggml.dll"),
                    new string('c', 64),
                    ExpectedSize: 4096),
            ]);
    }

    private static string ArgumentValue(
        LlamaServerProcessStartRequest request,
        string name)
    {
        var index = -1;
        for (var candidate = 0; candidate < request.Arguments.Count - 1; ++candidate)
        {
            if (string.Equals(request.Arguments[candidate], name, StringComparison.Ordinal))
            {
                index = candidate;
                break;
            }
        }
        Assert.True(index >= 0);
        return request.Arguments[index + 1];
    }

    private static LocalInferenceArtifact CreateArtifactFromFile(
        string relativePath,
        string fullPath)
    {
        var bytes = File.ReadAllBytes(fullPath);
        return new LocalInferenceArtifact(
            relativePath,
            fullPath,
            Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            bytes.LongLength);
    }

    private static void AssertSensitiveStringEqual(string left, string? right)
    {
        var leftBytes = Encoding.UTF8.GetBytes(left);
        var rightBytes = Encoding.UTF8.GetBytes(right ?? string.Empty);
        try
        {
            Assert.True(CryptographicOperations.FixedTimeEquals(leftBytes, rightBytes));
        }
        finally
        {
            CryptographicOperations.ZeroMemory(leftBytes);
            CryptographicOperations.ZeroMemory(rightBytes);
        }
    }

    private sealed class MutableSettingsProvider(LocalInferenceSettings? settings) :
        ILocalInferenceSettingsProvider
    {
        internal LocalInferenceSettings? Settings { get; set; } = settings;

        public ValueTask<LocalInferenceSettings?> LoadAsync(
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(Settings);
        }
    }

    private sealed class FakePlatform : ILlamaServerPlatform
    {
        private readonly ConcurrentQueue<LlamaServerProcessStartRequest> _startRequests = [];
        private readonly ConcurrentQueue<FakeProcess> _processes = [];
        private readonly ConcurrentQueue<LocalInferenceArtifact> _verifiedArtifacts = [];
        private readonly ConcurrentQueue<string> _capturedApiKeys = [];
        private int _startCount;
        private int _healthCallCount;
        private int _portOwnerCheckCount;
        private int _disposeCount;

        internal Func<LocalInferenceArtifact, CancellationToken, ValueTask<bool>>?
            ArtifactHandler
        { get; init; }
        internal Func<Uri, CancellationToken, ValueTask<bool>>?
            HealthHandler
        { get; init; }
        internal Func<FakeProcess>? ProcessFactory { get; init; }
        internal Func<int, int, bool>? PortOwnerHandler { get; init; }
        internal int Port { get; init; } = 23_450;
        internal int StartCount => Volatile.Read(ref _startCount);
        internal int HealthCallCount => Volatile.Read(ref _healthCallCount);
        internal int PortOwnerCheckCount => Volatile.Read(ref _portOwnerCheckCount);
        internal int DisposeCount => Volatile.Read(ref _disposeCount);
        internal IReadOnlyList<LlamaServerProcessStartRequest> StartRequests =>
            _startRequests.ToArray();
        internal IReadOnlyList<FakeProcess> Processes => _processes.ToArray();
        internal IReadOnlyList<LocalInferenceArtifact> VerifiedArtifacts =>
            _verifiedArtifacts.ToArray();
        internal IReadOnlyList<string> CapturedApiKeys => _capturedApiKeys.ToArray();

        public async ValueTask<ILlamaArtifactLease?> AcquireVerifiedArtifactAsync(
            LocalInferenceArtifact artifact,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            _verifiedArtifacts.Enqueue(artifact);
            var valid = ArtifactHandler is null ||
                await ArtifactHandler(artifact, cancellationToken);
            return valid ? new FakeArtifactLease() : null;
        }

        public async ValueTask<ILlamaArtifactLease?> AcquireVerifiedRuntimeBundleAsync(
            LocalInferenceArtifact runtime,
            IReadOnlyList<LocalInferenceArtifact> dependencies,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var artifacts = new[] { runtime }.Concat(dependencies);
            foreach (var artifact in artifacts)
            {
                _verifiedArtifacts.Enqueue(artifact);
                if (ArtifactHandler is not null &&
                    !await ArtifactHandler(artifact, cancellationToken))
                {
                    return null;
                }
            }
            return new FakeArtifactLease();
        }

        public int AllocateLoopbackPort() => Port;

        public ILlamaServerProcess Start(LlamaServerProcessStartRequest request)
        {
            var keyFile = ArgumentValue(request, "--api-key-file");
            _capturedApiKeys.Enqueue(File.ReadAllText(keyFile).Trim());
            _startRequests.Enqueue(request);
            Interlocked.Increment(ref _startCount);
            var process = ProcessFactory?.Invoke() ?? new FakeProcess();
            _processes.Enqueue(process);
            return process;
        }

        public ValueTask<bool> IsHealthyAsync(
            Uri endpoint,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref _healthCallCount);
            return HealthHandler?.Invoke(endpoint, cancellationToken) ??
                ValueTask.FromResult(true);
        }

        public ValueTask DelayAsync(
            TimeSpan delay,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }

        public bool IsLoopbackPortOwnedByProcess(int port, int processId)
        {
            Interlocked.Increment(ref _portOwnerCheckCount);
            return PortOwnerHandler?.Invoke(port, processId) ?? true;
        }

        public IDisposable RegisterTrustedEndpoint(int port, int processId) =>
            NoopRegistration.Instance;

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }

    private sealed class NoopRegistration : IDisposable
    {
        internal static NoopRegistration Instance { get; } = new();
        public void Dispose()
        {
        }
    }

    private sealed class FakeArtifactLease : ILlamaArtifactLease
    {
        public bool IsValid => true;
        public void RegisterInvalidationCallback(Action callback) =>
            ArgumentNullException.ThrowIfNull(callback);
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class FakeProcess : ILlamaServerProcess
    {
        private readonly TaskCompletionSource _exit = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly bool _exitOnKill;
        private int _hasExited;
        private int _killCount;
        private int _waitCount;
        private int _disposeCount;
        private static int _nextProcessId = 4000;

        internal FakeProcess(
            bool initiallyExited = false,
            bool exitOnKill = true)
        {
            _exitOnKill = exitOnKill;
            ProcessId = Interlocked.Increment(ref _nextProcessId);
            if (initiallyExited) Exit();
        }

        public int ProcessId { get; }
        public bool HasExited => Volatile.Read(ref _hasExited) != 0;
        internal int KillCount => Volatile.Read(ref _killCount);
        internal int WaitCount => Volatile.Read(ref _waitCount);
        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void KillEntireProcessTree()
        {
            Interlocked.Increment(ref _killCount);
            if (_exitOnKill) Exit();
        }

        public async ValueTask WaitForExitAsync(CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref _waitCount);
            await _exit.Task.WaitAsync(cancellationToken);
        }

        public ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            return ValueTask.CompletedTask;
        }

        private void Exit()
        {
            Volatile.Write(ref _hasExited, 1);
            _exit.TrySetResult();
        }
    }

    private sealed class RecordingHealthHandler : HttpMessageHandler
    {
        internal HttpMethod? Method { get; private set; }
        internal Uri? RequestUri { get; private set; }
        internal string? AuthorizationScheme { get; private set; }
        internal string? AuthorizationParameter { get; private set; }

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Method = request.Method;
            RequestUri = request.RequestUri;
            AuthorizationScheme = request.Headers.Authorization?.Scheme;
            AuthorizationParameter = request.Headers.Authorization?.Parameter;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK));
        }
    }
}
