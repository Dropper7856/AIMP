using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

internal static class RemoteCharacterPositionMemoryLayout
{
    internal const int PositionOffset = 0x70;
    internal const int BlockSize = 12;

    internal static bool TryDecode(ReadOnlySpan<byte> bytes, out LocalPlayerPosition position)
    {
        position = default;
        if (bytes.Length < BlockSize) return false;
        var candidate = new LocalPlayerPosition(BitConverter.ToSingle(bytes[..4]),
            BitConverter.ToSingle(bytes.Slice(4, 4)), BitConverter.ToSingle(bytes.Slice(8, 4)));
        if (!LocalPlayerPositionMemoryLayout.IsReasonable(candidate)) return false;
        position = candidate;
        return true;
    }
}

internal sealed class RemoteCharacterPositionReader : IDisposable
{
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmRead = 0x0010;
    private SafeProcessHandle? _handle;
    private int _processId;

    internal bool TryRead(
        NativeRuntimeSnapshot runtime,
        uint characterBase,
        out LocalPlayerPosition position)
    {
        position = default;
        if (!CanRead(runtime, characterBase) || !EnsureAttached(runtime.ProcessId)) return false;
        var address = unchecked((nint)(nuint)characterBase) + RemoteCharacterPositionMemoryLayout.PositionOffset;
        var buffer = new byte[RemoteCharacterPositionMemoryLayout.BlockSize];
        return ReadBlock(address, buffer) && RemoteCharacterPositionMemoryLayout.TryDecode(buffer, out position);
    }

    private static bool CanRead(NativeRuntimeSnapshot runtime, uint characterBase) =>
        runtime.AiRunning && runtime.ProcessId > 0 && characterBase >= 0x10000
        && (characterBase & 0xFU) == 0;

    private bool EnsureAttached(int processId)
    {
        if (_processId == processId && _handle is { IsInvalid: false, IsClosed: false }) return true;
        Reset();
        _handle = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, processId);
        if (_handle.IsInvalid) return false;
        _processId = processId;
        return true;
    }

    private bool ReadBlock(nint address, byte[] buffer)
    {
        if (_handle is null || _handle.IsInvalid || _handle.IsClosed) return false;
        var ok = ReadProcessMemory(_handle, address, buffer, (nuint)buffer.Length, out var read);
        return ok && read == (nuint)buffer.Length;
    }

    private void Reset()
    {
        _handle?.Dispose();
        _handle = null;
        _processId = 0;
    }

    public void Dispose() => Reset();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeProcessHandle OpenProcess(
        uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReadProcessMemory(
        SafeProcessHandle process,
        nint baseAddress,
        [Out] byte[] buffer,
        nuint size,
        out nuint bytesRead);
}
