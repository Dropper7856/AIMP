using AIMP.Launcher;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Reflection;

namespace AIMP.Protocol.Tests;

internal static class AISyncShadowBridgeTests
{
    internal static async Task ConnectionAndGenerationFencesAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version, "test", "test",
            new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Shadow bridge", "Ripley", Guid.NewGuid(), 0, 2, "TestLevel", compatibility));
        host.Start();
        await using var client = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Dallas", compatibility), token);
        client.Start(); await client.SetReadyAsync(true, token);
        await Wait(() => host.CanStart, token);
        var round = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await Wait(() => client.Snapshot?.State == "Starting", token);
        using var temporary = new TemporaryDirectory();
        using var diagnostics = new LauncherDiagnostics(temporary.Combine("logs"));
        var connection = Guid.NewGuid();
        var binding = new BindingBox(new(host, host.SessionId, round.LevelGeneration, 1,
            1234, connection, 1, true));
        var batches = new ConcurrentQueue<AISyncShadowData>();
        var receipts = new ConcurrentQueue<AISyncShadowReceipt>();
        client.AISyncShadowObservationBatchReceived += (_, batch) => batches.Enqueue(batch);
        host.AISyncShadowReceiptReceived += (_, receipt) => receipts.Enqueue(receipt);
        var raw = Observation(host.SessionId, round.LevelGeneration, 1, 1);
        var valid = new NativeAISyncObservation(connection, 1234, raw);
        using (var disabled = new AISyncShadowSession(diagnostics, binding.Read, enabled: false))
        {
            disabled.Observe(valid);
            AssertEx.True(disabled.Completion.IsCompleted);
            AssertEx.Equal(0, PeerCount(host));
        }
        using var bridge = new AISyncShadowSession(diagnostics, binding.Read, enabled: true);
        bridge.BindSession(host.SessionId);
        bridge.BindHost(host); bridge.BindClient(client);
        bridge.Observe(valid with { ConnectionIdentity = Guid.NewGuid() });
        bridge.Observe(valid with { ProcessId = 9999 });
        bridge.Observe(valid with { Bytes = Observation(host.SessionId, round.LevelGeneration, 2, 1) });
        bridge.Observe(valid);
        await Wait(() => receipts.Count >= 1, token);
        AssertEx.Equal(1, batches.Count);
        AssertEx.SequenceEqual(raw, batches.Single().Observations.Single());
        var firstStream = batches.Single().Stream;

        var nextConnection = Guid.NewGuid();
        binding.Write(binding.Read() with { ConnectionIdentity = nextConnection, Epoch = 2 });
        bridge.Observe(valid); // A delayed callback cannot borrow the new pipe's scope.
        bridge.Observe(valid with { ConnectionIdentity = nextConnection });
        await Wait(() => receipts.Count >= 2, token);
        AssertEx.Equal(2, batches.Count);
        AssertEx.True(batches.Last().Stream.StreamGuid != firstStream.StreamGuid);
        bridge.Observe(valid with { ConnectionIdentity = nextConnection,
            Bytes = Observation(host.SessionId, round.LevelGeneration, 1, 2) });
        await Wait(() => receipts.Count >= 3, token);
        AssertEx.Equal(3, batches.Select(batch => batch.Stream.StreamGuid).Distinct().Count());
        AssertEx.True(receipts.All(receipt => receipt.AppliedOrdinal == 0));
        binding.Write(binding.Read() with { Active = false });
        await Wait(() => PeerCount(host) == 0, token);
        // Even an unobserved earlier binding is remembered before its native
        // configure. A collision cannot be laundered through a new pipe nonce.
        var colliding = host.SessionId.ToByteArray(); colliding[15] ^= 1;
        var alias = new Guid(colliding);
        AssertEx.Equal(AISyncCodec.SessionCookie(host.SessionId), AISyncCodec.SessionCookie(alias));
        bridge.BindSession(alias);
        binding.Write(binding.Read() with { Active = true, Epoch = 3 });
        var refused = bridge.RejectedCount;
        bridge.Observe(valid with { ConnectionIdentity = nextConnection });
        AssertEx.Equal(refused + 1, bridge.RejectedCount);
        AssertEx.Equal(0, PeerCount(host));
        bridge.Dispose();
        await bridge.Completion.WaitAsync(token);
        AssertEx.Equal("Starting", client.Snapshot!.State);
    }

    private static byte[] Observation(Guid session, int world, ulong nativeGeneration, ulong capture)
    {
        var bytes = new byte[216];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, AISyncShadowCodec.NativeMagic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), 216);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), capture);
        var local = new byte[16];
        BinaryPrimitives.WriteUInt64LittleEndian(local, AISyncCodec.SessionCookie(session));
        "AIMPLOCL"u8.CopyTo(local.AsSpan(8));
        AISyncTransactionCodec.Encode(new AISyncTransaction {
            Session = new Guid(local), AuthorityEpoch = 1, WorldEpoch = (ulong)world,
            ConsumerEpoch = 1, OwnerEpoch = nativeGeneration, EntityA = 1, Birth = 1, CharacterClass = 1,
            CommandId = 1, GlobalSequence = 1, OwnerSequence = 1, ParameterRevision = 1,
            Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
            Flags = AISyncTransactionFlags.Incomplete | AISyncTransactionFlags.ObservationOnly
        }, bytes.AsSpan(56));
        return bytes;
    }
    private static int PeerCount(LobbyHost host)
    {
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        lock (typeof(LobbyHost).GetField("_stateLock", flags)!.GetValue(host)!)
            return ((System.Collections.IDictionary)typeof(LobbyHost).GetField("_shadowPeers", flags)!.GetValue(host)!).Count;
    }
    private static async Task Wait(Func<bool> predicate, CancellationToken token)
    {
        while (!predicate()) await Task.Delay(2, token);
    }
    private sealed class BindingBox(AISyncShadowBinding value)
    {
        private readonly object _gate = new();
        public AISyncShadowBinding Read() { lock (_gate) return value; }
        public void Write(AISyncShadowBinding next) { lock (_gate) value = next; }
    }
}
