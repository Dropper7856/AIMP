using AIMP.Launcher;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Cryptography;

namespace AIMP.Protocol.Tests;

internal static partial class IpcTests
{
    internal static async Task CharacterIncarnationAsync()
    {
        using var temporary = new TemporaryDirectory();
        var executablePath = temporary.Combine("AI.exe"); var executableBytes = new byte[] {0x4D,0x5A,1,2,3};
        await File.WriteAllBytesAsync(executablePath,executableBytes);
        var identity = Guid.NewGuid(); var frames = new List<MessageType>();
        await using var session = new DllIpcSession(Environment.ProcessId);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token = timeout.Token;
        var states = new ConcurrentQueue<NativeCharacterIncarnationState>();
        session.CharacterIncarnationChanged += (_,value) => states.Enqueue(value);
        var execution = 0; session.AISyncExecutionProduced += (_,_) => Interlocked.Increment(ref execution);
        var handshake = session.AcceptAndHandshakeAsync(executablePath,identity,TimeSpan.FromSeconds(5),token);
        using var client = CreateClient(); await client.ConnectAsync(token);
        await SendHandshakeAsync(client,executableBytes,identity,frames,token); await handshake;
        var value = new NativeCharacterIncarnationState(7,9,0x100000,15);
        await WriteRawAsync(client,MessageType.CharacterIncarnationState,NativeCharacterIncarnationCodec.Encode(value),10,token);
        await AISyncExecutionTransportTests.Wait(() => states.Count == 1,token);
        AssertEx.Equal(value,states.Single());AssertEx.Equal(0,execution);
        var bad = NativeCharacterIncarnationCodec.Encode(value);bad[33]=1;
        await WriteRawAsync(client,MessageType.CharacterIncarnationState,bad,11,token);
        await AISyncExecutionTransportTests.Wait(() => states.Count == 2,token);
        AssertEx.Equal(new NativeCharacterIncarnationState(0,0,0,0),states.Last());
        AssertEx.True(session.IsReady,"Malformed optional lifetime data must close that identity, preserving IPC.");
        await WriteRawAsync(client,MessageType.CharacterIncarnationState,NativeCharacterIncarnationCodec.Encode(value with {Serial=10}),12,token);
        await AISyncExecutionTransportTests.Wait(() => states.Count == 3,token);
        AssertEx.Equal(10UL,states.Last().Serial);AssertEx.Equal(0,execution);
    }

