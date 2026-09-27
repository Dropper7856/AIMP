using System.Runtime.InteropServices;

namespace AIMP.Injector32;

internal static class NativeMethods
{
    internal const uint ProcessCreateThread = 0x0002;
    internal const uint ProcessQueryInformation = 0x0400;
    internal const uint ProcessVmOperation = 0x0008;
    internal const uint ProcessVmWrite = 0x0020;
    internal const uint ProcessVmRead = 0x0010;
    internal const uint MemCommit = 0x1000;
    internal const uint MemReserve = 0x2000;
    internal const uint MemRelease = 0x8000;
    internal const uint PageReadWrite = 0x04;
    internal const uint WaitObject0 = 0;
    internal const uint WaitTimeout = 0x102;
    internal const uint Th32CsSnapModule = 0x00000008;
    internal const uint Th32CsSnapModule32 = 0x00000010;
    internal const ushort ImageFileMachineUnknown = 0;
    internal const ushort ImageFileMachineI386 = 0x014c;

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern nint OpenProcess(uint desiredAccess, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern nint VirtualAllocEx(
        nint process,
        nint address,
        nuint size,
        uint allocationType,
        uint protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool VirtualFreeEx(
        nint process,
        nint address,
        nuint size,
        uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WriteProcessMemory(
        nint process,
        nint baseAddress,
        byte[] buffer,
        nuint size,
        out nuint written);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern nint CreateRemoteThread(
        nint process,
        nint threadAttributes,
        nuint stackSize,
        nint startAddress,
        nint parameter,
        uint creationFlags,
        out uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern uint WaitForSingleObject(nint handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GetExitCodeThread(nint thread, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool CloseHandle(nint handle);

    [DllImport("kernel32.dll", EntryPoint = "GetModuleHandleW", SetLastError = true,
        CharSet = CharSet.Unicode)]
    internal static extern nint GetModuleHandle(string moduleName);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    internal static extern nint GetProcAddress(nint module, string procedureName);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern nint CreateToolhelp32Snapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", EntryPoint = "Module32FirstW", SetLastError = true,
        CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool Module32First(nint snapshot, ref ModuleEntry32 entry);

    [DllImport("kernel32.dll", EntryPoint = "Module32NextW", SetLastError = true,
        CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool Module32Next(nint snapshot, ref ModuleEntry32 entry);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool IsWow64Process(nint process, [MarshalAs(UnmanagedType.Bool)] out bool wow64);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool IsWow64Process2(
        nint process,
        out ushort processMachine,
        out ushort nativeMachine);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct ModuleEntry32
    {
        internal uint Size;
        internal uint ModuleId;
        internal uint ProcessId;
        internal uint GlobalUsage;
        internal uint ProcessUsage;
        internal nint ModuleBaseAddress;
        internal uint ModuleBaseSize;
        internal nint ModuleHandle;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        internal string Module;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        internal string ExecutablePath;
    }
}
