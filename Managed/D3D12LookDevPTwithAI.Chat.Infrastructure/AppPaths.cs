using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.Chat.Infrastructure;

public sealed class AppPaths : IAppPaths
{
    public const string DataDirectoryEnvironmentVariable =
        "D3D12LOOKDEVPT_AI_DATA_DIRECTORY";

    public AppPaths(string? dataDirectory = null)
    {
        if (dataDirectory is not null)
        {
            if (string.IsNullOrWhiteSpace(dataDirectory))
                throw new ArgumentException("The AI data directory must not be empty.", nameof(dataDirectory));
            DataDirectory = Path.GetFullPath(dataDirectory);
            return;
        }

        var environmentDirectory = Environment.GetEnvironmentVariable(
            DataDirectoryEnvironmentVariable);
        DataDirectory = !string.IsNullOrWhiteSpace(environmentDirectory)
            ? Path.GetFullPath(environmentDirectory)
            : Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "D3D12LookDevPTwithAI",
                "AI");
    }

    public string DataDirectory { get; }
    public string DatabasePath => Path.Combine(DataDirectory, "chat-history.sqlite3");
    public string ModelsDirectory => Path.Combine(DataDirectory, "Models");
    public string RuntimesDirectory => Path.Combine(DataDirectory, "Runtimes");
    public string InferenceSettingsPath => Path.Combine(DataDirectory, "inference.json");

    public void EnsureCreated()
    {
        Directory.CreateDirectory(DataDirectory);
        Directory.CreateDirectory(ModelsDirectory);
        Directory.CreateDirectory(RuntimesDirectory);
    }
}
