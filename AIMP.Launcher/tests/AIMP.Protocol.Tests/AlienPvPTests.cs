namespace AIMP.Protocol.Tests;

// Pure lobby/network authority tests. Native Alien possession is covered by
// later game-thread tests, never implied by selecting this mode in the menu.
internal static class AlienPvPTests
{
    internal static Task ValidationAsync()
    {
        var session = Guid.NewGuid();
        var normal = new LobbyStartPayload(session, 1, "ChallengeMap16", 1);
        ModelValidation.Validate(normal);
        AssertEx.False(normal.AlienVsSurvivors);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(normal with
        {
            AlienPeerId = 42
        }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(normal with
        {
            AlienVsSurvivors = true
        }));
        var pvp = normal with { AlienVsSurvivors = true, AlienPeerId = 42 };
        ModelValidation.Validate(pvp);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(pvp with
        {
            AlienEnabled = false
        }));
        var players = new List<LobbyPlayer>
        {
            new(1, Guid.NewGuid(), "Host", true, true),
            new(2, Guid.NewGuid(), "Alien", false, true)
        };
        var lobby = new LobbySnapshotPayload(session, "Test", "ChallengeMap16", "Lobby",
            2, 0, players, AlienVsSurvivors: true, AlienPeerId: 2);
        ModelValidation.Validate(lobby);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(lobby with
        {
            AlienPeerId = 3
        }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(lobby with
        {
            AlienVsSurvivors = false
        }));
        var serialized = JsonPayload.Deserialize<LobbySnapshotPayload>(JsonPayload.Serialize(lobby));
        AssertEx.True(serialized.AlienVsSurvivors);
        AssertEx.Equal(2UL, serialized.AlienPeerId);
        return Task.CompletedTask;
    }

    internal static async Task LobbyAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version,
            "test", "test", new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Alien PvP", "Amanda", Guid.NewGuid(),
            0, 2, "ChallengeMap16", compatibility, AlienVsSurvivors: true));
        host.Start();
        await using var client = await LobbyClient.ConnectAsync(new(
            "127.0.0.1", host.BoundPort, Guid.NewGuid(), "Xenomorph", compatibility), token);
        client.Start();
        await client.SetReadyAsync(true, token);
        await WaitUntilAsync(() => client.Snapshot?.Players.Count == 2
            && client.Snapshot.Players.Single(p => p.PeerId == client.PeerId).Ready, token);
        AssertEx.False(host.CanStart);
        // No alien is implicitly chosen by the client's display name or Ready state.
        AssertEx.Equal(0UL, client.Snapshot!.AlienPeerId);
        await host.SetAlienPlayerAsync(client.PeerId, token);
        await WaitUntilAsync(() => client.Snapshot?.AlienPeerId == client.PeerId, token);
        AssertEx.True(host.CanStart);
        await host.SetAlienPlayerAsync(host.HostPeerId, token);
        await WaitUntilAsync(() => client.Snapshot?.AlienPeerId == host.HostPeerId, token);
        AssertEx.Equal(host.HostPeerId, client.Snapshot!.AlienPeerId);
        await host.SetAlienPlayerAsync(client.PeerId, token);
        var start = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        AssertEx.True(start.AlienVsSurvivors);
        AssertEx.Equal(client.PeerId, start.AlienPeerId);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(start with
        {
            AlienPeerId = 0
        }));
    }

    private static async Task WaitUntilAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token);
    }
}
