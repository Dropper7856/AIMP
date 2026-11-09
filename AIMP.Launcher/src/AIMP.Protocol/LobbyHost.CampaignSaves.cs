namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private CampaignLaunchDescriptor? _campaignLaunch;
    private byte[]? _campaignSaveBundle;

    public void SetCampaignSaveBundle(byte[] bytes) => SetCampaignSaveBundle(
        _campaignLaunch ?? throw new InvalidOperationException("Lobby options must select a campaign save first."), bytes);

    // Call before Start: clients/preflight can never observe a mutable selection.
    // The defensive copy is private and is never returned to the caller.
    public void SetCampaignSaveBundle(CampaignLaunchDescriptor descriptor, byte[] bytes)
    {
        ArgumentNullException.ThrowIfNull(bytes);
        var frozen = bytes.ToArray();
        CampaignSaveValidation.ValidateBundle(descriptor, frozen);
        lock (_stateLock)
        {
            if (_acceptLoop is not null || !_clients.IsEmpty || _state != "Lobby")
                throw new InvalidOperationException("Campaign saves must be selected before starting the lobby.");
            if (_campaignSaveBundle is not null)
                throw new InvalidOperationException("The campaign save selection is already frozen.");
            if (_campaignLaunch is not null && _campaignLaunch != descriptor)
                throw new InvalidOperationException("The save descriptor differs from the lobby options.");
            _campaignLaunch = descriptor;
            _campaignSaveBundle = frozen;
        }
    }

    private void QueueCampaignSaveDownload(ClientState state, ReadOnlyMemory<byte> payload)
    {
        if (payload.Length > 1024) throw new ProtocolException("Campaign save request is too large.");
        var request = JsonPayload.Deserialize<CampaignSaveRequest>(payload.Span);
        byte[] bytes;
        lock (_stateLock)
        {
            if (request.SessionId != SessionId || request.RequestId == Guid.Empty
                || _campaignLaunch is not { Action: "continue" } selected || selected.SaveId != request.SaveId
                || _campaignSaveBundle is null || !_clients.TryGetValue(state.PeerId, out var authenticated)
                || !ReferenceEquals(authenticated, state))
                throw new ProtocolException("Campaign save request does not identify the selected authenticated session save.");
            bytes = _campaignSaveBundle;
        }
        if (Interlocked.CompareExchange(ref state.CampaignSaveSending, 1, 0) != 0)
            throw new ProtocolException("A campaign save download is already active.");
        // Task.Run ensures serialization and socket writes never occupy the read loop.
        _ = Task.Run(async () =>
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            try
            {
                for (var offset = 0; offset < bytes.Length; offset += CampaignSaveValidation.ChunkBytes)
                {
                    if (!_clients.TryGetValue(state.PeerId, out var current) || !ReferenceEquals(current, state)) return;
                    var count = Math.Min(CampaignSaveValidation.ChunkBytes, bytes.Length - offset);
                    var chunk = new CampaignSaveChunk(SessionId, request.SaveId, request.RequestId,
                        offset, bytes.AsSpan(offset, count).ToArray());
                    await state.Connection.SendAsync(MessageType.CampaignSaveChunk,
                        JsonPayload.Serialize(chunk), timeout.Token).ConfigureAwait(false);
                }
            }
            catch (Exception error) when (error is IOException or System.Net.Sockets.SocketException
                or ObjectDisposedException or OperationCanceledException or ProtocolException)
            {
                Log?.Invoke(this, $"Campaign save download ended: {error.Message}");
                state.Connection.Client.Dispose();
            }
            finally { Interlocked.Exchange(ref state.CampaignSaveSending, 0); }
        });
    }
}
