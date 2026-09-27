using AIMP.Protocol;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

internal readonly record struct LocalPlayerPosition(float X, float Y, float Z)
{
    internal string ToDisplayText(float? yawRadians = null) => yawRadians is { } yaw
        ? FormattableString.Invariant($"X={X:0.000}  Y={Y:0.000}  Z={Z:0.000}  Yaw={yaw:0.000} rad")
        : FormattableString.Invariant($"X={X:0.000}  Y={Y:0.000}  Z={Z:0.000}");
}

internal readonly record struct LocalPlayerPositionAddresses(nint X, nint Y, nint Z);

internal static class LocalPlayerPositionMemoryLayout
{
    internal const int XOffset = 0x1307AC8;
    internal const int ZOffset = 0x1307AD0;
    internal const int YOffset = 0x1307AD8;
    internal const int BlockSize = 0x14;

    internal static LocalPlayerPositionAddresses Resolve(nint moduleBase) => new(
        moduleBase + XOffset,
        moduleBase + YOffset,
        moduleBase + ZOffset);

    internal static bool TryDecode(ReadOnlySpan<byte> bytes, out LocalPlayerPosition position)
    {
        position = default;
        if (bytes.Length < BlockSize) return false;
        var candidate = new LocalPlayerPosition(
            BitConverter.ToSingle(bytes[..4]),
            BitConverter.ToSingle(bytes.Slice(0x10, 4)),
            BitConverter.ToSingle(bytes.Slice(0x08, 4)));
        if (!IsReasonable(candidate)) return false;
        position = candidate;
        return true;
    }

    internal static bool IsReasonable(LocalPlayerPosition value) =>
        IsReasonable(value.X) && IsReasonable(value.Y) && IsReasonable(value.Z);

    private static bool IsReasonable(float value) =>
        float.IsFinite(value) && value > -100_000.0f && value < 100_000.0f;
}

internal static class LocalPlayerYawMemoryLayout
{
    internal const int CharacterBasePointerOffset = 0x1307A48;
    internal const int YawOffset = 0x174;
    internal const int PointerSize = sizeof(uint);
    internal const int YawSize = sizeof(float);

    internal static nint ResolveCharacterBasePointer(nint moduleBase) =>
        moduleBase + CharacterBasePointerOffset;

    internal static nint ResolveYaw(nint characterBase) => characterBase + YawOffset;

    internal static bool TryDecodeCharacterBase(ReadOnlySpan<byte> bytes, out nint characterBase)
    {
        characterBase = default;
        if (bytes.Length < PointerSize) return false;
        var candidate = BitConverter.ToUInt32(bytes[..PointerSize]);
        if (candidate < 0x10000) return false;
        characterBase = unchecked((nint)(long)candidate);
        return true;
    }

    internal static bool TryDecodeYaw(ReadOnlySpan<byte> bytes, out float yaw)
    {
        yaw = default;
        if (bytes.Length < YawSize) return false;
        var candidate = BitConverter.ToSingle(bytes[..YawSize]);
        if (!float.IsFinite(candidate) || candidate < -3.3f || candidate > 3.3f) return false;
        yaw = candidate;
        return true;
    }
}


