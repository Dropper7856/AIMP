using AIMP.Protocol;

namespace AIMP.Launcher;

// Leaving a running multiplayer game: the pause menu "Disconnect" item, the
// "X left the game" popups shown to the remaining players, and the return to
// the main menu of Clients whose Host left mid-game.
internal sealed partial class MultiplayerSessionCoordinator
{
    private const int HostLeftReturnDelayMilliseconds = 5000;
    private long _networkStopVersion;

    private static bool IsPauseMenuMessage(ScaleformBridgeMessage message) =>
        message.Type.Trim().StartsWith("PAUSE_MENU_", StringComparison.OrdinalIgnoreCase);

    private async Task HandlePauseMenuMessageAsync(ScaleformBridgeEvent value)
    {
        var type = value.Message.Type.Trim().ToUpperInvariant();
        if (type == "PAUSE_MENU_HELLO") { await ReplyPauseMenuStateAsync(value).ConfigureAwait(false); return; }
        if (type != "PAUSE_MENU_DISCONNECT" || value.Reply is null) return;
        var success = false;
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try { await LeaveGameFromPauseMenuAsync().ConfigureAwait(false); success = true; }
        catch (Exception exception)
        {
            _diagnostics.Log("Multiplayer", $"Pause menu disconnect failed: {exception.Message}");
        }
        finally { _commandGate.Release(); }
        await value.Reply("PAUSE_MENU_DISCONNECT_READY", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["requestId"] = value.Message.Get("requestId", "0"),
            ["success"] = success ? "1" : "0"
        }).ConfigureAwait(false);
    }

    // The pause menu only shows "Disconnect" while a lobby session is inside
    // an active launch generation; otherwise it keeps the retail item.
    private Task ReplyPauseMenuStateAsync(ScaleformBridgeEvent value)
    {
        string role;
        lock (_stateGate)
            role = _start is null ? string.Empty : _host is not null ? "host" : _client is not null ? "client" : string.Empty;
        if (value.Reply is null) return Task.CompletedTask;
        return value.Reply("PAUSE_MENU_STATE", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["multiplayer"] = role.Length == 0 ? "0" : "1",
            ["role"] = role
        });
    }

    // The pause menu itself runs the retail back_to_frontend path (the engine
    // command dispatcher does not tick while paused), so remote Characters are
    // not driven here: they are destroyed with the level, which releases their
    // native movement mailboxes. Everything else matches StopNetworkAsync.
    private async Task LeaveGameFromPauseMenuAsync()
    {
        await ReleaseSessionLoadingBestEffortAsync().ConfigureAwait(false);
        var stopState = BeginNetworkStop();
        if (stopState.Host is null && stopState.Client is null) return;
        DetachNetwork(stopState.Host, stopState.Client);
        try { await UnbindNativeWorldSyncAuthorityAsync("pause-menu-disconnect").ConfigureAwait(false); }
        finally
        {
            lock (_stateGate) ClearSessionState();
            await DisposeNetworkAsync(stopState.Host, stopState.Client).ConfigureAwait(false);
        }
        _diagnostics.Log("Multiplayer", "Disconnected from the pause menu.");
        PublishState();
    }

    private string[] CaptureDepartedPlayerNames(LobbySnapshotPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _lobby is null) return Array.Empty<string>();
            var active = value.Players.Select(player => player.PeerId).ToHashSet();
            return _lobby.Players.Where(player => !active.Contains(player.PeerId))
                .Select(player => player.DisplayName).ToArray();
        }
    }

    private async Task ShowPlayersLeftAsync(IEnumerable<string> names, Guid sessionId)
    {
        foreach (var name in names)
        {
            lock (_stateGate)
                if (_start?.SessionId != sessionId) return;
            await ShowPlayerLeftMessageAsync(name).ConfigureAwait(false);
        }
    }

    private async Task ShowPlayerLeftMessageAsync(string playerName)
    {
        try
        {
            var result = await _controller.ExecuteCommandAsync(GameCommandNames.PopupMessage,
                RemoteSpawnPlanner.CreateLeftMessage(playerName), _lifetime.Token).ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
            _diagnostics.Log("Multiplayer",
                $"Player left PopMessage was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Multiplayer", $"Player left PopMessage failed: {exception.Message}");
        }
    }

    private string? CaptureInGameHostName()
    {
        lock (_stateGate)
        {
            if (_client is null || _start is null) return null;
            return _lobby?.Players.FirstOrDefault(player => player.IsHost)?.DisplayName ?? "Host";
        }
    }

    // Without its Host the level has no AI or world authority left: tell the
    // player, then leave through the same retail path as the end of a round.
    private async Task HandleHostLeftGameAsync(string hostName, long stopVersion)
    {
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            lock (_stateGate)
                if (_networkStopVersion != stopVersion || _host is not null || _client is not null) return;
            await ShowPlayerLeftMessageAsync(hostName).ConfigureAwait(false);
        }
        finally { _commandGate.Release(); }
        try { await Task.Delay(HostLeftReturnDelayMilliseconds, _lifetime.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) { return; }
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            lock (_stateGate)
                if (_networkStopVersion != stopVersion || _host is not null || _client is not null) return;
            await RequestNativeFrontendAsync().ConfigureAwait(false);
        }
        finally { _commandGate.Release(); }
    }
}
