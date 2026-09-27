namespace AIMP.Protocol;

internal sealed class SpawnPositionRound
{
    private readonly object _gate = new();
    private readonly HashSet<ulong> _expectedPeerIds;
    private readonly Dictionary<ulong, PlayerSpawnPositionPayload> _positions = new();
    private readonly TaskCompletionSource<IReadOnlyList<PlayerSpawnPositionPayload>> _completion =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal SpawnPositionRound(
        SpawnPositionRequestPayload request,
        IEnumerable<ulong> expectedPeerIds,
        PlayerSpawnPositionPayload hostPosition)
    {
        Request = request;
        _expectedPeerIds = expectedPeerIds.ToHashSet();
        _expectedPeerIds.Add(hostPosition.PeerId);
        _positions[hostPosition.PeerId] = hostPosition;
        CompleteIfReady();
    }

    internal SpawnPositionRequestPayload Request { get; }

    internal bool TryAdd(SpawnPositionResponsePayload response)
    {
        lock (_gate)
        {
            if (!Matches(response) || !_expectedPeerIds.Contains(response.PeerId)) return false;
            _positions[response.PeerId] = ToPosition(response);
            CompleteIfReady();
            return true;
        }
    }

    internal Task<IReadOnlyList<PlayerSpawnPositionPayload>> WaitAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken) =>
        _completion.Task.WaitAsync(timeout, cancellationToken);

    private bool Matches(SpawnPositionResponsePayload response) =>
        response.SessionId == Request.SessionId && response.RequestId == Request.RequestId
        && response.LevelGeneration == Request.LevelGeneration;

    private static PlayerSpawnPositionPayload ToPosition(SpawnPositionResponsePayload value) =>
        new(value.PeerId, value.X, value.Y, value.Z);

    private void CompleteIfReady()
    {
        if (_positions.Count != _expectedPeerIds.Count) return;
        var values = _positions.Values.OrderBy(value => value.PeerId).ToArray();
        _completion.TrySetResult(values);
    }
}
