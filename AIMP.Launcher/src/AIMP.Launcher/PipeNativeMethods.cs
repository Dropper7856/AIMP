using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

internal static class PipeNativeMethods
{
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientProcessId(
        SafePipeHandle pipe,
        out uint clientProcessId);

    internal static int GetClientProcessId(SafePipeHandle pipe)
    {
        if (GetNamedPipeClientProcessId(pipe, out var processId))
        {
            return checked((int)processId);
        }

        throw new Win32Exception(
            Marshal.GetLastWin32Error(),
            "GetNamedPipeClientProcessId failed.");
    }
}
