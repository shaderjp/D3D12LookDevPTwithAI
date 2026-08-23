using System.Text.Json;
using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class JsonLocalInferenceSettingsProviderTests
{
    private const string ValidSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    [Fact]
    public async Task Missing_document_is_the_normal_unconfigured_state()
    {
        using var fixture = new TemporarySettingsDirectory();

        var settings = await fixture.Provider.LoadAsync();

        Assert.Null(settings);
    }

    [Fact]
    public async Task Valid_document_resolves_artifacts_below_integrated_ai_roots()
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(
            modelPath: "gemma/gemma-4-E2B_q4_0-it.gguf",
            runtimePath: "cpu/b10205/llama-server.exe",
            modelSha256: ValidSha256.ToUpperInvariant()));

        var settings = Assert.IsType<LocalInferenceSettings>(
            await fixture.Provider.LoadAsync());

        Assert.Equal(1, settings.SchemaVersion);
        Assert.Equal("gemma-4-e2b-it-q4", settings.ModelId);
        Assert.Equal(LocalInferenceBackend.Cpu, settings.Backend);
        Assert.Equal(8192, settings.ContextSize);
        Assert.Equal(2048, settings.MaxTokens);
        Assert.Equal(0.25, settings.Temperature);
        Assert.Equal("gemma/gemma-4-E2B_q4_0-it.gguf", settings.Model.RelativePath);
        Assert.Equal(
            Path.GetFullPath(Path.Combine(
                fixture.Paths.ModelsDirectory,
                "gemma",
                "gemma-4-E2B_q4_0-it.gguf")),
            settings.Model.FullPath);
        Assert.Equal(ValidSha256, settings.Model.Sha256);
        Assert.Equal(3_349_516_256, settings.Model.ExpectedSize);
        Assert.Equal("cpu/b10205/llama-server.exe", settings.Runtime.RelativePath);
        Assert.Equal(
            Path.GetFullPath(Path.Combine(
                fixture.Paths.RuntimesDirectory,
                "cpu",
                "b10205",
                "llama-server.exe")),
            settings.Runtime.FullPath);
        Assert.Equal(18_351_085, settings.Runtime.ExpectedSize);
        var dependency = Assert.Single(settings.RuntimeDependencies);
        Assert.Equal("cpu/b10205/ggml.dll", dependency.RelativePath);
        Assert.Equal(0, dependency.ExpectedSize);
    }

    [Fact]
    public async Task Unknown_top_level_field_is_rejected()
    {
        using var fixture = new TemporarySettingsDirectory();
        var json = CreateJson();
        await fixture.WriteAsync(json[..^1] + ",\"unexpected\":true}");

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid_json", exception.Code);
    }

    [Fact]
    public async Task Unknown_artifact_field_is_rejected()
    {
        using var fixture = new TemporarySettingsDirectory();
        var json = CreateJson().Replace(
            "\"expectedSize\":3349516256",
            "\"expectedSize\":3349516256,\"unexpected\":true",
            StringComparison.Ordinal);
        await fixture.WriteAsync(json);

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid_json", exception.Code);
    }

    [Fact]
    public async Task Runtime_dependency_manifest_is_required_and_confined_to_the_runtime_bundle()
    {
        using var missingFixture = new TemporarySettingsDirectory();
        await missingFixture.WriteAsync(CreateJson().Replace(
            "\"runtimeDependencies\":",
            "\"renamedDependencies\":",
            StringComparison.Ordinal));
        var missingException = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => missingFixture.Provider.LoadAsync().AsTask());
        Assert.Equal("inference_settings_invalid_json", missingException.Code);

        using var outsideFixture = new TemporarySettingsDirectory();
        await outsideFixture.WriteAsync(CreateJson().Replace(
            "cpu/b10205/ggml.dll",
            "cpu/other/ggml.dll",
            StringComparison.Ordinal));
        var outsideException = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => outsideFixture.Provider.LoadAsync().AsTask());
        Assert.Equal("inference_settings_invalid", outsideException.Code);
    }

    [Fact]
    public async Task Document_over_64_KiB_is_rejected_before_JSON_parsing()
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(new string(' ',
            LocalInferenceSettingsLimits.MaximumDocumentBytes + 1));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_too_large", exception.Code);
    }

    [Fact]
    public async Task Document_at_exactly_64_KiB_is_accepted()
    {
        using var fixture = new TemporarySettingsDirectory();
        var json = CreateJson();
        Assert.True(json.Length < LocalInferenceSettingsLimits.MaximumDocumentBytes);
        await fixture.WriteAsync(json + new string(
            ' ',
            LocalInferenceSettingsLimits.MaximumDocumentBytes - json.Length));

        var settings = await fixture.Provider.LoadAsync();

        Assert.NotNull(settings);
    }

    [Theory]
    [InlineData("../outside.gguf")]
    [InlineData("folder/../../outside.gguf")]
    [InlineData("folder\\model.gguf")]
    [InlineData("/absolute.gguf")]
    [InlineData("folder//model.gguf")]
    [InlineData("folder/./model.gguf")]
    [InlineData("folder/model.gguf/")]
    [InlineData("folder/model.gguf:alternate-stream")]
    [InlineData("CON.gguf")]
    [InlineData(" folder/model.gguf")]
    [InlineData("folder /model.gguf")]
    public async Task Unsafe_model_relative_path_is_rejected(string relativePath)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(modelPath: relativePath));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_unsafe_path", exception.Code);
    }

    [Fact]
    public async Task Rooted_model_path_is_rejected()
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(
            modelPath: Path.GetFullPath(Path.Combine(fixture.Root, "outside.gguf"))));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_unsafe_path", exception.Code);
    }

    [Theory]
    [InlineData("model.bin", "cpu/b10205/llama-server.exe")]
    [InlineData("model.gguf", "cpu/b10205/server.exe")]
    public async Task Artifact_file_kind_is_enforced(
        string modelPath,
        string runtimePath)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(
            modelPath: modelPath,
            runtimePath: runtimePath));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Theory]
    [InlineData("auto")]
    [InlineData("CPU")]
    [InlineData("directml")]
    public async Task Backend_must_be_exactly_cpu_cuda_or_vulkan(string backend)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(backend: backend));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Theory]
    [InlineData("../model")]
    [InlineData("model id")]
    [InlineData(".model")]
    [InlineData("model-")]
    [InlineData("モデル")]
    public async Task Model_identifier_must_be_a_bounded_ASCII_token(string modelId)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(modelId: modelId));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Theory]
    [InlineData(511, 2048, 0.25)]
    [InlineData(131073, 2048, 0.25)]
    [InlineData(8192, 63, 0.25)]
    [InlineData(8192, 32769, 0.25)]
    [InlineData(8192, 2048, -0.01)]
    [InlineData(8192, 2048, 2.01)]
    public async Task Numeric_ranges_are_enforced(
        int contextSize,
        int maxTokens,
        double temperature)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(
            contextSize: contextSize,
            maxTokens: maxTokens,
            temperature: temperature));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Theory]
    [InlineData("")]
    [InlineData("abc")]
    [InlineData("gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg")]
    public async Task Artifact_hash_must_be_exactly_64_hex_characters(string sha256)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(modelSha256: sha256));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public async Task Artifact_expected_size_must_be_positive(long expectedSize)
    {
        using var fixture = new TemporarySettingsDirectory();
        await fixture.WriteAsync(CreateJson(modelExpectedSize: expectedSize));

        var exception = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_invalid", exception.Code);
    }

    [Fact]
    public async Task Existing_reparse_artifact_is_rejected_when_links_are_available()
    {
        using var fixture = new TemporarySettingsDirectory();
        var outsideModel = Path.Combine(fixture.Root, "outside.gguf");
        await File.WriteAllTextAsync(outsideModel, "fixture");
        var modelLink = Path.Combine(fixture.Paths.ModelsDirectory, "linked.gguf");
        try
        {
            File.CreateSymbolicLink(modelLink, outsideModel);
        }
        catch (Exception exception) when (exception is
            IOException or UnauthorizedAccessException or PlatformNotSupportedException)
        {
            return;
        }
        await fixture.WriteAsync(CreateJson(modelPath: "linked.gguf"));

        var settingsException = await Assert.ThrowsAsync<LocalInferenceSettingsException>(
            () => fixture.Provider.LoadAsync().AsTask());

        Assert.Equal("inference_settings_unsafe_path", settingsException.Code);
    }

    private static string CreateJson(
        string modelId = "gemma-4-e2b-it-q4",
        string backend = "cpu",
        int contextSize = 8192,
        int maxTokens = 2048,
        double temperature = 0.25,
        string modelPath = "gemma-4-E2B_q4_0-it.gguf",
        string runtimePath = "cpu/b10205/llama-server.exe",
        string modelSha256 = ValidSha256,
        long modelExpectedSize = 3_349_516_256) =>
        JsonSerializer.Serialize(new
        {
            schemaVersion = 1,
            modelId,
            backend,
            contextSize,
            maxTokens,
            temperature,
            model = new
            {
                relativePath = modelPath,
                sha256 = modelSha256,
                expectedSize = modelExpectedSize,
            },
            runtime = new
            {
                relativePath = runtimePath,
                sha256 = ValidSha256,
                expectedSize = 18_351_085,
            },
            runtimeDependencies = new[]
            {
                new
                {
                    relativePath = "cpu/b10205/ggml.dll",
                    sha256 = ValidSha256,
                    expectedSize = 0,
                },
            },
        });

    private sealed class TemporarySettingsDirectory : IDisposable
    {
        public TemporarySettingsDirectory()
        {
            Root = Path.GetFullPath(Path.Combine(
                Path.GetTempPath(),
                "D3D12LookDevPTwithAI-InferenceSettingsTests-" +
                Guid.NewGuid().ToString("N")));
            Paths = new AppPaths(Path.Combine(Root, "AI"));
            Paths.EnsureCreated();
            Provider = new JsonLocalInferenceSettingsProvider(Paths);
        }

        public string Root { get; }
        public AppPaths Paths { get; }
        public JsonLocalInferenceSettingsProvider Provider { get; }

        public Task WriteAsync(string content) =>
            File.WriteAllTextAsync(Paths.InferenceSettingsPath, content);

        public void Dispose()
        {
            var temporaryPrefix = Path.GetFullPath(Path.GetTempPath())
                .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (!Root.StartsWith(temporaryPrefix, StringComparison.OrdinalIgnoreCase) ||
                !Path.GetFileName(Root).StartsWith(
                    "D3D12LookDevPTwithAI-InferenceSettingsTests-",
                    StringComparison.Ordinal))
            {
                throw new InvalidOperationException(
                    "Refusing to clean up outside the inference settings test root.");
            }
            if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true);
        }
    }
}
