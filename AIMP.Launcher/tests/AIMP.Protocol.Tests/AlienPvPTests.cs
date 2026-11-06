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
        var nativeProbe = new GameCommandExecutePayload(
            ProtocolConstants.ExperimentalSchemaVersion, Guid.NewGuid(), 1,
            Guid.NewGuid(), GameCommandNames.AlienPvPProbe,
            new Dictionary<string, string>());
        ModelValidation.Validate(nativeProbe);
        // Possession controls are local launcher->DLL commands only. No peer
        // can smuggle a raw Character pointer or a second Alien owner.
        var nativeBind = nativeProbe with
        {
            Command = GameCommandNames.AlienPvPBind,
            Arguments = new Dictionary<string, string> { ["ownerPeer"] = "42" }
        };
        ModelValidation.Validate(nativeBind);
        ModelValidation.Validate(nativeBind with
        {
            Arguments = new Dictionary<string, string> { ["ownerPeer"] = ulong.MaxValue.ToString() }
        });
        foreach (var invalid in new[] { "", "0", "-1", "+1", "1.0", "0x2A", "18446744073709551616" })
        {
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(nativeBind with
            {
                Arguments = new Dictionary<string, string> { ["ownerPeer"] = invalid }
            }));
        }
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(nativeBind with
        {
            Arguments = new Dictionary<string, string> { ["ownerPeer"] = "42", ["character"] = "1234" }
        }));
        ModelValidation.Validate(nativeProbe with
        {
            Command = GameCommandNames.AlienPvPRelease
        });
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(nativeProbe with
        {
            Command = GameCommandNames.AlienPvPRelease,
            Arguments = new Dictionary<string, string> { ["force"] = "1" }
        }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(nativeBind with
        {
            LevelGeneration = 0
        }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(nativeProbe with
        {
            Arguments = new Dictionary<string, string> { ["alienPointer"] = "1234" }
        }));
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
        var delivered = new TaskCompletionSource<AlienControlCommit>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        client.AlienControlCommitted += (_, update) => delivered.TrySetResult(update);
        var firstIntent = new AlienControlIntent(host.SessionId, client.PeerId,
            start.LevelGeneration, 1, 1F, 0F, 90F, 0F, true, false, AlienAction.None);
        await client.SendAlienControlAsync(firstIntent, token);
        var committed = await delivered.Task.WaitAsync(token);
        AssertEx.Equal(client.PeerId, committed.Intent.AlienPeerId);
        AssertEx.Equal(1UL, committed.HostSequence);
        AssertEx.Throws<ProtocolException>(() => AlienControlProtocol.Validate(firstIntent with
        {
            Forward = 2F
        }));
        AssertEx.Throws<ProtocolException>(() => AlienControlProtocol.Validate(firstIntent with
        {
            Action = (AlienAction)255
        }));
        AssertEx.Throws<ProtocolException>(() => AlienControlProtocol.Validate(firstIntent with
        {
            Sprint = true, Sneak = true
        }));
        await AssertEx.ThrowsAsync<ProtocolException>(() =>
            host.SubmitHostAlienControlAsync(firstIntent with
            {
                AlienPeerId = host.HostPeerId, Sequence = 2
            }, token));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(start with
        {
            AlienPeerId = 0
        }));

        // Win/lose and repeated-round protocol must not depend on the Alien's
        // hidden technical THE_PLAYER reaching the usual solo end state.
        var roundEnded = new TaskCompletionSource<RoundEndedPayload>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        host.RoundEnded += (_, ended) => roundEnded.TrySetResult(ended);
        await host.ReportHostPlayerDeathAsync(start.LevelGeneration, token);
        AssertEx.Equal("AlienWin", (await roundEnded.Task.WaitAsync(token)).Reason);
        await WaitUntilAsync(() => host.CanStart && client.Snapshot?.State == "Lobby", token);

        // The Host resets its command sequence on a new round. Client must
        // reset its accepted Host sequence or it will drop the first input.
        var nextCommitted = new TaskCompletionSource<AlienControlCommit>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        client.AlienControlCommitted += (_, next) =>
        {
            if (next.Intent.LevelGeneration > start.LevelGeneration)
                nextCommitted.TrySetResult(next);
        };
        var nextRound = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await WaitUntilAsync(() => client.Snapshot?.LevelGeneration ==
            nextRound.LevelGeneration, token);
        await client.SendAlienControlAsync(firstIntent with
        {
            LevelGeneration = nextRound.LevelGeneration, Sequence = 1
        }, token);
        var firstAfterRestart = await nextCommitted.Task.WaitAsync(token);
        AssertEx.Equal(1UL, firstAfterRestart.HostSequence);
        AssertEx.Equal(nextRound.LevelGeneration, firstAfterRestart.Intent.LevelGeneration);
    }

    private static async Task WaitUntilAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token);
    }
}
