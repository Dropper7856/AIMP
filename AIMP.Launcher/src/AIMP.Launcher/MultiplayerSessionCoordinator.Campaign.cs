using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private bool _campaignAdvanceRunning;
    private DateTimeOffset _nextCampaignAdvanceUtc;
    private ulong _campaignObservedCharacterSerial;

    private static string CampaignLevelName(string path)
    {
        var parts = path.Replace('/', '\\').Split('\\', StringSplitOptions.RemoveEmptyEntries);
        // Native levels are full engine paths. Only accept a single campaign
        // map under Production; DLC/frontend is not a campaign continuation.
        if (parts.Length != 2 || !parts[0].Equals("Production", StringComparison.OrdinalIgnoreCase)
            || parts[1].Equals("Frontend", StringComparison.OrdinalIgnoreCase)
            || parts[1].Any(value => !char.IsAsciiLetterOrDigit(value) && value is not ('_' or '-'))) return "";
        return parts[1];
    }

    private void TryScheduleCampaignContinuation(NativeRuntimeSnapshot runtime)
    {
        LobbyHost host; LobbyStartPayload previous; string next;
        lock (_stateGate)
        {
            if (_start is not null && LevelGameMode(_start.Level) == "Campaign"
                && CampaignLevelName(runtime.CurrentLevelName).Equals(_start.Level, StringComparison.OrdinalIgnoreCase)
                && !runtime.LevelTransitioning && runtime.LocalCharacterSerial != 0)
                _campaignObservedCharacterSerial = runtime.LocalCharacterSerial;
            if (_host is null || _start is null || LevelGameMode(_start.Level) != "Campaign"
                || _campaignAdvanceRunning || DateTimeOffset.UtcNow < _nextCampaignAdvanceUtc
                || _missionNativeGeneration == 0 || !runtime.IpcConnected || !runtime.EngineThreadReady
                || runtime.LevelTransitioning || _campaignObservedCharacterSerial == 0
                || runtime.LocalCharacterSerial == 0 || runtime.LocalCharacterSerial == _campaignObservedCharacterSerial) return;
            next = CampaignLevelName(runtime.CurrentLevelName);
            if (next.Length == 0 || next.Equals(_start.Level, StringComparison.OrdinalIgnoreCase)) return;
            host = _host; previous = _start;
            _campaignAdvanceRunning = true;
        }
        _ = AdvanceCampaignAsync(host, previous, next);
    }

    private async Task AdvanceCampaignAsync(LobbyHost host, LobbyStartPayload previous, string next)
    {
        try
        {
            // Verify that the selected retail destination resolves to this
            // installation before changing the shared network epoch.
            var game = ResolveRuntimeGameDirectory(_controller.Snapshot.ExecutablePath)
                ?? _settingsStore.Load().GameDirectory;
            WorldManifestAutoResolver.ResolveLevelSource(game, next, includeCampaign: true);
            await host.ContinueCampaignAsync(next, previous.LevelGeneration, _lifetime.Token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("MissionSync", $"Campaign transition to {next} failed: {exception.Message}");
            lock (_stateGate) _nextCampaignAdvanceUtc = DateTimeOffset.UtcNow.AddSeconds(5);
        }
        finally { lock (_stateGate) _campaignAdvanceRunning = false; }
    }

    private async Task ApplyCampaignContinuationAsync(LobbyStartPayload next)
    {
        LobbyStartPayload previous;
        ulong previousCharacterSerial;
        lock (_stateGate)
        {
            if (_start is null || next.SessionId != _start.SessionId
                || next.LevelGeneration <= _start.LevelGeneration) return;
            if (LevelGameMode(_start.Level) != "Campaign" || LevelGameMode(next.Level) != "Campaign"
                || next.LevelGeneration != _start.LevelGeneration + 1)
                throw new ProtocolException("Invalid campaign continuation epoch.");
            previous = _start;
            previousCharacterSerial = _campaignObservedCharacterSerial;
        }
        _diagnostics.Log("MissionSync", $"Campaign continuing {previous.Level} -> {next.Level}, " +
            $"generation={next.LevelGeneration}, previousCommit={next.PreviousMissionSequence}.");
        // LobbyStart can overtake the asynchronous local apply worker. Never
        // clear its transition commit before this PC has executed it.
        var deadline = Environment.TickCount64 + 120_000;
        while (true)
        {
            _lifetime.Token.ThrowIfCancellationRequested();
            bool drained;
            lock (_stateGate)
            {
                if (_start?.SessionId != previous.SessionId || _start.LevelGeneration != previous.LevelGeneration)
                    throw new OperationCanceledException("Campaign session changed.");
                if (_missionFaulted) throw new InvalidOperationException("MissionSync faulted before the level transition.");
                drained = _missionLastApplied >= next.PreviousMissionSequence;
            }
            var runtime = _controller.Snapshot;
            if (drained && runtime.IpcConnected && runtime.EngineThreadReady && !runtime.LevelTransitioning
                && previousCharacterSerial != 0 && runtime.LocalCharacterSerial != 0
                && runtime.LocalCharacterSerial != previousCharacterSerial
                && CampaignLevelName(runtime.CurrentLevelName).Equals(next.Level, StringComparison.OrdinalIgnoreCase)) break;
            if (Environment.TickCount64 >= deadline)
                throw new TimeoutException("The retail campaign transition did not reach the Host's selected level.");
            await Task.Delay(100, _lifetime.Token).ConfigureAwait(false);
        }

        LobbyHost? host; LobbyClient? client;
        lock (_stateGate)
        {
            ResetGameplayState(next);
            _worldSyncEnabledForSession = _performanceIsolation.IsEnabled(PerformanceIsolationModule.WorldSync);
            host = _host; client = _client;
            if (_gamePreparation is null) throw new InvalidOperationException("Campaign preparation is unavailable.");
            _automaticWorldManifestPreparation = PrepareCampaignManifestAsync(next.Level);
        }
        await BeginNativeRoundAsync(next).ConfigureAwait(false);
        var peer = host?.HostPeerId ?? client?.PeerId ?? 0;
        await SetNativeWorldSyncLifecycleAsync(next.SessionId, peer, host is not null,
            next.LevelGeneration, true, false, "campaign-continuation").ConfigureAwait(false);
        await SetSessionLoadingHoldAsync(next, true, _lifetime.Token).ConfigureAwait(false);
        lock (_stateGate)
        {
            _loadingHoldStart = next;
            _worldQuiescenceWaitSinceUtc = null;
            _loadingHoldStartedUtc = DateTimeOffset.UtcNow;
            _nextPreparationProgressUtc = DateTimeOffset.MinValue;
            _loadingOverlayAcknowledged = false; _loadingOverlayConnection = Guid.Empty;
            _sessionPreparedSending = _sessionPreparedSent = _sessionReleaseAuthorized = _sessionReleaseRunning = false;
        }
        BeginRemoteEquipmentCatalogPreparation(next);
        // The new movie may have announced gameplay before the network epoch
        // arrived; do not wait for an event that only fires once per load.
        MarkGameplayUiLoaded(DateTimeOffset.UtcNow);
        await SendSessionLoadingAsync(next, true, "Synchronizing campaign...").ConfigureAwait(false);
        PublishState();
    }

    private async Task<PreparedWorldManifest?> PrepareCampaignManifestAsync(string level) =>
        await _gamePreparation!.PrepareLevelAsync(level, _lifetime.Token, allowCampaign: true).ConfigureAwait(false);
}
