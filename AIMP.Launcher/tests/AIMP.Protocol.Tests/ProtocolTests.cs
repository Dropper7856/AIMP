using System.Buffers.Binary;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class ProtocolTests
{
    internal static Task WorldPublicActionContractAsync()
    {
        var entity = new WorldEntityId(0x1122334455667788UL, 0x8877665544332211UL);
        var manifest = new WorldManifestIdentity(
            WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion,
            new WorldEntityId(0x0102030405060708UL, 0x1112131415161718UL));
        AssertWorldPublicAction(manifest, entity);
        AssertWorldReplicationAuthority(manifest, entity);
        AssertWorldManifestV5RoundTrip(entity);
        return Task.CompletedTask;
    }

    internal static async Task FrameRoundTripAndValidationAsync()
    {
        var payload = new byte[] { 0x10, 0x20, 0x30 };
        var frame = new AimpFrame(
            MessageType.OutboundNetZFrame,
            0,
            0x1122334455667788,
            payload);
        var encoded = FrameCodec.Encode(frame);

        AssertHeader(encoded, payload.Length);
        await AssertStreamRoundTripAsync(frame, payload).ConfigureAwait(false);
        AssertInvalidHeaders(encoded);
    }

    internal static Task ModelValidationAsync()
    {
        var identity = Guid.NewGuid();
        var capture = CreateCapture(identity);
        ModelValidation.Validate(capture);
        var skip = new CinematicSkipPayload(identity,1,1);
        ModelValidation.Validate(skip);
        AssertEx.Equal(skip,JsonPayload.Deserialize<CinematicSkipPayload>(JsonPayload.Serialize(skip)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(skip with { Sequence=0 }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(skip with { SessionId=Guid.Empty }));
        var decoded = JsonPayload.Deserialize<CaptureStatePayload>(
            JsonPayload.Serialize(capture));

        AssertEx.Equal(capture, decoded);
        AssertEx.Equal(37, decoded.EngineThreadId);
        AssertEx.True(decoded.EngineHooksInstalled);
        AssertEx.True(decoded.CharacterReadinessProducerHookInstalled);
        AssertEx.Equal(RemoteCharacterStateNames.Ready, decoded.RemoteCharacterState);
        AssertEx.Equal(73U, decoded.RemoteCharacterSpawnKey);
        AssertEx.Equal(0x2C37BBB0UL, decoded.RemoteCharacterAddress);
        AssertEx.Equal(0x1C374418UL, decoded.RemoteCharacterRecordAddress);
        AssertEx.True(decoded.CharacterCreateHookInstalled);
        AssertEx.Throws<ProtocolException>(() =>
            ModelValidation.Validate(capture with { PendingCommandCount = 129 }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            capture with { RemoteCharacterAddress = 0 }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            capture with { RemoteCharacterAddress = 0x2C37BBB1 }));
        var gameplayReady = new GameplayReadyPayload(Guid.NewGuid(), 7, 1);
        ModelValidation.Validate(gameplayReady);
        AssertEx.Throws<ProtocolException>(() =>
            ModelValidation.Validate(gameplayReady with { PeerId = 0 }));
        var spawnPosition = new PlayerSpawnPositionPayload(7, 1, 2, 3);
        ModelValidation.Validate(spawnPosition);
        var positionRequest = new SpawnPositionRequestPayload(
            Guid.NewGuid(), Guid.NewGuid(), 7, 1);
        ModelValidation.Validate(positionRequest);
        var positionResponse = new SpawnPositionResponsePayload(
            positionRequest.SessionId, positionRequest.RequestId, 8, 1, 4, 5, 6);
        ModelValidation.Validate(positionResponse);
        var spawnRequest = new SpawnRemotePlayersPayload(positionRequest.SessionId,
            positionRequest.RequestId, 7, 1,
            new[] { spawnPosition, new PlayerSpawnPositionPayload(8, 4, 5, 6) });
        ModelValidation.Validate(spawnRequest);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            spawnRequest with { RequestId = Guid.Empty }));
        var playerPosition = new PlayerPositionSnapshotPayload(
            positionRequest.SessionId, 7, 1, 1, 1_700_000_000_000, 1, 2, 3, 0.75F,
            PlayerMoveType.Walk, 1.15073F);
        ModelValidation.Validate(playerPosition);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            playerPosition with { Sequence = 0 }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            playerPosition with { Yaw = 4.0F }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            playerPosition with { MoveType = (PlayerMoveType)4 }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            playerPosition with { NativeMoveSpeed = 9.0F }));

        // v12.1 WorldSync 2.0 contract surface exists, but the old Door wire
        // protocol is deliberately gone until the generic v12.4 model lands.
        var worldEntityId = new WorldEntityId(0x1122334455667788UL, 0x8877665544332211UL);
        AssertEx.True(worldEntityId.IsValid);
        AssertEx.False(default(WorldEntityId).IsValid);
        var worldManifestIdentity = new WorldManifestIdentity(
            WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion,
            new WorldEntityId(0x0102030405060708UL, 0x1112131415161718UL));
        var timeline = new WorldTimeline(
            worldManifestIdentity, worldEntityId, 1, 7, 12, 3, new byte[] { 0x55, 0xAA });
        AssertEx.Equal(worldEntityId, timeline.Entity);
        AssertWorldReplicationCodec(worldManifestIdentity, worldEntityId);
        AssertCathodePrimitiveCodec(worldManifestIdentity, worldEntityId);
        AssertWorldInteractionAdapterCodec(worldManifestIdentity, worldEntityId);
        AssertWorldCharacterCallbackInteractionAdapterCodec(worldManifestIdentity, worldEntityId);
        AssertWorldCallbackInteractionAdapterCodec(worldManifestIdentity, worldEntityId);
        AssertWorldCathodeBusCodec(worldManifestIdentity, worldEntityId);
        AssertWorldAnimatedAdapterCodec(worldManifestIdentity, worldEntityId);
        AssertWorldReplicationAuthority(worldManifestIdentity, worldEntityId);
        AssertWorldPublicAction(worldManifestIdentity, worldEntityId);

        AssertEx.Equal(0U,
            WorldManifestRuntimeIdentity.ComputeCathodeHierarchyId(Array.Empty<uint>()));
        AssertEx.Equal(0x11223344U,
            WorldManifestRuntimeIdentity.ComputeCathodeHierarchyId(
                new uint[] { 0x11223344U }));
        AssertEx.Equal(0xA020452CU,
            WorldManifestRuntimeIdentity.ComputeCathodeHierarchyId(
                new uint[] { 0x11223344U, 0x55667788U }));
        AssertEx.Equal(0x1B9744DBU,
            WorldManifestRuntimeIdentity.ComputeCathodeHierarchyId(
                new uint[] { 0x11223344U, 0x55667788U, 0xAABBCCDDU }));
        AssertEx.Equal(0x57EB2E4EU,
            WorldManifestRuntimeIdentity.ComputeCathodeHierarchyId(
                new uint[] { 1U, 2U, 3U, 4U }));

        // v12.3 CLEAN.1: scripted EntityB is the CathodeLib composite-instance
        // ID, anchored to the COMMANDS entry-point tree rather than zero.
        AssertEx.Equal(0x4AF05BFEU,
            WorldManifestRuntimeIdentity.ComputeCathodeCompositeInstanceId(
                Array.Empty<uint>(), WorldManifestRuntimeIdentity.RootInstanceBaseId));
        AssertEx.Equal(0x74566060U,
            WorldManifestRuntimeIdentity.ComputeCathodeCompositeInstanceId(
                new uint[] { 0x11223344U }, WorldManifestRuntimeIdentity.RootInstanceBaseId));
        AssertEx.Equal(0x980F10FAU,
            WorldManifestRuntimeIdentity.ComputeCathodeCompositeInstanceId(
                new uint[] { 0x11223344U, 0x55667788U }, WorldManifestRuntimeIdentity.RootInstanceBaseId));

        AssertWorldManifestV5RoundTrip(worldEntityId);
        foreach (var raw in new ushort[] { 16, 17, 18, 28, 29, 111, 112, 113, 114, 124, 125, 126, 127 })
            AssertEx.True(Enum.IsDefined(typeof(MessageType), raw));
        AssertEx.False(Enum.IsDefined(typeof(MessageType), (ushort)30));
        AssertEx.False(Enum.IsDefined(typeof(MessageType), (ushort)131));
        AssertEx.False(Enum.IsDefined(typeof(MessageType), (ushort)217));

        // ItemSync: 32-hex item keys, never all-zero; identities are validated.
        const string itemKey = "4954000A0014FFE212345678000000AB";
        var itemSession = Guid.NewGuid();
        ModelValidation.Validate(new ItemPickupRequestPayload(itemSession, 7UL, 1, itemKey));
        ModelValidation.Validate(new ItemCollectedPayload(itemSession, 1, itemKey.ToLowerInvariant(), 7UL));
        ModelValidation.Validate(new NativeItemSyncPickupPayload(1, 1UL, itemKey));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            new ItemPickupRequestPayload(itemSession, 7UL, 1, "00000000000000000000000000000000")));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            new ItemPickupRequestPayload(itemSession, 7UL, 1, itemKey[..31])));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            new ItemCollectedPayload(itemSession, 1, itemKey, 0UL)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            new NativeItemSyncPickupPayload(2, 1UL, itemKey)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            new NativeItemSyncPickupPayload(1, 0UL, itemKey)));

        // ItemSync spawn authority: owner decisions (including "nothing") and drops.
        var itemTransform = new float[] { 1F, 0F, 0F, 12.5F, 0F, 1F, 0F, -3F, 0F, 0F, 1F, 0.25F };
        var ownerSpawn = new ItemSpawnPayload(itemSession, 7UL, 1, "owner", "12345678000000AB",
            true, "Pickups\\Medkit", 1, itemTransform);
        ModelValidation.Validate(ownerSpawn);
        ModelValidation.Validate(ownerSpawn with { Spawn = false, Path = "", Quantity = 0 });
        ModelValidation.Validate(ownerSpawn with { Kind = "drop", OwnerKey = "00000001A1D12345" });
        ModelValidation.Validate(new NativeItemSyncSpawnPayload(1, 1UL, "owner", "12345678000000AB",
            false, "", 0, new float[12]));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Kind = "drop" }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Kind = "drop", OwnerKey = "00000001A1D12345", Spawn = false, Path = "" }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Spawn = false }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { OwnerKey = "0000000000000000" }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Transform = new float[11] }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Transform = Enumerable.Repeat(float.NaN, 12).ToArray() }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Path = "Pickups\\\nMedkit" }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            ownerSpawn with { Kind = "unknown" }));
        AssertEx.True(ModelValidation.TryParseItemTransform(
            ModelValidation.FormatItemTransform(itemTransform), out var parsedTransform));
        AssertEx.True(parsedTransform.SequenceEqual(itemTransform));
        AssertEx.False(Enum.IsDefined(typeof(MessageType), (ushort)218));

        AssertGameCommand(identity);
        return Task.CompletedTask;
    }

    internal static async Task LobbyLifecycleAsync()
    {
        var compatibility = CreateCompatibility();
        await using var host = new LobbyHost(new LobbyHostOptions(
            "Nostromo",
            "Ripley",
            Guid.NewGuid(),
            0,
            2,
            "TestLevel",
            compatibility));
        host.Start();
        await using var client = await ConnectClientAsync(host, compatibility)
            .ConfigureAwait(false);

        var started = new TaskCompletionSource<LobbyStartPayload>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        client.Started += (_, value) => started.TrySetResult(value);
        client.Start();
        AssertEx.Equal(2, client.Snapshot?.Players.Count ?? 0);

        host.SetHostReady(false);
        AssertEx.False(host.CanStart);
        await client.SetReadyAsync(true).ConfigureAwait(false);
        await WaitUntilAsync(() => host.CanStart).ConfigureAwait(false);
        var expected = await host.StartGameAsync(DateTimeOffset.UtcNow)
            .ConfigureAwait(false);
        var actual = await started.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        AssertEx.Equal(expected.SessionId, actual.SessionId);
        AssertEx.Equal(expected.LevelGeneration, actual.LevelGeneration);
        await WaitUntilAsync(() =>
            client.Snapshot?.LevelGeneration == expected.LevelGeneration).ConfigureAwait(false);
        AssertEx.Equal(expected.LevelGeneration, client.Snapshot?.LevelGeneration ?? 0);
        await AssertGameplayReadyRelayAsync(host, client, expected).ConfigureAwait(false);
        await AssertPlayerPositionRelayAsync(host, client, expected).ConfigureAwait(false);
        await AssertSpawnRequestRelayAsync(host, client, expected).ConfigureAwait(false);
        await AssertWorldReplicationRelayAsync(host, client, expected).ConfigureAwait(false);
    }

    private static void AssertWorldReplicationCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var intent = new WorldIntent(
            manifest, entity, 7, 0, 9, WorldMutationKind.Event,
            WorldReplicationConstants.NetworkProbeOperation,
            new byte[] { 1, 2, 3 });
        var decodedIntent = WorldReplicationCodec.DecodeIntent(
            WorldReplicationCodec.EncodeIntent(intent));
        AssertEx.Equal(intent.Manifest, decodedIntent.Manifest);
        AssertEx.Equal(intent.Entity, decodedIntent.Entity);
        AssertEx.Equal(intent.LevelGeneration, decodedIntent.LevelGeneration);
        AssertEx.Equal(intent.ClientSequence, decodedIntent.ClientSequence);
        AssertEx.SequenceEqual(intent.Payload, decodedIntent.Payload);

        var mutation = new WorldMutation(
            manifest, WorldMutationKind.Event,
            WorldMutationFlags.Reliable | WorldMutationFlags.SyntheticProbe,
            entity, 7, 11, 0, 13, 17,
            WorldReplicationConstants.NetworkProbeOperation,
            new byte[] { 4, 5, 6 });
        var decodedMutation = WorldReplicationCodec.DecodeMutation(
            WorldReplicationCodec.EncodeMutation(mutation));
        AssertEx.Equal(mutation.Manifest, decodedMutation.Manifest);
        AssertEx.Equal(mutation.Sequence, decodedMutation.Sequence);
        AssertEx.Equal(mutation.TimelineTick, decodedMutation.TimelineTick);
        AssertEx.Equal(mutation.Flags, decodedMutation.Flags);
        AssertEx.SequenceEqual(mutation.Payload, decodedMutation.Payload);

        var state = mutation with
        {
            Kind = WorldMutationKind.State,
            Flags = WorldMutationFlags.Reliable | WorldMutationFlags.Replaceable
                | WorldMutationFlags.Replayable,
            Sequence = 12,
            Revision = 1,
            TimelineTick = 14,
            Operation = 0x1234U
        };
        WorldReplicationValidation.Validate(state);
        AssertEx.Equal(1UL, WorldReplicationCodec.DecodeMutation(
            WorldReplicationCodec.EncodeMutation(state)).Revision);

        var result = new WorldMutationResult(11, WorldMutationResultCode.Accepted, true, true);
        AssertEx.Equal(result, WorldReplicationCodec.DecodeResult(
            WorldReplicationCodec.EncodeResult(result)));

        var session = Guid.NewGuid();
        var context = new WorldNetworkContext(
            session, 21, 3, manifest, new string('A', 64), true);
        AssertEx.Equal(context, WorldReplicationCodec.DecodeNetworkContext(
            WorldReplicationCodec.EncodeNetworkContext(context)));
        var networkIntent = new WorldNetworkIntent(session, 3, intent with { SourcePeer = 21 });
        var decodedNetworkIntent = WorldReplicationCodec.DecodeNetworkIntent(
            WorldReplicationCodec.EncodeNetworkIntent(networkIntent));
        AssertEx.Equal(networkIntent.SessionId, decodedNetworkIntent.SessionId);
        AssertEx.Equal(networkIntent.WorldGeneration, decodedNetworkIntent.WorldGeneration);
        AssertEx.Equal(21UL, decodedNetworkIntent.Intent.SourcePeer);
        var networkMutation = new WorldNetworkMutation(
            session, 3, mutation with { LevelGeneration = 3 });
        AssertEx.Equal(networkMutation.Mutation.Sequence,
            WorldReplicationCodec.DecodeNetworkMutation(
                WorldReplicationCodec.EncodeNetworkMutation(networkMutation)).Mutation.Sequence);
        var ack = new WorldNetworkMutationAck(session, 21, 3, result);
        AssertEx.Equal(ack, WorldReplicationCodec.DecodeNetworkAck(
            WorldReplicationCodec.EncodeNetworkAck(ack)));

        var trailing = WorldReplicationCodec.EncodeIntent(intent).Concat(new byte[] { 0 }).ToArray();
        AssertEx.Throws<ProtocolException>(() =>
        {
            _ = WorldReplicationCodec.DecodeIntent(trailing);
        });
        AssertEx.Throws<ProtocolException>(() => WorldReplicationCodec.EncodeIntent(
            intent with { Payload = new byte[WorldReplicationConstants.MaximumPayloadBytes + 1] }));
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            state with { Revision = 0 }));
    }

    private static void AssertCathodePrimitiveCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        const uint input = 0xA1B2C3D4U;
        var payload = CathodePrimitiveCodec.EncodeTriggerInput(input, entity);
        AssertEx.Equal(CathodePrimitiveCodec.TriggerInputPayloadBytes, payload.Length);
        AssertEx.Equal(CathodePrimitiveCodec.PayloadVersion, payload[0]);
        AssertEx.Equal(CathodePrimitiveCodec.TriggerInputKind, payload[1]);
        AssertEx.Equal(CathodePrimitiveCodec.SourceEntityFlag, payload[2]);
        AssertEx.Equal(input, BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4)));
        AssertEx.Equal(entity.High, BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(8, 8)));
        AssertEx.Equal(entity.Low, BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(16, 8)));

        var decoded = CathodePrimitiveCodec.DecodeTriggerInput(payload);
        AssertEx.Equal(input, decoded.InputShortGuid);
        AssertEx.Equal(entity, decoded.SourceEntity ?? default);

        var intent = new WorldIntent(
            manifest, entity, 7, 9, 11, WorldMutationKind.Event,
            CathodePrimitiveCodec.TriggerInputOperation, payload);
        WorldReplicationValidation.Validate(intent);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            intent with { RequestedKind = WorldMutationKind.State }));

        var mutation = new WorldMutation(
            manifest, WorldMutationKind.Event, WorldMutationFlags.Reliable,
            entity, 7, 12, 0, 13, 9,
            CathodePrimitiveCodec.TriggerInputOperation, payload);
        WorldReplicationValidation.Validate(mutation);

        var badHeader = payload.ToArray();
        badHeader[3] = 1;
        AssertEx.Throws<ProtocolException>(() => CathodePrimitiveCodec.DecodeTriggerInput(badHeader));
        var badFlags = payload.ToArray();
        badFlags[2] = 0x80;
        AssertEx.Throws<ProtocolException>(() => CathodePrimitiveCodec.DecodeTriggerInput(badFlags));
        AssertEx.Throws<ProtocolException>(() => CathodePrimitiveCodec.EncodeTriggerInput(0));
    }

    private static void AssertWorldInteractionAdapterCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var source = new WorldEntityId(0x0123456789ABCDEFUL, 0x1020304050607080UL);
        var value = new WorldObservedInputEdgePayload(
            WorldInteractionSourceCategory.GenericWorld,
            0xA1B2C3D4U,
            0xF2FEBB21U,
            0x8877665544332211UL,
            source,
            1.25F);
        var payload = WorldInteractionAdapterCodec.EncodeObservedInputEdge(value);
        AssertEx.Equal(WorldInteractionAdapterCodec.PayloadBytes, payload.Length);
        AssertEx.Equal(WorldInteractionAdapterCodec.PayloadVersion, payload[0]);
        AssertEx.Equal(WorldInteractionAdapterCodec.ObservedInputEdgeKind, payload[1]);
        AssertEx.Equal((byte)WorldInteractionSourceCategory.GenericWorld, payload[2]);
        AssertEx.Equal(value.SchedulerParameterShortGuid,
            BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4)));
        AssertEx.Equal(value.TargetInputShortGuid,
            BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(8, 4)));
        AssertEx.Equal(value.ObservationId,
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(16, 8)));
        AssertEx.Equal(source.High,
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(24, 8)));
        AssertEx.Equal(source.Low,
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(32, 8)));
        AssertEx.Equal(value, WorldInteractionAdapterCodec.DecodeObservedInputEdge(payload));

        var intent = new WorldIntent(
            manifest, entity, 7, 9, 12, WorldMutationKind.Event,
            WorldInteractionAdapterCodec.ObservedInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(intent);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            intent with { RequestedKind = WorldMutationKind.State }));

        var mutation = new WorldMutation(
            manifest, WorldMutationKind.Event, WorldMutationFlags.Reliable,
            entity, 7, 15, 0, 16, 9,
            WorldInteractionAdapterCodec.ObservedInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(mutation);

        var badReserved = payload.ToArray();
        badReserved[12] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.DecodeObservedInputEdge(badReserved));
        var badTailReserved = payload.ToArray();
        badTailReserved[44] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.DecodeObservedInputEdge(badTailReserved));
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.EncodeObservedInputEdge(
                value with { ObservationId = 0 }));
    }


    private static void AssertWorldCharacterCallbackInteractionAdapterCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var source = new WorldEntityId(0x1111111122222222UL, 0x3333333344444444UL);
        var callback = new WorldEntityId(0x5555555566666666UL, 0x7777777788888888UL);
        var value = new WorldObservedCharacterCallbackInputEdgePayload(
            WorldInteractionSourceCategory.GenericWorld,
            0xEB826BC5U,
            0x06478FC6U,
            WorldInteractionAdapterCodec.CharacterCallbackInputShortGuid,
            WorldInteractionAdapterCodec.CharacterCallbackConstructorFlags,
            WorldInteractionAdapterCodec.CharacterCallbackExecutionVtableRva,
            WorldInteractionAdapterCodec.CharacterCallbackFamilyCallerRva,
            0x1020304050607080UL,
            source,
            callback,
            0.0F);
        var payload = WorldInteractionAdapterCodec.EncodeObservedCharacterCallbackInputEdge(value);
        AssertEx.Equal(WorldInteractionAdapterCodec.CharacterCallbackPayloadBytes, payload.Length);
        AssertEx.Equal(WorldInteractionAdapterCodec.ObservedCharacterCallbackInputEdgeKind, payload[1]);
        AssertEx.Equal(value, WorldInteractionAdapterCodec.DecodeObservedCharacterCallbackInputEdge(payload));
        var intent = new WorldIntent(
            manifest, entity, 7, 9, 20, WorldMutationKind.Event,
            WorldInteractionAdapterCodec.ObservedCharacterCallbackInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(intent);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            intent with { RequestedKind = WorldMutationKind.State }));
        var mutation = new WorldMutation(
            manifest, WorldMutationKind.Event, WorldMutationFlags.Reliable,
            entity, 7, 21, 0, 22, 9,
            WorldInteractionAdapterCodec.ObservedCharacterCallbackInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(mutation);
        var bad = payload.ToArray(); bad[28] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.DecodeObservedCharacterCallbackInputEdge(bad));
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.EncodeObservedCharacterCallbackInputEdge(
                value with { CallbackConstructorFlags = 0x80000000U }));
    }

    private static void AssertWorldCallbackInteractionAdapterCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var source = new WorldEntityId(0x1111222233334444UL, 0x5555666677778888UL);
        var value = new WorldObservedCallbackInputEdgePayload(
            WorldInteractionSourceCategory.GenericWorld,
            0x1BF58EB1U,
            0xD2B3CC2BU,
            WorldInteractionAdapterCodec.CallbackReferenceConstructorFlags,
            0U,
            WorldInteractionAdapterCodec.CallbackReferenceFamilyCallerRva,
            0x1020304050607080UL,
            source,
            0.125F);
        var payload = WorldInteractionAdapterCodec.EncodeObservedCallbackInputEdge(value);
        AssertEx.Equal(WorldInteractionAdapterCodec.CallbackPayloadBytes, payload.Length);
        AssertEx.Equal(WorldInteractionAdapterCodec.ObservedCallbackInputEdgeKind, payload[1]);
        AssertEx.Equal(value, WorldInteractionAdapterCodec.DecodeObservedCallbackInputEdge(payload));

        var intent = new WorldIntent(
            manifest, entity, 7, 9, 21, WorldMutationKind.Event,
            WorldInteractionAdapterCodec.ObservedCallbackInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(intent);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            intent with { RequestedKind = WorldMutationKind.State }));

        var mutation = new WorldMutation(
            manifest, WorldMutationKind.Event, WorldMutationFlags.Reliable,
            entity, 7, 22, 0, 23, 9,
            WorldInteractionAdapterCodec.ObservedCallbackInputEdgeOperation, payload);
        WorldReplicationValidation.Validate(mutation);

        var badTail = payload.ToArray();
        badTail[52] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.DecodeObservedCallbackInputEdge(badTail));
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.EncodeObservedCallbackInputEdge(
                value with { CallbackFamilyCallerRva = 0x12345678U }));
        AssertEx.Throws<ProtocolException>(() =>
            WorldInteractionAdapterCodec.EncodeObservedCallbackInputEdge(
                value with { CallbackConstructorFlags = 0x10000000U }));
    }


    private static void AssertWorldCathodeBusCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var source = new WorldCathodeRuntimeKey(0x11112222U, 0x33334444U);
        var target = new WorldCathodeRuntimeKey(0x55556666U, 0x77778888U);
        var value = new WorldCathodeTypedMessagePayload(
            0x100050U,
            0xAABBCCDDEEFF0011UL,
            source,
            target,
            0.75,
            new byte[] { 0x10, 0x20, 0x30, 0x40, 0x50 });
        var payload = WorldCathodeBusCodec.EncodeTypedMessage(value);
        AssertEx.Equal(WorldCathodeBusCodec.TypedMessageHeaderBytes + value.Data.Length, payload.Length);
        var decoded = WorldCathodeBusCodec.DecodeTypedMessage(payload);
        AssertEx.Equal(value.MessageType, decoded.MessageType);
        AssertEx.Equal(value.ObservationId, decoded.ObservationId);
        AssertEx.Equal(value.SourceRuntimeKey, decoded.SourceRuntimeKey);
        AssertEx.Equal(value.TargetRuntimeKey, decoded.TargetRuntimeKey);
        AssertEx.Equal(value.RelativeDelaySeconds, decoded.RelativeDelaySeconds);
        AssertEx.SequenceEqual(value.Data, decoded.Data);

        // Fix3.14 BuildFix01: the Launcher must accept the diagnostic v3
        // typed-message envelope emitted by the native probe and preserve the
        // data offset after the two stable WorldEntityIds. Production encoding
        // remains v2-only.
        var identityPayload = new byte[WorldCathodeBusCodec.TypedMessageIdentityHeaderBytes + value.Data.Length];
        payload.AsSpan(0, WorldCathodeBusCodec.TypedMessageHeaderBytes).CopyTo(identityPayload);
        identityPayload[0] = WorldCathodeBusCodec.IdentityPayloadVersion;
        BinaryPrimitives.WriteUInt64LittleEndian(identityPayload.AsSpan(48, 8), 0x0123456789ABCDEFUL);
        BinaryPrimitives.WriteUInt64LittleEndian(identityPayload.AsSpan(56, 8), 0x1111222233334444UL);
        BinaryPrimitives.WriteUInt64LittleEndian(identityPayload.AsSpan(64, 8), 0xFEDCBA9876543210UL);
        BinaryPrimitives.WriteUInt64LittleEndian(identityPayload.AsSpan(72, 8), 0x5555666677778888UL);
        value.Data.CopyTo(identityPayload.AsSpan(WorldCathodeBusCodec.TypedMessageIdentityHeaderBytes));
        var identityDecoded = WorldCathodeBusCodec.DecodeTypedMessage(identityPayload);
        AssertEx.Equal(value.MessageType, identityDecoded.MessageType);
        AssertEx.Equal(value.ObservationId, identityDecoded.ObservationId);
        AssertEx.Equal(value.SourceRuntimeKey, identityDecoded.SourceRuntimeKey);
        AssertEx.Equal(value.TargetRuntimeKey, identityDecoded.TargetRuntimeKey);
        AssertEx.Equal(value.RelativeDelaySeconds, identityDecoded.RelativeDelaySeconds);
        AssertEx.SequenceEqual(value.Data, identityDecoded.Data);

        var invalidIdentity = identityPayload.ToArray();
        invalidIdentity.AsSpan(48, 32).Clear();
        AssertEx.Throws<ProtocolException>(() =>
            WorldCathodeBusCodec.DecodeTypedMessage(invalidIdentity));

        var intent = new WorldIntent(
            manifest, entity, 7, 9, 14, WorldMutationKind.Event,
            WorldCathodeBusCodec.TypedMessageOperation, payload);
        WorldReplicationValidation.Validate(intent);
        WorldReplicationValidation.Validate(intent with { Payload = identityPayload });
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            intent with { RequestedKind = WorldMutationKind.State }));

        var badReserved = payload.ToArray();
        badReserved[42] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldCathodeBusCodec.DecodeTypedMessage(badReserved));
        AssertEx.Throws<ProtocolException>(() => WorldCathodeBusCodec.EncodeTypedMessage(
            value with { Data = new byte[WorldCathodeBusCodec.MaximumTypedMessageDataBytes + 1] }));
    }

    private static void AssertWorldAnimatedAdapterCodec(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var door = new WorldDoorActionPayload(
            WorldDoorAction.ForceOpen,
            0x1020304050607080UL);
        var doorPayload = WorldAnimatedAdapterCodec.EncodeDoorAction(door);
        AssertEx.Equal(WorldAnimatedAdapterCodec.DoorActionPayloadBytes, doorPayload.Length);
        AssertEx.Equal(door, WorldAnimatedAdapterCodec.DecodeDoorAction(doorPayload));
        var doorIntent = new WorldIntent(
            manifest, entity, 7, 9, 13, WorldMutationKind.Event,
            WorldAnimatedAdapterCodec.DoorActionOperation, doorPayload);
        WorldReplicationValidation.Validate(doorIntent);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            doorIntent with { RequestedKind = WorldMutationKind.State }));

        var source = new WorldEntityId(0x8877665544332211UL, 0x1020304050607080UL);
        var animated = new WorldAnimatedInputEdgePayload(
            WorldAnimatedTargetCategory.PlayEnvironmentAnimation,
            0xA1B2C3D4U,
            0xF2FEBB21U,
            0x0123456789ABCDEFUL,
            source,
            2.5F);
        var animatedPayload = WorldAnimatedAdapterCodec.EncodeAnimatedInputEdge(animated);
        AssertEx.Equal(WorldAnimatedAdapterCodec.AnimatedInputEdgePayloadBytes, animatedPayload.Length);
        AssertEx.Equal(animated, WorldAnimatedAdapterCodec.DecodeAnimatedInputEdge(animatedPayload));
        var animatedMutation = new WorldMutation(
            manifest, WorldMutationKind.Event, WorldMutationFlags.Reliable,
            entity, 7, 17, 0, 18, 9,
            WorldAnimatedAdapterCodec.AnimatedInputEdgeOperation, animatedPayload);
        WorldReplicationValidation.Validate(animatedMutation);
        AssertEx.Throws<ProtocolException>(() => WorldReplicationValidation.Validate(
            animatedMutation with {
                Kind = WorldMutationKind.State,
                Flags = WorldMutationFlags.Reliable | WorldMutationFlags.Replaceable | WorldMutationFlags.Replayable,
                Revision = 1
            }));

        var badReserved = animatedPayload.ToArray();
        badReserved[44] = 1;
        AssertEx.Throws<ProtocolException>(() =>
            WorldAnimatedAdapterCodec.DecodeAnimatedInputEdge(badReserved));
    }

    private static void AssertWorldPublicAction(WorldManifestIdentity manifest, WorldEntityId entity)
    {
        var golden = Convert.FromHexString("0607000178563412080706050403020188776655443322111122334455667788000000000000F83F");
        AssertEx.True(WorldPublicActionCodec.TryDecode(golden, out var input, out var observation, out var root));
        AssertEx.Equal(0x12345678U, input);
        AssertEx.Equal(0x0102030405060708UL, observation);
        AssertEx.True(root);
        for (var version = 0; version < 256; version++)
        {
            var sample = golden.ToArray(); sample[0] = (byte)version;
            AssertEx.Equal(version == 6, WorldPublicActionCodec.TryDecode(sample, out _, out _, out _));
        }
        AssertEx.True(WorldPublicActionCodec.TryDecode(golden, out var action));
        AssertEx.Equal(entity, action.Target);
        AssertEx.Equal(1.5, action.RelativeDelaySeconds);
        AssertEx.Equal(WorldPublicActionCodec.ExternalInvocation, action.Flags);
        var detached = golden.ToArray(); detached[3] = WorldPublicActionCodec.DetachedCallback;
        AssertEx.True(WorldPublicActionCodec.TryDecode(detached, out var detachedAction));
        AssertEx.Equal(WorldPublicActionCodec.DetachedCallback, detachedAction.Flags);
        var conflictingFlags = golden.ToArray(); conflictingFlags[3] = WorldPublicActionCodec.KnownFlags;
        AssertEx.False(WorldPublicActionCodec.TryDecode(conflictingFlags, out _));
        for (var length = 0; length < golden.Length; ++length)
            AssertEx.False(WorldPublicActionCodec.TryDecode(golden.AsSpan(0, length), out _));
        AssertEx.False(WorldPublicActionCodec.TryDecode(golden.Concat(new byte[] { 0 }).ToArray(), out _));
        foreach (var invalidDelay in new[] { -1.0, 601.0, double.NaN, double.PositiveInfinity })
        {
            var bad = golden.ToArray();
            BinaryPrimitives.WriteDoubleLittleEndian(bad.AsSpan(32, 8), invalidDelay);
            AssertEx.False(WorldPublicActionCodec.TryDecode(bad, out _));
        }
        foreach (var offset in new[] { 1, 2, 3 })
        {
            var bad = golden.ToArray(); bad[offset] = 0xFF;
            AssertEx.False(WorldPublicActionCodec.TryDecode(bad, out _));
        }
        var missingTarget = golden.ToArray(); missingTarget.AsSpan(16, 16).Clear();
        AssertEx.False(WorldPublicActionCodec.TryDecode(missingTarget, out _));
        var legacy = golden[..16]; legacy[0] = 1; legacy[3] = 0;
        AssertEx.True(WorldPublicActionCodec.TryDecode(legacy, out action));
        AssertEx.False(action.RetailRoot);
        legacy[0] = 4;
        AssertEx.False(WorldPublicActionCodec.TryDecode(legacy, out _));
        var obsoleteV5 = golden.ToArray(); obsoleteV5[0] = 5;
        AssertEx.False(WorldPublicActionCodec.TryDecode(obsoleteV5, out _));
        var session = Guid.NewGuid();
        var authority = new WorldReplicationAuthority(session, 10);
        authority.SetHostContext(new WorldNetworkContext(session, 10, 4, manifest, new string('A', 64), true));
        authority.SetPeerContext(new WorldNetworkContext(session, 20, 4, manifest, new string('A', 64), true));
        var owner = new WorldEntityId(entity.High ^ 1UL, entity.Low);
        var intent = new WorldNetworkIntent(session, 4, new WorldIntent(manifest, owner, 71, 20, 1,
            WorldMutationKind.Event, WorldReplicationConstants.CompositePublicInputOperation, golden));
        var malformed = golden.ToArray(); malformed[2] = 1;
        AssertEx.False(authority.TryAcceptIntent(intent with { Intent = intent.Intent with { Payload = malformed } },
            20, out _, out var error));
        AssertEx.Equal("INVALID_COMPOSITE_PUBLIC_INPUT", error);
        AssertEx.True(authority.TryAcceptIntent(intent, 20, out var mutation, out error), error);
        AssertEx.Equal(1UL, mutation.Mutation.Sequence); // rejected bytes did not consume sequence
        AssertEx.Equal(0UL, mutation.Mutation.Revision); // ROOT is transient, never a persistent snapshot
        AssertEx.Equal(owner, mutation.Mutation.Entity);
        AssertEx.True(WorldPublicActionCodec.TryDecode(mutation.Mutation.Payload, out action));
        AssertEx.Equal(entity, action.Target); // authority must not collapse target into owner
        AssertEx.Equal(1.5, action.RelativeDelaySeconds);
        AssertEx.False(authority.TryAcceptIntent(intent, 20, out _, out _));
        var repeated = golden.ToArray(); repeated[8] = 9;
        AssertEx.True(authority.TryAcceptIntent(intent with
        { Intent = intent.Intent with { ClientSequence = 2, Payload = repeated } }, 20, out mutation, out error), error);
        AssertEx.Equal(2UL, mutation.Mutation.Sequence); // distinct rapid actions are not time-deduplicated
        var detachedAuthorityPayload = golden.ToArray();
        detachedAuthorityPayload[3] = WorldPublicActionCodec.DetachedCallback;
        detachedAuthorityPayload[8] = 10; // distinct observation id
        AssertEx.True(authority.TryAcceptIntent(intent with
        { Intent = intent.Intent with { ClientSequence = 3, Payload = detachedAuthorityPayload } },
            20, out var detachedMutation, out error), error);
        AssertEx.True(WorldPublicActionCodec.TryDecode(detachedMutation.Mutation.Payload, out var detachedAuthorityAction));
        AssertEx.Equal(WorldPublicActionCodec.DetachedCallback, detachedAuthorityAction.Flags);
        AssertEx.Equal(entity, detachedAuthorityAction.Target);
        AssertEx.False(authority.TryAcceptIntent(intent with { WorldGeneration = 5 }, 20, out _, out _));

        // Defense-in-depth: even if Native regresses and starts producing a
        // callback-root flood again, authority must not fan out an unbounded
        // number of public ROOTs into every receiver's game-thread dispatcher.
        var limitedAuthority = new WorldReplicationAuthority(session, 10);
        limitedAuthority.SetHostContext(new WorldNetworkContext(
            session, 10, 4, manifest, new string('A', 64), true));
        limitedAuthority.SetPeerContext(new WorldNetworkContext(
            session, 20, 4, manifest, new string('A', 64), true));
        for (ulong sequence = 1; sequence <= 64; sequence++)
        {
            var boundedIntent = intent with
            {
                Intent = intent.Intent with { ClientSequence = sequence }
            };
            AssertEx.True(limitedAuthority.TryAcceptIntent(
                boundedIntent, 20, out _, out var boundedError), boundedError);
        }
        var overBudget = intent with
        {
            Intent = intent.Intent with { ClientSequence = 65 }
        };
        AssertEx.False(limitedAuthority.TryAcceptIntent(
            overBudget, 20, out _, out var rateError));
        AssertEx.Equal("WS3_PUBLIC_ROOT_RATE_LIMIT", rateError);
    }

    private static void AssertWorldReplicationAuthority(
        WorldManifestIdentity manifest,
        WorldEntityId entity)
    {
        var session = Guid.NewGuid();
        const ulong hostPeer = 10;
        const ulong clientPeer = 20;
        var authority = new WorldReplicationAuthority(session, hostPeer);
        authority.SetHostContext(new WorldNetworkContext(
            session, hostPeer, 4, manifest, new string('A', 64), true));
        authority.SetPeerContext(new WorldNetworkContext(
            session, clientPeer, 4, manifest, new string('A', 64), true));
        AssertEx.True(authority.AllPeersCompatible(new[] { clientPeer }));

        var eventIntent = new WorldNetworkIntent(session, 4, new WorldIntent(
            manifest, entity, 71, clientPeer, 1, WorldMutationKind.Event,
            WorldReplicationConstants.NetworkProbeOperation, Array.Empty<byte>()));
        AssertEx.True(authority.TryAcceptIntent(
            eventIntent, clientPeer, out var first, out var firstError), firstError);
        AssertEx.Equal(1UL, first.Mutation.Sequence);
        AssertEx.Equal(1UL, first.Mutation.TimelineTick);
        AssertEx.Equal(0UL, first.Mutation.Revision);
        AssertEx.True((first.Mutation.Flags & WorldMutationFlags.SyntheticProbe) != 0);
        AssertEx.False(authority.TryAcceptIntent(
            eventIntent, clientPeer, out _, out var duplicate));
        AssertEx.Equal("DUPLICATE_INTENT", duplicate);

        authority.SetPeerContext(new WorldNetworkContext(
            session, clientPeer, 4, manifest, new string('A', 64), false));
        authority.SetPeerContext(new WorldNetworkContext(
            session, clientPeer, 4, manifest, new string('A', 64), true));
        AssertEx.True(authority.TryAcceptIntent(
            eventIntent, clientPeer, out var afterPeerReset, out var resetError), resetError);
        AssertEx.Equal(2UL, afterPeerReset.Mutation.Sequence);

        var animatedPayload = WorldAnimatedAdapterCodec.EncodeAnimatedInputEdge(
            new WorldAnimatedInputEdgePayload(
                WorldAnimatedTargetCategory.PlayEnvironmentAnimation,
                0xA1B2C3D4U, 0xF2FEBB21U, 0x0102030405060708UL, entity, 0.0F));
        var clientAnimatedIntent = eventIntent with
        {
            Intent = eventIntent.Intent with
            {
                ClientSequence = 2,
                Operation = WorldAnimatedAdapterCodec.AnimatedInputEdgeOperation,
                Payload = animatedPayload
            }
        };
        AssertEx.False(authority.TryAcceptIntent(
            clientAnimatedIntent, clientPeer, out _, out var animatedAuthorityError));
        AssertEx.Equal("HOST_AUTHORITY_REQUIRED_FOR_ANIMATED_WORLD", animatedAuthorityError);

        var typedPayload = WorldCathodeBusCodec.EncodeTypedMessage(
            new WorldCathodeTypedMessagePayload(
                0x100050U,
                0x1020304050607080UL,
                new WorldCathodeRuntimeKey(0x11112222U, 0x33334444U),
                new WorldCathodeRuntimeKey(0x55556666U, 0x77778888U),
                0.0,
                Array.Empty<byte>()));
        var clientTypedIntent = eventIntent with
        {
            Intent = eventIntent.Intent with
            {
                ClientSequence = 2,
                Operation = WorldCathodeBusCodec.TypedMessageOperation,
                Payload = typedPayload
            }
        };
        AssertEx.True(authority.TryAcceptIntent(
            clientTypedIntent, clientPeer, out var clientTypedMutation, out var typedAuthorityError),
            typedAuthorityError);
        AssertEx.Equal(clientPeer, clientTypedMutation.Mutation.SourcePeer);
        AssertEx.Equal(WorldCathodeBusCodec.TypedMessageOperation,
            clientTypedMutation.Mutation.Operation);

        var duplicateHostTypedPayload = WorldCathodeBusCodec.EncodeTypedMessage(
            WorldCathodeBusCodec.DecodeTypedMessage(typedPayload) with
            {
                ObservationId = 0x8877665544332211UL
            });
        var duplicateHostTypedIntent = clientTypedIntent with
        {
            Intent = clientTypedIntent.Intent with
            {
                SourcePeer = hostPeer,
                ClientSequence = 1,
                Payload = duplicateHostTypedPayload
            }
        };
        AssertEx.False(authority.TryAcceptIntent(
            duplicateHostTypedIntent, hostPeer, out _, out var typedDuplicateError));
        AssertEx.Equal("DUPLICATE_CATHODE_MESSAGE", typedDuplicateError);

        var stateIntent = eventIntent with
        {
            Intent = eventIntent.Intent with
            {
                ClientSequence = 3,
                RequestedKind = WorldMutationKind.State,
                Operation = 0x12345678U
            }
        };
        AssertEx.True(authority.TryAcceptIntent(
            stateIntent, clientPeer, out var state1, out var stateError), stateError);
        AssertEx.Equal(1UL, state1.Mutation.Revision);
        AssertEx.True((state1.Mutation.Flags & WorldMutationFlags.Replaceable) != 0);
        AssertEx.True((state1.Mutation.Flags & WorldMutationFlags.Replayable) != 0);
        var stateIntent2 = stateIntent with
        {
            Intent = stateIntent.Intent with { ClientSequence = 4 }
        };
        AssertEx.True(authority.TryAcceptIntent(
            stateIntent2, clientPeer, out var state2, out var stateError2), stateError2);
        AssertEx.Equal(2UL, state2.Mutation.Revision);

        var logicSwitchStateIntent = stateIntent2 with
        {
            Intent = stateIntent2.Intent with
            {
                ClientSequence = 5,
                Operation = WorldReplicationConstants.ObservedCathodeLogicSwitchStateOperation
            }
        };
        AssertEx.True(authority.TryAcceptIntent(
            logicSwitchStateIntent, clientPeer, out var switchState, out var switchStateError),
            switchStateError);
        AssertEx.Equal(WorldMutationKind.State, switchState.Mutation.Kind);
        AssertEx.True((switchState.Mutation.Flags & WorldMutationFlags.Replayable) != 0);
        AssertEx.False(authority.TryAcceptIntent(
            logicSwitchStateIntent with
            {
                Intent = logicSwitchStateIntent.Intent with
                {
                    ClientSequence = 6,
                    RequestedKind = WorldMutationKind.Event
                }
            },
            clientPeer, out _, out var switchKindError));
        AssertEx.Equal("CATHODE_LOGIC_SWITCH_REQUIRES_STATE", switchKindError);

        var replay = authority.ReplaySnapshot();
        AssertEx.Equal(2, replay.Count);
        AssertEx.Equal(state2.Mutation.Sequence, replay[0].Mutation.Sequence);
        AssertEx.Equal(switchState.Mutation.Sequence, replay[1].Mutation.Sequence);
    }

    private static async Task AssertWorldReplicationRelayAsync(
        LobbyHost host,
        LobbyClient client,
        LobbyStartPayload start)
    {
        var hostContextAtClient = new TaskCompletionSource<WorldNetworkContext>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var clientContextAtHost = new TaskCompletionSource<WorldNetworkContext>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var hostMutation = new TaskCompletionSource<WorldNetworkMutation>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var clientMutation = new TaskCompletionSource<WorldNetworkMutation>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var hostAck = new TaskCompletionSource<WorldNetworkMutationAck>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        client.WorldContextReceived += (_, value) => hostContextAtClient.TrySetResult(value);
        host.WorldContextReceived += (_, value) =>
        {
            if (value.PeerId == client.PeerId) clientContextAtHost.TrySetResult(value);
        };
        host.WorldMutationAccepted += (_, value) => hostMutation.TrySetResult(value.Mutation);
        client.WorldMutationReceived += (_, value) => clientMutation.TrySetResult(value);
        host.WorldMutationAckReceived += (_, value) => hostAck.TrySetResult(value);

        var manifest = new WorldManifestIdentity(
            WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion,
            new WorldEntityId(0xABCDEF0102030405UL, 0x1020304050607080UL));
        var hostContext = new WorldNetworkContext(
            host.SessionId, host.HostPeerId, start.LevelGeneration, manifest,
            new string('C', 64), true);
        await host.SetHostWorldContextAsync(hostContext).ConfigureAwait(false);
        var receivedHostContext = await hostContextAtClient.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        AssertEx.Equal(host.HostPeerId, receivedHostContext.PeerId);

        var clientContext = hostContext with { PeerId = client.PeerId };
        await client.SendWorldContextAsync(clientContext).ConfigureAwait(false);
        AssertEx.Equal(client.PeerId, (await clientContextAtHost.Task
            .WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false)).PeerId);
        await host.WaitForWorldCompatibilityAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);

        var intent = new WorldNetworkIntent(
            host.SessionId,
            start.LevelGeneration,
            new WorldIntent(
                manifest,
                new WorldEntityId(0x1111222233334444UL, 0x5555666677778888UL),
                99,
                client.PeerId,
                1,
                WorldMutationKind.Event,
                WorldReplicationConstants.NetworkProbeOperation,
                Array.Empty<byte>()));
        await client.SendWorldIntentAsync(intent).ConfigureAwait(false);
        var accepted = await hostMutation.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        var received = await clientMutation.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        AssertEx.Equal(accepted.Mutation.Sequence, received.Mutation.Sequence);
        AssertEx.Equal(client.PeerId, received.Mutation.SourcePeer);
        AssertEx.Equal(start.LevelGeneration, received.WorldGeneration);
        AssertEx.True((received.Mutation.Flags & WorldMutationFlags.Reliable) != 0);

        var ack = new WorldNetworkMutationAck(
            host.SessionId, client.PeerId, start.LevelGeneration,
            new WorldMutationResult(received.Mutation.Sequence,
                WorldMutationResultCode.Accepted, true, true));
        await client.SendWorldMutationAckAsync(ack).ConfigureAwait(false);
        AssertEx.Equal(client.PeerId, (await hostAck.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false)).PeerId);
    }

    private static async Task AssertPlayerPositionRelayAsync(
        LobbyHost host,
        LobbyClient client,
        LobbyStartPayload start)
    {
        var hostSeen = NewPositionCompletion();
        var clientSeen = NewPositionCompletion();
        host.PlayerPositionReceived += (_, value) =>
        {
            if (value.PeerId == client.PeerId) hostSeen.TrySetResult(value);
        };
        client.PlayerPositionReceived += (_, value) =>
        {
            if (value.PeerId == host.HostPeerId) clientSeen.TrySetResult(value);
        };
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var fromClient = new PlayerPositionSnapshotPayload(host.SessionId, client.PeerId,
            start.LevelGeneration, 1, now, 4, 5, 6, -1.25F,
            PlayerMoveType.FastWalk, 2.0F);
        var fromHost = new PlayerPositionSnapshotPayload(host.SessionId, host.HostPeerId,
            start.LevelGeneration, 1, now, 1, 2, 3, 1.75F,
            PlayerMoveType.Run, 3.0F);
        await client.SendPlayerPositionAsync(fromClient).ConfigureAwait(false);
        await host.PublishHostPlayerPositionAsync(fromHost).ConfigureAwait(false);
        AssertEx.Equal(fromClient, await hostSeen.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false));
        AssertEx.Equal(fromHost, await clientSeen.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false));
    }

    private static TaskCompletionSource<PlayerPositionSnapshotPayload>
        NewPositionCompletion() => new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static async Task AssertSpawnRequestRelayAsync(
        LobbyHost host,
        LobbyClient client,
        LobbyStartPayload start)
    {
        var hostSeen = NewSpawnRequestCompletion();
        var clientSeen = NewSpawnRequestCompletion();
        var positionRequested = new TaskCompletionSource<SpawnPositionRequestPayload>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        host.SpawnRemotePlayersRequested += (_, value) => hostSeen.TrySetResult(value);
        client.SpawnRemotePlayersRequested += (_, value) => clientSeen.TrySetResult(value);
        client.SpawnPositionRequested += (_, value) => positionRequested.TrySetResult(value);
        var hostPosition = new PlayerSpawnPositionPayload(host.HostPeerId, 1, 2, 3);
        var operation = host.RequestRemotePlayerSpawnsAsync(start.LevelGeneration, hostPosition);
        var request = await positionRequested.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        await client.SendSpawnPositionAsync(request,
            new PlayerSpawnPositionPayload(client.PeerId, 4, 5, 6)).ConfigureAwait(false);
        var expected = await operation.ConfigureAwait(false);
        var local = await hostSeen.Task.WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);
        var remote = await clientSeen.Task.WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);
        AssertSpawnRequestEqual(expected, local);
        AssertSpawnRequestEqual(expected, remote);
        AssertEx.Equal(2, expected.Players.Count);
        AssertEx.Equal(4F, expected.Players.Single(value => value.PeerId == client.PeerId).X);
    }

    private static void AssertSpawnRequestEqual(
        SpawnRemotePlayersPayload expected,
        SpawnRemotePlayersPayload actual)
    {
        AssertEx.Equal(expected.SessionId, actual.SessionId);
        AssertEx.Equal(expected.RequestId, actual.RequestId);
        AssertEx.Equal(expected.HostPeerId, actual.HostPeerId);
        AssertEx.Equal(expected.LevelGeneration, actual.LevelGeneration);
        AssertEx.True(expected.Players.OrderBy(value => value.PeerId)
            .SequenceEqual(actual.Players.OrderBy(value => value.PeerId)));
    }

    private static TaskCompletionSource<SpawnRemotePlayersPayload>
        NewSpawnRequestCompletion() => new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static async Task AssertGameplayReadyRelayAsync(
        LobbyHost host,
        LobbyClient client,
        LobbyStartPayload start)
    {
        var hostReady = new TaskCompletionSource<GameplayReadyPayload>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var clientReady = new TaskCompletionSource<GameplayReadyPayload>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        host.GameplayReadyReceived += (_, value) =>
        {
            if (value.PeerId == client.PeerId) hostReady.TrySetResult(value);
        };
        client.GameplayReadyReceived += (_, value) =>
        {
            if (value.PeerId == host.HostPeerId) clientReady.TrySetResult(value);
        };
        await client.SendGameplayReadyAsync(start.LevelGeneration).ConfigureAwait(false);
        await host.SetHostGameplayReadyAsync(start.LevelGeneration).ConfigureAwait(false);
        var fromClient = await hostReady.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        var fromHost = await clientReady.Task.WaitAsync(TimeSpan.FromSeconds(3))
            .ConfigureAwait(false);
        AssertEx.Equal(client.PeerId, fromClient.PeerId);
        AssertEx.Equal(host.HostPeerId, fromHost.PeerId);
    }

    private static void AssertHeader(byte[] encoded, int payloadLength)
    {
        AssertEx.Equal(ProtocolConstants.HeaderSize + payloadLength, encoded.Length);
        AssertEx.Equal("AIMP", Encoding.ASCII.GetString(encoded, 0, 4));
        AssertEx.Equal(ProtocolConstants.Version,
            BinaryPrimitives.ReadUInt16LittleEndian(encoded.AsSpan(4, 2)));
        AssertEx.Equal((ushort)MessageType.OutboundNetZFrame,
            BinaryPrimitives.ReadUInt16LittleEndian(encoded.AsSpan(6, 2)));
        AssertEx.Equal((uint)payloadLength,
            BinaryPrimitives.ReadUInt32LittleEndian(encoded.AsSpan(10, 4)));
        AssertEx.Equal(0x1122334455667788UL,
            BinaryPrimitives.ReadUInt64LittleEndian(encoded.AsSpan(14, 8)));
    }

    private static async Task AssertStreamRoundTripAsync(AimpFrame frame, byte[] payload)
    {
        await using var stream = new MemoryStream();
        await FrameCodec.WriteAsync(stream, frame).ConfigureAwait(false);
        stream.Position = 0;
        var decoded = await FrameCodec.ReadAsync(stream).ConfigureAwait(false);
        AssertEx.Equal(frame.Type, decoded.Type);
        AssertEx.Equal(frame.Sequence, decoded.Sequence);
        AssertEx.True(decoded.Payload.Span.SequenceEqual(payload));
    }

    private static void AssertInvalidHeaders(byte[] encoded)
    {
        var badMagic = encoded.ToArray();
        badMagic[0] ^= 0xFF;
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(badMagic));

        var unknownType = encoded.ToArray();
        BinaryPrimitives.WriteUInt16LittleEndian(unknownType.AsSpan(6, 2), ushort.MaxValue);
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(unknownType));

        var unknownFlags = encoded.ToArray();
        BinaryPrimitives.WriteUInt16LittleEndian(unknownFlags.AsSpan(8, 2), 1);
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(unknownFlags));
    }

    private static CaptureStatePayload CreateCapture(Guid identity) => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        Guid.NewGuid(),
        4,
        identity,
        true,
        true,
        true,
        2,
        true,
        true,
        true,
        8,
        "Engine callback observed",
        37,
        2,
        37,
        "MainThreadCharacterProcessing",
        100,
        200,
        1,
        true,
        false,
        true,
        true,
        RemoteCharacterStateNames.Ready,
        73,
        0x2C37BBB0,
        0x1C374418,
        true);

    private static void AssertWorldManifestV5RoundTrip(WorldEntityId entityId)
    {
        var ownerCompositeId = new WorldEntityId(
            entityId.High ^ 0x1010UL, entityId.Low ^ 0x2020UL);
        var interfaceVariableId = new WorldEntityId(
            entityId.High ^ 0x3030UL, entityId.Low ^ 0x4040UL);
        var manifest = new WorldManifest(
            WorldManifestConstants.FormatVersion,
            WorldManifestConstants.CompilerVersion,
            0x0102030405060708UL,
            0x1112131415161718UL,
            0x100U,
            0x200U,
            0x300U,
            new WorldManifestSourceInfo(
                "AA", "BB", true, true, true, "CC", "CC"),
            Array.Empty<WorldCompositeDescriptor>(),
            new[]
            {
                new WorldInstancePathDescriptor(
                    1U, new uint[] { 0x10U, 0x20U },
                    0x0123456789ABCDEFUL, 0xAABBCCDDU)
            },
            new[]
            {
                new WorldEntityDescriptor(
                    entityId,
                    WorldDomain.World,
                    WorldEntityKind.Function,
                    WorldEntityFlags.EnvironmentPrimitive | WorldEntityFlags.RuntimeAddressable,
                    0x101U,
                    0x202U,
                    1U,
                    0x0123456789ABCDEFUL,
                    0x202U,
                    0xAABBCCDDU,
                    0U,
                    0x303U,
                    "LOGIC_SWITCH",
                    "LOGIC_SWITCH@00000202"),
                new WorldEntityDescriptor(
                    ownerCompositeId,
                    WorldDomain.World,
                    WorldEntityKind.CompositeInstance,
                    WorldEntityFlags.CompositeInstance | WorldEntityFlags.RuntimeAddressable,
                    0x707U,
                    0x808U,
                    1U,
                    0x0123456789ABCDEFUL,
                    0x808U,
                    0xAABBCCDDU,
                    0U,
                    0x909U,
                    "NESTED_COMPOSITE",
                    "NESTED_COMPOSITE@00000808"),
                new WorldEntityDescriptor(
                    interfaceVariableId,
                    WorldDomain.World,
                    WorldEntityKind.Variable,
                    WorldEntityFlags.HasOutgoingLinks | WorldEntityFlags.RuntimeAddressable,
                    0x909U,
                    0xA0A0U,
                    1U,
                    0x0123456789ABCDEFUL,
                    0xA0A0U,
                    0xAABBCCDDU,
                    0U,
                    0U,
                    "Variable:NONE",
                    "B0B0B0B0"),
                new WorldEntityDescriptor(
                    new WorldEntityId(entityId.High ^ 0x11UL, entityId.Low ^ 0x22UL),
                    WorldDomain.UI,
                    WorldEntityKind.Function,
                    WorldEntityFlags.None,
                    0x404U,
                    0x505U,
                    1U,
                    0x0123456789ABCDEFUL,
                    0x505U,
                    0U,
                    0U,
                    0x606U,
                    "STATIC_ONLY",
                    "STATIC_ONLY@00000505")
            },
            new[]
            {
                new WorldCompositePublicInputDescriptor(
                    ownerCompositeId,
                    interfaceVariableId,
                    0xB0B0B0B0U,
                    0xC0C0C0C0U,
                    entityId,
                    0xD2B3CC2BU,
                    0U,
                    true,
                    true)
            },
            new[]
            {
                new WorldCompositePublicInputCanonicalAnchor(
                    ownerCompositeId,
                    0xB0B0B0B0U,
                    entityId,
                    0xD2B3CC2BU)
            },
            Array.Empty<WorldCompositePublicInputCausalProfile>(),
            Array.Empty<WorldConnectionDescriptor>(),
            Array.Empty<WorldOverrideDescriptor>(),
            Array.Empty<WorldReferenceDescriptor>(),
            Array.Empty<WorldZoneDescriptor>(),
            0U,
            0U,
            Array.Empty<string>());

        var path = Path.Combine(
            Path.GetTempPath(),
            $"aimp-world-manifest-{Guid.NewGuid():N}.aimpwm");
        try
        {
            var compatibilitySha =
                WorldReplicationCodec.ComputeManifestCompatibilitySha256(manifest);
            var diagnosticVariant = manifest with
            {
                Source = manifest.Source with
                {
                    StructuralFingerprintPak = "diagnostic-pak-variant",
                    StructuralFingerprintBin = "diagnostic-bin-variant"
                },
                Composites = manifest.Composites
                    .Select(static value => value with { Name = $"diag:{value.Name}" })
                    .Reverse()
                    .ToArray(),
                Entities = manifest.Entities
                    .Select(static value => value with
                    {
                        PrimitiveName = $"diag:{value.PrimitiveName}",
                        EntityName = $"diag:{value.EntityName}"
                    })
                    .Reverse()
                    .ToArray(),
                CompositePublicInputs = manifest.CompositePublicInputs.Reverse().ToArray(),
                CompositePublicInputCanonicalAnchors =
                    manifest.CompositePublicInputCanonicalAnchors.Reverse().ToArray(),
                Warnings = new[] { "machine-local diagnostic warning" }
            };
            AssertEx.Equal(
                compatibilitySha,
                WorldReplicationCodec.ComputeManifestCompatibilitySha256(diagnosticVariant));

            var runtimeVariantInputs = manifest.CompositePublicInputs.ToArray();
            runtimeVariantInputs[0] = runtimeVariantInputs[0] with
            {
                PublicInputShortGuid = runtimeVariantInputs[0].PublicInputShortGuid ^ 1U
            };
            var runtimeVariant = manifest with
            {
                CompositePublicInputs = runtimeVariantInputs
            };
            AssertEx.False(string.Equals(
                compatibilitySha,
                WorldReplicationCodec.ComputeManifestCompatibilitySha256(runtimeVariant),
                StringComparison.Ordinal));

            WorldManifestBinaryCodec.Write(path, manifest);
            var decoded = WorldManifestBinaryCodec.Read(path);
            AssertEx.Equal(WorldManifestConstants.FormatVersion, decoded.FormatVersion);
            AssertEx.Equal(WorldManifestConstants.CompilerVersion, decoded.CompilerVersion);
            AssertEx.Equal(manifest.MapContentId, decoded.MapContentId);
            AssertEx.Equal(4, decoded.Entities.Count);
            AssertEx.Equal(1, decoded.CompositePublicInputs.Count);
            AssertEx.Equal(ownerCompositeId, decoded.CompositePublicInputs[0].OwnerCompositeInstance);
            AssertEx.Equal(0xB0B0B0B0U, decoded.CompositePublicInputs[0].PublicInputShortGuid);
            AssertEx.True(decoded.CompositePublicInputs[0].NoTypedData);
            AssertEx.True(decoded.CompositePublicInputs[0].InternalFallbackExclusive);
            AssertEx.Equal(1, decoded.CompositePublicInputCanonicalAnchors.Count);
            AssertEx.Equal(ownerCompositeId, decoded.CompositePublicInputCanonicalAnchors[0].OwnerCompositeInstance);
            AssertEx.Equal(entityId, decoded.CompositePublicInputCanonicalAnchors[0].InternalTargetEntity);
            AssertEx.Equal(0xD2B3CC2BU, decoded.CompositePublicInputCanonicalAnchors[0].InternalTargetInputShortGuid);
            AssertEx.Equal(0x202U, decoded.Entities[0].CathodeEntityA);
            AssertEx.Equal(0xAABBCCDDU, decoded.Entities[0].CathodeEntityB);
            AssertEx.Equal(0xAABBCCDDU, decoded.InstancePaths[0].CathodeHierarchyId);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    private static void AssertGameCommand(Guid identity)
    {
        var command = new GameCommandExecutePayload(
            ProtocolConstants.ExperimentalSchemaVersion,
            Guid.NewGuid(),
            4,
            identity,
            GameCommandNames.PopupMessage,
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["headerText"] = "AIMP",
                ["mainText"] = "Engine thread ready",
                ["duration"] = "5.0",
                ["icon"] = PopupMessageIcons.Alert,
                ["soundEvent"] = PopupMessageSoundEvents.ObjectiveNew
            });
        ModelValidation.Validate(command);
        var decoded = JsonPayload.Deserialize<GameCommandExecutePayload>(
            JsonPayload.Serialize(command));
        AssertEx.Equal(command.Command, decoded.Command);
        AssertEx.SequenceEqual(command.Arguments, decoded.Arguments);
        AssertEx.Throws<ProtocolException>(() =>
            ModelValidation.Validate(command with { Command = "Unknown" }));

        var registryProbe = command with
        {
            Command = GameCommandNames.WorldRegistryProbe,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["manifestPath"] = @"C:\AIMP\WorldManifestCache\example.aimpwm"
            }
        };
        ModelValidation.Validate(registryProbe);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            registryProbe with
            {
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["manifestPath"] = "bad\npath"
                }
            }));

        var manifestLoad = command with
        {
            Command = GameCommandNames.WorldManifestLoad,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["manifestPath"] = @"C:\AIMP\WorldManifestCache\example.aimpwm"
            }
        };
        ModelValidation.Validate(manifestLoad);

        var worldNetProbe = command with
        {
            Command = GameCommandNames.WorldNetProbe,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["entityId"] = "112233445566778899AABBCCDDEEFF00"
            }
        };
        ModelValidation.Validate(worldNetProbe);

        var worldInteractionStats = command with
        {
            Command = GameCommandNames.WorldInteractionStats,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
        };
        ModelValidation.Validate(worldInteractionStats);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            worldInteractionStats with
            {
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["unexpected"] = "1"
                }
            }));

        var worldCompositeInputStats = command with
        {
            Command = GameCommandNames.WorldCompositeInputStats,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
        };
        ModelValidation.Validate(worldCompositeInputStats);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            worldCompositeInputStats with
            {
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["unexpected"] = "1"
                }
            }));

        foreach (var stateDiscoveryCommandName in new[]
                 {
                     GameCommandNames.WorldStateDiscoveryBaseline,
                     GameCommandNames.WorldStateDiscoveryAfter,
                     GameCommandNames.WorldStateDiscoveryStatus,
                     GameCommandNames.ReturnToFrontend
                 })
        {
            var stateDiscoveryCommand = command with
            {
                Command = stateDiscoveryCommandName,
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            };
            ModelValidation.Validate(stateDiscoveryCommand);
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
                stateDiscoveryCommand with
                {
                    Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                    {
                        ["unexpected"] = "1"
                    }
                }));
        }

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            worldNetProbe with
            {
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["entityId"] = "00000000000000000000000000000000"
                }
            }));

        var itemSyncApply = command with
        {
            Command = GameCommandNames.ItemSyncApply,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["entityId"] = "4954000A0014FFE212345678000000AB"
            }
        };
        AssertEx.True(GameCommandNames.All.Contains(GameCommandNames.ItemSyncApply));
        ModelValidation.Validate(itemSyncApply);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            itemSyncApply with
            {
                Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["entityId"] = "00000000000000000000000000000000"
                }
            }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            itemSyncApply with { Arguments = new Dictionary<string, string>(StringComparer.Ordinal) }));

        var itemSyncSpawn = command with
        {
            Command = GameCommandNames.ItemSyncSpawn,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["kind"] = "owner",
                ["ownerKey"] = "12345678000000AB",
                ["spawn"] = "1",
                ["path"] = "Pickups\\Medkit",
                ["quantity"] = "2",
                ["transform"] = "1,0,0,12.5,0,1,0,-3,0,0,1,0.25"
            }
        };
        AssertEx.True(GameCommandNames.All.Contains(GameCommandNames.ItemSyncSpawn));
        ModelValidation.Validate(itemSyncSpawn);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            itemSyncSpawn with
            {
                Arguments = new Dictionary<string, string>(itemSyncSpawn.Arguments, StringComparer.Ordinal)
                {
                    ["transform"] = "1,0,0,12.5,0,1,0,-3,0,0,1"
                }
            }));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            itemSyncSpawn with
            {
                Arguments = new Dictionary<string, string>(itemSyncSpawn.Arguments, StringComparer.Ordinal)
                {
                    ["spawn"] = "yes"
                }
            }));

        var netzCommand = command with
        {
            Command = GameCommandNames.SpawnNetZCharacter,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["x"] = "-47.270",
                ["y"] = "19.258",
                ["z"] = "-17.890",
                ["participantKey"] = "1",
                ["displayName"] = "RemotePlayer"
            }
        };
        ModelValidation.Validate(netzCommand);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            netzCommand with
            {
                Arguments = new Dictionary<string, string>(netzCommand.Arguments,
                    StringComparer.Ordinal) { ["participantKey"] = "0" }
            }));


        var directMoveCommand = command with
        {
            Command = GameCommandNames.CharacterDirectMove,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["characterAddress"] = "0x2BD73220",
                ["directionX"] = "0.466112",
                ["directionZ"] = "1.052107",
                ["intensity"] = "1.000",
                ["frameCount"] = "180"
            }
        };
        ModelValidation.Validate(directMoveCommand);
        var worldDirectMoveCommand = directMoveCommand with
        {
            Arguments = new Dictionary<string, string>(
                directMoveCommand.Arguments,
                StringComparer.Ordinal)
            {
                ["directionSpace"] = "world"
            }
        };
        ModelValidation.Validate(worldDirectMoveCommand);
        var replicationMoveCommand = worldDirectMoveCommand with
        {
            Arguments = new Dictionary<string, string>(
                worldDirectMoveCommand.Arguments,
                StringComparer.Ordinal)
            {
                ["replicationPositionX"] = "12.500",
                ["replicationPositionY"] = "19.258",
                ["replicationPositionZ"] = "-8.250",
                ["replicationYaw"] = "1.250",
                ["moveType"] = "1"
            }
        };
        ModelValidation.Validate(replicationMoveCommand);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            replicationMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    replicationMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["directionSpace"] = "local"
                }
            }));
        var incompleteReplicationPosition = new Dictionary<string, string>(
            worldDirectMoveCommand.Arguments,
            StringComparer.Ordinal)
        {
            ["replicationPositionX"] = "12.500"
        };
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            worldDirectMoveCommand with { Arguments = incompleteReplicationPosition }));
        ModelValidation.Validate(
            replicationMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    replicationMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["directionX"] = "0.000",
                    ["directionZ"] = "0.000",
                    ["frameCount"] = "0"
                }
            });
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            worldDirectMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    worldDirectMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["directionSpace"] = "camera"
                }
            }));
        ModelValidation.Validate(directMoveCommand with
        {
            Arguments = new Dictionary<string, string>(
                directMoveCommand.Arguments,
                StringComparer.Ordinal)
            {
                ["frameCount"] = "480"
            }
        });
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            replicationMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    replicationMoveCommand.Arguments,
                    StringComparer.Ordinal) { ["replicationPositionY"] = "NaN" }
            }));

        var directStepCommand = command with
        {
            Command = GameCommandNames.CharacterDirectStep,
            Arguments = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["characterAddress"] = "0x2BD73220",
                ["deltaX"] = "0.000",
                ["deltaY"] = "0.000",
                ["deltaZ"] = "0.100",
                ["deltaYaw"] = "0.000",
                ["frameCount"] = "180"
            }
        };
        ModelValidation.Validate(directStepCommand);
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directStepCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directStepCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["deltaZ"] = "10.100"
                }
            }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directStepCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directStepCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["frameCount"] = "601"
                }
            }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["characterAddress"] = "0x00000000"
                }
            }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["intensity"] = "0.900"
                }
            }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["directionX"] = "0.000",
                    ["directionZ"] = "0.000"
                }
            }));

        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(
            directMoveCommand with
            {
                Arguments = new Dictionary<string, string>(
                    directMoveCommand.Arguments,
                    StringComparer.Ordinal)
                {
                    ["directionX"] = "2.000",
                    ["directionZ"] = "2.000"
                }
            }));

        ModelValidation.Validate(directMoveCommand with
        {
            Arguments = new Dictionary<string, string>(
                directMoveCommand.Arguments,
                StringComparer.Ordinal)
            {
                ["directionX"] = "0.000",
                ["directionZ"] = "0.000",
                ["frameCount"] = "0"
            }
        });
    }

    private static CompatibilityInfo CreateCompatibility() => new(
        ProtocolConstants.Version,
        "test-launcher",
        "test-dll",
        new string('A', 64),
        "test-profile",
        new string('B', 64));

    private static Task<LobbyClient> ConnectClientAsync(
        LobbyHost host,
        CompatibilityInfo compatibility) =>
        LobbyClient.ConnectAsync(new LobbyClientOptions(
            "127.0.0.1",
            host.BoundPort,
            Guid.NewGuid(),
            "Dallas",
            compatibility));

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTime.UtcNow < deadline)
        {
            if (condition())
            {
                return;
            }

            await Task.Delay(10).ConfigureAwait(false);
        }

        throw new TimeoutException("The lobby condition was not reached.");
    }
}
