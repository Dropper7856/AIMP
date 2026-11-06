using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    // A native lease only isolates ownership. It does not provide input or camera control.
    private int _alienPvPBindingRunning;
    private DateTimeOffset _nextAlienPvPBindUtc = DateTimeOffset.MinValue;
    private (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId, ulong OwnerPeer)?
        _alienPvPBoundScope;
    private string _alienPvPBindLastResult = string.Empty;

    private void TryScheduleAlienPvPBinding(NativeRuntimeSnapshot runtime, DateTimeOffset now)
    {
        if (!runtime.AiRunning || !runtime.IpcConnected ||
            !runtime.EngineThreadReady || runtime.LevelGeneration < 1)
            return;
        if (Interlocked.CompareExchange(ref _alienPvPBindingRunning, 1, 0) != 0) return;

        (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId, ulong OwnerPeer) scope;
        lock (_stateGate)
        {
            var start = _start;
            if (start?.AlienVsSurvivors != true || start.AlienPeerId == 0UL || !_localGameplayReady ||
                now < _nextAlienPvPBindUtc)
            {
                Interlocked.Exchange(ref _alienPvPBindingRunning, 0);
                return;
            }
            scope = (start.SessionId, start.LevelGeneration, runtime.LevelGeneration, runtime.ProcessId, start.AlienPeerId);
            // Keep checking after success. A retail Alien can be replaced at a vent.
            _nextAlienPvPBindUtc = now.AddSeconds(_alienPvPBoundScope == scope ? 5 : 2);
        }
        _ = BindNativeAlienAsync(scope);
    }

    private async Task BindNativeAlienAsync(
        (Guid Session, int StartGeneration, int NativeGeneration, int ProcessId, ulong OwnerPeer) scope)
    {
        try
        {
            var response = await _controller.ExecuteCommandAsync(
                GameCommandNames.AlienPvPBind,
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["ownerPeer"] = scope.OwnerPeer.ToString(CultureInfo.InvariantCulture)
                },
                _lifetime.Token,
                waitForExecution: true,
                expectedLevelGeneration: scope.NativeGeneration).ConfigureAwait(false);
            var changed = false;
            var message = response.StatusCode + ": " + response.Message;
            lock (_stateGate)
            {
                if (_start?.SessionId != scope.Session ||
                    _start.LevelGeneration != scope.StartGeneration ||
                    _start.AlienPeerId != scope.OwnerPeer ||
                    _controller.Snapshot.LevelGeneration != scope.NativeGeneration ||
                    _controller.Snapshot.ProcessId != scope.ProcessId)
                    return;
                // Revocation is as important as the initial identity proof.
                var previousProof = _alienPvPBoundScope;
                _alienPvPBoundScope = response.Executed ? scope : null;
                changed = previousProof != _alienPvPBoundScope ||
                    !string.Equals(_alienPvPBindLastResult, message, StringComparison.Ordinal);
                _alienPvPBindLastResult = message;
            }
            if (changed)
                _diagnostics.Log("AlienPvP", (response.Executed ?
                    "Native Alien lease owned: " : "Native Alien lease pending: ") + message);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception error)
        {
            _diagnostics.Log("AlienPvP", "Native Alien binding failed: " + error.Message);
        }
        finally
        {
            Interlocked.Exchange(ref _alienPvPBindingRunning, 0);
        }
    }

    private async Task ReleaseNativeAlienPvPLeaseBestEffortAsync()
    {
        var runtime = _controller.Snapshot;
        if (!runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration < 1)
            return;
        try
        {
            var result = await _controller.ExecuteCommandAsync(
                GameCommandNames.AlienPvPRelease,
                new Dictionary<string, string>(StringComparer.Ordinal),
                CancellationToken.None,
                waitForExecution: true,
                expectedLevelGeneration: runtime.LevelGeneration).ConfigureAwait(false);
            _diagnostics.Log("AlienPvP", "Native release: " + result.StatusCode + " - " + result.Message);
        }
        catch (Exception error)
        {
            _diagnostics.Log("AlienPvP", "Best-effort native lease release failed: " + error.Message);
        }
        finally
        {
            lock (_stateGate)
            {
                _alienPvPBoundScope = null;
                _alienPvPBindLastResult = string.Empty;
                _nextAlienPvPBindUtc = DateTimeOffset.MinValue;
            }
        }
    }
}
