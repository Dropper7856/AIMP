using AIMP.Protocol;
using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace AIMP.Launcher;

internal sealed record VoiceSessionContext(Guid Session, ulong LocalPeer, int Round,
    int NativeGeneration, int ProcessId, bool Active, bool CanSpeak, LobbyHost? Host, LobbyClient? Client);

// Three bounded queues (capture, transport and native render) share one consumer.
// No audio work, device calls or native IPC runs on the lobby read callback.
internal sealed class VoiceSyncSession : IDisposable
{
    private readonly NativeRuntimeController _controller;
    private readonly LauncherSettingsStore _settings;
    private readonly VoiceMicrophoneCapture _capture = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly Channel<(VoiceSessionContext Context, short[] Pcm)> _captured =
        Channel.CreateBounded<(VoiceSessionContext, short[])>(new BoundedChannelOptions(3)
        { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });
    private readonly Channel<(VoiceSessionContext Context, VoiceFramePayload Frame)> _received =
        Channel.CreateBounded<(VoiceSessionContext, VoiceFramePayload)>(new BoundedChannelOptions(12)
        { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });
    private readonly Task _worker;
    private readonly VoiceSyncStats _stats = new();
    private string _reportedState = string.Empty;
    private long _lastStatsAt;
    private VoiceSessionContext _context = new(Guid.Empty, 0, 0, 0, 0, false, false, null, null);
    private VoiceSettings _voice = new();
    private byte[]? _lastConfiguration;
    private long _speechUntil, _lastSettingsRead, _lastErrorAt, _retryCaptureAt;
    private uint _sequence;
    private int _disposed;

    internal VoiceSyncSession(NativeRuntimeController controller, LauncherSettingsStore settings)
    {
        _controller = controller;
        _settings = settings;
        _capture.FrameCaptured += OnCaptured;
        _capture.ErrorOccurred += OnCaptureError;
        _worker = Task.Run(RunAsync);
    }

    internal void Update(VoiceSessionContext context) => Volatile.Write(ref _context, context);
    internal void Receive(object? sender, VoiceFramePayload frame)
    {
        var context = Volatile.Read(ref _context);
        if (!context.Active || frame.SessionId != context.Session || frame.LevelGeneration != context.Round
            || frame.PeerId == context.LocalPeer || !ReferenceEquals(sender, context.Host ?? (object?)context.Client)) return;
        _stats.Add(VoiceStat.Received);
        _received.Writer.TryWrite((context, frame));
    }

    private void OnCaptured(object? sender, short[] pcm)
    {
        var context = Volatile.Read(ref _context);
        var voice = Volatile.Read(ref _voice);
        if (!context.Active || !context.CanSpeak || !voice.Enabled) return;
        var gate = !GameFocused(context.ProcessId) ? VoiceStat.Unfocused
            : voice.Mode == "pushToTalk" && (GetAsyncKeyState(voice.PushToTalkKey) & 0x8000) == 0
                ? VoiceStat.PushToTalkIdle : VoiceStat.Captured;
        _stats.Add(gate);
        if (gate == VoiceStat.Captured) _captured.Writer.TryWrite((context, pcm));
    }

    private async Task RunAsync()
    {
        try
        {
            using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(10));
            while (await timer.WaitForNextTickAsync(_stop.Token).ConfigureAwait(false))
            {
                await PumpSafelyAsync().ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) { }
        finally { _capture.Dispose(); }
    }

    private async Task PumpSafelyAsync()
    {
        try { await PumpAsync().ConfigureAwait(false); }
        catch (Exception error) when (!_stop.IsCancellationRequested)
        {
            Report(error.Message);
            _lastConfiguration = null;
            _capture.Stop();
            _retryCaptureAt = Environment.TickCount64 + 3000;
        }
    }

