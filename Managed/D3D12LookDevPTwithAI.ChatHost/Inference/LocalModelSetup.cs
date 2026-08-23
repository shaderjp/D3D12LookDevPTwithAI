using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.ChatHost.Inference;

public interface ILocalModelSetupService : IDisposable
{
    Task InstallAsync(
        ModelSetupStartRequest request,
        Func<ModelSetupProgressEvent, CancellationToken, Task> reportAsync,
        CancellationToken cancellationToken);
}

public sealed class LocalModelSetupCoordinator(
    ILocalModelSetupService setupService) : IAsyncDisposable
{
    private readonly object _gate = new();
    private CancellationTokenSource? _cancellation;
    private Task? _operation;

    public bool TryStart(
        ModelSetupStartRequest request,
        IPipePeer peer,
        Guid requestId)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(peer);
        ValidateRequest(request);
        lock (_gate)
        {
            if (_operation is { IsCompleted: false }) return false;
            _cancellation?.Dispose();
            _cancellation = new CancellationTokenSource();
            _operation = RunAsync(request, peer, requestId, _cancellation.Token);
            return true;
        }
    }

    public bool Cancel()
    {
        lock (_gate)
        {
            if (_operation is not { IsCompleted: false } || _cancellation is null)
                return false;
            _cancellation.Cancel();
            return true;
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        Task? operation;
        lock (_gate)
        {
            _cancellation?.Cancel();
            operation = _operation;
        }
        if (operation is null) return;
        try
        {
            await operation.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private async Task RunAsync(
        ModelSetupStartRequest request,
        IPipePeer peer,
        Guid requestId,
        CancellationToken cancellationToken)
    {
        ModelSetupProgressEvent? latest = null;
        try
        {
            await setupService.InstallAsync(
                request,
                async (progress, token) =>
                {
                    latest = progress;
                    await peer.SendEventAsync(
                        requestId,
                        "modelSetupProgress",
                        progress,
                        token).ConfigureAwait(false);
                },
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await TryReportTerminalAsync(
                peer,
                requestId,
                CreateTerminalProgress(
                    latest,
                    "cancelled",
                    "Setup cancelled. The partial download was kept and can be resumed.",
                    succeeded: false)).ConfigureAwait(false);
        }
        catch (ModelSetupException exception)
        {
            await TryReportTerminalAsync(
                peer,
                requestId,
                CreateTerminalProgress(
                    latest,
                    "failed",
                    exception.SafeMessage,
                    succeeded: false)).ConfigureAwait(false);
        }
        catch
        {
            await TryReportTerminalAsync(
                peer,
                requestId,
                CreateTerminalProgress(
                    latest,
                    "failed",
                    "Local model setup failed.",
                    succeeded: false)).ConfigureAwait(false);
        }
    }

    private static ModelSetupProgressEvent CreateTerminalProgress(
        ModelSetupProgressEvent? latest,
        string stage,
        string message,
        bool succeeded) => new(
            stage,
            latest?.Artifact ?? "",
            latest?.BytesReceived ?? 0,
            latest?.TotalBytes ?? 0,
            latest?.OverallBytesReceived ?? 0,
            latest?.OverallTotalBytes ?? 0,
            latest?.Percent ?? 0,
            message,
            Terminal: true,
            Succeeded: succeeded);

    private static async Task TryReportTerminalAsync(
        IPipePeer peer,
        Guid requestId,
        ModelSetupProgressEvent progress)
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
            await peer.SendEventAsync(
                requestId,
                "modelSetupProgress",
                progress,
                timeout.Token).ConfigureAwait(false);
        }
        catch
        {
            // A disconnected UI cannot receive terminal setup state.
        }
    }

    private static void ValidateRequest(ModelSetupStartRequest request)
    {
        if (!request.LicenseAccepted)
        {
            throw new ChatRequestException(
                "model_license_not_accepted",
                "Review and accept the model and llama.cpp licenses before downloading.");
        }
        if (string.IsNullOrWhiteSpace(request.ModelId) ||
            string.IsNullOrWhiteSpace(request.Backend) ||
            !ModelSetupCatalog.Models.ContainsKey(request.ModelId) ||
            request.Backend is not ("cpu" or "cuda" or "vulkan"))
        {
            throw new ChatRequestException(
                "model_setup_selection_invalid",
                "The selected local model or inference backend is not supported.");
        }
    }

    public async ValueTask DisposeAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        await StopAsync(timeout.Token).ConfigureAwait(false);
        lock (_gate)
        {
            _cancellation?.Dispose();
            _cancellation = null;
        }
        setupService.Dispose();
    }
}

