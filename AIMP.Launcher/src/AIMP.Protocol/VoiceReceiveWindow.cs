namespace AIMP.Protocol;

// Fixed memory, no buffered audio. Call only after checking current round and ownership.
internal sealed class VoiceReceiveWindow
{
    private readonly long[] _acceptedAt = new long[60];
    private int _generation, _next, _count;
    private uint _sequence;

    public bool TryAccept(int generation, uint sequence, long now)
    {
        if (generation < 1 || generation < _generation || sequence == 0) return false;
        if (generation != _generation)
        {
            _generation = generation;
            _sequence = 0;
            _next = _count = 0;
        }
        // TCP is ordered: permit gaps, but never permit a huge jump to poison
        // an established stream. A generation change permits a fresh counter.
        if (_sequence != 0 && (sequence <= _sequence || sequence - _sequence > 3000)) return false;
        if (_count == 60 && now - _acceptedAt[_next] < 1000) return false;
        _sequence = sequence;
        _acceptedAt[_next] = now;
        _next = (_next + 1) % 60;
        _count = Math.Min(60, _count + 1);
        return true;
    }
}
