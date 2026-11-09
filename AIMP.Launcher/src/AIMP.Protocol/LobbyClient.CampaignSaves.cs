namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    private readonly object _campaignDownloadLock = new();
    private CampaignDownload? _campaignDownload;
    private bool _campaignConnectionEnded;

    private sealed class CampaignDownload(CampaignLaunchDescriptor descriptor)
    {
        public CampaignLaunchDescriptor Descriptor { get; } = descriptor;
        public Guid RequestId { get; } = Guid.NewGuid();
        public byte[] Bytes { get; } = new byte[checked((int)descriptor.ByteLength)];
        public int Offset { get; set; }
        public TaskCompletionSource<byte[]> Completion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    // Cancellation stops waiting; already queued chunks are still validated/drained.
    // Only one transfer may be active until its final chunk or connection closure.
    public async Task<byte[]> DownloadCampaignSaveAsync(CampaignLaunchDescriptor descriptor,
        CancellationToken cancellationToken = default)
    {
        CampaignSaveValidation.Validate(descriptor);
        if (descriptor.Action != "continue") throw new ArgumentException("A continue descriptor is required.", nameof(descriptor));
        cancellationToken.ThrowIfCancellationRequested();
        CampaignDownload download;
        lock (_campaignDownloadLock)
        {
            if (_campaignConnectionEnded || Volatile.Read(ref _disposing) != 0)
                throw new IOException("The lobby connection has ended.");
            if (Snapshot?.Campaign != descriptor)
                throw new ProtocolException("The download descriptor is not the lobby's selected save.");
            if (_campaignDownload is not null) throw new InvalidOperationException("A campaign download is already active.");
            _campaignDownload = download = new CampaignDownload(descriptor);
        }
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _shutdown.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            try
            {
                await _connection.SendAsync(MessageType.CampaignSaveRequest,
                    JsonPayload.Serialize(new CampaignSaveRequest(SessionId, descriptor.SaveId!, download.RequestId)), timeout.Token).ConfigureAwait(false);
            }
            catch
            {
                // Cancellation mid-frame can leave the stream partially written.
                _connection.Client.Dispose();
                AbortCampaignDownload();
                throw;
            }
            return await download.Completion.Task.WaitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (Exception error) when (error is IOException or System.Net.Sockets.SocketException or ObjectDisposedException)
        {
            AbortCampaignDownload();
            throw;
        }
    }

    private void HandleCampaignSaveChunk(ReadOnlyMemory<byte> payload)
    {
        // Bound the JSON/base64 envelope before decoding or allocating chunk data.
        if (payload.Length > CampaignSaveValidation.ChunkBytes * 2)
            throw new ProtocolException("Campaign save chunk envelope is too large.");
        var chunk = JsonPayload.Deserialize<CampaignSaveChunk>(payload.Span);
        lock (_campaignDownloadLock)
        {
            var download = _campaignDownload;
            if (download is null || chunk.SessionId != SessionId || chunk.SaveId != download.Descriptor.SaveId
                || chunk.RequestId != download.RequestId || chunk.Offset != download.Offset || chunk.Data is null
                || chunk.Data.Length != Math.Min(CampaignSaveValidation.ChunkBytes, download.Bytes.Length - download.Offset))
                throw new ProtocolException("Invalid campaign save chunk identity, offset or size.");
            chunk.Data.CopyTo(download.Bytes, download.Offset);
            download.Offset += chunk.Data.Length;
            if (download.Offset == download.Bytes.Length)
            {
                CampaignSaveValidation.ValidateBundle(download.Descriptor, download.Bytes);
                _campaignDownload = null;
                download.Completion.TrySetResult(download.Bytes);
            }
        }
    }

    private void AbortCampaignDownload()
    {
        lock (_campaignDownloadLock)
        {
            _campaignConnectionEnded = true;
            _campaignDownload?.Completion.TrySetException(new IOException("Lobby disconnected during campaign save download."));
            _campaignDownload = null;
        }
    }
}
