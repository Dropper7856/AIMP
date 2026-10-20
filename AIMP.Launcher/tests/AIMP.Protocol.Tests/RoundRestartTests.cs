using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class RoundRestartTests
{
    internal static Task AuthorityGenerationsAsync()
    {
        var counter = new SessionWorldGenerationCounter();
        var session = Guid.NewGuid();
        AssertEx.Equal(1, counter.Next(session));
        AssertEx.Equal(2, counter.Next(session), "A new round must not reuse the previous world's journal identity.");
        AssertEx.Equal(3, counter.Next(session), "An additional world activation must also have a unique generation.");
        AssertEx.Equal(1, counter.Next(Guid.NewGuid()), "A different lobby owns a separate generation space.");
        return Task.CompletedTask;
    }

    internal static async Task GameplaySequencesAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version,
            "test-launcher", "test-dll", new string('A', 64), "test-profile", new string('B', 64));
        await using var host = new LobbyHost(new LobbyHostOptions("Restart test", "Ripley", Guid.NewGuid(),
            0, 2, "TestLevel", compatibility));
        host.Start();
        await using var client = await LobbyClient.ConnectAsync(new LobbyClientOptions("127.0.0.1",
            host.BoundPort, Guid.NewGuid(), "Dallas", compatibility), timeout.Token).ConfigureAwait(false);
        client.Start();
        await client.SetReadyAsync(true, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart, timeout.Token).ConfigureAwait(false);
        var positions = 0; var equipment = 0; var actions = 0;
        host.PlayerPositionReceived += (_, _) => Interlocked.Increment(ref positions);
        host.PlayerEquipmentReceived += (_, _) => Interlocked.Increment(ref equipment);
        host.PlayerActionReceived += (_, _) => Interlocked.Increment(ref actions);
        var first = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        await SendAndFlushAsync(host, client, first.LevelGeneration, 1000, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(1, positions); AssertEx.Equal(1, equipment); AssertEx.Equal(1, actions);
        await host.ReportHostPlayerDeathAsync(first.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await client.SendPlayerDeathAsync(first.LevelGeneration, timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.State == "Lobby", timeout.Token).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart, timeout.Token).ConfigureAwait(false);
        var second = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        await SendAndFlushAsync(host, client, second.LevelGeneration, 1, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(2, positions); AssertEx.Equal(2, equipment); AssertEx.Equal(2, actions);
        // Duplicate packets and a delayed packet from round one must remain
        // rejected without poisoning the new round's low sequence numbers.
        await SendAndFlushAsync(host, client, second.LevelGeneration, 1, timeout.Token).ConfigureAwait(false);
        await SendAndFlushAsync(host, client, first.LevelGeneration, 1001, timeout.Token,
            second.LevelGeneration).ConfigureAwait(false);
        await SendAndFlushAsync(host, client, second.LevelGeneration, 2, timeout.Token).ConfigureAwait(false);
        AssertEx.Equal(3, positions); AssertEx.Equal(3, equipment); AssertEx.Equal(3, actions);
    }

    private static async Task SendAndFlushAsync(LobbyHost host, LobbyClient client, int generation,
        ulong sequence, CancellationToken token, int? activeGeneration = null)
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        await client.SendPlayerPositionAsync(new(host.SessionId, client.PeerId, generation, sequence,
            now, 1, 2, 3, 0, PlayerMoveType.Walk, 1), token).ConfigureAwait(false);
        await client.SendPlayerEquipmentAsync(new(host.SessionId, client.PeerId, generation, sequence,
            now, 4, 4, true, 0, false, false), token).ConfigureAwait(false);
        await client.SendPlayerActionAsync(new(host.SessionId, client.PeerId, generation, sequence,
            now, PlayerActionKind.Aim, true, 4, 0), token).ConfigureAwait(false);
        var seen = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void Ready(object? sender, GameplayReadyPayload value) => seen.TrySetResult();
        host.GameplayReadyReceived += Ready;
        try
        {
            await client.SendGameplayReadyAsync(activeGeneration ?? generation, token).ConfigureAwait(false);
            await seen.Task.WaitAsync(token).ConfigureAwait(false);
        }
        finally { host.GameplayReadyReceived -= Ready; }
    }

    private static async Task WaitUntilAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token).ConfigureAwait(false);
    }
}