internal readonly record struct LocalPlayerMovementProfile(
    PlayerMoveType RequestedMoveType,
    PlayerMoveType CurrentMoveType,
    float SlowWalkSpeed,
    float WalkSpeed,
    float FastWalkSpeed,
    float RunSpeed)
{
    internal float SpeedFor(PlayerMoveType moveType) => moveType switch
    {
        PlayerMoveType.SlowWalk => SlowWalkSpeed,
        PlayerMoveType.Walk => WalkSpeed,
        PlayerMoveType.FastWalk => FastWalkSpeed,
        PlayerMoveType.Run => RunSpeed,
        _ => WalkSpeed
    };

    internal PlayerMoveType ResolveEffectiveMoveType(float observedPlanarSpeed)
    {
        // +0x194/+0x198 remain the primary retail evidence. Position-derived
        // speed is used only to promote the gait when the player controller
        // outruns those NPC MovementAction fields (notably sprint/RUN). It is
        // deliberately NOT reused as the remote locomotion magnitude: doing
        // that made timer/transform jitter feed directly into the animation
        // and physical controller in v10.7.
        if (!float.IsFinite(observedPlanarSpeed) || observedPlanarSpeed < 0.05F)
            return CurrentMoveType;

        var speedSelected = PlayerMoveType.SlowWalk;
        if (observedPlanarSpeed >= (SlowWalkSpeed + WalkSpeed) * 0.5F)
            speedSelected = PlayerMoveType.Walk;
        if (observedPlanarSpeed >= (WalkSpeed + FastWalkSpeed) * 0.5F)
            speedSelected = PlayerMoveType.FastWalk;
        if (observedPlanarSpeed >= (FastWalkSpeed + RunSpeed) * 0.5F)
            speedSelected = PlayerMoveType.Run;

        var selected = (PlayerMoveType)Math.Max(
            (int)RequestedMoveType, (int)speedSelected);

        // Preserve a higher retail current gait only while the observed speed
        // still supports it. This prevents a stale +0x198 RUN from sticking
        // after the player has already fallen back to WALK.
        if ((int)CurrentMoveType > (int)selected &&
            observedPlanarSpeed >= SpeedFor(CurrentMoveType) * 0.80F)
            selected = CurrentMoveType;
        return selected;
    }
}

internal static class LocalPlayerMovementMemoryLayout
{
    // Retail evidence: CharacterBase+0x42A8 is the movement-action object.
    // +0x194 is the requested gait, while +0x198 is the current/effective
    // retail gait selected by the movement action and consumed by downstream
    // run/animation logic. The same object owns the 4-entry native speed table
    // at +0x4C. We read these fields only; no GoTo/navigation call is used.
    internal const int MovementActionPointerOffset = 0x42A8;
    internal const int RequestedMoveTypeOffset = 0x194;
    internal const int CurrentMoveTypeOffset = 0x198;
    internal const int NativeSpeedTableOffset = 0x4C;
    internal const int PointerSize = sizeof(uint);
    internal const int MoveTypeSize = sizeof(int);
    internal const int NativeSpeedSize = sizeof(float);

    internal static nint ResolveMovementActionPointer(nint characterBase) =>
        characterBase + MovementActionPointerOffset;

    internal static nint ResolveRequestedMoveType(nint movementAction) =>
        movementAction + RequestedMoveTypeOffset;

    internal static nint ResolveCurrentMoveType(nint movementAction) =>
        movementAction + CurrentMoveTypeOffset;

    internal static nint ResolveNativeSpeed(nint movementAction, PlayerMoveType moveType) =>
        movementAction + NativeSpeedTableOffset + (int)moveType * sizeof(float);

    internal static bool TryDecodePointer(ReadOnlySpan<byte> bytes, out nint pointer)
    {
        pointer = default;
        if (bytes.Length < PointerSize) return false;
        var candidate = BitConverter.ToUInt32(bytes[..PointerSize]);
        if (candidate < 0x10000) return false;
        pointer = unchecked((nint)(long)candidate);
        return true;
    }

    internal static bool TryDecodeMoveType(ReadOnlySpan<byte> bytes, out PlayerMoveType moveType)
    {
        moveType = PlayerMoveType.Walk;
        if (bytes.Length < MoveTypeSize) return false;
        var candidate = BitConverter.ToInt32(bytes[..MoveTypeSize]);
        if (candidate is < 0 or > 3) return false;
        moveType = (PlayerMoveType)candidate;
        return true;
    }

    internal static bool TryDecodeNativeSpeed(ReadOnlySpan<byte> bytes, out float speed)
    {
        speed = default;
        if (bytes.Length < NativeSpeedSize) return false;
        var candidate = BitConverter.ToSingle(bytes[..NativeSpeedSize]);
        if (!float.IsFinite(candidate) || candidate is < 0.05F or > 8.0F) return false;
        speed = candidate;
        return true;
    }
}

