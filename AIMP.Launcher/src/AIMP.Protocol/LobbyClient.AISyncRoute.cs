namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    // Received means value bytes passed the current command/scope checks. Native
    // application is reported solely by the native AIEX operation receipts.
    public event EventHandler<AISyncRouteGraph>? AISyncRouteReceived;
    private void HandleAISyncRoute(ReadOnlyMemory<byte> bytes)
    {
        AISyncRouteKey key;
        AISyncExecutionJournalResult result;
        byte[]? canonical=null;
        lock (_executionGate)
        {
            if (!_executionReady || _executionFenced || _executionOffer is null || _executionRoutes is null
                || !ExecutionGenerationActive(_executionOffer.Scope)) return;
            key = AISyncRouteCodec.DecodeKey(bytes.Span);
            if (key.Scope != _executionOffer.Scope) return;
            result = _executionRoutes.Append(bytes.Span);
            if(result==AISyncExecutionJournalResult.Accepted)canonical=_executionRoutes.Retained(key);
            if (result == AISyncExecutionJournalResult.RecoveryRequired)
                FenceClientExecutionLocked(AISyncExecutionRecovery.InvalidStream);
        }
        if (result is AISyncExecutionJournalResult.Duplicate or AISyncExecutionJournalResult.Rejected) return;
        if (result != AISyncExecutionJournalResult.Accepted)
        {
            RaiseClientExecutionControl(new(key.Scope, AISyncExecutionControlKind.Recovery, (uint)AISyncExecutionRecovery.InvalidStream));
            return;
        }
        try { AISyncRouteReceived?.Invoke(this, AISyncRouteCodec.Decode(canonical!, key)); }
        catch (Exception) { RejectAISyncExecution(key.Scope, AISyncExecutionRecovery.NativeUnavailable); }
    }
}
