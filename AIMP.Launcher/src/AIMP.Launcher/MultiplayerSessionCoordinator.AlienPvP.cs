using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    // Read-only native candidate discovery. A candidate is NOT possession:
    // the Player-owned Alien still needs a certified lifetime/authority lease.
    private int _alienPvPProbeRunning;
    private DateTimeOffset _nextAlienPvPProbeUtc = DateTimeOffset.MinValue;
    private (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId)?
        _alienPvPProvenScope;
    private string _alienPvPProbeLastResult = string.Empty;

    private void TryScheduleAlienPvPIdentityProbe(NativeRuntimeSnapshot runtime, DateTimeOffset now)
    {
        if (!runtime.AiRunning || !runtime.IpcConnected ||
            !runtime.EngineThreadReady || runtime.LevelGeneration < 1)
            return;
        if (Interlocked.CompareExchange(ref _alienPvPProbeRunning, 1, 0) != 0) return;

        (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId) scope;
        lock (_stateGate)
        {
            var start = _start;
            if (start?.AlienVsSurvivors != true || !_localGameplayReady ||
                now < _nextAlienPvPProbeUtc)
            {
                Interlocked.Exchange(ref _alienPvPProbeRunning, 0);
                return;
            }
            scope = (start.SessionId, start.LevelGeneration, runtime.LevelGeneration, runtime.ProcessId);
            // Keep checking after success. A retail Alien may be despawned into
            // a vent and replaced without changing the level generation.
            _nextAlienPvPProbeUtc = now.AddSeconds(5);
        }
        _ = ProbeNativeAlienCandidateAsync(scope);
    }

    private async Task ProbeNativeAlienCandidateAsync(
        (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId) scope)
    {
        try
        {
            var response = await _controller.ExecuteCommandAsync(
                GameCommandNames.AlienPvPProbe,
                new Dictionary<string, string>(StringComparer.Ordinal),
                _lifetime.Token,
                waitForExecution: true,
                expectedLevelGeneration: scope.NativeGeneration).ConfigureAwait(false);
            var changed = false;
            var message = (response.Executed ? "Read-only candidate verified: " : "Native candidate not ready: ") +
                response.Message;
            lock (_stateGate)
            {
                if (_start?.SessionId != scope.Session ||
                    _start.LevelGeneration != scope.StartGeneration ||
                    _controller.Snapshot.LevelGeneration != scope.NativeGeneration ||
                    _controller.Snapshot.ProcessId != scope.ProcessId)
                    return;
                // Revocation is as important as the initial identity proof.
                var previousProof = _alienPvPProvenScope;
                _alienPvPProvenScope = response.Executed ? scope : null;
                changed = previousProof != _alienPvPProvenScope ||
                    !string.Equals(_alienPvPProbeLastResult, message, StringComparison.Ordinal);
                _alienPvPProbeLastResult = message;
            }
            if (changed)
                _diagnostics.Log("AlienPvP", message +
                    " Possession remains disabled until its exclusive native lease is implemented.");
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception error)
        {
            _diagnostics.Log("AlienPvP", "Native candidate preflight failed: " + error.Message);
        }
        finally
        {
            Interlocked.Exchange(ref _alienPvPProbeRunning, 0);
        }
    }
}
