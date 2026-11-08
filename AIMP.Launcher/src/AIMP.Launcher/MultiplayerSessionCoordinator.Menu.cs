using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private static bool ReadLobbyAlienVsSurvivors(ScaleformBridgeMessage message) =>
        message.Get("alienVsSurvivors", "0") == "1";

    private static bool ReadLobbyAlienEnabled(ScaleformBridgeMessage message) =>
        message.Get("alienEnabled", "1") switch
        {
            "1" => true,
            "0" => false,
            _ => throw new InvalidOperationException("Invalid Alien setting.")
        };

    private async Task SendMenuReadyStateAsync()
    {
        ScheduleSteamMenuResume();
        await _bridge.SendAsync("FRONTEND_STATE", new Dictionary<string, string>
        {
            ["ready"] = _controller.Snapshot.IpcConnected ? "1" : "0"
        }).ConfigureAwait(false);
        LobbySnapshotPayload? lobby;
        lock (_stateGate) lobby = _start is null ? _lobby : null;
        if (lobby is null) return;
        ScheduleLobbyPreparation(lobby);
        await SendLobbyStateAsync(lobby).ConfigureAwait(false);
    }

    private void EnsureMenuConnectionAvailable()
    {
        if (!_controller.Snapshot.IpcConnected)
            throw new InvalidOperationException("Wait for the launcher to be ready before connecting.");
        lock (_stateGate)
            if (_host is not null || _client is not null)
                throw new InvalidOperationException("Leave the current lobby before opening another one.");
    }

    private bool CanStartMenuGame()
    {
        lock (_stateGate) return _host?.CanStart == true;
    }

    private Task SendMenuLobbyOpenedAsync(string role) => _bridge.SendAsync(
        "LOBBY_OPENED", new Dictionary<string, string> { ["role"] = role });

    private Task SendMenuSessionClosedAsync(string reason) => _bridge.SendAsync(
        "SESSION_CLOSED", new Dictionary<string, string> { ["reason"] = reason });

    private async Task<bool> AwaitMenuHelloReadyAsync(ScaleformBridgeMessage message)
    {
        if (!IsGameplaySource(message.Get("source", "frontend"), message.Get("phase", "unknown")))
            return _controller.Snapshot.IpcConnected;
        var deadline = DateTimeOffset.UtcNow.AddSeconds(30);
        while (!_controller.Snapshot.IpcConnected)
        {
            if (DateTimeOffset.UtcNow >= deadline) return false;
            await Task.Delay(100, _lifetime.Token).ConfigureAwait(false);
        }
        return true;
    }

    private async Task<LobbyHost> StartMenuHostAsync(LobbyHostOptions options)
    {
        try { return await StartHostAsync(options).ConfigureAwait(false); }
        catch
        {
            await StopNetworkAsync("Unable to create lobby").ConfigureAwait(false);
            await SendMenuSessionClosedAsync("Unable to create lobby").ConfigureAwait(false);
            throw;
        }
    }

    private async Task CompleteMenuJoinAsync(LobbyClient client)
    {
        try { await ActivateMenuClientAsync(client).ConfigureAwait(false); }
        catch
        {
            await StopNetworkAsync("Unable to join lobby").ConfigureAwait(false);
            throw;
        }
    }

    private async Task ActivateMenuClientAsync(LobbyClient client)
    {
        AttachClient(client);
        lock (_stateGate) { _client = client; _lobby = client.Snapshot; }
        client.Start();
        await SetNativeWorldSyncLifecycleAsync(
            client.SessionId, client.PeerId, false, 0, true, false, "lobby-client-joined")
            .ConfigureAwait(false);
        await PublishLobbyAsync(client, client.Snapshot).ConfigureAwait(false);
    }

    private bool TryHandleImmediateMenuMessage(ScaleformBridgeEvent value)
    {
        if (value.Message.Type.Equals("MENU_STATE", StringComparison.OrdinalIgnoreCase))
        { _ = SendMenuReadyStateAsync(); return true; }
        if (value.Reply is not null && value.Channel == "event" && IsVoiceSettingsMessage(value.Message))
        { _ = HandlePauseVoiceMessageAsync(value); return true; }
        if (!IsPauseMenuMessage(value.Message)) return false;
        _ = HandlePauseMenuMessageAsync(value);
        return true;
    }

    private IReadOnlyDictionary<string, string> WithMenuLobbyControls(
        LobbySnapshotPayload snapshot, Dictionary<string, string> values)
    {
        values["canStart"] = CanStartMenuGame() ? "1" : "0";
        values["localReady"] = snapshot.Players.Any(value => IsLocalPlayer(value) && value.Ready) ? "1" : "0";
        return values;
    }
}