    private async Task PumpAsync()
    {
        var now = Environment.TickCount64;
        RefreshSettings(now);
        var context = Volatile.Read(ref _context);
        var voice = Volatile.Read(ref _voice);
        var enabled = context.Active && voice.Enabled;
        ReportState(context, voice, now);
        if (enabled && context.CanSpeak && GameFocused(context.ProcessId) && now >= _retryCaptureAt)
            _capture.Start(voice.MicrophoneId);
        else _capture.Stop();

        if (!_controller.Snapshot.IpcConnected) { _lastConfiguration = null; return; }
        if (!await ConfigureNativeAsync(context, enabled, voice.PlaybackVolume).ConfigureAwait(false)) return;
        await SendCapturedAsync(context, voice, enabled, now).ConfigureAwait(false);
        await RenderReceivedAsync(context, enabled).ConfigureAwait(false);
    }

    private void RefreshSettings(long now)
    {
        if (now - _lastSettingsRead < 500) return;
        _lastSettingsRead = now;
        // A concurrent settings save must never stop the microphone; keep the last values.
        try { Volatile.Write(ref _voice, (_settings.Load().Voice ?? new VoiceSettings()).Normalize()); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private void ReportState(VoiceSessionContext context, VoiceSettings voice, long now)
    {
        var role = context.Host is not null ? "Host" : context.Client is not null ? "Client" : "none";
        // Capture stops while the game window is not in the foreground (alt-tab, overlays).
        var state = $"active={context.Active} canSpeak={context.CanSpeak} focused={GameFocused(context.ProcessId)} "
            + $"enabled={voice.Enabled} mode={voice.Mode} "
            + $"key={voice.PushToTalkKey} threshold={voice.ActivationThreshold} microphone={voice.MicrophoneId} "
            + $"role={role} round={context.Round} native={context.NativeGeneration}";
        if (state != _reportedState) { _reportedState = state; _controller.Diagnostics.Log("VoiceSync", state); }
        if (!context.Active || now - _lastStatsAt < 5000) return;
        _lastStatsAt = now;
        if (_stats.TakeChanged() is { } stats) _controller.Diagnostics.Log("VoiceSync", $"Pipeline: {stats}");
    }

    private async Task<bool> ConfigureNativeAsync(VoiceSessionContext context, bool enabled, float volume)
    {
        var configuration = BuildConfiguration(context, enabled, volume);
        if (_lastConfiguration is null || !_lastConfiguration.AsSpan().SequenceEqual(configuration))
        {
            if (!await SendNativeAsync(MessageType.VoiceConfigure, configuration).ConfigureAwait(false)) return false;
            _lastConfiguration = configuration;
            _speechUntil = 0;
        }
        return true;
    }

    private async Task SendCapturedAsync(VoiceSessionContext context, VoiceSettings voice, bool enabled, long now)
    {
        // Cap each tick even when input arrives faster than IPC can be drained.
        for (var i = 0; i < 3 && _captured.Reader.TryRead(out var item); i++)
        {
            if (!enabled || !context.CanSpeak || !SameRound(item.Context, context)
                || !GameFocused(context.ProcessId)) continue;
            if (voice.Mode == "pushToTalk" && (GetAsyncKeyState(voice.PushToTalkKey) & 0x8000) == 0) continue;
            var pcm = item.Pcm;
            if (ApplyGain(pcm, voice.InputGain) >= voice.ActivationThreshold) _speechUntil = now + 250;
            if (voice.Mode == "voiceActivity" && now > _speechUntil) { _stats.Add(VoiceStat.BelowThreshold); continue; }
            _stats.Add(await SendLocalAsync(context, pcm).ConfigureAwait(false)
                ? VoiceStat.NetworkSent : VoiceStat.NetworkDropped);
        }
    }

    private async Task<bool> SendLocalAsync(VoiceSessionContext context, short[] pcm)
    {
        var frame = new VoiceFramePayload(context.Session, context.LocalPeer, context.Round,
            ++_sequence, VoiceCodec.Encode(pcm));
        if (context.Host is not null)
        {
            var sent = await context.Host.SendVoiceFrameAsync(frame, _stop.Token).ConfigureAwait(false);
            await SendPcmAsync(context, frame, pcm, true).ConfigureAwait(false);
            return sent;
        }
        return context.Client is not null
            && await context.Client.SendVoiceFrameAsync(frame, _stop.Token).ConfigureAwait(false);
    }

    private static double ApplyGain(short[] pcm, float gain)
    {
        double energy = 0;
        for (var i = 0; i < pcm.Length; i++)
        {
            pcm[i] = (short)Math.Clamp((int)(pcm[i] * gain), short.MinValue, short.MaxValue);
            var value = pcm[i] / 32768.0;
            energy += value * value;
        }
        return Math.Sqrt(energy / pcm.Length);
    }

    private async Task RenderReceivedAsync(VoiceSessionContext context, bool enabled)
    {
        for (var i = 0; i < 6 && _received.Reader.TryRead(out var item); i++)
        {
            if (!enabled || !SameRound(item.Context, context)) continue;
            _stats.Add(await SendPcmAsync(context, item.Frame, VoiceCodec.Decode(item.Frame.Audio), false)
                .ConfigureAwait(false) ? VoiceStat.ForwardedToGame : VoiceStat.GamePipeBusy);
        }
    }

    private Task<bool> SendPcmAsync(VoiceSessionContext context, VoiceFramePayload frame, short[] pcm, bool local)
    {
        var bytes = new byte[48 + VoiceCodec.SamplesPerFrame * 2];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 0x31465641); // AVF1
        WriteContext(bytes, context, local ? 1u : 0u, frame.PeerId);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(44), frame.Sequence);
        for (var i = 0; i < pcm.Length; i++)
            BinaryPrimitives.WriteInt16LittleEndian(bytes.AsSpan(48 + i * 2), pcm[i]);
        return SendNativeAsync(MessageType.VoicePcmFrame, bytes);
    }

