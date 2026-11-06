using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class PlayerCharacterTests
{
    internal static async Task LobbyAssignmentAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version,
            "test", "test", new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Crew", "Host", Guid.NewGuid(),
            0, 3, "TestLevel", compatibility));
        host.Start();
        await using var first = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Dallas", compatibility), timeout.Token);
        first.Start();
        await using var second = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Parker", compatibility), timeout.Token);
        second.Start();
        while (first.Snapshot?.Players.Count != 3)
            await Task.Delay(10, timeout.Token);
        var roster = second.Snapshot!.Players;
        AssertEx.Equal("E_RIPLEY", roster.Single(p => p.IsHost).Character!);
        AssertEx.Equal("DALLAS", roster.Single(p => p.DisplayName == "Dallas").Character!);
        AssertEx.Equal("PARKER", roster.Single(p => p.DisplayName == "Parker").Character!);
        foreach (var player in roster)
            AssertEx.Equal(player.Character!, first.Snapshot!.Players.Single(p => p.PeerId == player.PeerId).Character!);
        // No gameplay-ready event or legacy spawn roster was needed.
    }

    internal static Task CatalogAsync()
    {
        AssertEx.Equal(5, PlayerCharacterCatalog.All.Count);
        AssertEx.Equal(PlayerCharacterCatalog.DefaultName, PlayerCharacterCatalog.All[0].Name);
        AssertEx.Equal(5, PlayerCharacterCatalog.All.Select(c => c.Name).Distinct().Count());
        foreach (var character in PlayerCharacterCatalog.All)
        {
            // Clip DB / skeleton definition identity, as hashed by the patcher.
            AssertEx.Equal(AnimClipDatabase.HashName(character.Name), character.NameHash);
            AssertEx.Equal(character.NameHash, PlayerCharacterCatalog.HashName(character.Name));
        }
        AssertEx.Equal(569978346U, PlayerCharacterCatalog.Find("E_RIPLEY")!.NameHash);
        AssertEx.True(PlayerCharacterCatalog.Find("dallas") is null);
        AssertEx.True(!PlayerCharacterCatalog.IsKnown("BISHOP") && !PlayerCharacterCatalog.IsKnown(null));
        var profiles = RemoteCrouchAnimationInstaller.Profiles.ToDictionary(p => p.Character);
        AssertEx.Equal("FEMALENPC", profiles["LAMBERT"].LowRes);
        AssertEx.Equal(0, profiles["E_RIPLEY"].ExtraSources.Length);
        foreach (var male in new[] { "DALLAS", "PARKER", "ASH" })
        {
            AssertEx.Equal("MALE", profiles[male].LowRes);
            AssertEx.True(profiles[male].ExtraSources.SequenceEqual(["FEMALE"]));
        }
        AssertEx.Equal(@"DATA\ANIM_SYS\569978346_ANIM_CLIP_DB.BIN", profiles["E_RIPLEY"].ClipDbEntry);
        AssertEx.Equal(@"DATA\ANIM_SYS\SKELE\DEFS\569978346.BIN", profiles["E_RIPLEY"].SkeletonDefEntry);
        return Task.CompletedTask;
    }

    internal static Task AssignmentAsync()
    {
        var assigned = new Dictionary<ulong, string>();
        var first = PlayerCharacterCatalog.Assign(assigned, [30, 10, 20], 10);
        AssertEx.Equal("E_RIPLEY", first[10]);
        AssertEx.Equal(3, first.Values.Distinct().Count());
        // Stable across rounds; a departed player frees its character for a newcomer.
        var second = PlayerCharacterCatalog.Assign(assigned, [10, 20, 30], 10);
        AssertEx.True(first.OrderBy(p => p.Key).SequenceEqual(second.OrderBy(p => p.Key)));
        var freed = first[30];
        var third = PlayerCharacterCatalog.Assign(assigned, [10, 20, 40], 10);
        AssertEx.Equal(first[20], third[20]);
        AssertEx.Equal(freed, third[40]);
        AssertEx.True(!third.ContainsKey(30));
        // More players than characters: the extras stay unassigned (E_RIPLEY default).
        var crowd = PlayerCharacterCatalog.Assign(new Dictionary<ulong, string>(), [1, 2, 3, 4, 5, 6], 1);
        AssertEx.Equal(5, crowd.Count);
        AssertEx.Equal(5, crowd.Values.Distinct().Count());
        return Task.CompletedTask;
    }

    internal static Task WireAsync()
    {
        var session = Guid.NewGuid();
        var payload = new SpawnRemotePlayersPayload(session, Guid.NewGuid(), 1, 1,
        [
            new(1, 1, 2, 3, "E_RIPLEY"),
            new(2, 4, 5, 6, "DALLAS"),
        ]);
        ModelValidation.Validate(payload);
        AssertEx.Equal(payload.Players[1], JsonPayload.Deserialize<SpawnRemotePlayersPayload>(JsonPayload.Serialize(payload)).Players[1]);
        ModelValidation.Validate(payload with { Players = [new(1, 1, 2, 3), new(2, 4, 5, 6)] });
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(payload with
            { Players = [new(1, 1, 2, 3, "DALLAS"), new(2, 4, 5, 6, "DALLAS")] }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(payload with
            { Players = [new(1, 1, 2, 3, "BISHOP"), new(2, 4, 5, 6)] }));

        var spawn = RemoteSpawnPlanner.Create(new PlayerSpawnPositionPayload(2, 1, 2, 3, "PARKER"));
        AssertEx.Equal("PARKER", spawn["character"]);
        AssertEx.Equal("E_RIPLEY", RemoteSpawnPlanner.Create(new PlayerSpawnPositionPayload(2, 1, 2, 3))["character"]);
        ModelValidation.Validate(Command(spawn));
        var withoutCharacter = new Dictionary<string, string>(spawn);
        withoutCharacter.Remove("character");
        ModelValidation.Validate(Command(withoutCharacter));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Command(
            new Dictionary<string, string>(spawn) { ["character"] = "BISHOP" })));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Command(
            new Dictionary<string, string>(spawn) { ["skin"] = "DALLAS" })));
        return Task.CompletedTask;
    }

    private static GameCommandExecutePayload Command(IReadOnlyDictionary<string, string> arguments) =>
        new(1, Guid.NewGuid(), 1, Guid.NewGuid(),
            GameCommandNames.SpawnCharacter, arguments);
}
