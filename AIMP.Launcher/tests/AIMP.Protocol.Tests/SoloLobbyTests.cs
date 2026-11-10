namespace AIMP.Protocol.Tests;

// A Private lobby is solo: one slot, so it starts without any client and
// rejects every join. Multi-slot lobbies still need a ready client.
internal static class SoloLobbyTests
{
    internal static async Task StartsAloneAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await using var host = CreateHost(1);
        host.Start();
        AssertEx.True(host.IsSolo && host.CanStart, "a one-slot lobby can start with no client");
        var ended = new TaskCompletionSource<RoundEndedPayload>(TaskCreationOptions.RunContinuationsAsynchronously);
        host.RoundEnded += (_, value) => ended.TrySetResult(value);
        var start = await host.StartGameAsync(DateTimeOffset.UtcNow, timeout.Token).ConfigureAwait(false);
        await host.ReportHostPlayerDeathAsync(start.LevelGeneration, timeout.Token).ConfigureAwait(false);
        var round = await ended.Task.WaitAsync(timeout.Token).ConfigureAwait(false);
        AssertEx.True(round.LevelGeneration == start.LevelGeneration, "the solo round ends when the host dies");
    }

    internal static async Task RejectsJoinAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await using var host = CreateHost(1);
        host.Start();
        var rejected = false;
        try
        {
            await using var client = await LobbyClient.ConnectAsync(new LobbyClientOptions("127.0.0.1",
                host.BoundPort, Guid.NewGuid(), "Dallas", Compatibility()), timeout.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException) { rejected = true; }
        AssertEx.True(rejected, "a solo lobby has no slot for another player");
    }

    internal static async Task MultiplayerNeedsClientAsync()
    {
        await using var host = CreateHost(4);
        host.Start();
        AssertEx.True(!host.IsSolo && !host.CanStart, "a multi-slot lobby still waits for a ready client");
    }

    private static LobbyHost CreateHost(int capacity) => new(new LobbyHostOptions(
        "Solo test", "Ripley", Guid.NewGuid(), 0, capacity, "TestLevel", Compatibility()));

    private static CompatibilityInfo Compatibility() => new(ProtocolConstants.Version,
        "test-launcher", "test-dll", new string('A', 64), "test-profile", new string('B', 64));
}
