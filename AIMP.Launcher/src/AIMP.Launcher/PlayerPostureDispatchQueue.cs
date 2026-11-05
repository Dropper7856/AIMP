using AIMP.Protocol;

namespace AIMP.Launcher;

// Position is replaceable; posture/torch/hiding edges are ordered together. Null
// fields are unavailable samples, so retain each field independently. Bound memory
// during a stalled engine: on overflow discard the oldest edge, retaining the newest
// state (especially completion). Normal entry/exit pairs remain separate.
internal sealed class PlayerPostureDispatchQueue
{
    internal const int Capacity = 32;
    private readonly Queue<(PlayerPostureSnapshot? Posture, PlayerTorchSnapshot? Torch, PlayerHidingSnapshot? Hiding)> _pending = new();
    private PlayerPostureSnapshot? _latest;
    private PlayerTorchSnapshot? _latestTorch;
    private PlayerHidingSnapshot? _latestHiding;
    internal int Count => _pending.Count;
    internal PlayerPostureSnapshot? Next => _pending.TryPeek(out var value) ? value.Posture : _latest;
    internal PlayerTorchSnapshot? NextTorch => _pending.TryPeek(out var value) ? value.Torch : _latestTorch;
    internal PlayerHidingSnapshot? NextHiding => _pending.TryPeek(out var value) ? value.Hiding : _latestHiding;
    internal bool HasPending => _pending.Count != 0;

    internal void Observe(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null)
    {
        var posture = value ?? _latest;
        torch ??= _latestTorch;
        hiding ??= _latestHiding;
        // Aim is replaceable at movement cadence; only on/off transitions need
        // an ordered edge. Looking around must not saturate the posture queue.
        var changed = posture != _latest || torch?.Enabled != _latestTorch?.Enabled
            || hiding != _latestHiding;
        _latest = posture;
        _latestTorch = torch;
        _latestHiding = hiding;
        if (!changed) return;
        if (_pending.Count == Capacity) _pending.Dequeue();
        _pending.Enqueue((posture, torch, hiding));
    }

    internal void Bind(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null)
    {
        _pending.Clear();
        _latest = null;
        _latestTorch = null;
        _latestHiding = null;
        Observe(value, torch, hiding);
    }

    internal void Complete(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null)
    {
        if (_pending.TryPeek(out var next) && next == (value, torch, hiding)) _pending.Dequeue();
    }
}
