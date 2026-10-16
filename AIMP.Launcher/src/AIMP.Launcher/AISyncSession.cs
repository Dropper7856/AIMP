using AIMP.Protocol;

namespace AIMP.Launcher;

// One serialized sender per direction; one latest motion snapshot per actor
// and a bounded, ordered queue for native results, shots and short requests.
// Socket/pipe backpressure never creates an unbounded Task or packet queue.
internal sealed class AISyncSession : IDisposable
{
    private bool Enabled => _performanceIsolation.IsEnabled(PerformanceIsolationModule.AISync);
    private readonly NativeRuntimeController _controller;
    private readonly LauncherDiagnostics _diagnostics;
    private readonly PerformanceIsolationState _performanceIsolation;
    private readonly object _gate = new();
    private readonly SemaphoreSlim _configureGate = new(1, 1);
    private readonly Dictionary<ulong, ReadOnlyMemory<byte>> _pending = new();
    private readonly Queue<ReadOnlyMemory<byte>> _outcomes = new();
    private readonly Queue<ReadOnlyMemory<byte>> _impacts = new();
    private readonly AISyncEventHistory _eventHistory = new();
    private readonly Dictionary<ulong, uint> _participants = new();
    private ulong _localPeer;
    private byte[]? _lastConfiguration;
    private int _configurationProcessId;
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
    private bool _active, _sending, _impactSending, _disposed;
    private bool _availabilityWarningReported;
    private long _nativeProduced, _remoteReceived, _networkSent, _nativeSent, _lastTrafficLogTick;
    private long _impactNativeProduced, _impactRemoteReceived, _impactNetworkSent, _impactNativeSent;

    internal AISyncSession(NativeRuntimeController controller, PerformanceIsolationState performanceIsolation)
    {
        _controller = controller; _diagnostics = controller.Diagnostics;
        _performanceIsolation = performanceIsolation;
        _controller.AISyncStateProduced += OnNativeState;
        _controller.AISyncImpactProduced += OnNativeImpact;
    }
    internal void BindHost(LobbyHost host)
    {
        LobbyHost? previous;
        lock (_gate) { previous = _host; _host = host; }
        if (previous is not null && !ReferenceEquals(previous, host)) previous.AISyncImpactReceived -= OnRemoteImpact;
        host.AISyncImpactReceived -= OnRemoteImpact;
        host.AISyncImpactReceived += OnRemoteImpact;
    }
    internal void UnbindHost(LobbyHost host)
    {
        host.AISyncImpactReceived -= OnRemoteImpact;
        lock (_gate) if (ReferenceEquals(_host, host)) { _host = null; _bound = false; InvalidateLocked(); }
    }
    internal void BindClient(LobbyClient client)
    {
        lock (_gate) _client = client;
        client.AISyncStateReceived += OnRemoteState;
    }
    internal void UnbindClient(LobbyClient client)
    {
        client.AISyncStateReceived -= OnRemoteState;
        lock (_gate) if (ReferenceEquals(_client, client)) { _client = null; _bound = false; InvalidateLocked(); }
    }
    private void InvalidateLocked()
    {
        _active = false; ++_epoch; _pending.Clear(); _outcomes.Clear(); _impacts.Clear(); _eventHistory.Clear();
    }

    internal async Task ConfigureAsync(Guid session, bool isHost, int nativeGeneration,
        int worldGeneration, bool bound, bool compatible, ulong localPeer, CancellationToken token)
    {
        await _configureGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            lock (_gate)
            {
                _session = session; _worldGeneration = worldGeneration;
                _nativeGeneration = nativeGeneration;
                _isHost = isHost; _bound = bound; _compatible = compatible;
                _localPeer = localPeer;
            }
            await ApplyConfigurationAsync(Enabled, token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            lock (_gate) InvalidateLocked();
            _diagnostics.Log("AISync", $"V2 configuration refused: {exception.Message}");
        }
        finally { _configureGate.Release(); }
    }

