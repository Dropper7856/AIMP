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
        ResetCinematicSync();
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
        var mode = LevelGameMode(start.Level);
        var campaign = mode == "Campaign";
        var survival = mode == "Survival";
        var enabled = campaign || survival;
        var (role, epoch) = MissionRole(enabled);
        IReadOnlyList<MissionSyncFrontier> roots = enabled
            ? MissionSyncFrontierCompiler.Compile(prepared.Manifest, survival) : [];
        var catalog = enabled ? MissionSyncFrontierCompiler.Encode(prepared.Manifest, roots) : [];
        LobbyHost? missionHost;
        lock (_stateGate) missionHost = _host;
        missionHost?.SetMissionRepeatableFrontiers(roots.Where(root => root.Repeatable)
            .Select(root => (root.Source.EntityId.ToString(), root.Signal)), expectedGeneration: start.LevelGeneration);
        missionHost?.SetMissionFrontierCatalog(start.LevelGeneration, roots.SelectMany(root =>
            (root.ScheduledInput ? new byte[] { 4 } : root.PublicInput ? new byte[] { 3 } : new byte[] { 1, 2 })
                .Select(selector => (root.Source.EntityId.ToString(), root.Signal, selector, root.OrderedOccurrences))));
        var path = enabled ? Path.Combine(Path.GetTempPath(), $"aimp-mission-{Guid.NewGuid():N}.bin") : "";
        try
        {
            if (enabled) await File.WriteAllBytesAsync(path, catalog, _lifetime.Token).ConfigureAwait(false);
            await SendMissionConfigurationAsync(start, generation, epoch, path, role).ConfigureAwait(false);
            CompleteMissionConfiguration(epoch, enabled, catalog);
            // A session-scoped witness for offline comparison of existing
            // Fix3.28 native state snapshots. No gameplay or packet changes.
            if (enabled)
                _diagnostics.Log("TestLabContext", MissionTestLabReceipts.FormatSessionContext(
                    start.SessionId, generation, role));
            LogMissionConfiguration(role, generation, prepared, roots, enabled, mode);
        }
        finally { if (path.Length != 0) File.Delete(path); }
    }

    private (string Role, long Epoch) MissionRole(bool enabled)
    {
        lock (_stateGate) return (enabled ? (_host is not null ? "host" : "client") : "off", _missionEpoch);
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

    private void CompleteMissionConfiguration(long epoch, bool enabled, byte[] catalog)
    {
        lock (_stateGate)
        {
            if (epoch != _missionEpoch) throw new OperationCanceledException("MissionSync generation changed.");
            _missionCatalogSha256 = enabled ? Convert.ToHexString(SHA256.HashData(catalog)) : "";
            _missionConfigured.TrySetResult(enabled);
        }
    }

    private void LogMissionConfiguration(string role, int generation, PreparedWorldManifest prepared,
        IReadOnlyList<MissionSyncFrontier> roots, bool enabled, string mode)
    {
        _diagnostics.Log("MissionSync", $"Configured role={role} mode={mode} nativeGeneration={generation} " +
            $"map={prepared.Manifest.MapContentId} roots={roots.Count} sha256={_missionCatalogSha256}.");
        // Bounded per-round evidence lets us correlate the compiler's admitted
        // static signals with the actual native output identities. The prior
        // diagnostics reported roots=5 but zero captured events on both PCs.
        foreach (var root in roots.Take(32))
            _diagnostics.Log("MissionSync",
                $"Frontier entity={root.Source.EntityId} output=0x{root.Signal:X8} " +
                $"runtimeKey={root.Source.CathodeEntityA:X8}:{root.Source.CathodeEntityB:X8} " +
                $"sourceComposite=0x{root.Source.SourceCompositeShortGuid:X8} " +
                $"primitive={root.Source.PrimitiveName} kind={root.Source.Kind} domain={root.Source.Domain} " +
                $"payloadTolerant={root.PayloadTolerant} publicInput={root.PublicInput} orderedOccurrences={root.OrderedOccurrences}.");
        if (enabled && roots.Count == 0)
            _diagnostics.Log("MissionSync", $"No safe {mode} objective frontier was proven by the " +
                "manifest. Raw player interactions are NOT replayed as mission commits.");
    }

    private void OnNativeMissionSignalProduced(object? sender, NativeMissionSignalPayload value)
    {
        lock (_stateGate)
        {
            if (_missionFaulted || _start is null || _missionNativeGeneration != value.LevelGeneration
                || LevelGameMode(_start.Level) is not ("Campaign" or "Survival")
                || (_host is null && _client is null)) return;
            var signal = new MissionSignalPayload(_start.SessionId, _host?.HostPeerId ?? _client!.PeerId,
                _start.LevelGeneration, value.Sequence, value.EntityId, value.Signal, value.InterfaceSelector, value.Occurrence);
            if (!_missionReports.Writer.TryWrite((_missionEpoch, _host, _client, signal)))
                FailMissionSyncLocked("MissionSync local report queue is full; progression stopped.");
            else LogMissionTestLabReceipt("native-produced", signal);
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
            else LogMissionTestLabReceipt("commit-observed", value);
        }
    }

    private bool MissionCurrent(long epoch, MissionSignalPayload signal)
    {
        lock (_stateGate) return !_missionFaulted && epoch == _missionEpoch
            && _start?.SessionId == signal.SessionId && _start.LevelGeneration == signal.LevelGeneration;
    }

    // Diagnostics only: a hashed, peer-joinable receipt is not proof of a
    // CATHODE side effect. Never let TestLab become a gameplay authority.
    private void LogMissionTestLabReceipt(string stage, MissionSignalPayload signal)
    {
        var role = _host is not null ? "host" : _client is not null ? "client" : null;
        if (role is not null)
            _diagnostics.Log("TestLabReceipt", MissionTestLabReceipts.Format(signal, role, stage));
    }

    private void FailMissionSyncLocked(string reason)
    {
        var first = !_missionFaulted;
        _missionFaulted = true;
        _diagnostics.Log("MissionSync", reason);
        if (first) _ = DisableNativeMissionSyncAfterFaultAsync(_missionEpoch, _missionNativeGeneration);
    }

    // Native MissionSync suppresses each captured mission output until its Host
    // commit is applied. Once faulted, no commit will ever arrive: keep it
    // armed and the campaign soft-locks (black screen at the BSP_TORRENS
    // opening in build 491). Fail open: the mission graph runs locally again.
    private async Task DisableNativeMissionSyncAfterFaultAsync(long epoch, int nativeGeneration)
    {
        try
        {
            if (nativeGeneration <= 0) return;
            var result = await _controller.ExecuteCommandAsync("MISSION_SYNC_CONFIG",
                new Dictionary<string, string> { ["path"] = "", ["role"] = "off" },
                _lifetime.Token, waitForExecution: true, expectedLevelGeneration: nativeGeneration)
                .WaitAsync(TimeSpan.FromSeconds(20), _lifetime.Token).ConfigureAwait(false);
            lock (_stateGate)
                if (epoch == _missionEpoch)
                    _diagnostics.Log("MissionSync", result.Executed
                        ? "Native MissionSync disabled after the fault; missions now run locally on this machine."
                        : $"Native MissionSync could not be disabled after the fault: {result.StatusCode} {result.Message}");
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _diagnostics.Log("MissionSync", "Native MissionSync could not be disabled after the fault: " + exception.Message);
        }
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
        LogMissionTestLabReceipt("apply-attempted", signal);
        await ExecuteMissionApplyAsync(signal, nativeGeneration).ConfigureAwait(false);
        LogMissionTestLabReceipt("native-command-executed", signal);
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
            ["sequence"] = signal.Sequence.ToString(CultureInfo.InvariantCulture),
            ["occurrence"] = signal.Occurrence.ToString(CultureInfo.InvariantCulture)
        }, _lifetime.Token, waitForExecution: true, expectedLevelGeneration: nativeGeneration)
            .WaitAsync(TimeSpan.FromSeconds(20), _lifetime.Token).ConfigureAwait(false);
        if (!result.Executed) throw new InvalidOperationException($"{result.StatusCode}: {result.Message}");
    }
}
