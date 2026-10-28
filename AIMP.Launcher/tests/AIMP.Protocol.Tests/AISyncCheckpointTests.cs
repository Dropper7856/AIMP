using System.Collections.Concurrent;
using AIMP.Launcher;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncCheckpointTests
{
    internal static async Task HostBridgeAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var first = await Client(host, "Dallas", token);
        await using var second = await Client(host, "Lambert", token);
        var round = await Start(host, new[] { first, second }, token);
        var held = new ConcurrentQueue<AISyncExecutionControl>();
        var native = new ConcurrentQueue<AISyncExecutionControl>();
        var errors = new ConcurrentQueue<string>();
        var scopes = new ConcurrentDictionary<ulong,AISyncExecutionScope>();
        foreach (var client in new[] { first, second })
            WireConsumer(client, ReferenceEquals(client, second), held, scopes);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        AISyncHostBridge? bridge = null;
        using var owner = bridge = new AISyncHostBridge(() => binding, (_, connection, pid, bytes, _) =>
        {
            var control = AISyncExecutionControlCodec.Decode(bytes.Span); native.Enqueue(control);
            if (control.Kind is AISyncExecutionControlKind.Offer or AISyncExecutionControlKind.Checkpoint)
                bridge!.ObserveNative(new(connection, pid, MessageType.AISyncHostControl, AISyncExecutionControlCodec.Encode(control with {
                    Kind = control.Kind == AISyncExecutionControlKind.Offer ? AISyncExecutionControlKind.Ready : AISyncExecutionControlKind.CheckpointAck })));
            return Task.CompletedTask;
        }, message => errors.Enqueue(message), enabled: true);
        bridge.BindHost(host); await Wait(() => native.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
        var scope = native.First().Scope;
        // Bound the simulated producer burst independently of the checkpoint
        // threshold: two consumers also feed receipts into the bounded ingress.
        for (ulong end = 16; end <= 128; end += 16)
        {
            PublishSource(bridge, binding, scope, end - 15, end);
            await Wait(() => new[] { first, second }.All(c =>
                scopes.TryGetValue(c.PeerId,out var consumerScope)
                && host.AISyncExecutionSettledThrough(c.PeerId,consumerScope) >= end) || !errors.IsEmpty, token);
            AssertEx.True(errors.IsEmpty,string.Join(" | ",errors));
        }
        await Wait(() => !held.IsEmpty || !errors.IsEmpty, token);
        AssertEx.True(errors.IsEmpty,string.Join(" | ",errors));
        AssertEx.False(native.Any(c => c.Kind == AISyncExecutionControlKind.Checkpoint), "Host freed history before every consumer ACK.");
        AssertEx.True(held.TryDequeue(out var request));
        AssertEx.True(second.AcknowledgeAISyncExecutionCheckpoint(request! with { Kind = AISyncExecutionControlKind.CheckpointAck }));
        await Wait(() => native.Any(c => c.Kind == AISyncExecutionControlKind.Checkpoint) || !errors.IsEmpty, token);
        AssertEx.True(native.Any(c => c.Kind == AISyncExecutionControlKind.Checkpoint),string.Join(" | ",errors));
        AssertEx.False(native.Any(c => c.Kind == AISyncExecutionControlKind.Close));
        bridge.Dispose(); await bridge.Completion.WaitAsync(token);
    }
    private static void WireConsumer(LobbyClient client, bool hold, ConcurrentQueue<AISyncExecutionControl> held,
        ConcurrentDictionary<ulong,AISyncExecutionScope> scopes)
    {
        client.AISyncExecutionControlReceived += (_, c) => {
            if (c.Kind == AISyncExecutionControlKind.Offer) { scopes[client.PeerId]=c.Scope; client.AcceptAISyncExecution(c.Scope); }
            if (c.Kind != AISyncExecutionControlKind.Checkpoint) return;
            if (hold) held.Enqueue(c);
            else client.AcknowledgeAISyncExecutionCheckpoint(c with { Kind = AISyncExecutionControlKind.CheckpointAck });
        };
        client.AISyncExecutionBatchReceived += (_, b) => {
            foreach (var r in b.Records) client.SendAISyncExecutionReceipt(new(b.Scope, b.BatchId, r,
                r.Opcode == AISyncTransactionOpcode.Begin ? AISyncExecutionStage.Received : AISyncExecutionStage.Admitted,
                AISyncExecutionResult.Ok));
        };
    }
    private static void PublishSource(AISyncHostBridge bridge, AISyncHostBinding binding,
        AISyncExecutionScope scope, ulong first, ulong last)
    {
        for (var seq = first; seq <= last; seq++)
        {
            var op = seq == 1 ? AISyncTransactionOpcode.Begin : seq == 2 ? AISyncTransactionOpcode.Submit : AISyncTransactionOpcode.Update;
            var record = Record(scope, seq, op) with { ParameterRevision = seq < 3 ? 1 : (uint)seq - 1 };
            bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostBatch,
                AISyncExecutionCodec.EncodeBatch(new(scope, seq, new[] { record }))));
        }
    }
    internal static Task JournalAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var journal = new AISyncExecutionJournal(scope, maxRecords: 16);
        ulong seq = 0, batchId = 0;
        for (ulong command = 1; command <= 10000; command++)
        {
            var records = new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit,
                AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release }
                .Select(op => Record(scope, ++seq, op) with { CommandId = command }).ToArray();
            var bytes = AISyncExecutionCodec.EncodeBatch(new(scope, ++batchId, records));
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Append(bytes));
            AssertEx.False(journal.Checkpoint(seq));
            foreach (var r in records)
            {
                var stage = r.Opcode == AISyncTransactionOpcode.Release ? AISyncExecutionStage.Released
                    : r.Opcode == AISyncTransactionOpcode.Submit ? AISyncExecutionStage.Admitted : AISyncExecutionStage.Received;
                AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Receive(new(scope, batchId, r, stage, AISyncExecutionResult.Ok)));
            }
            AssertEx.Equal(seq, journal.SettledThrough());
            AssertEx.False(journal.Checkpoint(seq - 1), "A checkpoint cut a retained batch.");
            AssertEx.True(journal.Checkpoint(seq)); AssertEx.True(journal.Checkpoint(seq));
            AssertEx.Equal(0, journal.RecordCount); AssertEx.Equal(0, journal.RetainedBytes);
            AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(bytes));
            AssertEx.False(journal.RecoveryRequired);
        }
        return Task.CompletedTask;
    }

    internal static async Task TransportAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var started = System.Diagnostics.Stopwatch.StartNew();
        var logs = new ConcurrentQueue<string>();host.Log += (_,message) => logs.Enqueue("Host: " + message);
        client.Log += (_,message) => logs.Enqueue("Client: " + message);
        var round = await Start(host, new[] { client }, token);
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>();
        var changed = new SemaphoreSlim(0);
        client.AISyncExecutionControlReceived += (_, c) => { controls.Enqueue(c); changed.Release(); };
        client.AISyncExecutionBatchReceived += (_, b) => { batches.Enqueue(b); changed.Release(); };
        host.AISyncExecutionReceiptReceived += (_, _) => changed.Release();
        host.AISyncExecutionControlReceived += (_, _) => changed.Release();
        var scope = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
        await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
        AssertEx.True(client.AcceptAISyncExecution(scope)); await Wait(() => host.AISyncExecutionReady(client.PeerId, scope), token);
        for (ulong id = 1; id <= 600; id++)
        {
            try { await CycleAsync(host, client, scope, id, controls, batches, changed, token); }
            catch {
                Console.WriteLine($"Checkpoint cycle={id} elapsedMs={started.ElapsedMilliseconds} settled={host.AISyncExecutionSettledThrough(client.PeerId,scope)} batches={batches.Count} controls={controls.Count}; {string.Join(" | ",logs.TakeLast(8))}");
                throw;
            }
        }
        AssertEx.Equal(2400UL, host.AISyncExecutionSettledThrough(client.PeerId, scope));
        Console.WriteLine($"Checkpoint loopback: 600 cycles /2400 operations in {started.ElapsedMilliseconds} ms.");
    }

    private static async Task CycleAsync(LobbyHost host, LobbyClient client, AISyncExecutionScope scope, ulong id,
        ConcurrentQueue<AISyncExecutionControl> controls, ConcurrentQueue<AISyncExecutionBatch> batches,
        SemaphoreSlim changed, CancellationToken token)
    {
        var start = (id - 1) * 4;
        var records = new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit,
            AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release }
            .Select((op, i) => Record(scope, start + (ulong)i + 1, op) with { CommandId = id }).ToArray();
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,
            host.PublishAISyncExecution(client.PeerId, AISyncExecutionCodec.EncodeBatch(new(scope, id, records))));
        await WaitForChangeAsync(() => batches.Any(b => b.BatchId == id), changed, token);
        await AcknowledgeCycleAsync(host, client, scope, id, records, changed, token);
        var end = start + 4;
        AssertEx.True(host.CheckpointAISyncExecution(client.PeerId, scope, end));
        await WaitForChangeAsync(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Checkpoint && c.Through == end), changed, token);
        // Simulates the DLL response; native ledger behavior is tested by CTest.
        AssertEx.False(client.AcknowledgeAISyncExecutionCheckpoint(new(scope, AISyncExecutionControlKind.CheckpointAck, 0, end + 1)));
        AssertEx.True(client.AcknowledgeAISyncExecutionCheckpoint(new(scope, AISyncExecutionControlKind.CheckpointAck, 0, end)));
        await WaitForChangeAsync(() => host.AISyncExecutionSettledThrough(client.PeerId, scope) == end
            && !host.TryGetAISyncExecutionReceipt(client.PeerId, scope, end, out _), changed, token);
    }
    private static async Task AcknowledgeCycleAsync(LobbyHost host, LobbyClient client, AISyncExecutionScope scope,
        ulong id, AISyncTransaction[] records, SemaphoreSlim changed, CancellationToken token)
    {
        foreach (var record in records)
        {
            var stage = record.Opcode == AISyncTransactionOpcode.Release ? AISyncExecutionStage.Released
                : record.Opcode == AISyncTransactionOpcode.Submit ? AISyncExecutionStage.Admitted : AISyncExecutionStage.Received;
            AssertEx.True(client.SendAISyncExecutionReceipt(new(scope, id, record, stage, AISyncExecutionResult.Ok)));
        }
        await WaitForChangeAsync(() => host.AISyncExecutionSettledThrough(client.PeerId, scope) == records[^1].GlobalSequence, changed, token);
    }
    private static async Task WaitForChangeAsync(Func<bool> condition, SemaphoreSlim changed, CancellationToken token)
    {
        while (!condition()) await changed.WaitAsync(token);
    }
}
