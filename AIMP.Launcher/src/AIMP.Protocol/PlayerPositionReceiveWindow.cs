namespace AIMP.Protocol;

// One window per peer/connection. A generation change resets traversal history;
// a temporary loss of local Character may clear the key but cannot rewind it.
public sealed class PlayerPositionReceiveWindow
{
    private PlayerPositionSnapshotPayload? _latest;
    private PlayerPostureSnapshot? _traversal;

    public bool TryAccept(PlayerPositionSnapshotPayload value)
    {
        ModelValidation.Validate(value);
        var previous = _latest;
        if (previous is not null && (value.SessionId != previous.SessionId || value.PeerId != previous.PeerId
            || value.LevelGeneration < previous.LevelGeneration)) return false;
        var sameGeneration = previous?.LevelGeneration == value.LevelGeneration;
        if (sameGeneration && (value.Sequence <= previous!.Sequence || !ValidTraversal(value.Posture))) return false;
        if (!sameGeneration) _traversal = null;
        if (value.Posture is { TraversalRevision: > 0 } posture) _traversal = posture;
        else if (value.Posture is not null && _traversal is not null)
            _traversal = _traversal with { TraversalActive = false };
        _latest = value;
        return true;
    }

    private bool ValidTraversal(PlayerPostureSnapshot? value)
    {
        if (_traversal is null || value is null || value.TraversalRevision == 0) return true;
        if (value.TraversalRevision != _traversal.TraversalRevision)
            return value.TraversalRevision > _traversal.TraversalRevision;
        return value.TraversalIndex == _traversal.TraversalIndex
            && value.TraversalType == _traversal.TraversalType && value.TraversalOwner == _traversal.TraversalOwner
            && (!value.TraversalActive || _traversal.TraversalActive);
    }
}