    private Task<bool> SendNativeAsync(MessageType type, byte[] bytes) =>
        _controller.SendVoiceAsync(type, bytes, _stop.Token);

    private static byte[] BuildConfiguration(VoiceSessionContext context, bool enabled, float volume)
    {
        var bytes = new byte[48];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 0x31435641); // AVC1
        WriteContext(bytes, context, (enabled ? 1u : 0u) | (context.Host is not null ? 2u : 0u), context.LocalPeer);
        BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(40), volume);
        return bytes;
    }

    private static void WriteContext(byte[] bytes, VoiceSessionContext context, uint flags, ulong peer)
    {
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(4), context.NativeGeneration);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(8), context.Round);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), flags);
        context.Session.TryWriteBytes(bytes.AsSpan(16, 16));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), peer);
    }

    private static bool SameRound(VoiceSessionContext a, VoiceSessionContext b) =>
        a.Session == b.Session && a.Round == b.Round && a.NativeGeneration == b.NativeGeneration
        && a.ProcessId == b.ProcessId && a.LocalPeer == b.LocalPeer;
    private static bool GameFocused(int processId)
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out var foreground);
        return processId > 0 && foreground == (uint)processId;
    }
    private void OnCaptureError(object? sender, string error) => Report(error);
    private void Report(string error)
    {
        var now = Environment.TickCount64;
        if (now - Interlocked.Read(ref _lastErrorAt) < 5000) return;
        Interlocked.Exchange(ref _lastErrorAt, now);
        _controller.Diagnostics.Log("VoiceSync", error);
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _stop.Cancel();
        _capture.FrameCaptured -= OnCaptured;
        _capture.ErrorOccurred -= OnCaptureError;
        // The worker owns device teardown; never join a capture callback from itself.
        _ = _worker.ContinueWith(_ => _stop.Dispose(), TaskScheduler.Default);
    }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
}
