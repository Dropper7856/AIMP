using AIMP.Launcher;
using System.Collections.Concurrent;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncHostBridgeTests
{
    internal static Task CurrentIntentRetryPriorityAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var submit = Record(scope, 2, AISyncTransactionOpcode.Submit)
            with { NativeKind = 1, NativeAdmission = 1 };
        var updates = Enumerable.Range(0, 10).Select(index =>
            Record(scope, (ulong)(3 + index), AISyncTransactionOpcode.Update)
                with { NativeKind = 1, CommandId = submit.CommandId,
                    ParameterRevision = (uint)(index + 2) }).ToArray();
        AssertEx.Equal(0UL, AISyncIntentRetryPriority.Select(Array.Empty<AISyncTransaction>()));
        AssertEx.Equal(updates[^1].GlobalSequence, AISyncIntentRetryPriority.Select(updates),
            "Older parameter revisions must not delay the latest Host goal.");
        AssertEx.Equal(submit.GlobalSequence,
            AISyncIntentRetryPriority.Select(new[] { submit, updates[^1] }),
            "An Update cannot overtake its own unentered Submit.");

        var stale = Record(scope, 13, AISyncTransactionOpcode.Submit)
            with { NativeKind = 3, CommandId = submit.CommandId + 1, NativeAdmission = 1 };
        var replacement = Record(scope, 14, AISyncTransactionOpcode.Submit)
            with { NativeKind = 1, CommandId = stale.CommandId + 1, NativeAdmission = 0 };
        AssertEx.Equal(replacement.GlobalSequence,
            AISyncIntentRetryPriority.Select(new[] { submit, updates[0], stale, replacement }),
            "An admission-0 replacement must retry before obsolete movement intents.");
        AssertEx.Equal(submit.GlobalSequence,
            AISyncIntentRetryPriority.Select(new[] { submit, replacement with { NativeAdmission = 1 } }),
            "Append admission does not authorize preemption.");
        foreach (var kind in new uint[] { 2, 5, 8, 0x100 })
        {
            var protectedWork = submit with { NativeKind = kind };
            AssertEx.Equal(protectedWork.GlobalSequence,
                AISyncIntentRetryPriority.Select(new[] { protectedWork, replacement }),
                "Native marker, action, placement and constructor calls must keep their order.");
        }
        var cleanup = submit with { Opcode = AISyncTransactionOpcode.Release };
        AssertEx.Equal(cleanup.GlobalSequence,
            AISyncIntentRetryPriority.Select(new[] { cleanup, replacement }),
            "Cleanup must not be skipped by a newer navigation goal.");
        return Task.CompletedTask;
    }

    internal static Task UpdateRejectedIsolationAsync() => RefusalIsolationAsync(AISyncExecutionStage.Rejected);
    internal static Task UnknownIsolationAsync() => RefusalIsolationAsync(AISyncExecutionStage.Unknown);

    internal static Task NativeRecoveryPolicyAsync()
    {
        AssertEx.True(AISyncNativeRecoveryPolicy.Terminal(AISyncExecutionStage.Rejected));
        AssertEx.True(AISyncNativeRecoveryPolicy.Terminal(AISyncExecutionStage.Unknown));
        foreach (var stage in new[] { AISyncExecutionStage.Waiting, AISyncExecutionStage.Admitted,
            AISyncExecutionStage.Released, AISyncExecutionStage.Superseded })
            AssertEx.False(AISyncNativeRecoveryPolicy.Terminal(stage));
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var movement = Record(scope, 2, AISyncTransactionOpcode.Submit) with { NativeKind = 1 };
        AssertEx.False(AISyncNativeRecoveryPolicy.MovementTimedOut(movement, 11999));
        AssertEx.True(AISyncNativeRecoveryPolicy.MovementTimedOut(movement, 12000));
        AssertEx.True(AISyncNativeRecoveryPolicy.MovementTimedOut(
            movement with { NativeKind = 3, Opcode = AISyncTransactionOpcode.Update }, 20000));
        foreach (var kind in new uint[] { 2, 5, 8, 0x100 })
            AssertEx.False(AISyncNativeRecoveryPolicy.MovementTimedOut(
                movement with { NativeKind = kind }, 60000),
                "Non-interruptible native requests must not time out as movements.");
        AssertEx.False(AISyncNativeRecoveryPolicy.MovementTimedOut(
            movement with { Opcode = AISyncTransactionOpcode.Release }, 60000));
        for (var count = 0; count < AISyncNativeRecoveryPolicy.MaxAutomaticRepairs; count++)
            AssertEx.True(AISyncNativeRecoveryPolicy.MayRepair(count));
        AssertEx.False(AISyncNativeRecoveryPolicy.MayRepair(AISyncNativeRecoveryPolicy.MaxAutomaticRepairs));
        AssertEx.False(AISyncNativeRecoveryPolicy.MayRepair(-1));
        return Task.CompletedTask;
    }

    private static async Task RefusalIsolationAsync(AISyncExecutionStage stage)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>();
        var retries = new ConcurrentQueue<AISyncExecutionBatch>();
        var logs = new ConcurrentQueue<string>();
        client.AISyncExecutionControlReceived += (_, c) =>
        {
            if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope);
        };
        client.AISyncExecutionBatchReceived += (_, b) => batches.Enqueue(b);
        client.AISyncExecutionRetryReceived += (_, b) => retries.Enqueue(b);
        using var bridge = new AISyncHostBridge(() => binding, (_, _, _, bytes, _) =>
        {
            controls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span));
            return Task.CompletedTask;
        }, logs.Enqueue);
        try
        {
            bridge.BindHost(host);
            await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
            var offer = controls.First(c => c.Kind == AISyncExecutionControlKind.Offer);
            bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostControl,
                AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready })));
            bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostBatch,
                AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 1,
                    new[] { Record(offer.Scope), Record(offer.Scope, 2, AISyncTransactionOpcode.Submit),
                        Record(offer.Scope, 3, AISyncTransactionOpcode.Update) with { ParameterRevision = 2 } }))));
            await Wait(() => batches.Count == 1, token);
            var batch = batches.Single();
            AssertEx.True(client.SendAISyncExecutionReceipt(new(batch.Scope, batch.BatchId, batch.Records[0],
                AISyncExecutionStage.Received, AISyncExecutionResult.Ok)));
            AssertEx.True(client.SendAISyncExecutionReceipt(new(batch.Scope, batch.BatchId, batch.Records[1],
                AISyncExecutionStage.Admitted, AISyncExecutionResult.Ok)));
            AssertEx.True(client.SendAISyncExecutionReceipt(new(batch.Scope, batch.BatchId, batch.Records[2],
                AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok)));
            await Wait(() => !retries.IsEmpty, token);
            var refusal = new AISyncExecutionReceipt(batch.Scope, batch.BatchId, batch.Records[2], stage,
                stage == AISyncExecutionStage.Unknown ? AISyncExecutionResult.OutcomeUnknown : AISyncExecutionResult.Ok);
            AssertEx.True(client.SendAISyncExecutionReceipt(refusal));
            // An immutable final refusal must NEVER be replayed as the same
            // native operation. Instead the old source is retired.
            await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Close &&
                c.Scope == offer.Scope), token);
            AssertEx.True(logs.Any(l => l.Contains("Client native command refused:", StringComparison.Ordinal)));
            AssertEx.True(logs.Any(l => l.Contains("AISync native recovery:", StringComparison.Ordinal)));
            AssertEx.Equal(1, bridge.NativeFaultRecoveries);
            AssertEx.False(host.RetryAISyncExecution(client.PeerId, 3));
            AssertEx.Equal(1, batches.Count, "Recovery must not fabricate a replacement native operation.");
            await Wait(() => controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer) >= 2, token);
            var fresh = controls.Last(c => c.Kind == AISyncExecutionControlKind.Offer);
            AssertEx.True(fresh.Scope.AuthorityEpoch > offer.Scope.AuthorityEpoch,
                "Native repair resumed the old command identity instead of rebasing authority.");
            AssertEx.Equal(offer.Scope.WorldEpoch, fresh.Scope.WorldEpoch);
            AssertEx.False(controls.Any(c => c.Kind == AISyncExecutionControlKind.Close &&
                c.Scope == fresh.Scope));
        }
        finally { bridge.Dispose(); await bridge.Completion.WaitAsync(token); }
    }

    internal static async Task NativeRepairBudgetAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(35)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration,
            7, 123, Guid.NewGuid(), 1, true);
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>();
        var logs = new ConcurrentQueue<string>();
        client.AISyncExecutionControlReceived += (_, c) =>
        {
            if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope);
        };
        client.AISyncExecutionBatchReceived += (_, b) => batches.Enqueue(b);
        using var bridge = new AISyncHostBridge(() => Volatile.Read(ref binding), (_, _, _, bytes, _) =>
        {
            controls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span));
            return Task.CompletedTask;
        }, logs.Enqueue);
        try
        {
            bridge.BindHost(host);
            for (var i = 0; i <= AISyncNativeRecoveryPolicy.MaxAutomaticRepairs; i++)
            {
                var ordinal = i + 1;
                await Wait(() => controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer) >= ordinal, token);
                var offer = controls.Where(c => c.Kind == AISyncExecutionControlKind.Offer).ElementAt(i);
                bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostControl,
                    AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready })));
                bridge.ObserveNative(new(binding.ConnectionIdentity, binding.ProcessId, MessageType.AISyncHostBatch,
                    AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 1,
                        new[] { Record(offer.Scope),
                            Record(offer.Scope, 2, AISyncTransactionOpcode.Submit) }))));
                await Wait(() => batches.Any(b => b.Scope == offer.Scope), token);
                var batch = batches.Single(b => b.Scope == offer.Scope);
                var stage = i % 2 == 0 ? AISyncExecutionStage.Rejected : AISyncExecutionStage.Unknown;
                AssertEx.True(client.SendAISyncExecutionReceipt(new(batch.Scope, batch.BatchId,
                    batch.Records[1], stage, stage == AISyncExecutionStage.Unknown
                        ? AISyncExecutionResult.OutcomeUnknown : AISyncExecutionResult.Ok)));
                await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Close &&
                    c.Scope == offer.Scope), token);
            }
            AssertEx.Equal(AISyncNativeRecoveryPolicy.MaxAutomaticRepairs, bridge.NativeFaultRecoveries);
            AssertEx.Equal(1, bridge.NativeRepairLimitHits);
            AssertEx.True(logs.Any(l => l.Contains("AISync native recovery exhausted:", StringComparison.Ordinal)));
            await Task.Delay(TimeSpan.FromMilliseconds(2600), token);
            AssertEx.Equal(AISyncNativeRecoveryPolicy.MaxAutomaticRepairs + 1,
                controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer),
                "A repeatedly failing native binding entered an unbounded restart loop.");
            // A genuinely new native connection/world binding is a new repair
            // opportunity, not a replay of the old failed operation or scope.
            Volatile.Write(ref binding, binding with { ConnectionIdentity = Guid.NewGuid(), Epoch = 2 });
            await Wait(() => controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer) ==
                AISyncNativeRecoveryPolicy.MaxAutomaticRepairs + 2, token);
            var afterRebind = controls.Last(c => c.Kind == AISyncExecutionControlKind.Offer);
            AssertEx.True(afterRebind.Scope.AuthorityEpoch >
                controls.First(c => c.Kind == AISyncExecutionControlKind.Offer).Scope.AuthorityEpoch);
            AssertEx.Equal(1, bridge.NativeRepairLimitHits);
        }
        finally { bridge.Dispose(); await bridge.Completion.WaitAsync(token); }
    }

    internal static Task RefusalEchoValidationAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 1, 1);
        var operation = Record(scope, 2, AISyncTransactionOpcode.Update);
        var bytes = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { Record(scope), operation }));
        foreach (var stage in new[] { AISyncExecutionStage.Rejected, AISyncExecutionStage.Unknown })
        {
            var exact = new AISyncExecutionReceipt(scope, 1, operation, stage,
                stage == AISyncExecutionStage.Unknown ? AISyncExecutionResult.OutcomeUnknown : AISyncExecutionResult.Ok);
            foreach (var forged in new[] { exact with { BatchId = 2 },
                exact with { Operation = operation with { CommandId = 2 } },
                exact with { Operation = operation with { EntityA = 2 } },
                exact with { Operation = operation with { ParameterRevision = 2 } },
                exact with { Operation = operation with { GlobalSequence = 3, OwnerSequence = 3 } } })
            {
                var journal = new AISyncExecutionJournal(scope); journal.Append(bytes);
                AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired, journal.Receive(forged));
                AssertEx.False(journal.TryGetReceipt(operation.GlobalSequence, out _));
            }
            var valid = new AISyncExecutionJournal(scope); valid.Append(bytes);
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, valid.Receive(exact));
            AssertEx.Equal(0UL, valid.SettledThrough());
            AssertEx.False(valid.Checkpoint(2));
            AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired,
                valid.Receive(exact with { Stage = AISyncExecutionStage.Admitted, Result = AISyncExecutionResult.Ok }));
        }
        return Task.CompletedTask;
    }

    internal static async Task ForgedRefusalRecoveryAsync()
    {
        foreach (var stage in new[] { AISyncExecutionStage.Rejected, AISyncExecutionStage.Unknown })
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token = timeout.Token;
            await using var host = Host(); await using var client = await Client(host, "Dallas", token);
            var round = await Start(host, new[] { client }, token);
            var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
            var controls = new ConcurrentQueue<AISyncExecutionControl>();
            var clientControls = new ConcurrentQueue<AISyncExecutionControl>();
            var batches = new ConcurrentQueue<AISyncExecutionBatch>(); var receipts = new ConcurrentQueue<AISyncExecutionReceipt>();
            var logs = new ConcurrentQueue<string>();
            host.AISyncExecutionReceiptReceived += (_, r) => receipts.Enqueue(r.Receipt);
            client.AISyncExecutionControlReceived += (_, c) =>
            {
                clientControls.Enqueue(c);
                if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope);
            };
            client.AISyncExecutionBatchReceived += (_, b) => batches.Enqueue(b);
            using var bridge = new AISyncHostBridge(() => binding, (_, _, _, bytes, _) =>
            { controls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span)); return Task.CompletedTask; }, logs.Enqueue);
            try
            {
                bridge.BindHost(host);
                await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
                var offer = controls.Single(c => c.Kind == AISyncExecutionControlKind.Offer);
                bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl,
                    AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready })));
                bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
                    AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 1, new[] { Record(offer.Scope),
                        Record(offer.Scope, 2, AISyncTransactionOpcode.Submit) }))));
                await Wait(() => batches.Count == 1, token); var batch = batches.Single();
                var forged = new AISyncExecutionReceipt(batch.Scope, batch.BatchId,
                    batch.Records[1] with { CommandId = 999 }, stage,
                    stage == AISyncExecutionStage.Unknown ? AISyncExecutionResult.OutcomeUnknown : AISyncExecutionResult.Ok);
                // Bypass the Client journal to exercise hostile network input at
                // the actual Host journal, rather than only its local send guard.
                var connection = typeof(LobbyClient).GetField("_connection",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(client)!;
                await ((ValueTask)connection.GetType().GetMethod("SendAsync")!.Invoke(connection,
                    new object[] { MessageType.AISyncExecutionNetworkReceipt,
                        (ReadOnlyMemory<byte>)AISyncExecutionCodec.EncodeReceipt(forged), token })!).AsTask();
                await Wait(() => clientControls.Any(c => c.Kind == AISyncExecutionControlKind.Recovery), token);
                AssertEx.Equal(0, receipts.Count, "Fabricated identity escaped Host journal validation.");
                AssertEx.False(host.AISyncExecutionReady(client.PeerId, batch.Scope));
                bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
                    AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 2,
                        new[] { Record(offer.Scope, 3, AISyncTransactionOpcode.Cancel) }))));
                await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Close), token);
                AssertEx.False(logs.Any(l => l.StartsWith("Client native command refused:", StringComparison.Ordinal)),
                    "Corrupt receipts must not enter the command-local refusal path.");
            }
            finally { bridge.Dispose(); await bridge.Completion.WaitAsync(token); }
        }
    }

    internal static async Task IngressOverflowAsync()
    {
        using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(15));var token=timeout.Token;
        await using var host=Host();await using var client=await Client(host,"Dallas",token);
        var round=await Start(host,new[]{client},token);
        var controls=new ConcurrentQueue<AISyncExecutionControl>();var logs=new ConcurrentQueue<string>();
        var batches=new ConcurrentQueue<AISyncExecutionBatch>();
        var release=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        client.AISyncExecutionControlReceived+=(_,c)=>{if(c.Kind==AISyncExecutionControlKind.Offer)client.AcceptAISyncExecution(c.Scope);};
        client.AISyncExecutionBatchReceived+=(_,b)=>batches.Enqueue(b);
        var binding=new AISyncHostBinding(host,host.SessionId,round.LevelGeneration,7,123,Guid.NewGuid(),1,true);
        using var bridge=new AISyncHostBridge(()=>binding,async(_,_,_,bytes,cancellation)=>
        {
            var control=AISyncExecutionControlCodec.Decode(bytes.Span);controls.Enqueue(control);
            if(control.Kind==AISyncExecutionControlKind.Offer)await release.Task.WaitAsync(cancellation);
        },message=>logs.Enqueue(message),enabled:true);
        try
        {
            bridge.BindHost(host);await Wait(()=>controls.Any(c=>c.Kind==AISyncExecutionControlKind.Offer),token);
            var offer=controls.First(c=>c.Kind==AISyncExecutionControlKind.Offer);
            var bytes=AISyncExecutionCodec.EncodeBatch(new(offer.Scope,1,new[]{Record(offer.Scope)}));
            // The in-flight Offer holds the sole reader. No scheduling/timer
            // assumption is needed to exceed the 256-entry ingress bound.
            for(var i=0;i<300;i++)bridge.ObserveNative(new(binding.ConnectionIdentity,binding.ProcessId,MessageType.AISyncHostBatch,bytes));
            release.TrySetResult();
            await Wait(()=>controls.Any(c=>c.Kind==AISyncExecutionControlKind.Close),token);
            AssertEx.True(logs.Any(l=>l.Contains("bounded ingress lost coverage",StringComparison.Ordinal)));
            AssertEx.Equal(0,batches.Count,"Lost causal history must not enter a consumer.");
            AssertEx.Equal(1,controls.Count(c=>c.Kind==AISyncExecutionControlKind.Offer));
        }
        finally {release.TrySetResult();bridge.Dispose();await bridge.Completion.WaitAsync(token);}
    }
    internal static async Task FanoutAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host();
        await using var first = await Client(host, "Dallas", token);
        await using var second = await Client(host, "Lambert", token);
        var round = await Start(host, new[] { first, second }, token);
        var offers = new ConcurrentDictionary<ulong, AISyncExecutionScope>();
        var received = new ConcurrentDictionary<ulong, AISyncExecutionBatch>();
        var retries = new ConcurrentQueue<AISyncExecutionBatch>();
        foreach (var client in new[] { first, second })
        {
            client.AISyncExecutionControlReceived += (_, c) =>
            { if (c.Kind == AISyncExecutionControlKind.Offer) offers[client.PeerId] = c.Scope; };
            client.AISyncExecutionBatchReceived += (_, b) => received[client.PeerId] = b;
            client.AISyncExecutionRetryReceived += (_, b) => retries.Enqueue(b);
        }
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var sourceControls = new ConcurrentQueue<AISyncExecutionControl>();
        using var bridge = new AISyncHostBridge(() => binding, (type, connection, pid, bytes, _) =>
        {
            AssertEx.Equal(MessageType.AISyncHostBinding, type);
            AssertEx.Equal(binding.ConnectionIdentity, connection); AssertEx.Equal(binding.ProcessId, pid);
            sourceControls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span)); return Task.CompletedTask;
        }, _ => { }); // Production defaults include native execution and Host routes.
        bridge.BindHost(host);
        await Wait(() => offers.Count == 2, token);
        AssertEx.Equal(0, sourceControls.Count, "Host producer opened before consumers were ready.");
        AssertEx.True(first.AcceptAISyncExecution(offers[first.PeerId]));
        await Task.Delay(60, token); AssertEx.Equal(0, sourceControls.Count);
        AssertEx.True(second.AcceptAISyncExecution(offers[second.PeerId]));
        await Wait(() => sourceControls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
        var offer = sourceControls.Single(c => c.Kind == AISyncExecutionControlKind.Offer);
        AssertEx.Equal(1UL, offer.Scope.ConsumerEpoch);
        var ready = AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready });
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl, ready));
        var begin = AISyncMovementTests.Move() with { Session = offer.Scope.Session, AuthorityEpoch = offer.Scope.AuthorityEpoch,
            WorldEpoch = offer.Scope.WorldEpoch, ConsumerEpoch = offer.Scope.ConsumerEpoch };
        var submit = begin with { GlobalSequence = 2, OwnerSequence = 2, Opcode = AISyncTransactionOpcode.Submit };
        var source = AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 1, new[] { begin, submit }));
        bridge.ObserveNative(new(Guid.NewGuid(), 123, MessageType.AISyncHostBatch, source));
        bridge.ObserveNative(new(binding.ConnectionIdentity, 999, MessageType.AISyncHostBatch, source));
        await Task.Delay(50, token); AssertEx.Equal(0, received.Count);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch, source));
        await Wait(() => received.Count == 2, token);
        foreach (var peer in offers)
        {
            var actual = received[peer.Key];
            AssertEx.Equal(peer.Value, actual.Scope);
            AssertEx.Equal(begin with { ConsumerEpoch = peer.Value.ConsumerEpoch }, actual.Records[0]);
            AssertEx.Equal(submit with { ConsumerEpoch = peer.Value.ConsumerEpoch }, actual.Records[1]);
        }
        var firstBatch = received[first.PeerId];
        AssertEx.True(first.SendAISyncExecutionReceipt(new(firstBatch.Scope, 1, firstBatch.Records[1],
            AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok)));
        await Wait(() => !retries.IsEmpty, token);
        AssertEx.Equal(firstBatch.Records[1], retries.First().Records.Single());
        AssertEx.True(first.SendAISyncExecutionReceipt(new(firstBatch.Scope, 1, firstBatch.Records[1],
            AISyncExecutionStage.Admitted, AISyncExecutionResult.Ok)));
        await Task.Delay(50, token);
        bridge.Dispose(); await bridge.Completion.WaitAsync(token);
        AssertEx.True(sourceControls.Any(c => c.Kind == AISyncExecutionControlKind.Close));
    }

    internal static async Task RecoveryAndIncarnationAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var controls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>();
        client.AISyncExecutionControlReceived += (_, c) =>
        { if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope); };
        client.AISyncExecutionBatchReceived += (_, b) => batches.Enqueue(b);
        using var bridge = new AISyncHostBridge(() => Volatile.Read(ref binding), (_, connection, pid, bytes, _) =>
        {
            var current = Volatile.Read(ref binding);
            if (connection != current.ConnectionIdentity || pid != current.ProcessId) throw new IOException("retired IPC");
            controls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span)); return Task.CompletedTask;
        }, _ => { }, enabled: true);
        bridge.BindHost(host);
        await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
        var old = controls.Single(c => c.Kind == AISyncExecutionControlKind.Offer);
        var oldConnection = binding.ConnectionIdentity;
        Volatile.Write(ref binding, binding with { ConnectionIdentity = Guid.NewGuid(), Epoch = 2 });
        await Wait(() => controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer) == 2, token);
        var fresh = controls.Last(c => c.Kind == AISyncExecutionControlKind.Offer);
        AssertEx.True(fresh.Scope.AuthorityEpoch > old.Scope.AuthorityEpoch);
        bridge.ObserveNative(new(oldConnection, 123, MessageType.AISyncHostControl,
            AISyncExecutionControlCodec.Encode(old with { Kind = AISyncExecutionControlKind.Ready })));
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl,
            AISyncExecutionControlCodec.Encode(fresh with { Kind = AISyncExecutionControlKind.Ready })));
        var delayedControl=AISyncExecutionControlCodec.Encode(old with {Kind=AISyncExecutionControlKind.Ready});
        delayedControl[6]=0xFF;
        bridge.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostControl,delayedControl));
        // Old scope on the current pipe still cannot inherit a fresh identity.
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
            AISyncExecutionCodec.EncodeBatch(new(old.Scope, 1, new[] { Record(old.Scope) }))));
        var currentBytes = AISyncExecutionCodec.EncodeBatch(new(fresh.Scope, 1, new[] { Record(fresh.Scope) }));
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch, currentBytes));
        await Wait(() => batches.Count == 1, token);
        AssertEx.Equal(fresh.Scope.AuthorityEpoch, batches.Single().Scope.AuthorityEpoch);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl,
            AISyncExecutionControlCodec.Encode(fresh with { Kind = AISyncExecutionControlKind.Recovery,
                Value = (uint)AISyncExecutionRecovery.NativeUnavailable })));
        await Wait(() => controls.Any(c => c.Scope == fresh.Scope && c.Kind == AISyncExecutionControlKind.Close), token);
        await Task.Delay(100, token);
        AssertEx.Equal(2, controls.Count(c => c.Kind == AISyncExecutionControlKind.Offer),
            "Coverage loss reopened the same source without a new native incarnation.");
        bridge.Dispose(); await bridge.Completion.WaitAsync(token);
    }

    internal static async Task CancelWaitingAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var sourceControls = new ConcurrentQueue<AISyncExecutionControl>();
        var batches = new ConcurrentQueue<AISyncExecutionBatch>(); var retries = new ConcurrentQueue<AISyncExecutionBatch>();
        client.AISyncExecutionControlReceived += (_, c) =>
        { if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope); };
        client.AISyncExecutionBatchReceived += (_, b) => batches.Enqueue(b);
        client.AISyncExecutionRetryReceived += (_, b) => retries.Enqueue(b);
        using var bridge = new AISyncHostBridge(() => binding, (_, _, _, bytes, _) =>
        { sourceControls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span)); return Task.CompletedTask; }, _ => { }, enabled: true);
        bridge.BindHost(host);
        await Wait(() => sourceControls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
        var offer = sourceControls.Single(c => c.Kind == AISyncExecutionControlKind.Offer);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl,
            AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready })));
        var begin = Record(offer.Scope); var submit = Record(offer.Scope, 2, AISyncTransactionOpcode.Submit);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
            AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 1, new[] { begin, submit }))));
        await Wait(() => batches.Count == 1, token); var first = batches.Single();
        var waiting = new AISyncExecutionReceipt(first.Scope, 1, first.Records[1], AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok);
        AssertEx.True(client.SendAISyncExecutionReceipt(waiting)); await Wait(() => !retries.IsEmpty, token);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
            AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 2, new[] { Record(offer.Scope, 3, AISyncTransactionOpcode.Cancel) }))));
        await Wait(() => batches.Count == 2, token);
        // The final retry obtains the native Superseded disposition after the
        // ordered Cancel; it cannot enter the obsolete request. This evidence
        // is needed before retiring the settled history prefix.
        AssertEx.True(client.SendAISyncExecutionReceipt(waiting));
        AssertEx.True(client.SendAISyncExecutionReceipt(waiting with { Stage = AISyncExecutionStage.Superseded }));
        await Task.Delay(120, token); var count = retries.Count; await Task.Delay(250, token);
        AssertEx.Equal(count, retries.Count, "Cancelled command kept producing retries.");
        AssertEx.False(sourceControls.Any(c => c.Kind == AISyncExecutionControlKind.Close));
        bridge.Dispose(); await bridge.Completion.WaitAsync(token);
    }
}
