namespace D3D12LookDevPTwithAI.Chat.Core;

public static class LocalInferenceSettingsLimits
{
    public const int MaximumDocumentBytes = 64 * 1024;
    public const int MaximumModelIdCharacters = 64;
    public const int MaximumRelativePathCharacters = 1024;
    public const int MaximumRuntimeDependencies = 512;
    public const int MinimumContextSize = 512;
    public const int MaximumContextSize = 131_072;
    public const int MinimumMaxTokens = 64;
    public const int MaximumMaxTokens = 32_768;
    public const double MinimumTemperature = 0;
    public const double MaximumTemperature = 2;
}

public enum LocalInferenceBackend
{
    Cpu,
    Cuda,
    Vulkan,
}

public sealed record LocalInferenceArtifact(
    string RelativePath,
    string FullPath,
    string Sha256,
    long ExpectedSize);

public sealed record LocalInferenceSettings(
    int SchemaVersion,
    string ModelId,
    LocalInferenceBackend Backend,
    int ContextSize,
    int MaxTokens,
    double Temperature,
    LocalInferenceArtifact Model,
    LocalInferenceArtifact Runtime,
    IReadOnlyList<LocalInferenceArtifact> RuntimeDependencies);

public interface ILocalInferenceSettingsProvider
{
    // A missing file is the normal first-run state. Invalid or unsafe content
    // is surfaced as LocalInferenceSettingsException rather than silently
    // falling back to a different model or runtime.
    ValueTask<LocalInferenceSettings?> LoadAsync(
        CancellationToken cancellationToken = default);
}

public sealed class LocalInferenceSettingsException(
    string code,
    string message,
    Exception? innerException = null) : Exception(message, innerException)
{
    public string Code { get; } = code;
}
