using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class AISyncHostBridge
{
    private void ProcessSourceState(NativeAISyncExecution packet)
    {
        var value = AISyncCausalStateCodec.Decode(packet.Bytes.Span);
        if (value.Scope != _scope) return;
        if (!_ready || _journal is null || value.Through != _journal.LastSequence)
            throw new ProtocolException("Host event lost its exact command prefix.");
        if (value.Serial <= _lastStateSerial) return;
        foreach (var peer in _peers)
        {
            var bytes = AISyncCausalStateCodec.Encode(value with { Scope = peer.Value });
            if (_binding!.Host!.PublishAISyncCausalState(peer.Key, bytes) != AISyncExecutionJournalResult.Accepted)
                throw new ProtocolException("Host event lost its ordered consumer sender.");
        }
        _lastStateSerial = value.Serial;
        var count=++_statePackets;
        if(count<=8||(count&63)==0)_log($"Host causal event forwarded: packet={count}, serial={value.Serial}, through={value.Through}, peers={_peers.Count}.");
    }
}
