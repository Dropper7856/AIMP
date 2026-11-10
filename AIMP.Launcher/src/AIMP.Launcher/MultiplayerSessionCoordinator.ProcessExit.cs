namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private int _processExitCleanup;
    private int _gameSeenRunning;

    // Runs for any lobby, also one still in preflight (no _start yet): a lobby left
    // open after a crash re-runs the preflight that crashed the game on the next launch.
    // Only a real exit counts: AI.exe must have been seen running first.
    private void OnGameRuntimeStateChanged(object? sender, NativeRuntimeSnapshot runtime)
    {
        if (runtime.AiRunning) { Volatile.Write(ref _gameSeenRunning, 1); return; }
        if (Volatile.Read(ref _disposed) != 0 || Interlocked.Exchange(ref _gameSeenRunning, 0) == 0) return;
        lock (_stateGate)
            if (_host is null && _client is null && Volatile.Read(ref _steamMenuProcessId) == 0) return;
        if (Interlocked.CompareExchange(ref _processExitCleanup, 1, 0) == 0)
            _ = CleanUpAfterGameExitAsync();
    }

    private async Task CleanUpAfterGameExitAsync()
    {
        try
        {
            await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try { await CleanUpAfterGameExitLockedAsync().ConfigureAwait(false); }
            finally { _commandGate.Release(); }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) { _diagnostics.Log("Multiplayer", $"Game-exit session cleanup failed: {exception.Message}"); }
        finally { Interlocked.Exchange(ref _processExitCleanup, 0); }
    }

    private async Task CleanUpAfterGameExitLockedAsync()
    {
        if (_controller.Snapshot.AiRunning) return;
        bool inLobby;
        lock (_stateGate) inLobby = _host is not null || _client is not null;
        if (inLobby)
        {
            _diagnostics.Log("Multiplayer", "AI.exe exited during a session; closing the lobby transport.");
            await StopNetworkAsync("Local game exited").ConfigureAwait(false);
        }
        await ReleaseSteamAfterGameExitAsync().ConfigureAwait(false);
    }
}
