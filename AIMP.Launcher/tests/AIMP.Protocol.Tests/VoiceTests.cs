using System.Net.Sockets;

namespace AIMP.Protocol.Tests;

internal static class VoiceTests
{
    internal static Task CodecAsync()
    {
        AssertEx.Equal(16000, VoiceCodec.SampleRate);
        AssertEx.Equal(320, VoiceCodec.SamplesPerFrame);
        var silence = new short[320];
        AssertEx.SequenceEqual(silence, VoiceCodec.Decode(VoiceCodec.Encode(silence)));
        var tone = Enumerable.Range(0, 320).Select(i => (short)(12000 * Math.Sin(i * 0.08))).ToArray();
        var audio = VoiceCodec.Encode(tone);
        var decoded = VoiceCodec.Decode(audio);
        AssertEx.Equal(tone[0], decoded[0]);
        AssertEx.True(tone.Zip(decoded, (a, b) => Math.Abs((int)a - b)).Average() < 1000,
            "ADPCM must retain a speech-band signal with reasonable error.");
        var extremes = Enumerable.Range(0, 320).Select(i => i % 2 == 0 ? short.MinValue : short.MaxValue).ToArray();
        AssertEx.Equal(320, VoiceCodec.Decode(VoiceCodec.Encode(extremes)).Length);
        // The previous packet cannot influence the next packet's predictor/index.
        AssertEx.SequenceEqual(audio, VoiceCodec.Encode(tone));
        var frame = new VoiceFramePayload(Guid.NewGuid(), 42, 3, 7, audio);
        var wire = VoiceCodec.EncodeNetwork(frame);
        AssertEx.True(wire.Length - 16 + ProtocolConstants.HeaderSize <= 512);
        var roundtrip = VoiceCodec.DecodeNetwork(wire);
        AssertEx.Equal(frame.SessionId, roundtrip.SessionId);
        AssertEx.Equal(frame.PeerId, roundtrip.PeerId);
        AssertEx.Equal(frame.LevelGeneration, roundtrip.LevelGeneration);
        AssertEx.Equal(frame.Sequence, roundtrip.Sequence);
        AssertEx.SequenceEqual(audio, roundtrip.Audio);
        for (var size = 0; size < audio.Length; size++)
            AssertEx.Throws<ProtocolException>(() => VoiceCodec.Decode(audio.AsSpan(0, size)));
        for (var size = 0; size < wire.Length; size++)
            AssertEx.Throws<ProtocolException>(() => VoiceCodec.DecodeNetwork(wire.AsSpan(0, size)));
        AssertEx.Throws<ProtocolException>(() => VoiceCodec.DecodeNetwork(new byte[513]));
        AssertEx.Throws<ArgumentException>(() => VoiceCodec.Encode(new short[319]));
        foreach (var offset in new[] { 2, 3, audio.Length - 1 })
        {
            var invalid = audio.ToArray(); invalid[offset] = 255;
            AssertEx.Throws<ProtocolException>(() => VoiceCodec.Decode(invalid));
        }
        foreach (var invalid in new[] { frame with { SessionId = Guid.Empty }, frame with { PeerId = 0 },
            frame with { LevelGeneration = 0 }, frame with { Sequence = 0 }, frame with { Audio = null! } })
            AssertEx.Throws<ProtocolException>(() => VoiceCodec.EncodeNetwork(invalid));
        // Independent decoder vector: predictor 1000, index 0, all code 0 => constant 1000.
        var vector = new byte[164]; vector[0] = 0xe8; vector[1] = 3;
        AssertEx.True(VoiceCodec.Decode(vector).All(sample => sample == 1000));
        vector[0] = vector[1] = 0; vector[4] = 7;
        var known = VoiceCodec.Decode(vector);
        AssertEx.Equal((short)11, known[1]); AssertEx.Equal((short)13, known[2]);
        return Task.CompletedTask;
    }

    internal static Task ReceiveWindowAsync()
    {
        // Inspect the internal guard with a deterministic clock, without sleeps
        // or a public production API just for testing transport policy.
        var type = typeof(VoiceCodec).Assembly.GetType("AIMP.Protocol.VoiceReceiveWindow", true)!;
        var window = Activator.CreateInstance(type)!;
        var method = type.GetMethod("TryAccept")!;
        bool Accept(int generation, uint sequence, long now) =>
            (bool)method.Invoke(window, new object[] { generation, sequence, now })!;
        AssertEx.False(Accept(0, 1, 1000));
        AssertEx.False(Accept(1, 0, 1000));
        for (uint i = 1; i <= 60; i++) AssertEx.True(Accept(1, i, 1000));
        AssertEx.False(Accept(1, 61, 1999), "Only 60 frames may enter a sliding second.");
        AssertEx.True(Accept(1, 61, 2000), "Dropped frames must not consume sequence space.");
        AssertEx.False(Accept(1, 61, 3000));
        AssertEx.False(Accept(1, 60, 3000));
        AssertEx.False(Accept(1, uint.MaxValue, 3000));
        AssertEx.True(Accept(1, 62, 3000));
        AssertEx.True(Accept(2, 1, 3000), "New rounds reset both the sequence and rate window.");
        AssertEx.False(Accept(1, 63, 3000), "A stale round cannot roll the window back.");
        AssertEx.True(Accept(2, 2, 3000));
        return Task.CompletedTask;
    }

