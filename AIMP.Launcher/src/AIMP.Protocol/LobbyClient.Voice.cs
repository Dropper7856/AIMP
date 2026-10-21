using System.Net.Sockets;

namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private readonly Dictionary<ulong, VoiceReceiveWindow> _voiceWindows = new();
    private readonly SemaphoreSlim _voiceSendGate = new(1, 1);
    private readonly VoiceReceiveWindow _outboundVoiceWindow = new();
    private long _voiceDrops;

    public event EventHandler<VoiceFramePayload>? VoiceFrameReceived;

    public async Task SendVoiceFrameAsync(VoiceFramePayload frame, CancellationToken token = default)
    {
        var bytes = VoiceCodec.EncodeNetwork(frame);
        if (frame.SessionId != SessionId || frame.PeerId != PeerId)
            throw new InvalidOperationException("The client voice source identity is invalid.");
        if (!await _voiceSendGate.WaitAsync(0, token).ConfigureAwait(false)) return;
        try
        {
            var snapshot = Snapshot;
            if (snapshot?.State != "Starting" || snapshot.LevelGeneration != frame.LevelGeneration
                || !_outboundVoiceWindow.TryAccept(frame.LevelGeneration, frame.Sequence, Environment.TickCount64)) return;
            try { await _connection.TrySendVoiceAsync(bytes, token).ConfigureAwait(false); }
            catch (Exception exception) when (exception is IOException or SocketException or ObjectDisposedException)
            {
                VoiceDropped($"send failed: {exception.Message}");
            }
        }
        finally { _voiceSendGate.Release(); }
    }

    private void HandleVoiceFrame(ReadOnlyMemory<byte> bytes)
    {
        VoiceFramePayload frame;
        try { frame = VoiceCodec.DecodeNetwork(bytes.Span); }
        catch (ProtocolException exception) { VoiceDropped(exception.Message); return; }
        var snapshot = Snapshot;
        if (frame.SessionId != SessionId || snapshot?.State != "Starting"
            || frame.LevelGeneration != snapshot.LevelGeneration || frame.PeerId == PeerId
            || !snapshot.Players.Any(player => player.PeerId == frame.PeerId))
        {
            VoiceDropped("source or round rejected");
            return;
        }
        // Only the ordered read loop touches this dictionary; retain current peers only.
        foreach (var peer in _voiceWindows.Keys.Where(peer => !snapshot.Players.Any(p => p.PeerId == peer)).ToArray())
            _voiceWindows.Remove(peer);
        if (!_voiceWindows.TryGetValue(frame.PeerId, out var window))
            _voiceWindows.Add(frame.PeerId, window = new VoiceReceiveWindow());
        if (!window.TryAccept(frame.LevelGeneration, frame.Sequence, Environment.TickCount64))
        {
            VoiceDropped("rate or sequence rejected");
            return;
        }
        try { VoiceFrameReceived?.Invoke(this, frame); }
        catch (Exception exception) { VoiceDropped($"consumer failed: {exception.Message}"); }
    }

    private void VoiceDropped(string reason)
    {
        var count = Interlocked.Increment(ref _voiceDrops);
        if ((count & (count - 1)) == 0) Log?.Invoke(this, $"Voice dropped ({count}): {reason}.");
    }
}
