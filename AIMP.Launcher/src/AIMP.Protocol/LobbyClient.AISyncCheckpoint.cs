namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private ulong _executionCheckpoint;
    private void ReceiveExecutionCheckpoint(AISyncExecutionControl control)
    {
        if (!_executionReady || _executionFenced || _executionJournal is null
            || (_executionCheckpoint != 0 && _executionCheckpoint != control.Through)
            || control.Through < _executionJournal.HistoryFloor
            || control.Through > _executionJournal.SettledThrough())
            throw new ProtocolException("Host requested an unsettled execution checkpoint.");
        _executionCheckpoint = control.Through;
    }
    // Called only with the current DLL's actual engine-thread checkpoint ACK.
    public bool AcknowledgeAISyncExecutionCheckpoint(AISyncExecutionControl control)
    {
        lock (_executionGate)
        {
            if (!_executionReady || _executionFenced || _executionOffer?.Scope != control.Scope
                || control.Kind != AISyncExecutionControlKind.CheckpointAck || _executionJournal is null
                || _executionSender is null || _executionCheckpoint != control.Through
                || !ExecutionGenerationActive(control.Scope) || !_executionJournal.Checkpoint(control.Through)) return false;
            _executionRoutes?.Checkpoint(control.Through); _executionCheckpoint = 0;
            if (_executionSender.Enqueue(MessageType.AISyncExecutionControl, AISyncExecutionControlCodec.Encode(control))) return true;
            FenceClientExecutionLocked(AISyncExecutionRecovery.Capacity); return false;
        }
    }
}
