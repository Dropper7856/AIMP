namespace AIMP.Launcher;

internal enum VoiceStat
{
    Captured, Unfocused, PushToTalkIdle, BelowThreshold, NetworkSent, NetworkDropped,
    Received, ForwardedToGame, GamePipeBusy
}

// Lock-free pipeline counters; one consumer reports the changed totals.
internal sealed class VoiceSyncStats
{
    private static readonly VoiceStat[] Stats = Enum.GetValues<VoiceStat>();
    private readonly long[] _values = new long[Stats.Length];
    private string _reported = string.Empty;

    internal void Add(VoiceStat stat) => Interlocked.Increment(ref _values[(int)stat]);

    internal string? TakeChanged()
    {
        var text = string.Join(", ", Stats.Select(stat => $"{stat}={Interlocked.Read(ref _values[(int)stat])}"));
        if (text == _reported) return null;
        _reported = text;
        return text;
    }
}
