using D3D12LookDevPTwithAI.Chat.Infrastructure;

namespace D3D12LookDevPTwithAI.Chat.Tests;

[Collection("Process environment")]
public sealed class AppPathsTests
{
    [Fact]
    public void Default_constructor_resolves_nonempty_data_directory_environment_override()
    {
        var relativeDirectory = Path.Combine(
            ".",
            "portable-chat-data",
            Guid.NewGuid().ToString("N"));

        WithDataDirectoryEnvironment(relativeDirectory, () =>
        {
            var paths = new AppPaths();

            Assert.Equal(Path.GetFullPath(relativeDirectory), paths.DataDirectory);
            Assert.Equal(
                Path.Combine(Path.GetFullPath(relativeDirectory), "chat-history.sqlite3"),
                paths.DatabasePath);
        });
    }

    [Fact]
    public void Explicit_data_directory_takes_precedence_over_environment_override()
    {
        var explicitDirectory = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));

        WithDataDirectoryEnvironment("ignored-portable-data", () =>
            Assert.Equal(explicitDirectory, new AppPaths(explicitDirectory).DataDirectory));
    }

    [Fact]
    public void Whitespace_environment_override_keeps_the_normal_local_app_data_default()
    {
        WithDataDirectoryEnvironment("   ", () =>
            Assert.Equal(
                Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "D3D12LookDevPTwithAI",
                    "AI"),
                new AppPaths().DataDirectory));
    }

    private static void WithDataDirectoryEnvironment(string value, Action action)
    {
        var previous = Environment.GetEnvironmentVariable(AppPaths.DataDirectoryEnvironmentVariable);
        try
        {
            Environment.SetEnvironmentVariable(AppPaths.DataDirectoryEnvironmentVariable, value);
            action();
        }
        finally
        {
            Environment.SetEnvironmentVariable(AppPaths.DataDirectoryEnvironmentVariable, previous);
        }
    }
}

[CollectionDefinition("Process environment", DisableParallelization = true)]
public sealed class ProcessEnvironmentCollection;
