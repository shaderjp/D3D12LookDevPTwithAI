using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.ChatHost.Inference;

internal sealed class LlamaServerProcessSessionProvider :
    ILlamaServerSessionProvider,
    IAsyncDisposable
{
    private const string LoopbackHost = "127.0.0.1";
    private const int ApiKeyBytes = 32;
    private static readonly TimeSpan StartupTimeout = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan HealthPollInterval = TimeSpan.FromMilliseconds(200);
    private static readonly TimeSpan ProcessExitWait = TimeSpan.FromMilliseconds(500);

    private readonly ILocalInferenceSettingsProvider _settingsProvider;
    private readonly ILlamaServerPlatform _platform;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    private readonly CancellationToken _lifetimeToken;
    private readonly object _disposeLock = new();
    private RunningSession? _running;
    private Task? _disposeTask;
    private int _disposeStarted;

    internal LlamaServerProcessSessionProvider(
        ILocalInferenceSettingsProvider settingsProvider)
        : this(settingsProvider, new SystemLlamaServerPlatform())
    {
    }

    internal LlamaServerProcessSessionProvider(
        ILocalInferenceSettingsProvider settingsProvider,
        ILlamaServerPlatform platform)
    {
        ArgumentNullException.ThrowIfNull(settingsProvider);
        ArgumentNullException.ThrowIfNull(platform);
        _settingsProvider = settingsProvider;
        _platform = platform;
        _lifetimeToken = _lifetimeCancellation.Token;
    }

    public async ValueTask<LlamaServerSession?> GetSessionAsync(
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        using var linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            _lifetimeToken);
        var effectiveCancellation = linkedCancellation.Token;

        await _gate.WaitAsync(effectiveCancellation).ConfigureAwait(false);
        try
        {
            ThrowIfDisposed();
            effectiveCancellation.ThrowIfCancellationRequested();

            LocalInferenceSettings? settings;
            try
            {
                settings = await _settingsProvider.LoadAsync(effectiveCancellation)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (effectiveCancellation.IsCancellationRequested)
            {
                throw;
            }
            catch (LocalInferenceSettingsException)
            {
                throw InvalidConfiguration();
            }
            catch (Exception)
            {
                throw new ChatInferenceException(
                    "local_runtime_configuration_unavailable",
                    "The local inference settings could not be loaded.",
                    retryable: true);
            }

            if (settings is not null) ValidateSettings(settings);

            if (settings is null)
            {
                await StopRunningSessionAsync().ConfigureAwait(false);
                return null;
            }

            if (_running is { } current &&
                SameConfiguration(current.Settings, settings) &&
                !HasExited(current.Process) &&
                current.ArtifactLeases.All(lease => lease.IsValid) &&
                _platform.IsLoopbackPortOwnedByProcess(
                    current.Session.Endpoint.Port,
                    current.Process.ProcessId))
            {
                var healthy = await ProbeHealthAsync(
                    current.Session,
                    effectiveCancellation).ConfigureAwait(false);
                if (healthy &&
                    !HasExited(current.Process) &&
                    current.ArtifactLeases.All(lease => lease.IsValid) &&
                    _platform.IsLoopbackPortOwnedByProcess(
                        current.Session.Endpoint.Port,
                        current.Process.ProcessId))
                {
                    return current.Session;
                }
            }

            await StopRunningSessionAsync().ConfigureAwait(false);
            var artifactLeases = await AcquireArtifactsAsync(
                settings,
                effectiveCancellation).ConfigureAwait(false);
            return await StartAsync(
                settings,
                artifactLeases,
                effectiveCancellation).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeLock)
        {
            _disposeTask ??= DisposeCoreAsync();
            return new ValueTask(_disposeTask);
        }
    }

    private async Task DisposeCoreAsync()
    {
        if (Interlocked.Exchange(ref _disposeStarted, 1) == 0)
            _lifetimeCancellation.Cancel();

        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            await StopRunningSessionAsync().ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
            try
            {
                _platform.Dispose();
            }
            catch (Exception)
            {
                // Shutdown remains bounded and never exposes platform details.
            }
            _lifetimeCancellation.Dispose();
            _gate.Dispose();
        }
    }

    private async ValueTask<IReadOnlyList<ILlamaArtifactLease>> AcquireArtifactsAsync(
        LocalInferenceSettings settings,
        CancellationToken cancellationToken)
    {
        var leases = new List<ILlamaArtifactLease>(2);
        try
        {
            var modelLease = await AcquireArtifactAsync(
                settings.Model,
                cancellationToken).ConfigureAwait(false);
            if (modelLease is null) throw InvalidArtifact();
            leases.Add(modelLease);

            var runtimeLease = await AcquireRuntimeBundleAsync(
                settings.Runtime,
                settings.RuntimeDependencies,
                cancellationToken).ConfigureAwait(false);
            if (runtimeLease is null) throw InvalidArtifact();
            leases.Add(runtimeLease);
            return leases;
        }
        catch
        {
            await DisposeArtifactLeasesAsync(leases).ConfigureAwait(false);
            throw;
        }
    }

    private async ValueTask<ILlamaArtifactLease?> AcquireArtifactAsync(
        LocalInferenceArtifact artifact,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _platform.AcquireVerifiedArtifactAsync(artifact, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private async ValueTask<ILlamaArtifactLease?> AcquireRuntimeBundleAsync(
        LocalInferenceArtifact runtime,
        IReadOnlyList<LocalInferenceArtifact> dependencies,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _platform.AcquireVerifiedRuntimeBundleAsync(
                runtime,
                dependencies,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private async ValueTask<LlamaServerSession> StartAsync(
        LocalInferenceSettings settings,
        IReadOnlyList<ILlamaArtifactLease> artifactLeases,
        CancellationToken cancellationToken)
    {
        ILlamaServerProcess? process = null;
        IReadOnlyList<ILlamaArtifactLease>? unownedLeases = artifactLeases;
        TemporaryApiKeyFile? apiKeyFile = null;
        IDisposable? processRegistration = null;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var port = _platform.AllocateLoopbackPort();
            if (port is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort)
                throw StartFailure();

            var apiKey = CreateApiKey();
            var endpoint = new Uri(
                $"http://{LoopbackHost}:{port.ToString(CultureInfo.InvariantCulture)}/",
                UriKind.Absolute);
            var backend = BackendName(settings.Backend);
            var session = new LlamaServerSession(
                endpoint,
                apiKey,
                backend,
                settings.ModelId,
                settings.Temperature,
                settings.MaxTokens);
            apiKeyFile = await TemporaryApiKeyFile.CreateAsync(
                apiKey,
                cancellationToken).ConfigureAwait(false);
            var startRequest = CreateStartRequest(settings, port, apiKeyFile.Path);

            process = _platform.Start(startRequest);
            if (HasExited(process)) throw StartFailure();
            var startedProcess = process;
            foreach (var lease in artifactLeases)
            {
                lease.RegisterInvalidationCallback(() =>
                {
                    try
                    {
                        startedProcess.KillEntireProcessTree();
                    }
                    catch (Exception)
                    {
                        // A failed kill is contained by the native parent Job Object.
                    }
                });
            }
            if (artifactLeases.Any(lease => !lease.IsValid)) throw StartFailure();

            await WaitUntilHealthyAsync(
                process,
                session,
                cancellationToken).ConfigureAwait(false);
            if (HasExited(process)) throw StartFailure();
            await apiKeyFile.DisposeAsync().ConfigureAwait(false);
            apiKeyFile = null;
            processRegistration = _platform.RegisterTrustedEndpoint(
                port,
                process.ProcessId);

            _running = new RunningSession(
                settings,
                session,
                process,
                artifactLeases,
                processRegistration);
            process = null;
            unownedLeases = null;
            processRegistration = null;
            return session;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (ChatInferenceException)
        {
            throw;
        }
        catch (Exception)
        {
            throw StartFailure();
        }
        finally
        {
            processRegistration?.Dispose();
            if (process is not null)
                await TerminateProcessAsync(process).ConfigureAwait(false);
            try
            {
                if (apiKeyFile is not null)
                    await apiKeyFile.DisposeAsync().ConfigureAwait(false);
            }
            finally
            {
                if (unownedLeases is not null)
                    await DisposeArtifactLeasesAsync(unownedLeases).ConfigureAwait(false);
            }
        }
    }

    private async ValueTask WaitUntilHealthyAsync(
        ILlamaServerProcess process,
        LlamaServerSession session,
        CancellationToken cancellationToken)
    {
        using var startupCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        startupCancellation.CancelAfter(StartupTimeout);
        try
        {
            while (true)
            {
                if (HasExited(process)) throw StartFailure();
                if (!_platform.IsLoopbackPortOwnedByProcess(
                        session.Endpoint.Port,
                        process.ProcessId))
                {
                    await _platform.DelayAsync(
                        HealthPollInterval,
                        startupCancellation.Token).ConfigureAwait(false);
                    continue;
                }
                if (await ProbeHealthAsync(
                        session,
                        startupCancellation.Token).ConfigureAwait(false))
                {
                    if (_platform.IsLoopbackPortOwnedByProcess(
                            session.Endpoint.Port,
                            process.ProcessId))
                    {
                        return;
                    }
                }
                if (HasExited(process)) throw StartFailure();
                await _platform.DelayAsync(
                    HealthPollInterval,
                    startupCancellation.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new ChatInferenceException(
                "local_runtime_start_timeout",
                "The local inference runtime did not become ready in time.",
                retryable: true);
        }
    }

    private async ValueTask<bool> ProbeHealthAsync(
        LlamaServerSession session,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _platform.IsHealthyAsync(
                session.Endpoint,
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private async ValueTask StopRunningSessionAsync()
    {
        var running = _running;
        _running = null;
        if (running is not null)
        {
            running.ProcessRegistration.Dispose();
            await TerminateProcessAsync(running.Process).ConfigureAwait(false);
            await DisposeArtifactLeasesAsync(running.ArtifactLeases).ConfigureAwait(false);
        }
    }

    private static async ValueTask DisposeArtifactLeasesAsync(
        IReadOnlyList<ILlamaArtifactLease> leases)
    {
        for (var index = leases.Count - 1; index >= 0; --index)
        {
            try
            {
                await leases[index].DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Artifact paths and platform details are deliberately not propagated.
            }
        }
    }

    private static async ValueTask TerminateProcessAsync(ILlamaServerProcess process)
    {
        try
        {
            var confirmedExited = false;
            try
            {
                confirmedExited = process.HasExited;
            }
            catch (Exception)
            {
                // An unknown state is treated as running during cleanup.
            }
            if (!confirmedExited) process.KillEntireProcessTree();
        }
        catch (Exception)
        {
            // The bounded exit wait below is still attempted.
        }

        using var waitCancellation = new CancellationTokenSource(ProcessExitWait);
        try
        {
            await process.WaitForExitAsync(waitCancellation.Token).ConfigureAwait(false);
        }
        catch (Exception)
        {
            // Cleanup must not delay or fault host shutdown.
        }

        try
        {
            await process.DisposeAsync().ConfigureAwait(false);
        }
        catch (Exception)
        {
            // Process details are deliberately not propagated.
        }
    }

    private static LlamaServerProcessStartRequest CreateStartRequest(
        LocalInferenceSettings settings,
        int port,
        string apiKeyFilePath)
    {
        var gpuLayers = settings.Backend == LocalInferenceBackend.Cpu ? "0" : "999";
        string[] arguments =
        [
            "--model",
            settings.Model.FullPath,
            "--alias",
            settings.ModelId,
            "--host",
            LoopbackHost,
            "--port",
            port.ToString(CultureInfo.InvariantCulture),
            "--api-key-file",
            apiKeyFilePath,
            "--no-ui",
            "--offline",
            "--cors-origins",
            "localhost",
            "--no-cors-credentials",
            "--log-disable",
            "--ctx-size",
            settings.ContextSize.ToString(CultureInfo.InvariantCulture),
            "--n-predict",
            settings.MaxTokens.ToString(CultureInfo.InvariantCulture),
            "--n-gpu-layers",
            gpuLayers,
        ];
        return new LlamaServerProcessStartRequest(
            settings.Runtime.FullPath,
            Path.GetDirectoryName(settings.Runtime.FullPath)!,
            arguments);
    }

    private static string CreateApiKey()
    {
        Span<byte> bytes = stackalloc byte[ApiKeyBytes];
        RandomNumberGenerator.Fill(bytes);
        try
        {
            return Convert.ToHexString(bytes).ToLowerInvariant();
        }
        finally
        {
            CryptographicOperations.ZeroMemory(bytes);
        }
    }

    private static string BackendName(LocalInferenceBackend backend) => backend switch
    {
        LocalInferenceBackend.Cpu => "cpu",
        LocalInferenceBackend.Cuda => "cuda",
        LocalInferenceBackend.Vulkan => "vulkan",
        _ => throw InvalidConfiguration(),
    };

    private static void ValidateSettings(LocalInferenceSettings settings)
    {
        if (settings.SchemaVersion != 1 ||
            !IsSafeToken(
                settings.ModelId,
                LocalInferenceSettingsLimits.MaximumModelIdCharacters) ||
            !Enum.IsDefined(settings.Backend) ||
            settings.ContextSize is < LocalInferenceSettingsLimits.MinimumContextSize or
                > LocalInferenceSettingsLimits.MaximumContextSize ||
            settings.MaxTokens is < LocalInferenceSettingsLimits.MinimumMaxTokens or
                > LocalInferenceSettingsLimits.MaximumMaxTokens ||
            !double.IsFinite(settings.Temperature) ||
            settings.Temperature is < LocalInferenceSettingsLimits.MinimumTemperature or
                > LocalInferenceSettingsLimits.MaximumTemperature ||
            !IsValidArtifact(settings.Model) ||
            !IsValidArtifact(settings.Runtime) ||
            settings.RuntimeDependencies is null ||
            settings.RuntimeDependencies.Count >
                LocalInferenceSettingsLimits.MaximumRuntimeDependencies ||
            settings.RuntimeDependencies.Any(dependency =>
                !IsValidArtifact(dependency, allowEmpty: true)))
        {
            throw InvalidConfiguration();
        }
    }

    private static bool IsSafeToken(string? value, int maximumCharacters)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > maximumCharacters)
            return false;
        foreach (var character in value)
        {
            if ((character is >= 'a' and <= 'z') ||
                (character is >= 'A' and <= 'Z') ||
                (character is >= '0' and <= '9') ||
                character is '-' or '_' or '.')
            {
                continue;
            }
            return false;
        }
        return true;
    }

    private static bool IsValidArtifact(
        LocalInferenceArtifact? artifact,
        bool allowEmpty = false)
    {
        if (artifact is null ||
            string.IsNullOrWhiteSpace(artifact.RelativePath) ||
            string.IsNullOrWhiteSpace(artifact.FullPath) ||
            !Path.IsPathFullyQualified(artifact.FullPath) ||
            (allowEmpty ? artifact.ExpectedSize < 0 : artifact.ExpectedSize <= 0) ||
            artifact.Sha256 is not { Length: 64 })
        {
            return false;
        }
        return artifact.Sha256.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f' or >= 'A' and <= 'F');
    }

    private static bool SameConfiguration(
        LocalInferenceSettings left,
        LocalInferenceSettings right) =>
        left.SchemaVersion == right.SchemaVersion &&
        string.Equals(left.ModelId, right.ModelId, StringComparison.Ordinal) &&
        left.Backend == right.Backend &&
        left.ContextSize == right.ContextSize &&
        left.MaxTokens == right.MaxTokens &&
        BitConverter.DoubleToInt64Bits(left.Temperature) ==
            BitConverter.DoubleToInt64Bits(right.Temperature) &&
        SameArtifact(left.Model, right.Model) &&
        SameArtifact(left.Runtime, right.Runtime) &&
        left.RuntimeDependencies.Count == right.RuntimeDependencies.Count &&
        left.RuntimeDependencies.Zip(right.RuntimeDependencies).All(pair =>
            SameArtifact(pair.First, pair.Second));

    private static bool SameArtifact(
        LocalInferenceArtifact left,
        LocalInferenceArtifact right) =>
        string.Equals(left.RelativePath, right.RelativePath, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(left.FullPath, right.FullPath, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(left.Sha256, right.Sha256, StringComparison.OrdinalIgnoreCase) &&
        left.ExpectedSize == right.ExpectedSize;

    private static bool HasExited(ILlamaServerProcess process)
    {
        try
        {
            return process.HasExited;
        }
        catch (Exception)
        {
            return true;
        }
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposeStarted) != 0)
            throw new ObjectDisposedException(nameof(LlamaServerProcessSessionProvider));
    }

    private static ChatInferenceException InvalidConfiguration() => new(
        "local_runtime_configuration_invalid",
        "The local inference settings are invalid.");

    private static ChatInferenceException StartFailure() => new(
        "local_runtime_start_failed",
        "The local inference runtime could not be started.",
        retryable: true);

    private static ChatInferenceException InvalidArtifact() => new(
        "local_runtime_artifact_invalid",
        "Local inference files failed integrity verification.");

    private sealed class RunningSession(
        LocalInferenceSettings settings,
        LlamaServerSession session,
        ILlamaServerProcess process,
        IReadOnlyList<ILlamaArtifactLease> artifactLeases,
        IDisposable processRegistration)
    {
        internal LocalInferenceSettings Settings { get; } = settings;
        internal LlamaServerSession Session { get; } = session;
        internal ILlamaServerProcess Process { get; } = process;
        internal IReadOnlyList<ILlamaArtifactLease> ArtifactLeases { get; } =
            artifactLeases;
        internal IDisposable ProcessRegistration { get; } = processRegistration;

        public override string ToString() => nameof(RunningSession);
    }

    private sealed class TemporaryApiKeyFile : IAsyncDisposable
    {
        private readonly FileStream _stream;
        private int _disposed;

        private TemporaryApiKeyFile(string path, FileStream stream)
        {
            Path = path;
            _stream = stream;
        }

        internal string Path { get; }

        internal static async ValueTask<TemporaryApiKeyFile> CreateAsync(
            string apiKey,
            CancellationToken cancellationToken)
        {
            var directory = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                "D3D12LookDevPTwithAI",
                "AI");
            Directory.CreateDirectory(directory);
            var attributes = File.GetAttributes(directory);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw StartFailure();

            var path = System.IO.Path.Combine(
                directory,
                "llama-key-" + Guid.NewGuid().ToString("N") + ".tmp");
            FileStream? stream = null;
            try
            {
                await using (var writer = new FileStream(
                    path,
                    FileMode.CreateNew,
                    FileAccess.Write,
                    FileShare.None,
                    bufferSize: 4096,
                    FileOptions.Asynchronous |
                        FileOptions.WriteThrough))
                {
                    var payload = Encoding.UTF8.GetBytes(apiKey + "\n");
                    try
                    {
                        await writer.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
                        await writer.FlushAsync(cancellationToken).ConfigureAwait(false);
                    }
                    finally
                    {
                        CryptographicOperations.ZeroMemory(payload);
                    }
                }
                stream = new FileStream(
                    path,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.Read,
                    bufferSize: 4096,
                    FileOptions.Asynchronous);
                return new TemporaryApiKeyFile(path, stream);
            }
            catch
            {
                if (stream is not null) await stream.DisposeAsync().ConfigureAwait(false);
                try
                {
                    File.Delete(path);
                }
                catch (Exception exception) when (exception is
                    IOException or UnauthorizedAccessException)
                {
                    // The original fixed public startup failure remains authoritative.
                }
                throw;
            }
        }

        public async ValueTask DisposeAsync()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                await _stream.DisposeAsync().ConfigureAwait(false);
            try
            {
                File.Delete(Path);
            }
            catch (FileNotFoundException)
            {
                // The key file was already removed.
            }
            catch (DirectoryNotFoundException)
            {
                // The key file was already removed.
            }
            catch (IOException)
            {
                throw StartFailure();
            }
            catch (UnauthorizedAccessException)
            {
                throw StartFailure();
            }
        }

        public override string ToString() => nameof(TemporaryApiKeyFile);
    }
}

internal sealed class LlamaServerProcessStartRequest
{
    internal LlamaServerProcessStartRequest(
        string executablePath,
        string workingDirectory,
        IReadOnlyList<string> arguments)
    {
        ExecutablePath = executablePath;
        WorkingDirectory = workingDirectory;
        Arguments = arguments;
    }

    internal string ExecutablePath { get; }
    internal string WorkingDirectory { get; }
    internal IReadOnlyList<string> Arguments { get; }

    public override string ToString() => nameof(LlamaServerProcessStartRequest);
}

internal interface ILlamaServerProcess : IAsyncDisposable
{
    int ProcessId { get; }
    bool HasExited { get; }
    void KillEntireProcessTree();
    ValueTask WaitForExitAsync(CancellationToken cancellationToken);
}

internal interface ILlamaArtifactLease : IAsyncDisposable
{
    bool IsValid { get; }
    void RegisterInvalidationCallback(Action callback);
}

internal interface ILlamaServerPlatform : IDisposable
{
    ValueTask<ILlamaArtifactLease?> AcquireVerifiedArtifactAsync(
        LocalInferenceArtifact artifact,
        CancellationToken cancellationToken);

    ValueTask<ILlamaArtifactLease?> AcquireVerifiedRuntimeBundleAsync(
        LocalInferenceArtifact runtime,
        IReadOnlyList<LocalInferenceArtifact> dependencies,
        CancellationToken cancellationToken);

    int AllocateLoopbackPort();
    ILlamaServerProcess Start(LlamaServerProcessStartRequest request);

    ValueTask<bool> IsHealthyAsync(
        Uri endpoint,
        CancellationToken cancellationToken);

    bool IsLoopbackPortOwnedByProcess(int port, int processId);

    IDisposable RegisterTrustedEndpoint(int port, int processId);

    ValueTask DelayAsync(TimeSpan delay, CancellationToken cancellationToken);
}

internal sealed class SystemLlamaServerPlatform : ILlamaServerPlatform
{
    private static readonly TimeSpan HealthRequestTimeout = TimeSpan.FromSeconds(2);
    private readonly HttpClient _httpClient;

    internal SystemLlamaServerPlatform()
        : this(CreateHttpHandler())
    {
    }

    internal SystemLlamaServerPlatform(HttpMessageHandler handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        _httpClient = new HttpClient(handler, disposeHandler: true)
        {
            Timeout = HealthRequestTimeout,
        };
    }

    public async ValueTask<ILlamaArtifactLease?> AcquireVerifiedArtifactAsync(
        LocalInferenceArtifact artifact,
        CancellationToken cancellationToken)
    {
        var directoryHandles = new List<SafeFileHandle>(1);
        try
        {
            var directory = Path.GetDirectoryName(artifact.FullPath);
            if (string.IsNullOrWhiteSpace(directory)) return null;
            directoryHandles.Add(OpenLockedDirectory(directory));
            var streams = await AcquireVerifiedFilesAsync(
                [artifact],
                allowEmpty: false,
                cancellationToken).ConfigureAwait(false);
            if (streams is null) return null;
            var lease = new SystemLlamaArtifactLease(streams, directoryHandles);
            directoryHandles = [];
            return lease;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (exception is
            IOException or
            UnauthorizedAccessException or
            NotSupportedException or
            ArgumentException)
        {
            return null;
        }
        finally
        {
            foreach (var handle in directoryHandles) handle.Dispose();
        }
    }

    public async ValueTask<ILlamaArtifactLease?> AcquireVerifiedRuntimeBundleAsync(
        LocalInferenceArtifact runtime,
        IReadOnlyList<LocalInferenceArtifact> dependencies,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var directoryHandles = new List<SafeFileHandle>();
        RuntimeTreeMutationMonitor? mutationMonitor = null;
        try
        {
            if (runtime.ExpectedSize <= 0 ||
                dependencies.Count > LocalInferenceSettingsLimits.MaximumRuntimeDependencies)
            {
                return null;
            }
            var runtimeDirectory = Path.GetDirectoryName(runtime.FullPath);
            if (string.IsNullOrWhiteSpace(runtimeDirectory)) return null;
            runtimeDirectory = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(runtimeDirectory));
            var runtimePath = Path.GetFullPath(runtime.FullPath);
            mutationMonitor = new RuntimeTreeMutationMonitor(runtimeDirectory);

            var artifacts = new LocalInferenceArtifact[dependencies.Count + 1];
            artifacts[0] = runtime;
            for (var index = 0; index < dependencies.Count; ++index)
                artifacts[index + 1] = dependencies[index];

            var expectedPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var artifact in artifacts)
            {
                var path = Path.GetFullPath(artifact.FullPath);
                if ((!string.Equals(path, runtimePath, StringComparison.OrdinalIgnoreCase) &&
                     !IsStrictDescendant(path, runtimeDirectory)) ||
                    !expectedPaths.Add(path))
                {
                    return null;
                }
            }

            var actualPaths = EnumerateAndLockRuntimeTree(
                runtimeDirectory,
                directoryHandles,
                expectedPaths.Count);
            if (!expectedPaths.SetEquals(actualPaths)) return null;

            var streams = await AcquireVerifiedFilesAsync(
                artifacts,
                allowEmpty: true,
                cancellationToken).ConfigureAwait(false);
            if (streams is null) return null;
            if (!mutationMonitor.IsValid)
            {
                foreach (var stream in streams) await stream.DisposeAsync().ConfigureAwait(false);
                return null;
            }
            var lease = new SystemLlamaArtifactLease(
                streams,
                directoryHandles,
                mutationMonitor);
            directoryHandles = [];
            mutationMonitor = null;
            return lease;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (exception is
            IOException or
            UnauthorizedAccessException or
            NotSupportedException or
            ArgumentException or
            CryptographicException)
        {
            return null;
        }
        finally
        {
            foreach (var handle in directoryHandles) handle.Dispose();
            mutationMonitor?.Dispose();
        }
    }

    private static async ValueTask<IReadOnlyList<FileStream>?> AcquireVerifiedFilesAsync(
        IReadOnlyList<LocalInferenceArtifact> artifacts,
        bool allowEmpty,
        CancellationToken cancellationToken)
    {
        var streams = new List<FileStream>(artifacts.Count);
        try
        {
            foreach (var artifact in artifacts)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if ((allowEmpty ? artifact.ExpectedSize < 0 : artifact.ExpectedSize <= 0) ||
                    artifact.Sha256 is not { Length: 64 })
                {
                    return null;
                }

                byte[] expectedHash;
                try
                {
                    expectedHash = Convert.FromHexString(artifact.Sha256);
                }
                catch (FormatException)
                {
                    return null;
                }
                if (expectedHash.Length != SHA256.HashSizeInBytes)
                {
                    CryptographicOperations.ZeroMemory(expectedHash);
                    return null;
                }

                var attributes = File.GetAttributes(artifact.FullPath);
                if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                {
                    CryptographicOperations.ZeroMemory(expectedHash);
                    return null;
                }

                var stream = new FileStream(
                    artifact.FullPath,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.Read,
                    bufferSize: 1024 * 1024,
                    FileOptions.Asynchronous | FileOptions.SequentialScan);
                streams.Add(stream);
                if (stream.Length != artifact.ExpectedSize)
                {
                    CryptographicOperations.ZeroMemory(expectedHash);
                    return null;
                }

                var actualHash = await SHA256.HashDataAsync(stream, cancellationToken)
                    .ConfigureAwait(false);
                try
                {
                    if (stream.Length != artifact.ExpectedSize ||
                        !CryptographicOperations.FixedTimeEquals(actualHash, expectedHash))
                    {
                        return null;
                    }
                }
                finally
                {
                    CryptographicOperations.ZeroMemory(actualHash);
                    CryptographicOperations.ZeroMemory(expectedHash);
                }
            }
            var acquiredStreams = streams;
            streams = [];
            return acquiredStreams;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (exception is
            IOException or
            UnauthorizedAccessException or
            NotSupportedException or
            ArgumentException or
            CryptographicException)
        {
            return null;
        }
        finally
        {
            foreach (var stream in streams) await stream.DisposeAsync().ConfigureAwait(false);
        }
    }

    private static HashSet<string> EnumerateAndLockRuntimeTree(
        string runtimeDirectory,
        ICollection<SafeFileHandle> directoryHandles,
        int maximumFiles)
    {
        var files = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Stack<string>();
        directoryHandles.Add(OpenLockedDirectory(runtimeDirectory));
        pending.Push(runtimeDirectory);
        while (pending.TryPop(out var directory))
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(
                directory,
                "*",
                SearchOption.TopDirectoryOnly))
            {
                var attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Unsafe runtime entry.");
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    if (directoryHandles.Count >
                        LocalInferenceSettingsLimits.MaximumRuntimeDependencies)
                    {
                        throw new IOException("The runtime tree contains too many directories.");
                    }
                    directoryHandles.Add(OpenLockedDirectory(entry));
                    pending.Push(entry);
                }
                else
                {
                    files.Add(Path.GetFullPath(entry));
                    if (files.Count > maximumFiles)
                        throw new IOException("The runtime tree contains unlisted files.");
                }
            }
        }
        return files;
    }

    private static SafeFileHandle OpenLockedDirectory(string path)
    {
        var handle = CreateFileW(
            Path.GetFullPath(path),
            desiredAccess: 0,
            FileShare.Read,
            securityAttributes: IntPtr.Zero,
            creationDisposition: OpenExisting,
            FlagsBackupSemantics | FlagsOpenReparsePoint,
            templateFile: IntPtr.Zero);
        if (handle.IsInvalid)
        {
            handle.Dispose();
            throw new IOException("The inference artifact directory could not be locked.");
        }
        if (!GetFileInformationByHandleEx(
                handle,
                FileInfoByHandleClass.FileAttributeTagInfo,
                out var information,
                (uint)Marshal.SizeOf<FileAttributeTagInfo>()) ||
            (information.FileAttributes & (uint)FileAttributes.Directory) == 0 ||
            (information.FileAttributes & (uint)FileAttributes.ReparsePoint) != 0)
        {
            handle.Dispose();
            throw new IOException("The inference artifact directory is unsafe.");
        }
        return handle;
    }

    private static bool IsStrictDescendant(string path, string directory)
    {
        var prefix = Path.TrimEndingDirectorySeparator(directory) +
            Path.DirectorySeparatorChar;
        return path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    public int AllocateLoopbackPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        try
        {
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    public ILlamaServerProcess Start(LlamaServerProcessStartRequest request)
    {
        var process = new Process
        {
            StartInfo = CreateStartInfo(request),
            EnableRaisingEvents = true,
        };
        try
        {
            if (!process.Start())
                throw new InvalidOperationException("The local runtime did not start.");
            return new SystemLlamaServerProcess(process);
        }
        catch
        {
            process.Dispose();
            throw;
        }
    }

    public async ValueTask<bool> IsHealthyAsync(
        Uri endpoint,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!endpoint.IsLoopback || endpoint.Scheme != Uri.UriSchemeHttp)
            return false;

        using var request = new HttpRequestMessage(
            HttpMethod.Get,
            new Uri(endpoint, "health"));
        try
        {
            using var response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
            return response.IsSuccessStatusCode;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception) when (exception is
            HttpRequestException or
            IOException or
            OperationCanceledException)
        {
            return false;
        }
    }

    public ValueTask DelayAsync(TimeSpan delay, CancellationToken cancellationToken) =>
        new(Task.Delay(delay, cancellationToken));

    public bool IsLoopbackPortOwnedByProcess(int port, int processId)
    {
        if (port is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort || processId <= 0)
            return false;

        if (!TryGetTcpRows(TcpTableClass.TcpTableOwnerPidListener, out var rows))
            return false;
        var foundExpectedProcess = false;
        foreach (var row in rows)
        {
            if (DecodePort(row.LocalPort) != port ||
                row.LocalAddress != LoopbackIpv4Address)
            {
                continue;
            }
            if (row.OwningPid != (uint)processId) return false;
            foundExpectedProcess = true;
        }
        return foundExpectedProcess;
    }

    public IDisposable RegisterTrustedEndpoint(int port, int processId) =>
        VerifiedLoopbackProcessRegistry.Register(port, processId);

    internal static bool IsLoopbackConnectionOwnedByProcess(
        int serverPort,
        int clientPort,
        int processId)
    {
        if (serverPort is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort ||
            clientPort is <= IPEndPoint.MinPort or > IPEndPoint.MaxPort ||
            processId <= 0 ||
            !TryGetTcpRows(TcpTableClass.TcpTableOwnerPidAll, out var rows))
        {
            return false;
        }
        return rows.Any(row =>
            row.State == TcpStateEstablished &&
            row.LocalAddress == LoopbackIpv4Address &&
            row.RemoteAddress == LoopbackIpv4Address &&
            DecodePort(row.LocalPort) == serverPort &&
            DecodePort(row.RemotePort) == clientPort &&
            row.OwningPid == (uint)processId);
    }

    private static bool TryGetTcpRows(
        TcpTableClass tableClass,
        out IReadOnlyList<MibTcpRowOwnerPid> rows)
    {
        rows = Array.Empty<MibTcpRowOwnerPid>();

        var bufferSize = 0;
        var result = GetExtendedTcpTable(
            IntPtr.Zero,
            ref bufferSize,
            order: false,
            AddressFamilyInterNetwork,
            tableClass,
            reserved: 0);
        if (result != ErrorInsufficientBuffer || bufferSize <= sizeof(uint))
            return false;

        var buffer = Marshal.AllocHGlobal(bufferSize);
        try
        {
            result = GetExtendedTcpTable(
                buffer,
                ref bufferSize,
                order: false,
                AddressFamilyInterNetwork,
                tableClass,
                reserved: 0);
            if (result != ErrorSuccess) return false;

            var count = Marshal.ReadInt32(buffer);
            if (count < 0) return false;
            var results = new MibTcpRowOwnerPid[count];
            var rowSize = Marshal.SizeOf<MibTcpRowOwnerPid>();
            var rowAddress = IntPtr.Add(buffer, sizeof(uint));
            for (var index = 0; index < count; ++index)
            {
                results[index] = Marshal.PtrToStructure<MibTcpRowOwnerPid>(rowAddress);
                rowAddress = IntPtr.Add(rowAddress, rowSize);
            }
            rows = results;
            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static int DecodePort(uint encodedPort) =>
        (int)(((encodedPort & 0xffU) << 8) | ((encodedPort >> 8) & 0xffU));

    public void Dispose() => _httpClient.Dispose();

    internal static ProcessStartInfo CreateStartInfo(
        LlamaServerProcessStartRequest request)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = request.ExecutablePath,
            WorkingDirectory = request.WorkingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = false,
        };
        startInfo.Environment.Clear();
        var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        var systemDirectory = Environment.SystemDirectory;
        var temporaryDirectory = Path.GetTempPath();
        startInfo.Environment["SystemRoot"] = windowsDirectory;
        startInfo.Environment["WINDIR"] = windowsDirectory;
        startInfo.Environment["TEMP"] = temporaryDirectory;
        startInfo.Environment["TMP"] = temporaryDirectory;
        startInfo.Environment["PATH"] = systemDirectory;
        startInfo.Environment["PATHEXT"] = ".COM;.EXE;.BAT;.CMD";
        startInfo.Environment["PROCESSOR_ARCHITECTURE"] = "AMD64";
        startInfo.Environment["NUMBER_OF_PROCESSORS"] =
            Environment.ProcessorCount.ToString(CultureInfo.InvariantCulture);
        foreach (var argument in request.Arguments)
            startInfo.ArgumentList.Add(argument);
        return startInfo;
    }

    internal static SocketsHttpHandler CreateHttpHandler() => new()
    {
        UseProxy = false,
        AllowAutoRedirect = false,
        UseCookies = false,
    };

    private sealed class SystemLlamaArtifactLease(
        IReadOnlyList<FileStream> streams,
        IReadOnlyList<SafeFileHandle> directoryHandles,
        RuntimeTreeMutationMonitor? mutationMonitor = null) : ILlamaArtifactLease
    {
        private int _disposed;

        public bool IsValid => Volatile.Read(ref _disposed) == 0 &&
            (mutationMonitor?.IsValid ?? true);

        public void RegisterInvalidationCallback(Action callback)
        {
            ArgumentNullException.ThrowIfNull(callback);
            mutationMonitor?.RegisterInvalidationCallback(callback);
        }

        public async ValueTask DisposeAsync()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            for (var index = streams.Count - 1; index >= 0; --index)
                await streams[index].DisposeAsync().ConfigureAwait(false);
            for (var index = directoryHandles.Count - 1; index >= 0; --index)
                directoryHandles[index].Dispose();
            mutationMonitor?.Dispose();
        }

        public override string ToString() => nameof(SystemLlamaArtifactLease);
    }

    private sealed class RuntimeTreeMutationMonitor : IDisposable
    {
        private readonly FileSystemWatcher _watcher;
        private readonly object _callbackLock = new();
        private Action? _callback;
        private int _invalid;
        private int _disposed;

        internal RuntimeTreeMutationMonitor(string runtimeDirectory)
        {
            _watcher = new FileSystemWatcher(runtimeDirectory)
            {
                IncludeSubdirectories = true,
                NotifyFilter = NotifyFilters.FileName |
                    NotifyFilters.DirectoryName |
                    NotifyFilters.Attributes |
                    NotifyFilters.Size |
                    NotifyFilters.LastWrite |
                    NotifyFilters.Security,
                InternalBufferSize = 16 * 1024,
            };
            _watcher.Changed += OnChanged;
            _watcher.Created += OnChanged;
            _watcher.Deleted += OnChanged;
            _watcher.Renamed += OnRenamed;
            _watcher.Error += OnError;
            _watcher.EnableRaisingEvents = true;
        }

        internal bool IsValid => Volatile.Read(ref _invalid) == 0 &&
            Volatile.Read(ref _disposed) == 0;

        internal void RegisterInvalidationCallback(Action callback)
        {
            var invoke = false;
            lock (_callbackLock)
            {
                _callback += callback;
                invoke = !IsValid;
            }
            if (invoke) InvokeSafely(callback);
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _watcher.Dispose();
            lock (_callbackLock) _callback = null;
        }

        private void OnChanged(object sender, FileSystemEventArgs args) => Invalidate();
        private void OnRenamed(object sender, RenamedEventArgs args) => Invalidate();
        private void OnError(object sender, ErrorEventArgs args) => Invalidate();

        private void Invalidate()
        {
            if (Interlocked.Exchange(ref _invalid, 1) != 0) return;
            Action? callback;
            lock (_callbackLock) callback = _callback;
            if (callback is not null) InvokeSafely(callback);
        }

        private static void InvokeSafely(Action callback)
        {
            try
            {
                callback();
            }
            catch (Exception)
            {
                // File-system callbacks never expose artifact or process details.
            }
        }
    }

    private const uint OpenExisting = 3;
    private const uint ErrorSuccess = 0;
    private const uint ErrorInsufficientBuffer = 122;
    private const int AddressFamilyInterNetwork = 2;
    private const uint TcpStateEstablished = 5;
    private const uint LoopbackIpv4Address = 0x0100007f;
    private const uint FlagsOpenReparsePoint = 0x00200000;
    private const uint FlagsBackupSemantics = 0x02000000;

    private enum FileInfoByHandleClass
    {
        FileAttributeTagInfo = 9,
    }

    private enum TcpTableClass
    {
        TcpTableOwnerPidListener = 3,
        TcpTableOwnerPidAll = 5,
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileAttributeTagInfo
    {
        internal uint FileAttributes;
        internal uint ReparseTag;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MibTcpRowOwnerPid
    {
        internal uint State;
        internal uint LocalAddress;
        internal uint LocalPort;
        internal uint RemoteAddress;
        internal uint RemotePort;
        internal uint OwningPid;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string fileName,
        uint desiredAccess,
        FileShare shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandleEx(
        SafeFileHandle file,
        FileInfoByHandleClass fileInformationClass,
        out FileAttributeTagInfo fileInformation,
        uint bufferSize);

    [DllImport("iphlpapi.dll", SetLastError = true)]
    private static extern uint GetExtendedTcpTable(
        IntPtr tcpTable,
        ref int size,
        [MarshalAs(UnmanagedType.Bool)] bool order,
        int addressFamily,
        TcpTableClass tableClass,
        uint reserved);

    private sealed class SystemLlamaServerProcess : ILlamaServerProcess
    {
        private readonly Process _process;
        private readonly CancellationTokenSource _drainCancellation = new();
        private readonly Task _standardOutputDrain;
        private readonly Task _standardErrorDrain;
        private int _disposed;

        internal SystemLlamaServerProcess(Process process)
        {
            _process = process;
            _standardOutputDrain = DrainAsync(
                process.StandardOutput,
                _drainCancellation.Token);
            _standardErrorDrain = DrainAsync(
                process.StandardError,
                _drainCancellation.Token);
        }

        public int ProcessId => _process.Id;
        public bool HasExited => _process.HasExited;

        public void KillEntireProcessTree() => _process.Kill(entireProcessTree: true);

        public async ValueTask WaitForExitAsync(CancellationToken cancellationToken) =>
            await _process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        public async ValueTask DisposeAsync()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _drainCancellation.Cancel();
            try
            {
                await Task.WhenAll(_standardOutputDrain, _standardErrorDrain)
                    .WaitAsync(TimeSpan.FromMilliseconds(250)).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Output is discarded and never included in logs or exceptions.
            }
            _process.Dispose();
            _drainCancellation.Dispose();
        }

        private static async Task DrainAsync(
            StreamReader reader,
            CancellationToken cancellationToken)
        {
            var buffer = new char[4096];
            try
            {
                while (await reader.ReadAsync(buffer, cancellationToken).ConfigureAwait(false) > 0)
                {
                    // Intentionally discard output. The fixed buffer bounds memory use.
                }
            }
            catch (Exception exception) when (exception is
                IOException or
                ObjectDisposedException or
                OperationCanceledException)
            {
                // Process output is neither retained nor logged.
            }
        }
    }
}
