using AIMP.Protocol;

namespace AIMP.Launcher;

// Host-native output uses the single execution sender. Reverse Client impact
// requests remain separate input to the authoritative Host.
// Socket/pipe backpressure never creates an unbounded Task or packet queue.
internal sealed partial class AISyncSession : IDisposable
{
    private bool Enabled => _performanceIsolation.IsEnabled(PerformanceIsolationModule.AISync);
    private readonly NativeRuntimeController _controller;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly PerformanceIsolationState _performanceIsolation;
    private readonly AISyncExecutionBridge _transactionExecution;
    private readonly AISyncHostBridge _transactionHost;
    private readonly object _gate = new();
    private readonly SemaphoreSlim _configureGate = new(1, 1);
    private readonly Queue<ReadOnlyMemory<byte>> _impacts = new();
    private readonly Dictionary<ulong, uint> _participants = new();
    private ulong _localPeer;
    private byte[]? _lastConfiguration;
    private int _configurationProcessId;
    private Guid _configurationConnectionIdentity;
    private long _participantRevision;
    private bool _participantRefreshRunning;
    private readonly CancellationTokenSource _shutdown = new();
    private LobbyHost? _host;
    private LobbyClient? _client;
    private Guid _session;
    private int _worldGeneration;
    private int _nativeGeneration;
    private long _epoch;
    private bool _isHost, _bound, _compatible;
    private bool _alienEnabled = true;
    private bool _active, _impactSending, _disposed;
    private bool _availabilityWarningReported;
    private long _lastTrafficLogTick;
    private long _impactNativeProduced, _impactRemoteReceived, _impactNetworkSent, _impactNativeSent;

    internal AISyncSession(NativeRuntimeController controller, PerformanceIsolationState performanceIsolation)
    {
        _controller = controller; _diagnostics = controller.Diagnostics;
        _performanceIsolation = performanceIsolation;
        _transactionExecution = new AISyncExecutionBridge(CaptureExecutionBinding,
            controller.SendAISyncExecutionAsync, text => _diagnostics.Log("AISyncExecution", text));
        _transactionHost = new AISyncHostBridge(CaptureHostBinding,
            controller.SendAISyncExecutionAsync, text => _diagnostics.Log("AISyncHost", text));
        _controller.AISyncImpactProduced += OnNativeImpact;
        _controller.AISyncExecutionProduced += OnNativeExecution;
    }
    internal void BindHost(LobbyHost host)
    {
        LobbyHost? previous;
        lock (_gate) { previous = _host; _host = host; }
        if (previous is not null && !ReferenceEquals(previous, host)) previous.AISyncImpactReceived -= OnRemoteImpact;
        host.AISyncImpactReceived -= OnRemoteImpact;
        host.AISyncImpactReceived += OnRemoteImpact;
        _transactionHost.BindHost(host);
    }
    internal void UnbindHost(LobbyHost host)
    {
        host.AISyncImpactReceived -= OnRemoteImpact;
        _transactionHost.UnbindHost(host);
        lock (_gate) if (ReferenceEquals(_host, host)) { _host = null; _bound = false; InvalidateLocked(); }
    }
    internal void BindClient(LobbyClient client)
    {
        lock (_gate) _client = client;
        _transactionExecution.BindClient(client);
    }
    internal void UnbindClient(LobbyClient client)
    {
        _transactionExecution.UnbindClient(client);
        lock (_gate) if (ReferenceEquals(_client, client)) { _client = null; _bound = false; InvalidateLocked(); }
    }
    private void InvalidateLocked()
    {
        _active = false; ++_epoch; _impacts.Clear();
    }