internal sealed class LocalModelSetupService : ILocalModelSetupService
{
    private const int BufferSize = 256 * 1024;
    private const string RuntimeVersion = "b10205";
    private readonly IAppPaths paths;
    private readonly IReadOnlyDictionary<string, SetupModel> _models;
    private readonly IReadOnlyList<SetupArtifact> _runtimeArtifacts;
    private readonly HttpClient _httpClient;

    internal LocalModelSetupService(IAppPaths paths)
        : this(
            paths,
            ModelSetupCatalog.Models,
            ModelSetupCatalog.RuntimeArtifacts,
            CreateHttpHandler(),
            disposeHandler: true)
    {
    }

    internal LocalModelSetupService(
        IAppPaths paths,
        IReadOnlyDictionary<string, SetupModel> models,
        IReadOnlyList<SetupArtifact> runtimeArtifacts,
        HttpMessageHandler httpHandler)
        : this(paths, models, runtimeArtifacts, httpHandler, disposeHandler: false)
    {
    }

    private LocalModelSetupService(
        IAppPaths paths,
        IReadOnlyDictionary<string, SetupModel> models,
        IReadOnlyList<SetupArtifact> runtimeArtifacts,
        HttpMessageHandler httpHandler,
        bool disposeHandler)
    {
        ArgumentNullException.ThrowIfNull(paths);
        ArgumentNullException.ThrowIfNull(models);
        ArgumentNullException.ThrowIfNull(runtimeArtifacts);
        ArgumentNullException.ThrowIfNull(httpHandler);
        this.paths = paths;
        _models = models;
        _runtimeArtifacts = runtimeArtifacts;
        _httpClient = new HttpClient(httpHandler, disposeHandler)
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
    }

