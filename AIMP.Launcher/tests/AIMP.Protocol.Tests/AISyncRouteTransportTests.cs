using AIMP.Launcher;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Reflection;
using static AIMP.Protocol.Tests.AISyncExecutionTransportTests;

namespace AIMP.Protocol.Tests;

internal static class AISyncRouteTransportTests
{
    internal static AISyncTransaction Move(AISyncExecutionScope scope) => AISyncMovementTests.Move() with
    { Session = scope.Session, AuthorityEpoch = scope.AuthorityEpoch, WorldEpoch = scope.WorldEpoch, ConsumerEpoch = scope.ConsumerEpoch,
        Movement = AISyncMovementTests.Move().Movement with { Navigation = AISyncMovementNavigation.HostRoute } };
    internal static AISyncRouteGraph Graph(AISyncTransaction r, uint part = 0, uint revision = 1) => AISyncRouteTests.Fixture() with
    {
        Key = new(new(r.Session, r.AuthorityEpoch, r.WorldEpoch, r.ConsumerEpoch), r.OwnerEpoch,
            r.EntityA, r.EntityB, r.Birth, r.CharacterClass, r.Capability, r.CommandId, r.ParameterRevision, revision, part)
    };
    private static AISyncExecutionBatch Batch(AISyncExecutionScope scope)
    {
        var begin = Move(scope);
        return new(scope, 1, new[] { begin, begin with { GlobalSequence = 2, OwnerSequence = 2, Opcode = AISyncTransactionOpcode.Submit } });
    }
    internal static Task JournalAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 2, 3); var batch = Batch(scope);
        var graph = Graph(batch.Records[1]); var bytes = AISyncRouteCodec.Encode(graph);
        var journal = new AISyncRouteJournal(scope);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(bytes));
        AssertEx.True(journal.Observe(new(scope, 1, new[] { batch.Records[0] })));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(bytes), "Begin alone cannot authorize a solved route.");
        AssertEx.True(journal.Observe(new(scope, 2, new[] { batch.Records[1] })));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Append(bytes));
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, journal.Append(bytes));
        var original = bytes.ToArray(); Array.Clear(bytes);
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, journal.Append(original), "Retained route must own its bytes.");
        var companion = AISyncRouteCodec.Encode(graph with { Key = graph.Key with { Part = 1 } });
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Append(companion));
        AssertEx.Equal(2, journal.RouteCount);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(AISyncRouteCodec.Encode(graph with
        { Key = graph.Key with { Scope = scope with { ConsumerEpoch = 4 } } })));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(AISyncRouteCodec.Encode(graph with
        { Key = graph.Key with { OwnerEpoch = 2 } })));
        var conflict = graph with { Auxiliary = new[] { 9F, 1, 2, 3, 4, 5 } };
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, journal.Append(AISyncRouteCodec.Encode(conflict)));
        AssertEx.False(journal.RecoveryRequired); AssertEx.True(journal.IsBlocked(graph.Key));AssertEx.Equal(0,journal.RetainedBytes);
        var refusal=AISyncRouteCodec.Decode(journal.Retained(graph.Key),graph.Key);
        AssertEx.Equal(AISyncRouteFailure.InvalidStream,refusal.Failure);
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate,journal.Append(AISyncRouteCodec.Encode(refusal)));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected,journal.Append(companion));
        var otherRecords=batch.Records.Select((r,i)=>r with {EntityA=r.EntityA+1,CommandId=2,GlobalSequence=(ulong)i+3}).ToArray();
        AssertEx.True(journal.Observe(new(scope,2,otherRecords)));
        var otherGraph=Graph(otherRecords[1]);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Append(AISyncRouteCodec.Encode(otherGraph)));
        AssertEx.False(journal.IsBlocked(otherGraph.Key));AssertEx.Equal(1,journal.RouteCount);
        AssertEx.True(journal.Observe(new(scope,3,new[]{batch.Records[1] with {Opcode=AISyncTransactionOpcode.Update,ParameterRevision=2}})));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected,journal.Append(AISyncRouteCodec.Encode(Graph(batch.Records[1]) with {Key=graph.Key with {ParameterRevision=2}})));
        var malformed=new AISyncRouteJournal(scope);malformed.Observe(batch);var invalid=original.ToArray();invalid[310]=0xFF;
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,malformed.Append(invalid));
        AssertEx.Equal(AISyncRouteFailure.InvalidStream,AISyncRouteCodec.Decode(malformed.Retained(graph.Key),graph.Key).Failure);
        AssertEx.False(malformed.RecoveryRequired);
        var bounded = new AISyncRouteJournal(scope, original.Length, 1); AssertEx.True(bounded.Observe(batch));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, bounded.Append(original));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, bounded.Append(companion));
        AssertEx.False(bounded.RecoveryRequired);AssertEx.True(bounded.IsBlocked(graph.Key));
        AssertEx.Equal(AISyncRouteFailure.Capacity,AISyncRouteCodec.Decode(bounded.Retained(graph.Key with {Part=1}),graph.Key with {Part=1}).Failure);
        var terminal = new AISyncRouteJournal(scope); AssertEx.True(terminal.Observe(batch)); terminal.Append(original);
        var complete = batch.Records[1] with { GlobalSequence = 3, OwnerSequence = 3, Opcode = AISyncTransactionOpcode.Complete, Movement = default };
        AssertEx.True(terminal.Observe(new(scope, 2, new[] { complete })));
        AssertEx.Equal(0, terminal.RouteCount); AssertEx.Equal(0, terminal.RetainedBytes);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, terminal.Append(original));
        AssertEx.True(terminal.Observe(new(scope, 3, new[] { complete with { Opcode = AISyncTransactionOpcode.Release } })));
        var revisions = new AISyncRouteJournal(scope); revisions.Observe(batch); revisions.Append(original);
        var update = batch.Records[1] with { ParameterRevision = 2, Opcode = AISyncTransactionOpcode.Update };
        AssertEx.True(revisions.Observe(new(scope, 2, new[] { update }))); AssertEx.Equal(0, revisions.RouteCount);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, revisions.Append(original));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, revisions.Append(AISyncRouteCodec.Encode(Graph(update))));
        // Replanning does not require an AIEX parameter Update. Full graphs
        // remain independently keyed and byte-exact for each route revision.
        var replans = new AISyncRouteJournal(scope); AssertEx.True(replans.Observe(batch));
        for (uint revision = 1; revision <= 40; revision++)
        {
            var recalculated = AISyncRouteCodec.Encode(Graph(batch.Records[1], revision: revision) with
            { Auxiliary = new[] { (float)revision, 1, 2, 3, 4, 5 } });
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, replans.Append(recalculated));
            AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, replans.Append(recalculated));
        }
        AssertEx.Equal(40, replans.RouteCount);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, replans.Append(companion), "Companion revisions are independent.");
        AssertEx.True(replans.Observe(new(scope, 2, new[] { update })));
        AssertEx.Equal(0, replans.RouteCount);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, replans.Append(original), "Old parameter routes cannot return after Update.");
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, replans.Append(AISyncRouteCodec.Encode(Graph(update))));
        var single = batch.Records.Select(r => r with { Movement = r.Movement with
        { Parts = 1, Companion = default, CompanionFlags = 0, CompanionRetainedAfter = 0 } }).ToArray();
        var one = new AISyncRouteJournal(scope); one.Observe(batch with { Records = single });
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, one.Append(companion));
        var guidedRecords = batch.Records.Select(r => r with { Movement = r.Movement with
        { Navigation = AISyncMovementNavigation.HostGuided } }).ToArray();
        var guided = new AISyncRouteJournal(scope);
        AssertEx.True(guided.Observe(batch with { Records = guidedRecords }));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, guided.Append(original));
        var guidedUpdate = guidedRecords[1] with { Opcode = AISyncTransactionOpcode.Update, ParameterRevision = 2 };
        AssertEx.True(guided.Observe(new(scope, 2, new[] { guidedUpdate })));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, guided.Append(original));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, guided.Append(AISyncRouteCodec.Encode(Graph(guidedUpdate))));
        AssertEx.True(guided.Observe(new(scope, 3, new[] { guidedUpdate with
        { Opcode = AISyncTransactionOpcode.Release, Movement = default } })));
        AssertEx.Equal(0, guided.RouteCount);
        return Task.CompletedTask;
    }
    internal static Task NegotiationAsync()
    {
        var scope = new AISyncExecutionScope(Guid.NewGuid(), 1, 2, 3);
        var strict = Move(scope);
        AssertEx.False(AISyncExecutionControlCodec.Allows((uint)AISyncExecutionProfiles.All, strict));
        AssertEx.True(AISyncExecutionControlCodec.Allows((uint)AISyncExecutionProfiles.Known, strict));
        var local = strict with { Movement = strict.Movement with { Navigation = AISyncMovementNavigation.LocalNative } };
        AssertEx.True(AISyncExecutionControlCodec.Allows((uint)AISyncExecutionProfiles.All, local));
        var journal = new AISyncRouteJournal(scope);
        AssertEx.True(journal.Observe(new(scope, 1, new[] { local, local with {
            GlobalSequence = 2, OwnerSequence = 2, Opcode = AISyncTransactionOpcode.Submit } })));
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, journal.Append(AISyncRouteCodec.Encode(Graph(local))));
        foreach (var profiles in new[] { 12U, 13U, 14U, 15U })
        {
            var control = new AISyncExecutionControl(scope, AISyncExecutionControlKind.Offer, profiles);
            AssertEx.Equal(control, AISyncExecutionControlCodec.Decode(AISyncExecutionControlCodec.Encode(control)));
        }
        foreach (var profiles in new[] { 8U, 9U, 10U, 11U })
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Encode(new(scope, AISyncExecutionControlKind.Offer, profiles)));
        return Task.CompletedTask;
    }
    internal static async Task LoopbackAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var round = await Start(host, new[] { client }, token);
        var received = new ConcurrentQueue<AISyncRouteGraph>(); var order = new ConcurrentQueue<string>(); var receipts = 0;
        client.AISyncExecutionControlReceived += (_, c) => { if (c.Kind == AISyncExecutionControlKind.Offer) client.AcceptAISyncExecution(c.Scope); };
        client.AISyncExecutionBatchReceived += (_, _) => order.Enqueue("AIEX");
        client.AISyncRouteReceived += (_, graph) => { received.Enqueue(graph); order.Enqueue("AISR"); };
        host.AISyncExecutionReceiptReceived += (_, _) => Interlocked.Increment(ref receipts);
        var scope = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1, AISyncExecutionProfiles.Known);
        await Wait(() => host.AISyncExecutionReady(client.PeerId, scope), token);
        var batch = Batch(scope); var graph = Graph(batch.Records[1]); var bytes = AISyncRouteCodec.Encode(graph);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, host.PublishAISyncRoute(client.PeerId, bytes));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncExecution(client.PeerId, AISyncExecutionCodec.EncodeBatch(batch)));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncRoute(client.PeerId, bytes));
        AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, host.PublishAISyncRoute(client.PeerId, bytes));
        Array.Clear(bytes); await Wait(() => received.Count == 1, token);
        AssertEx.SequenceEqual(new[] { "AIEX", "AISR" }, order); AssertEx.Equal(0, receipts);
        AssertEx.SequenceEqual(AISyncRouteCodec.Encode(graph), AISyncRouteCodec.Encode(received.Single()));
        for (uint revision = 2; revision <= 3; revision++)
        {
            var recalculated = graph with { Key = graph.Key with { RouteRevision = revision },
                Auxiliary = new[] { (float)revision, 1, 2, 3, 4, 5 } };
            var replanBytes = AISyncRouteCodec.Encode(recalculated);
            AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncRoute(client.PeerId, replanBytes));
            AssertEx.Equal(AISyncExecutionJournalResult.Duplicate, host.PublishAISyncRoute(client.PeerId, replanBytes));
            await Wait(() => received.Count == revision, token);
            AssertEx.SequenceEqual(replanBytes, AISyncRouteCodec.Encode(received.Last()));
            AssertEx.True(host.AISyncExecutionReady(client.PeerId, scope));
        }
        AssertEx.SequenceEqual(new uint[] { 1, 2, 3 }, received.Select(r => r.Key.RouteRevision));
        AssertEx.SequenceEqual(new[] { "AIEX", "AISR", "AISR", "AISR" }, order);
        AssertEx.Equal(0, receipts, "Transporting a recalculation does not prove native admission.");
        var conflict = graph with { Auxiliary = new[] { 7F, 1, 2, 3, 4, 5 } };
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted, host.PublishAISyncRoute(client.PeerId, AISyncRouteCodec.Encode(conflict)));
        await Wait(()=>received.Count==4,token);
        AssertEx.Equal(AISyncRouteFailure.InvalidStream,received.Last().Failure);
        AssertEx.True(host.AISyncExecutionReady(client.PeerId, scope));
        var otherRecords=batch.Records.Select((r,i)=>r with {EntityA=r.EntityA+1,CommandId=2,GlobalSequence=(ulong)i+3}).ToArray();
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncExecution(client.PeerId,AISyncExecutionCodec.EncodeBatch(new(scope,2,otherRecords))));
        var otherGraph=Graph(otherRecords[1]);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncRoute(client.PeerId,AISyncRouteCodec.Encode(otherGraph)));
        await Wait(()=>received.Count==5,token);
        AssertEx.Equal(2UL,received.Last().Key.CommandId);AssertEx.Equal(AISyncRouteFailure.None,received.Last().Failure);
        AssertEx.True(host.AISyncExecutionReady(client.PeerId,scope));
        await host.StopAISyncExecutionAsync();
        var fresh = host.OfferAISyncExecution(client.PeerId, round.LevelGeneration, 1, AISyncExecutionProfiles.Known);
        await Wait(() => host.AISyncExecutionReady(client.PeerId, fresh), token);
        AssertEx.Equal(AISyncExecutionJournalResult.Rejected, host.PublishAISyncRoute(client.PeerId, AISyncRouteCodec.Encode(graph)));
        AssertEx.True(host.AISyncExecutionReady(client.PeerId, fresh));
        // Inject directly over the authenticated Host socket: prove Client-side
        // checks independently of the public publisher's earlier validation.
        await SendHostRouteAsync(host, client.PeerId, AISyncRouteCodec.Encode(graph), token);
        await Task.Delay(50, token); AssertEx.True(host.AISyncExecutionReady(client.PeerId, fresh));
        var unknown = graph with { Key = graph.Key with { Scope = fresh } };
        await SendHostRouteAsync(host, client.PeerId, AISyncRouteCodec.Encode(unknown), token);
        // A following valid command/route proves the unknown packet did not
        // close the stream and was not delivered as a native route.
        var freshBatch=Batch(fresh);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncExecution(client.PeerId,AISyncExecutionCodec.EncodeBatch(freshBatch)));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncRoute(client.PeerId,AISyncRouteCodec.Encode(Graph(freshBatch.Records[1]))));
        await Wait(()=>received.Count==6,token);AssertEx.True(host.AISyncExecutionReady(client.PeerId,fresh));
        AssertEx.Equal(6,received.Count,"Unknown packet must not be delivered or close the next valid command.");
        var injected=Graph(freshBatch.Records[1]) with {Goal=new[]{42F,0,0}};
        await SendHostRouteAsync(host,client.PeerId,AISyncRouteCodec.Encode(injected),token);
        await Wait(()=>received.Count==7,token);AssertEx.Equal(AISyncRouteFailure.InvalidStream,received.Last().Failure);
        AssertEx.True(host.AISyncExecutionReady(client.PeerId,fresh));
        var afterFailure=freshBatch.Records.Select((r,i)=>r with {EntityA=r.EntityA+1,CommandId=2,GlobalSequence=(ulong)i+3}).ToArray();
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncExecution(client.PeerId,AISyncExecutionCodec.EncodeBatch(new(fresh,2,afterFailure))));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,host.PublishAISyncRoute(client.PeerId,AISyncRouteCodec.Encode(Graph(afterFailure[1]))));
        await Wait(()=>received.Count==8,token);AssertEx.Equal(AISyncRouteFailure.None,received.Last().Failure);
        AssertEx.True(host.AISyncExecutionReady(client.PeerId,fresh));
        await host.StopAISyncExecutionAsync();
    }
    private static async Task SendHostRouteAsync(LobbyHost host, ulong peer, byte[] bytes, CancellationToken token)
    {
        var clients = (System.Collections.IDictionary)typeof(LobbyHost).GetField("_clients", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(host)!;
        var connection = clients[peer]!.GetType().GetProperty("Connection")!.GetValue(clients[peer])!;
        var stream = (NetworkStream)connection.GetType().GetProperty("Stream")!.GetValue(connection)!;
        var gate = (SemaphoreSlim)connection.GetType().GetField("_writeLock", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(connection)!;
        await gate.WaitAsync(token);
        try { await FrameCodec.WriteAsync(stream, new(MessageType.AISyncRouteNetwork, 0, 1, bytes), token); }
        finally { gate.Release(); }
    }
    internal static async Task BridgesAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = timeout.Token;
        await using var host = Host(); await using var first = await Client(host, "Dallas", token);
        await using var second = await Client(host, "Lambert", token);
        var round = await Start(host, new[] { first, second }, token);
        var binding = new AISyncHostBinding(host, host.SessionId, round.LevelGeneration, 7, 123, Guid.NewGuid(), 1, true);
        var clientBinding = new AISyncExecutionBinding(first, host.SessionId, round.LevelGeneration, 8, 456, Guid.NewGuid(), 1, true);
        var controls = new ConcurrentQueue<AISyncExecutionControl>(); var native = new ConcurrentQueue<(MessageType Type, byte[] Bytes)>();
        var routes = new ConcurrentDictionary<ulong, AISyncRouteGraph>(); var logs = new ConcurrentQueue<string>(); var receipts = 0;
        host.AISyncExecutionReceiptReceived += (_, _) => Interlocked.Increment(ref receipts);
        AISyncExecutionBridge? receiver = null;
        using (receiver = new AISyncExecutionBridge(() => clientBinding, (type, connection, pid, bytes, _) =>
        {
            AssertEx.Equal(clientBinding.ConnectionIdentity, connection); AssertEx.Equal(456, pid);
            native.Enqueue((type, bytes.ToArray()));
            if (type == MessageType.AISyncExecutionBinding)
            {
                var control = AISyncExecutionControlCodec.Decode(bytes.Span);
                if (control.Kind == AISyncExecutionControlKind.Offer) receiver!.ObserveNative(new(connection, pid, type,
                    AISyncExecutionControlCodec.Encode(control with { Kind = AISyncExecutionControlKind.Ready })));
            }
            return Task.CompletedTask;
        }, logs.Enqueue, enabled: true))
        using (var source = new AISyncHostBridge(() => binding, (type, _, _, bytes, _) =>
        { AssertEx.Equal(MessageType.AISyncHostBinding, type); controls.Enqueue(AISyncExecutionControlCodec.Decode(bytes.Span)); return Task.CompletedTask; }, logs.Enqueue, enabled: true, authoritativeRoutes: true))
        {
            receiver.BindClient(first);
            second.AISyncExecutionControlReceived += (_, c) => { if (c.Kind == AISyncExecutionControlKind.Offer) second.AcceptAISyncExecution(c.Scope); };
            foreach (var client in new[] { first, second }) client.AISyncRouteReceived += (_, graph) => routes[client.PeerId] = graph;
            source.BindHost(host);
            await Wait(() => controls.Any(c => c.Kind == AISyncExecutionControlKind.Offer), token);
            var offer = controls.Single(c => c.Kind == AISyncExecutionControlKind.Offer);
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostControl,
                AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready })));
            var batch = Batch(offer.Scope); var route = Graph(batch.Records[1]); var bytes = AISyncRouteCodec.Encode(route);
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch, AISyncExecutionCodec.EncodeBatch(batch)));
            source.ObserveNative(new(Guid.NewGuid(), 123, MessageType.AISyncHostRoute, bytes));
            source.ObserveNative(new(binding.ConnectionIdentity, 999, MessageType.AISyncHostRoute, bytes));
            await Wait(() => native.Any(p => p.Type == MessageType.AISyncExecutionBatch), token); AssertEx.Equal(0, routes.Count);
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostRoute, bytes));
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostRoute, bytes));
            await Wait(() => routes.Count == 2 && native.Any(p => p.Type == MessageType.AISyncClientRoute), token);
            foreach (var received in routes.Values)
            {
                AssertEx.Equal(route.Key with { Scope = received.Key.Scope }, received.Key);
                AssertEx.SequenceEqual(bytes, AISyncRouteCodec.Encode(received with { Key = route.Key }));
            }
            AssertEx.True(routes[first.PeerId].Key.Scope.ConsumerEpoch != routes[second.PeerId].Key.Scope.ConsumerEpoch);
            AssertEx.SequenceEqual(AISyncRouteCodec.Encode(routes[first.PeerId]), native.Single(p => p.Type == MessageType.AISyncClientRoute).Bytes);
            AssertEx.Equal(0, receipts, "Route queued to native is not native-applied.");
            AssertEx.SequenceEqual(new[] { MessageType.AISyncExecutionBatch, MessageType.AISyncClientRoute },
                native.Where(p => p.Type is MessageType.AISyncExecutionBatch or MessageType.AISyncClientRoute).Select(p => p.Type));
            for (uint revision = 2; revision <= 3; revision++)
            {
                var recalculated = route with { Key = route.Key with { RouteRevision = revision },
                    Auxiliary = new[] { (float)revision, 1, 2, 3, 4, 5 } };
                var replanBytes = AISyncRouteCodec.Encode(recalculated);
                source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostRoute, replanBytes));
                source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostRoute, replanBytes));
                await Wait(() => routes.Count == 2 && routes.Values.All(g => g.Key.RouteRevision == revision)
                    && native.Count(p => p.Type == MessageType.AISyncClientRoute) == revision, token);
                foreach (var received in routes.Values)
                    AssertEx.SequenceEqual(replanBytes, AISyncRouteCodec.Encode(received with { Key = recalculated.Key }));
                var latestNative = native.Last(p => p.Type == MessageType.AISyncClientRoute);
                AssertEx.SequenceEqual(AISyncRouteCodec.Encode(routes[first.PeerId]), latestNative.Bytes);
            }
            AssertEx.Equal(0, receipts, "Recalculated graphs reaching native IPC do not prove native execution.");
            var refusal=new AISyncRouteGraph(route.Key with {RouteRevision=4},new float[6],new uint[4],Array.Empty<AISyncRouteResource>(),Array.Empty<AISyncRouteNode>()){Failure=AISyncRouteFailure.Unsupported};
            source.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostRoute,AISyncRouteCodec.Encode(refusal)));
            await Wait(()=>routes.Values.All(g=>g.Failure==AISyncRouteFailure.Unsupported)&&native.Count(p=>p.Type==MessageType.AISyncClientRoute)==4,token);
            var otherRecords=batch.Records.Select((r,i)=>r with {EntityA=r.EntityA+1,CommandId=2,GlobalSequence=(ulong)i+3}).ToArray();
            source.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostBatch,AISyncExecutionCodec.EncodeBatch(new(offer.Scope,2,otherRecords))));
            var otherRoute=Graph(otherRecords[1]);
            source.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostRoute,AISyncRouteCodec.Encode(otherRoute)));
            await Wait(()=>routes.Values.All(g=>g.Key.CommandId==2)&&native.Count(p=>p.Type==MessageType.AISyncClientRoute)==5,token);
            var complete = batch.Records[1] with { GlobalSequence = 5, OwnerSequence = 3, Opcode = AISyncTransactionOpcode.Complete, Movement = default };
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostBatch,
                AISyncExecutionCodec.EncodeBatch(new(offer.Scope, 3, new[] { complete }))));
            source.ObserveNative(new(binding.ConnectionIdentity, 123, MessageType.AISyncHostRoute, bytes));
            var update=otherRecords[1] with {GlobalSequence=6,OwnerSequence=3,Opcode=AISyncTransactionOpcode.Update,ParameterRevision=2};
            source.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostBatch,AISyncExecutionCodec.EncodeBatch(new(offer.Scope,4,new[]{update}))));
            source.ObserveNative(new(binding.ConnectionIdentity,123,MessageType.AISyncHostRoute,AISyncRouteCodec.Encode(Graph(update))));
            await Wait(()=>routes.Values.All(g=>g.Key.CommandId==2&&g.Key.ParameterRevision==2)&&native.Count(p=>p.Type==MessageType.AISyncClientRoute)==6,token);
            AssertEx.False(controls.Any(c=>c.Kind==AISyncExecutionControlKind.Close),"A late terminal route must not close unrelated command streams.");
            AssertEx.Equal(0,receipts,"Command refusal is not a fabricated native outcome receipt.");
            source.Dispose(); receiver.Dispose(); await Task.WhenAll(source.Completion, receiver.Completion).WaitAsync(token);
        }
    }
    internal static async Task DirectionAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token = timeout.Token;
        await using var host = Host(); await using var client = await Client(host, "Dallas", token);
        var disconnected = 0; client.Disconnected += (_, _) => Interlocked.Increment(ref disconnected);
        var connection = typeof(LobbyClient).GetField("_connection", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(client)!;
        var stream = (NetworkStream)connection.GetType().GetProperty("Stream")!.GetValue(connection)!;
        var gate = (SemaphoreSlim)connection.GetType().GetField("_writeLock", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(connection)!;
        await gate.WaitAsync(token);
        try { await FrameCodec.WriteAsync(stream, new(MessageType.AISyncRouteNetwork, 0, 1, Array.Empty<byte>()), token); }
        finally { gate.Release(); }
        await Wait(() => Volatile.Read(ref disconnected) != 0, token);
    }
}
