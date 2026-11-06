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
            if (_alienPvPProvenScope == scope)
            {
                Interlocked.Exchange(ref _alienPvPProbeRunning, 0);
                return;
            }
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
            lock (_stateGate)
            {
                if (_start?.SessionId != scope.Session ||
                    _start.LevelGeneration != scope.StartGeneration ||
                    _controller.Snapshot.LevelGeneration != scope.NativeGeneration ||
                    _controller.Snapshot.ProcessId != scope.ProcessId)
                    return;
                if (response.Executed) _alienPvPProvenScope = scope;
            }
            _diagnostics.Log("AlienPvP",
                (response.Executed ? "Read-only candidate verified: " : "Native candidate not ready: ") +
                response.Message +
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
