using AIMP.Protocol;

namespace AIMP.Launcher;

// ItemSync: the local player's world pickups are reported to the Host, which
// keeps the first collector of each item key for the generation and broadcasts
// ItemCollected. Every participant except the collector removes the pickup
// natively (ITEM_SYNC_APPLY). Random GCIP spawns and player drops go through
// the Host too (ItemSpawnDecision -> ITEM_SYNC_SPAWN), so every participant has
// the same pickups. Late clients receive the Host's decisions, then collections.
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

    private void OnNativeItemSyncSpawnProduced(
        object? sender,
        NativeItemSyncSpawnPayload value) =>
        _ = ReportLocalItemSpawnAsync(value);

    // GCIP spawn results are proposals: every peer, the Host included, waits
    // for the Host decision. Drops are always accepted.
    private async Task ReportLocalItemSpawnAsync(NativeItemSyncSpawnPayload value)
    {
        if (CaptureItemPickupReport() is not { } report) return;
        _diagnostics.Log("ItemSync",
            $"Local {value.Kind} {value.OwnerKey} proposed: " +
            (value.Spawn ? $"{value.Path} x{value.Quantity}." : "nothing spawns."));
        try
        {
            if (report.Host is not null)
                await report.Host.ReportHostItemSpawnAsync(report.Generation, value, _lifetime.Token)
                    .ConfigureAwait(false);
            else
                await report.Client!.SendItemSpawnProposalAsync(report.Generation, value, _lifetime.Token)
                    .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException
            or InvalidOperationException or ProtocolException or OperationCanceledException)
        {
            _diagnostics.Log("ItemSync", $"Local spawn report failed: {exception.Message}");
        }
    }

    private void OnItemSpawnDecided(object? sender, ItemSpawnPayload value)
    {
        lock (_stateGate)
        {
            if (!IsCurrentLobbySource(sender, value.SessionId)
                || _start is null
                || _start.LevelGeneration != value.LevelGeneration) return;
            var localPeerId = _host?.HostPeerId ?? _client?.PeerId ?? 0;
            // The dropper already has its own pickup; owner decisions apply to
            // every peer, the proposer included.
            if (value.Kind == "drop" && value.PeerId == localPeerId) return;
        }

        _ = ApplyItemSpawnAsync(value);
    }

    private async Task ApplyItemSpawnAsync(ItemSpawnPayload value)
    {
        var result = await ExecuteItemSyncCommandAsync(GameCommandNames.ItemSyncSpawn,
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["kind"] = value.Kind,
                ["ownerKey"] = value.OwnerKey,
                ["spawn"] = value.Spawn ? "1" : "0",
                ["path"] = value.Path,
                ["quantity"] = value.Quantity.ToString(System.Globalization.CultureInfo.InvariantCulture),
                ["transform"] = ModelValidation.FormatItemTransform(value.Transform)
            },
            value.LevelGeneration).ConfigureAwait(false);
        _diagnostics.Log("ItemSync",
            $"Spawn decision {value.Kind} {value.OwnerKey} from participant {value.PeerId}: " +
            (result is null ? "abandoned (round changed)." : $"{result.StatusCode} - {result.Message}"));
    }

    private const int ItemSyncCommandAttempts = 30;

    // The level can still be loading or switching generation when a decision
    // arrives (EXPIRED, ITEM_SYNC_UNAVAILABLE...). Retry while the round lasts;
    // only an explicit native rejection or fault is final.
    private async Task<GameCommandResultPayload?> ExecuteItemSyncCommandAsync(
        string command,
        Dictionary<string, string> arguments,
        int levelGeneration)
    {
        GameCommandResultPayload? result = null;
        for (var attempt = 0; attempt < ItemSyncCommandAttempts; attempt++)
        {
            lock (_stateGate)
                if (_start is null || _start.LevelGeneration != levelGeneration) return null;
            try
            {
                result = await _controller.ExecuteCommandAsync(command, arguments, _lifetime.Token,
                    waitForExecution: true).ConfigureAwait(false);
                if (result.Executed
                    || result.StatusCode is "ITEM_SYNC_REJECTED" or "ITEM_SYNC_NATIVE_FAULT")
                    return result;
            }
            catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
            {
                return null;
            }
            catch (Exception exception) when (exception is IOException or ObjectDisposedException
                or InvalidOperationException or ProtocolException or TimeoutException)
            {
                _diagnostics.Log("ItemSync", $"{command} attempt {attempt + 1} failed: {exception.Message}");
            }

            try { await Task.Delay(1000, _lifetime.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) { return null; }
        }

        return result;
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
        // The native side keeps the id until the pickup is resolvable (zone
        // streamed in, Host decision spawned), so a deferred result is final.
        var result = await ExecuteItemSyncCommandAsync(GameCommandNames.ItemSyncApply,
            new Dictionary<string, string>(StringComparer.Ordinal) { ["entityId"] = value.EntityId },
            value.LevelGeneration).ConfigureAwait(false);
        _diagnostics.Log("ItemSync",
            $"Pickup {value.EntityId} collected by participant {value.CollectorPeerId}: " +
            (result is null ? "abandoned (round changed)." : $"{result.StatusCode} - {result.Message}"));
    }
}
