using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class ThrowableSyncTests
{
    private static byte[] Release(Guid session, ulong peer, int generation, ulong sequence = 1)
    {
        var bytes = new byte[ThrowableSyncCodec.EventBytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), 37); // sender-native, not receiver-native
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)generation);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), AISyncCodec.SessionCookie(session));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), peer);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), sequence);
        Float(bytes, 48, 1); Float(bytes, 68, 1); Float(bytes, 88, 1);
        Float(bytes, 60, 120); Float(bytes, 100, 12); Float(bytes, 108, 45);
        Encoding.ASCII.GetBytes(@"Required_Assets\Archetypes\Equipment\test_released").CopyTo(bytes, 112);
        return bytes;
    }

    private static void Float(byte[] bytes, int offset, float value) =>
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(offset), BitConverter.SingleToInt32Bits(value));
    private static byte[] End(byte[] release)
    {
        var ended = release.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(ended.AsSpan(12), 2);
        return ended;
    }

    internal static Task CodecAsync()
    {
        var session = Guid.NewGuid();
        var release = Release(session, 123, 9, 88);
        ThrowableSyncCodec.Validate(release);
        ThrowableSyncCodec.Validate(End(release));
        AssertEx.Equal(123UL, ThrowableSyncCodec.Peer(release));
        AssertEx.Equal(88UL, ThrowableSyncCodec.Sequence(release));
        AssertEx.Equal(9, ThrowableSyncCodec.Generation(release));
        AssertEx.Equal(37U, ThrowableSyncCodec.NativeGeneration(release));
        var config = ThrowableSyncCodec.Configuration(session, 17, 9, 123, true);
        AssertEx.Equal(32, config.Length);
        AssertEx.Equal(17U, BinaryPrimitives.ReadUInt32LittleEndian(config.AsSpan(8)));
        AssertEx.Equal(9U, BinaryPrimitives.ReadUInt32LittleEndian(config.AsSpan(12)));
        foreach (var offset in new[] { 0, 4, 8, 12, 16, 24, 32 })
        {
            var invalid = release.ToArray();
            invalid.AsSpan(offset, offset >= 16 ? 8 : 4).Clear();
            AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(invalid));
        }
        foreach (var offset in new[] { 48, 60, 96, 108 })
        foreach (var value in new[] { float.NaN, float.PositiveInfinity, 100001F })
        {
            var invalid = release.ToArray(); Float(invalid, offset, value);
            AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(invalid));
        }
        var negative = release.ToArray(); Float(negative, 108, -1);
        AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(negative));
        var fast = release.ToArray(); Float(fast, 96, 1001);
        AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(fast));
        var path = release.ToArray(); path[271] = 1;
        AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(path));
        AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(release.AsSpan(0, 271)));
        var reserved = release.ToArray(); reserved[44] = 1;
        AssertEx.Throws<ProtocolException>(() => ThrowableSyncCodec.Validate(reserved));
        return Task.CompletedTask;
    }

    internal static async Task LobbyAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version, "test", "test",
            new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Throwables", "Ripley", Guid.NewGuid(), 0, 3, "TestLevel", compatibility));
        host.Start();
        using var raw = new TcpClient();
        await raw.ConnectAsync("127.0.0.1", host.BoundPort, token);
        var stream = raw.GetStream();
        await Send(MessageType.LobbyJoin, JsonPayload.Serialize(new LobbyJoinPayload(Guid.NewGuid(), "Dallas", compatibility, 123)));
        var acceptedFrame = await FrameCodec.ReadAsync(stream, token);
        var accepted = JsonPayload.Deserialize<LobbyJoinAcceptedPayload>(acceptedFrame.Payload.Span);
        await FrameCodec.ReadAsync(stream, token);
        await using var observer = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Lambert", compatibility), token);
        observer.Start();
        await Send(MessageType.LobbyReady, JsonPayload.Serialize(new LobbyReadyPayload(true)));
        await observer.SetReadyAsync(true, token);
        await Wait(() => host.CanStart);
        var proposals = new List<byte[]>(); var commits = new List<byte[]>();
        host.ThrowableProposed += (_, bytes) => { lock (proposals) proposals.Add(bytes.ToArray()); };
        observer.ThrowableCommitted += (_, bytes) => { lock (commits) commits.Add(bytes.ToArray()); };
        var round = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        var one = Release(host.SessionId, accepted.PeerId, round.LevelGeneration);
        var two = Release(host.SessionId, accepted.PeerId, round.LevelGeneration, 2);
        await Send(MessageType.ThrowableProposal, one);
        await Send(MessageType.ThrowableProposal, one); // duplicate must not reach native twice
        await Send(MessageType.ThrowableProposal, Release(host.SessionId, accepted.PeerId, round.LevelGeneration + 1, 99));
        await Send(MessageType.ThrowableProposal, two); // ordered barrier for rejected carriers above
        await Wait(() => Count(proposals) == 2);
        AssertEx.Equal(0, Count(commits), "A proposal must not broadcast before native Host admission.");
        await AssertEx.ThrowsAsync<ProtocolException>(() => observer.SendThrowableProposalAsync(one, token));
        await host.CommitThrowableAsync(one, token);
        await host.CommitThrowableAsync(one, token);
        await host.CommitThrowableAsync(End(one), token);
        await host.CommitThrowableAsync(End(one), token);
        await host.CommitThrowableAsync(two, token);
        await Wait(() => Count(commits) == 3);
        lock (commits)
            AssertEx.SequenceEqual(new uint[] { 1, 2, 1 }, commits.Select(bytes => ThrowableSyncCodec.Operation(bytes)).ToArray());

        await host.ReportHostPlayerDeathAsync(round.LevelGeneration, token);
        await Send(MessageType.PlayerDeath, JsonPayload.Serialize(new PlayerDeathPayload(host.SessionId, accepted.PeerId, round.LevelGeneration)));
        await observer.SendPlayerDeathAsync(round.LevelGeneration, token);
        await Wait(() => observer.Snapshot?.State == "Lobby" && host.CanStart);
        var next = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        var renewed = Release(host.SessionId, accepted.PeerId, next.LevelGeneration);
        await Send(MessageType.ThrowableProposal, one); // stale round
        await Send(MessageType.ThrowableProposal, renewed); // sequence 1 belongs to a new generation
        await Wait(() => Count(proposals) == 3);
        await host.CommitThrowableAsync(renewed, token);
        await host.CommitThrowableAsync(End(one), token); // stale end must not terminate new lifetime
        await host.CommitThrowableAsync(End(renewed), token);
        await host.CommitThrowableAsync(Release(host.SessionId, host.HostPeerId, next.LevelGeneration), token);
        await Wait(() => Count(commits) == 6);
        // The raw authenticated socket cannot propose the Host's owner id.
        await Send(MessageType.ThrowableProposal, Release(host.SessionId, host.HostPeerId, next.LevelGeneration, 99));
        try
        {
            while (true) await FrameCodec.ReadAsync(stream, token);
        }
        catch (EndOfStreamException) { } // Host closes the authenticated sender on forged ownership.
        AssertEx.Equal(3, Count(proposals));

        Task Send(MessageType type, byte[] bytes) => FrameCodec.WriteAsync(stream, new AimpFrame(type, 0, 1, bytes), token).AsTask();
        async Task Wait(Func<bool> predicate) { while (!predicate()) await Task.Delay(10, token); }
    }
    private static int Count(List<byte[]> values) { lock (values) return values.Count; }
}