    internal static async Task RouteDeliveryAsync()
    {
        using var temporary = new TemporaryDirectory();
        var executablePath = temporary.Combine("AI.exe"); var executableBytes = new byte[] { 0x4D, 0x5A, 1, 2, 3 };
        await File.WriteAllBytesAsync(executablePath, executableBytes);
        var identity = Guid.NewGuid(); var frames = new List<MessageType>();
        await using var session = new DllIpcSession(Environment.ProcessId);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15)); var token = timeout.Token;
        var packets = new ConcurrentQueue<(MessageType Type, byte[] Bytes)>();
        session.AISyncExecutionProduced += (_, p) => packets.Enqueue((p.Type, p.Bytes.ToArray()));
        var handshake = session.AcceptAndHandshakeAsync(executablePath, identity, TimeSpan.FromSeconds(5), token);
        using var client = CreateClient(); await client.ConnectAsync(token);
        await SendHandshakeAsync(client, executableBytes, identity, frames, token); await handshake;
        var bytes = AISyncRouteCodec.Encode(AISyncRouteTests.Fixture());
        await WriteRawAsync(client, MessageType.AISyncHostRoute, bytes, 10, token);
        await AISyncExecutionTransportTests.Wait(() => packets.Count == 1, token);
        AssertEx.Equal(MessageType.AISyncHostRoute, packets.Single().Type);
        AssertEx.SequenceEqual(bytes, packets.Single().Bytes);
        var send = session.SendAISyncExecutionAsync(MessageType.AISyncClientRoute, bytes, token);
        var delivered = await FrameCodec.ReadAsync(client, token); await send;
        AssertEx.Equal(MessageType.AISyncClientRoute, delivered.Type); AssertEx.SequenceEqual(bytes, delivered.Payload.ToArray());
        AssertEx.Equal(1, packets.Count, "Sending route IPC must not manufacture execution receipts.");
        AssertEx.Throws<ProtocolException>(() => session.SendAISyncExecutionAsync(MessageType.AISyncClientRoute, new byte[160], token));
        await session.DisposeAsync();
    }

    internal static async Task GenericHandshakeAndQueuedCommandAsync()
    {
        using var temporary = new TemporaryDirectory();
        var executablePath = temporary.Combine("AI.exe");
        var executableBytes = new byte[] { 0x4D, 0x5A, 0x01, 0x02, 0x03 };
        await File.WriteAllBytesAsync(executablePath, executableBytes).ConfigureAwait(false);

        var identity = Guid.NewGuid();
        var serverFrames = new List<MessageType>();
        await using var session = new DllIpcSession(Environment.ProcessId);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var handshake = session.AcceptAndHandshakeAsync(
            executablePath,
            identity,
            TimeSpan.FromSeconds(5),
            timeout.Token);

        using var client = CreateClient();
        await client.ConnectAsync(timeout.Token).ConfigureAwait(false);
        await SendHandshakeAsync(client, executableBytes, identity, serverFrames, timeout.Token)
            .ConfigureAwait(false);
        var handshakeResult = await handshake.ConfigureAwait(false);
        AssertGenericBuild(handshakeResult);

        await QueryCapabilitiesAsync(session, client, identity, serverFrames, timeout.Token)
            .ConfigureAwait(false);
        await ExecuteQueuedCommandAsync(session, client, identity, serverFrames, timeout.Token)
            .ConfigureAwait(false);

        AssertEx.False(serverFrames.Contains(MessageType.TestModeConfigure),
            "The minimal launcher must never send TestModeConfigure.");
        await session.DisposeAsync().ConfigureAwait(false);
    }

    private static NamedPipeClientStream CreateClient() => new(
        ".",
        ProtocolConstants.PipeName(Environment.ProcessId),
        PipeDirection.InOut,
        PipeOptions.Asynchronous);

    internal static async Task AISyncRejectionPreservesConnectionAsync()
    {
        using var temporary = new TemporaryDirectory();
        var executablePath = temporary.Combine("AI.exe");
        var executableBytes = new byte[] { 0x4D, 0x5A, 0x01, 0x02, 0x03 };
        await File.WriteAllBytesAsync(executablePath, executableBytes).ConfigureAwait(false);
        var identity = Guid.NewGuid();
        var serverFrames = new List<MessageType>();
        await using var session = new DllIpcSession(Environment.ProcessId);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var logs = new ConcurrentQueue<string>();
        var impacts = 0; var closed = 0; var observations = 0;
        var executionControls = 0; var executionReceipts = 0;
        session.AISyncExecutionProduced += (_, packet) => {
            if (packet.Type == MessageType.AISyncExecutionBinding) Interlocked.Increment(ref executionControls);
            else Interlocked.Increment(ref executionReceipts);
        };
        session.Log += (_, message) => logs.Enqueue(message);
        session.AISyncImpactProduced += (_, _) => Interlocked.Increment(ref impacts);
        session.AISyncTransactionObservationProduced += (_, _) => Interlocked.Increment(ref observations);
        session.ConnectionClosed += (_, _) => Interlocked.Increment(ref closed);
        var handshake = session.AcceptAndHandshakeAsync(
            executablePath, identity, TimeSpan.FromSeconds(5), timeout.Token);
        using var client = CreateClient();
        await client.ConnectAsync(timeout.Token).ConfigureAwait(false);
        await SendHandshakeAsync(client, executableBytes, identity, serverFrames, timeout.Token).ConfigureAwait(false);
        await handshake.ConfigureAwait(false);

        // Bad application payloads arrive in valid frames. None may reach an
        // AISync subscriber or prevent subsequent gameplay/diagnostic requests.
        await WriteRawAsync(client, MessageType.AISyncState, new byte[1], 10, timeout.Token).ConfigureAwait(false);
        var missingPose = AISyncCodecTests.ValidState(identity);
        BinaryPrimitives.WriteUInt32LittleEndian(missingPose.AsSpan(40), 2U);
        await WriteRawAsync(client, MessageType.AISyncState, missingPose, 11, timeout.Token).ConfigureAwait(false);
        await WriteRawAsync(client, MessageType.AISyncImpact,
            new byte[AISyncCodec.NativeImpactBytes], 12, timeout.Token).ConfigureAwait(false);
        await WriteRawAsync(client, MessageType.AISyncState,
            AISyncCodecTests.ValidState(identity), 13, timeout.Token).ConfigureAwait(false);
        var legacyImpact = ValidImpact(identity);
        BinaryPrimitives.WriteUInt32LittleEndian(legacyImpact, 1U);
        await WriteRawAsync(client, MessageType.AISyncImpact,
            legacyImpact, 14, timeout.Token).ConfigureAwait(false);
        await WriteRawAsync(client, MessageType.AISyncImpact,
            ValidImpact(identity), 15, timeout.Token).ConfigureAwait(false);
        await WriteRawAsync(client, MessageType.AISyncTransactionObservation,
            new byte[216], 16, timeout.Token).ConfigureAwait(false);
        await WriteRawAsync(client, MessageType.AISyncTransactionObservation,
            ValidShadowObservation(identity), 17, timeout.Token).ConfigureAwait(false);
        var scope = new AISyncExecutionScope(identity, 1, 1, 1);
        await WriteRawAsync(client, MessageType.AISyncExecutionBinding, new byte[64], 18, timeout.Token);
        await WriteRawAsync(client, MessageType.AISyncExecutionReceipt, new byte[232], 19, timeout.Token);
        var offer = new AISyncExecutionControl(scope, AISyncExecutionControlKind.Offer, 3);
        await WriteRawAsync(client, MessageType.AISyncExecutionBinding, AISyncExecutionControlCodec.Encode(offer), 20, timeout.Token);
        await WriteRawAsync(client, MessageType.AISyncExecutionBinding,
            AISyncExecutionControlCodec.Encode(offer with { Kind = AISyncExecutionControlKind.Ready }), 21, timeout.Token);
        var operation = AISyncExecutionTransportTests.Record(scope);
        await WriteRawAsync(client, MessageType.AISyncExecutionReceipt, AISyncExecutionCodec.EncodeReceipt(
            new(scope, 1, operation, AISyncExecutionStage.Received, AISyncExecutionResult.Ok)), 22, timeout.Token);
        foreach (var type in new[] { MessageType.AISyncExecutionBinding, MessageType.AISyncExecutionBatch, MessageType.AISyncExecutionRetry })
        {
            var bytes = type == MessageType.AISyncExecutionBinding ? AISyncExecutionControlCodec.Encode(offer)
                : AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { operation }));
            var send = session.SendAISyncExecutionAsync(type, bytes, timeout.Token);
            var frame = await ReadExpectedAsync(client, type, serverFrames, timeout.Token);
            await send; AssertEx.SequenceEqual(bytes, frame.Payload.ToArray());
        }

        // The response is ordered after these frames, so it also serves as a
        // processing barrier without sleeps or polling the read-loop thread.
        await QueryCapabilitiesAsync(session, client, identity, serverFrames, timeout.Token).ConfigureAwait(false);
        await ExecuteQueuedCommandAsync(session, client, identity, serverFrames, timeout.Token).ConfigureAwait(false);
        AssertEx.True(session.IsReady);
        AssertEx.Equal(0, Volatile.Read(ref closed));
        AssertEx.Equal(1, Volatile.Read(ref impacts));
        AssertEx.Equal(1, Volatile.Read(ref observations));
        AssertEx.Equal(1, Volatile.Read(ref executionControls));
        AssertEx.Equal(1, Volatile.Read(ref executionReceipts));
        AssertEx.Equal(3, logs.Count(message => message.StartsWith("Rejected AISync execution disposition", StringComparison.Ordinal)));
        AssertEx.True(session.ConnectionIdentity != Guid.Empty);
        AssertEx.Equal(1, logs.Count(message => message.StartsWith("Rejected optional AISync shadow", StringComparison.Ordinal)));
        AssertEx.Equal(3, logs.Count(message => message.StartsWith("Retired AISync state message", StringComparison.Ordinal)));
        AssertEx.Equal(2, logs.Count(message => message.StartsWith("Rejected AISYNC_IMPACT", StringComparison.Ordinal)));
    }

    private static byte[] ValidImpact(Guid identity)
    {
        var bytes = new byte[AISyncCodec.NativeImpactBytes];
        foreach (var (offset, value) in new (int, uint)[]
        {
            (0, AISyncCodec.ImpactVersion), (4, 1), (16, 7), (20, 1), (32, 11),
            (40, 2), (44, 1), (48, 25), (56, 1)
        }) BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8), AISyncCodec.SessionCookie(identity));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), 1234UL);
        return bytes;
    }

    private static byte[] ValidShadowObservation(Guid identity)
    {
        var bytes = new byte[216];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, AISyncShadowCodec.NativeMagic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), 216);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(16), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), 1);
        AISyncTransactionCodec.Encode(new AISyncTransaction {
            Session = identity, AuthorityEpoch = 1, WorldEpoch = 1, ConsumerEpoch = 1, OwnerEpoch = 1,
            EntityA = 1, Birth = 1, CharacterClass = 1, CommandId = 1, GlobalSequence = 1, OwnerSequence = 1,
            Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
            ParameterRevision = 1, Flags = AISyncTransactionFlags.Incomplete | AISyncTransactionFlags.ObservationOnly
        }, bytes.AsSpan(56));
        return bytes;
    }

    private static ValueTask WriteRawAsync(Stream stream, MessageType type,
        ReadOnlyMemory<byte> payload, ulong sequence, CancellationToken token) =>
        FrameCodec.WriteAsync(stream, new AimpFrame(type, 0, sequence, payload), token);

    private static async Task SendHandshakeAsync(
        Stream client,
        byte[] executableBytes,
        Guid identity,
        ICollection<MessageType> serverFrames,
        CancellationToken token, bool aiSyncAvailable = false)
    {
        var nonce = "0123456789ABCDEF0123456789ABCDEF";
        var hello = new HelloPayload(
            ProtocolConstants.Version,
            ProductInfo.ExpectedDllVersion,
            Environment.ProcessId,
            true,
            nonce,
            AiSyncV1Available: aiSyncAvailable,
            UiHeapSynchronized: true);
        await WriteAsync(client, MessageType.Hello, hello, 1, token).ConfigureAwait(false);
        var ackFrame = await ReadExpectedAsync(
            client, MessageType.HelloAck, serverFrames, token).ConfigureAwait(false);
        var ack = JsonPayload.Deserialize<HelloAckPayload>(ackFrame.Payload.Span);
        AssertEx.Equal(nonce, ack.Nonce);

        var build = new BuildInfoPayload(
            Convert.ToHexString(SHA256.HashData(executableBytes)),
            "generic-discovery",
            false);
        await WriteAsync(client, MessageType.BuildInfo, build, 2, token).ConfigureAwait(false);
        var configFrame = await ReadExpectedAsync(
            client, MessageType.SessionConfig, serverFrames, token).ConfigureAwait(false);
        var config = JsonPayload.Deserialize<SessionConfigPayload>(configFrame.Payload.Span);
        ModelValidation.Validate(config);
        AssertEx.Equal(identity, config.LauncherPlayerId);
        AssertEx.False(config.NativeReplicationEnabled);

        var ready = new NetZReadyPayload(
            "observation",
            false,
            new[] { "native_replication_disabled" });
        await WriteAsync(client, MessageType.NetZReady, ready, 3, token).ConfigureAwait(false);
    }

    private static void AssertGenericBuild(RuntimeHandshakeResult handshake)
    {
        AssertEx.Equal("generic-discovery", handshake.BuildInfo.ProfileId);
        AssertEx.False(handshake.BuildInfo.ProfileValidated);
        AssertEx.Equal("observation", handshake.Ready.Mode);
        AssertEx.False(handshake.Ready.NativeReplicationReady);
        AssertEx.Equal(ProductInfo.ExpectedDllVersion, handshake.NativeDllVersion);
    }

    private static async Task QueryCapabilitiesAsync(
        DllIpcSession session,
        Stream client,
        Guid identity,
        ICollection<MessageType> serverFrames,
        CancellationToken token)
    {
        var request = new CapabilityQueryPayload(
            ProtocolConstants.ExperimentalSchemaVersion,
            Guid.NewGuid(),
            0,
            identity);
        var queryTask = session.QueryCapabilitiesAsync(
            request,
            TimeSpan.FromSeconds(3),
            token);
        var frame = await ReadExpectedAsync(
            client, MessageType.CapabilityQuery, serverFrames, token).ConfigureAwait(false);
        var received = JsonPayload.Deserialize<CapabilityQueryPayload>(frame.Payload.Span);
        AssertEx.Equal(request, received);

        var state = new CapabilityStatePayload(
            ProtocolConstants.ExperimentalSchemaVersion,
            request.RequestId,
            request.LevelGeneration,
            identity,
            "generic-discovery",
            BuildResolutionModes.GenericDiscovery,
            false,
            false,
            new[]
            {
                new NativeCapabilityState(
                    NativeCapabilityNames.ScheduleGameThreadWork,
                    CapabilityStateNames.Observed,
                    "Engine hook installed; waiting for a callback.")
            },
            new[] { "engine_thread_waiting" },
            "Wait for the first engine callback.");
        await WriteAsync(client, MessageType.CapabilityState, state, 4, token)
            .ConfigureAwait(false);
        var response = await queryTask.ConfigureAwait(false);
        AssertEx.Equal(request.RequestId, response.RequestId);
        AssertEx.Equal(CapabilityStateNames.Observed, response.Capabilities.Single().State);
    }

    private static async Task ExecuteQueuedCommandAsync(
        DllIpcSession session,
        Stream client,
        Guid identity,
        ICollection<MessageType> serverFrames,
        CancellationToken token)
    {
        var request = CreatePopupCommand(identity);
        var commandTask = session.ExecuteGameCommandAsync(
            request,
            TimeSpan.FromSeconds(3),
            token);
        var frame = await ReadExpectedAsync(
            client, MessageType.GameCommandExecute, serverFrames, token).ConfigureAwait(false);
        var received = JsonPayload.Deserialize<GameCommandExecutePayload>(frame.Payload.Span);
        AssertEx.Equal(request.RequestId, received.RequestId);
        AssertEx.Equal(GameCommandNames.PopupMessage, received.Command);

        var queued = new GameCommandResultPayload(
            ProtocolConstants.ExperimentalSchemaVersion,
            request.RequestId,
            request.LevelGeneration,
            identity,
            request.Command,
            "QUEUED_WAITING_FOR_ENGINE_THREAD",
            "Command queued until an engine callback is observed.",
            false);
        await WriteAsync(client, MessageType.GameCommandResult, queued, 5, token)
            .ConfigureAwait(false);
        var response = await commandTask.ConfigureAwait(false);
        AssertEx.True(NativeRuntimeController.IsQueuedResult(response));
        AssertEx.Equal("Command queued", NativeRuntimeController.CommandResultText(response));
        AssertEx.False(response.Executed);
    }

    private static GameCommandExecutePayload CreatePopupCommand(Guid identity) => new(
        ProtocolConstants.ExperimentalSchemaVersion,
        Guid.NewGuid(),
        0,
        identity,
        GameCommandNames.PopupMessage,
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["headerText"] = "AIMP",
            ["mainText"] = "Queued command",
            ["duration"] = "5.0",
            ["icon"] = PopupMessageIcons.Alert,
            ["soundEvent"] = PopupMessageSoundEvents.ObjectiveNew
        });

    private static async Task WriteAsync<T>(
        Stream stream,
        MessageType type,
        T payload,
        ulong sequence,
        CancellationToken token)
    {
        var frame = new AimpFrame(type, 0, sequence, JsonPayload.Serialize(payload));
        await FrameCodec.WriteAsync(stream, frame, token).ConfigureAwait(false);
    }

    private static async Task<AimpFrame> ReadExpectedAsync(
        Stream stream,
        MessageType expected,
        ICollection<MessageType> serverFrames,
        CancellationToken token)
    {
        for (var index = 0; index < 8; index++)
        {
            var frame = await FrameCodec.ReadAsync(stream, token).ConfigureAwait(false);
            serverFrames.Add(frame.Type);
            if (frame.Type == MessageType.TestModeConfigure)
            {
                throw new InvalidOperationException("Launcher sent obsolete TestModeConfigure.");
            }

            if (frame.Type == MessageType.Heartbeat)
            {
                continue;
            }

            AssertEx.Equal(expected, frame.Type);
            return frame;
        }

        throw new TimeoutException($"Expected launcher frame {expected}.");
    }
}
