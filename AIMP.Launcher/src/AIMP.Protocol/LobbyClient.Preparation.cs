namespace AIMP.Protocol;

public sealed partial class LobbyClient
{
    public event EventHandler<LobbyPreparationPayload>? LobbyPreparationReceived;
    public event EventHandler<SessionReleasePayload>? SessionReleased;

    public Task SendLobbyPreparationAsync(
        LobbyPreparationPayload preparation, CancellationToken cancellationToken = default)
    {
        ModelValidation.Validate(preparation);
        if (preparation.SessionId != SessionId || preparation.PeerId != PeerId)
            throw new ProtocolException("Local lobby preparation identity is invalid.");
        return _connection.SendAsync(MessageType.LobbyPreparation,
            JsonPayload.Serialize(preparation), cancellationToken).AsTask();
    }

    public Task SendSessionPreparedAsync(
        int levelGeneration, CancellationToken cancellationToken = default, string missionSha256 = "")
    {
        var preparation = new SessionPreparedPayload(SessionId, PeerId, levelGeneration, missionSha256);
        ModelValidation.Validate(preparation);
        return _connection.SendAsync(MessageType.SessionPrepared,
            JsonPayload.Serialize(preparation), cancellationToken).AsTask();
    }
}