    private void OnNativeExecution(object? sender, NativeAISyncExecution value)
    {
        if (value.Type is MessageType.AISyncHostBatch or MessageType.AISyncHostControl or MessageType.AISyncHostRoute or MessageType.AISyncHostState) _transactionHost.ObserveNative(value);
        else _transactionExecution.ObserveNative(value);
    }
    private AISyncHostBinding CaptureHostBinding()
    {
        var runtime = _controller.Snapshot; var connection = _controller.AISyncConnectionIdentity;
        lock (_gate) return new(_host, _session, _worldGeneration, _nativeGeneration,
            _configurationProcessId, connection, _epoch, Enabled && _active && _isHost && !_disposed &&
            runtime.IpcConnected && runtime.ProcessId == _configurationProcessId && connection != Guid.Empty &&
            connection == _configurationConnectionIdentity);
    }
    private AISyncExecutionBinding CaptureExecutionBinding()
    {
        var runtime = _controller.Snapshot; var connection = _controller.AISyncConnectionIdentity;
        lock (_gate) return new(_client, _session, _worldGeneration, _nativeGeneration,
            _configurationProcessId, connection, _epoch, Enabled && _active && !_isHost && !_disposed &&
            runtime.IpcConnected && runtime.ProcessId == _configurationProcessId && connection != Guid.Empty &&
            connection == _configurationConnectionIdentity);
    }

    internal async Task ConfigureAsync(Guid session, bool isHost, int nativeGeneration,
        int worldGeneration, bool bound, bool compatible, ulong localPeer, CancellationToken token,
        bool alienEnabled = true)
    {
        await _configureGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            // Record full GUIDs before any native configuration can emit data.
            // A reused 64-bit cookie never proves the new full session scope.
            lock (_gate)
            {
                if(_session!=session||_localPeer!=localPeer){_localEvidence=null;_participantEvidence.Clear();}
                _session = session; _worldGeneration = worldGeneration;
                _nativeGeneration = nativeGeneration;
                _isHost = isHost; _bound = bound; _compatible = compatible;
                _localPeer = localPeer;
                _alienEnabled = alienEnabled;
            }
            await ApplyConfigurationAsync(Enabled, token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            lock (_gate) InvalidateLocked();
            _diagnostics.Log("AISync", $"Native configuration refused: {exception.Message}");
        }
        finally { _configureGate.Release(); }
    }

    internal void SetParticipant(ulong peer, uint character,AISyncParticipantEvidence? evidence=null)
    {
        lock (_gate)
        {
            if (_disposed || peer == 0) return;
            _participantEvidence.TryGetValue(peer,out var oldEvidence);
            if(evidence is not null && evidence.Binding.Character!=character)
                throw new ProtocolException("Participant evidence differs from its stored Character.");
            if (character == 0) { if (!_participants.Remove(peer)) return; }
            else
            {
                if (_participants.TryGetValue(peer, out var old) && old == character&&oldEvidence==evidence) return;
                if (!_participants.ContainsKey(peer) && _participants.Count >= AISyncCodec.MaximumParticipants) return;
                _participants[peer] = character;
            }
            if(evidence is null)_participantEvidence.Remove(peer);else _participantEvidence[peer]=evidence;
            ++_participantRevision;
            ScheduleParticipantRefreshLocked();
        }
    }
    internal void ClearParticipants()
    {
        lock (_gate)
        {
            if (_participants.Count == 0&&_participantEvidence.Count==0&&_localEvidence is null) return;
            _participants.Clear(); _participantEvidence.Clear(); _localEvidence=null; ++_participantRevision;
            ScheduleParticipantRefreshLocked();
        }
    }
    private void ScheduleParticipantRefreshLocked()
    {
        if (_participantRefreshRunning || _disposed) return;
        _participantRefreshRunning = true;
        _ = Task.Run(RefreshParticipantsAsync);
    }
    private async Task RefreshParticipantsAsync()
    {
        var acquired = false;
        long attempted = -1;
        try
        {
            await _configureGate.WaitAsync(_shutdown.Token).ConfigureAwait(false);
            acquired = true;
            while (!_shutdown.IsCancellationRequested)
            {
                lock (_gate) attempted = _participantRevision;
                await ApplyConfigurationAsync(Enabled, _shutdown.Token).ConfigureAwait(false);
                lock (_gate) if (attempted == _participantRevision) return;
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception exception)
        {
            _diagnostics.Log("AISync", $"Participant binding update failed: {exception.Message}");
        }
        finally
        {
            if (acquired) _configureGate.Release();
            lock (_gate)
            {
                _participantRefreshRunning = false;
                if (!_disposed && attempted != _participantRevision) ScheduleParticipantRefreshLocked();
            }
        }
    }

    internal async Task SetEnabledAsync(bool enabled, CancellationToken token)
    {
        await _configureGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            // OFF submits Retail, so native requests are released on the engine
            // thread. ON reuses the latest lobby/map context, even after an OFF
            // period spanning a level change. No hook uninstall/reinstall.
            await ApplyConfigurationAsync(enabled, token).ConfigureAwait(false);
            _performanceIsolation.SetEnabled(PerformanceIsolationModule.AISync, enabled);
        }
        finally { _configureGate.Release(); }
    }

