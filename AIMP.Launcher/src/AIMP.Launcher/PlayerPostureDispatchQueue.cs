using AIMP.Protocol;

namespace AIMP.Launcher;

// Position is replaceable; posture/torch edges are ordered together. Null fields
// are unavailable samples, so retain each field independently. Bound memory during a
// stalled engine: on overflow discard the oldest edge, retaining the newest
// state (especially completion). Normal entry/exit pairs remain separate.
internal sealed class PlayerPostureDispatchQueue
{
    internal const int Capacity = 32;
    private readonly Queue<(PlayerPostureSnapshot? Posture, PlayerTorchSnapshot? Torch)> _pending = new();
    private PlayerPostureSnapshot? _latest;
    private PlayerTorchSnapshot? _latestTorch;
    internal int Count => _pending.Count;
    internal PlayerPostureSnapshot? Next => _pending.TryPeek(out var value) ? value.Posture : _latest;
    internal PlayerTorchSnapshot? NextTorch => _pending.TryPeek(out var value) ? value.Torch : _latestTorch;
    internal bool HasPending => _pending.Count != 0;

    internal void Observe(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null)
    {
        var posture = value ?? _latest;
        torch ??= _latestTorch;
        if (posture == _latest && torch == _latestTorch) return;
        _latest = posture;
        _latestTorch = torch;
        if (_pending.Count == Capacity) _pending.Dequeue();
        _pending.Enqueue((posture, torch));
    }

    internal void Bind(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null)
    {
        _pending.Clear();
        _latest = null;
        _latestTorch = null;
        Observe(value, torch);
    }

    internal void Complete(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null)
    {
        if (_pending.TryPeek(out var next) && next == (value, torch)) _pending.Dequeue();
    }
}
