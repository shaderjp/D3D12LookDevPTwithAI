using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace D3D12LookDevPTwithAI.ChatHost;

internal static class NativeMethods
{
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeServerProcessId(
        SafePipeHandle pipe,
        out uint serverProcessId);

    public static void VerifyNamedPipeServerProcess(
        SafePipeHandle pipe,
        int expectedProcessId)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("The ChatHost named-pipe transport is Windows-only.");
        if (!GetNamedPipeServerProcessId(pipe, out var actualProcessId))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not identify the named-pipe server process.");
        if (actualProcessId != (uint)expectedProcessId)
            throw new UnauthorizedAccessException("The named-pipe server is not the expected parent process.");
    }
}
