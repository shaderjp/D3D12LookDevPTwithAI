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
            Assert.Equal(Path.GetFullPath(relativeDirectory), paths.ArtifactDirectory);
            Assert.Equal(
                Path.Combine(Path.GetFullPath(relativeDirectory), "chat-history.sqlite3"),
                paths.DatabasePath);
            Assert.Equal(
                Path.Combine(Path.GetFullPath(relativeDirectory), "Models"),
                paths.ModelsDirectory);
            Assert.Equal(
                Path.Combine(Path.GetFullPath(relativeDirectory), "Runtimes"),
                paths.RuntimesDirectory);
            Assert.Equal(
                Path.Combine(Path.GetFullPath(relativeDirectory), "inference.json"),
                paths.InferenceSettingsPath);
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
    public void Explicit_artifact_directory_is_separate_from_writable_history()
    {
        var writableDirectory = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        var artifactDirectory = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        var paths = new AppPaths(writableDirectory, artifactDirectory);

        Assert.Equal(writableDirectory, paths.DataDirectory);
        Assert.Equal(artifactDirectory, paths.ArtifactDirectory);
        Assert.Equal(Path.Combine(writableDirectory, "chat-history.sqlite3"), paths.DatabasePath);
        Assert.Equal(Path.Combine(artifactDirectory, "inference.json"), paths.InferenceSettingsPath);
    }

    [Fact]
    public void Artifact_environment_override_does_not_move_writable_history()
    {
        var artifactDirectory = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));

        WithEnvironment(AppPaths.DataDirectoryEnvironmentVariable, null, () =>
            WithEnvironment(AppPaths.ArtifactDirectoryEnvironmentVariable, artifactDirectory, () =>
            {
                var paths = new AppPaths();

                Assert.Equal(Path.GetFullPath(artifactDirectory), paths.ArtifactDirectory);
                Assert.NotEqual(paths.ArtifactDirectory, paths.DataDirectory);
                Assert.Equal(Path.Combine(paths.DataDirectory, "chat-history.sqlite3"), paths.DatabasePath);
            }));
    }

    [Fact]
    public void Bundled_configuration_is_discovered_without_moving_writable_history()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-BundledPathsTests",
            Guid.NewGuid().ToString("N"));
        var artifactDirectory = Path.Combine(root, "AI");
        Directory.CreateDirectory(artifactDirectory);
        File.WriteAllText(Path.Combine(artifactDirectory, "inference.json"), "{}");
        try
        {
            WithEnvironment(AppPaths.DataDirectoryEnvironmentVariable, null, () =>
                WithEnvironment(AppPaths.ArtifactDirectoryEnvironmentVariable, null, () =>
                {
                    var paths = new AppPaths(applicationDirectory: root);

                    Assert.Equal(Path.GetFullPath(artifactDirectory), paths.ArtifactDirectory);
                    Assert.NotEqual(paths.ArtifactDirectory, paths.DataDirectory);
                }));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Portable_manifest_locks_bundled_artifacts_over_environment_override()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-PortablePathsTests",
            Guid.NewGuid().ToString("N"));
        var bundledDirectory = Path.Combine(root, "AI");
        var environmentDirectory = Path.Combine(root, "AmbientArtifacts");
        var writableDirectory = Path.Combine(root, "AmbientWritable");
        Directory.CreateDirectory(bundledDirectory);
        File.WriteAllText(Path.Combine(bundledDirectory, "inference.json"), "{}");
        File.WriteAllText(Path.Combine(root, "integrated-portable-manifest.json"), "{}");
        try
        {
            WithEnvironment(AppPaths.DataDirectoryEnvironmentVariable, writableDirectory, () =>
                WithEnvironment(AppPaths.ArtifactDirectoryEnvironmentVariable, environmentDirectory, () =>
                {
                    var paths = new AppPaths(applicationDirectory: root);

                    Assert.Equal(
                        Path.Combine(
                            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                            "D3D12LookDevPTwithAI",
                            "AI"),
                        paths.DataDirectory);
                    Assert.Equal(Path.GetFullPath(bundledDirectory), paths.ArtifactDirectory);
                }));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Portable_manifest_keeps_explicit_constructor_history_root_for_tests()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-PortableExplicitPathsTests",
            Guid.NewGuid().ToString("N"));
        var bundledDirectory = Path.Combine(root, "AI");
        var writableDirectory = Path.Combine(root, "ExplicitWritable");
        Directory.CreateDirectory(bundledDirectory);
        File.WriteAllText(Path.Combine(bundledDirectory, "inference.json"), "{}");
        File.WriteAllText(Path.Combine(root, "integrated-portable-manifest.json"), "{}");
        try
        {
            var paths = new AppPaths(
                dataDirectory: writableDirectory,
                applicationDirectory: root);

            Assert.Equal(Path.GetFullPath(writableDirectory), paths.DataDirectory);
            Assert.Equal(Path.GetFullPath(bundledDirectory), paths.ArtifactDirectory);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Portable_without_ai_does_not_activate_ambient_artifacts()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-PortableNoAiPathsTests",
            Guid.NewGuid().ToString("N"));
        var bundledDirectory = Path.Combine(root, "AI");
        var environmentDirectory = Path.Combine(root, "AmbientArtifacts");
        Directory.CreateDirectory(root);
        File.WriteAllText(Path.Combine(root, "integrated-portable-manifest.json"), "{}");
        try
        {
            WithEnvironment(AppPaths.ArtifactDirectoryEnvironmentVariable, environmentDirectory, () =>
            {
                var paths = new AppPaths(applicationDirectory: root);

                Assert.Equal(Path.GetFullPath(bundledDirectory), paths.ArtifactDirectory);
                Assert.False(File.Exists(paths.InferenceSettingsPath));
            });
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
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

    [Fact]
    public void Ensure_created_does_not_write_to_a_separate_artifact_directory()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-SeparatePathsTests",
            Guid.NewGuid().ToString("N"));
        var dataDirectory = Path.Combine(root, "Writable");
        var artifactDirectory = Path.Combine(root, "ReadOnlyArtifacts");
        var paths = new AppPaths(dataDirectory, artifactDirectory);
        try
        {
            paths.EnsureCreated();

            Assert.True(Directory.Exists(dataDirectory));
            Assert.False(Directory.Exists(artifactDirectory));
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Ensure_created_creates_integrated_ai_artifact_directories()
    {
        var dataDirectory = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-AppPathsTests",
            Guid.NewGuid().ToString("N"));
        var paths = new AppPaths(dataDirectory);
        try
        {
            paths.EnsureCreated();

            Assert.True(Directory.Exists(paths.DataDirectory));
            Assert.True(Directory.Exists(paths.ModelsDirectory));
            Assert.True(Directory.Exists(paths.RuntimesDirectory));
            Assert.False(File.Exists(paths.InferenceSettingsPath));
        }
        finally
        {
            if (Directory.Exists(dataDirectory))
                Directory.Delete(dataDirectory, recursive: true);
        }
    }

    [Fact]
    public void Equivalent_roots_with_trailing_separator_remain_a_combined_root()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "D3D12LookDevPTwithAI-EquivalentPathsTests",
            Guid.NewGuid().ToString("N"));
        var paths = new AppPaths(root, root + Path.DirectorySeparatorChar);
        try
        {
            paths.EnsureCreated();

            Assert.Equal(paths.DataDirectory, paths.ArtifactDirectory);
            Assert.True(Directory.Exists(paths.ModelsDirectory));
            Assert.True(Directory.Exists(paths.RuntimesDirectory));
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    private static void WithDataDirectoryEnvironment(string value, Action action)
    {
        WithEnvironment(AppPaths.DataDirectoryEnvironmentVariable, value, action);
    }

    private static void WithEnvironment(string name, string? value, Action action)
    {
        var previous = Environment.GetEnvironmentVariable(name);
        try
        {
            Environment.SetEnvironmentVariable(name, value);
            action();
        }
        finally
        {
            Environment.SetEnvironmentVariable(name, previous);
        }
    }
}

[CollectionDefinition("Process environment", DisableParallelization = true)]
public sealed class ProcessEnvironmentCollection;
