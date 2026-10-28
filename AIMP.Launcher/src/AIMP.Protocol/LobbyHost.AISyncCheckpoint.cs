namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    // A prefix is eligible only after final native receipts, for this consumer.
    public ulong AISyncExecutionSettledThrough(ulong peerId, AISyncExecutionScope scope)
    {
        lock (_stateLock) return _executionPeers.TryGetValue(peerId, out var peer)
            && peer.Offer.Scope == scope && peer.Ready && !peer.Fenced ? peer.Journal.SettledThrough() : 0;
    }
    public bool CheckpointAISyncExecution(ulong peerId, AISyncExecutionScope scope, ulong through)
    {
        lock (_stateLock)
        {
            if (_state != "Starting" || !_executionPeers.TryGetValue(peerId, out var peer)
                || peer.Offer.Scope != scope || !peer.Ready || peer.Fenced || through == 0
                || peer.CheckpointPending != 0 || through <= peer.Journal.HistoryFloor
                || through > peer.Journal.SettledThrough()) return false;
            peer.CheckpointPending = through;
            if (peer.Sender.Enqueue(MessageType.AISyncExecutionControl,
                AISyncExecutionControlCodec.Encode(new(scope, AISyncExecutionControlKind.Checkpoint, 0, through)))) return true;
            FenceExecutionPeerLocked(peer, AISyncExecutionRecovery.Capacity); return false;
        }
    }
    private static void AcknowledgeExecutionCheckpoint(ExecutionPeer peer, AISyncExecutionControl control)
    {
        if (control.Through == peer.Journal.HistoryFloor) return; // Retained watermark, no execution.
        if (control.Through != peer.CheckpointPending || !peer.Journal.Checkpoint(control.Through))
            throw new ProtocolException("Native execution checkpoint did not match its settled prefix.");
        peer.Routes.Checkpoint(control.Through); peer.CheckpointPending = 0;
    }
}
