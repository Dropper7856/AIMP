namespace AIMP.Protocol;

// Nostromo crew taken from DLC level BSPNOSTROMO_TWOTEAMS (PODLC_TwoTeams):
// every one is a CIVILIAN/HUMANOID NPC whose anim_set, reference_skeleton and
// display_model share its name. NameHash is FNV-1a 32 of that name (clip DB,
// skeleton definition and spawn-request identity); DisplayModelGuid is the
// ShortGuid of its DisplayModel:<NAME> composite.
public sealed record PlayerCharacter(string Name, uint NameHash, uint DisplayModelGuid, bool Female);

public static class PlayerCharacterCatalog
{
    public const string DefaultName = "E_RIPLEY";

    // Assignment order: the Host takes the first character, then lobby order.
    public static readonly IReadOnlyList<PlayerCharacter> All =
    [
        new("E_RIPLEY", 0x21F92DEAU, 0xD9A16D5BU, true),
        new("DALLAS", 0xE4CDDDE2U, 0x70BD7431U, false),
        new("PARKER", 0x1A576592U, 0xEAC1FB77U, false),
        new("LAMBERT", 0xB473FB3AU, 0x358D9B30U, true),
        new("ASH", 0x5BAD7BDFU, 0x127E16ABU, false),
    ];

    public static PlayerCharacter? Find(string? name) =>
        name is null ? null : All.FirstOrDefault(c => string.Equals(c.Name, name, StringComparison.Ordinal));

    public static bool IsKnown(string? name) => Find(name) is not null;

    // FNV-1a 32, the retail animation/skeleton name hash.
    public static uint HashName(string name)
    {
        var hash = 2166136261U;
        foreach (var c in name) { hash ^= (byte)c; hash *= 16777619U; }
        return hash;
    }

    // One distinct character per player, decided by the Host and broadcast in
    // the spawn roster so every peer shows the same character. `assigned`
    // persists across rounds: a player keeps its character, departed players
    // free theirs, the Host is E_RIPLEY and newcomers take the first free one.
    // Beyond the catalog size a player stays unassigned (default E_RIPLEY).
    public static IReadOnlyDictionary<ulong, string> Assign(
        IDictionary<ulong, string> assigned, IReadOnlyCollection<ulong> peers, ulong hostPeerId)
    {
        foreach (var peer in assigned.Keys.Where(peer => !peers.Contains(peer)).ToArray()) assigned.Remove(peer);
        foreach (var peer in peers.OrderBy(peer => peer == hostPeerId ? 0 : 1))
        {
            if (assigned.ContainsKey(peer)) continue;
            var free = All.FirstOrDefault(c => !assigned.Values.Contains(c.Name, StringComparer.Ordinal));
            if (free is not null) assigned[peer] = free.Name;
        }
        return new Dictionary<ulong, string>(assigned);
    }
}