internal sealed class LocalPlayerPositionReader : IDisposable
{
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmRead = 0x0010;

    // 5.14 performance pass: these reads happen at the 16 ms gameplay sample
    // cadence.  Reuse the buffers instead of allocating several arrays every
    // tick, and read contiguous retail fields as one block where possible.
    private readonly byte[] _positionBytes = new byte[LocalPlayerPositionMemoryLayout.BlockSize];
    private readonly byte[] _pointerBytes = new byte[LocalPlayerYawMemoryLayout.PointerSize];
    private readonly byte[] _yawBytes = new byte[LocalPlayerYawMemoryLayout.YawSize];
    private readonly byte[] _moveTypeBytes = new byte[LocalPlayerMovementMemoryLayout.MoveTypeSize * 2];
    private readonly byte[] _speedTableBytes = new byte[LocalPlayerMovementMemoryLayout.NativeSpeedSize * 4];

    private SafeProcessHandle? _handle;
    private int _processId;

    internal string LastError { get; private set; } = string.Empty;

    internal bool TryReadFrame(
        NativeRuntimeSnapshot runtime,
        out LocalPlayerPosition position,
        out bool yawAvailable,
        out float yaw,
        out bool movementProfileAvailable,
        out LocalPlayerMovementProfile movementProfile)
    {
        position = default;
        yawAvailable = false;
        yaw = default;
        movementProfileAvailable = false;
        movementProfile = default;

        if (!CanRead(runtime) || !EnsureAttached(runtime.ProcessId)) return false;
        if (!TryReadPosition(runtime.ModuleBase, out position)) return false;

        // CharacterBase used to be fetched independently by TryReadYaw and
        // TryReadMovementProfile.  Resolve it once for the whole 60 Hz sample.
        if (!TryReadCharacterBase(runtime.ModuleBase, out var characterBase)) return true;
        yawAvailable = TryReadYaw(characterBase, out yaw);
        movementProfileAvailable = TryReadMovementProfile(characterBase, out movementProfile);
        return true;
    }

    internal bool TryRead(
        NativeRuntimeSnapshot runtime,
        out LocalPlayerPosition position)
    {
        position = default;
        if (!CanRead(runtime)) return false;
        if (!EnsureAttached(runtime.ProcessId)) return false;
        return TryReadPosition(runtime.ModuleBase, out position);
    }

    internal bool TryReadYaw(NativeRuntimeSnapshot runtime, out float yaw)
    {
        yaw = default;
        if (!CanRead(runtime) || !EnsureAttached(runtime.ProcessId)) return false;
        if (!TryReadCharacterBase(runtime.ModuleBase, out var characterBase)) return false;
        return TryReadYaw(characterBase, out yaw);
    }

    internal bool TryReadMovementProfile(
        NativeRuntimeSnapshot runtime,
        out LocalPlayerMovementProfile profile)
    {
        profile = default;
        if (!CanRead(runtime) || !EnsureAttached(runtime.ProcessId)) return false;
        if (!TryReadCharacterBase(runtime.ModuleBase, out var characterBase)) return false;
        return TryReadMovementProfile(characterBase, out profile);
    }

    private bool CanRead(NativeRuntimeSnapshot runtime)
    {
        if (runtime.AiRunning && runtime.ProcessId > 0 && runtime.ModuleBase != 0) return true;
        Reset("AI.exe is not running.");
        return false;
    }

