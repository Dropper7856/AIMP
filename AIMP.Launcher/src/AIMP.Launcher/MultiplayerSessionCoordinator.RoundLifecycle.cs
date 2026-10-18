using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

// Round lifecycle: each peer reports its own THE_PLAYER death to the Host, the
// Host ends the round once every participant is dead, then every peer shows the
// end message in PICKUPOVERLAY and returns to the frontend lobby through the
// retail "quit to main menu" path. The lobby connection itself stays alive.
internal sealed partial class MultiplayerSessionCoordinator
{
    private const string RoundEndTitle = "FIN DE LA PARTIE";
    private const string RoundEndMessage = "Tous les joueurs sont morts.";
    private readonly record struct DeathReport(LobbyHost? Host, LobbyClient? Client, int Generation);
    private int _deathReportedGeneration;
    private int _roundEndingGeneration;
    private string? _frontendLobbyReturnRole;

    private void OnNativePlayerLifecycleStateChanged(
        object? sender,
        NativePlayerLifecycleStatePayload value) =>
        _ = ReportLocalDeathAsync(value);

    private async Task ReportLocalDeathAsync(NativePlayerLifecycleStatePayload value)
    {
        if (CaptureDeathReport() is not { } report) return;
        _diagnostics.Log("PlayerLifecycle",
            $"Local player death #{value.DeathSequence} reported for generation {report.Generation}.");
        try { await SendLocalDeathAsync(report).ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or InvalidOperationException or ProtocolException or OperationCanceledException)
        {
            _diagnostics.Log("PlayerLifecycle", $"Local death report failed: {exception.Message}");
        }
    }

    // One report per launch generation: there is no respawn, and a death seen
    // outside an active round (frontend, stopped session) is not a round event.
    private DeathReport? CaptureDeathReport()
    {
        lock (_stateGate)
        {
            if (_start is null || _deathReportedGeneration == _start.LevelGeneration) return null;
            if (_host is null && _client is null) return null;
            _deathReportedGeneration = _start.LevelGeneration;
            return new DeathReport(_host, _client, _start.LevelGeneration);
        }
    }

    private Task SendLocalDeathAsync(DeathReport report)
    {
        if (report.Host is not null)
            return report.Host.ReportHostPlayerDeathAsync(report.Generation, _lifetime.Token);
        return report.Client!.SendPlayerDeathAsync(report.Generation, _lifetime.Token);
    }

    private void OnRoundEnded(object? sender, RoundEndedPayload value) =>
        _ = HandleRoundEndedAsync(value);

    private async Task HandleRoundEndedAsync(RoundEndedPayload value)
    {
        if (!BeginRoundEnd(value)) return;
        _diagnostics.Log("PlayerLifecycle",
            $"Round {value.LevelGeneration} ended ({value.Reason}); returning to lobby in " +
            $"{value.ReturnDelayMilliseconds} ms.");
        await ShowRoundEndAsync(value).ConfigureAwait(false);
        try { await Task.Delay(value.ReturnDelayMilliseconds, _lifetime.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) { return; }
        await ReturnToLobbyAsync(value).ConfigureAwait(false);
    }

    private bool BeginRoundEnd(RoundEndedPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _start.LevelGeneration != value.LevelGeneration
                || _roundEndingGeneration == value.LevelGeneration) return false;
            _roundEndingGeneration = value.LevelGeneration;
            return true;
        }
    }

    private Task ShowRoundEndAsync(RoundEndedPayload value)
    {
        var seconds = (value.ReturnDelayMilliseconds + 999) / 1000;
        return _bridge.SendAsync("ROUND_END", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["title"] = RoundEndTitle,
            ["message"] = RoundEndMessage,
            ["seconds"] = seconds.ToString(CultureInfo.InvariantCulture),
            ["generation"] = value.LevelGeneration.ToString(CultureInfo.InvariantCulture)
        });
    }

    private async Task ReturnToLobbyAsync(RoundEndedPayload value)
    {
        if (!EndRoundGameplayState(value)) return;
        await CloseNativeWorldBusAsync("round-ended").ConfigureAwait(false);
        await RequestNativeFrontendAsync().ConfigureAwait(false);
        PublishState();
    }

    // Keep the lobby connection and roster; drop every per-round replication
    // state exactly like a stop would, so the next LobbyStart starts clean.
    private bool EndRoundGameplayState(RoundEndedPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _start.LevelGeneration != value.LevelGeneration) return false;
            _start = null;
            _uiGameplayLoaded = false;
            _localGameplayReady = false;
            _localReadySending = false;
            _remoteGameplayReady = false;
            ResetWorldReplicationState(preserveCommittedCheckpoint: true);
            ResetRemoteSpawnState();
            _gameplayReadiness.Reset();
            _frontendLobbyReturnRole = _host is not null ? "host" : "client";
            return true;
        }
    }

    private async Task RequestNativeFrontendAsync()
    {
        try
        {
            var result = await _controller.ExecuteCommandAsync(GameCommandNames.ReturnToFrontend,
                new Dictionary<string, string>(StringComparer.Ordinal), _lifetime.Token).ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result)) return;
            _diagnostics.Log("PlayerLifecycle",
                $"Return to frontend was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("PlayerLifecycle", $"Return to frontend failed: {exception.Message}");
        }
    }

    // The frontend movie reconnects after the level unloads. CONFIG and the
    // lobby snapshot were already re-sent by HELLO; LOBBY_RETURN asks the menu
    // to reopen the lobby page instead of leaving the player on the root menu.
    private async Task SendFrontendReadyAsync()
    {
        await SendStatusAsync("Launcher bridge ready").ConfigureAwait(false);
        string? role;
        lock (_stateGate)
        {
            role = _host is null && _client is null ? null : _frontendLobbyReturnRole;
            _frontendLobbyReturnRole = null;
        }
        if (role is null) return;
        await _bridge.SendAsync("LOBBY_RETURN", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["role"] = role
        }).ConfigureAwait(false);
    }

    private void ClearRoundLifecycleState()
    {
        _deathReportedGeneration = 0;
        _roundEndingGeneration = 0;
        _frontendLobbyReturnRole = null;
    }
}
