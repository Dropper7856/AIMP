using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class AISyncHostBridge
{
    private ulong _checkpointThrough;
    private long _checkpointStarted;
    private bool _checkpointSourceSent;
    private readonly HashSet<ulong> _checkpointPeers = new();
    private bool AcceptCheckpointAck(Work work, AISyncExecutionScope scope)
    {
        if (work.Control is not { Kind: AISyncExecutionControlKind.CheckpointAck } ack || ack.Scope != scope) return false;
        if (ack.Through != _checkpointThrough) throw new ProtocolException("Unexpected consumer checkpoint ACK.");
        _checkpointPeers.Add(work.Peer); return true;
    }

    private async Task CheckpointAsync()
    {
        if (_checkpointThrough == 0) BeginCheckpoint();
        if (_checkpointThrough == 0) return;
        if (Environment.TickCount64 - _checkpointStarted > 5000)
            throw new ProtocolException("Native settled-prefix checkpoint timed out.");
        if (_checkpointSourceSent || _checkpointPeers.Count != _peers.Count) return;
        _checkpointSourceSent = true;
        await SendAsync(_binding!, new(_scope!.Value, AISyncExecutionControlKind.Checkpoint, 0, _checkpointThrough)).ConfigureAwait(false);
    }
    private void BeginCheckpoint()
    {
        if (_journal!.RecordCount < 128) return;
        var through = _journal.LastSequence;
        foreach (var peer in _peers)
            through = Math.Min(through, _binding!.Host!.AISyncExecutionSettledThrough(peer.Key, peer.Value));
        if (through <= _journal.HistoryFloor) return;
        _checkpointThrough = through; _checkpointStarted = Environment.TickCount64;
        foreach (var peer in _peers)
            if (!_binding!.Host!.CheckpointAISyncExecution(peer.Key, peer.Value, through))
                throw new ProtocolException("Consumer checkpoint lost its settled native prefix.");
    }
    private void CompleteCheckpoint(AISyncExecutionControl ack)
    {
        if (!_checkpointSourceSent || ack.Through != _checkpointThrough
            || !_journal!.Checkpoint(ack.Through, externalSettlement: true))
            throw new ProtocolException("Host source checkpoint mismatched the acknowledged consumer prefix.");
        _routes!.Checkpoint(ack.Through); ResetCheckpoint();
    }
    private void ResetCheckpoint()
    { _checkpointThrough = 0; _checkpointStarted = 0; _checkpointSourceSent = false; _checkpointPeers.Clear(); }
}
