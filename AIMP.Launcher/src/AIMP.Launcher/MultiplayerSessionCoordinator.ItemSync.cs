using AIMP.Protocol;

namespace AIMP.Launcher;

// ItemSync: the local player's world pickups are reported to the Host, which
// keeps the first collector of each WorldEntityId for the generation and
// broadcasts ItemCollected. Every participant except the collector removes the
// pickup natively (ITEM_SYNC_APPLY). Late clients receive the Host's full set.
internal sealed partial class MultiplayerSessionCoordinator
{
    private readonly record struct ItemPickupReport(LobbyHost? Host, LobbyClient? Client, int Generation);

    private void OnNativeItemSyncPickupProduced(
        object? sender,
        NativeItemSyncPickupPayload value) =>
        _ = ReportLocalItemPickupAsync(value);

    private async Task ReportLocalItemPickupAsync(NativeItemSyncPickupPayload value)
    {
        if (CaptureItemPickupReport() is not { } report) return;
        _diagnostics.Log("ItemSync",
            $"Local pickup #{value.Sequence} {value.EntityId} reported for generation {report.Generation}.");
        try
        {
            if (report.Host is not null)
                await report.Host.ReportHostItemPickupAsync(report.Generation, value.EntityId, _lifetime.Token)
                    .ConfigureAwait(false);
            else
                await report.Client!.SendItemPickupAsync(report.Generation, value.EntityId, _lifetime.Token)
                    .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or InvalidOperationException or ProtocolException or OperationCanceledException)
        {
            _diagnostics.Log("ItemSync", $"Local pickup report failed: {exception.Message}");
        }
    }

    private ItemPickupReport? CaptureItemPickupReport()
    {
        lock (_stateGate)
        {
            if (_start is null || (_host is null && _client is null)) return null;
            return new ItemPickupReport(_host, _client, _start.LevelGeneration);
        }
    }

    private void OnItemCollected(object? sender, ItemCollectedPayload value)
    {
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, value.SessionId)
                || _start is null
                || _start.LevelGeneration != value.LevelGeneration) return;
            var localPeerId = _host?.HostPeerId ?? _client?.PeerId ?? 0;
            if (value.CollectorPeerId == localPeerId) return;
        }

        _ = ApplyRemoteItemCollectedAsync(value);
    }

    private async Task ApplyRemoteItemCollectedAsync(ItemCollectedPayload value)
    {
        try
        {
            // The native side keeps the id until the pickup is resolvable
            // (zone streamed in), so a queued result is final for the Launcher.
            var result = await _controller.ExecuteCommandAsync(GameCommandNames.ItemSyncApply,
                new Dictionary<string, string>(StringComparer.Ordinal) { ["entityId"] = value.EntityId },
                _lifetime.Token).ConfigureAwait(false);
            if (result.Executed || NativeRuntimeController.IsQueuedResult(result))
            {
                _diagnostics.Log("ItemSync",
                    $"Pickup {value.EntityId} collected by participant {value.CollectorPeerId} " +
                    $"removed locally: {result.StatusCode} - {result.Message}");
                return;
            }

            _diagnostics.Log("ItemSync",
                $"Pickup {value.EntityId} removal was rejected: {result.StatusCode} - {result.Message}");
        }
        catch (Exception exception)
        {
            _diagnostics.Log("ItemSync", $"Pickup {value.EntityId} removal failed: {exception.Message}");
        }
    }
}
