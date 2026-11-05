using AIMP.Launcher;
using System.Buffers.Binary;
using System.Reflection;

namespace AIMP.Protocol.Tests;

internal static class WorldTransactionSkipTests
{
    private const BindingFlags Hidden = BindingFlags.Instance | BindingFlags.NonPublic;
    private static readonly Type CoordinatorType = typeof(MultiplayerSessionCoordinator);
    private static readonly Type TransactionType = CoordinatorType.GetNestedType("ClientWorldTransaction", BindingFlags.NonPublic)!;

    internal static Task SkipAndContinueAsync()
    {
        using var directory = new TemporaryDirectory();
        using var diagnostics = new LauncherDiagnostics(directory.Combine("logs"));
        var settings = new LauncherSettingsStore(directory.Combine("settings.json"));
        using var controller = new NativeRuntimeController(diagnostics, settings);
        using var bridge = new ScaleformBridgeListener(diagnostics);
        using var coordinator = new MultiplayerSessionCoordinator(controller, settings, bridge, new PerformanceIsolationState());
        var manifest = new WorldManifestIdentity(WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion, new WorldEntityId(1, 1));
        var context = new WorldNetworkContext(Guid.NewGuid(), 42, 1, manifest, new string('A', 64), true);
        Set(coordinator, "_localWorldContext", context);
        Set(coordinator, "_nativeWorldBusActive", true);
        Set(coordinator, "_nativeAuthorityBound", true);
        var window = (WorldMutationDispatchWindow)Get(coordinator, "_worldDispatchWindow");
        var dispatchId = 100UL;

        object Transaction(ulong start, int count, WorldNetworkContext? world = null)
        {
            var transaction = Activator.CreateInstance(TransactionType,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null,
                new object?[] { start, count, world ?? context, null }, null)!;
            TransactionType.GetProperty("ReceivedCount", Hidden)!.SetValue(transaction, count);
            var completion = (TaskCompletionSource<bool>)TransactionType.GetProperty("Completion", Hidden)!.GetValue(transaction)!;
            window.Add(start, count, completion);
            return transaction;
        }
        void Result(object transaction, ulong sequence, WorldMutationResultCode code, bool dispatched)
        {
            var id = ++dispatchId;
            Call(coordinator, "TrackWorldResultLocked", id, sequence, transaction);
            Call(coordinator, "OnNativeWorldMutationCompleted", null,
                new WorldMutationResult(sequence, code, dispatched, dispatched, id));
        }
        Task<bool> Completion(object transaction) =>
            ((TaskCompletionSource<bool>)TransactionType.GetProperty("Completion", Hidden)!.GetValue(transaction)!).Task;

        // A multi-root failure must wait for every final result, then release
        // the dispatch window and checkpoint without retiring the live bus.
        var failed = Transaction(1, 2);
        Result(failed, 1, WorldMutationResultCode.RuntimePending, false);
        AssertEx.False(Completion(failed).IsCompleted);
        AssertEx.False((bool)Get(coordinator, "_worldRecoveryRequired"));
        Result(failed, 2, WorldMutationResultCode.QueueRejected, false);
        AssertEx.True(Completion(failed).Result);
        AssertEx.Equal(2UL, (ulong)Get(coordinator, "_lastCommittedWorldSequence"));
        AssertEx.Equal(0, window.PendingRoots);
        AssertEx.True((bool)Get(coordinator, "_nativeWorldBusActive"));
        AssertEx.True(diagnostics.SnapshotEntries().Any(line => line.Contains("transaction 1-2 skipped") && line.Contains("RuntimePending")));

        var next = Transaction(3, 1);
        Result(next, 3, WorldMutationResultCode.Accepted, true);
        AssertEx.True(Completion(next).Result);
        AssertEx.Equal(3UL, (ulong)Get(coordinator, "_lastCommittedWorldSequence"));

        // A partial transaction is never replayed: already applied members
        // remain applied and the failed suffix is consumed once.
        var partial = Transaction(4, 2);
        Result(partial, 4, WorldMutationResultCode.Accepted, true);
        Result(partial, 5, WorldMutationResultCode.NativeFault, false);
        AssertEx.True(Completion(partial).Result);
        AssertEx.Equal(5UL, (ulong)Get(coordinator, "_lastCommittedWorldSequence"));
        AssertEx.False((bool)Get(coordinator, "_worldRecoveryRequired"));
        AssertEx.True((bool)Get(coordinator, "_nativeWorldBusActive"));
        AssertEx.False(diagnostics.SnapshotEntries().Any(line => line.Contains("transaction recovery stopped")));

        // An explicit rejected proposal has no future authority echo. Drop its
        // pending AwaitAuthority cause rather than closing WorldSync later.
        var payload = new byte[WorldPublicActionCodec.RootHeaderBytes];
        payload[0] = WorldPublicActionCodec.RetailRootVersion;
        payload[1] = WorldPublicActionCodec.Kind;
        payload[2] = WorldPublicActionCodec.TypedMessageRoot;
        payload[3] = WorldPublicActionCodec.AwaitAuthority;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4), 65);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(8), 77);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(16), 2);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(24), 2);
        payload[40] = WorldPublicActionCodec.AutonomousContext;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(64), 77);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(72), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(82), 1);
        AssertEx.True(WorldPublicActionCodec.TryDecode(payload, out _));
        var causeType = CoordinatorType.GetNestedType("AwaitAuthorityCause", BindingFlags.NonPublic)!;
        var cause = Activator.CreateInstance(causeType, 42UL, 77UL)!;
        var pending = Get(coordinator, "_pendingAwaitAuthorityCauses");
        pending.GetType().GetMethod("Add")!.Invoke(pending, new[] { cause });
        var rejected = new WorldIntent(manifest, new WorldEntityId(2, 2), 1, 42, 1,
            WorldMutationKind.Event, WorldReplicationConstants.CompositePublicInputOperation, payload);
        ((Task)Call(coordinator, "ReleaseRejectedPublicRootAsync", rejected)!).GetAwaiter().GetResult();
        AssertEx.Equal(0, (int)pending.GetType().GetProperty("Count")!.GetValue(pending)!);
        AssertEx.False((bool)Get(coordinator, "_worldRecoveryRequired"));
        AssertEx.True((bool)Get(coordinator, "_nativeWorldBusActive"));

        // A late result from another world must remain inert.
        var stale = Transaction(6, 1, context with { WorldGeneration = 2 });
        Result(stale, 6, WorldMutationResultCode.RuntimePending, false);
        AssertEx.False(Completion(stale).Result);
        AssertEx.Equal(5UL, (ulong)Get(coordinator, "_lastCommittedWorldSequence"));
        AssertEx.False((bool)Get(coordinator, "_worldRecoveryRequired"));
        return Task.CompletedTask;
    }

    private static object Get(object owner, string name) => CoordinatorType.GetField(name, Hidden)!.GetValue(owner)!;
    private static void Set(object owner, string name, object value) => CoordinatorType.GetField(name, Hidden)!.SetValue(owner, value);
    private static object? Call(object owner, string method, params object?[] arguments) =>
        CoordinatorType.GetMethod(method, Hidden)!.Invoke(owner, arguments);
}
