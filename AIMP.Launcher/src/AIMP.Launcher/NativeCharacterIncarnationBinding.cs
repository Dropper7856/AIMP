using AIMP.Protocol;

namespace AIMP.Launcher;

// Local observation reducer. The controller authenticates the sender session
// before Apply; these values grant neither actor ownership nor network authority.
internal static class NativeCharacterIncarnationBinding
{
    internal static NativeRuntimeSnapshot Apply(NativeRuntimeSnapshot state, NativeCharacterIncarnationState value, Guid connection) => state with
    {
        LocalCharacterGeneration = value.NativeGeneration, LocalCharacterSerial = value.Serial,
        LocalCharacterBase = value.Character, LocalCharacterRuntimeId = value.RuntimeId,
        LocalCharacterOrigin = value.Origin, LocalCharacterConnectionIdentity = connection
    };

    internal static NativeRuntimeSnapshot Clear(NativeRuntimeSnapshot state) => state with
    {
        LocalCharacterGeneration = 0, LocalCharacterSerial = 0, LocalCharacterBase = 0,
        LocalCharacterRuntimeId = 0, LocalCharacterOrigin = 0, LocalCharacterConnectionIdentity = Guid.Empty,
        LocalHiding = null, LocalHidingReceivedAt = null
    };

    // The ticket can precede LevelState on IPC. Preserve a matching new-world
    // observation instead of erasing it when that delayed level update arrives.
    internal static NativeRuntimeSnapshot RetainForGeneration(NativeRuntimeSnapshot value, int generation) =>
        generation > 0 && value.LocalCharacterGeneration == (ulong)generation ? value : Clear(value);

    internal static bool MatchesCurrent(NativeRuntimeSnapshot value, Guid connection, int generation) =>
        value.IpcConnected && connection != Guid.Empty && value.LocalCharacterConnectionIdentity == connection
        && generation > 0 && value.LevelGeneration == generation && value.LocalCharacterGeneration == (ulong)generation
        && value.LocalCharacterSerial > 0 && value.LocalCharacterRuntimeId > 0 && value.LocalCharacterOrigin <= 1
        && value.LocalCharacterBase is >= 0x10000 and <= uint.MaxValue - 0x50;
}
