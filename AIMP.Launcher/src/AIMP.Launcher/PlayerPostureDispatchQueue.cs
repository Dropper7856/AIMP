using AIMP.Protocol;

namespace AIMP.Launcher;

// Position is replaceable; posture/torch/hiding/animation edges are ordered together.
// Null fields are unavailable samples, so retain each field independently. Bound memory
// during a stalled engine: on overflow discard the oldest edge, retaining the newest
// state (especially completion). Normal entry/exit pairs remain separate.
internal sealed class PlayerPostureDispatchQueue
{
    internal const int Capacity = 32;
    private readonly Queue<(PlayerPostureSnapshot? Posture, PlayerTorchSnapshot? Torch, PlayerHidingSnapshot? Hiding,
        PlayerAnimationSnapshot? Animation, long Enqueued)> _pending = new();
    private PlayerPostureSnapshot? _latest;
    private PlayerTorchSnapshot? _latestTorch;
    private PlayerHidingSnapshot? _latestHiding;
    private PlayerAnimationSnapshot? _latestAnimation;
    private long _latestAnimationObserved;
    internal int Count => _pending.Count;
    internal PlayerPostureSnapshot? Next => _pending.TryPeek(out var value) ? value.Posture : _latest;
    internal PlayerTorchSnapshot? NextTorch => _pending.TryPeek(out var value) ? value.Torch : _latestTorch;
    internal PlayerHidingSnapshot? NextHiding => _pending.TryPeek(out var value)
        ? Advance(value.Hiding, Environment.TickCount64 - value.Enqueued) : _latestHiding;
    internal PlayerAnimationSnapshot? NextAnimation => _pending.TryPeek(out var value)
        && !PlayerAnimationCodec.SameTransition(value.Animation, _latestAnimation)
        ? PlayerAnimationCodec.Advance(value.Animation, Environment.TickCount64 - value.Enqueued)
        : PlayerAnimationCodec.Advance(_latestAnimation, Environment.TickCount64 - _latestAnimationObserved);
    internal bool HasPending => _pending.Count != 0;

    internal void Observe(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null, PlayerAnimationSnapshot? animation = null)
    {
        var observed = Environment.TickCount64;
        var posture = value ?? _latest;
        torch ??= _latestTorch;
        hiding ??= _latestHiding;
        animation ??= PlayerAnimationCodec.Advance(_latestAnimation, observed - _latestAnimationObserved);
        _latestAnimationObserved = observed;
        // Aim is replaceable at movement cadence; only on/off transitions need
        // an ordered edge. Looking around must not saturate the posture queue.
        var changed = posture != _latest || torch?.Enabled != _latestTorch?.Enabled
            || !PlayerHidingPresentationCodec.SameTransition(hiding, _latestHiding)
            || !PlayerAnimationCodec.SameTransition(animation, _latestAnimation);
        _latest = posture;
        _latestTorch = torch;
        _latestHiding = hiding;
        _latestAnimation = animation;
        if (!changed) return;
        if (_pending.Count == Capacity) _pending.Dequeue();
        _pending.Enqueue((posture, torch, hiding, animation, observed));
    }

    internal void Bind(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null, PlayerAnimationSnapshot? animation = null)
    {
        _pending.Clear();
        _latest = null;
        _latestTorch = null;
        _latestHiding = null;
        _latestAnimation = null;
        Observe(value, torch, hiding, animation);
    }

    internal void Complete(PlayerPostureSnapshot? value, PlayerTorchSnapshot? torch = null,
        PlayerHidingSnapshot? hiding = null, PlayerAnimationSnapshot? animation = null)
    {
        if (_pending.TryPeek(out var next) && next.Posture == value && next.Torch == torch
            && PlayerHidingPresentationCodec.SameTransition(next.Hiding, hiding)
            && PlayerAnimationCodec.SameTransition(next.Animation, animation)) _pending.Dequeue();
    }

    internal static PlayerHidingSnapshot? Advance(PlayerHidingSnapshot? hiding, long milliseconds) =>
        hiding?.Presentation is { } presentation ? hiding with { Presentation = presentation with
        { ElapsedMilliseconds = (uint)Math.Clamp(presentation.ElapsedMilliseconds + Math.Max(0, milliseconds), 0, 60_000) } } : hiding;
}
