namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    // Route and preceding AIEX share one ordered, bounded peer sender. A Client
    // has no publisher API, and cannot negotiate itself into this direction.
    public AISyncExecutionJournalResult PublishAISyncRoute(ulong peerId, ReadOnlySpan<byte> bytes)
    {
        lock (_stateLock)
        {
            if (_disposed != 0 || _state != "Starting" || !_executionPeers.TryGetValue(peerId, out var peer)
                || !peer.Ready || peer.Fenced || peer.Offer.Scope.WorldEpoch != (ulong)_levelGeneration)
                return AISyncExecutionJournalResult.Rejected;
            var result = peer.Routes.Append(bytes);
            if (result == AISyncExecutionJournalResult.RecoveryRequired
                || (result == AISyncExecutionJournalResult.Accepted && !peer.Sender.Enqueue(MessageType.AISyncRouteNetwork, peer.Routes.Retained(AISyncRouteCodec.DecodeKey(bytes)))))
            {
                FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.InvalidStream);
                return AISyncExecutionJournalResult.RecoveryRequired;
            }
            return result;
        }
    }
}