    internal static async Task LobbyAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var token = timeout.Token;
        var compatibility = new CompatibilityInfo(ProtocolConstants.Version, "test", "test",
            new string('A', 64), "test", new string('B', 64));
        await using var host = new LobbyHost(new("Voice test", "Ripley", Guid.NewGuid(), 0, 3, "TestLevel", compatibility));
        host.Start();
        using var raw = new TcpClient();
        await raw.ConnectAsync("127.0.0.1", host.BoundPort, token);
        var stream = raw.GetStream();
        await SendAsync(stream, MessageType.LobbyJoin,
            JsonPayload.Serialize(new LobbyJoinPayload(Guid.NewGuid(), "Dallas", compatibility, 123)), token);
        var acceptedFrame = await FrameCodec.ReadAsync(stream, token);
        AssertEx.Equal(MessageType.LobbyJoinAccepted, acceptedFrame.Type);
        var accepted = JsonPayload.Deserialize<LobbyJoinAcceptedPayload>(acceptedFrame.Payload.Span);
        await FrameCodec.ReadAsync(stream, token); // initial snapshot
        await using var observer = await LobbyClient.ConnectAsync(new("127.0.0.1", host.BoundPort,
            Guid.NewGuid(), "Lambert", compatibility), token);
        observer.Start();
        await SendAsync(stream, MessageType.LobbyReady, JsonPayload.Serialize(new LobbyReadyPayload(true)), token);
        await observer.SetReadyAsync(true, token);
        await WaitAsync(() => host.CanStart, token);
        var atHost = 0; var atObserver = 0;
        host.VoiceFrameReceived += (_, _) => Interlocked.Increment(ref atHost);
        observer.VoiceFrameReceived += (_, _) => Interlocked.Increment(ref atObserver);
        var first = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await WaitAsync(() => observer.Snapshot?.LevelGeneration == first.LevelGeneration, token);
        var frame = new VoiceFramePayload(host.SessionId, accepted.PeerId, first.LevelGeneration, 100,
            VoiceCodec.Encode(new short[320]));
        await VoiceAsync(frame);
        await FlushAsync(first.LevelGeneration);
        AssertEx.Equal(1, atHost); AssertEx.Equal(1, atObserver);
        await VoiceAsync(frame); // replay
        await VoiceAsync(frame with { PeerId = host.HostPeerId, Sequence = 101 }); // forged owner
        await VoiceAsync(frame with { SessionId = Guid.NewGuid(), Sequence = 101 });
        await SendAsync(stream, MessageType.VoiceFrame, new byte[1], token);
        await VoiceAsync(frame with { Sequence = uint.MaxValue }); // bounded forward jump
        await VoiceAsync(frame with { Sequence = 101 }); // invalid frames did not poison sequence
        await FlushAsync(first.LevelGeneration);
        AssertEx.Equal(2, atHost); AssertEx.Equal(2, atObserver);
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => host.SendVoiceFrameAsync(frame, token));
        await AssertEx.ThrowsAsync<InvalidOperationException>(() => observer.SendVoiceFrameAsync(frame, token));
        await host.SendVoiceFrameAsync(frame with { PeerId = host.HostPeerId, Sequence = 1 }, token);
        await WaitAsync(() => atObserver == 3, token);
        // Dead senders cannot speak, even while other participants remain alive.
        await SendAsync(stream, MessageType.PlayerDeath,
            JsonPayload.Serialize(new PlayerDeathPayload(host.SessionId, accepted.PeerId, first.LevelGeneration)), token);
        await VoiceAsync(frame with { Sequence = 102 });
        await FlushAsync(first.LevelGeneration);
        AssertEx.Equal(2, atHost);
        await host.ReportHostPlayerDeathAsync(first.LevelGeneration, token);
        await observer.SendPlayerDeathAsync(first.LevelGeneration, token);
        await WaitAsync(() => observer.Snapshot?.State == "Lobby" && host.CanStart, token);
        var second = await host.StartGameAsync(DateTimeOffset.UtcNow, token);
        await WaitAsync(() => observer.Snapshot?.LevelGeneration == second.LevelGeneration, token);
        await VoiceAsync(frame with { Sequence = 102 }); // old generation
        await VoiceAsync(frame with { LevelGeneration = second.LevelGeneration, Sequence = 1 });
        await FlushAsync(second.LevelGeneration);
        AssertEx.Equal(3, atHost); AssertEx.Equal(4, atObserver);

        Task VoiceAsync(VoiceFramePayload value) => SendAsync(stream, MessageType.VoiceFrame, VoiceCodec.EncodeNetwork(value), token);

        async Task FlushAsync(int generation)
        {
            var seen = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            void Ready(object? sender, GameplayReadyPayload value)
            {
                if (value.PeerId == accepted.PeerId) seen.TrySetResult();
            }
            observer.GameplayReadyReceived += Ready;
            try
            {
                await SendAsync(stream, MessageType.GameplayReady,
                    JsonPayload.Serialize(new GameplayReadyPayload(host.SessionId, accepted.PeerId, generation)), token);
                // The original speaker must never receive its own relayed voice.
                while (true)
                {
                    var incoming = await FrameCodec.ReadAsync(stream, token);
                    if (incoming.Type == MessageType.VoiceFrame)
                        AssertEx.True(VoiceCodec.DecodeNetwork(incoming.Payload.Span).PeerId != accepted.PeerId);
                    if (incoming.Type == MessageType.GameplayReady) break;
                }
                await seen.Task.WaitAsync(token);
            }
            finally { observer.GameplayReadyReceived -= Ready; }
        }
    }

    private static Task SendAsync(NetworkStream stream, MessageType type, byte[] bytes, CancellationToken token) =>
        FrameCodec.WriteAsync(stream, new AimpFrame(type, 0, 1, bytes), token).AsTask();

    private static async Task WaitAsync(Func<bool> condition, CancellationToken token)
    {
        while (!condition()) await Task.Delay(10, token);
    }
}
