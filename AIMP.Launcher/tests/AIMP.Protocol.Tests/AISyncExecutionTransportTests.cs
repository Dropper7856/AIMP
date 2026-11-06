using System.Collections;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Reflection;

namespace AIMP.Protocol.Tests;

internal static class AISyncExecutionTransportTests
{
    private static readonly CompatibilityInfo Compatibility = new(ProtocolConstants.Version, "test", "test",
        new string('A', 64), "test", new string('B', 64));
    internal static Task ControlsAsync()
    {
        var scope = Scope();
        AssertEx.SequenceEqual(ControlFixture(), AISyncExecutionControlCodec.Encode(new(scope, AISyncExecutionControlKind.Offer, 3)));
        foreach (var kind in Enum.GetValues<AISyncExecutionControlKind>())
        {
            var checkpoint = kind is AISyncExecutionControlKind.Checkpoint or AISyncExecutionControlKind.CheckpointAck;
            var value = kind == AISyncExecutionControlKind.Close || checkpoint ? 0U : 3U;
            var control = new AISyncExecutionControl(scope, kind, value, checkpoint ? 128UL : 0);
            var bytes = AISyncExecutionControlCodec.Encode(control);
            AssertEx.Equal(control, AISyncExecutionControlCodec.Decode(bytes));
            for (var n = 0; n < bytes.Length; n++)
                AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Decode(bytes.AsSpan(0, n)));
            var extra = bytes.Concat(new byte[] { 0 }).ToArray();
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Decode(extra));
            bytes[56] = checkpoint ? (byte)0 : (byte)1;
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Decode(bytes));
        }
        foreach (var invalid in new[] { 0U, 8U, uint.MaxValue })
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Encode(new(scope, AISyncExecutionControlKind.Offer, invalid)));
        var empty = scope with { Session = Guid.Empty };
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Encode(new(empty, AISyncExecutionControlKind.Offer, 3)));
        return Task.CompletedTask;
    }
    internal static Task JournalAsync()
    {
        var scope = Scope(); var journal = new AISyncExecutionJournal(scope);
        var begin = Record(scope); var submit = Record(scope, 2, AISyncTransactionOpcode.Submit);
        var packet = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { begin, submit }));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Append(packet));
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, journal.Append(packet));
        Array.Clear(packet); AssertEx.True(journal.TryReadBatch(1, out var retained));
        AssertEx.True(retained.Any(b => b != 0)); Array.Clear(retained);
        AssertEx.True(journal.TryReadBatch(1, out retained));
        var wait = new AISyncExecutionReceipt(scope, 1, submit, AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Receive(wait));
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, journal.Receive(wait));
        AssertEx.True(journal.ContainsOperation(1, submit));
        AssertEx.False(journal.ContainsOperation(2, submit));
        var admitted = wait with { Stage = AISyncExecutionStage.Admitted };
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Receive(admitted));
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired, journal.Receive(wait));
        AssertEx.True(journal.TryGetReceipt(2, out var last)); AssertEx.Equal(admitted, last!);
        var bounded = new AISyncExecutionJournal(scope, maxRecords: 2);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, bounded.Append(retained));
        var next = AISyncExecutionCodec.EncodeBatch(new(scope, 2, new[] { Record(scope, 3, AISyncTransactionOpcode.Complete) }));
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired, bounded.Append(next));
        AssertEx.Equal(2, bounded.RecordCount);
        var mismatch = new AISyncExecutionJournal(scope); mismatch.Append(retained);
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired, mismatch.Receive(wait with { BatchId = 2 }));
        var gap = new AISyncExecutionJournal(scope);
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired,
            gap.Append(AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { submit }))));
        var conflict = new AISyncExecutionJournal(scope); conflict.Append(retained);
        var changed = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { begin with { Mode = 1 }, submit }));
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired, conflict.Append(changed));
        return Task.CompletedTask;
    }
    internal static async Task DeliveryAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>(); var retries = new ConcurrentQueue<AISyncExecutionBatch>();
        var receipts = new ConcurrentQueue<AISyncExecutionReceipt>();
        client.AISyncExecutionControlReceived += (_, value) => controls.Enqueue(value);
        client.AISyncExecutionBatchReceived += (_, value) => batches.Enqueue(value);
        client.AISyncExecutionRetryReceived += (_, value) => retries.Enqueue(value);
        host.AISyncExecutionReceiptReceived += (_, value) => receipts.Enqueue(value.Receipt);
        var round = await Start(host, new[] { client }, token);
        var scope = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
        await Wait(() => controls.Any(c => c.Scope == scope && c.Kind == AISyncExecutionControlKind.Offer), token);
        var packet = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { Record(scope), Record(scope, 2, AISyncTransactionOpcode.Submit) }));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, host.PublishAISyncExecution(client.PeerId, packet));
        AssertEx.False(client.AcceptAISyncExecution(scope with { ConsumerEpoch = scope.ConsumerEpoch + 1 }));
        AssertEx.True(client.AcceptAISyncExecution(scope)); // Synthetic binding acknowledgment, no native/game execution.
        await Wait(() => host.AISyncExecutionReady(client.PeerId, scope), token);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncExecution(client.PeerId, packet));
        Array.Clear(packet);
        await Wait(() => batches.Count == 1, token);
        AssertEx.Equal(0, receipts.Count, "Transport receive cannot invent a native disposition.");
        var delivered = batches.Single();
        AssertEx.True(client.SendAISyncExecutionReceipt(new(scope, 1, delivered.Records[0], AISyncExecutionStage.Received, AISyncExecutionResult.Ok)));
        AssertEx.True(client.SendAISyncExecutionReceipt(new(scope, 1, delivered.Records[1], AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok)));
        await Wait(() => receipts.Count == 2, token);
        AssertEx.True(host.RetryAISyncExecution(client.PeerId, 2));
        await Wait(() => retries.Count == 1, token);
        AssertEx.Equal(delivered.Records[1], retries.Single().Records.Single());
        AssertEx.True(client.SendAISyncExecutionReceipt(new(scope, 1, delivered.Records[1], AISyncExecutionStage.Admitted, AISyncExecutionResult.Ok)));
        await Wait(() => receipts.Any(r => r.Stage == AISyncExecutionStage.Admitted), token);
        AssertEx.False(host.RetryAISyncExecution(client.PeerId, 2));
        await host.CloseAISyncExecutionPeerAsync(client.PeerId);
        await Wait(() => controls.Any(c => c.Scope == scope && c.Kind == AISyncExecutionControlKind.Close), token);
        AssertEx.False(client.SendAISyncExecutionReceipt(receipts.First()));
        var fresh = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
        AssertEx.True(fresh.ConsumerEpoch > scope.ConsumerEpoch);
        await Wait(() => controls.Any(c => c.Scope == fresh && c.Kind == AISyncExecutionControlKind.Offer), token);
        AssertEx.False(client.AcceptAISyncExecution(scope)); AssertEx.True(client.AcceptAISyncExecution(fresh));
        await Wait(() => host.AISyncExecutionReady(client.PeerId, fresh), token);
        await host.StopAISyncExecutionAsync();
    }
    internal static async Task SenderQueueTelemetryAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var token = timeout.Token;
        var entered = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var resume = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var sent = new ConcurrentQueue<byte>();
        var failures = 0;
        await using (var sender = new AISyncExecutionSender(async (_, bytes, ct) =>
        {
            entered.TrySetResult(true);
            await resume.Task.WaitAsync(ct).ConfigureAwait(false);
            sent.Enqueue(bytes.ToArray()[0]);
        }, () => Interlocked.Increment(ref failures), token))
        {
            AssertEx.True(sender.Enqueue(MessageType.AISyncExecutionNetworkBatch, new byte[] { 1 }));
            await entered.Task.WaitAsync(token);
            AssertEx.True(sender.Enqueue(MessageType.AISyncExecutionNetworkBatch, new byte[] { 2 }));
            AssertEx.True(sender.Enqueue(MessageType.AISyncExecutionNetworkBatch, new byte[] { 3 }));
            await Task.Delay(40, token);
            var waiting = sender.Snapshot();
            AssertEx.Equal(2, waiting.PendingPackets);
            AssertEx.Equal(3, waiting.PendingBytes);
            AssertEx.True(waiting.OldestPendingMs >= 20);
            resume.TrySetResult(true);
            await Wait(() => sent.Count == 3, token);
            AssertEx.SequenceEqual(new byte[] { 1, 2, 3 }, sent.ToArray(),
                "Congestion telemetry must not change FIFO causal delivery order.");
            var after = sender.Snapshot();
            AssertEx.Equal(0, after.PendingPackets);
            AssertEx.True(after.MaxQueuedMs >= 20);
            AssertEx.True(after.MaxSendMs >= 20);
            AssertEx.Equal(0, failures);
        }
    }

    internal static async Task IsolationAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = timeout.Token;
        await using var host = Host(); await using var slow = await Client(host, "Dallas", token);
        await using var fast = await Client(host, "Lambert", token);
        var round = await Start(host, new[] { slow, fast }, token);
        var received = 0; var recoveries = new ConcurrentQueue<ulong>();
        slow.AISyncExecutionControlReceived += (_, value) => { if (value.Kind == AISyncExecutionControlKind.Offer) slow.AcceptAISyncExecution(value.Scope); };
        fast.AISyncExecutionControlReceived += (_, value) => { if (value.Kind == AISyncExecutionControlKind.Offer) fast.AcceptAISyncExecution(value.Scope); };
        fast.AISyncExecutionBatchReceived += (_, _) => Interlocked.Increment(ref received);
        host.AISyncExecutionControlReceived += (_, value) => { if (value.Control.Kind == AISyncExecutionControlKind.Recovery) recoveries.Enqueue(value.PeerId); };
        var slowScope = host.OfferAISyncExecution(slow.PeerId, round.LevelGeneration, 1);
        var fastScope = host.OfferAISyncExecution(fast.PeerId, round.LevelGeneration, 1);
        await Wait(() => host.AISyncExecutionReady(slow.PeerId, slowScope) && host.AISyncExecutionReady(fast.PeerId, fastScope), token);
        var gate = (SemaphoreSlim)Field(Connection(host, slow.PeerId), "_writeLock");
        await gate.WaitAsync(token);
        try
        {
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncExecution(slow.PeerId,
                AISyncExecutionCodec.EncodeBatch(new(slowScope, 1, new[] { Record(slowScope) }))));
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncExecution(fast.PeerId,
                AISyncExecutionCodec.EncodeBatch(new(fastScope, 1, new[] { Record(fastScope) }))));
            await Wait(() => Volatile.Read(ref received) == 1, token);
            await Wait(() => recoveries.Contains(slow.PeerId), token);
            AssertEx.Equal(1, Volatile.Read(ref received));
        }
        finally { try { gate.Release(); } catch (ObjectDisposedException) { } }
        await host.StopAISyncExecutionAsync();
    }
    internal static async Task DirectionAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var disconnected = false; client.Disconnected += (_, _) => disconnected = true;
        var connection = Field(client, "_connection");
        var stream = (NetworkStream)connection.GetType().GetProperty("Stream")!.GetValue(connection)!;
        var gate = (SemaphoreSlim)Field(connection, "_writeLock"); await gate.WaitAsync(token);
        try { await FrameCodec.WriteAsync(stream, new(MessageType.AISyncExecutionNetworkBatch, 0, 1, Array.Empty<byte>()), token); }
        finally { gate.Release(); }
        await Wait(() => disconnected, token);
    }
    internal static async Task PendingWriterShutdownAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var connection = Field(client, "_connection"); var gate = (SemaphoreSlim)Field(connection, "_writeLock");
        await gate.WaitAsync(token);
        try
        {
            var send = (ValueTask)connection.GetType().GetMethod("SendAsync")!.Invoke(connection,
                new object[] { MessageType.Heartbeat, ReadOnlyMemory<byte>.Empty, CancellationToken.None })!;
            var pending = send.AsTask(); AssertEx.False(pending.IsCompleted);
            await ((IAsyncDisposable)connection).DisposeAsync();
            try { await pending.WaitAsync(TimeSpan.FromSeconds(2)); throw new InvalidOperationException("Closed writer unexpectedly sent."); }
            catch (IOException) { }
        }
        finally { gate.Release(); }
    }
    private static AISyncExecutionScope Scope() => new(Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"), 1, 2, 3);
    internal static AISyncTransaction Record(AISyncExecutionScope scope, ulong seq = 1, AISyncTransactionOpcode op = AISyncTransactionOpcode.Begin) => new()
    {
        Session = scope.Session, AuthorityEpoch = scope.AuthorityEpoch, WorldEpoch = scope.WorldEpoch, ConsumerEpoch = scope.ConsumerEpoch,
        EntityA = 1, Birth = 1, CharacterClass = 1, OwnerEpoch = 1, CommandId = 1,
        GlobalSequence = seq, OwnerSequence = seq, Opcode = op, Capability = AISyncTransactionCapability.Movement,
        ParameterRevision = 1, NativeKind = 4
    };
    internal static LobbyHost Host() { var host = new LobbyHost(new("Execution", "Ripley", Guid.NewGuid(), 0, 4, "TestLevel", Compatibility)); host.Start(); return host; }
    internal static async Task<LobbyClient> Client(LobbyHost host, string name, CancellationToken token)
    { var client = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort, Guid.NewGuid(), name, Compatibility), token); client.Start(); return client; }
    internal static async Task<LobbyStartPayload> Start(LobbyHost host, LobbyClient[] clients, CancellationToken token)
    {
        foreach (var client in clients) await client.SetReadyAsync(true, token);
        await Wait(() => host.CanStart, token); var round = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await Wait(() => clients.All(c => c.Snapshot?.State == "Starting" && c.Snapshot.LevelGeneration == round.LevelGeneration), token); return round;
    }
    internal static async Task Wait(Func<bool> condition, CancellationToken token) { while (!condition()) await Task.Delay(2, token); }
    private static byte[] ControlFixture()
    {
        for (var path = new DirectoryInfo(AppContext.BaseDirectory); path is not null; path = path.Parent)
        {
            var fixture = Path.Combine(path.FullName, "tests", "fixtures", "aisync-execution-control-v1.hex");
            if (File.Exists(fixture)) return Convert.FromHexString(string.Concat(File.ReadAllText(fixture).Where(c => !char.IsWhiteSpace(c))));
        }
        throw new FileNotFoundException("Shared execution control fixture missing.");
    }
    private static object Field(object value, string name) => value.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(value)!;
    private static object Connection(LobbyHost host, ulong peer)
    {
        var entry = ((IEnumerable)Field(host, "_clients")).Cast<object>().Single(e => (ulong)e.GetType().GetProperty("Key")!.GetValue(e)! == peer);
        var client = entry.GetType().GetProperty("Value")!.GetValue(entry)!;
        return client.GetType().GetProperty("Connection")!.GetValue(client)!;
    }
}
