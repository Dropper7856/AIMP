namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private ulong _executionCarrierSerial;
    public event EventHandler<AISyncCausalState>? AISyncCausalStateReceived;
    private void HandleAISyncCausalState(ReadOnlyMemory<byte> bytes)
    {
        var value = AISyncCausalStateCodec.Decode(bytes.Span);
        lock (_executionGate)
        {
            if (!_executionReady || _executionFenced || _executionOffer?.Scope != value.Scope
                || _executionJournal is null || !ExecutionGenerationActive(value.Scope)) return;
            if (value.Serial <= _executionCarrierSerial) return;
            if (value.Through != _executionJournal.LastSequence)
            {
                FenceClientExecutionLocked(AISyncExecutionRecovery.InvalidStream);
                return;
            }
            _executionCarrierSerial = value.Serial;
        }
        try { AISyncCausalStateReceived?.Invoke(this, value); }
        catch (Exception) { RejectAISyncExecution(value.Scope, AISyncExecutionRecovery.NativeUnavailable); }
    }
}
