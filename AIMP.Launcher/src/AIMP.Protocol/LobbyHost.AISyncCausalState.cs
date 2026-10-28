namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    public AISyncExecutionJournalResult PublishAISyncCausalState(ulong peerId, ReadOnlySpan<byte> bytes)
    {
        var value = AISyncCausalStateCodec.Decode(bytes);
        lock (_stateLock)
        {
            if (_disposed != 0 || _state != "Starting" || !_executionPeers.TryGetValue(peerId, out var peer)
                || !peer.Ready || peer.Fenced || value.Scope != peer.Offer.Scope
                || value.Scope.WorldEpoch != (ulong)_levelGeneration)
                return AISyncExecutionJournalResult.Rejected;
            if (value.Serial <= peer.LastCarrierSerial) return AISyncExecutionJournalResult.Duplicate;
            if (value.Through != peer.Journal.LastSequence
                || !peer.Sender.Enqueue(MessageType.AISyncCausalStateNetwork, bytes.ToArray()))
            {
                FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.InvalidStream);
                return AISyncExecutionJournalResult.RecoveryRequired;
            }
            peer.LastCarrierSerial = value.Serial;
            return AISyncExecutionJournalResult.Accepted;
        }
    }
}
