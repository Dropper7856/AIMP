namespace AIMP.Launcher;

// Owned by the coordinator for its entire lifetime, including native reloads.
// Only the current authenticated DLL snapshot may supply construction evidence.
// Counters belong to (session, peer), never an address or a level generation.
// No history eviction: exhaustion leaves the optional capability unavailable.
internal sealed class PlayerIncarnationAllocator(int capacity = 64)
{
    private readonly record struct Source(Guid Connection, int Process, int World,
        ulong Generation, ulong Serial, uint Character, uint Runtime);
    private sealed class Entry { internal Source Source; internal uint Counter; internal bool Active; }
    private readonly Dictionary<(Guid Session, ulong Peer), Entry> _entries = new();

    // Caller serializes observations and authenticates its current controller.
    // Zero retires a capability; it never means that an old token may resume.
    internal uint Observe(Guid session, ulong peer, int world, NativeRuntimeSnapshot state, Guid connection)
    {
        if (session == Guid.Empty || peer == 0) return 0;
        var valid = world > 0 && state.ProcessId > 0
            && NativeCharacterIncarnationBinding.MatchesCurrent(state, connection, state.LevelGeneration);
        var entry=GetEntry((session,peer),valid);
        if(entry is null)return 0;
        if (!valid) { entry.Active = false; return 0; }
        var source = new Source(connection, state.ProcessId, world, state.LocalCharacterGeneration,
            state.LocalCharacterSerial, state.LocalCharacterBase, state.LocalCharacterRuntimeId);
        return Advance(entry,source);
    }
    private Entry? GetEntry((Guid Session,ulong Peer) key,bool valid)
    {
        if(_entries.TryGetValue(key,out var entry))return entry;
        if(!valid||capacity<=0||_entries.Count>=capacity)return null;
        _entries.Add(key,entry=new());return entry;
    }
    private static uint Advance(Entry entry,Source source)
    {
        if (entry.Active && entry.Source == source) return entry.Counter;
        entry.Active = false;
        if (entry.Counter == uint.MaxValue) return 0;
        entry.Source = source; ++entry.Counter; entry.Active = true;
        return entry.Counter;
    }
}
