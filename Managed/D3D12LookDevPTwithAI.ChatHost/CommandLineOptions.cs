namespace D3D12LookDevPTwithAI.ChatHost;

public sealed record CommandLineOptions(string PipeName, int ParentProcessId)
{
    public static bool TryParse(
        IReadOnlyList<string> arguments,
        out CommandLineOptions? options,
        out string? error)
    {
        options = null;
        error = null;
        string? pipeName = null;
        int? parentProcessId = null;

        for (var index = 0; index < arguments.Count; index++)
        {
            var argument = arguments[index];
            if (argument == "--pipe-name")
            {
                if (pipeName is not null)
                {
                    error = "--pipe-name may only be specified once.";
                    return false;
                }
                if (++index >= arguments.Count)
                {
                    error = "--pipe-name requires a value.";
                    return false;
                }
                pipeName = arguments[index];
                continue;
            }

            if (argument == "--parent-pid")
            {
                if (parentProcessId is not null)
                {
                    error = "--parent-pid may only be specified once.";
                    return false;
                }
                if (++index >= arguments.Count ||
                    !int.TryParse(arguments[index], out var parsedParentProcessId) ||
                    parsedParentProcessId <= 0)
                {
                    error = "--parent-pid requires a positive integer.";
                    return false;
                }
                parentProcessId = parsedParentProcessId;
                continue;
            }

            error = $"Unknown argument: {argument}";
            return false;
        }

        if (!IsValidPipeName(pipeName))
        {
            error = "--pipe-name is required and must be a simple local pipe name of at most 200 characters.";
            return false;
        }
        if (parentProcessId is null)
        {
            error = "--parent-pid is required.";
            return false;
        }

        options = new CommandLineOptions(pipeName!, parentProcessId.Value);
        return true;
    }

    private static bool IsValidPipeName(string? pipeName) =>
        !string.IsNullOrWhiteSpace(pipeName) &&
        pipeName.Length <= 200 &&
        pipeName.All(character => char.IsAsciiLetterOrDigit(character) || character is '.' or '-' or '_');
}
