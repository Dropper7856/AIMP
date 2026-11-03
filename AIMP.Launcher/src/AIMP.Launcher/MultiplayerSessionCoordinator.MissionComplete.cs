using AIMP.Protocol;
using System.Globalization;

namespace AIMP.Launcher;

// Mission completion has two generic sources: native PlayerLifecycle suppresses
// a retail level exit (GoToFrontend/SwitchLevel, every map and the campaign),
// and observes the native terminal HUD trigger, which every DLC
// SurvivorUILogic invokes on LevelComplete, before attempting Flash dispatch.
// The HUD callback remains a fallback. The earlier native trigger matters because outside
// a frontend playlist the retail results branch (indexed by "Playlist Order No")
// never runs, so no level exit is ever reached. The finished player waits on a PICKUPOVERLAY screen while
// the Host counts finished and dead participants; RoundEnded(MissionComplete)
// then reuses the game-over return to the lobby.
internal sealed partial class MultiplayerSessionCoordinator
{
    private ulong _nativeDeathEdge;
    private ulong _nativeLevelExitEdge;
    private int _missionCompleteReportedGeneration;
    private RoundProgressPayload? _roundProgress;

    // Native sequences are process-cumulative and published only on change.
    // A decrease means AI.exe restarted; its counters begin again at zero.
    private (bool Died, bool Finished) AcceptLifecycleEdges(NativePlayerLifecycleStatePayload value)
    {
        lock (_stateGate)
        {
            if (value.DeathSequence < _nativeDeathEdge || value.LevelExitSequence < _nativeLevelExitEdge)
                _nativeDeathEdge = _nativeLevelExitEdge = 0;
            var died = value.LocalPlayerDead && value.DeathSequence != _nativeDeathEdge;
            var finished = value.LevelExitSequence != _nativeLevelExitEdge;
            _nativeDeathEdge = value.DeathSequence;
            _nativeLevelExitEdge = value.LevelExitSequence;
            return (died, finished);
        }
    }

    private bool TryAcceptMissionResultsOpened(ScaleformBridgeEvent value)
    {
        if (!value.Message.Type.Equals("MISSION_RESULTS_OPENED", StringComparison.OrdinalIgnoreCase)
            || value.Message.Get("source") != "challenge_map_overlay") return false;
        _ = ReportLocalMissionCompleteAsync("challenge results opened");
        return true;
    }

    private async Task ReportLocalMissionCompleteAsync(string origin)
    {
        if (CaptureMissionCompleteReport() is not { } report) return;
        _diagnostics.Log("PlayerLifecycle", $"Local mission complete ({origin}) reported for " +
            $"generation {report.Generation}.");
        await ShowMissionWaitAsync(report.Generation).ConfigureAwait(false);
        try { await SendLocalMissionCompleteAsync(report).ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or InvalidOperationException or ProtocolException or OperationCanceledException)
        {
            _diagnostics.Log("PlayerLifecycle", $"Local mission-complete report failed: {exception.Message}");
        }
    }

    // One terminal report per launch generation: dead or finished, never both.
    private DeathReport? CaptureMissionCompleteReport()
    {
        lock (_stateGate)
        {
            if (_start is null || _missionCompleteReportedGeneration == _start.LevelGeneration
                || _deathReportedGeneration == _start.LevelGeneration) return null;
            if (_host is null && _client is null) return null;
            _missionCompleteReportedGeneration = _start.LevelGeneration;
            return new DeathReport(_host, _client, _start.LevelGeneration);
        }
    }

    private Task SendLocalMissionCompleteAsync(DeathReport report) => report.Host is not null
        ? report.Host.ReportHostMissionCompleteAsync(report.Generation, _lifetime.Token)
        : report.Client!.SendMissionCompleteAsync(report.Generation, _lifetime.Token);

    private void OnRoundProgress(object? sender, RoundProgressPayload value)
    {
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, value.SessionId) || _start is null
                || _start.LevelGeneration != value.LevelGeneration) return;
            _roundProgress = value;
            if (_missionCompleteReportedGeneration != value.LevelGeneration) return;
        }
        _ = ShowMissionWaitAsync(value.LevelGeneration);
    }

    private async Task ShowMissionWaitAsync(int generation)
    {
        Dictionary<string, string> values;
        lock (_stateGate)
        {
            if (_start is null || _start.LevelGeneration != generation
                || _roundEndingGeneration == generation) return;
            values = MissionWaitFields(_start.SessionId, generation, _roundProgress);
        }
        try { await _bridge.SendAsync("MISSION_WAIT", values, _lifetime.Token).ConfigureAwait(false); }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or OperationCanceledException)
        { _diagnostics.Log("PlayerLifecycle", $"Mission wait HUD deferred: {exception.Message}"); }
    }

    internal static Dictionary<string, string> MissionWaitFields(
        Guid sessionId, int generation, RoundProgressPayload? progress) => new(StringComparer.Ordinal)
    {
        ["title"] = MissionCompleteTitle,
        ["message"] = MissionWaitMessage(progress is not null && progress.LevelGeneration == generation
            ? progress : null),
        ["sessionId"] = sessionId.ToString("D"),
        ["generation"] = generation.ToString(CultureInfo.InvariantCulture)
    };

    internal static string MissionWaitMessage(RoundProgressPayload? progress) => progress is null
        ? "Waiting for the other players..."
        : $"Waiting for players: {progress.Finished}/{progress.Participants} finished"
          + (progress.Dead > 0 ? $", {progress.Dead} dead" : "");

    private void ClearMissionCompleteState()
    {
        _missionCompleteReportedGeneration = 0;
        _roundProgress = null;
    }
}
