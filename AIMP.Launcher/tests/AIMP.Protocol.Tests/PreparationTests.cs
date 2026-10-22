using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class PreparationTests
{
    internal static async Task LobbyManifestPreflightAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await using var host = CreateHost(2);
        host.Start();
        await using var client = await ConnectAsync(host, "Dallas", timeout.Token).ConfigureAwait(false);
        client.Start();
        await client.SetReadyAsync(true, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.Players.Any(player =>
            player.PeerId == client.PeerId && player.Ready) == true).ConfigureAwait(false);

        AssertEx.False(host.CanStart, "Ready checkboxes alone cannot bypass required world preparation.");
        await AssertStartRejectedAsync(host, timeout.Token).ConfigureAwait(false);
        var originalToken = client.Snapshot!.PreparationToken;
        AssertEx.True(originalToken != Guid.Empty);
        var hostReport = Report(host, host.HostPeerId, originalToken);
        await host.SetHostPreparationAsync(hostReport, timeout.Token).ConfigureAwait(false);
        var wrongContent = Report(host, client.PeerId, originalToken) with
        {
            ManifestSha256 = new string('D', 64)
        };
        await client.SendLobbyPreparationAsync(wrongContent, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.Preparations?.Any(report =>
            report.PeerId == client.PeerId && report.ManifestSha256 == wrongContent.ManifestSha256) == true)
            .ConfigureAwait(false);
        AssertEx.False(host.CanStart, "Different runtime content digests must block launch.");
        await AssertStartRejectedAsync(host, timeout.Token).ConfigureAwait(false);

        var wrongMap = Report(host, client.PeerId, originalToken) with
        {
            Manifest = Manifest(new WorldEntityId(3UL, 4UL))
        };
        await client.SendLobbyPreparationAsync(wrongMap, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.Preparations?.Any(report =>
            report.PeerId == client.PeerId && report.Manifest == wrongMap.Manifest) == true)
            .ConfigureAwait(false);
        AssertEx.False(host.CanStart, "Equal digests cannot substitute for matching manifest identity.");
        await AssertStartRejectedAsync(host, timeout.Token).ConfigureAwait(false);

        await client.SendLobbyPreparationAsync(Report(host, client.PeerId, originalToken), timeout.Token)
            .ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart).ConfigureAwait(false);
        host.SetLevel("TestLevel");
        await WaitUntilAsync(() => client.Snapshot?.PreparationToken == originalToken).ConfigureAwait(false);
        AssertEx.True(host.CanStart, "Selecting the same map must not invalidate prepared participants.");

        host.RefreshWorldPreparation();
        await WaitUntilAsync(() => client.Snapshot?.PreparationToken != originalToken).ConfigureAwait(false);
        var refreshed = client.Snapshot!.PreparationToken;
        AssertEx.False(host.CanStart, "An explicit new-start preflight must invalidate previous readiness.");
        await AssertEx.ThrowsAsync<InvalidOperationException>(() =>
            host.SetHostPreparationAsync(hostReport, timeout.Token)).ConfigureAwait(false);
        await host.SetHostPreparationAsync(Report(host, host.HostPeerId, refreshed), timeout.Token)
            .ConfigureAwait(false);
        await client.SendLobbyPreparationAsync(Report(host, client.PeerId, originalToken), timeout.Token)
            .ConfigureAwait(false);
        // A subsequent frame on the SAME socket proves the stale report has
        // been processed; no timing sleep is used to assert its rejection.
        await client.SetReadyAsync(false, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.Players.Any(player =>
            player.PeerId == client.PeerId && !player.Ready) == true).ConfigureAwait(false);
        AssertEx.False(client.Snapshot!.Preparations!.Any(report => report.PeerId == client.PeerId),
            "A stale-token Client report must not survive refresh.");
        await client.SetReadyAsync(true, timeout.Token).ConfigureAwait(false);
        await client.SendLobbyPreparationAsync(Report(host, client.PeerId, refreshed), timeout.Token)
            .ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart).ConfigureAwait(false);
        var started = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(1, started.LevelGeneration);
    }

    internal static async Task SessionReleaseBarrierAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        await using var host = CreateHost(3);
        host.Start();
        await using var first = await ConnectAsync(host, "Dallas", timeout.Token).ConfigureAwait(false);
        first.Start();
        await using var second = await ConnectAsync(host, "Lambert", timeout.Token).ConfigureAwait(false);
        second.Start();
        var hostReleases = 0;
        var firstReleases = 0;
        var secondReleases = 0;
        host.SessionReleased += (_, _) => Interlocked.Increment(ref hostReleases);
        first.SessionReleased += (_, _) => Interlocked.Increment(ref firstReleases);
        second.SessionReleased += (_, _) => Interlocked.Increment(ref secondReleases);
        await PrepareEveryoneAsync(host, first, second, timeout.Token).ConfigureAwait(false);
        var previous = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => first.Snapshot?.LevelGeneration == previous.LevelGeneration
            && second.Snapshot?.LevelGeneration == previous.LevelGeneration).ConfigureAwait(false);
        await host.SetHostSessionPreparedAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(0, Volatile.Read(ref hostReleases), "Host readiness alone must not release gameplay.");
        await first.SendSessionPreparedAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await FlushClientFramesAsync(host, first, previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(0, Volatile.Read(ref hostReleases), "Every connected Client must finish loading.");
        await second.SendSessionPreparedAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => Volatile.Read(ref hostReleases) == 1
            && Volatile.Read(ref firstReleases) == 1 && Volatile.Read(ref secondReleases) == 1)
            .ConfigureAwait(false);

        await first.SendSessionPreparedAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await FlushClientFramesAsync(host, first, previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => Volatile.Read(ref firstReleases) == 2).ConfigureAwait(false);
        AssertEx.Equal(1, Volatile.Read(ref secondReleases), "A late report releases only its sender.");
        AssertEx.Equal(1, Volatile.Read(ref hostReleases), "A Client retry must not re-signal the Host.");

        // Start another generation to exercise a genuinely OLD readiness frame.
        await host.ReportHostPlayerDeathAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await first.SendPlayerDeathAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await second.SendPlayerDeathAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => first.Snapshot?.State == "Lobby" && second.Snapshot?.State == "Lobby")
            .ConfigureAwait(false);
        host.RefreshWorldPreparation();
        await PrepareEveryoneAsync(host, first, second, timeout.Token).ConfigureAwait(false);
        var current = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(previous.LevelGeneration + 1, current.LevelGeneration);
        await WaitUntilAsync(() => first.Snapshot?.LevelGeneration == current.LevelGeneration
            && second.Snapshot?.LevelGeneration == current.LevelGeneration).ConfigureAwait(false);
        await host.SetHostSessionPreparedAsync(current.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await second.SendSessionPreparedAsync(current.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await FlushClientFramesAsync(host, second, current.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await first.SendSessionPreparedAsync(previous.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await FlushClientFramesAsync(host, first, current.LevelGeneration, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(1, Volatile.Read(ref hostReleases), "Old-generation readiness must never release a new round.");
        await first.SendSessionPreparedAsync(current.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => Volatile.Read(ref hostReleases) == 2
            && Volatile.Read(ref firstReleases) == 3 && Volatile.Read(ref secondReleases) == 2)
            .ConfigureAwait(false);
    }

    internal static async Task MissionCatalogBarrierAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await using var host = CreateHost(3);
        host.Start();
        await using var first = await ConnectAsync(host, "Dallas", timeout.Token).ConfigureAwait(false);
        first.Start();
        await using var second = await ConnectAsync(host, "Lambert", timeout.Token).ConfigureAwait(false);
        second.Start();
        var releases = 0;
        var failures = 0;
        var peerErrors = 0;
        host.SessionReleased += (_, _) => Interlocked.Increment(ref releases);
        host.MissionPreparationFailed += (_, _) => Interlocked.Increment(ref failures);
        first.ErrorReceived += (_, error) => { if (error.Code == "MISSION_CATALOG_MISMATCH") Interlocked.Increment(ref peerErrors); };
        await PrepareEveryoneAsync(host, first, second, timeout.Token).ConfigureAwait(false);
        var round = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => first.Snapshot?.LevelGeneration == round.LevelGeneration
            && second.Snapshot?.LevelGeneration == round.LevelGeneration).ConfigureAwait(false);
        var hash = new string('A', 64);
        // A Client may finish first. Comparison begins when the Host reports its own catalog.
        await first.SendSessionPreparedAsync(round.LevelGeneration, timeout.Token, new string('B', 64)).ConfigureAwait(false);
        await second.SendSessionPreparedAsync(round.LevelGeneration, timeout.Token, hash).ConfigureAwait(false);
        await FlushClientFramesAsync(host, first, round.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await FlushClientFramesAsync(host, second, round.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await host.SetHostSessionPreparedAsync(round.LevelGeneration, timeout.Token, hash).ConfigureAwait(false);
        await WaitUntilAsync(() => Volatile.Read(ref failures) > 0 && Volatile.Read(ref peerErrors) > 0).ConfigureAwait(false);
        AssertEx.Equal(0, Volatile.Read(ref releases), "A different mission graph must keep every player behind the loading barrier.");
        await first.SendSessionPreparedAsync(round.LevelGeneration, timeout.Token, hash).ConfigureAwait(false);
        await WaitUntilAsync(() => Volatile.Read(ref releases) == 1).ConfigureAwait(false);
    }

    private static LobbyHost CreateHost(int capacity) => new(new LobbyHostOptions(
        "Preparation test", "Ripley", Guid.NewGuid(), 0, capacity, "TestLevel", Compatibility(),
        RequireWorldPreparation: true));

    private static CompatibilityInfo Compatibility() => new(ProtocolConstants.Version,
        "test-launcher", "test-dll", new string('A', 64), "test-profile", new string('B', 64));

    private static Task<LobbyClient> ConnectAsync(LobbyHost host, string name, CancellationToken token) =>
        LobbyClient.ConnectAsync(new LobbyClientOptions("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), name, Compatibility()), token);

    private static WorldManifestIdentity Manifest(WorldEntityId map) => new(
        WorldManifestConstants.FormatVersion, WorldManifestConstants.CompilerVersion, map);

    private static LobbyPreparationPayload Report(LobbyHost host, ulong peer, Guid token) => new(
        host.SessionId, peer, "TestLevel", token, Manifest(new WorldEntityId(1UL, 2UL)),
        new string('C', 64), true, string.Empty);

    private static async Task PrepareEveryoneAsync(
        LobbyHost host, LobbyClient first, LobbyClient second, CancellationToken token)
    {
        await WaitUntilAsync(() => first.Snapshot?.PreparationToken == second.Snapshot?.PreparationToken
            && first.Snapshot?.Preparations?.Count == 0).ConfigureAwait(false);
        var preparationToken = first.Snapshot!.PreparationToken;
        await host.SetHostPreparationAsync(Report(host, host.HostPeerId, preparationToken), token)
            .ConfigureAwait(false);
        await first.SendLobbyPreparationAsync(Report(host, first.PeerId, preparationToken), token)
            .ConfigureAwait(false);
        await second.SendLobbyPreparationAsync(Report(host, second.PeerId, preparationToken), token)
            .ConfigureAwait(false);
        await first.SetReadyAsync(true, token).ConfigureAwait(false);
        await second.SetReadyAsync(true, token).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart).ConfigureAwait(false);
    }

    private static async Task FlushClientFramesAsync(
        LobbyHost host, LobbyClient client, int generation, CancellationToken token)
    {
        var observed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnReady(object? sender, GameplayReadyPayload value)
        {
            if (value.PeerId == client.PeerId && value.LevelGeneration == generation)
                observed.TrySetResult();
        }
        host.GameplayReadyReceived += OnReady;
        try
        {
            await client.SendGameplayReadyAsync(generation, token).ConfigureAwait(false);
            await observed.Task.WaitAsync(TimeSpan.FromSeconds(3), token).ConfigureAwait(false);
        }
        finally { host.GameplayReadyReceived -= OnReady; }
    }

    private static async Task AssertStartRejectedAsync(LobbyHost host, CancellationToken token) =>
        await AssertEx.ThrowsAsync<InvalidOperationException>(() =>
            host.StartGameAsync(DateTimeOffset.UtcNow, token)).ConfigureAwait(false);

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (!condition())
        {
            if (DateTime.UtcNow >= deadline) throw new TimeoutException("Preparation condition was not reached.");
            await Task.Delay(10).ConfigureAwait(false);
        }
    }
}
