using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace AIMP.Injector32;

internal static class NativeInjector
{
    private const string NativeDllName = "AIMP.NetZ.dll";
    private const uint RequiredProcessAccess = NativeMethods.ProcessCreateThread
        | NativeMethods.ProcessQueryInformation
        | NativeMethods.ProcessVmOperation
        | NativeMethods.ProcessVmWrite
        | NativeMethods.ProcessVmRead;
    private static readonly nint InvalidHandleValue = new(-1);

    public static string Inject(
        int processId,
        string dllPath,
        string expectedExecutablePath,
        long expectedProcessStartUtcTicks,
        TimeSpan timeout)
    {
        var canonicalDllPath = ValidateDll(dllPath);
        using var process = Process.GetProcessById(processId);
        ValidateTargetProcess(process, expectedExecutablePath, expectedProcessStartUtcTicks);
        EnsureDllNotAlreadyLoaded(processId, canonicalDllPath);
        using var processHandle = OpenTargetProcess(processId);
        EnsureX86Target(processHandle.Value);
        var loadLibrary = ResolveRemoteLoadLibrary(processId);
        var loadedModule = LoadDll(processHandle.Value, loadLibrary, canonicalDllPath, timeout);
        return $"DLL loaded into AI.exe (PID {processId}, module 0x{loadedModule:X8}).";
    }

    private static string ValidateDll(string dllPath)
    {
        var canonicalPath = Path.GetFullPath(dllPath);
        PeImage.EnsureX86Dll(canonicalPath);
        if (!string.Equals(Path.GetFileName(canonicalPath), NativeDllName,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"The injected DLL must be named {NativeDllName}.");
        }

        return canonicalPath;
    }

    private static void ValidateTargetProcess(
        Process process,
        string expectedExecutablePath,
        long expectedStartUtcTicks)
    {
        process.Refresh();
        EnsureProcessIsActive(process);
        EnsureProcessIdentity(process, expectedStartUtcTicks);
        var executablePath = ReadExecutablePath(process);
        EnsureExecutablePath(executablePath, expectedExecutablePath);
    }

    private static void EnsureProcessIsActive(Process process)
    {
        if (process.HasExited)
        {
            throw new InvalidOperationException("The target process has already exited.");
        }
    }

    private static void EnsureProcessIdentity(Process process, long expectedStartUtcTicks)
    {
        if (process.StartTime.ToUniversalTime().Ticks != expectedStartUtcTicks)
        {
            throw new InvalidOperationException(
                "The target PID creation identity changed. Injection was refused.");
        }
    }

    private static string ReadExecutablePath(Process process)
    {
        try
        {
            return process.MainModule?.FileName
                ?? throw new InvalidOperationException("The target executable path is unavailable.");
        }
        catch (Win32Exception exception)
        {
            throw new InvalidOperationException(
                "The target executable could not be validated.", exception);
        }
    }

