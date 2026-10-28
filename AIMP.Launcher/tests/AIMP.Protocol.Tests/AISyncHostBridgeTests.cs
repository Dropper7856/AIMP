using AIMP.Launcher;
using System.Collections.Concurrent;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncHostBridgeTests
{
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
