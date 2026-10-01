namespace AIMP.Launcher;

internal enum PerformanceIsolationModule
{
    LocalPlayerSampling,
    PositionNetworkTransmit,
    RemoteCharacterSpawn,
    RemoteMovement,
    EquipmentSync,
    ActionSync,
    WorldSync,
    UiBridge,
    EngineThreadEvidence,
    CharacterLifecycleCapture,
    GameThreadPumps,
    Diagnostics,
    AISync
}

internal sealed record PerformanceIsolationDefinition(
    PerformanceIsolationModule Module,
    string Label,
    string Description);

internal static class PerformanceIsolationCatalog
{
    internal static IReadOnlyList<PerformanceIsolationDefinition> All { get; } =
        new[]
        {
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.LocalPlayerSampling,
                "Local player memory sampling",
                "Stops launcher ReadProcessMemory sampling for local position, yaw and movement profile."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.PositionNetworkTransmit,
                "Position network transmit",
                "Stops publishing local position snapshots while keeping the lobby connected."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.RemoteCharacterSpawn,
                "Remote Character spawn / convergence",
                "Stops new remote Character spawn and convergence. Existing Characters are not despawned."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.RemoteMovement,
                "Remote movement replication",
                "Stops remote Character movement commands and disables the native movement follower."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.EquipmentSync,
                "Equipment replication",
                "Stops PlayerSync equipment snapshots and remote equipment convergence."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.ActionSync,
                "Aim / Primary replication",
                "Stops PlayerSync Aim and Primary events and releases currently applied remote actions."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.AISync,
                "AISync / NPC authority",
                "Stops AISync traffic and native proxy requests, restoring local retail AI."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.WorldSync,
                "WorldSync / CATHODE",
                "Closes the WorldSync bus and bypasses native WorldSync/CATHODE replication hooks."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.UiBridge,
                "Scaleform / UI bridge",
                "Stops the launcher Scaleform TCP bridge while leaving lobby IPC and network connected."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.EngineThreadEvidence,
                "Native engine-thread evidence",
                "Disables native engine-thread evidence and capture work."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.CharacterLifecycleCapture,
                "Native Character lifecycle capture",
                "Disables native Character creation and lifecycle observation while preserving safety cleanup."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.GameThreadPumps,
                "Native game-thread pumps",
                "Disables dispatcher, replay and movement pumps while keeping the IPC control channel live."),
            new PerformanceIsolationDefinition(
                PerformanceIsolationModule.Diagnostics,
                "Diagnostics / log recording",
                "Stops launcher/native diagnostic recording and the locomotion flight recorder.")
        };

    internal static PerformanceIsolationDefinition Get(PerformanceIsolationModule module) =>
        All.First(definition => definition.Module == module);
}

internal sealed class PerformanceIsolationState
{
    private const int AllMask = (1 << 13) - 1;
    private int _mask = AllMask;

    internal bool IsEnabled(PerformanceIsolationModule module) =>
        (Volatile.Read(ref _mask) & Bit(module)) != 0;

    internal void SetEnabled(PerformanceIsolationModule module, bool enabled)
    {
        var bit = Bit(module);
        while (true)
        {
            var current = Volatile.Read(ref _mask);
            var next = enabled ? current | bit : current & ~bit;
            if (current == next || Interlocked.CompareExchange(ref _mask, next, current) == current)
                return;
        }
    }

    private static int Bit(PerformanceIsolationModule module) => 1 << (int)module;
}