    private bool EnsureAttached(int processId)
    {
        if (_processId == processId && _handle is { IsInvalid: false, IsClosed: false }) return true;
        Reset(string.Empty);
        _handle = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, processId);
        if (!_handle.IsInvalid) { _processId = processId; return true; }
        LastError = new Win32Exception(Marshal.GetLastWin32Error()).Message;
        return false;
    }

    private bool TryReadPosition(nint moduleBase, out LocalPlayerPosition position)
    {
        var start = LocalPlayerPositionMemoryLayout.Resolve(moduleBase).X;
        if (!ReadBlock(start, _positionBytes)) { position = default; return false; }
        if (LocalPlayerPositionMemoryLayout.TryDecode(_positionBytes, out position))
        {
            LastError = string.Empty;
            return true;
        }
        LastError = "The position values read from AI.exe are invalid.";
        return false;
    }

    private bool TryReadCharacterBase(nint moduleBase, out nint characterBase)
    {
        var address = LocalPlayerYawMemoryLayout.ResolveCharacterBasePointer(moduleBase);
        if (!ReadBlock(address, _pointerBytes)) { characterBase = default; return false; }
        if (LocalPlayerYawMemoryLayout.TryDecodeCharacterBase(_pointerBytes, out characterBase))
            return true;
        LastError = "The local CharacterBase pointer read from AI.exe is invalid.";
        return false;
    }

    private bool TryReadYaw(nint characterBase, out float yaw)
    {
        var address = LocalPlayerYawMemoryLayout.ResolveYaw(characterBase);
        if (!ReadBlock(address, _yawBytes)) { yaw = default; return false; }
        if (LocalPlayerYawMemoryLayout.TryDecodeYaw(_yawBytes, out yaw)) return true;
        LastError = "The local player yaw read from CharacterBase is invalid.";
        return false;
    }

    private bool TryReadMovementProfile(
        nint characterBase,
        out LocalPlayerMovementProfile profile)
    {
        profile = default;
        if (!ReadBlock(
                LocalPlayerMovementMemoryLayout.ResolveMovementActionPointer(characterBase),
                _pointerBytes)
            || !LocalPlayerMovementMemoryLayout.TryDecodePointer(
                _pointerBytes, out var movementAction))
        {
            LastError = "The local MovementAction pointer is invalid.";
            return false;
        }

        // +0x194 and +0x198 are adjacent.  One ReadProcessMemory replaces the
        // two per-field reads used by the stable 5.13 launcher.
        if (!ReadBlock(
                LocalPlayerMovementMemoryLayout.ResolveRequestedMoveType(movementAction),
                _moveTypeBytes)
            || !LocalPlayerMovementMemoryLayout.TryDecodeMoveType(
                _moveTypeBytes.AsSpan(0, LocalPlayerMovementMemoryLayout.MoveTypeSize),
                out var requestedMoveType)
            || !LocalPlayerMovementMemoryLayout.TryDecodeMoveType(
                _moveTypeBytes.AsSpan(LocalPlayerMovementMemoryLayout.MoveTypeSize,
                    LocalPlayerMovementMemoryLayout.MoveTypeSize),
                out var currentMoveType))
        {
            LastError = "The local retail move_type fields are invalid.";
            return false;
        }

        // The four native speeds are a contiguous 16-byte table at +0x4C.
        // Read it once instead of crossing the process boundary four times.
        if (!ReadBlock(
                LocalPlayerMovementMemoryLayout.ResolveNativeSpeed(
                    movementAction, PlayerMoveType.SlowWalk),
                _speedTableBytes))
        {
            LastError = "The local retail move_type speed table is unavailable.";
            return false;
        }

        Span<float> speeds = stackalloc float[4];
        for (var index = 0; index < speeds.Length; index++)
        {
            if (!LocalPlayerMovementMemoryLayout.TryDecodeNativeSpeed(
                    _speedTableBytes.AsSpan(
                        index * LocalPlayerMovementMemoryLayout.NativeSpeedSize,
                        LocalPlayerMovementMemoryLayout.NativeSpeedSize),
                    out speeds[index]))
            {
                LastError = "The local retail move_type speed table is invalid.";
                return false;
            }
        }

        profile = new LocalPlayerMovementProfile(
            requestedMoveType, currentMoveType,
            speeds[0], speeds[1], speeds[2], speeds[3]);
        LastError = string.Empty;
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
