namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    public event EventHandler<RoundProgressPayload>? RoundProgressReceived;

    public Task SendMissionCompleteAsync(
        int levelGeneration,
        CancellationToken cancellationToken = default)
    {
        var payload = new PlayerMissionCompletePayload(SessionId, PeerId, levelGeneration);
        ModelValidation.Validate(payload);
        return _connection.SendAsync(
            MessageType.PlayerMissionComplete,
            JsonPayload.Serialize(payload),
            cancellationToken).AsTask();
    }

    private void HandleRoundProgress(ReadOnlyMemory<byte> bytes)
    {
        var progress = JsonPayload.Deserialize<RoundProgressPayload>(bytes.Span);
        ModelValidation.Validate(progress);
        if (progress.SessionId != SessionId)
            throw new ProtocolException("The round-progress SessionId is invalid.");
        RoundProgressReceived?.Invoke(this, progress);
    }
}
