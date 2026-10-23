using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    private readonly record struct ThrowableDelivery(
        Guid Session, int Generation, int Process, int NativeGeneration,
        LobbyHost? Host, LobbyClient? Client, byte[] Payload, bool ToNative);
    private readonly Queue<ThrowableDelivery> _throwableDeliveries = new();
    private bool _throwableSending;

    private Task ConfigureThrowableSyncAsync(Guid session, ulong peer, bool active)
    {
        int generation;
        lock (_stateGate)
        {
            generation = _start?.LevelGeneration ?? 0;
            active = active && _start is not null && LevelGameMode(_start.Level) == "Survival";
        }
        return _controller.SendThrowableAsync(MessageType.ThrowableConfigure,
            ThrowableSyncCodec.Configuration(session, _controller.Snapshot.LevelGeneration,
                generation, peer, active), _lifetime.Token);
    }

    private void OnNativeThrowableProduced(object? sender, ReadOnlyMemory<byte> value) =>
        EnqueueThrowable(null, value, false);
    private void OnRemoteThrowableProposed(object? sender, ReadOnlyMemory<byte> value) =>
        EnqueueThrowable(sender, value, true);
    private void OnRemoteThrowableCommitted(object? sender, ReadOnlyMemory<byte> value) =>
        EnqueueThrowable(sender, value, true);

    private void EnqueueThrowable(object? sender, ReadOnlyMemory<byte> value, bool toNative)
    {
        ThrowableSyncCodec.Validate(value.Span);
        lock (_stateGate)
        {
            if (!ThrowableAdmissibleLocked(sender, value.Span, toNative)) return;
            if (_throwableDeliveries.Count >= 256)
            {
                _diagnostics.Log("Throwables", "Bounded release queue is full; native item was not replicated.");
                return;
            }
            var runtime = _controller.Snapshot;
            _throwableDeliveries.Enqueue(new(_start!.SessionId, _start.LevelGeneration,
                runtime.ProcessId, runtime.LevelGeneration, _host, _client, value.ToArray(), toNative));
            if (!_throwableSending && (_throwableSending = true)) _ = SendThrowablesAsync();
        }
    }

    // Caller holds _stateGate.
    private bool ThrowableAdmissibleLocked(object? sender, ReadOnlySpan<byte> value, bool toNative) =>
        _start is not null && LevelGameMode(_start.Level) == "Survival"
        && (_host is not null || _client is not null)
        && (!toNative || IsCurrentLobbySource(sender, _start.SessionId))
        && ThrowableSyncCodec.Cookie(value) == AISyncCodec.SessionCookie(_start.SessionId)
        && ThrowableSyncCodec.Generation(value) == _start.LevelGeneration
        && (toNative || ThrowableSyncCodec.NativeGeneration(value) == _controller.Snapshot.LevelGeneration);

    private async Task SendThrowablesAsync()
    {
        while (NextThrowableDelivery() is { } delivery)
        {
            try { await DeliverThrowableAsync(delivery).ConfigureAwait(false); }
            catch (Exception exception) when (exception is IOException or ObjectDisposedException
                or InvalidOperationException or ProtocolException or OperationCanceledException)
            {
                _diagnostics.Log("Throwables", $"Release/lifetime delivery failed: {exception.Message}");
            }
        }
    }

    // Drops deliveries whose session, lobby transport or game process changed.
    private ThrowableDelivery? NextThrowableDelivery()
    {
        lock (_stateGate)
        {
            while (_throwableDeliveries.TryDequeue(out var delivery))
            {
                var runtime = _controller.Snapshot;
                if (_start?.SessionId == delivery.Session && _start.LevelGeneration == delivery.Generation
                    && ReferenceEquals(delivery.Host, _host) && ReferenceEquals(delivery.Client, _client)
                    && runtime.ProcessId == delivery.Process && runtime.LevelGeneration == delivery.NativeGeneration)
                    return delivery;
            }
            _throwableSending = false;
            return null;
        }
    }

    private Task DeliverThrowableAsync(ThrowableDelivery delivery)
    {
        if (delivery.ToNative)
            return _controller.SendThrowableAsync(MessageType.ThrowableEvent, delivery.Payload, _lifetime.Token);
        if (delivery.Host is not null)
            return delivery.Host.CommitThrowableAsync(delivery.Payload, _lifetime.Token);
        return ThrowableSyncCodec.Operation(delivery.Payload) == 1
            ? delivery.Client!.SendThrowableProposalAsync(delivery.Payload, _lifetime.Token)
            : Task.CompletedTask;
    }
}
