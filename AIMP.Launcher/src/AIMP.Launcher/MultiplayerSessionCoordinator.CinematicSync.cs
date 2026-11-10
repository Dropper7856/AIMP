using AIMP.Protocol;

namespace AIMP.Launcher;

// Cinematic authority is separate from objective progression. MissionSync
// configures the native Host/Client role and its verified game-import hook;
// only a Host-only network decision can grant a temporary remote Enter hold.
internal sealed partial class MultiplayerSessionCoordinator
{
    private bool _cinematicPollRunning, _cinematicProbeWarningLogged;
    private DateTimeOffset _nextCinematicPollUtc, _nextCinematicNoticeUtc;
    private ulong _cinematicNextSequence, _cinematicLastApplied;
    private string _cinematicLastState = "";

    // Called with _stateGate when a campaign level/mission generation changes.
    private void ResetCinematicSync()
    {
        _cinematicPollRunning = false;
        _cinematicProbeWarningLogged = false;
        _nextCinematicPollUtc = DateTimeOffset.MinValue;
        _nextCinematicNoticeUtc = DateTimeOffset.MinValue;
        _cinematicNextSequence = 0UL;
        _cinematicLastApplied = 0UL;
        _cinematicLastState = "";
    }

    private void TryScheduleCinematicPoll(NativeRuntimeSnapshot runtime, DateTimeOffset now)
    {
        LobbyStartPayload start;
        lock (_stateGate)
        {
            if (_cinematicPollRunning || now < _nextCinematicPollUtc ||
                _start is not { } current || LevelGameMode(current.Level)!="Campaign" ||
                (_host is null && _client is null) ||
                !runtime.IpcConnected || !runtime.EngineThreadReady ||
                _missionNativeGeneration != runtime.LevelGeneration ||
                !_missionConfigured.Task.IsCompletedSuccessfully) return;
            start=current;
            _cinematicPollRunning=true;
            // 5 Hz provides prompt Host decision while avoiding per-frame IPC
            // commands and permitting the input firewall's 1.2-second lease.
            _nextCinematicPollUtc=now.AddMilliseconds(200);
        }
        _ = PollCinematicAsync(start,runtime.LevelGeneration);
    }

