using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    // Release world simulation normally so the map can create its Alien. Keep
    // the local Alien's screen covered until its actual native camera is ready.
    private Task SendAlienAwareLoadingReleaseAsync(LobbyStartPayload start)
    {
        bool alien;
        lock (_stateGate) alien=start.AlienVsSurvivors && start.AlienPeerId==LocalPeerId();
        return SendSessionLoadingAsync(start,alien,alien ? "Waiting for the Alien to appear and possession to become ready..." : "Ready");
    }
    private readonly AlienPvPNetworkBridge _alienPvPNetwork;
    private AlienPvPNetworkBinding CaptureAlienPvPNetworkBinding()
    {
        var runtime=_controller.Snapshot;var connection=_controller.AISyncConnectionIdentity;
        lock(_stateGate)
        {
            var start=_start;
            var active=start?.AlienVsSurvivors==true && _lobby?.State=="Starting" &&
                _localGameplayReady && _nativeAuthorityBound && _nativeWorldBusActive &&
                runtime.IpcConnected && runtime.EngineThreadReady && runtime.LevelGeneration>0 &&
                connection!=Guid.Empty && (_localWorldContext?.WorldGeneration??0)>0;
            return new(_host,_client,start?.SessionId??Guid.Empty,start?.LevelGeneration??0,
                _localWorldContext?.WorldGeneration??0,runtime.LevelGeneration,start?.AlienPeerId??0,
                LocalPeerId(),connection,runtime.ProcessId,active);
        }
    }
    // Native ownership and network readiness are distinct proofs.
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
            // Until owned, poll fast: the spawn Move must be cancelled while the
            // Alien still runs (cycle56), before its route enters a vent.
            _nextAlienPvPBindUtc = _alienPvPBoundScope == scope ? now.AddSeconds(5) : now.AddMilliseconds(500);
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
            LobbyStartPayload? overlayStart;
            lock (_stateGate) overlayStart=_loadingHoldStart is null && _start?.AlienVsSurvivors==true &&
                _start.AlienPeerId==LocalPeerId() ? _start : null;
            if (overlayStart is not null)
            {
                var pilotReady=response.Executed && response.Message.Contains(" localPilotReady=1 ",StringComparison.Ordinal);
                await SendSessionLoadingAsync(overlayStart,!pilotReady,pilotReady ? "Ready" :
                    "Waiting for the Alien to appear and possession to become ready...").ConfigureAwait(false);
            }
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
