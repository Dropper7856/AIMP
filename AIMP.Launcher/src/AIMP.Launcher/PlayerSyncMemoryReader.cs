using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

internal readonly record struct LocalPlayerEquipmentState(
    int CurrentEquipmentIndex,
    bool EquipmentActive,
    int PresentationState);

internal static class PlayerSyncMemoryLayout
{
    // Retail x86 profile confirmed live with Cheat Engine on 2026-09-15.
    // FUN_0078AAF0 / AI.exe+38BAF0 (AI.exe.text+38AAF0 in CE) resolves the local Player through this root.
    internal const int LocalPlayerRootPointerOffset = 0x12F0C88;
    internal const int RootContextOffset = 0x08;
    internal const int LocalPlayerCountOffset = 0x84;
    internal const int LocalPlayerArrayOffset = 0x80;

    // Local Player -> generic equipment manager.
    internal const int EquipmentManagerOffset = 0x1100;

    // Equipment manager fields confirmed live:
    // +0x150C current equipment index
    // +0x1510 non-zero while the selected equipment is active/deployed
    // +0x152C points to the presentation state object; +0x04 is its state enum.
    internal const int CurrentEquipmentOffset = 0x150C;
    internal const int EquipmentActiveOffset = 0x1510;
    internal const int PresentationStateObjectOffset = 0x152C;
    internal const int PresentationStateOffset = 0x04;

    internal const int PointerSize = sizeof(uint);
    internal const int IntSize = sizeof(int);
    internal const int ByteSize = sizeof(byte);
}

internal sealed class PlayerSyncMemoryReader : IDisposable
{
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmRead = 0x0010;

    private SafeProcessHandle? _handle;
    private int _processId;

    internal string LastError { get; private set; } = string.Empty;

    internal bool TryRead(
        NativeRuntimeSnapshot runtime,
        out LocalPlayerEquipmentState state)
    {
        state = default;
        if (!runtime.AiRunning || runtime.ProcessId <= 0 || runtime.ModuleBase == 0)
        {
            Reset("AI.exe is not running.");
            return false;
        }

        if (!EnsureAttached(runtime.ProcessId)) return false;
        if (!TryReadPointer(runtime.ModuleBase + PlayerSyncMemoryLayout.LocalPlayerRootPointerOffset,
                out var root))
        {
            LastError = "PlayerSync local-player root pointer is unavailable.";
            return false;
        }
        if (!TryReadPointer(root + PlayerSyncMemoryLayout.RootContextOffset, out var context))
        {
            LastError = "PlayerSync local-player context is unavailable.";
            return false;
        }
        if (!TryReadInt(context + PlayerSyncMemoryLayout.LocalPlayerCountOffset, out var playerCount)
            || playerCount <= 0)
        {
            LastError = "PlayerSync local player is not present yet.";
            return false;
        }
        if (!TryReadPointer(context + PlayerSyncMemoryLayout.LocalPlayerArrayOffset, out var playerArray)
            || !TryReadPointer(playerArray, out var localPlayer))
        {
            LastError = "PlayerSync local Player pointer is unavailable.";
            return false;
        }
        if (!TryReadPointer(localPlayer + PlayerSyncMemoryLayout.EquipmentManagerOffset,
                out var equipmentManager))
        {
            LastError = "PlayerSync equipment manager is unavailable.";
            return false;
        }
        if (!TryReadInt(equipmentManager + PlayerSyncMemoryLayout.CurrentEquipmentOffset,
                out var currentEquipment)
            || currentEquipment is < -1 or > 64)
        {
            LastError = "PlayerSync current-equipment index is invalid.";
            return false;
        }
        if (!TryReadByte(equipmentManager + PlayerSyncMemoryLayout.EquipmentActiveOffset,
                out var activeByte))
        {
            LastError = "PlayerSync equipment-active state is unavailable.";
            return false;
        }
        if (!TryReadPointer(equipmentManager + PlayerSyncMemoryLayout.PresentationStateObjectOffset,
                out var presentationStateObject)
            || !TryReadInt(presentationStateObject + PlayerSyncMemoryLayout.PresentationStateOffset,
                out var presentationState)
            || presentationState is < 0 or > 64)
        {
            LastError = "PlayerSync equipment presentation state is unavailable.";
            return false;
        }

        state = new LocalPlayerEquipmentState(
            currentEquipment,
            activeByte != 0,
            presentationState);
        LastError = string.Empty;
        return true;
    }

    private bool EnsureAttached(int processId)
    {
        if (_processId == processId && _handle is { IsInvalid: false, IsClosed: false }) return true;
        Reset(string.Empty);
        _handle = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, processId);
        if (!_handle.IsInvalid)
        {
            _processId = processId;
            return true;
        }

        LastError = new Win32Exception(Marshal.GetLastWin32Error()).Message;
        return false;
    }

    private bool TryReadPointer(nint address, out nint value)
    {
        value = default;
        var buffer = new byte[PlayerSyncMemoryLayout.PointerSize];
        if (!ReadBlock(address, buffer)) return false;
        var candidate = BitConverter.ToUInt32(buffer);
        if (candidate < 0x10000) return false;
        value = unchecked((nint)(long)candidate);
        return true;
    }

    private bool TryReadInt(nint address, out int value)
    {
        value = default;
        var buffer = new byte[PlayerSyncMemoryLayout.IntSize];
        if (!ReadBlock(address, buffer)) return false;
        value = BitConverter.ToInt32(buffer);
        return true;
    }

    private bool TryReadByte(nint address, out byte value)
    {
        value = default;
        var buffer = new byte[PlayerSyncMemoryLayout.ByteSize];
        if (!ReadBlock(address, buffer)) return false;
        value = buffer[0];
        return true;
    }

    private bool ReadBlock(nint address, byte[] buffer)
    {
        if (_handle is null || _handle.IsInvalid || _handle.IsClosed) return false;
        var ok = ReadProcessMemory(_handle, address, buffer, (nuint)buffer.Length, out var read);
        if (ok && read == (nuint)buffer.Length) return true;
        LastError = new Win32Exception(Marshal.GetLastWin32Error()).Message;
        return false;
    }

    private void Reset(string error)
    {
        _handle?.Dispose();
        _handle = null;
        _processId = 0;
        LastError = error;
    }

    public void Dispose() => Reset(string.Empty);

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
