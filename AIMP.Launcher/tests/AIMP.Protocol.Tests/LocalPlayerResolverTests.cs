using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class LocalPlayerResolverTests
{
    internal static Task ResolvesRegisteredPlayerAsync()
    {
        nint module = 0x670000, root = 0x10000000, context = 0x11000000,
            table = 0x12000000, player = 0x13000000, unrelatedCharacter = 0x14000000;
        var pointers = new Dictionary<nint, nint>
        {
            [module + PlayerSyncMemoryLayout.LocalPlayerRootPointerOffset] = root,
            [root + PlayerSyncMemoryLayout.RootContextOffset] = context,
            [context + PlayerSyncMemoryLayout.LocalPlayerArrayOffset] = table,
            [table] = player,
            [module + LocalPlayerYawMemoryLayout.CharacterBasePointerOffset] = unrelatedCharacter
        };
        nint? Pointer(nint address) => pointers.TryGetValue(address, out var value) ? value : null;
        int? Count(nint address) => address == context + PlayerSyncMemoryLayout.LocalPlayerCountOffset ? 1 : null;
        AssertEx.True(LocalPlayerCharacterResolver.TryResolve(module, Pointer, Count, out var resolved));
        AssertEx.Equal(player, resolved, "The native local-player table must win over the global Character pool slot.");
        pointers.Remove(module + LocalPlayerYawMemoryLayout.CharacterBasePointerOffset);
        AssertEx.True(LocalPlayerCharacterResolver.TryResolve(module, Pointer, Count, out resolved));
        AssertEx.Equal(player, resolved, "A missing pool slot must not block local spawn readiness.");
        return Task.CompletedTask;
    }

    internal static Task RejectsBootstrapPoolSlotAsync()
    {
        nint module = 0x670000, root = 0x10000000, context = 0x11000000, table = 0x12000000;
        var pointers = new Dictionary<nint, nint>
        {
            [module + PlayerSyncMemoryLayout.LocalPlayerRootPointerOffset] = root,
            [root + PlayerSyncMemoryLayout.RootContextOffset] = context,
            [context + PlayerSyncMemoryLayout.LocalPlayerArrayOffset] = table,
            [table] = 0x13000000,
            [module + LocalPlayerYawMemoryLayout.CharacterBasePointerOffset] = 0x14000000
        };
        nint? Pointer(nint address) => pointers.TryGetValue(address, out var value) ? value : null;
        AssertEx.False(LocalPlayerCharacterResolver.TryResolve(module, Pointer, _ => 0, out _),
            "A populated generic pool is not evidence that THE_PLAYER has registered.");
        pointers.Remove(table);
        AssertEx.False(LocalPlayerCharacterResolver.TryResolve(module, Pointer, _ => 1, out _));
        return Task.CompletedTask;
    }
}
