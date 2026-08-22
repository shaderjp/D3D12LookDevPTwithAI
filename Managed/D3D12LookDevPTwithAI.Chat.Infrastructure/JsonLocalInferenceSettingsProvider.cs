using System.Text.Json;
using System.Text.Json.Serialization;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.Chat.Infrastructure;

public sealed class JsonLocalInferenceSettingsProvider(IAppPaths paths) :
    ILocalInferenceSettingsProvider
{
    private const int CurrentSchemaVersion = 1;
    private static readonly char[] InvalidWindowsFileNameCharacters =
        ['<', '>', ':', '"', '|', '?', '*', '\\'];
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        ReadCommentHandling = JsonCommentHandling.Disallow,
        AllowTrailingCommas = false,
        NumberHandling = JsonNumberHandling.Strict,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    public async ValueTask<LocalInferenceSettings?> LoadAsync(
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        FileAttributes settingsAttributes;
        try
        {
            settingsAttributes = File.GetAttributes(paths.InferenceSettingsPath);
        }
        catch (FileNotFoundException)
        {
            return null;
        }
        catch (DirectoryNotFoundException)
        {
            return null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw Unavailable(exception);
        }

        if ((settingsAttributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
            throw UnsafePath();

        EnsureExistingPathIsNotReparsePoint(paths.ArtifactDirectory);

        byte[] payload;
        try
        {
            payload = await ReadBoundedAsync(
                paths.InferenceSettingsPath,
                cancellationToken).ConfigureAwait(false);
        }
        catch (LocalInferenceSettingsException)
        {
            throw;
        }
        catch (FileNotFoundException)
        {
            return null;
        }
        catch (DirectoryNotFoundException)
        {
            return null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw Unavailable(exception);
        }

        SettingsDocument document;
        try
        {
            document = JsonSerializer.Deserialize<SettingsDocument>(
                payload,
                SerializerOptions) ?? throw new JsonException("The document is null.");
        }
        catch (Exception exception) when (exception is JsonException or NotSupportedException)
        {
            throw new LocalInferenceSettingsException(
                "inference_settings_invalid_json",
                "The local inference settings document is not valid schema v1 JSON.",
                exception);
        }

        return ValidateAndResolve(document);
    }

    private LocalInferenceSettings ValidateAndResolve(SettingsDocument document)
    {
        if (document.SchemaVersion != CurrentSchemaVersion)
            throw InvalidSettings("The local inference settings schema is unsupported.");
        if (!IsSafeToken(
                document.ModelId,
                LocalInferenceSettingsLimits.MaximumModelIdCharacters))
        {
            throw InvalidSettings("The configured model identifier is invalid.");
        }
        if (!TryParseBackend(document.Backend, out var backend))
            throw InvalidSettings("The configured inference backend is invalid.");
        if (document.ContextSize is < LocalInferenceSettingsLimits.MinimumContextSize or
            > LocalInferenceSettingsLimits.MaximumContextSize)
        {
            throw InvalidSettings("The configured context size is outside the supported range.");
        }
        if (document.MaxTokens is < LocalInferenceSettingsLimits.MinimumMaxTokens or
            > LocalInferenceSettingsLimits.MaximumMaxTokens)
        {
            throw InvalidSettings("The configured output token limit is outside the supported range.");
        }
        if (!double.IsFinite(document.Temperature) ||
            document.Temperature is < LocalInferenceSettingsLimits.MinimumTemperature or
            > LocalInferenceSettingsLimits.MaximumTemperature)
        {
            throw InvalidSettings("The configured temperature is outside the supported range.");
        }

        var artifactDirectory = NormalizeDirectory(paths.ArtifactDirectory);
        var modelsDirectory = NormalizeArtifactRoot(paths.ModelsDirectory, artifactDirectory);
        var runtimesDirectory = NormalizeArtifactRoot(paths.RuntimesDirectory, artifactDirectory);
        var model = ValidateArtifact(
            document.Model,
            modelsDirectory,
            ArtifactPathKind.Model);
        var runtime = ValidateArtifact(
            document.Runtime,
            runtimesDirectory,
            ArtifactPathKind.Runtime);
        if (document.RuntimeDependencies is null ||
            document.RuntimeDependencies.Count >
                LocalInferenceSettingsLimits.MaximumRuntimeDependencies)
        {
            throw InvalidSettings("The runtime dependency manifest is invalid.");
        }

        var runtimeDirectory = Path.GetDirectoryName(runtime.FullPath) ??
            throw InvalidSettings("The runtime artifact directory is invalid.");
        var dependencyPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            runtime.FullPath,
        };
        var runtimeDependencies = new LocalInferenceArtifact[
            document.RuntimeDependencies.Count];
        for (var index = 0; index < document.RuntimeDependencies.Count; ++index)
        {
            var dependency = ValidateArtifact(
                document.RuntimeDependencies[index],
                runtimesDirectory,
                ArtifactPathKind.RuntimeDependency);
            if (!IsStrictDescendant(dependency.FullPath, runtimeDirectory) ||
                !dependencyPaths.Add(dependency.FullPath) ||
                index > 0 && string.CompareOrdinal(
                    runtimeDependencies[index - 1].RelativePath,
                    dependency.RelativePath) >= 0)
            {
                throw InvalidSettings(
                    "The runtime dependency manifest contains an invalid path.");
            }
            runtimeDependencies[index] = dependency;
        }

        return new LocalInferenceSettings(
            CurrentSchemaVersion,
            document.ModelId,
            backend,
            document.ContextSize,
            document.MaxTokens,
            document.Temperature,
            model,
            runtime,
            runtimeDependencies);
    }

    private static LocalInferenceArtifact ValidateArtifact(
        ArtifactDocument? document,
        string rootDirectory,
        ArtifactPathKind kind)
    {
        if (document is null)
            throw InvalidSettings("A required local inference artifact is missing.");
        if (kind == ArtifactPathKind.RuntimeDependency
                ? document.ExpectedSize < 0
                : document.ExpectedSize <= 0)
            throw InvalidSettings("An inference artifact expected size is invalid.");
        if (!IsSha256(document.Sha256))
            throw InvalidSettings("An inference artifact SHA-256 is invalid.");

        var normalizedRelativePath = ValidateRelativePath(document.RelativePath);
        var fileName = normalizedRelativePath.Split('/')[^1];
        if (kind == ArtifactPathKind.Model &&
            !fileName.EndsWith(".gguf", StringComparison.OrdinalIgnoreCase))
        {
            throw InvalidSettings("The model artifact must be a GGUF file.");
        }
        if (kind == ArtifactPathKind.Runtime &&
            !fileName.Equals("llama-server.exe", StringComparison.OrdinalIgnoreCase))
        {
            throw InvalidSettings("The runtime artifact must be llama-server.exe.");
        }

        var fullPath = ResolveArtifactPath(
            rootDirectory,
            normalizedRelativePath);

        return new LocalInferenceArtifact(
            normalizedRelativePath,
            fullPath,
            document.Sha256!.ToLowerInvariant(),
            document.ExpectedSize);
    }

    private static string ResolveArtifactPath(
        string rootDirectory,
        string normalizedRelativePath)
    {
        try
        {
            var fullPath = Path.GetFullPath(Path.Combine(
                rootDirectory,
                normalizedRelativePath.Replace('/', Path.DirectorySeparatorChar)));
            if (!IsStrictDescendant(fullPath, rootDirectory))
                throw UnsafePath();
            EnsureNoExistingReparsePoints(rootDirectory, fullPath);
            return fullPath;
        }
        catch (LocalInferenceSettingsException)
        {
            throw;
        }
        catch (Exception exception) when (exception is
            ArgumentException or NotSupportedException or PathTooLongException)
        {
            throw new LocalInferenceSettingsException(
                "inference_settings_unsafe_path",
                "The local inference settings contain an unsafe artifact path.",
                exception);
        }
    }

    private static string ValidateRelativePath(string? relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath) ||
            relativePath.Length > LocalInferenceSettingsLimits.MaximumRelativePathCharacters ||
            relativePath.Contains('\\') ||
            relativePath.StartsWith('/') ||
            relativePath.EndsWith('/') ||
            Path.IsPathRooted(relativePath))
        {
            throw UnsafePath();
        }

        var segments = relativePath.Split('/');
        foreach (var segment in segments)
        {
            if (string.IsNullOrWhiteSpace(segment) ||
                segment is "." or ".." ||
                !segment.Equals(segment.Trim(), StringComparison.Ordinal) ||
                segment.EndsWith('.') ||
                segment.Any(char.IsControl) ||
                segment.IndexOfAny(InvalidWindowsFileNameCharacters) >= 0 ||
                IsReservedWindowsDeviceName(segment))
            {
                throw UnsafePath();
            }
        }
        return string.Join('/', segments);
    }

    private static string NormalizeArtifactRoot(
        string directory,
        string dataDirectory)
    {
        var normalized = NormalizeDirectory(directory);
        if (!IsStrictDescendant(normalized, dataDirectory))
            throw UnsafePath();
        EnsureExistingPathIsNotReparsePoint(normalized);
        return normalized;
    }

    private static string NormalizeDirectory(string directory)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(directory)) throw UnsafePath();
            var normalized = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
            if (string.IsNullOrWhiteSpace(normalized)) throw UnsafePath();
            EnsureExistingPathIsNotReparsePoint(normalized);
            return normalized;
        }
        catch (LocalInferenceSettingsException)
        {
            throw;
        }
        catch (Exception exception) when (exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            throw new LocalInferenceSettingsException(
                "inference_settings_unsafe_path",
                "The local inference settings contain an unsafe artifact path.",
                exception);
        }
    }

    private static void EnsureNoExistingReparsePoints(
        string rootDirectory,
        string fullPath)
    {
        EnsureExistingPathIsNotReparsePoint(rootDirectory);
        var relative = Path.GetRelativePath(rootDirectory, fullPath);
        var current = rootDirectory;
        foreach (var segment in relative.Split(Path.DirectorySeparatorChar))
        {
            current = Path.Combine(current, segment);
            if (!TryGetAttributes(current, out var attributes)) break;
            if ((attributes & FileAttributes.ReparsePoint) != 0) throw UnsafePath();
        }
    }

    private static void EnsureExistingPathIsNotReparsePoint(string path)
    {
        if (TryGetAttributes(path, out var attributes) &&
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw UnsafePath();
        }
    }

    private static bool TryGetAttributes(
        string path,
        out FileAttributes attributes)
    {
        try
        {
            attributes = File.GetAttributes(path);
            return true;
        }
        catch (FileNotFoundException)
        {
            attributes = default;
            return false;
        }
        catch (DirectoryNotFoundException)
        {
            attributes = default;
            return false;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            throw new LocalInferenceSettingsException(
                "inference_settings_unsafe_path",
                "The local inference artifact path could not be verified safely.",
                exception);
        }
    }

    private static bool IsStrictDescendant(string path, string directory)
    {
        var prefix = Path.TrimEndingDirectorySeparator(directory) +
            Path.DirectorySeparatorChar;
        return path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsSafeToken(string? value, int maximumCharacters)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            value.Length > maximumCharacters ||
            !char.IsAsciiLetterOrDigit(value[0]) ||
            !char.IsAsciiLetterOrDigit(value[^1]))
        {
            return false;
        }
        return value.All(character =>
            char.IsAsciiLetterOrDigit(character) || character is '-' or '_' or '.');
    }

    private static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f' or >= 'A' and <= 'F');

    private static bool TryParseBackend(
        string? value,
        out LocalInferenceBackend backend)
    {
        backend = value switch
        {
            "cpu" => LocalInferenceBackend.Cpu,
            "cuda" => LocalInferenceBackend.Cuda,
            "vulkan" => LocalInferenceBackend.Vulkan,
            _ => default,
        };
        return value is "cpu" or "cuda" or "vulkan";
    }

    private static bool IsReservedWindowsDeviceName(string segment)
    {
        var stem = segment.Split('.')[0];
        if (stem.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("NUL", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        return stem.Length == 4 &&
            (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) ||
             stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) &&
            stem[3] is >= '1' and <= '9';
    }

    private static async Task<byte[]> ReadBoundedAsync(
        string path,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (stream.Length > LocalInferenceSettingsLimits.MaximumDocumentBytes)
            throw SettingsTooLarge();

        var payload = new byte[LocalInferenceSettingsLimits.MaximumDocumentBytes + 1];
        var length = 0;
        while (length < payload.Length)
        {
            var read = await stream.ReadAsync(
                payload.AsMemory(length, payload.Length - length),
                cancellationToken).ConfigureAwait(false);
            if (read == 0) break;
            length += read;
        }
        if (length > LocalInferenceSettingsLimits.MaximumDocumentBytes ||
            stream.ReadByte() != -1)
        {
            throw SettingsTooLarge();
        }
        return payload[..length];
    }

    private static LocalInferenceSettingsException InvalidSettings(string message) =>
        new("inference_settings_invalid", message);

    private static LocalInferenceSettingsException UnsafePath() =>
        new(
            "inference_settings_unsafe_path",
            "The local inference settings contain an unsafe artifact path.");

    private static LocalInferenceSettingsException SettingsTooLarge() =>
        new(
            "inference_settings_too_large",
            $"The local inference settings must not exceed {LocalInferenceSettingsLimits.MaximumDocumentBytes} bytes.");

    private static LocalInferenceSettingsException Unavailable(Exception exception) =>
        new(
            "inference_settings_unavailable",
            "The local inference settings could not be read.",
            exception);

    private enum ArtifactPathKind
    {
        Model,
        Runtime,
        RuntimeDependency,
    }

    private sealed class SettingsDocument
    {
        public required int SchemaVersion { get; init; }
        public required string ModelId { get; init; }
        public required string Backend { get; init; }
        public required int ContextSize { get; init; }
        public required int MaxTokens { get; init; }
        public required double Temperature { get; init; }
        public required ArtifactDocument Model { get; init; }
        public required ArtifactDocument Runtime { get; init; }
        public required IReadOnlyList<ArtifactDocument> RuntimeDependencies { get; init; }
    }

    private sealed class ArtifactDocument
    {
        public required string RelativePath { get; init; }
        public required string Sha256 { get; init; }
        public required long ExpectedSize { get; init; }
    }
}
