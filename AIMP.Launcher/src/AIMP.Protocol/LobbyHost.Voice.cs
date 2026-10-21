using System.Net.Sockets;

namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private readonly VoiceReceiveWindow _hostVoiceWindow = new();
    private readonly SemaphoreSlim _hostVoiceSendGate = new(1, 1);
    private long _voiceDrops;

    public event EventHandler<VoiceFramePayload>? VoiceFrameReceived;

    // Returns whether the Host frame was admitted for relay to the clients.
    public async Task<bool> SendVoiceFrameAsync(VoiceFramePayload frame, CancellationToken token = default)
    {
        var bytes = VoiceCodec.EncodeNetwork(frame);
        if (frame.SessionId != SessionId || frame.PeerId != HostPeerId)
            throw new InvalidOperationException("The host voice source identity is invalid.");
        if (!await _hostVoiceSendGate.WaitAsync(0, token).ConfigureAwait(false)) return false;
        try
        {
            lock (_stateLock)
            {
                if (!IsActiveVoiceSourceLocked(frame)
                    || !_hostVoiceWindow.TryAccept(frame.LevelGeneration, frame.Sequence, Environment.TickCount64))
                    return false;
            }
            await RelayVoiceAsync(frame, bytes, token).ConfigureAwait(false);
            return true;
        }
        finally { _hostVoiceSendGate.Release(); }
    }

    private async Task HandleVoiceFrameAsync(ClientState sender, ReadOnlyMemory<byte> bytes, CancellationToken token)
    {
        VoiceFramePayload frame;
        try { frame = VoiceCodec.DecodeNetwork(bytes.Span); }
        catch (ProtocolException exception) { VoiceDropped(exception.Message); return; }
        bool accepted;
        lock (_stateLock)
        {
            accepted = frame.PeerId == sender.PeerId && IsActiveVoiceSourceLocked(frame)
                && sender.VoiceWindow.TryAccept(frame.LevelGeneration, frame.Sequence, Environment.TickCount64);
        }
        if (!accepted) { VoiceDropped("source, round, rate or sequence rejected"); return; }
        // Audio consumer faults must not terminate the game transport.
        try { VoiceFrameReceived?.Invoke(this, frame); }
        catch (Exception exception) { VoiceDropped($"consumer failed: {exception.Message}"); }
        // Relay the original validated bytes: subscribers cannot mutate the wire frame.
        await RelayVoiceAsync(frame, bytes, token).ConfigureAwait(false);
    }

    // Caller holds _stateLock. "Starting" is the existing active-round state.
    private bool IsActiveVoiceSourceLocked(VoiceFramePayload frame) =>
        frame.SessionId == SessionId && frame.LevelGeneration == _levelGeneration
        && _state == "Starting" && !_deadPeers.Contains(frame.PeerId)
        && (frame.PeerId == HostPeerId || _clients.ContainsKey(frame.PeerId));

    private async Task RelayVoiceAsync(VoiceFramePayload frame, ReadOnlyMemory<byte> bytes, CancellationToken token)
    {
        foreach (var destination in _clients.Values)
        {
            if (destination.PeerId == frame.PeerId) continue;
            lock (_stateLock) { if (!IsActiveVoiceSourceLocked(frame)) return; }
            try
            {
                if (!await destination.Connection.TrySendVoiceAsync(bytes, token).ConfigureAwait(false))
                    VoiceDropped("client connection busy");
            }
            catch (Exception exception) when (exception is IOException or SocketException or ObjectDisposedException)
            {
                VoiceDropped($"send failed: {exception.Message}");
            }
        }
    }

    private void VoiceDropped(string reason)
    {
        var count = Interlocked.Increment(ref _voiceDrops);
        // Logarithmic diagnostics avoid a hostile source flooding the log.
        if ((count & (count - 1)) == 0) Log?.Invoke(this, $"Voice dropped ({count}): {reason}.");
    }
}
