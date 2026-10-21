namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private int _processExitCleanup;

    private void OnGameRuntimeStateChanged(object? sender, NativeRuntimeSnapshot runtime)
    {
        if (runtime.AiRunning || Volatile.Read(ref _disposed) != 0) return;
        Guid session;
        lock (_stateGate)
        {
            if (_start is null || (_host is null && _client is null)) return;
            session = _start.SessionId;
        }
        if (Interlocked.CompareExchange(ref _processExitCleanup, 1, 0) == 0)
            _ = StopExitedGameSessionAsync(session);
    }

    private async Task StopExitedGameSessionAsync(Guid session)
    {
        try
        {
            await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try
            {
                lock (_stateGate)
                    if (_start?.SessionId != session || _controller.Snapshot.AiRunning) return;
                _diagnostics.Log("Multiplayer", "AI.exe exited during a session; closing the lobby transport.");
                await StopNetworkAsync("Local game exited").ConfigureAwait(false);
            }
            finally { _commandGate.Release(); }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) { _diagnostics.Log("Multiplayer", $"Game-exit session cleanup failed: {exception.Message}"); }
        finally { Interlocked.Exchange(ref _processExitCleanup, 0); }
    }
}