    private static void EnsureExecutablePath(string actualPath, string expectedPath)
    {
        var actual = Path.GetFullPath(actualPath);
        var expected = Path.GetFullPath(expectedPath);
        if (!string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                "The target PID no longer matches the selected AI.exe path. "
                + $"Actual={actual}; Expected={expected}.");
        }
    }

    private static NativeHandle OpenTargetProcess(int processId)
    {
        var handle = NativeMethods.OpenProcess(
            RequiredProcessAccess,
            false,
            checked((uint)processId));
        if (handle == 0)
        {
            ThrowLastWin32("OpenProcess");
        }

        return new NativeHandle(handle);
    }

    private static void EnsureX86Target(nint processHandle)
    {
        // IsWow64Process2 handles both native x86 Windows and x86 under WOW64.
        try
        {
            EnsureX86TargetModern(processHandle);
        }
        catch (EntryPointNotFoundException)
        {
            EnsureX86TargetLegacy(processHandle);
        }
    }

    private static void EnsureX86TargetModern(nint processHandle)
    {
        if (!NativeMethods.IsWow64Process2(
                processHandle,
                out var processMachine,
                out var nativeMachine))
        {
            ThrowLastWin32("IsWow64Process2");
        }

        if (!IsX86Machine(processMachine, nativeMachine))
        {
            throw new InvalidOperationException(
                $"AI.exe must be x86 (process=0x{processMachine:X4}, native=0x{nativeMachine:X4}).");
        }
    }

    private static bool IsX86Machine(ushort processMachine, ushort nativeMachine) =>
        processMachine == NativeMethods.ImageFileMachineI386
        || (processMachine == NativeMethods.ImageFileMachineUnknown
            && nativeMachine == NativeMethods.ImageFileMachineI386);

    private static void EnsureX86TargetLegacy(nint processHandle)
    {
        if (!NativeMethods.IsWow64Process(processHandle, out var wow64))
        {
            ThrowLastWin32("IsWow64Process");
        }

        if (Environment.Is64BitOperatingSystem && !wow64)
        {
            throw new InvalidOperationException("The target process is not x86.");
        }
    }

    private static nint ResolveRemoteLoadLibrary(int processId)
    {
        var localLoadLibrary = GetLocalLoadLibrary();
        var localOwner = FindContainingModule(Environment.ProcessId, localLoadLibrary);
        // Windows may forward kernel32!LoadLibraryW to KernelBase. Apply the RVA
        // to the same owning module in the target process.
        var functionRva = CalculateRva(localLoadLibrary, localOwner.BaseAddress);
        var remoteOwner = FindRemoteModuleBase(processId, localOwner.Name);
        return checked(remoteOwner + (nint)functionRva);
    }

    private static nint GetLocalLoadLibrary()
    {
        var kernel32 = NativeMethods.GetModuleHandle("kernel32.dll");
        if (kernel32 == 0)
        {
            ThrowLastWin32("GetModuleHandleW(kernel32.dll)");
        }

        var loadLibrary = NativeMethods.GetProcAddress(kernel32, "LoadLibraryW");
        if (loadLibrary == 0)
        {
            ThrowLastWin32("GetProcAddress(LoadLibraryW)");
        }

        return loadLibrary;
    }

    private static nuint CalculateRva(nint address, nint moduleBase) =>
        checked(unchecked((nuint)address) - unchecked((nuint)moduleBase));

    private static uint LoadDll(
        nint processHandle,
        nint loadLibrary,
        string dllPath,
        TimeSpan timeout)
    {
        var pathBytes = Encoding.Unicode.GetBytes(dllPath + '\0');
        nint remotePath = 0;
        nint remoteThread = 0;
        var threadCompleted = false;
        try
        {
            return ExecuteRemoteLoad(processHandle, loadLibrary, pathBytes, timeout,
                ref remotePath, ref remoteThread, ref threadCompleted);
        }
        finally
        {
            ReleaseRemoteResources(processHandle, remotePath, remoteThread, threadCompleted);
        }
    }

    private static uint ExecuteRemoteLoad(
        nint processHandle,
        nint loadLibrary,
        byte[] pathBytes,
        TimeSpan timeout,
        ref nint remotePath,
        ref nint remoteThread,
        ref bool threadCompleted)
    {
        remotePath = AllocateRemotePath(processHandle, pathBytes.Length);
        WriteRemotePath(processHandle, remotePath, pathBytes);
        remoteThread = StartRemoteLoad(processHandle, loadLibrary, remotePath);
        WaitForRemoteThread(remoteThread, timeout);
        threadCompleted = true;
        return ReadLoadedModule(remoteThread);
    }

    private static nint AllocateRemotePath(nint processHandle, int byteCount)
    {
        var remotePath = NativeMethods.VirtualAllocEx(
            processHandle,
            0,
            checked((nuint)byteCount),
            NativeMethods.MemCommit | NativeMethods.MemReserve,
            NativeMethods.PageReadWrite);
        if (remotePath == 0)
        {
            ThrowLastWin32("VirtualAllocEx");
        }

        return remotePath;
    }

    private static void WriteRemotePath(nint processHandle, nint remotePath, byte[] pathBytes)
    {
        var expected = checked((nuint)pathBytes.Length);
        if (!NativeMethods.WriteProcessMemory(
                processHandle,
                remotePath,
                pathBytes,
                expected,
                out var written)
            || written != expected)
        {
            ThrowLastWin32("WriteProcessMemory");
        }
    }

    private static nint StartRemoteLoad(nint processHandle, nint loadLibrary, nint remotePath)
    {
        var remoteThread = NativeMethods.CreateRemoteThread(
            processHandle,
            0,
            0,
            loadLibrary,
            remotePath,
            0,
            out _);
        if (remoteThread == 0)
        {
            ThrowLastWin32("CreateRemoteThread");
        }

        return remoteThread;
    }

    private static void WaitForRemoteThread(nint remoteThread, TimeSpan timeout)
    {
        var waitResult = NativeMethods.WaitForSingleObject(
            remoteThread,
            checked((uint)timeout.TotalMilliseconds));
        if (waitResult == NativeMethods.WaitTimeout)
        {
            throw new InvalidOperationException(
                "Injection timed out. Remote memory was retained because the thread may still use it.");
        }

        if (waitResult != NativeMethods.WaitObject0)
        {
            ThrowLastWin32("WaitForSingleObject");
        }
    }

    private static uint ReadLoadedModule(nint remoteThread)
    {
        if (!NativeMethods.GetExitCodeThread(remoteThread, out var loadedModule))
        {
            ThrowLastWin32("GetExitCodeThread");
        }

        if (loadedModule == 0)
        {
            throw new InvalidOperationException(
                "LoadLibraryW failed in AI.exe and returned a null module.");
        }

        return loadedModule;
    }

    private static void ReleaseRemoteResources(
        nint processHandle,
        nint remotePath,
        nint remoteThread,
        bool threadCompleted)
    {
        if (remoteThread != 0)
        {
            NativeMethods.CloseHandle(remoteThread);
        }

        // Keep the path only when a live remote thread may still read it.
        if (remotePath != 0 && (remoteThread == 0 || threadCompleted))
        {
            NativeMethods.VirtualFreeEx(
                processHandle, remotePath, 0, NativeMethods.MemRelease);
        }
    }

    private static nint FindRemoteModuleBase(int processId, string moduleName)
    {
        var module = FindModule(processId, entry => string.Equals(
            entry.Module, moduleName, StringComparison.OrdinalIgnoreCase));
        return module?.ModuleBaseAddress
            ?? throw new InvalidOperationException(
                $"{moduleName} is missing from the target process.");
    }

    private static void EnsureDllNotAlreadyLoaded(int processId, string canonicalDllPath)
    {
        var loaded = FindModule(processId, entry => IsSameDll(entry, canonicalDllPath));
        if (loaded is null)
        {
            return;
        }

        var loadedPath = string.IsNullOrWhiteSpace(loaded.Value.ExecutablePath)
            ? loaded.Value.Module
            : loaded.Value.ExecutablePath;
        throw new InvalidOperationException(
            $"{NativeDllName} is already loaded in the target process ({loadedPath}). "
            + "A second load is not allowed.");
    }

    private static bool IsSameDll(
        NativeMethods.ModuleEntry32 entry,
        string canonicalDllPath)
    {
        if (string.Equals(entry.Module, NativeDllName, StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return !string.IsNullOrWhiteSpace(entry.ExecutablePath)
            && string.Equals(Path.GetFullPath(entry.ExecutablePath), canonicalDllPath,
                StringComparison.OrdinalIgnoreCase);
    }

    private static (string Name, nint BaseAddress) FindContainingModule(
        int processId,
        nint address)
    {
        var candidate = unchecked((nuint)address);
        var module = FindModule(processId, entry => ContainsAddress(entry, candidate));
        return module is null
            ? throw new InvalidOperationException(
                "The module that owns LoadLibraryW could not be found.")
            : (module.Value.Module, module.Value.ModuleBaseAddress);
    }

    private static bool ContainsAddress(NativeMethods.ModuleEntry32 entry, nuint address)
    {
        var start = unchecked((nuint)entry.ModuleBaseAddress);
        var end = checked(start + entry.ModuleBaseSize);
        return address >= start && address < end;
    }

    private static NativeMethods.ModuleEntry32? FindModule(
        int processId,
        Func<NativeMethods.ModuleEntry32, bool> matches)
    {
        using var snapshot = CreateModuleSnapshot(processId);
        var entry = CreateModuleEntry();
        EnsureFirstModule(snapshot.Value, ref entry);
        while (!matches(entry))
        {
            if (!NativeMethods.Module32Next(snapshot.Value, ref entry))
            {
                return null;
            }
        }

        return entry;
    }

    private static NativeHandle CreateModuleSnapshot(int processId)
    {
        var snapshot = NativeMethods.CreateToolhelp32Snapshot(
            NativeMethods.Th32CsSnapModule | NativeMethods.Th32CsSnapModule32,
            checked((uint)processId));
        if (snapshot == InvalidHandleValue)
        {
            ThrowLastWin32("CreateToolhelp32Snapshot");
        }

        return new NativeHandle(snapshot);
    }

    private static NativeMethods.ModuleEntry32 CreateModuleEntry()
    {
        var entry = new NativeMethods.ModuleEntry32();
        entry.Size = checked((uint)Marshal.SizeOf<NativeMethods.ModuleEntry32>());
        entry.Module = string.Empty;
        entry.ExecutablePath = string.Empty;
        return entry;
    }

    private static void EnsureFirstModule(
        nint snapshot,
        ref NativeMethods.ModuleEntry32 entry)
    {
        if (!NativeMethods.Module32First(snapshot, ref entry))
        {
            ThrowLastWin32("Module32FirstW");
        }
    }

    private static void ThrowLastWin32(string operation) =>
        throw new Win32Exception(Marshal.GetLastWin32Error(), $"{operation} failed");

    private sealed class NativeHandle : SafeHandle
    {
        public NativeHandle(nint value) : base(IntPtr.Zero, true) => SetHandle(value);

        public nint Value => DangerousGetHandle();

        public override bool IsInvalid =>
            handle == IntPtr.Zero || handle == InvalidHandleValue;

        protected override bool ReleaseHandle() => NativeMethods.CloseHandle(handle);
    }
}
