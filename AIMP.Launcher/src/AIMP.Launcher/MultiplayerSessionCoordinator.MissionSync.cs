using AIMP.Protocol;
using System.Globalization;
using System.Security.Cryptography;
using System.Threading.Channels;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private readonly Channel<(long Epoch, LobbyHost? Host, LobbyClient? Client, MissionSignalPayload Signal)> _missionReports =
        Channel.CreateBounded<(long, LobbyHost?, LobbyClient?, MissionSignalPayload)>(new BoundedChannelOptions(4096)
        { SingleReader = false, FullMode = BoundedChannelFullMode.Wait });
    private readonly Channel<(long Epoch, MissionSignalPayload Signal)> _missionCommits =
        Channel.CreateBounded<(long, MissionSignalPayload)>(new BoundedChannelOptions(4096)
        { SingleReader = false, FullMode = BoundedChannelFullMode.Wait });
    private long _missionEpoch;
    private int _missionNativeGeneration;
    private ulong _missionLastApplied;
    private bool _missionFaulted;
    private string _missionCatalogSha256 = "";
    private TaskCompletionSource<bool> _missionConfigured = NewMissionReady();
    private Task _missionReportTask = Task.CompletedTask;
    private Task _missionApplyTask = Task.CompletedTask;

    private static TaskCompletionSource<bool> NewMissionReady() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private void InitializeMissionSync()
    {
        _missionReportTask = RunMissionReportsAsync();
        _missionApplyTask = RunMissionCommitsAsync();
    }

    // Called with the coordinator state lock, including round restart in the same lobby.
    private void ResetMissionSync()
    {
        ++_missionEpoch;
        _missionNativeGeneration = 0;
        _missionLastApplied = 0;
        _missionFaulted = false;
        _missionCatalogSha256 = "";
        _missionConfigured.TrySetCanceled();
        _missionConfigured = NewMissionReady();
        while (_missionReports.Reader.TryRead(out _)) { }
        while (_missionCommits.Reader.TryRead(out _)) { }
    }

    private async Task ConfigureMissionSyncAsync(PreparedWorldManifest prepared, LobbyStartPayload start, int generation)
    {
        EnsureManifestLoadContext(start, generation);
        bool campaign = LevelGameMode(start.Level) == "Campaign";
        var (role, epoch) = MissionRole(campaign);
        IReadOnlyList<MissionSyncFrontier> roots = campaign ? MissionSyncFrontierCompiler.Compile(prepared.Manifest) : [];
        var catalog = campaign ? MissionSyncFrontierCompiler.Encode(prepared.Manifest, roots) : [];
        var path = campaign ? Path.Combine(Path.GetTempPath(), $"aimp-mission-{Guid.NewGuid():N}.bin") : "";
        try
        {
            if (campaign) await File.WriteAllBytesAsync(path, catalog, _lifetime.Token).ConfigureAwait(false);
            await SendMissionConfigurationAsync(start, generation, epoch, path, role).ConfigureAwait(false);
            CompleteMissionConfiguration(epoch, campaign, catalog);
            LogMissionConfiguration(role, generation, prepared, roots.Count, campaign);
        }
        finally { if (path.Length != 0) File.Delete(path); }
    }

    private (string Role, long Epoch) MissionRole(bool campaign)
    {
        lock (_stateGate) return (campaign ? (_host is not null ? "host" : "client") : "off", _missionEpoch);
    }

    private async Task SendMissionConfigurationAsync(LobbyStartPayload start, int generation, long epoch,
        string path, string role)
    {
        lock (_stateGate)
        {
            EnsureManifestLoadContext(start, generation);
            if (epoch != _missionEpoch) throw new OperationCanceledException("MissionSync generation changed.");
            _missionNativeGeneration = generation;
        }
        var result = await _controller.ExecuteCommandAsync("MISSION_SYNC_CONFIG",
            new Dictionary<string, string> { ["path"] = path, ["role"] = role },
            _lifetime.Token, waitForExecution: true, expectedLevelGeneration: generation)
            .WaitAsync(TimeSpan.FromSeconds(20), _lifetime.Token).ConfigureAwait(false);
        EnsureManifestLoadContext(start, generation);
        if (!result.Executed)
            throw new InvalidOperationException($"MissionSync configuration failed: {result.StatusCode} - {result.Message}");
    }

    private void CompleteMissionConfiguration(long epoch, bool campaign, byte[] catalog)
    {
        lock (_stateGate)
        {
            if (epoch != _missionEpoch) throw new OperationCanceledException("MissionSync generation changed.");
            _missionCatalogSha256 = campaign ? Convert.ToHexString(SHA256.HashData(catalog)) : "";
            _missionConfigured.TrySetResult(campaign);
        }
    }

    private void LogMissionConfiguration(string role, int generation, PreparedWorldManifest prepared, int roots, bool campaign)
    {
        _diagnostics.Log("MissionSync", $"Configured role={role} nativeGeneration={generation} " +
            $"map={prepared.Manifest.MapContentId} roots={roots} sha256={_missionCatalogSha256}.");
        if (campaign && roots == 0)
            _diagnostics.Log("MissionSync", "This map contains no supported mission completion frontier; mission progression coverage is unavailable.");
    }

    private void OnNativeMissionSignalProduced(object? sender, NativeMissionSignalPayload value)
    {
        lock (_stateGate)
        {
            if (_missionFaulted || _start is null || _missionNativeGeneration != value.LevelGeneration
                || LevelGameMode(_start.Level) != "Campaign" || (_host is null && _client is null)) return;
            var signal = new MissionSignalPayload(_start.SessionId, _host?.HostPeerId ?? _client!.PeerId,
                _start.LevelGeneration, value.Sequence, value.EntityId, value.Signal, value.InterfaceSelector);
            if (!_missionReports.Writer.TryWrite((_missionEpoch, _host, _client, signal)))
                FailMissionSyncLocked("MissionSync local report queue is full; progression stopped.");
        }
    }

    private void OnMissionSignalCommitted(object? sender, MissionSignalPayload value)
    {
        lock (_stateGate)
        {
            if (_missionFaulted || !IsCurrentLobbySource(sender, value.SessionId)
                || _start?.LevelGeneration != value.LevelGeneration) return;
            if (!_missionCommits.Writer.TryWrite((_missionEpoch, value)))
                FailMissionSyncLocked("MissionSync commit queue is full; progression stopped.");
        }
    }

    private bool MissionCurrent(long epoch, MissionSignalPayload signal)
    {
        lock (_stateGate) return !_missionFaulted && epoch == _missionEpoch
            && _start?.SessionId == signal.SessionId && _start.LevelGeneration == signal.LevelGeneration;
    }

    private void FailMissionSyncLocked(string reason)
    {
        _missionFaulted = true;
        _diagnostics.Log("MissionSync", reason);
    }

    private async Task RunMissionReportsAsync()
    {
        try
        {
            await foreach (var work in _missionReports.Reader.ReadAllAsync(_lifetime.Token).ConfigureAwait(false))
            {
                if (!MissionCurrent(work.Epoch, work.Signal)) continue;
                try
                {
                    if (work.Host is not null) await work.Host.ReportHostMissionSignalAsync(work.Signal, _lifetime.Token).ConfigureAwait(false);
                    else await work.Client!.SendMissionSignalAsync(work.Signal, _lifetime.Token).ConfigureAwait(false);
                }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                { lock (_stateGate) if (work.Epoch == _missionEpoch) FailMissionSyncLocked($"Mission report failed: {exception.Message}"); }
            }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
    }

    private async Task RunMissionCommitsAsync()
    {
        try
        {
            await foreach (var work in _missionCommits.Reader.ReadAllAsync(_lifetime.Token).ConfigureAwait(false))
            {
                try { await ApplyMissionCommitAsync(work.Epoch, work.Signal).ConfigureAwait(false); }
                catch (OperationCanceledException) when (!MissionCurrent(work.Epoch, work.Signal) || _lifetime.IsCancellationRequested) { }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                { lock (_stateGate) if (work.Epoch == _missionEpoch) FailMissionSyncLocked($"Mission commit failed: {exception.Message}"); }
            }
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
    }

    // Commits apply strictly in Host order once this machine's catalogue is configured.
    private async Task ApplyMissionCommitAsync(long epoch, MissionSignalPayload signal)
    {
        Task<bool> configured;
        lock (_stateGate)
        {
            if (!MissionCurrent(epoch, signal)) return;
            configured = _missionConfigured.Task;
        }
        if (!await configured.WaitAsync(_lifetime.Token).ConfigureAwait(false) || !MissionCurrent(epoch, signal)) return;
        if (NextMissionApplyGeneration(signal) is not { } nativeGeneration) return;
        await ExecuteMissionApplyAsync(signal, nativeGeneration).ConfigureAwait(false);
        lock (_stateGate) if (epoch == _missionEpoch) _missionLastApplied = signal.Sequence;
        _diagnostics.Log("MissionSync", $"Applied mission commit #{signal.Sequence} {signal.EntityId}/{signal.Signal:X8}.");
    }

    // Null for an already applied commit; a gap is a broken Host order.
    private int? NextMissionApplyGeneration(MissionSignalPayload signal)
    {
        lock (_stateGate)
        {
            if (signal.Sequence <= _missionLastApplied) return null;
            if (signal.Sequence != _missionLastApplied + 1)
                throw new ProtocolException("MissionSync commit sequence has a gap.");
            return _missionNativeGeneration;
        }
    }

    private async Task ExecuteMissionApplyAsync(MissionSignalPayload signal, int nativeGeneration)
    {
        var result = await _controller.ExecuteCommandAsync("MISSION_SYNC_APPLY", new Dictionary<string, string>
        {
            ["entityId"] = signal.EntityId,
            ["signal"] = signal.Signal.ToString(CultureInfo.InvariantCulture),
            ["selector"] = signal.InterfaceSelector.ToString(CultureInfo.InvariantCulture),
            ["sequence"] = signal.Sequence.ToString(CultureInfo.InvariantCulture)
        }, _lifetime.Token, waitForExecution: true, expectedLevelGeneration: nativeGeneration)
            .WaitAsync(TimeSpan.FromSeconds(20), _lifetime.Token).ConfigureAwait(false);
        if (!result.Executed) throw new InvalidOperationException($"{result.StatusCode}: {result.Message}");
    }
}
