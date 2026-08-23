using System.Buffers.Binary;
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;
using D3D12LookDevPTwithAI.ChatHost.Inference;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class LocalModelSetupTests : IAsyncLifetime
{
    private readonly string _directory = Path.Combine(
        Path.GetTempPath(),
        "D3D12LookDevPTwithAI.Chat.Tests",
        Guid.NewGuid().ToString("N"));

    [Fact]
    public async Task Install_resumes_download_verifies_artifacts_and_publishes_configuration()
    {
        var modelUri = new Uri("https://downloads.example.test/tiny-model.gguf");
        var runtimeUri = new Uri("https://downloads.example.test/tiny-runtime.zip");
        var modelBytes = CreateGguf();
        var runtimeBytes = CreateRuntimeZip();
        var modelArtifact = new SetupArtifact(
            "tiny-model",
            "Tiny model",
            "tiny-model.gguf",
            modelUri,
            modelBytes.Length,
            Sha256(modelBytes));
        var model = new SetupModel(
            "tiny-model",
            "Tiny model",
            "test-revision",
            "https://licenses.example.test/model",
            modelArtifact);
        var runtime = new SetupArtifact(
            "tiny-runtime",
            "Tiny CPU runtime",
            "tiny-runtime.zip",
            runtimeUri,
            runtimeBytes.Length,
            Sha256(runtimeBytes),
            Backend: "cpu");
        var paths = new AppPaths(_directory);
        var partialDirectory = Path.Combine(paths.DataDirectory, "Downloads");
        Directory.CreateDirectory(partialDirectory);
        var resumedBytes = modelBytes.Length / 2;
        await File.WriteAllBytesAsync(
            Path.Combine(partialDirectory, "tiny-model.partial"),
            modelBytes[..resumedBytes]);
        var handler = new ArtifactHandler(new Dictionary<Uri, byte[]>
        {
            [modelUri] = modelBytes,
            [runtimeUri] = runtimeBytes,
        });
        using var setup = new LocalModelSetupService(
            paths,
            new Dictionary<string, SetupModel>(StringComparer.Ordinal)
            {
                [model.Id] = model,
            },
            [runtime],
            handler);
        var progress = new List<ModelSetupProgressEvent>();

        await setup.InstallAsync(
            new ModelSetupStartRequest(model.Id, "cpu", LicenseAccepted: true),
            (value, _) =>
            {
                progress.Add(value);
                return Task.CompletedTask;
            },
            CancellationToken.None);

        Assert.Contains(handler.Requests, request =>
            request.Uri == modelUri && request.RangeFrom == resumedBytes);
        var completed = Assert.Single(progress, value => value.Terminal);
        Assert.True(completed.Succeeded);
        Assert.Equal(100, completed.Percent);
        Assert.True(progress.Zip(progress.Skip(1), (left, right) =>
            right.Percent >= left.Percent).All(value => value));
        Assert.True(File.Exists(paths.InferenceSettingsPath));
        Assert.True(File.Exists(Path.Combine(
            paths.ModelsDirectory,
            model.Id,
            modelArtifact.FileName)));

        var settings = await new JsonLocalInferenceSettingsProvider(paths)
            .LoadAsync(CancellationToken.None);
        Assert.NotNull(settings);
        Assert.Equal(model.Id, settings.ModelId);
        Assert.Equal(LocalInferenceBackend.Cpu, settings.Backend);
        Assert.Equal(modelArtifact.Size, settings.Model.ExpectedSize);
        Assert.EndsWith("llama-server.exe", settings.Runtime.FullPath,
            StringComparison.OrdinalIgnoreCase);
        Assert.NotEmpty(settings.RuntimeDependencies);
    }

    public Task InitializeAsync() => Task.CompletedTask;

    public Task DisposeAsync()
    {
        if (Directory.Exists(_directory))
            Directory.Delete(_directory, recursive: true);
        return Task.CompletedTask;
    }

    private static byte[] CreateGguf()
    {
        var bytes = new byte[32];
        "GGUF"u8.CopyTo(bytes);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), 3);
        RandomNumberGenerator.Fill(bytes.AsSpan(8));
        return bytes;
    }

    private static byte[] CreateRuntimeZip()
    {
        using var output = new MemoryStream();
        using (var archive = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
        {
            WriteEntry(archive, "llama-server.exe", "MZ-test-runtime"u8);
            WriteEntry(archive, "ggml-test.dll", "test-dependency"u8);
        }
        return output.ToArray();
    }

    private static void WriteEntry(
        ZipArchive archive,
        string name,
        ReadOnlySpan<byte> payload)
    {
        var entry = archive.CreateEntry(name, CompressionLevel.NoCompression);
        using var stream = entry.Open();
        stream.Write(payload);
    }

    private static string Sha256(byte[] payload) =>
        Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();

    private sealed class ArtifactHandler(IReadOnlyDictionary<Uri, byte[]> artifacts) :
        HttpMessageHandler
    {
        public List<ArtifactRequest> Requests { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var uri = request.RequestUri ?? throw new HttpRequestException();
            if (!artifacts.TryGetValue(uri, out var artifact))
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
            var rangeFrom = request.Headers.Range?.Ranges.Single().From;
            var offset = checked((int)(rangeFrom ?? 0));
            Requests.Add(new ArtifactRequest(uri, rangeFrom));
            var response = new HttpResponseMessage(
                rangeFrom.HasValue ? HttpStatusCode.PartialContent : HttpStatusCode.OK)
            {
                RequestMessage = request,
                Content = new ByteArrayContent(artifact[offset..]),
            };
            response.Content.Headers.ContentLength = artifact.Length - offset;
            if (rangeFrom.HasValue)
            {
                response.Content.Headers.ContentRange = new ContentRangeHeaderValue(
                    offset,
                    artifact.Length - 1,
                    artifact.Length);
            }
            return Task.FromResult(response);
        }
    }

    private sealed record ArtifactRequest(Uri Uri, long? RangeFrom);
}
