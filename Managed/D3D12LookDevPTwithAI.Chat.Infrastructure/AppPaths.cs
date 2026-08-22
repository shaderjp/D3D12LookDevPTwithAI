using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.Chat.Infrastructure;

public sealed class AppPaths : IAppPaths
{
    private const string PortableManifestFileName =
        "integrated-portable-manifest.json";
    public const string DataDirectoryEnvironmentVariable =
        "D3D12LOOKDEVPT_AI_DATA_DIRECTORY";
    public const string ArtifactDirectoryEnvironmentVariable =
        "D3D12LOOKDEVPT_AI_ARTIFACT_DIRECTORY";

    public AppPaths(
        string? dataDirectory = null,
        string? artifactDirectory = null,
        string? applicationDirectory = null)
    {
        var baseDirectory = NormalizeDirectory(
            applicationDirectory ?? AppContext.BaseDirectory);
        var bundledDirectory = Path.Combine(baseDirectory, "AI");
        var hasBundledConfiguration =
            File.Exists(Path.Combine(bundledDirectory, "inference.json"));
        var isPortablePackage =
            File.Exists(Path.Combine(baseDirectory, PortableManifestFileName));
        var hasExplicitDataDirectory = dataDirectory is not null;
        if (dataDirectory is not null)
        {
            if (string.IsNullOrWhiteSpace(dataDirectory))
                throw new ArgumentException("The AI data directory must not be empty.", nameof(dataDirectory));
            DataDirectory = NormalizeDirectory(dataDirectory);
        }
        else
        {
            var environmentDirectory = Environment.GetEnvironmentVariable(
                DataDirectoryEnvironmentVariable);
            hasExplicitDataDirectory = !isPortablePackage &&
                !string.IsNullOrWhiteSpace(environmentDirectory);
            DataDirectory = hasExplicitDataDirectory
                ? NormalizeDirectory(environmentDirectory!)
                : DefaultDataDirectory();
        }

        ArtifactDirectory = ResolveArtifactDirectory(
            artifactDirectory,
            bundledDirectory,
            hasBundledConfiguration,
            isPortablePackage,
            hasExplicitDataDirectory);
    }

    public string DataDirectory { get; }
    public string DatabasePath => Path.Combine(DataDirectory, "chat-history.sqlite3");
    public string ArtifactDirectory { get; }
    public string ModelsDirectory => Path.Combine(ArtifactDirectory, "Models");
    public string RuntimesDirectory => Path.Combine(ArtifactDirectory, "Runtimes");
    public string InferenceSettingsPath => Path.Combine(ArtifactDirectory, "inference.json");

    public void EnsureCreated()
    {
        Directory.CreateDirectory(DataDirectory);
        if (ArtifactDirectory.Equals(
                DataDirectory,
                StringComparison.OrdinalIgnoreCase))
        {
            Directory.CreateDirectory(ModelsDirectory);
            Directory.CreateDirectory(RuntimesDirectory);
        }
    }

    private string ResolveArtifactDirectory(
        string? artifactDirectory,
        string bundledDirectory,
        bool hasBundledConfiguration,
        bool isPortablePackage,
        bool hasExplicitDataDirectory)
    {
        if (artifactDirectory is not null)
        {
            if (string.IsNullOrWhiteSpace(artifactDirectory))
            {
                throw new ArgumentException(
                    "The AI artifact directory must not be empty.",
                    nameof(artifactDirectory));
            }
            return NormalizeDirectory(artifactDirectory);
        }

        // A portable manifest locks the child to the reviewed, app-adjacent
        // artifact payload. Ambient user/process environment must not silently
        // replace an exhibition package's model or runtime.
        if (isPortablePackage)
            return bundledDirectory;

        var environmentDirectory = Environment.GetEnvironmentVariable(
            ArtifactDirectoryEnvironmentVariable);
        if (!string.IsNullOrWhiteSpace(environmentDirectory))
            return NormalizeDirectory(environmentDirectory);

        // Preserve the original data-directory override as a combined
        // development/test root. A portable package does not set it, so its
        // immutable AI payload can remain separate from writable chat history.
        if (hasExplicitDataDirectory)
            return DataDirectory;

        if (hasBundledConfiguration)
            return bundledDirectory;

        return DataDirectory;
    }

    private static string NormalizeDirectory(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private static string DefaultDataDirectory() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "D3D12LookDevPTwithAI",
        "AI");
}