    private async Task PollCinematicAsync(LobbyStartPayload start,int nativeGeneration)
    {
        try
        {
            var result=await _controller.ExecuteCommandAsync(
                GameCommandNames.CinematicSyncStatus,
                new Dictionary<string,string>(),
                _lifetime.Token,waitForExecution:true,
                expectedLevelGeneration:nativeGeneration)
                .WaitAsync(TimeSpan.FromSeconds(3),_lifetime.Token).ConfigureAwait(false);
            if (!result.Executed)
            {
                lock (_stateGate)
                    if (!_cinematicProbeWarningLogged && _start?.SessionId==start.SessionId)
                    {
                        _cinematicProbeWarningLogged=true;
                        _diagnostics.Log("CinematicSync",
                            "Native campaign input control is unavailable: "+result.StatusCode+" "+result.Message);
                    }
                return;
            }
            var state=result.StatusCode is "CINEMATIC_SYNC_IDLE" ? "idle" : "cinematic";
            bool changed;
            lock (_stateGate)
            {
                changed=_cinematicLastState!=state;
                _cinematicLastState=state;
            }
            if (changed)
                _diagnostics.Log("CinematicSync", state=="cinematic"
                    ? "Cinematic camera detected; only the Host can skip it (hold Enter)."
                    : "Gameplay camera restored.");
            if (result.StatusCode=="CINEMATIC_SYNC_HOST_SKIP")
            {
                LobbyHost? host;
                CinematicSkipPayload? decision=null;
                lock (_stateGate)
                {
                    if (_start?.SessionId==start.SessionId &&
                        _start.LevelGeneration==start.LevelGeneration && _host is not null)
                    {
                        host=_host;
                        decision=new CinematicSkipPayload(start.SessionId,start.LevelGeneration,
                            ++_cinematicNextSequence);
                    }
                    else host=null;
                }
                if (decision is not null && host is not null)
                {
                    await host.PublishHostCinematicSkipAsync(decision,_lifetime.Token).ConfigureAwait(false);
                    _diagnostics.Log("CinematicSync",
                        "Host skip committed for campaign generation "+decision.LevelGeneration+
                        " sequence="+decision.Sequence+".");
                }
            }
            else if (result.StatusCode=="CINEMATIC_SYNC_CLIENT_BLOCKED")
            {
                DateTimeOffset now=DateTimeOffset.UtcNow;
                bool notify;
                lock (_stateGate)
                {
                    notify=_start?.SessionId==start.SessionId && _client is not null &&
                        now>=_nextCinematicNoticeUtc;
                    if (notify) _nextCinematicNoticeUtc=now.AddSeconds(3);
                }
                if (notify) await ShowCinematicHostOnlyNoticeAsync().ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            lock (_stateGate)
                if (!_cinematicProbeWarningLogged && _start?.SessionId==start.SessionId)
                {
                    _cinematicProbeWarningLogged=true;
                    _diagnostics.Log("CinematicSync","Cinematic input observation failed: "+exception.Message);
                }
        }
        finally
        {
            lock (_stateGate) _cinematicPollRunning=false;
        }
    }

    private async Task ShowCinematicHostOnlyNoticeAsync()
    {
        const string message="Seul l'hôte peut passer la cinématique.";
        _diagnostics.Log("CinematicSync","Client Enter blocked. Only the Host may skip.");
        try
        {
            await SendStatusAsync(message).ConfigureAwait(false);
            await _controller.ExecuteCommandAsync(
                GameCommandNames.PopupMessage,
                new Dictionary<string,string>
                {
                    ["headerText"]="AIMP",
                    ["mainText"]=message,
                    ["duration"]="3.0",
                    ["icon"]=PopupMessageIcons.Alert,
                    ["soundEvent"]=PopupMessageSoundEvents.ObjectiveNew
                },_lifetime.Token,waitForExecution:true).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("CinematicSync",
                "The Host-only skip notice could not be displayed in-game: "+exception.Message);
        }
    }

    private void OnCinematicSkipCommitted(object? sender,CinematicSkipPayload value)
    {
        lock (_stateGate)
        {
            if (_client is null || !ReferenceEquals(sender,_client) ||
                _start?.SessionId!=value.SessionId ||
                _start.LevelGeneration!=value.LevelGeneration ||
                LevelGameMode(_start.Level)!="Campaign" ||
                value.Sequence<=_cinematicLastApplied) return;
            _cinematicLastApplied=value.Sequence;
        }
        _ = ApplyHostCinematicSkipAsync(value);
    }

    private async Task ApplyHostCinematicSkipAsync(CinematicSkipPayload value)
    {
        try
        {
            // A remote Host event may precede the local cinematic camera by a
            // few engine frames. Retry briefly, never outside its generation.
            for (var attempt=0;attempt<6;++attempt)
            {
                NativeRuntimeSnapshot runtime=_controller.Snapshot;
                lock (_stateGate)
                    if (_client is null || _start?.SessionId!=value.SessionId ||
                        _start.LevelGeneration!=value.LevelGeneration) return;
                if (runtime.IpcConnected && runtime.EngineThreadReady && runtime.LevelGeneration>0)
                {
                    var result=await _controller.ExecuteCommandAsync(
                        GameCommandNames.CinematicSyncSkip,
                        new Dictionary<string,string>(),
                        _lifetime.Token,waitForExecution:true,
                        expectedLevelGeneration:runtime.LevelGeneration)
                        .WaitAsync(TimeSpan.FromSeconds(3),_lifetime.Token).ConfigureAwait(false);
                    if (result.Executed)
                    {
                        _diagnostics.Log("CinematicSync",
                            "Host cinematic skip applied by native input pipeline, sequence="+value.Sequence+".");
                        return;
                    }
                }
                await Task.Delay(200,_lifetime.Token).ConfigureAwait(false);
            }
            _diagnostics.Log("CinematicSync",
                "Host skip was received but the local native cinematic camera was not ready, sequence="+
                value.Sequence+".");
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("CinematicSync","Host skip failed: "+exception.Message);
        }
    }
}
