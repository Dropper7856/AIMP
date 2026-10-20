using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private readonly GamePreparationService? _gamePreparation;
    private Guid _preflightToken;
    private Task<PreparedWorldManifest?>? _preflightTask;
    private PreparedWorldManifest? _preflightManifest;
    private CancellationTokenSource? _preflightCancellation;
    private readonly SemaphoreSlim _nativeManifestGate = new(1, 1);
    private (Guid Session, int Start, int Native, int Process, string Sha)? _loadedManifest;
    private LobbyStartPayload? _loadingHoldStart;
    private bool _loadingOverlayAcknowledged;
    private bool _sessionPreparedSending;
    private bool _sessionPreparedSent;
    private bool _sessionReleaseAuthorized;
    private bool _sessionReleaseRunning;
    private bool _manifestPreloadRunning;
    private DateTimeOffset _nextPreparedReportUtc;
    private DateTimeOffset _nextManifestPreloadUtc;
    private Guid _loadingOverlayConnection;
    private DateTimeOffset _nextPreparationProgressUtc;

    private void ReportSessionPreparationProgress(NativeRuntimeSnapshot runtime, DateTimeOffset now, string positionError)
    {
        string message;
        lock (_stateGate)
        {
            if (_loadingHoldStart is null || now < _nextPreparationProgressUtc) return;
            _nextPreparationProgressUtc = now.AddSeconds(5);
            message = $"Loading barrier: overlay={_loadingOverlayAcknowledged}, position={_positionAvailable}, " +
                $"spawnStable={_gameplayReadiness.IsReady(now)}, localReady={_localGameplayReady}, remoteReady={_remoteGameplayReady}, " +
                $"characters={_remoteCharacters.Count}/{Math.Max(0, (_lobby?.Players.Count ?? 1) - 1)}, " +
                $"manifest={_loadedManifest is { } loaded && loaded.Native == runtime.LevelGeneration && loaded.Process == runtime.ProcessId}, " +
                $"world={GetWorldBusLifecycleStateLocked()}, release={_sessionReleaseAuthorized}; {positionError}";
        }
        _diagnostics.Log("Preparation", message);
    }

    private void ScheduleLobbyPreparation(LobbySnapshotPayload lobby)
    {
        lock (_stateGate)
        {
            if (_start is not null || !lobby.WorldPreparationRequired || lobby.PreparationToken == Guid.Empty
                || (_host is null && _client is null)) return;
            if (_preflightToken == lobby.PreparationToken && _preflightTask is not null) return;
            _preflightCancellation?.Cancel();
            _preflightCancellation?.Dispose();
            _preflightCancellation = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
            _preflightToken = lobby.PreparationToken;
            _preflightManifest = null;
            var token = _preflightCancellation.Token;
            _preflightTask = Task.Run(() => PrepareLobbyLevelAsync(lobby, token), token);
        }
    }

    private async Task<PreparedWorldManifest?> PrepareLobbyLevelAsync(LobbySnapshotPayload lobby, CancellationToken token)
    {
        PreparedWorldManifest? prepared = null;
        string error = string.Empty;
        try
        {
            await SendStatusAsync("Verifying level preparation...").ConfigureAwait(false);
            if (_gamePreparation is null) throw new InvalidOperationException("Game preparation is unavailable. Complete launcher setup first.");
            prepared = await _gamePreparation.PrepareLevelAsync(lobby.Level, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { return null; }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            error = exception.Message.Length <= 2048 ? exception.Message : exception.Message[..2048];
            _diagnostics.Log("Preparation", $"Level preflight failed for {lobby.Level}: {error}");
        }
        LobbyHost? host;
        LobbyClient? client;
        lock (_stateGate)
        {
            if (token.IsCancellationRequested || _lobby?.SessionId != lobby.SessionId
                || _preflightToken != lobby.PreparationToken || _start is not null) return null;
            _preflightManifest = prepared;
            host = _host; client = _client;
        }
        var peer = host?.HostPeerId ?? client?.PeerId ?? 0UL;
        if (peer == 0UL) return null;
        var report = new LobbyPreparationPayload(lobby.SessionId, peer, lobby.Level,
            lobby.PreparationToken, prepared?.Identity, prepared?.Sha256 ?? string.Empty,
            prepared is not null, error);
        try
        {
            if (host is not null) await host.SetHostPreparationAsync(report, token).ConfigureAwait(false);
            else if (client is not null) await client.SendLobbyPreparationAsync(report, token).ConfigureAwait(false);
            await SendStatusAsync(prepared is not null ? "Level verified. Waiting for players." : "Level preparation failed. Check launcher setup.").ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or OperationCanceledException or ProtocolException)
        { if (!token.IsCancellationRequested) _diagnostics.Log("Preparation", $"Preparation report failed: {exception.Message}"); }
        return prepared;
    }

    private async Task AwaitLobbyPreparationAsync(LobbyHost host)
    {
        var deadline = DateTimeOffset.UtcNow.AddSeconds(60);
        while (true)
        {
            LobbySnapshotPayload snapshot;
            Task<PreparedWorldManifest?>? preparation;
            lock (_stateGate)
            {
                if (!ReferenceEquals(host, _host) || _lobby is null) throw new OperationCanceledException("Lobby changed during preparation.");
                snapshot = _lobby;
            }
            ScheduleLobbyPreparation(snapshot);
            lock (_stateGate) preparation = _preflightTask;
            if (!snapshot.WorldPreparationRequired) return;
            if (preparation is not null) await preparation.ConfigureAwait(false);
            lock (_stateGate)
            {
                if (_lobby?.SessionId != snapshot.SessionId || _lobby.PreparationToken != snapshot.PreparationToken)
                    throw new OperationCanceledException("Selected level changed during preparation.");
                if (_preflightManifest is null) throw new InvalidOperationException("Level preparation failed. Complete launcher setup before starting.");
                if (host.CanStart) return;
                var failure = _lobby.Preparations?.FirstOrDefault(value => !value.Ready && !string.IsNullOrWhiteSpace(value.Error));
                if (failure is not null) throw new InvalidOperationException($"A player's level preparation failed: {failure.Error}");
                var mismatch = _lobby.Preparations?.FirstOrDefault(value => value.Ready
                    && (value.Manifest != _preflightManifest.Identity
                        || !string.Equals(value.ManifestSha256, _preflightManifest.Sha256, StringComparison.OrdinalIgnoreCase)));
                if (mismatch is not null)
                    throw new InvalidOperationException("Players have different level files. Prepare the same game version and level modifications on every installation before starting.");
            }
            if (DateTimeOffset.UtcNow >= deadline) throw new InvalidOperationException("Waiting for all players to verify this level and become Ready. Try Start again when they are ready.");
            await Task.Delay(100, _lifetime.Token).ConfigureAwait(false);
        }
    }

    private async Task SetSessionLoadingHoldAsync(LobbyStartPayload start, bool active, CancellationToken token)
    {
        if (active)
        {
            var runningGame = ResolveRuntimeGameDirectory(_controller.Snapshot.ExecutablePath);
            var configuredGame = _settingsStore.Load().GameDirectory;
            if (runningGame is null || string.IsNullOrWhiteSpace(configuredGame)
                || !string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(runningGame)),
                    Path.TrimEndingDirectorySeparator(Path.GetFullPath(configuredGame)), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The running game does not match the installation prepared by the launcher.");
        }
        var result = await _controller.ExecuteCommandAsync("SESSION_LOADING_HOLD",
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["active"] = active ? "1" : "0", ["sessionId"] = start.SessionId.ToString("D"),
                ["generation"] = start.LevelGeneration.ToString(CultureInfo.InvariantCulture)
            }, token).ConfigureAwait(false);
        if (!result.Executed || result.StatusCode != "SESSION_LOADING_APPLIED")
            throw new InvalidOperationException($"Session preparation hold failed: {result.StatusCode} - {result.Message}");
    }

    private async Task SendSessionLoadingAsync(LobbyStartPayload start, bool active, string message)
    {
        await _bridge.SendAsync("SESSION_LOADING", new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["active"] = active ? "1" : "0", ["sessionId"] = start.SessionId.ToString("D"),
            ["generation"] = start.LevelGeneration.ToString(CultureInfo.InvariantCulture), ["message"] = message
        }).ConfigureAwait(false);
    }

    private void AcceptSessionLoadingAck(ScaleformBridgeEvent value)
    {
        var message = value.Message;
        if (value.Channel != "command" || value.ConnectionId == Guid.Empty || value.Reply is null
            || message.Get("source") != "pickup_overlay" || message.Get("phase") != "gameplay"
            || !Guid.TryParse(message.Get("sessionId"), out var session)
            || !int.TryParse(message.Get("generation"), NumberStyles.None, CultureInfo.InvariantCulture, out var generation)) return;
        lock (_stateGate)
        {
            if (_loadingHoldStart is not { } start || start.SessionId != session || start.LevelGeneration != generation
                || _start?.SessionId != session || _start.LevelGeneration != generation) return;
            if (_loadingOverlayConnection == Guid.Empty || _loadingOverlayConnection != value.ConnectionId) return;
            if (message.Get("active") == "1")
            {
                _loadingOverlayConnection = value.ConnectionId;
                _loadingOverlayAcknowledged = true;
            }
        }
        _ = TryCompleteSessionPreparationAsync();
    }

    private void OnSessionReleased(object? sender, SessionReleasePayload release)
    {
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, release.SessionId) || _start?.SessionId != release.SessionId
                || _start.LevelGeneration != release.LevelGeneration) return;
            _sessionReleaseAuthorized = true;
        }
        _ = TryCompleteSessionPreparationAsync();
    }

    private async Task TryCompleteSessionPreparationAsync()
    {
        LobbyStartPayload start;
        LobbyHost? host;
        LobbyClient? client;
        bool release;
        lock (_stateGate)
        {
            if (_loadingHoldStart is not { } current || _start?.SessionId != current.SessionId
                || _start.LevelGeneration != current.LevelGeneration || !_loadingOverlayAcknowledged
                || !_localGameplayReady || !_remoteGameplayReady || _lobby is null) return;
            var localPeer = _host?.HostPeerId ?? _client?.PeerId ?? 0UL;
            if (_lobby.Players.Any(player => player.PeerId != localPeer &&
                (!_remoteCharacters.TryGetValue(player.PeerId, out var character) || character == 0U))) return;
            if (_worldSyncEnabledForSession && GetWorldBusLifecycleStateLocked() != WorldBusLifecycleState.Active) return;
            start = current; host = _host; client = _client;
            release = _sessionReleaseAuthorized;
            if (release)
            {
                if (_sessionReleaseRunning) return;
                _sessionReleaseRunning = true;
            }
            else
            {
                if (_sessionPreparedSending || (_sessionPreparedSent && DateTimeOffset.UtcNow < _nextPreparedReportUtc)) return;
                _nextPreparedReportUtc = DateTimeOffset.UtcNow.AddSeconds(1);
                _sessionPreparedSending = true;
            }
        }
        try
        {
            if (release)
            {
                await SetSessionLoadingHoldAsync(start, false, _lifetime.Token).ConfigureAwait(false);
                await SendSessionLoadingAsync(start, false, "Ready").ConfigureAwait(false);
                lock (_stateGate)
                    if (_loadingHoldStart?.SessionId == start.SessionId && _loadingHoldStart.LevelGeneration == start.LevelGeneration)
                        _loadingHoldStart = null;
                _diagnostics.Log("Preparation", "All players and world systems are ready. Multiplayer released.");
            }
            else
            {
                if (host is not null) await host.SetHostSessionPreparedAsync(start.LevelGeneration, _lifetime.Token).ConfigureAwait(false);
                else if (client is not null) await client.SendSessionPreparedAsync(start.LevelGeneration, _lifetime.Token).ConfigureAwait(false);
                lock (_stateGate)
                    if (_start?.SessionId == start.SessionId && _start.LevelGeneration == start.LevelGeneration) _sessionPreparedSent = true;
            }
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        { if (!_lifetime.IsCancellationRequested) _diagnostics.Log("Preparation", $"Session remains held: {exception.Message}"); }
        finally
        {
            lock (_stateGate) { _sessionPreparedSending = false; _sessionReleaseRunning = false; }
        }
    }

    private async Task ShowPreparationFailureAsync(string error)
    {
        LobbyStartPayload? held;
        lock (_stateGate) held = _loadingHoldStart;
        if (held is null) return;
        var bounded = error.Length <= 240 ? error : error[..240];
        await SendSessionLoadingAsync(held, true,
            "Preparation could not complete: " + bounded + "\nUse Disconnect in Pause or leave from the launcher.").ConfigureAwait(false);
    }

    private async Task ReleaseSessionLoadingBestEffortAsync()
    {
        LobbyStartPayload? held;
        lock (_stateGate)
        {
            held = _loadingHoldStart;
            _preflightCancellation?.Cancel();
        }
        if (held is null) return;
        try
        {
            if (_controller.Snapshot.IpcConnected)
                await SetSessionLoadingHoldAsync(held, false, CancellationToken.None).ConfigureAwait(false);
            await SendSessionLoadingAsync(held, false, "Disconnected").ConfigureAwait(false);
            lock (_stateGate) if (ReferenceEquals(_loadingHoldStart, held)) _loadingHoldStart = null;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        { _diagnostics.Log("Preparation", $"Preparation cleanup: {exception.Message}"); }
    }

    private void EnsureManifestLoadContext(LobbyStartPayload start, int generation, int processId = 0)
    {
        lock (_stateGate)
            if (_start?.SessionId != start.SessionId || _start.LevelGeneration != start.LevelGeneration
                || _controller.Snapshot.LevelGeneration != generation
                || (processId != 0 && _controller.Snapshot.ProcessId != processId))
                throw new OperationCanceledException("Manifest registration belongs to an obsolete level.");
    }

    private async Task EnsureNativeManifestLoadedAsync(PreparedWorldManifest prepared, LobbyStartPayload start, int generation)
    {
        var processId = _controller.Snapshot.ProcessId;
        await _nativeManifestGate.WaitAsync(_lifetime.Token).ConfigureAwait(false);
        try
        {
            EnsureManifestLoadContext(start, generation, processId);
            var key = (start.SessionId, start.LevelGeneration, generation, processId, prepared.BinarySha256);
            if (_loadedManifest == key) return;
            while (true)
            {
                EnsureManifestLoadContext(start, generation, processId);
                // ExecuteCommandAsync creates a fresh request ID on every poll.
                var result = await _controller.ExecuteCommandAsync(GameCommandNames.WorldManifestLoad,
                    new Dictionary<string, string>(StringComparer.Ordinal) { ["manifestPath"] = prepared.Path },
                    _lifetime.Token).ConfigureAwait(false);
                EnsureManifestLoadContext(start, generation, processId);
                if (result.Executed) { _loadedManifest = key; return; }
                if (result.StatusCode != "WORLD_MANIFEST_PREPARING")
                    throw new InvalidOperationException($"WORLD_MANIFEST_LOAD failed: {result.StatusCode} - {result.Message}");
                await Task.Delay(75, _lifetime.Token).ConfigureAwait(false);
            }
        }
        finally { _nativeManifestGate.Release(); }
    }

    private async Task PreloadCurrentManifestAsync(NativeRuntimeSnapshot runtime)
    {
        Task<PreparedWorldManifest?>? preparation;
        LobbyStartPayload? start;
        lock (_stateGate)
        {
            if (_manifestPreloadRunning || DateTimeOffset.UtcNow < _nextManifestPreloadUtc || !_worldSyncEnabledForSession || !_uiGameplayLoaded
                || !runtime.IpcConnected || !runtime.EngineThreadReady || runtime.LevelGeneration <= 0) return;
            preparation = _automaticWorldManifestPreparation; start = _start;
            if (start is null || preparation is null) return;
            if (_loadedManifest is { } loaded && loaded.Session == start.SessionId
                && loaded.Start == start.LevelGeneration && loaded.Native == runtime.LevelGeneration
                && loaded.Process == runtime.ProcessId) return;
            _manifestPreloadRunning = true;
        }
        try
        {
            var prepared = await preparation.ConfigureAwait(false);
            if (prepared is not null) await EnsureNativeManifestLoadedAsync(prepared, start, runtime.LevelGeneration).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            lock (_stateGate) _nextManifestPreloadUtc = DateTimeOffset.UtcNow.AddSeconds(1);
            if (!_lifetime.IsCancellationRequested && exception is not OperationCanceledException)
            {
                _diagnostics.Log("Preparation", exception.Message);
                await ShowPreparationFailureAsync(exception.Message).ConfigureAwait(false);
            }
        }
        finally { lock (_stateGate) _manifestPreloadRunning = false; }
    }
}
