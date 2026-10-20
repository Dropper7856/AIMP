namespace AIMP.Launcher;

internal static class LocalPlayerCharacterResolver
{
    // Same THE_PLAYER chain as native PlayerSyncObserver / PlayerLifecycle.
    // The global Character pool's first slot is not necessarily the local player.
    internal static bool TryResolve(nint moduleBase, Func<nint, nint?> pointer,
        Func<nint, int?> integer, out nint character)
    {
        character = default;
        if (pointer(moduleBase + PlayerSyncMemoryLayout.LocalPlayerRootPointerOffset) is not { } root
            || root == 0
            || pointer(root + PlayerSyncMemoryLayout.RootContextOffset) is not { } context
            || context == 0
            || integer(context + PlayerSyncMemoryLayout.LocalPlayerCountOffset) is not > 0
            || pointer(context + PlayerSyncMemoryLayout.LocalPlayerArrayOffset) is not { } players
            || players == 0
            || pointer(players) is not { } player || player == 0) return false;
        character = player;
        return true;
    }
}
