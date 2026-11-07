using AIMP.Protocol;
using System.Runtime.InteropServices;

namespace AIMP.Launcher;

// One worker owns each planner. Scratch storage is reused and retains only
// lightweight ordering keys. No transaction payloads or native authority live here.
internal sealed class AISyncRetryPlanner
{
    internal const int MaxHostPerTick = 8, MaxLocalPerTick = 8;
    internal const long HostRetryIntervalMs = 120, LocalRetryIntervalMs = 80;
    private readonly record struct Actor(ulong Peer, uint A, uint B, uint Birth,
        uint Class, AISyncTransactionCapability Capability, ulong OwnerEpoch);
    private readonly record struct OrderedActor(Actor Actor, ulong Sequence);
    private readonly Dictionary<Actor, AISyncIntentRetryPriority.Selection> _groups = new();
    private readonly List<OrderedActor> _order = new();
    private readonly List<Actor> _retired = new();
    private readonly HashSet<Actor> _bypassNext = new();
    private readonly Comparison<OrderedActor> _compare;
    private int _cursor;

    internal AISyncRetryPlanner() => _compare = Compare;

    internal (ulong Peer, ulong Sequence)[] ChooseCycle(
        IEnumerable<(ulong Peer, AISyncTransaction Operation)> waiting, int limit)
    {
        if (limit <= 0) return Array.Empty<(ulong, ulong)>();
        Collect(waiting);
        if (_order.Count == 0) { Reset(); return Array.Empty<(ulong, ulong)>(); }
        _order.Sort(_compare);
        var start = (int)((uint)_cursor % (uint)_order.Count);
        var count = Math.Min(limit, _order.Count);
        var selected = new (ulong Peer, ulong Sequence)[count];
        for (var i = 0; i < count; ++i) selected[i] = Select(_order[(start + i) % _order.Count].Actor);
        _cursor = (start + count) % _order.Count;
        return selected;
    }

    private void Collect(IEnumerable<(ulong Peer, AISyncTransaction Operation)> waiting)
    {
        _groups.Clear(); _order.Clear();
        foreach (var (peer, operation) in waiting) Add(peer, operation);
        foreach (var group in _groups) _order.Add(new(group.Key, group.Value.First.Sequence));
        _retired.Clear();
        foreach (var actor in _bypassNext) if (!_groups.ContainsKey(actor)) _retired.Add(actor);
        foreach (var actor in _retired) _bypassNext.Remove(actor);
    }

    private void Add(ulong peer, in AISyncTransaction operation)
    {
        var actor = new Actor(peer, operation.EntityA, operation.EntityB, operation.Birth,
            operation.CharacterClass, operation.Capability, operation.OwnerEpoch);
        ref var group = ref CollectionsMarshal.GetValueRefOrAddDefault(_groups, actor, out _);
        group.Add(operation);
    }

    private static int Compare(OrderedActor first, OrderedActor second)
    {
        var sequence = first.Sequence.CompareTo(second.Sequence);
        return sequence != 0 ? sequence : first.Actor.Peer.CompareTo(second.Actor.Peer);
    }

    private (ulong Peer, ulong Sequence) Select(Actor actor)
    {
        var group = _groups[actor];
        var bypass = group.Bypass();
        if (bypass == 0) _bypassNext.Remove(actor);
        else if (!_bypassNext.Add(actor)) { _bypassNext.Remove(actor); return (actor.Peer, bypass); }
        return (actor.Peer, group.Primary());
    }

    internal void Reset()
    {
        _groups.Clear(); _order.Clear(); _retired.Clear(); _bypassNext.Clear(); _cursor = 0;
    }
}
