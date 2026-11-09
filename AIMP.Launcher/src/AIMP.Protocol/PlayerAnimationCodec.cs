using System.Globalization;

namespace AIMP.Protocol;

// AnimationSync wire form, shared by the DLL observation, the peer snapshot and
// the CHARACTER_DIRECT_MOVE argument: "0" (no clip) or
// transition,animation,sampleAge,start,end,loops,speed,blendIn,x,y,z,yaw,phase,duration,attachment0..3.
// Mirrors AIMP.Native include/aimp/AnimationSync.h.
public static class PlayerAnimationCodec
{
    public const uint MaximumElapsedMilliseconds = 60_000;
    public static readonly PlayerAnimationSnapshot None = new(0, 0, 0, -1, -1, 1, 1F, 0.2F, 0F, 0F, 0F, 0F, 0F, 0F);

    public static bool IsValid(PlayerAnimationSnapshot value) =>
        value.Transition == 0 ? value == None
        : value.Animation != 0 && value.ElapsedMilliseconds <= MaximumElapsedMilliseconds
            && value.StartFrame is >= -1 and <= 1_000_000 && value.EndFrame is >= -1 and <= 1_000_000
            && value.Loops is >= -1 and <= 65_535
            && float.IsFinite(value.Speed) && value.Speed is > 0F and <= 64F
            && float.IsFinite(value.BlendIn) && value.BlendIn is >= 0F and <= 60F
            && Coordinate(value.X) && Coordinate(value.Y) && Coordinate(value.Z)
            && float.IsFinite(value.Yaw) && MathF.Abs(value.Yaw) <= 3.3F
            && float.IsFinite(value.PhaseSeconds) && value.PhaseSeconds is >= 0F and <= 100_000F
            && float.IsFinite(value.DurationSeconds) && value.DurationSeconds is > 0F and <= 100_000F;

    // Time advances without constituting a new ordered transition.
    public static bool SameTransition(PlayerAnimationSnapshot? left, PlayerAnimationSnapshot? right) =>
        left == right || left is not null && right is not null
        && left.Transition == right.Transition && left.Animation == right.Animation;

    public static PlayerAnimationSnapshot? Advance(PlayerAnimationSnapshot? value, long milliseconds) =>
        value is { Transition: not 0 } ? value with
        {
            ElapsedMilliseconds = (uint)Math.Clamp(value.ElapsedMilliseconds + Math.Max(0, milliseconds), 0, MaximumElapsedMilliseconds)
        } : value;

    public static string Encode(PlayerAnimationSnapshot value)
    {
        if (!IsValid(value)) throw new ProtocolException("Invalid player animation.");
        if (value.Transition == 0) return "0";
        return FormattableString.Invariant(
            $"{value.Transition},{value.Animation},{value.ElapsedMilliseconds},{value.StartFrame},{value.EndFrame},{value.Loops},{value.Speed:R},{value.BlendIn:R},{value.X:R},{value.Y:R},{value.Z:R},{value.Yaw:R},{value.PhaseSeconds:R},{value.DurationSeconds:R},{value.Attachment0},{value.Attachment1},{value.Attachment2},{value.Attachment3}");
    }

    public static bool TryDecode(string? text, out PlayerAnimationSnapshot? value)
    {
        value = null;
        if (text == "0") { value = None; return true; }
        if (text is null || text.Length > 384) return false;
        var fields = text.Split(',');
        if (fields.Length != 18
            || !ulong.TryParse(fields[0], NumberStyles.None, CultureInfo.InvariantCulture, out var transition)
            || !uint.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out var animation)
            || !uint.TryParse(fields[2], NumberStyles.None, CultureInfo.InvariantCulture, out var elapsed)
            || !int.TryParse(fields[3], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var start)
            || !int.TryParse(fields[4], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var end)
            || !int.TryParse(fields[5], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var loops)
            || !float.TryParse(fields[6], NumberStyles.Float, CultureInfo.InvariantCulture, out var speed)
            || !float.TryParse(fields[7], NumberStyles.Float, CultureInfo.InvariantCulture, out var blend)
            || !float.TryParse(fields[8], NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !float.TryParse(fields[9], NumberStyles.Float, CultureInfo.InvariantCulture, out var y)
            || !float.TryParse(fields[10], NumberStyles.Float, CultureInfo.InvariantCulture, out var z)
            || !float.TryParse(fields[11], NumberStyles.Float, CultureInfo.InvariantCulture, out var yaw)
            || !float.TryParse(fields[12], NumberStyles.Float, CultureInfo.InvariantCulture, out var phase)
            || !float.TryParse(fields[13], NumberStyles.Float, CultureInfo.InvariantCulture, out var duration)
            || !uint.TryParse(fields[14], NumberStyles.None, CultureInfo.InvariantCulture, out var attachment0)
            || !uint.TryParse(fields[15], NumberStyles.None, CultureInfo.InvariantCulture, out var attachment1)
            || !uint.TryParse(fields[16], NumberStyles.None, CultureInfo.InvariantCulture, out var attachment2)
            || !uint.TryParse(fields[17], NumberStyles.None, CultureInfo.InvariantCulture, out var attachment3)
            || transition == 0) return false;
        var candidate = new PlayerAnimationSnapshot(transition, animation, elapsed, start, end, loops, speed, blend, x, y, z, yaw, phase, duration,
            attachment0, attachment1, attachment2, attachment3);
        if (!IsValid(candidate)) return false;
        value = candidate;
        return true;
    }

    private static bool Coordinate(float value) => float.IsFinite(value) && MathF.Abs(value) <= PlayerHidingCodec.MaximumCoordinate;
}
