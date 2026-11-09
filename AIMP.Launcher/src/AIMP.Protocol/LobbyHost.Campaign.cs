namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    // The retail SwitchLevel already executes from the mission commit on each
    // PC. This boundary changes only the network epoch/preparation; it never
    // starts a new game or replaces the engine's checkpoint/spawn selection.
    public async Task ContinueCampaignAsync(string level, int previousGeneration,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(level) || level.Length > 256
            || level.Any(value => !char.IsAsciiLetterOrDigit(value) && value is not ('_' or '-')))
            throw new ArgumentException("Invalid retail campaign level.", nameof(level));
        await _missionCommitGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            LobbyStartPayload next;
            lock (_stateLock)
            {
                if (_state != "Starting" || _levelGeneration != previousGeneration
                    || _options.AlienVsSurvivors || string.Equals(_level, level, StringComparison.OrdinalIgnoreCase)) return;
                _level = level;
                var previousMissionSequence = _missionCommitSequence;
                ++_levelGeneration;
                ResetMissionCatalogLocked();
                _sessionPreparedPeers.Clear(); _sessionReleasedGeneration = 0;
                _deadPeers.Clear(); _finishedPeers.Clear(); _collectedItems.Clear();
                ClearSpawnDecisionsLocked();
                next = new LobbyStartPayload(SessionId, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                    _level, _levelGeneration, _options.AlienEnabled, CampaignContinuation: true,
                    PreviousMissionSequence: previousMissionSequence, Campaign: _campaignLaunch);
            }
            ModelValidation.Validate(next);
            await BroadcastAsync(MessageType.LobbyStart, JsonPayload.Serialize(next), cancellationToken).ConfigureAwait(false);
            await BroadcastSnapshotAsync(cancellationToken).ConfigureAwait(false);
            PublishSnapshot();
            Started?.Invoke(this, next);
        }
        finally { _missionCommitGate.Release(); }
    }
}
