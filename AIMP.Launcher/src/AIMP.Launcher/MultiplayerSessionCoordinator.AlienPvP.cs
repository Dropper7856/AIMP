using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    // A native lease only isolates ownership. It does not provide input or camera control.
    private readonly SemaphoreSlim _alienPvPOperationGate = new(1, 1);
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
            // The release path uses this same gate. A delayed bind from an
            // ended round can never run after its release command.
            await _alienPvPOperationGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try
            {
                lock (_stateGate)
                {
                    if (_start?.SessionId != scope.Session ||
                        _start.LevelGeneration != scope.StartGeneration ||
                        _start.AlienPeerId != scope.OwnerPeer ||
                        !_localGameplayReady ||
                        _controller.Snapshot.LevelGeneration != scope.NativeGeneration ||
                        _controller.Snapshot.ProcessId != scope.ProcessId)
                        return;
                }
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
            finally { _alienPvPOperationGate.Release(); }
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
        // Bind and release are ordered even if a pending engine-thread command
        // was queued just before the lobby disconnected.
        await _alienPvPOperationGate.WaitAsync(CancellationToken.None).ConfigureAwait(false);
        try
        {
            var runtime = _controller.Snapshot;
            if (!runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration < 1)
                return;
            try
            {
                // Allow a short native drain to finish, but never announce
                // release success when it remains fenced with outstanding Refs.
                for (var attempt = 0; attempt < 3; attempt++)
                {
                    var result = await _controller.ExecuteCommandAsync(
                        GameCommandNames.AlienPvPRelease,
                        new Dictionary<string, string>(StringComparer.Ordinal),
                        CancellationToken.None,
                        waitForExecution: true,
                        expectedLevelGeneration: runtime.LevelGeneration).ConfigureAwait(false);
                    if (result.Executed || attempt == 2)
                    {
                        _diagnostics.Log("AlienPvP",
                            "Native release: " + result.StatusCode + " - " + result.Message);
                        break;
                    }
                    await Task.Delay(80).ConfigureAwait(false);
                }
            }
            catch (Exception error)
            {
                _diagnostics.Log("AlienPvP",
                    "Best-effort native lease release failed: " + error.Message);
            }
        }
        finally
        {
            lock (_stateGate)
            {
                _alienPvPBoundScope = null;
                _alienPvPBindLastResult = string.Empty;
                _nextAlienPvPBindUtc = DateTimeOffset.MinValue;
            }
            _alienPvPOperationGate.Release();
        }
    }
}