    internal void SetParticipant(ulong peer, uint character)
    {
        lock (_gate)
        {
            if (_disposed || peer == 0) return;
            if (character == 0) { if (!_participants.Remove(peer)) return; }
            else
            {
                if (_participants.TryGetValue(peer, out var old) && old == character) return;
                if (!_participants.ContainsKey(peer) && _participants.Count >= AISyncCodec.MaximumParticipants) return;
                _participants[peer] = character;
            }
            ++_participantRevision;
            ScheduleParticipantRefreshLocked();
        }
    }
    internal void ClearParticipants()
    {
        lock (_gate)
        {
            if (_participants.Count == 0) return;
            _participants.Clear(); ++_participantRevision;
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
        lock (_gate)
        {
            session = _session; nativeGeneration = _nativeGeneration; worldGeneration = _worldGeneration;
            bound = _bound; compatible = _compatible;
            role = !enabled || !bound ? 0U : _isHost ? 1U : compatible ? 2U : 3U;
            if (enabled && bound && (worldGeneration <= 0 || nativeGeneration <= 0))
                role = _isHost ? 0U : 3U;
            configuration = AISyncCodec.Configuration(role, Math.Max(nativeGeneration, 0),
                Math.Max(worldGeneration, 0), session, _localPeer, _participants);
            var snapshot = _controller.Snapshot;
            processId = snapshot.ProcessId;
            var desiredActive = enabled && bound && compatible && (role is 1 or 2) && !_disposed;
            if (snapshot.IpcConnected && _controller.AISyncAvailable &&
                _configurationProcessId == processId && _lastConfiguration is not null &&
                _active == desiredActive && configuration.AsSpan().SequenceEqual(_lastConfiguration)) return;
            // Only role/session/generation changes invalidate queued NPC states.
            // A new participant binding must not interrupt an active attack.
            if (!snapshot.IpcConnected || !_controller.AISyncAvailable ||
                (_active && !desiredActive) || _configurationProcessId != processId || _lastConfiguration is null ||
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
                configuration, token).ConfigureAwait(false);
            lock (_gate)
            {
                if (!_controller.Snapshot.IpcConnected || _controller.Snapshot.ProcessId != processId)
                {
                    _lastConfiguration = null;
                    InvalidateLocked();
                    return;
                }
                _lastConfiguration = configuration;
                _configurationProcessId = processId;
                _active = enabled && bound && compatible && (role is 1 or 2)
                    && epoch == _epoch && !_disposed;
            }
        }
        bool active;
        lock (_gate) active = _active;
        _diagnostics.Log("AISync", $"V2 enabled={(enabled ? 1 : 0)} role={role} active={(active ? 1 : 0)} " +
            $"nativeGeneration={nativeGeneration} worldGeneration={worldGeneration}; " +
            "Host AI intent + certified Character execution replay + Host-native paired/weapon consequences; animation/audio and CATHODE presentation run locally; control=Performance Isolation.");
    }
    private void OnNativeState(object? sender, ReadOnlyMemory<byte> bytes)
    {
        lock (_gate)
        {
            if (!Enabled || !_active || _host is null || !AISyncCodec.BelongsTo(bytes.Span, _session, _worldGeneration)) return;
            ++_nativeProduced;
            QueueLocked(bytes);
        }
    }
    private void OnRemoteState(object? sender, ReadOnlyMemory<byte> bytes)
    {
        lock (_gate)
        {
            if (!Enabled || !_active || !ReferenceEquals(sender, _client) ||
                !AISyncCodec.BelongsTo(bytes.Span, _session, _worldGeneration)) return;
            ++_remoteReceived;
            QueueLocked(bytes);
        }
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
    private void QueueLocked(ReadOnlyMemory<byte> bytes)
    {
        var key = AISyncCodec.ActorKey(bytes.Span);
        if (!_pending.ContainsKey(key) && _pending.Count >= AISyncCodec.MaximumActors) return;
        if (_eventHistory.Observe(bytes.Span) && !QueueEventLocked(bytes)) return;
        RememberLatestLocked(key, bytes);
        if (_sending) return;
        _sending = true;
        // Never execute a synchronous continuation/native event under _gate.
        _ = Task.Run(PumpAsync);
    }
    private bool QueueEventLocked(ReadOnlyMemory<byte> bytes)
    {
        if (_outcomes.Count >= 1024)
        {
            InvalidateLocked();
            _diagnostics.Log("AISync", "Native consequence queue exceeded 1024 events; authority transport paused.");
            return false;
        }
        _outcomes.Enqueue(bytes);
        return true;
    }
    private void RememberLatestLocked(ulong key, ReadOnlyMemory<byte> bytes)
    {
        var revision = AISyncCodec.ActorRevision(bytes.Span);
        if (!_pending.TryGetValue(key, out var pending)
            || AISyncCodec.ActorRevision(pending.Span).CompareTo(revision) <= 0)
            _pending[key] = bytes;
    }

    private async Task PumpAsync()
    {
        try
        {
            while (!_shutdown.IsCancellationRequested)
            {
                ReadOnlyMemory<byte> bytes;
                LobbyHost? host;
                Guid session;
                long epoch;
                lock (_gate)
                {
                    if (!Enabled || !_active || (_pending.Count == 0 && _outcomes.Count == 0)) return;
                    if (_outcomes.Count != 0) bytes = _outcomes.Dequeue();
                    else
                    {
                        var next = _pending.First();
                        bytes = next.Value; _pending.Remove(next.Key);
                    }
                    host = _host; session = _session; epoch = _epoch;
                }
                lock (_gate) if (!Enabled || !_active || epoch != _epoch) continue;
                if (host is not null)
                {
                    await host.PublishHostAISyncAsync(AISyncCodec.EncodeNetwork(session, bytes.Span),
                        _shutdown.Token).ConfigureAwait(false);
                    lock (_gate) ++_networkSent;
                }
                else
                {
                    await _controller.SendAISyncStateAsync(bytes, _shutdown.Token).ConfigureAwait(false);
                    lock (_gate) ++_nativeSent;
                }
                LogTrafficIfDue();
            }
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            lock (_gate) InvalidateLocked();
            _diagnostics.Log("AISync", $"V2 transport paused: {exception.Message}");
            // The native 1.5s deadline cancels movement but keeps a Client passive.
        }
        catch (OperationCanceledException) { }
        finally
        {
            lock (_gate)
            {
                _sending = false;
                if (Enabled && _active && (_pending.Count != 0 || _outcomes.Count != 0) && !_disposed)
                { _sending = true; _ = Task.Run(PumpAsync); }
            }
        }
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
        long produced, received, networkSent, nativeSent, impactProduced, impactReceived, impactNetworkSent, impactNativeSent;
        bool isHost;
        lock (_gate)
        {
            var now = Environment.TickCount64;
            if (_lastTrafficLogTick != 0 && now - _lastTrafficLogTick < 5000) return;
            _lastTrafficLogTick = now;
            produced = _nativeProduced; received = _remoteReceived;
            networkSent = _networkSent; nativeSent = _nativeSent; isHost = _isHost;
            impactProduced = _impactNativeProduced; impactReceived = _impactRemoteReceived;
            impactNetworkSent = _impactNetworkSent; impactNativeSent = _impactNativeSent;
        }
        _diagnostics.Log("AISync", $"V2 transport role={(isHost ? "Host" : "Client")} " +
            $"nativeProduced={produced} remoteReceived={received} networkSent={networkSent} nativeSent={nativeSent} " +
            $"impactProduced={impactProduced} impactReceived={impactReceived} impactNetworkSent={impactNetworkSent} impactNativeSent={impactNativeSent}; cumulative, at most every 5s.");
    }
    public void Dispose()
    {
        _controller.AISyncStateProduced -= OnNativeState;
        _controller.AISyncImpactProduced -= OnNativeImpact;
        lock (_gate)
        {
            _disposed = true; InvalidateLocked();
            if (_host is not null) _host.AISyncImpactReceived -= OnRemoteImpact;
            if (_client is not null) _client.AISyncStateReceived -= OnRemoteState;
        }
        _shutdown.Cancel();
        // Ongoing send/configure continuations still own these small primitives.
    }
}
