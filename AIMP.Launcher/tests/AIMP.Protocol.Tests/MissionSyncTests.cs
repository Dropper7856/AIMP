using System.Text;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class MissionSyncTests
{
    private const string Entity = "1234567890ABCDEF0000000012345678";

    internal static Task ModelAsync()
    {
        var valid = new MissionSignalPayload(Guid.NewGuid(), 42, 1, 1, Entity, 123, 1);
        MissionSignalValidation.Validate(valid);
        var produced = MissionTestLabReceipts.Format(valid, "client", "native-produced");
        var committed = MissionTestLabReceipts.Format(valid with { Sequence = 999 }, "host", "commit-observed");
        AssertEx.True(produced.StartsWith("TLR1 v=1 module=MissionSync stage=native-produced ", StringComparison.Ordinal));
        AssertEx.True(produced.Contains(" order=0", StringComparison.Ordinal));
        AssertEx.True(committed.Contains(" order=999", StringComparison.Ordinal));
        var observedId = produced.Split(' ').Single(part => part.StartsWith("operation=", StringComparison.Ordinal));
        AssertEx.True(committed.Contains(observedId, StringComparison.Ordinal),
            "A Host-global commit sequence must retain the original proposal identity.");
        AssertEx.True(!produced.Contains(valid.SessionId.ToString("N"), StringComparison.OrdinalIgnoreCase));
        AssertEx.True(!produced.Contains(valid.EntityId, StringComparison.OrdinalIgnoreCase));
        AssertEx.Equal(valid, JsonPayload.Deserialize<MissionSignalPayload>(JsonPayload.Serialize(valid)));
        MissionSignalValidation.Validate(valid with { InterfaceSelector = 2, Sequence = ulong.MaxValue });
        foreach (var invalid in new[] { valid with { SessionId = Guid.Empty }, valid with { PeerId = 0 },
            valid with { LevelGeneration = 0 }, valid with { Sequence = 0 }, valid with { Signal = 0 },
            valid with { EntityId = "123" }, valid with { EntityId = new string('0', 32) },
            valid with { EntityId = new string('G', 32) }, valid with { InterfaceSelector = 0 },
            valid with { InterfaceSelector = 3 } })
            AssertEx.Throws<ProtocolException>(() => MissionSignalValidation.Validate(invalid));
        var json = Encoding.UTF8.GetString(JsonPayload.Serialize(valid));
        foreach (var missing in new[] { "\"sequence\":1,", "\"signal\":123,", "\"interfaceSelector\":1" })
        {
            var malformed = json.Replace(missing, "", StringComparison.Ordinal).Replace(",}", "}", StringComparison.Ordinal);
            AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<MissionSignalPayload>(Encoding.UTF8.GetBytes(malformed)));
        }
        return Task.CompletedTask;
    }

    internal static async Task LobbyAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version, "test", "test",
            new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Mission test", "Ripley", Guid.NewGuid(), 0, 3,
            "TestLevel", compatibility));
        host.Start();
        await using var client = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Dallas", compatibility), token);
        await using var observer = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Lambert", compatibility), token);
        client.Start(); observer.Start();
        await client.SetReadyAsync(true, token); await observer.SetReadyAsync(true, token);
        await WaitAsync(() => host.CanStart, token);
        var atHost = new List<MissionSignalPayload>();
        var atClient = new List<MissionSignalPayload>();
        var atObserver = new List<MissionSignalPayload>();
        host.MissionSignalCommitted += (_, value) => { lock (atHost) atHost.Add(value); };
        client.MissionSignalCommitted += (_, value) => { lock (atClient) atClient.Add(value); };
        observer.MissionSignalCommitted += (_, value) => { lock (atObserver) atObserver.Add(value); };
        var round = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await WaitAsync(() => client.Snapshot?.State == "Starting" && observer.Snapshot?.State == "Starting", token);
        // Distinct frontiers per sequence; a frontier is committed once per round.
        MissionSignalPayload Signal(ulong peer, int generation, ulong sequence) =>
            new(host.SessionId, peer, generation, sequence, Entity,
                (peer == host.HostPeerId ? 100U : 200U) + (uint)sequence, 1);
        var tasks = Enumerable.Range(1, 10).Select(sequence => host.ReportHostMissionSignalAsync(
            Signal(host.HostPeerId, round.LevelGeneration, (ulong)sequence), token)).ToList();
        tasks.Add(client.SendMissionSignalAsync(Signal(client.PeerId, round.LevelGeneration, 10), token));
        tasks.Add(client.SendMissionSignalAsync(Signal(client.PeerId, round.LevelGeneration, 11), token));
        await Task.WhenAll(tasks);
        var log = new List<string>();
        host.Log += (_, message) => { lock (log) log.Add(message); };
        try { await WaitAsync(() => Count(atHost) == 12 && Count(atClient) == 12 && Count(atObserver) == 12, token); }
        catch (OperationCanceledException)
        {
            throw new InvalidOperationException($"Commits host={Count(atHost)} client={Count(atClient)} " +
                $"observer={Count(atObserver)}; Host log: {string.Join(" | ", log)}");
        }
        AssertEx.SequenceEqual(Copy(atHost), Copy(atClient));
        AssertEx.SequenceEqual(Copy(atHost), Copy(atObserver));
        AssertEx.SequenceEqual(Enumerable.Range(1, 12).Select(value => (ulong)value).ToArray(),
            Copy(atHost).Select(value => value.Sequence).ToArray());
        AssertEx.Equal(2, Copy(atHost).Count(value => value.PeerId == client.PeerId));
        // Replay protection is per source; dropping replay cannot create a commit gap.
        await host.ReportHostMissionSignalAsync(Signal(host.HostPeerId, round.LevelGeneration, 10), token);
        await client.SendMissionSignalAsync(Signal(client.PeerId, round.LevelGeneration, 11), token);
        // Another participant raising an already committed frontier changes nothing.
        await client.SendMissionSignalAsync(Signal(client.PeerId, round.LevelGeneration, 12) with
            { Signal = 101U }, token);
        await host.ReportHostMissionSignalAsync(Signal(host.HostPeerId, round.LevelGeneration, 13) with
            { Signal = 210U }, token);
        await Task.Delay(200, token);
        AssertEx.Equal(12, Count(atHost));
        AssertEx.Equal(12, Count(atClient));
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => client.SendMissionSignalAsync(
            Signal(host.HostPeerId, round.LevelGeneration, 12), token));
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => host.ReportHostMissionSignalAsync(
            Signal(client.PeerId, round.LevelGeneration, 12), token));
        await host.ReportHostPlayerDeathAsync(round.LevelGeneration, token);
        await client.SendPlayerDeathAsync(round.LevelGeneration, token);
        await observer.SendPlayerDeathAsync(round.LevelGeneration, token);
        await WaitAsync(() => client.Snapshot?.State == "Lobby" && observer.Snapshot?.State == "Lobby"
            && host.CanStart, token);
        var next = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await WaitAsync(() => client.Snapshot?.LevelGeneration == next.LevelGeneration
            && observer.Snapshot?.LevelGeneration == next.LevelGeneration, token);
        await host.ReportHostMissionSignalAsync(Signal(host.HostPeerId, round.LevelGeneration, 99), token);
        await host.ReportHostMissionSignalAsync(Signal(host.HostPeerId, next.LevelGeneration, 1), token);
        await WaitAsync(() => Count(atClient) == 13 && Count(atObserver) == 13, token);
        AssertEx.Equal(13, Count(atHost));
        AssertEx.Equal(1UL, Copy(atClient)[12].Sequence);
        AssertEx.Equal(next.LevelGeneration, Copy(atClient)[12].LevelGeneration);

        // A hub terminal can start the same nested mission again later in the
        // round. Repeatable frontiers commit again after the duplicate window,
        // while a near-simultaneous duplicate proposal still collapses.
        host.SetMissionRepeatableFrontiers([(Entity.ToLowerInvariant(), 777U)], TimeSpan.FromMilliseconds(300));
        MissionSignalPayload Start(ulong peer, ulong sequence) =>
            new(host.SessionId, peer, next.LevelGeneration, sequence, Entity, 777U, 1);
        await client.SendMissionSignalAsync(Start(client.PeerId, 1), token);
        await host.ReportHostMissionSignalAsync(Start(host.HostPeerId, 2), token);
        await WaitAsync(() => Count(atClient) == 14, token);
        await Task.Delay(400, token);
        await client.SendMissionSignalAsync(Start(client.PeerId, 2), token);
        await WaitAsync(() => Count(atClient) == 15, token);
        await Task.Delay(100, token);
        AssertEx.Equal(15, Count(atHost));
        AssertEx.True(Copy(atClient).Skip(13).All(value => value.Signal == 777U));
    }

    private static int Count(List<MissionSignalPayload> values) { lock (values) return values.Count; }
    private static MissionSignalPayload[] Copy(List<MissionSignalPayload> values) { lock (values) return values.ToArray(); }
    private static async Task WaitAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token);
    }
}
