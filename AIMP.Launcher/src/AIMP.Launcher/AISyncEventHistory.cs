using AIMP.Protocol;

namespace AIMP.Launcher;

// Called under AISyncSession's gate, after codec validation. Each channel has
// its own sequence: a movement revision cannot acknowledge a shot or a hit.
internal sealed class AISyncEventHistory
{
    private readonly Dictionary<(ulong Actor, byte Channel), (uint Birth, uint Pair, uint Revision)> _seen = new();

    internal void Clear() => _seen.Clear();

    internal bool Observe(ReadOnlySpan<byte> bytes)
    {
        var actor = AISyncCodec.ActorKey(bytes);
        var outcome = AISyncCodec.OutcomeKey(bytes);
        var damage = AISyncCodec.DamageKey(bytes);
        var participant = AISyncCodec.ParticipantDamageKey(bytes);
        var shot = AISyncCodec.ShotKey(bytes);
        var navigation = AISyncCodec.NavigationKey(bytes);
        // Evaluate every channel even when an earlier one is new.
        return Remember(actor, 0, outcome)
            | Remember(actor, 1, (damage.Birth, 0, damage.Revision))
            | Remember(actor, 2, (participant.Birth, 0, participant.Revision))
            | Remember(actor, 3, (shot.Birth, 0, shot.Revision))
            | Remember(actor, 4, (navigation.Birth, 0, navigation.Revision));
    }

    private bool Remember(ulong actor, byte channel, (uint Birth, uint Pair, uint Revision) stamp)
    {
        if (stamp.Revision == 0) return false;
        var key = (actor, channel);
        if (_seen.TryGetValue(key, out var previous) && stamp.CompareTo(previous) <= 0) return false;
        if (!_seen.ContainsKey(key) && _seen.Count >= AISyncCodec.MaximumActors * 5)
            _seen.Remove(_seen.Keys.First());
        _seen[key] = stamp;
        return true;
    }
}
