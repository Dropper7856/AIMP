using System.Buffers.Binary;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class AlienOptionTests
{
    internal static Task ConfigurationAsync()
    {
        var session = Guid.NewGuid();
        var peers = new Dictionary<ulong, uint>();
        var enabled = AISyncCodec.Configuration(1, 1, 1, session, 1, peers);
        var disabled = AISyncCodec.Configuration(1, 1, 1, session, 1, peers, alienEnabled: false);
        var retail = AISyncCodec.Configuration(0, 0, 0, Guid.Empty, 1, peers, alienEnabled: false);
        AssertEx.Equal(0U, BinaryPrimitives.ReadUInt32LittleEndian(enabled.AsSpan(36)));
        AssertEx.Equal(1U, BinaryPrimitives.ReadUInt32LittleEndian(disabled.AsSpan(36)));
        AssertEx.Equal(0U, BinaryPrimitives.ReadUInt32LittleEndian(retail.AsSpan(36)));
        AssertEx.SequenceEqual(enabled.AsSpan(0, 36).ToArray(), disabled.AsSpan(0, 36).ToArray());
        return Task.CompletedTask;
    }

    internal static Task PayloadDefaultsAsync()
    {
        var payload = new LobbyStartPayload(Guid.NewGuid(), 1, "TestLevel", 1);
        AssertEx.True(payload.AlienEnabled);
        var disabled = JsonPayload.Deserialize<LobbyStartPayload>(
            JsonPayload.Serialize(payload with { AlienEnabled = false }));
        AssertEx.False(disabled.AlienEnabled);
        var legacy = Encoding.UTF8.GetString(JsonPayload.Serialize(payload));
        legacy = legacy.Replace(",\"alienEnabled\":true", "", StringComparison.Ordinal);
        AssertEx.False(legacy.Contains("alienEnabled", StringComparison.Ordinal));
        AssertEx.True(JsonPayload.Deserialize<LobbyStartPayload>(Encoding.UTF8.GetBytes(legacy)).AlienEnabled);
        return Task.CompletedTask;
    }

    internal static async Task LobbyRoundRestartAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version,
            "test", "test", new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Exploration", "Ripley", Guid.NewGuid(),
            0, 2, "TestLevel", compatibility, AlienEnabled: false));
        host.Start();
        await using var client = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Dallas", compatibility), token).ConfigureAwait(false);
        var started = new TaskCompletionSource<LobbyStartPayload>(TaskCreationOptions.RunContinuationsAsynchronously);
        client.Started += (_, value) => started.TrySetResult(value);
        client.Start();
        await client.SetReadyAsync(true, token).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart && client.Snapshot is not null, token).ConfigureAwait(false);
        AssertEx.False(client.Snapshot!.AlienEnabled);
        var first = await host.StartGameAsync(DateTimeOffset.UtcNow, token).ConfigureAwait(false);
        AssertEx.False((await started.Task.WaitAsync(token).ConfigureAwait(false)).AlienEnabled);
        AssertEx.False(first.AlienEnabled);
        await host.ReportHostPlayerDeathAsync(first.LevelGeneration, token).ConfigureAwait(false);
        await client.SendPlayerDeathAsync(first.LevelGeneration, token).ConfigureAwait(false);
        await WaitUntilAsync(() => client.Snapshot?.State == "Lobby" && host.CanStart, token).ConfigureAwait(false);
        AssertEx.False(client.Snapshot!.AlienEnabled);
        AssertEx.False((await host.StartGameAsync(DateTimeOffset.UtcNow, token).ConfigureAwait(false)).AlienEnabled);
    }

    private static async Task WaitUntilAsync(Func<bool> ready, CancellationToken token)
    {
        while (!ready()) await Task.Delay(10, token).ConfigureAwait(false);
    }
}
