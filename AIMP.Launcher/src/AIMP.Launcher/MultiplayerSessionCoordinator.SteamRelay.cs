using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private readonly ISteamRelay? _steamRelay;
    private bool _steamFrontendReady;
    private int _steamMenuProcessId;
    private (string Friend, ulong Host)? _pendingSteamInvite;
    private readonly object _steamInviteGate = new();
    private Task _steamInviteTask = Task.CompletedTask;

    private void InitializeSteamRelay()
    {
        if (_steamRelay is null) return;
        _steamRelay.JoinRequested += OnSteamJoinRequested;
    }

    private async Task ActivateSteamForReadyMenuAsync()
    {
        if (_steamRelay is null || !_steamFrontendReady) return;
        var runtime = _controller.Snapshot;
        if (!runtime.IpcConnected || runtime.ProcessId <= 0 || runtime.ProcessId == _steamMenuProcessId) return;
        await _steamRelay.ActivateForMenuAsync(runtime.ProcessId).ConfigureAwait(false);
        _steamMenuProcessId = runtime.ProcessId;
    }

    private async Task StartSteamHostingAsync(LobbyHost host)
    {
        if (_steamRelay is null) return;
        try
        {
            await ActivateSteamForReadyMenuAsync().ConfigureAwait(false);
            if (!await _steamRelay.StartHostingAsync(host.BoundPort, "Hosting an AIMP lobby").ConfigureAwait(false))
            {
                _diagnostics.Log("Steam", "Steam is not running; this lobby is reachable by direct IP only.");
                return;
            }
            var address = SteamRelay.FormatAddress(_steamRelay.LocalSteamId);
            await SendStatusAsync($"Lobby created - Steam friends can join {address}").ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            _diagnostics.Log("Steam", $"Could not start Steam relay hosting: {exception.Message}");
        }
    }

    private async Task StopSteamRelayAsync()
    {
        if (_steamRelay is null) return;
        _steamRelay.CloseAllTunnels("lobby closed");
        try { await _steamRelay.StopHostingAsync().ConfigureAwait(false); }
        catch (Exception exception) { _diagnostics.Log("Steam", $"Stop hosting failed: {exception.Message}"); }
    }

    private void OnSteamJoinRequested(string friendName, string address)
    {
        lock (_steamInviteGate)
        {
            if (Volatile.Read(ref _disposed) != 0) return;
            _steamInviteTask = ProcessSteamActionAfterAsync(_steamInviteTask,
                () => AcceptSteamInviteAsync(friendName, address));
        }
    }

    private async Task ProcessSteamActionAfterAsync(Task previous, Func<Task> action)
    {
        await previous.ConfigureAwait(false);
        try { await action().ConfigureAwait(false); }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _diagnostics.Log("Steam", $"Steam invitation failed: {exception.Message}");
            try { await SendErrorAsync($"Steam invitation: {exception.Message}").ConfigureAwait(false); }
            catch (Exception error) { _diagnostics.Log("Steam", $"Could not report invitation error: {error.Message}"); }
        }
    }

    // Invites share the menu command gate and never overwrite the saved IP/port.
    internal async Task AcceptSteamInviteAsync(string friendName, string address)
    {
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            _pendingSteamInvite = (friendName, ValidateSteamInvitation(address));
            if (!_steamFrontendReady || !_controller.Snapshot.IpcConnected)
            {
                await SendStatusAsync($"Invite from {friendName} accepted - open the game's main menu to join automatically").ConfigureAwait(false);
                return;
            }
            await ActivateSteamForReadyMenuAsync().ConfigureAwait(false);
            await TryJoinPendingSteamInviteAsync().ConfigureAwait(false);
        }
        finally { _commandGate.Release(); }
    }

    private ulong ValidateSteamInvitation(string address)
    {
        if (!SteamRelay.TryParseAddress(address, out var host))
            throw new ArgumentException("The Steam invitation contains an invalid account address.");
        lock (_stateGate)
            if (_host is not null || _client is not null || _uiGameplayLoaded)
                throw new InvalidOperationException("Leave the current lobby or game before accepting a Steam invitation.");
        return host;
    }

    private async Task TryJoinPendingSteamInviteAsync()
    {
        if (!_steamFrontendReady || !_controller.Snapshot.IpcConnected || _pendingSteamInvite is not { } invite) return;
        _pendingSteamInvite = null;
        EnsureMenuConnectionAvailable();
        if (_steamRelay is null) throw new InvalidOperationException("Steam relay is not available.");
        await StopNetworkAsync("Joining a Steam invitation").ConfigureAwait(false);
        await SendStatusAsync($"Joining {invite.Friend}'s lobby through Steam...").ConfigureAwait(false);
        try
        {
            await ConnectSteamInviteAsync(invite, _settingsStore.Load()).ConfigureAwait(false);
        }
        catch
        {
            _steamRelay.CloseAllTunnels("Steam invitation failed");
            throw;
        }
    }

    private async Task ConnectSteamInviteAsync((string Friend, ulong Host) invite, LauncherSettings settings)
    {
        var port = await _steamRelay!.OpenClientTunnelAsync(invite.Host, _lifetime.Token).ConfigureAwait(false);
        var options = new LobbyClientOptions("127.0.0.1", port, settings.PlayerId,
            settings.PlayerName, LobbyCompatibility.Create(settings));
        var client = await LobbyClient.ConnectAsync(options, _lifetime.Token).ConfigureAwait(false);
        await CompleteMenuJoinAsync(client).ConfigureAwait(false);
        await SendMenuLobbyOpenedAsync("client").ConfigureAwait(false);
        await SendStatusAsync($"Connected to {invite.Friend} through Steam").ConfigureAwait(false);
    }

    private void ScheduleSteamMenuResume()
    {
        lock (_steamInviteGate)
        {
            if (Volatile.Read(ref _disposed) != 0 || _steamRelay is null) return;
            _steamInviteTask = ProcessSteamActionAfterAsync(_steamInviteTask, ResumeSteamInvitesFromMenuAsync);
        }
    }

    private async Task ResumeSteamInvitesFromMenuAsync()
    {
        // MENU_STATE also covers native reconnects after the frontend's first HELLO.
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            lock (_stateGate)
                _steamFrontendReady = !_uiGameplayLoaded && _start is null && _controller.Snapshot.IpcConnected;
            await ActivateSteamForReadyMenuAsync().ConfigureAwait(false);
            await TryJoinPendingSteamInviteAsync().ConfigureAwait(false);
        }
        finally { _commandGate.Release(); }
    }

    private void DisposeSteamInvites()
    {
        if (_steamRelay is not null) _steamRelay.JoinRequested -= OnSteamJoinRequested;
        Task pending;
        lock (_steamInviteGate) pending = _steamInviteTask;
        pending.GetAwaiter().GetResult();
        _pendingSteamInvite = null;
    }

    private Task LeaveLobbyFromMenuAsync()
    {
        _pendingSteamInvite = null;
        return StopNetworkAsync("Lobby left from the game UI");
    }
}
