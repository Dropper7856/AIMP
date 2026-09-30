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
