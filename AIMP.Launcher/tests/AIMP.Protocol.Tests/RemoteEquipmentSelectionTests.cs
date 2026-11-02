using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class RemoteEquipmentSelectionTests
{
    internal static Task SelectionAsync()
    {
        // Slot 9 is shared by multiple item archetypes, including throwables.
        foreach (var current in new[] { -1, 7, 8, 9, 11, 64 })
        foreach (var active in new[] { false, true })
        {
            AssertEx.Equal(current, RemoteEquipmentSelection.VisibleIndex(current, 1, active));
            AssertEx.False(RemoteEquipmentSelection.SupportsActions(current, active));
        }
        AssertEx.Equal(1, RemoteEquipmentSelection.VisibleIndex(10, 1, false));
        foreach (var flare in new[] { 32, 33 })
        {
            AssertEx.Equal(flare, RemoteEquipmentSelection.VisibleIndex(flare, 4, true));
            AssertEx.False(RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(flare, out var id));
            AssertEx.Equal(string.Empty, id);
            AssertEx.False(RemoteEquipmentSelection.SupportsActions(flare, true));
        }
        // The public COMMANDS catalog must not offer any inventory-held
        // throwable for a replicated Character, even through manual Give.
        foreach (var id in new[]
        {
            "flare-held", "flare-held-infinite", "emp-mine-held",
            "explosive-mine-held", "flashbang-held", "incendiary-mine-held",
            "molotov-held", "noisemaker-held", "pipe-bomb-held", "smoke-bomb-held"
        })
        {
            AssertEx.True(RemoteEquipmentDefinitions.IsUnsafeRemoteHeldThrowable(id));
            AssertEx.True(RemoteEquipmentDefinitions.TryGet(id, out var definition));
            AssertEx.False(definition.SelectorEnabled);
        }
        // Ordinary NPC equipment stays available for replication.
        AssertEx.False(RemoteEquipmentDefinitions.IsUnsafeRemoteHeldThrowable("npc-pistol"));
        AssertEx.True(RemoteEquipmentDefinitions.TryGetPlayerSyncEquipmentId(1, out var pistol));
        AssertEx.Equal("npc-pistol", pistol);

        // Guns lose their inputs when the flare takes over. A lit durable
        // identity remains distinguishable after the short activation edge.
        AssertEx.True(RemoteEquipmentSelection.ActionEligibilityChanged(4, true, 32, true));
        AssertEx.False(RemoteEquipmentSelection.ActionEligibilityChanged(32, true, 33, true));
        // Raw aim/primary and the visible gun can remain identical while its
        // active bit drops. This transition must still publish/converge Stop.
        AssertEx.True(RemoteEquipmentSelection.ActionEligibilityChanged(1, true, 1, false));
        AssertEx.True(RemoteEquipmentSelection.ActionEligibilityChanged(1, false, 1, true));
        AssertEx.False(RemoteEquipmentSelection.ActionEligibilityChanged(1, true, 1, true));
        AssertEx.False(RemoteEquipmentSelection.ActionEligibilityChanged(9, true, 9, false));
        AssertEx.Equal(10, RemoteEquipmentSelection.VisibleIndex(10, 1, true));
        AssertEx.Equal(10, RemoteEquipmentSelection.VisibleIndex(10, 9, false));
        AssertEx.False(RemoteEquipmentSelection.SupportsActions(10, false));
        foreach (var current in Enumerable.Range(0, 7))
        {
            AssertEx.Equal(current, RemoteEquipmentSelection.VisibleIndex(current, 1, false));
            AssertEx.True(RemoteEquipmentSelection.SupportsActions(current, true));
            AssertEx.False(RemoteEquipmentSelection.SupportsActions(current, false));
        }
        // Gun -> item -> holster(previous gun) must produce distinct selections.
        AssertEx.Equal(9, RemoteEquipmentSelection.VisibleIndex(9, 1, true));
        AssertEx.Equal(1, RemoteEquipmentSelection.VisibleIndex(10, 1, false));
        return Task.CompletedTask;
    }

    internal static Task FlareActivationAsync()
    {
        var arguments = new Dictionary<string, string>
        {
            ["characterAddress"] = "0x200000",
            ["compositeName"] = @"Required_Assets\Archetypes\Equipment\Flare_Held",
            ["equipmentEntityShortGuid"] = "0x1234",
            ["equipmentOutputShortGuid"] = "0x2345",
            ["spawnSchedulerParameterShortGuid"] = "0x3456",
            ["spawnTargetInputShortGuid"] = "0x4567",
            ["equipmentSlot"] = "9"
        };
        var command = new GameCommandExecutePayload(1, Guid.NewGuid(), 1, Guid.NewGuid(),
            GameCommandNames.RemoteEquipmentProvision, arguments);
        ModelValidation.Validate(command); // Legacy wire schema remains valid; native execution now refuses unsafe held items.
        arguments["activateHeld"] = "true";
        ModelValidation.Validate(command);
        foreach (var value in new[] { "false", "1", "TRUE", "" })
        {
            arguments["activateHeld"] = value;
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command));
        }
        arguments["activateHeld"] = "true";
        arguments["releaseHeld"] = "true";
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command));
        arguments.Remove("activateHeld");
        ModelValidation.Validate(command); // release the attached flare after throwing
        arguments["releaseHeld"] = "false";
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command));
        arguments["releaseHeld"] = "true";
        arguments["equipmentSlot"] = "4";
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command));
        arguments["equipmentSlot"] = "9";
        arguments["compositeName"] = @"Required_Assets\Archetypes\Weapons\NPC_Flamethrower_Standard";
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command));
        return Task.CompletedTask;
    }

    internal static Task RetailFlareGraphAsync()
    {
        var path = Environment.GetEnvironmentVariable("AIMP_TEST_COMMANDS");
        if (string.IsNullOrEmpty(path))
        {
            Console.WriteLine("SKIP retail flare graph: AIMP_TEST_COMMANDS is not set.");
            return Task.CompletedTask;
        }
        var catalog = RemoteEquipmentCatalogCompiler.CompileAll(path, out var diagnostic);
        AssertEx.False(catalog.TryGet("flare-held", out _), diagnostic);
        AssertEx.True(catalog.Unavailable.TryGetValue("flare-held", out var reason), diagnostic);
        AssertEx.True(reason!.Contains("unsafe", StringComparison.OrdinalIgnoreCase), diagnostic);
        Console.WriteLine("Retail flare is intentionally excluded from remote Character equipment catalogs.");
        return Task.CompletedTask;
    }
}