    public async Task InstallAsync(
        ModelSetupStartRequest request,
        Func<ModelSetupProgressEvent, CancellationToken, Task> reportAsync,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(reportAsync);
        if (!paths.ArtifactDirectory.Equals(
                paths.DataDirectory,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new ModelSetupException(
                "This portable package uses fixed bundled AI artifacts and cannot replace them in the app.");
        }

        var model = _models[request.ModelId];
        var runtimes = _runtimeArtifacts
            .Where(artifact => artifact.Backend == request.Backend)
            .OrderByDescending(artifact => artifact.IsDependency)
            .ToArray();
        if (runtimes.Length == 0)
            throw new ModelSetupException("The selected inference backend is unavailable.");

        EnsureSafeDirectories();
        var overallTotal = checked(model.Artifact.Size + runtimes.Sum(item => item.Size));
        EnsureFreeSpace(overallTotal);
        var progress = new SetupProgressSink(reportAsync, overallTotal);
        await progress.ReportAsync(
            "preparing", "", 0, 0, 0,
            "Preparing local model storage…", true, cancellationToken).ConfigureAwait(false);

        var completedBytes = 0L;
        string runtimeDirectory;
        var installedRuntime = FindInstalledRuntimeDirectory(request.Backend, runtimes);
        if (installedRuntime is not null)
        {
            completedBytes = runtimes.Sum(item => item.Size);
            runtimeDirectory = installedRuntime;
            await progress.ReportAsync(
                "verify", "llama.cpp runtime", completedBytes, completedBytes,
                completedBytes,
                "Verifying the installed llama.cpp runtime…", true,
                cancellationToken).ConfigureAwait(false);
        }
        else
        {
            var archives = new List<(SetupArtifact Artifact, string Path)>();
            foreach (var artifact in runtimes)
            {
                var archive = await DownloadAndVerifyAsync(
                    artifact,
                    destinationPath: null,
                    completedBytes,
                    progress,
                    cancellationToken).ConfigureAwait(false);
                archives.Add((artifact, archive));
                completedBytes = checked(completedBytes + artifact.Size);
            }
            runtimeDirectory = await InstallRuntimeAsync(
                request.Backend,
                runtimes,
                archives,
                completedBytes,
                progress,
                cancellationToken).ConfigureAwait(false);
            foreach (var archive in archives)
            {
                TryDeleteFile(archive.Path);
            }
        }

        var modelDirectory = Path.Combine(paths.ModelsDirectory, model.Id);
        Directory.CreateDirectory(modelDirectory);
        var modelPath = Path.Combine(modelDirectory, model.Artifact.FileName);
        await DownloadAndVerifyAsync(
            model.Artifact,
            modelPath,
            completedBytes,
            progress,
            cancellationToken).ConfigureAwait(false);
        completedBytes = checked(completedBytes + model.Artifact.Size);
        await ValidateGgufAsync(modelPath, cancellationToken).ConfigureAwait(false);

        await progress.ReportAsync(
            "configure", model.DisplayName,
            model.Artifact.Size, model.Artifact.Size,
            completedBytes,
            "Creating the verified local inference configuration…", true,
            cancellationToken).ConfigureAwait(false);
        var runtimeManifest = await BuildRuntimeManifestAsync(
            runtimeDirectory,
            request.Backend,
            completedBytes,
            progress,
            cancellationToken).ConfigureAwait(false);
        await WriteConfigurationAsync(
            model,
            modelPath,
            request.Backend,
            runtimeManifest,
            cancellationToken).ConfigureAwait(false);
        await WriteLicenseAcceptanceAsync(model, cancellationToken).ConfigureAwait(false);

        await progress.ReportAsync(
            "completed", model.DisplayName,
            model.Artifact.Size, model.Artifact.Size,
            overallTotal,
            "Setup complete. The local model is ready to load.", true,
            cancellationToken,
            terminal: true,
            succeeded: true).ConfigureAwait(false);
    }

    private async Task<string> DownloadAndVerifyAsync(
        SetupArtifact artifact,
        string? destinationPath,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        if (destinationPath is not null && File.Exists(destinationPath))
        {
            if (await VerifyFileAsync(
                    destinationPath,
                    artifact,
                    completedBytes,
                    progress,
                    cancellationToken).ConfigureAwait(false))
            {
                return destinationPath;
            }
            var quarantine = destinationPath + ".bad-" +
                DateTimeOffset.UtcNow.ToString("yyyyMMddHHmmss");
            File.Move(destinationPath, quarantine, overwrite: false);
        }

        var partialPath = Path.Combine(
            DownloadsDirectory,
            artifact.Id + ".partial");
        await DownloadWithRetriesAsync(
            artifact,
            partialPath,
            completedBytes,
            progress,
            cancellationToken).ConfigureAwait(false);
        if (!await VerifyFileAsync(
                partialPath,
                artifact,
                completedBytes,
                progress,
                cancellationToken).ConfigureAwait(false))
        {
            var quarantine = partialPath + ".bad-" +
                DateTimeOffset.UtcNow.ToString("yyyyMMddHHmmss");
            File.Move(partialPath, quarantine, overwrite: false);
            throw new ModelSetupException(
                "The downloaded artifact failed SHA-256 verification. The partial file was quarantined.");
        }

        if (destinationPath is null) return partialPath;
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        File.Move(partialPath, destinationPath, overwrite: true);
        return destinationPath;
    }

    private async Task DownloadWithRetriesAsync(
        SetupArtifact artifact,
        string partialPath,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        const int maximumAttempts = 4;
        for (var attempt = 1; ; ++attempt)
        {
            try
            {
                await DownloadOnceAsync(
                    artifact,
                    partialPath,
                    completedBytes,
                    progress,
                    cancellationToken).ConfigureAwait(false);
                return;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception) when (
                attempt < maximumAttempts &&
                exception is HttpRequestException or IOException or TaskCanceledException)
            {
                var received = File.Exists(partialPath)
                    ? new FileInfo(partialPath).Length : 0;
                await progress.ReportAsync(
                    "retrying", artifact.DisplayName,
                    received, artifact.Size,
                    checked(completedBytes + Math.Min(received, artifact.Size)),
                    $"Connection interrupted. Retrying ({attempt}/{maximumAttempts - 1})…",
                    true,
                    cancellationToken).ConfigureAwait(false);
                await Task.Delay(TimeSpan.FromSeconds(attempt), cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception exception) when (
                exception is HttpRequestException or IOException or TaskCanceledException)
            {
                throw new ModelSetupException(
                    "The download could not be completed. Check the network connection and available storage.");
            }
        }
    }

    private async Task DownloadOnceAsync(
        SetupArtifact artifact,
        string partialPath,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        var existingLength = File.Exists(partialPath)
            ? new FileInfo(partialPath).Length : 0L;
        if (existingLength < 0 || existingLength > artifact.Size)
        {
            TryDeleteFile(partialPath);
            existingLength = 0;
        }
        using var request = new HttpRequestMessage(HttpMethod.Get, artifact.DownloadUri);
        if (existingLength > 0)
            request.Headers.Range = new RangeHeaderValue(existingLength, null);

        using var response = await _httpClient.SendAsync(
            request,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken).ConfigureAwait(false);
        var finalUri = response.RequestMessage?.RequestUri;
        if (finalUri is null ||
            !finalUri.Scheme.Equals(Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
        {
            throw new HttpRequestException(
                "The artifact download was redirected to an unsafe endpoint.");
        }
        if (existingLength > 0 &&
            response.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable &&
            existingLength == artifact.Size)
        {
            return;
        }
        if (existingLength > 0 && response.StatusCode == HttpStatusCode.OK)
        {
            TryDeleteFile(partialPath);
            existingLength = 0;
        }
        else if (existingLength > 0 &&
                 response.StatusCode == HttpStatusCode.PartialContent &&
                 response.Content.Headers.ContentRange?.From != existingLength)
        {
            TryDeleteFile(partialPath);
            throw new IOException("The server returned a mismatched download range.");
        }
        response.EnsureSuccessStatusCode();
        var responseTotal = response.Content.Headers.ContentLength is { } length
            ? checked(length + existingLength)
            : artifact.Size;
        if (responseTotal != artifact.Size)
            throw new IOException("The download size does not match the fixed artifact catalog.");

        await progress.ReportAsync(
            "downloading", artifact.DisplayName,
            existingLength, artifact.Size,
            checked(completedBytes + existingLength),
            "Downloading…", true,
            cancellationToken).ConfigureAwait(false);
        await using var input = await response.Content
            .ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        await using var output = new FileStream(
            partialPath,
            FileMode.Append,
            FileAccess.Write,
            FileShare.Read,
            BufferSize,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var buffer = new byte[BufferSize];
        var received = existingLength;
        while (true)
        {
            using var inactivity = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken);
            inactivity.CancelAfter(TimeSpan.FromSeconds(60));
            int read;
            try
            {
                read = await input.ReadAsync(buffer, inactivity.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException exception) when (
                !cancellationToken.IsCancellationRequested)
            {
                throw new IOException(
                    "No download data was received for 60 seconds.", exception);
            }
            if (read == 0) break;
            await output.WriteAsync(
                buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
            received = checked(received + read);
            if (received > artifact.Size)
                throw new IOException("The download exceeded the fixed artifact size.");
            await progress.ReportAsync(
                "downloading", artifact.DisplayName,
                received, artifact.Size,
                checked(completedBytes + received),
                "Downloading…", false,
                cancellationToken).ConfigureAwait(false);
        }
        await output.FlushAsync(cancellationToken).ConfigureAwait(false);
        if (received != artifact.Size)
            throw new IOException("The downloaded artifact is incomplete.");
    }

    private static async Task<bool> VerifyFileAsync(
        string path,
        SetupArtifact artifact,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        FileInfo file;
        try
        {
            file = new FileInfo(path);
            if (!file.Exists || file.Length != artifact.Size ||
                (file.Attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            {
                return false;
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return false;
        }

        await progress.ReportAsync(
            "verify", artifact.DisplayName,
            0, artifact.Size, checked(completedBytes + artifact.Size),
            "Verifying SHA-256…", true,
            cancellationToken).ConfigureAwait(false);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        await using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read,
            BufferSize,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var buffer = new byte[BufferSize];
        var verified = 0L;
        while (true)
        {
            var read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read == 0) break;
            hash.AppendData(buffer, 0, read);
            verified = checked(verified + read);
            await progress.ReportAsync(
                "verify", artifact.DisplayName,
                verified, artifact.Size, checked(completedBytes + artifact.Size),
                "Verifying SHA-256…", false,
                cancellationToken).ConfigureAwait(false);
        }
        var actual = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
        var valid = verified == artifact.Size &&
            actual.Equals(artifact.Sha256, StringComparison.Ordinal);
        CryptographicOperations.ZeroMemory(buffer);
        return valid;
    }

    private async Task<string> InstallRuntimeAsync(
        string backend,
        IReadOnlyList<SetupArtifact> expectedArtifacts,
        IReadOnlyList<(SetupArtifact Artifact, string Path)> archives,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        var backendRoot = Path.Combine(paths.RuntimesDirectory, backend);
        Directory.CreateDirectory(backendRoot);
        var staging = Path.Combine(
            backendRoot,
            ".setup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        try
        {
            foreach (var archive in archives)
            {
                await progress.ReportAsync(
                    "extract", archive.Artifact.DisplayName,
                    archive.Artifact.Size, archive.Artifact.Size,
                    completedBytes,
                    "Extracting the verified runtime…", true,
                    cancellationToken).ConfigureAwait(false);
                await ExtractZipSafelyAsync(
                    archive.Path, staging, cancellationToken).ConfigureAwait(false);
            }
            var marker = Path.Combine(staging, "setup-source.json");
            await File.WriteAllTextAsync(
                marker,
                RuntimeMarker(expectedArtifacts),
                new UTF8Encoding(false),
                cancellationToken).ConfigureAwait(false);
            var executableCount = Directory.EnumerateFiles(
                staging,
                "llama-server.exe",
                SearchOption.AllDirectories).Count();
            if (executableCount != 1)
                throw new ModelSetupException(
                    "The verified runtime archive does not contain exactly one llama-server.exe.");
            var destination = Path.Combine(
                backendRoot,
                $"catalog-{RuntimeVersion}-{Guid.NewGuid():N}");
            Directory.Move(staging, destination);
            return destination;
        }
        finally
        {
            if (Directory.Exists(staging))
            {
                try { Directory.Delete(staging, recursive: true); }
                catch { }
            }
        }
    }

    private string? FindInstalledRuntimeDirectory(
        string backend,
        IReadOnlyList<SetupArtifact> expectedArtifacts)
    {
        var backendRoot = Path.Combine(paths.RuntimesDirectory, backend);
        if (!Directory.Exists(backendRoot)) return null;
        var markerText = RuntimeMarker(expectedArtifacts);
        try
        {
            foreach (var directory in Directory.EnumerateDirectories(
                         backendRoot,
                         $"catalog-{RuntimeVersion}-*",
                         SearchOption.TopDirectoryOnly))
            {
                var attributes = File.GetAttributes(directory);
                if ((attributes & FileAttributes.ReparsePoint) != 0) continue;
                var marker = Path.Combine(directory, "setup-source.json");
                if (!File.Exists(marker) ||
                    !File.ReadAllText(marker).Equals(markerText, StringComparison.Ordinal))
                {
                    continue;
                }
                if (Directory.EnumerateFiles(
                        directory,
                        "llama-server.exe",
                        SearchOption.AllDirectories).Count() == 1)
                {
                    return directory;
                }
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
        }
        return null;
    }

    private static string RuntimeMarker(IReadOnlyList<SetupArtifact> artifacts) =>
        JsonSerializer.Serialize(
            artifacts.Select(item => new
            {
                item.Id,
                item.Sha256,
                item.Size,
            }),
            PipeJson.SerializerOptions);

    private async Task<RuntimeManifest> BuildRuntimeManifestAsync(
        string installedDirectory,
        string backend,
        long completedBytes,
        SetupProgressSink progress,
        CancellationToken cancellationToken)
    {
        var executables = Directory.EnumerateFiles(
            installedDirectory,
            "llama-server.exe",
            SearchOption.AllDirectories).ToArray();
        if (executables.Length != 1)
            throw new ModelSetupException("The installed llama.cpp runtime is incomplete.");
        var executable = Path.GetFullPath(executables[0]);
        var runtimeDirectory = Path.GetDirectoryName(executable)!;
        EnsureNoReparsePoint(runtimeDirectory);
        var files = Directory.EnumerateFiles(
                runtimeDirectory, "*", SearchOption.AllDirectories)
            .Select(Path.GetFullPath)
            .OrderBy(path => Path.GetRelativePath(runtimeDirectory, path), StringComparer.Ordinal)
            .ToArray();
        if (files.Length == 0 ||
            files.Length > LocalInferenceSettingsLimits.MaximumRuntimeDependencies + 1)
        {
            throw new ModelSetupException("The installed llama.cpp runtime file set is invalid.");
        }

        var documents = new List<ArtifactDocument>(files.Length);
        foreach (var file in files)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var attributes = File.GetAttributes(file);
            if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                throw new ModelSetupException("The installed llama.cpp runtime contains an unsafe entry.");
            var relativeToArtifacts = ForwardRelative(paths.RuntimesDirectory, file);
            var length = new FileInfo(file).Length;
            await progress.ReportAsync(
                "verify", "llama.cpp runtime",
                0, Math.Max(length, 1), completedBytes,
                "Verifying installed runtime files…", false,
                cancellationToken).ConfigureAwait(false);
            var sha256 = await ComputeSha256Async(file, cancellationToken).ConfigureAwait(false);
            documents.Add(new ArtifactDocument(relativeToArtifacts, sha256, length));
        }
        var runtime = documents.Single(document =>
            Path.GetFullPath(Path.Combine(
                paths.RuntimesDirectory,
                document.RelativePath.Replace('/', Path.DirectorySeparatorChar)))
            .Equals(executable, StringComparison.OrdinalIgnoreCase));
        var dependencies = documents
            .Where(document => !ReferenceEquals(document, runtime) &&
                !document.RelativePath.Equals(runtime.RelativePath, StringComparison.OrdinalIgnoreCase))
            .OrderBy(document => document.RelativePath, StringComparer.Ordinal)
            .ToArray();
        return new RuntimeManifest(runtime, dependencies, backend);
    }

    private async Task WriteConfigurationAsync(
        SetupModel model,
        string modelPath,
        string backend,
        RuntimeManifest runtime,
        CancellationToken cancellationToken)
    {
        var document = new
        {
            schemaVersion = 1,
            modelId = model.Id,
            backend,
            contextSize = 8192,
            maxTokens = 2048,
            temperature = 0.7,
            model = new ArtifactDocument(
                ForwardRelative(paths.ModelsDirectory, modelPath),
                model.Artifact.Sha256,
                model.Artifact.Size),
            runtime = runtime.Runtime,
            runtimeDependencies = runtime.Dependencies,
        };
        var payload = JsonSerializer.SerializeToUtf8Bytes(
            document,
            PipeJson.SerializerOptions);
        if (payload.Length > LocalInferenceSettingsLimits.MaximumDocumentBytes)
            throw new ModelSetupException("The generated inference configuration is too large.");
        await WriteAtomicallyAsync(
            paths.InferenceSettingsPath,
            payload,
            cancellationToken).ConfigureAwait(false);
    }

    private async Task WriteLicenseAcceptanceAsync(
        SetupModel model,
        CancellationToken cancellationToken)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(
            new
            {
                schemaVersion = 1,
                modelId = model.Id,
                modelRevision = model.Revision,
                licenseId = "gemma-4-apache-2.0@2026-04-01",
                llamaCppLicense = "MIT",
                acceptedAt = DateTimeOffset.UtcNow,
            },
            PipeJson.SerializerOptions);
        await WriteAtomicallyAsync(
            Path.Combine(paths.DataDirectory, "model-setup-acceptance.json"),
            payload,
            cancellationToken).ConfigureAwait(false);
    }

    private static async Task WriteAtomicallyAsync(
        string path,
        byte[] payload,
        CancellationToken cancellationToken)
    {
        var directory = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(directory);
        var staging = Path.Combine(
            directory,
            $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            await using (var stream = new FileStream(
                staging,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                4096,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await stream.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                stream.Flush(flushToDisk: true);
            }
            File.Move(staging, path, overwrite: true);
        }
        finally
        {
            TryDeleteFile(staging);
        }
    }

    private void EnsureSafeDirectories()
    {
        paths.EnsureCreated();
        Directory.CreateDirectory(DownloadsDirectory);
        Directory.CreateDirectory(paths.ModelsDirectory);
        Directory.CreateDirectory(paths.RuntimesDirectory);
        EnsureNoReparsePoint(paths.DataDirectory);
        EnsureNoReparsePoint(DownloadsDirectory);
        EnsureNoReparsePoint(paths.ModelsDirectory);
        EnsureNoReparsePoint(paths.RuntimesDirectory);
    }

    private string DownloadsDirectory => Path.Combine(paths.DataDirectory, "Downloads");

    private void EnsureFreeSpace(long downloadBytes)
    {
        try
        {
            var root = Path.GetPathRoot(paths.DataDirectory) ?? paths.DataDirectory;
            var required = checked((long)(downloadBytes * 1.2));
            if (new DriveInfo(root).AvailableFreeSpace < required)
            {
                throw new ModelSetupException(
                    $"Not enough free space. Approximately {required / 1024d / 1024d / 1024d:F1} GiB is required.");
            }
        }
        catch (ModelSetupException)
        {
            throw;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw new ModelSetupException("Available storage could not be checked.");
        }
    }

    private static async Task ExtractZipSafelyAsync(
        string archivePath,
        string destination,
        CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(archivePath);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Read);
        var destinationRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(destination)) + Path.DirectorySeparatorChar;
        foreach (var entry in archive.Entries)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var unixType = (entry.ExternalAttributes >> 16) & 0xF000;
            if (unixType == 0xA000)
                throw new ModelSetupException("The runtime archive contains an unsafe link.");
            var target = Path.GetFullPath(Path.Combine(
                destination,
                entry.FullName.Replace('/', Path.DirectorySeparatorChar)));
            if (!target.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                throw new ModelSetupException("The runtime archive contains an unsafe path.");
            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(target);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            await using var input = entry.Open();
            await using var output = new FileStream(
                target,
                FileMode.Create,
                FileAccess.Write,
                FileShare.None,
                BufferSize,
                FileOptions.Asynchronous);
            await input.CopyToAsync(output, cancellationToken).ConfigureAwait(false);
        }
    }

    private static async Task ValidateGgufAsync(
        string path,
        CancellationToken cancellationToken)
    {
        var header = new byte[8];
        await using var stream = File.OpenRead(path);
        await stream.ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);
        if (!header.AsSpan(0, 4).SequenceEqual("GGUF"u8) ||
            BitConverter.ToUInt32(header, 4) is < 2 or > 3)
        {
            throw new ModelSetupException("The downloaded model does not have a supported GGUF header.");
        }
    }

    private static async Task<string> ComputeSha256Async(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read,
            BufferSize,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        var hash = await SHA256.HashDataAsync(stream, cancellationToken).ConfigureAwait(false);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static string ForwardRelative(string root, string path)
    {
        var relative = Path.GetRelativePath(root, path);
        if (relative.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(relative))
            throw new ModelSetupException("A generated artifact path escaped local model storage.");
        return relative.Replace(Path.DirectorySeparatorChar, '/');
    }

    private static void EnsureNoReparsePoint(string directory)
    {
        var current = Path.GetFullPath(directory);
        while (!string.IsNullOrEmpty(current))
        {
            if (Directory.Exists(current) &&
                (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new ModelSetupException("Local model storage contains an unsafe link.");
            }
            var parent = Path.GetDirectoryName(current);
            if (parent is null || parent.Equals(current, StringComparison.OrdinalIgnoreCase)) break;
            current = parent;
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch
        {
        }
    }

    private static SocketsHttpHandler CreateHttpHandler() => new()
    {
        AllowAutoRedirect = true,
        AutomaticDecompression = DecompressionMethods.All,
        ConnectTimeout = TimeSpan.FromSeconds(20),
        UseCookies = false,
        UseProxy = true,
    };

    public void Dispose() => _httpClient.Dispose();

    private sealed record ArtifactDocument(
        string RelativePath,
        string Sha256,
        long ExpectedSize);

    private sealed record RuntimeManifest(
        ArtifactDocument Runtime,
        IReadOnlyList<ArtifactDocument> Dependencies,
        string Backend);
}

internal sealed class SetupProgressSink(
    Func<ModelSetupProgressEvent, CancellationToken, Task> reportAsync,
    long overallTotal)
{
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private long _lastReportMilliseconds = long.MinValue;
    private long _lastOverallBytes = -1;
    private string _lastStage = string.Empty;

    public async Task ReportAsync(
        string stage,
        string artifact,
        long bytesReceived,
        long totalBytes,
        long overallBytesReceived,
        string message,
        bool force,
        CancellationToken cancellationToken,
        bool terminal = false,
        bool succeeded = false)
    {
        var now = _clock.ElapsedMilliseconds;
        var boundedOverall = Math.Clamp(overallBytesReceived, 0, overallTotal);
        if (!force &&
            stage == _lastStage &&
            now - _lastReportMilliseconds < 200 &&
            boundedOverall - _lastOverallBytes < 4 * 1024 * 1024)
        {
            return;
        }
        _lastStage = stage;
        _lastReportMilliseconds = now;
        _lastOverallBytes = boundedOverall;
        var percent = overallTotal > 0
            ? boundedOverall * 100d / overallTotal
            : 0;
        await reportAsync(
            new ModelSetupProgressEvent(
                stage,
                artifact,
                Math.Max(0, bytesReceived),
                Math.Max(0, totalBytes),
                boundedOverall,
                overallTotal,
                Math.Clamp(percent, 0, 100),
                message,
                terminal,
                succeeded),
            cancellationToken).ConfigureAwait(false);
    }
}

internal sealed class ModelSetupException(string safeMessage) : Exception(safeMessage)
{
    public string SafeMessage { get; } = safeMessage;
}

internal sealed record SetupArtifact(
    string Id,
    string DisplayName,
    string FileName,
    Uri DownloadUri,
    long Size,
    string Sha256,
    string? Backend = null,
    bool IsDependency = false);

internal sealed record SetupModel(
    string Id,
    string DisplayName,
    string Revision,
    string LicenseUrl,
    SetupArtifact Artifact);

internal static class ModelSetupCatalog
{
    internal static IReadOnlyDictionary<string, SetupModel> Models { get; } =
        new Dictionary<string, SetupModel>(StringComparer.Ordinal)
        {
            ["gemma-4-e2b-it-q4"] = new(
                "gemma-4-e2b-it-q4",
                "Gemma 4 E2B IT (Q4_0)",
                "675cff42a74c774d6cb76f76d8eacb49b48c9b93",
                "https://ai.google.dev/gemma/apache_2",
                new SetupArtifact(
                    "gemma-4-e2b-it-q4",
                    "Gemma 4 E2B IT (Q4_0)",
                    "gemma-4-E2B_q4_0-it.gguf",
                    new Uri("https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf/resolve/675cff42a74c774d6cb76f76d8eacb49b48c9b93/gemma-4-E2B_q4_0-it.gguf"),
                    3_349_516_256,
                    "fa401b55b07ee70a54c6dae3903c783a6e65064312529ea57175cb5f8dec6634")),
            ["gemma-4-e4b-it-q4"] = new(
                "gemma-4-e4b-it-q4",
                "Gemma 4 E4B IT (Q4_0)",
                "4b4a2c1d584be7264f87aac328a1bc739ce81b6c",
                "https://ai.google.dev/gemma/apache_2",
                new SetupArtifact(
                    "gemma-4-e4b-it-q4",
                    "Gemma 4 E4B IT (Q4_0)",
                    "gemma-4-E4B_q4_0-it.gguf",
                    new Uri("https://huggingface.co/google/gemma-4-E4B-it-qat-q4_0-gguf/resolve/4b4a2c1d584be7264f87aac328a1bc739ce81b6c/gemma-4-E4B_q4_0-it.gguf"),
                    5_154_941_280,
                    "676c35070db6dbe52f93e9c864ee0fba4eddea94b9c875d9cb10daff453fbaee")),
        };

    internal static IReadOnlyList<SetupArtifact> RuntimeArtifacts { get; } =
    [
        new(
            "llama-b10205-cpu",
            "llama.cpp b10205 CPU",
            "llama-b10205-bin-win-cpu-x64.zip",
            new Uri("https://github.com/ggml-org/llama.cpp/releases/download/b10205/llama-b10205-bin-win-cpu-x64.zip"),
            18_351_085,
            "b442f140a513e478e6bda26b0a769cfce18699c1c85f3d2df33a8637dcd5e14f",
            "cpu"),
        new(
            "llama-b10205-vulkan",
            "llama.cpp b10205 Vulkan",
            "llama-b10205-bin-win-vulkan-x64.zip",
            new Uri("https://github.com/ggml-org/llama.cpp/releases/download/b10205/llama-b10205-bin-win-vulkan-x64.zip"),
            33_648_680,
            "2df7c1567e87a41f5d55efdd3b70953518e96bc5604b159655320380584800ca",
            "vulkan"),
        new(
            "llama-b10205-cuda-runtime",
            "CUDA 12.4 runtime for llama.cpp b10205",
            "cudart-llama-bin-win-cuda-12.4-x64.zip",
            new Uri("https://github.com/ggml-org/llama.cpp/releases/download/b10205/cudart-llama-bin-win-cuda-12.4-x64.zip"),
            39_144_362,
            "8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6",
            "cuda",
            true),
        new(
            "llama-b10205-cuda",
            "llama.cpp b10205 CUDA 12.4",
            "llama-b10205-bin-win-cuda-12.4-x64.zip",
            new Uri("https://github.com/ggml-org/llama.cpp/releases/download/b10205/llama-b10205-bin-win-cuda-12.4-x64.zip"),
            25_098_515,
            "830d50bedb4dbb21e619ec7883fd7356f9b92c803a99c4d56deef000322ce787",
            "cuda"),
    ];
}
