using System.Buffers.Binary;
using System.Collections;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Reflection;

namespace AIMP.Protocol.Tests;

internal static class AISyncShadowLobbyTests
{
    private static readonly CompatibilityInfo Compatibility = new(ProtocolConstants.Version, "test", "test",
        new string('A', 64), "test", new string('B', 64));

    internal static async Task DeliveryAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = timeout.Token;
        await using var host = Host();
        AssertEx.Throws<InvalidOperationException>(() => host.StartAISyncShadow(1));
        await using var client = await Client(host, "Dallas", token);
        AssertEx.Equal(0, Peers(host).Length, "Normal lobby operation must not start shadow senders.");
        var batches = new ConcurrentQueue<AISyncShadowData>();
        var receipts = new ConcurrentQueue<AISyncShadowReceipt>();
        var stopped = new ConcurrentQueue<AISyncShadowRecovery>();
        client.AISyncShadowObservationBatchReceived += (_, batch) => batches.Enqueue(batch);
        client.AISyncShadowRecoveryRequired += (_, recovery) => stopped.Enqueue(recovery);
        host.AISyncShadowReceiptReceived += (_, receipt) => receipts.Enqueue(receipt);
        var round = await Start(host, new[] { client }, token);
        var stream = host.StartAISyncShadow(round.LevelGeneration);
        var expected = new List<byte[]>();
        for (ulong i = 1; i <= 24; i++)
        {
            var raw = Observation(stream, i);
            expected.Add(raw.ToArray());
            AssertEx.Equal(AISyncShadowResult.Accepted, await host.PublishAISyncShadowAsync(stream, raw, token));
            Array.Clear(raw); // Caller storage cannot mutate the journal or queued packets.
        }
        await Wait(() => receipts.Any(receipt => receipt.ReceivedOrdinal == 24), token);
        var delivered = batches.SelectMany(batch => batch.Observations).ToArray();
        AssertEx.Equal(expected.Count, delivered.Length);
        for (var i = 0; i < delivered.Length; i++) AssertEx.SequenceEqual(expected[i], delivered[i]);
        AssertEx.True(receipts.All(receipt => receipt.AppliedOrdinal == 0
            && receipt.VerifiedOrdinal == receipt.ReceivedOrdinal));
        AssertEx.True(batches.All(batch => batch.Stream == stream));
        AssertEx.Equal(3UL, BinaryPrimitives.ReadUInt64LittleEndian(delivered[0].AsSpan(48)),
            "Cumulative native loss is preserved, never reclassified as complete capture.");
        AssertEx.False(typeof(LobbyClient).GetMethods().Any(method => method.Name.Contains("PublishAISyncShadow")));
        AssertEx.Equal(AISyncShadowResult.Rejected, await host.PublishAISyncShadowAsync(
            stream with { SessionGuid = Guid.NewGuid() }, expected[0], token));
        var senders = Peers(host).Select(Sender).ToArray();
        await host.StopAISyncShadowAsync();
        await Wait(() => stopped.Any(value => value.Stream == stream), token);
        AssertEx.True(senders.All(task => task.IsCompleted));
        AssertEx.Equal(0, Peers(host).Length);
    }

    internal static async Task IsolationAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var token = timeout.Token;
        await using var host = Host();
        await using var slow = await Client(host, "Dallas", token);
        await using var fast = await Client(host, "Lambert", token);
        var received = 0;
        var fastLastReceipt = 0L;
        var recoveries = new ConcurrentQueue<AISyncShadowRecovery>();
        fast.AISyncShadowObservationBatchReceived += (_, batch) => Interlocked.Add(ref received, batch.Observations.Length);
        host.AISyncShadowReceiptReceived += (_, receipt) =>
        {
            if (receipt.ConsumerGuid == OpenFor(host, fast.PeerId)?.ConsumerGuid)
                Interlocked.Exchange(ref fastLastReceipt, (long)receipt.ReceivedOrdinal);
        };
        host.AISyncShadowRecoveryRequired += (_, recovery) => recoveries.Enqueue(recovery);
        var round = await Start(host, new[] { slow, fast }, token);
        // Hold the real connection's shared write gate: deterministic socket backpressure,
        // while both lobby readers and the healthy peer remain real loopback connections.
        var writeGate = WriteGate(host, slow.PeerId);
        await writeGate.WaitAsync(token);
        try
        {
            var stream = host.StartAISyncShadow(round.LevelGeneration);
            var slowConsumer = OpenFor(host, slow.PeerId)!.ConsumerGuid;
            for (var start = 1; start <= 2080; start += 32)
            {
                for (var i = start; i < start + 32; i++)
                {
                    var publish = host.PublishAISyncShadowAsync(stream, Observation(stream, (ulong)i), token);
                    AssertEx.True(publish.IsCompleted, "Publishing cannot await a peer's socket.");
                    AssertEx.Equal(AISyncShadowResult.Accepted, await publish);
                }
                await Wait(() => Volatile.Read(ref received) == start + 31, token);
                AssertBounds(host, 2);
            }
            await Wait(() => recoveries.Any(recovery => recovery.ConsumerGuid == slowConsumer), token);
            AssertEx.False(recoveries.Any(recovery => recovery.ConsumerGuid == OpenFor(host, fast.PeerId)!.ConsumerGuid));
            AssertEx.Equal(2080, Volatile.Read(ref received));
            AssertEx.Equal("Starting", fast.Snapshot!.State);
            writeGate.Release();
            await Wait(() => Interlocked.Read(ref fastLastReceipt) == 2080, token);
            // The slow peer's shadow recovery leaves its game connection alive.
            await slow.SendPlayerDeathAsync(round.LevelGeneration, token);
            await host.ReportHostPlayerDeathAsync(round.LevelGeneration, token);
            await fast.SendPlayerDeathAsync(round.LevelGeneration, token);
            await Wait(() => fast.Snapshot?.State == "Lobby", token);
        }
        finally { if (writeGate.CurrentCount == 0) writeGate.Release(); }
        AssertEx.Equal(0, Peers(host).Length);
    }

    internal static async Task RefusalAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var token = timeout.Token;
        await using (var host = Host())
        {
            await using var raw = await RawPeer.Connect(host, token);
            await using var healthy = await Client(host, "Lambert", token);
            var receipts = new ConcurrentQueue<AISyncShadowReceipt>();
            var recoveries = new ConcurrentQueue<AISyncShadowRecovery>();
            host.AISyncShadowReceiptReceived += (_, receipt) => receipts.Enqueue(receipt);
            host.AISyncShadowRecoveryRequired += (_, recovery) => recoveries.Enqueue(recovery);
            await raw.Send(MessageType.LobbyReady, JsonPayload.Serialize(new LobbyReadyPayload(true)), token);
            var round = await Start(host, new[] { healthy }, token);
            var stream = host.StartAISyncShadow(round.LevelGeneration);
            var open = AISyncShadowCodec.DecodeOpen((await raw.Read(MessageType.AISyncShadowOpen, token)).Payload.Span);
            await host.PublishAISyncShadowAsync(stream, Observation(stream, 1), token);
            var data = AISyncShadowCodec.DecodeData((await raw.Read(MessageType.AISyncShadowData, token)).Payload.Span);
            var receipt = new AISyncShadowReceipt(stream, open.ConsumerGuid, 1, 1, 0, data.Digest);
            await Rejected(receipt with { Stream = stream with { SessionGuid = Guid.NewGuid() } });
            await Rejected(receipt with { ConsumerGuid = Guid.NewGuid() });
            await Rejected(receipt with { ConsumerGuid = OpenFor(host, healthy.PeerId)!.ConsumerGuid });
            await Rejected(receipt with { ReceivedOrdinal = 2, VerifiedOrdinal = 2 });
            var appliedClaim = AISyncShadowCodec.EncodeReceipt(receipt);
            BinaryPrimitives.WriteUInt64LittleEndian(appliedClaim.AsSpan(88), 1);
            await raw.Send(MessageType.AISyncShadowReceipt, appliedClaim, token);
            await raw.Read(MessageType.Error, token);
            await raw.Send(MessageType.AISyncShadowReceipt, appliedClaim[..^1], token);
            await raw.Read(MessageType.Error, token);
            await raw.Send(MessageType.AISyncShadowReceipt, AISyncShadowCodec.EncodeReceipt(receipt), token);
            await Wait(() => receipts.Any(value => value.ConsumerGuid == open.ConsumerGuid), token);
            await Rejected(receipt with { ReceivedOrdinal = 0, VerifiedOrdinal = 0, Digest = new byte[32] });
            var next = host.StartAISyncShadow(round.LevelGeneration);
            var nextOpen = AISyncShadowCodec.DecodeOpen((await raw.Read(MessageType.AISyncShadowOpen, token)).Payload.Span);
            AssertEx.True(next.StreamGuid != stream.StreamGuid && nextOpen.ConsumerGuid != open.ConsumerGuid);
            await Rejected(receipt); // An old consumer cannot acknowledge the replacement stream.
            await host.PublishAISyncShadowAsync(next, Observation(next, 1), token);
            var nextData = AISyncShadowCodec.DecodeData((await raw.Read(MessageType.AISyncShadowData, token)).Payload.Span);
            await raw.Send(MessageType.AISyncShadowReceipt, AISyncShadowCodec.EncodeReceipt(new(next,
                nextOpen.ConsumerGuid, 1, 1, 0, new byte[32])), token);
            await raw.Read(MessageType.Error, token);
            await Wait(() => recoveries.Any(value => value.ConsumerGuid == nextOpen.ConsumerGuid), token);
            var healthyReceived = 0;
            healthy.AISyncShadowObservationBatchReceived += (_, _) => Interlocked.Increment(ref healthyReceived);
            AssertEx.Equal(AISyncShadowResult.Accepted, await host.PublishAISyncShadowAsync(next, Observation(next, 2), token));
            await Wait(() => Volatile.Read(ref healthyReceived) > 0, token);
            // A client cannot inject a valid host packet, even using its own consumer.
            await raw.Send(MessageType.AISyncShadowData, AISyncShadowCodec.EncodeData(nextData), token);
            await raw.ExpectEnd(token);
            AssertEx.Equal("Starting", healthy.Snapshot!.State);

            async Task Rejected(AISyncShadowReceipt value)
            {
                await raw.Send(MessageType.AISyncShadowReceipt, AISyncShadowCodec.EncodeReceipt(value), token);
                var frame = await raw.Read(MessageType.Error, token);
                AssertEx.Equal("AISYNC_SHADOW_RECEIPT_REJECTED", JsonPayload.Deserialize<ErrorPayload>(frame.Payload.Span).Code);
            }
        }
        await using (var host = Host())
        {
            await using var raw = await RawPeer.Connect(host, token);
            var forged = new AISyncShadowOpen(new(host.SessionId, 1, Guid.NewGuid()), Guid.NewGuid(), 1);
            await raw.Send(MessageType.AISyncShadowOpen, AISyncShadowCodec.EncodeOpen(forged), token);
            await raw.ExpectEnd(token);
        }
    }

    internal static async Task RestartAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = timeout.Token;
        await using var host = Host();
        await using var client = await Client(host, "Dallas", token);
        var receipts = new ConcurrentQueue<AISyncShadowReceipt>();
        host.AISyncShadowReceiptReceived += (_, receipt) => receipts.Enqueue(receipt);
        var round = await Start(host, new[] { client }, token);
        var old = host.StartAISyncShadow(round.LevelGeneration);
        await host.PublishAISyncShadowAsync(old, Observation(old, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == old), token);
        var sender = Sender(Peers(host).Single());
        var oldConsumer = OpenFor(host, client.PeerId)!.ConsumerGuid;
        var fresh = host.StartAISyncShadow(round.LevelGeneration);
        AssertEx.True(fresh.StreamGuid != old.StreamGuid);
        AssertEx.True(OpenFor(host, client.PeerId)!.ConsumerGuid != oldConsumer);
        AssertEx.True(ReferenceEquals(sender, Sender(Peers(host).Single())), "Rebind cannot spawn an additional sender.");
        AssertEx.Equal(AISyncShadowResult.Rejected, await host.PublishAISyncShadowAsync(old, Observation(old, 2), token));
        await host.PublishAISyncShadowAsync(fresh, Observation(fresh, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == fresh && value.ReceivedOrdinal == 1), token);
        await host.ReportHostPlayerDeathAsync(round.LevelGeneration, token);
        await client.SendPlayerDeathAsync(round.LevelGeneration, token);
        await Wait(() => client.Snapshot?.State == "Lobby", token);
        AssertEx.Equal(0, Peers(host).Length);
        AssertEx.True(sender.IsCompleted);
        var nextRound = await Start(host, new[] { client }, token);
        AssertEx.Throws<InvalidOperationException>(() => host.StartAISyncShadow(round.LevelGeneration));
        var next = host.StartAISyncShadow(nextRound.LevelGeneration);
        await AssertEx.ThrowsAsync<ProtocolException>(() => host.PublishAISyncShadowAsync(next, Observation(old, 3), token));
        await host.PublishAISyncShadowAsync(next, Observation(next, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == next), token);
        var lastSender = Sender(Peers(host).Single());
        await client.DisposeAsync();
        await Wait(() => Peers(host).Length == 0 && lastSender.IsCompleted, token);
        await host.StopAISyncShadowAsync();
    }

    internal static async Task TimeoutAndRetentionAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = timeout.Token;
        await using var host = Host();
        await using var client = await Client(host, "Dallas", token);
        var hostRecoveries = new ConcurrentQueue<AISyncShadowRecovery>();
        var clientRecoveries = new ConcurrentQueue<AISyncShadowRecovery>();
        var received = 0;
        var gameplayReady = 0;
        host.AISyncShadowRecoveryRequired += (_, value) => hostRecoveries.Enqueue(value);
        client.AISyncShadowRecoveryRequired += (_, value) => clientRecoveries.Enqueue(value);
        client.AISyncShadowObservationBatchReceived += (_, _) => Interlocked.Increment(ref received);
        client.GameplayReadyReceived += (_, _) => Interlocked.Increment(ref gameplayReady);
        var round = await Start(host, new[] { client }, token);
        var hostGate = WriteGate(host, client.PeerId);
        await hostGate.WaitAsync(token);
        AISyncShadowStreamId blocked;
        try
        {
            blocked = host.StartAISyncShadow(round.LevelGeneration);
            await host.PublishAISyncShadowAsync(blocked, Observation(blocked, 1), token);
            await Wait(() => hostRecoveries.Any(value => value.Stream == blocked), token);
        }
        finally { hostGate.Release(); }
        await host.SetHostGameplayReadyAsync(round.LevelGeneration, token);
        await Wait(() => Volatile.Read(ref gameplayReady) == 1, token);
        AssertEx.Equal("Starting", client.Snapshot!.State, "A write-lock timeout must leave the TCP connection usable.");

        var fresh = host.StartAISyncShadow(round.LevelGeneration);
        await Wait(() => ClientOpen(client)?.Stream == fresh, token);
        var controlSender = (Task)Field(client, "_shadowControlSender");
        var clientGate = (SemaphoreSlim)Field(Field(client, "_connection"), "_writeLock");
        await clientGate.WaitAsync(token);
        try
        {
            await host.PublishAISyncShadowAsync(fresh, Observation(fresh, 1), token);
            await Wait(() => Volatile.Read(ref received) == 1, token);
            await host.SetHostGameplayReadyAsync(round.LevelGeneration, token);
            using var readBudget = CancellationTokenSource.CreateLinkedTokenSource(token);
            readBudget.CancelAfter(TimeSpan.FromMilliseconds(800));
            await Wait(() => Volatile.Read(ref gameplayReady) == 2, readBudget.Token);
            await Wait(() => clientRecoveries.Any(value => value.Stream == fresh), token);
        }
        finally { clientGate.Release(); }
        await Wait(() => hostRecoveries.Any(value => value.Stream == fresh), token);

        // Use a deliberately small journal cap to force the production retention failure path.
        var bounded = host.StartAISyncShadow(round.LevelGeneration);
        var boundedOpen = OpenFor(host, client.PeerId)!;
        var journal = new AISyncShadowJournal(bounded, maxRecords: 2,
            maxBytes: 2 * AISyncShadowJournal.RetainedRecordBytes);
        journal.CreateOpen(boundedOpen.ConsumerGuid);
        lock (Field(host, "_stateLock"))
            host.GetType().GetField("_shadowJournal", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(host, journal);
        AssertEx.Equal(AISyncShadowResult.Accepted, await host.PublishAISyncShadowAsync(bounded, Observation(bounded, 1), token));
        AssertEx.Equal(AISyncShadowResult.Accepted, await host.PublishAISyncShadowAsync(bounded, Observation(bounded, 2), token));
        AssertEx.Equal(AISyncShadowResult.RecoveryRequired, await host.PublishAISyncShadowAsync(bounded, Observation(bounded, 3), token));
        AssertEx.Equal(2, journal.Count);
        AssertEx.Equal(2L * AISyncShadowJournal.RetainedRecordBytes, journal.Bytes);
        await Wait(() => clientRecoveries.Any(value => value.Stream == bounded), token);
        AssertEx.Equal(AISyncShadowResult.RecoveryRequired, await host.PublishAISyncShadowAsync(bounded, Observation(bounded, 4), token));
        var expired = host.StartAISyncShadow(round.LevelGeneration);
        await host.PublishAISyncShadowAsync(expired, Observation(expired, 1), token);
        lock (Field(host, "_stateLock"))
            host.GetType().GetField("_shadowRetentionStart", BindingFlags.Instance | BindingFlags.NonPublic)!
                .SetValue(host, Environment.TickCount64 - 60_001);
        AssertEx.Equal(AISyncShadowResult.RecoveryRequired, await host.PublishAISyncShadowAsync(expired, Observation(expired, 2), token));
        await Wait(() => clientRecoveries.Any(value => value.Stream == expired), token);
        await host.ReportHostPlayerDeathAsync(round.LevelGeneration, token);
        await client.SendPlayerDeathAsync(round.LevelGeneration, token);
        await Wait(() => client.Snapshot?.State == "Lobby", token);
        AssertEx.True(controlSender.IsCompleted, "The client control sender must drain at round end.");
    }

    internal static async Task ClientValidationAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = timeout.Token;
        await using var host = Host();
        await using var client = await Client(host, "Dallas", token);
        var batches = new ConcurrentQueue<AISyncShadowData>();
        var recoveries = new ConcurrentQueue<AISyncShadowRecovery>();
        var receipts = new ConcurrentQueue<AISyncShadowReceipt>();
        client.AISyncShadowObservationBatchReceived += (_, value) => batches.Enqueue(value);
        client.AISyncShadowRecoveryRequired += (_, value) => recoveries.Enqueue(value);
        host.AISyncShadowReceiptReceived += (_, value) => receipts.Enqueue(value);
        var round = await Start(host, new[] { client }, token);
        var old = host.StartAISyncShadow(round.LevelGeneration);
        await host.PublishAISyncShadowAsync(old, Observation(old, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == old), token);
        var oldOpen = ClientOpen(client)!;
        var oldData = batches.Single();
        var fresh = host.StartAISyncShadow(round.LevelGeneration);
        await Wait(() => ClientOpen(client)?.Stream == fresh, token);
        var staleCorrupt = AISyncShadowCodec.EncodeData(oldData);
        staleCorrupt[^1] ^= 0xFF;
        await HostSend(host, client.PeerId, MessageType.AISyncShadowOpen, AISyncShadowCodec.EncodeOpen(oldOpen), token);
        await HostSend(host, client.PeerId, MessageType.AISyncShadowData, staleCorrupt, token);
        await host.PublishAISyncShadowAsync(fresh, Observation(fresh, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == fresh), token);
        AssertEx.Equal(fresh, ClientOpen(client)!.Stream, "Old opens and malformed old bodies cannot roll back the new receiver.");
        AssertEx.Equal(1, batches.Count(value => value.Stream == fresh));
        var freshData = batches.Single(value => value.Stream == fresh);
        await HostSend(host, client.PeerId, MessageType.AISyncShadowData, AISyncShadowCodec.EncodeData(freshData), token);
        await Wait(() => receipts.Count(value => value.Stream == fresh) == 2, token);
        AssertEx.Equal(1, batches.Count(value => value.Stream == fresh), "A duplicate must not repeat the observation event.");
        var badDigest = AISyncShadowCodec.EncodeData(freshData);
        badDigest[120] ^= 0xFF;
        await HostSend(host, client.PeerId, MessageType.AISyncShadowData, badDigest, token);
        await Wait(() => recoveries.Any(value => value.Stream == fresh
            && value.Reason == AISyncShadowRecoveryReason.DigestMismatch), token);
        AssertEx.Equal("Starting", client.Snapshot!.State);
        var restarted = host.StartAISyncShadow(round.LevelGeneration);
        await host.PublishAISyncShadowAsync(restarted, Observation(restarted, 1), token);
        await Wait(() => receipts.Any(value => value.Stream == restarted), token);
        AssertEx.True(receipts.All(value => value.AppliedOrdinal == 0));
    }

    private static LobbyHost Host()
    {
        var host = new LobbyHost(new("Shadow", "Ripley", Guid.NewGuid(), 0, 4, "TestLevel", Compatibility));
        host.Start();
        return host;
    }

    private static async Task<LobbyClient> Client(LobbyHost host, string name, CancellationToken token)
    {
        var client = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), name, Compatibility), token);
        client.Start();
        return client;
    }

    private static async Task<LobbyStartPayload> Start(LobbyHost host, LobbyClient[] clients, CancellationToken token)
    {
        foreach (var client in clients) await client.SetReadyAsync(true, token);
        await Wait(() => host.CanStart, token);
        var round = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await Wait(() => clients.All(client => client.Snapshot?.State == "Starting"
            && client.Snapshot.LevelGeneration == round.LevelGeneration), token);
        return round;
    }

    private static async Task Wait(Func<bool> predicate, CancellationToken token)
    {
        while (!predicate()) await Task.Delay(2, token);
    }

    private static byte[] Observation(AISyncShadowStreamId stream, ulong ordinal)
    {
        var bytes = new byte[AISyncShadowCodec.NativeObservationBytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, AISyncShadowCodec.NativeMagic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), 216);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), ordinal);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), 7);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(40), ordinal);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(48), 3);
        var local = new byte[16];
        BinaryPrimitives.WriteUInt64LittleEndian(local, AISyncCodec.SessionCookie(stream.SessionGuid));
        "AIMPLOCL"u8.CopyTo(local.AsSpan(8));
        var record = new AISyncTransaction
        {
            Session = new Guid(local), AuthorityEpoch = 1, WorldEpoch = stream.WorldGeneration,
            ConsumerEpoch = 1, OwnerEpoch = 1, EntityA = 1, Birth = 1, CharacterClass = 1,
            CommandId = ordinal, GlobalSequence = ordinal, OwnerSequence = ordinal,
            Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
            ParameterRevision = 1, Flags = AISyncTransactionFlags.ObservationOnly | AISyncTransactionFlags.Incomplete
        };
        AISyncTransactionCodec.Encode(record).CopyTo(bytes, 56);
        return bytes;
    }

    private static object Field(object owner, string name) => owner.GetType()
        .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(owner)!;
    private static object Property(object owner, string name) => owner.GetType().GetProperty(name)!.GetValue(owner)!;
    private static object[] Peers(LobbyHost host)
    {
        lock (Field(host, "_stateLock"))
            return ((IDictionary)Field(host, "_shadowPeers")).Values.Cast<object>().ToArray();
    }
    private static Task Sender(object peer) => (Task)Property(peer, "Sender");
    private static AISyncShadowOpen? OpenFor(LobbyHost host, ulong peerId)
    {
        lock (Field(host, "_stateLock"))
            return Peers(host).Where(peer => (ulong)Property(Property(peer, "State"), "PeerId") == peerId)
                .Select(peer => (AISyncShadowOpen)Property(peer, "Open")).SingleOrDefault();
    }
    private static void AssertBounds(LobbyHost host, int connected)
    {
        lock (Field(host, "_stateLock"))
        {
            var peers = Peers(host);
            AssertEx.True(peers.Length <= connected);
            foreach (var peer in peers)
            {
                AssertEx.True((int)Property(peer, "Records") is >= 0 and <= 2048);
                AssertEx.True((int)Property(peer, "Bytes") is >= 0 and <= 2 * 1024 * 1024);
            }
        }
    }
    private static SemaphoreSlim WriteGate(LobbyHost host, ulong peerId)
    {
        return (SemaphoreSlim)Field(Connection(host, peerId), "_writeLock");
    }
    private static object Connection(LobbyHost host, ulong peerId)
    {
        var clients = ((IEnumerable)Field(host, "_clients")).Cast<object>();
        var state = clients.Where(entry => (ulong)Property(entry, "Key") == peerId)
            .Select(entry => Property(entry, "Value")).Single();
        return Property(state, "Connection");
    }
    private static AISyncShadowOpen? ClientOpen(LobbyClient client) => (AISyncShadowOpen?)client.GetType()
        .GetField("_shadowOpen", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(client);
    private static async Task HostSend(LobbyHost host, ulong peerId, MessageType type, byte[] bytes, CancellationToken token)
    {
        var connection = Connection(host, peerId);
        var gate = (SemaphoreSlim)Field(connection, "_writeLock");
        await gate.WaitAsync(token);
        try
        {
            await FrameCodec.WriteAsync((NetworkStream)Property(connection, "Stream"), new(type, 0, 1, bytes), token);
        }
        finally { gate.Release(); }
    }

    private sealed class RawPeer(TcpClient client) : IAsyncDisposable
    {
        private readonly NetworkStream _stream = client.GetStream();
        private ulong _sequence;
        public static async Task<RawPeer> Connect(LobbyHost host, CancellationToken token)
        {
            var socket = new TcpClient();
            await socket.ConnectAsync("127.0.0.1", host.BoundPort, token);
            var raw = new RawPeer(socket);
            await raw.Send(MessageType.LobbyJoin, JsonPayload.Serialize(new LobbyJoinPayload(
                Guid.NewGuid(), "Parker", Compatibility, 123)), token);
            await raw.Read(MessageType.LobbyJoinAccepted, token);
            await raw.Read(MessageType.LobbySnapshot, token);
            return raw;
        }
        public Task Send(MessageType type, byte[] bytes, CancellationToken token) => FrameCodec.WriteAsync(_stream,
            new AimpFrame(type, 0, ++_sequence, bytes), token).AsTask();
        public async Task<AimpFrame> Read(MessageType type, CancellationToken token)
        {
            while (true)
            {
                var frame = await FrameCodec.ReadAsync(_stream, token);
                if (frame.Type == type) return frame;
            }
        }
        public async Task ExpectEnd(CancellationToken token)
        {
            try
            {
                while (true) await FrameCodec.ReadAsync(_stream, token);
            }
            catch (EndOfStreamException) { }
            catch (IOException) { }
        }
        public ValueTask DisposeAsync() { client.Dispose(); return ValueTask.CompletedTask; }
    }
}
