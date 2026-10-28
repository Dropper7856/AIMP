using AIMP.Launcher;
using System.Collections.Concurrent;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncExecutionBridgeTests
{
    internal static async Task DeliveryAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncExecutionBinding(client, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var native = new ConcurrentQueue<(MessageType Type, byte[] Bytes)>();
        var receipts = new ConcurrentQueue<AISyncExecutionReceipt>();
        host.AISyncExecutionReceiptReceived += (_, value) => receipts.Enqueue(value.Receipt);
        using var bridge = new AISyncExecutionBridge(() => binding, (type, connection, pid, bytes, _) =>
        {
            AssertEx.Equal(binding.ConnectionIdentity, connection); AssertEx.Equal(binding.ProcessId, pid);
            native.Enqueue((type, bytes.ToArray())); return Task.CompletedTask;
        }, _ => { }); // Production default must run without an environment opt-in.
        bridge.BindClient(client);
        var scope = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
        await Wait(() => native.Any(p => p.Type == MessageType.AISyncExecutionBinding), token);
        AssertEx.False(host.AISyncExecutionReady(client.PeerId, scope));
        var ready = AISyncExecutionControlCodec.Encode(new(scope, AISyncExecutionControlKind.Ready, (uint)AISyncExecutionProfiles.All));
        bridge.ObserveNative(new(Guid.NewGuid(), 123, MessageType.AISyncExecutionBinding, ready));
        bridge.ObserveNative(new(binding.ConnectionIdentity, 999, MessageType.AISyncExecutionBinding, ready));
        await Task.Delay(50, token); AssertEx.False(host.AISyncExecutionReady(client.PeerId, scope));
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncExecutionBinding, ready));
        await Wait(() => host.AISyncExecutionReady(client.PeerId, scope), token);
        var begin = Record(scope); var submit = Record(scope, 2, AISyncTransactionOpcode.Submit);
        host.PublishAISyncExecution(client.PeerId, AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { begin, submit })));
        await Wait(() => native.Any(p => p.Type == MessageType.AISyncExecutionBatch), token);
        AssertEx.Equal(0, receipts.Count, "Writing IPC is not native admission.");
        foreach (var receipt in new[] { new AISyncExecutionReceipt(scope, 1, begin, AISyncExecutionStage.Received, AISyncExecutionResult.Ok),
            new AISyncExecutionReceipt(scope, 1, submit, AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok) })
            bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncExecutionReceipt, AISyncExecutionCodec.EncodeReceipt(receipt)));
        await Wait(() => receipts.Count == 2, token);
        AssertEx.True(host.RetryAISyncExecution(client.PeerId, 2));
        await Wait(() => native.Any(p => p.Type == MessageType.AISyncExecutionRetry), token);
        AssertEx.Equal(1, native.Count(p => p.Type == MessageType.AISyncExecutionBatch));
        var admitted = new AISyncExecutionReceipt(scope, 1, submit, AISyncExecutionStage.Admitted, AISyncExecutionResult.Ok);
        bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncExecutionReceipt, AISyncExecutionCodec.EncodeReceipt(admitted)));
        await Wait(() => receipts.Any(r => r.Stage == AISyncExecutionStage.Admitted), token);
        await host.StopAISyncExecutionAsync(); bridge.Dispose(); await bridge.Completion.WaitAsync(token);
    }
    internal static async Task IncarnationsAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var binding = new AISyncExecutionBinding(client, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var controls = new ConcurrentQueue<AISyncExecutionControl>(); AISyncExecutionBridge? bridge = null;
        Task Send(MessageType type, Guid connection, int pid, ReadOnlyMemory<byte> bytes, CancellationToken _)
        {
            var current = Volatile.Read(ref binding);
            if (connection != current.ConnectionIdentity || pid != current.ProcessId) throw new InvalidOperationException("retired IPC instance");
            if (type != MessageType.AISyncExecutionBinding) return Task.CompletedTask;
            var control = AISyncExecutionControlCodec.Decode(bytes.Span); controls.Enqueue(control);
            if (control.Kind == AISyncExecutionControlKind.Offer)
                bridge!.ObserveNative(new(connection, pid, type, AISyncExecutionControlCodec.Encode(control with { Kind = AISyncExecutionControlKind.Ready })));
            return Task.CompletedTask;
        }
        using (bridge = new AISyncExecutionBridge(() => Volatile.Read(ref binding), Send, _ => { }, enabled: true))
        {
            bridge.BindClient(client); var old = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
            await Wait(() => host.AISyncExecutionReady(client.PeerId, old), token);
            var oldConnection = binding.ConnectionIdentity;
            Volatile.Write(ref binding, binding with { ConnectionIdentity = Guid.NewGuid(), Epoch = 2 });
            await Wait(() => !host.AISyncExecutionReady(client.PeerId, old), token);
            await host.StopAISyncExecutionAsync();
            var fresh = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1);
            await Wait(() => host.AISyncExecutionReady(client.PeerId, fresh), token);
            var oldReceipt = AISyncExecutionCodec.EncodeReceipt(new(old, 1, Record(old), AISyncExecutionStage.Received, AISyncExecutionResult.Ok));
            bridge.ObserveNative(new(oldConnection, 123, MessageType.AISyncExecutionReceipt, oldReceipt));
            bridge.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncExecutionReceipt, oldReceipt));
            var badOldReceipt=oldReceipt.ToArray();badOldReceipt[^8]=0xFF;
            bridge.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncExecutionReceipt,badOldReceipt));
            var badOldControl=AISyncExecutionControlCodec.Encode(new(old,AISyncExecutionControlKind.Ready,(uint)AISyncExecutionProfiles.All));
            badOldControl[6]=0xFF;
            bridge.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncExecutionBinding,badOldControl));
            await Task.Delay(80, token);
            AssertEx.True(host.AISyncExecutionReady(client.PeerId, fresh), "Old IPC/scope outcome poisoned a new binding.");
            AssertEx.False(controls.Any(c => c.Scope == fresh && c.Kind == AISyncExecutionControlKind.Recovery));
            var recovery=AISyncExecutionControlCodec.Encode(new(fresh,AISyncExecutionControlKind.Recovery,
                (uint)AISyncExecutionRecovery.NativeUnavailable));
            bridge.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncExecutionBinding,recovery));
            await Wait(()=>controls.Any(c=>c.Scope==fresh&&c.Kind==AISyncExecutionControlKind.Recovery),token);
            await Wait(()=>!host.AISyncExecutionReady(client.PeerId,fresh),token);
            await host.StopAISyncExecutionAsync();
            var restarted=host.OfferAISyncExecution(client.PeerId,round.LevelGeneration,2);
            await Wait(()=>host.AISyncExecutionReady(client.PeerId,restarted),token);
            AssertEx.True(restarted.ConsumerEpoch>fresh.ConsumerEpoch);
            bridge.Dispose(); await bridge.Completion.WaitAsync(token);
        }
        await host.StopAISyncExecutionAsync();
    }
}
