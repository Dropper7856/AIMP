using AIMP.Launcher;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.Cryptography;

namespace AIMP.Protocol.Tests;

internal static class IpcTests
{
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
        var states = 0; var impacts = 0; var closed = 0;
        session.Log += (_, message) => logs.Enqueue(message);
        session.AISyncStateProduced += (_, _) => Interlocked.Increment(ref states);
        session.AISyncImpactProduced += (_, _) => Interlocked.Increment(ref impacts);
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

        // The response is ordered after these frames, so it also serves as a
        // processing barrier without sleeps or polling the read-loop thread.
        await QueryCapabilitiesAsync(session, client, identity, serverFrames, timeout.Token).ConfigureAwait(false);
        await ExecuteQueuedCommandAsync(session, client, identity, serverFrames, timeout.Token).ConfigureAwait(false);
        AssertEx.True(session.IsReady);
        AssertEx.Equal(0, Volatile.Read(ref closed));
        AssertEx.Equal(1, Volatile.Read(ref states));
        AssertEx.Equal(1, Volatile.Read(ref impacts));
        AssertEx.Equal(2, logs.Count(message => message.StartsWith("Rejected AISYNC_STATE", StringComparison.Ordinal)));
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

    private static ValueTask WriteRawAsync(Stream stream, MessageType type,
        ReadOnlyMemory<byte> payload, ulong sequence, CancellationToken token) =>
        FrameCodec.WriteAsync(stream, new AimpFrame(type, 0, sequence, payload), token);

    private static async Task SendHandshakeAsync(
        Stream client,
        byte[] executableBytes,
        Guid identity,
        ICollection<MessageType> serverFrames,
        CancellationToken token)
    {
        var nonce = "0123456789ABCDEF0123456789ABCDEF";
        var hello = new HelloPayload(
            ProtocolConstants.Version,
            ProductInfo.ExpectedDllVersion,
            Environment.ProcessId,
            true,
            nonce);
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
