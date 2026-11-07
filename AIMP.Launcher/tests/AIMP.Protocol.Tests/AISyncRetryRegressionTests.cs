using AIMP.Launcher;
using System.Collections.Concurrent;
using System.Diagnostics;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncRetryRegressionTests
{
    internal static Task CausalBypassAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var action = Record(scope, 2, AISyncTransactionOpcode.Submit) with { NativeKind = 5 };
        foreach (var kind in new uint[] { 0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x200 })
        {
            var constructor = action with { GlobalSequence = 4, CommandId = 2, NativeKind = kind };
            var planner = new AISyncRetryPlanner();
            var waiting = new[] { (1UL, action), (1UL, constructor) };
            for (var cycle = 0; cycle < 8; ++cycle)
                AssertEx.Equal(cycle % 2 == 0 ? 2UL : 4UL, planner.ChooseCycle(waiting, 8).Single().Sequence);
            // Reset must remove a previous scope's scheduling state, not its records.
            planner.Reset();
            AssertEx.Equal(2UL, planner.ChooseCycle(waiting, 8).Single().Sequence);
        }
        foreach (var kind in new uint[] { 2, 8, 6, 11, 0x106, 0x1FF, 0x201 })
            AssertNoBypass(action, action with { GlobalSequence = 4, CommandId = 2, NativeKind = kind });
        foreach (var opcode in new[] { AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop, AISyncTransactionOpcode.Release })
            AssertNoBypass(action with { Opcode = opcode }, action with { GlobalSequence = 4, CommandId = 2, NativeKind = 0x200 });
        AssertNoBypass(action, action with { GlobalSequence = 4, NativeKind = 0x200 });
        AssertNoBypass(action with { NativeKind = 2 }, action with { GlobalSequence = 4, CommandId = 2, NativeKind = 0x200 });
        return Task.CompletedTask;
    }

    private static void AssertNoBypass(AISyncTransaction first, AISyncTransaction next)
    {
        var planner = new AISyncRetryPlanner();
        for (var cycle = 0; cycle < 6; ++cycle)
            AssertEx.Equal(first.GlobalSequence, planner.ChooseCycle(new[] { (1UL, first), (1UL, next) }, 8).Single().Sequence,
                "A retry crossed cleanup/protected work, an unsupported constructor, or its own command.");
    }

    internal static Task AllocationBudgetAsync()
    {
        foreach (var groups in new[] { 1, 128 })
        {
            var planner = new AISyncRetryPlanner();
            var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
            var waiting = Enumerable.Range(0, 512).Select(i => (0UL,
                Record(scope, (ulong)i + 1, AISyncTransactionOpcode.Update) with {
                    EntityA = (uint)(i % groups + 1), ParameterRevision = (uint)i + 1, NativeKind = 1
                })).ToArray();
            for (var warm = 0; warm < 100; ++warm) planner.ChooseCycle(waiting, 8);
            var allocated = GC.GetAllocatedBytesForCurrentThread();
            var stopwatch = Stopwatch.StartNew();
            for (var cycle = 0; cycle < 1000; ++cycle) planner.ChooseCycle(waiting, 8);
            stopwatch.Stop();
            allocated = (GC.GetAllocatedBytesForCurrentThread() - allocated) / 1000;
            Console.WriteLine($"Retry planner 512 waits/{groups} groups: {allocated} B/cycle; {stopwatch.Elapsed.TotalMilliseconds / 1000:F3} ms/cycle.");
            AssertEx.True(allocated < 4096, "Retry selection allocated payload-sized storage after warm-up.");
        }
        return Task.CompletedTask;
    }

    internal static Task FullIdentityAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var first = Record(scope, 2, AISyncTransactionOpcode.Submit);
        var actors = new[] { first, first with { EntityB = 2 }, first with { Birth = 2 },
            first with { CharacterClass = 2 }, first with { OwnerEpoch = 2 } };
        var planner = new AISyncRetryPlanner();
        AssertEx.Equal(actors.Length, planner.ChooseCycle(actors.Select(a => (0UL, a)), 8).Length);
        AssertEx.Equal(2, planner.ChooseCycle(new[] { (0UL, first), (1UL, first) }, 8).Length);
        return Task.CompletedTask;
    }

    internal static async Task HostLoopFairnessAsync()
    {
        foreach (var groups in new[] { 16, 32 }) await HostLoopFairness(groups);
    }

    private static async Task HostLoopFairness(int groups)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(12)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var seen = new ConcurrentDictionary<uint, byte>();
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var failures = new ConcurrentQueue<string>();
        var batches = 0;
        client.AISyncExecutionControlReceived += (_, control) => {
            if (control.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(control.Scope);
        };
        client.AISyncExecutionBatchReceived += (_, batch) => {
            foreach (var operation in batch.Records)
                AssertEx.True(client.SendAISyncExecutionReceipt(new(batch.Scope, batch.BatchId, operation,
                    operation.Opcode == AISyncTransactionOpcode.Begin ? AISyncExecutionStage.Received : AISyncExecutionStage.Waiting,
                    AISyncExecutionResult.Ok)));
            Interlocked.Increment(ref batches);
        };
        client.AISyncExecutionRetryReceived += (_, batch) => {
            foreach (var operation in batch.Records) seen[operation.EntityA] = 0;
        };
        AISyncHostBridge? bridge = null;
        using (bridge = new AISyncHostBridge(() => binding, (_, connection, pid, bytes, _) => {
            var control = AISyncExecutionControlCodec.Decode(bytes.Span); controls.Enqueue(control);
            if (control.Kind == AISyncExecutionControlKind.Offer)
                bridge!.ObserveNative(new(connection, pid, MessageType.AISyncHostControl,
                    AISyncExecutionControlCodec.Encode(control with { Kind = AISyncExecutionControlKind.Ready })));
            return Task.CompletedTask;
        }, message => { if (message.Contains("refused", StringComparison.OrdinalIgnoreCase)) failures.Enqueue(message); }))
        {
            try
            {
                bridge.BindHost(host);
                await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
                var source = controls.First(c => c.Kind == AISyncExecutionControlKind.Offer).Scope;
                var records = Enumerable.Range(0, groups * 2).Select(i => Record(source, (ulong)i + 1,
                    i % 2 == 0 ? AISyncTransactionOpcode.Begin : AISyncTransactionOpcode.Submit) with {
                        EntityA = (uint)(i / 2 + 1), CommandId = (ulong)(i / 2 + 1), OwnerSequence = (ulong)(i % 2 + 1)
                    }).ToArray();
                for (var offset = 0; offset < records.Length; offset += 16)
                    bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostBatch,
                        AISyncExecutionCodec.EncodeBatch(new(source, (ulong)(offset / 16 + 1), records.Skip(offset).Take(16).ToArray()))));
                await Wait(() => Volatile.Read(ref batches) == groups / 8, token);
                // Discard partial-roster startup selections. All Waiting records
                // stay outstanding during this stable-roster observation window.
                await Task.Delay(200, token); seen.Clear();
                await Task.Delay(600, token);
                AssertEx.Equal(groups, seen.Count, $"The real Host tick starved {groups - seen.Count}/{groups} actor groups.");
                AssertEx.True(failures.IsEmpty, string.Join(" | ", failures));
            }
            finally { bridge.Dispose(); await bridge.Completion.WaitAsync(TimeSpan.FromSeconds(3)); }
        }
    }

    internal static async Task LocalConstructorProgressAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(12)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncExecutionBinding(client, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var receipts = new ConcurrentQueue<AISyncExecutionReceipt>();
        var attempts = new ConcurrentQueue<ulong>();
        var constructorEntered = false;
        AISyncExecutionBridge? bridge = null;
        host.AISyncExecutionReceiptReceived += (_, value) => receipts.Enqueue(value.Receipt);
        Task Send(MessageType type, Guid connection, int pid, ReadOnlyMemory<byte> bytes, CancellationToken cancellation)
        {
            if (type == MessageType.AISyncExecutionBinding)
            {
                var control = AISyncExecutionControlCodec.Decode(bytes.Span);
                if (control.Kind == AISyncExecutionControlKind.Offer)
                    bridge!.ObserveNative(new(connection, pid, type,
                        AISyncExecutionControlCodec.Encode(control with { Kind = AISyncExecutionControlKind.Ready })));
            }
            else if (type is MessageType.AISyncExecutionBatch or MessageType.AISyncExecutionRetry)
            {
                var batch = AISyncExecutionCodec.DecodeBatch(bytes.Span);
                foreach (var operation in batch.Records)
                {
                    var retry = type == MessageType.AISyncExecutionRetry;
                    if (retry) attempts.Enqueue(operation.GlobalSequence);
                    if (retry && operation.NativeKind == 0x200) constructorEntered = true;
                    var stage = operation.Opcode == AISyncTransactionOpcode.Begin ? AISyncExecutionStage.Received :
                        retry && constructorEntered ? AISyncExecutionStage.Admitted : AISyncExecutionStage.Waiting;
                    bridge!.ObserveNative(new(connection, pid, MessageType.AISyncExecutionReceipt,
                        AISyncExecutionCodec.EncodeReceipt(new(batch.Scope, batch.BatchId, operation, stage, AISyncExecutionResult.Ok))));
                }
            }
            return Task.CompletedTask;
        }
        using (bridge = new AISyncExecutionBridge(() => binding, Send, _ => { }))
        {
            try
            {
                bridge.BindClient(client);
                var scope = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
                await Wait(() => host.AISyncExecutionReady(client.PeerId, scope), token);
                var words = new uint[36]; words[0] = 1;
                var action = Record(scope) with { NativeKind = 5, NativeAdmission = 1 };
                var constructor = Record(scope, 3) with { NativeKind = 0x200, NativeAdmission = 1,
                    CommandId = 2, NativeIntent = new AISyncNativeIntent(words) };
                AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncExecution(client.PeerId,
                    AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { action,
                        action with { GlobalSequence = 2, OwnerSequence = 2, Opcode = AISyncTransactionOpcode.Submit }, constructor,
                        constructor with { GlobalSequence = 4, OwnerSequence = 4, Opcode = AISyncTransactionOpcode.Submit } }))));
                // No Host retries are sent: only the actual local bridge can
                // release this modeled native dependency cycle.
                await Wait(() => receipts.Count(r => r.Stage == AISyncExecutionStage.Admitted) == 2, token);
                AssertEx.True(attempts.Contains(2UL) && attempts.Contains(4UL));
                AssertEx.Equal(4UL, host.AISyncExecutionSettledThrough(client.PeerId, scope));
            }
            finally { bridge.Dispose(); await bridge.Completion.WaitAsync(TimeSpan.FromSeconds(3)); }
        }
    }
}
