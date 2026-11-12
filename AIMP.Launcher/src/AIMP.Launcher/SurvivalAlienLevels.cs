namespace AIMP.Launcher;

// Survival levels whose retail COMMANDS.PAK instantiates
// Archetypes\NPCs\Alien\Xenomorph_NPC from the level root. Alien vs Survivors
// leases that Alien to a player, so it is only offered on these levels.
// CHALLENGEMAP3 (androids only) and CHALLENGEMAP9 (androids, eggs and
// facehuggers) have no Xenomorph. Unknown levels fail closed.
internal static class SurvivalAlienLevels
{
    internal static readonly IReadOnlyList<string> Levels =
    [
        "BSPNOSTROMO_TWOTEAMS",
        "CHALLENGEMAP1",
        "CHALLENGEMAP4",
        "CHALLENGEMAP5",
        "CHALLENGEMAP7",
        "CHALLENGEMAP11",
        "CHALLENGEMAP12",
        "CHALLENGEMAP14",
        "CHALLENGEMAP16",
        "SALVAGEMODE1",
        "SALVAGEMODE2"
    ];

    internal static bool HasAlien(string level) =>
        Levels.Contains(level.Trim(), StringComparer.OrdinalIgnoreCase);

    internal const string UnsupportedMessage =
        "Alien vs Survivors needs a survival map with the Alien. This map has no Alien.";
}
