using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

// Round lifecycle: each peer reports its own THE_PLAYER death to the Host, the
// Host ends the round once every participant is dead, then every peer shows the
// end message in PICKUPOVERLAY and returns to the frontend lobby through the
// retail "quit to main menu" path. The lobby connection itself stays alive.
internal sealed partial class MultiplayerSessionCoordinator
{
    private const string RoundEndTitle = "GAME OVER";
    private const string RoundEndMessage = "All players are dead.";
    private readonly record struct DeathReport(LobbyHost? Host, LobbyClient? Client, int Generation);
    private int _deathReportedGeneration;
    private int _roundEndingGeneration;
    private string? _frontendLobbyReturnRole;
    private RoundReturnFence? _roundReturnFence;

    private sealed class RoundReturnFence(RoundEndedPayload round)
    {
        internal RoundEndedPayload Round { get; } = round;
        internal Guid Token { get; } = Guid.NewGuid();
        internal Guid ConnectionId { get; set; }
        internal TaskCompletionSource<bool> Closed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

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

    private bool IsCurrentLobbySource(object? sender, Guid sessionId) =>
        sender is LobbyHost host && ReferenceEquals(host, _host) && host.SessionId == sessionId
        || sender is LobbyClient client && ReferenceEquals(client, _client) && client.SessionId == sessionId;

    private void OnRoundEnded(object? sender, RoundEndedPayload value)
    {
        lock (_stateGate)
            if (!IsCurrentLobbySource(sender, value.SessionId)) return;
        _ = HandleRoundEndedAsync(value);
    }

    private async Task HandleRoundEndedAsync(RoundEndedPayload value)
    {
        var fence = BeginRoundEnd(value);
        if (fence is null) return;
        _diagnostics.Log("PlayerLifecycle",
            $"Round {value.LevelGeneration} ended ({value.Reason}); minimum lobby return delay " +
            $"{value.ReturnDelayMilliseconds} ms, followed by HUD readiness/closure.");
        try
        {
            if (!await ShowRoundEndAsync(fence).ConfigureAwait(false))
            {
                fence.Closed.TrySetResult(false);
                _diagnostics.Log("PlayerLifecycle", "Round return deferred: gameplay HUD is not connected.");
                return;
            }
            await Task.Delay(value.ReturnDelayMilliseconds, _lifetime.Token).ConfigureAwait(false);
            // The delay alone cannot establish that the old movie has stopped
            // its callbacks. Wait for READY and closure of that exact socket.
            if (!await fence.Closed.Task.WaitAsync(TimeSpan.FromSeconds(10), _lifetime.Token).ConfigureAwait(false)) return;
            await ReturnToLobbyAsync(fence).ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (TimeoutException)
        {
            fence.Closed.TrySetResult(false);
            _diagnostics.Log("PlayerLifecycle",
                $"Round {value.LevelGeneration} return deferred: HUD readiness/connection closure timed out. " +
                "No native frontend request was issued; Disconnect remains available in Pause.");
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        {
            fence.Closed.TrySetResult(false);
            _diagnostics.Log("PlayerLifecycle", $"Round return failed: {exception.Message}");
        }
    }

    private RoundReturnFence? BeginRoundEnd(RoundEndedPayload value)
    {
        lock (_stateGate)
        {
            if (_start is null || _start.SessionId != value.SessionId || _start.LevelGeneration != value.LevelGeneration
                || _roundEndingGeneration == value.LevelGeneration) return null;
            _roundEndingGeneration = value.LevelGeneration;
            _roundReturnFence = new RoundReturnFence(value);
            return _roundReturnFence;
        }
    }

    private Task<bool> ShowRoundEndAsync(RoundReturnFence fence)
    {
        var value = fence.Round;
        var seconds = (value.ReturnDelayMilliseconds + 999) / 1000;
        return _bridge.SendAsync("ROUND_END", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["title"] = RoundEndTitle,
            ["message"] = RoundEndMessage,
            ["seconds"] = seconds.ToString(CultureInfo.InvariantCulture),
            ["generation"] = value.LevelGeneration.ToString(CultureInfo.InvariantCulture),
            ["sessionId"] = value.SessionId.ToString("D"),
            ["returnToken"] = fence.Token.ToString("D")
        }, _lifetime.Token);
    }

    private void AcceptRoundReturnReady(ScaleformBridgeEvent value)
    {
        var message = value.Message;
        if (value.Channel != "command" || value.ConnectionId == Guid.Empty
            || message.Get("source") != "pickup_overlay" || message.Get("phase") != "gameplay"
            || !Guid.TryParse(message.Get("sessionId"), out var sessionId)
            || !Guid.TryParse(message.Get("returnToken"), out var token)
            || !int.TryParse(message.Get("generation"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var generation)) return;
        lock (_stateGate)
        {
            var fence = _roundReturnFence;
            if (fence is null || fence.Token != token || fence.Round.SessionId != sessionId
                || fence.Round.LevelGeneration != generation || _start is null || _start.SessionId != sessionId
                || _start.LevelGeneration != generation || fence.Closed.Task.IsCompleted) return;
            if (fence.ConnectionId != Guid.Empty && fence.ConnectionId != value.ConnectionId) return;
            fence.ConnectionId = value.ConnectionId;
        }
        _diagnostics.Log("PlayerLifecycle", $"Round {generation}: HUD ready; awaiting its connection closure.");
    }

    private void OnRoundBridgeConnectionClosed(object? sender, ScaleformBridgeConnectionClosed value)
    {
        if (value.Channel != "command" || !value.PeerClosed) return;
        lock (_stateGate)
        {
            var fence = _roundReturnFence;
            if (fence is null || fence.ConnectionId == Guid.Empty || fence.ConnectionId != value.ConnectionId) return;
            if (fence.Closed.TrySetResult(true))
                _diagnostics.Log("PlayerLifecycle", $"Round {fence.Round.LevelGeneration}: HUD connection closed after readiness.");
        }
    }

    private async Task ReturnToLobbyAsync(RoundReturnFence fence)
    {
        await _commandGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            if (!EndRoundGameplayState(fence)) return;
            await CloseNativeWorldBusAsync("round-ended").ConfigureAwait(false);
            lock (_stateGate)
                if (!ReferenceEquals(_roundReturnFence, fence) || _start is not null) return;
            await RequestNativeFrontendAsync().ConfigureAwait(false);
            lock (_stateGate)
                if (ReferenceEquals(_roundReturnFence, fence)) _roundReturnFence = null;
            PublishState();
        }
        finally { _commandGate.Release(); }
    }

    // Keep the lobby connection and roster; drop every per-round replication
    // state exactly like a stop would, so the next LobbyStart starts clean.
    private bool EndRoundGameplayState(RoundReturnFence fence)
    {
        var value = fence.Round;
        lock (_stateGate)
        {
            if (!ReferenceEquals(_roundReturnFence, fence) || _start is null
                || _start.SessionId != value.SessionId || _start.LevelGeneration != value.LevelGeneration) return false;
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
        _roundReturnFence?.Closed.TrySetResult(false);
        _roundReturnFence = null;
        _deathReportedGeneration = 0;
        _roundEndingGeneration = 0;
        _frontendLobbyReturnRole = null;
    }
}