    private async Task ApplyConfigurationAsync(bool enabled, CancellationToken token)
    {
        Guid session;
        uint role;
        int nativeGeneration, worldGeneration;
        bool bound, compatible;
        long epoch;
        byte[] configuration;
        int processId;
        Guid connection;
        lock (_gate)
        {
            session = _session; nativeGeneration = _nativeGeneration; worldGeneration = _worldGeneration;
            bound = _bound; compatible = _compatible;
            role = !enabled || !bound ? 0U : _isHost ? 1U : compatible ? 2U : 3U;
            if (enabled && bound && (worldGeneration <= 0 || nativeGeneration <= 0))
                role = _isHost ? 0U : 3U;
            var snapshot = _controller.Snapshot;
            connection = _controller.AISyncConnectionIdentity;
            configuration = AISyncCodec.Configuration(role, Math.Max(nativeGeneration, 0),
                Math.Max(worldGeneration, 0), session, _localPeer, _participants, _alienEnabled,
                _localEvidence?.Matches(snapshot,connection,nativeGeneration)==true ? _localEvidence.Binding : null,
                CurrentParticipantEvidence(snapshot));
            processId = snapshot.ProcessId;
            var desiredActive = enabled && bound && compatible && (role is 1 or 2) && !_disposed;
            if (snapshot.IpcConnected && _controller.AISyncAvailable &&
                _configurationProcessId == processId && _configurationConnectionIdentity == connection && _lastConfiguration is not null &&
                _active == desiredActive && configuration.AsSpan().SequenceEqual(_lastConfiguration)) return;
            // Only role/session/generation changes invalidate queued NPC states.
            // A new participant binding must not interrupt an active attack.
            if (!snapshot.IpcConnected || !_controller.AISyncAvailable ||
                (_active && !desiredActive) || _configurationProcessId != processId ||
                _configurationConnectionIdentity != connection || _lastConfiguration is null ||
                !configuration.AsSpan(0, 32).SequenceEqual(_lastConfiguration.AsSpan(0, 32)))
                InvalidateLocked();
            epoch = _epoch;
        }
        // A menu choice made before injection is retained by the shared state.
        // The first authority binding submits it after the IPC handshake.
        if (_controller.Snapshot.IpcConnected)
        {
            if (!_controller.AISyncAvailable)
            {
                if (!_availabilityWarningReported)
                    _diagnostics.Log("AISync", "Native AISync unavailable; execution and transport remain inactive. Inspect aisync_v2_install. Other multiplayer modules keep their IPC connection.");
                _availabilityWarningReported = true;
                return;
            }
            _availabilityWarningReported = false;
            await _controller.SendAISyncConfigurationAsync(
                connection, processId, configuration, token).ConfigureAwait(false);
            lock (_gate)
            {
                if (!_controller.Snapshot.IpcConnected || _controller.Snapshot.ProcessId != processId ||
                    _controller.AISyncConnectionIdentity != connection)
                {
                    _lastConfiguration = null;
                    InvalidateLocked();
                    return;
                }
                _lastConfiguration = configuration;
                _configurationProcessId = processId;
                _configurationConnectionIdentity = connection;
                _active = enabled && bound && compatible && (role is 1 or 2)
                    && epoch == _epoch && !_disposed;
            }
        }
        bool active;
        lock (_gate) active = _active;
        _diagnostics.Log("AISync", $"Native enabled={(enabled ? 1 : 0)} role={role} active={(active ? 1 : 0)} " +
            $"nativeGeneration={nativeGeneration} worldGeneration={worldGeneration}; " +
            "Host AI intent + certified Character execution replay + Host-native paired/weapon consequences; animation/audio and CATHODE presentation run locally; control=Performance Isolation.");
    }
    private void OnNativeImpact(object? sender, ReadOnlyMemory<byte> bytes)
    {
        lock (_gate)
        {
            if (!Enabled || !_active || _isHost || _client is null
                || !AISyncCodec.ImpactBelongsTo(bytes.Span, _session, _worldGeneration)
                || AISyncCodec.ImpactSourcePeer(bytes.Span) != _localPeer) return;
            ++_impactNativeProduced;
            QueueImpactLocked(bytes);
        }
    }
    private void OnRemoteImpact(object? sender, ReadOnlyMemory<byte> bytes)
    {
        lock (_gate)
        {
            if (!Enabled || !_active || !_isHost || !ReferenceEquals(sender, _host)
                || !AISyncCodec.ImpactBelongsTo(bytes.Span, _session, _worldGeneration)
                || AISyncCodec.ImpactSourcePeer(bytes.Span) == _localPeer) return;
            ++_impactRemoteReceived;
            QueueImpactLocked(bytes);
        }
    }
    private void QueueImpactLocked(ReadOnlyMemory<byte> bytes)
    {
        if (_impacts.Count >= 256)
        {
            _diagnostics.Log("AISync", "Client-impact queue exceeded 256 events; newest impact was dropped without changing Host AI state.");
            return;
        }
        _impacts.Enqueue(bytes);
        if (_impactSending) return;
        _impactSending = true;
        _ = Task.Run(PumpImpactsAsync);
    }
    private async Task PumpImpactsAsync()
    {
        try
        {
            while (!_shutdown.IsCancellationRequested)
            {
                ReadOnlyMemory<byte> bytes;
                LobbyHost? host;
                LobbyClient? client;
                Guid session;
                long epoch;
                bool isHost;
                lock (_gate)
                {
                    if (!Enabled || !_active || _impacts.Count == 0) return;
                    bytes = _impacts.Dequeue();
                    host = _host; client = _client; session = _session; epoch = _epoch; isHost = _isHost;
                }
                lock (_gate) if (!Enabled || !_active || epoch != _epoch) continue;
                if (isHost)
                {
                    if (host is null) continue;
                    await _controller.SendAISyncImpactAsync(bytes, _shutdown.Token).ConfigureAwait(false);
                    lock (_gate) ++_impactNativeSent;
                }
                else
                {
                    if (client is null) continue;
                    await client.SendAISyncImpactAsync(AISyncCodec.EncodeImpactNetwork(session, bytes.Span),
                        _shutdown.Token).ConfigureAwait(false);
                    lock (_gate) ++_impactNetworkSent;
                }
                LogTrafficIfDue();
            }
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            _diagnostics.Log("AISync", $"Client-impact transport paused: {exception.Message}");
        }
        catch (OperationCanceledException) { }
        finally
        {
            lock (_gate)
            {
                _impactSending = false;
                if (Enabled && _active && _impacts.Count != 0 && !_disposed)
                { _impactSending = true; _ = Task.Run(PumpImpactsAsync); }
            }
        }
    }
    private void LogTrafficIfDue()
    {
        long impactProduced, impactReceived, impactNetworkSent, impactNativeSent;
        bool isHost;
        lock (_gate)
        {
            var now = Environment.TickCount64;
            if (_lastTrafficLogTick != 0 && now - _lastTrafficLogTick < 5000) return;
            _lastTrafficLogTick = now;
            isHost = _isHost;
            impactProduced = _impactNativeProduced; impactReceived = _impactRemoteReceived;
            impactNetworkSent = _impactNetworkSent; impactNativeSent = _impactNativeSent;
        }
        _diagnostics.Log("AISync", $"Native transport role={(isHost ? "Host" : "Client")} " +
            $"impactProduced={impactProduced} impactReceived={impactReceived} impactNetworkSent={impactNetworkSent} impactNativeSent={impactNativeSent}; cumulative, at most every 5s.");
    }
    public void Dispose()
    {
        _controller.AISyncImpactProduced -= OnNativeImpact;
        _controller.AISyncExecutionProduced -= OnNativeExecution;
        _transactionExecution.Dispose();
        _transactionHost.Dispose();
        lock (_gate)
        {
            _disposed = true; InvalidateLocked();
            if (_host is not null) _host.AISyncImpactReceived -= OnRemoteImpact;
        }
        _shutdown.Cancel();
        // Ongoing send/configure continuations still own these small primitives.
    }
}
