namespace AIMP.Launcher;

internal sealed class GameplaySpawnReadinessGate
{
    // v13 no longer waits for a global all-player spawn round. Once this local
    // instance has a short, stable gameplay window, it can publish readiness and
    // converge each remote Character independently.
    private static readonly TimeSpan MinimumGameplayAge = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan MinimumPositionAge = TimeSpan.FromMilliseconds(750);
    private static readonly TimeSpan MaximumSampleGap = TimeSpan.FromMilliseconds(250);
    private const int MinimumPositionSamples = 12;
    private const float MaximumJumpSquared = 100.0F;
    private DateTimeOffset? _gameplayLoadedUtc;
    private DateTimeOffset? _positionStableSinceUtc;
    private DateTimeOffset? _lastSampleUtc;
    private LocalPlayerPosition _lastPosition;
    private int _positionSamples;

    internal void Reset()
    {
        _gameplayLoadedUtc = null;
        ResetPositionObservation();
    }

    internal void MarkGameplayUiLoaded(DateTimeOffset now)
    {
        _gameplayLoadedUtc = now;
        ResetPositionObservation();
    }

    internal void Observe(
        DateTimeOffset now,
        bool positionAvailable,
        LocalPlayerPosition position)
    {
        if (_gameplayLoadedUtc is null || !positionAvailable)
        {
            ResetPositionObservation();
            return;
        }
        if (MustRestartObservation(now, position)) StartObservation(now, position);
        else ContinueObservation(now, position);
    }

    internal bool IsReady(DateTimeOffset now) =>
        _gameplayLoadedUtc is { } loaded
        && _positionStableSinceUtc is { } stable
        && _positionSamples >= MinimumPositionSamples
        && now - loaded >= MinimumGameplayAge
        && now - stable >= MinimumPositionAge;

    private bool MustRestartObservation(DateTimeOffset now, LocalPlayerPosition position) =>
        _lastSampleUtc is null
        || now - _lastSampleUtc > MaximumSampleGap
        || HasLargeJump(_lastPosition, position);

    private void StartObservation(DateTimeOffset now, LocalPlayerPosition position)
    {
        _positionStableSinceUtc = now;
        _lastSampleUtc = now;
        _lastPosition = position;
        _positionSamples = 1;
    }

    private void ContinueObservation(DateTimeOffset now, LocalPlayerPosition position)
    {
        _lastSampleUtc = now;
        _lastPosition = position;
        _positionSamples++;
    }

    private void ResetPositionObservation()
    {
        _positionStableSinceUtc = null;
        _lastSampleUtc = null;
        _lastPosition = default;
        _positionSamples = 0;
    }

    private static bool HasLargeJump(LocalPlayerPosition before, LocalPlayerPosition after)
    {
        var x = after.X - before.X;
        var y = after.Y - before.Y;
        var z = after.Z - before.Z;
        return (x * x) + (y * y) + (z * z) > MaximumJumpSquared;
    }
}
