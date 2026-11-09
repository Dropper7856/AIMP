using AIMP.Protocol;

namespace AIMP.Launcher;

// AnimationSync owner side. The DLL samples THE_PLAYER's scripted cycle on the
// engine thread (any CMD_PlayAnimation); this only admits that observation for
// the exact current local Character and advances its clock to "now".
internal static class LocalPlayerAnimationReader
{
    internal static PlayerAnimationSnapshot? Read(NativeRuntimeSnapshot runtime, nint character, DateTimeOffset now)
    {
        var native = runtime.LocalAnimation;
        if (native is null || !runtime.IpcConnected
            || native.NativeGeneration != (ulong)runtime.LevelGeneration
            || native.NativeGeneration != runtime.LocalCharacterGeneration
            || native.Serial != runtime.LocalCharacterSerial || native.Character != (uint)character
            || native.Character != runtime.LocalCharacterBase || runtime.LocalAnimationReceivedAt is not { } received
            || now - received > TimeSpan.FromSeconds(1) || now < received
            || !PlayerAnimationCodec.TryDecode(native.Animation, out var animation)) return null;
        // Both processes use Windows uptime; include time spent queued in IPC,
        // rather than making an old native sample fresh when it is received.
        var tick = (ulong)Environment.TickCount64;
        if (native.SampledAtTickMilliseconds > tick || tick - native.SampledAtTickMilliseconds > 1000U) return null;
        return PlayerAnimationCodec.Advance(animation, (long)(tick - native.SampledAtTickMilliseconds));
    }
}
