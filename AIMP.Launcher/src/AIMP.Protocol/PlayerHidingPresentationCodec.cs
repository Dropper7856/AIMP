using System.Globalization;

namespace AIMP.Protocol;

public static class PlayerHidingPresentationCodec
{
    public static bool IsValid(PlayerHidingPresentationSnapshot value) =>
        value.Phase is >= PlayerHidingPhase.Entering and <= PlayerHidingPhase.Exiting
        && value.Kind is PlayerHidingKind.Tall or PlayerHidingKind.Low
        && value.EntityShortGuid == (value.Kind == PlayerHidingKind.Tall ? 0x5DC33E3EU : 0x61B51D78U)
        && value.HierarchyHash != 0 && value.Transition != 0
        && value.ElapsedMilliseconds <= 60_000
        && Coordinate(value.X) && Coordinate(value.Y) && Coordinate(value.Z)
        && float.IsFinite(value.Yaw) && MathF.Abs(value.Yaw) <= 3.3F;

    // Time advances without constituting a new ordered transition.
    public static bool SameTransition(PlayerHidingSnapshot? left, PlayerHidingSnapshot? right) =>
        left == right || left is not null && right is not null
        && left.Hidden == right.Hidden && left.AnchorX == right.AnchorX
        && left.AnchorY == right.AnchorY && left.AnchorZ == right.AnchorZ
        && (left.Presentation is { } a && right.Presentation is { } b
            ? a with { ElapsedMilliseconds = 0 } == b with { ElapsedMilliseconds = 0 }
            : left.Presentation == right.Presentation);

    public static string Encode(PlayerHidingPresentationSnapshot value, bool animate)
    {
        if (!IsValid(value)) throw new ProtocolException("Invalid hiding presentation.");
        return FormattableString.Invariant($"{(int)value.Phase},{(int)value.Kind},{value.EntityShortGuid},{value.HierarchyHash},{value.Transition},{value.ElapsedMilliseconds},{value.X:R},{value.Y:R},{value.Z:R},{value.Yaw:R},{(animate ? 1 : 0)}");
    }

    public static bool TryDecode(string? text, out PlayerHidingPresentationSnapshot? value, out bool animate)
    {
        value = null; animate = false;
        if (text is null || text.Length > 224) return false;
        var fields = text.Split(',');
        if (fields.Length != 11 || !uint.TryParse(fields[0], NumberStyles.None, CultureInfo.InvariantCulture, out var phase)
            || !uint.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out var kind)
            || !uint.TryParse(fields[2], NumberStyles.None, CultureInfo.InvariantCulture, out var entity)
            || !uint.TryParse(fields[3], NumberStyles.None, CultureInfo.InvariantCulture, out var hierarchy)
            || !ulong.TryParse(fields[4], NumberStyles.None, CultureInfo.InvariantCulture, out var transition)
            || !uint.TryParse(fields[5], NumberStyles.None, CultureInfo.InvariantCulture, out var elapsed)
            || !float.TryParse(fields[6], NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !float.TryParse(fields[7], NumberStyles.Float, CultureInfo.InvariantCulture, out var y)
            || !float.TryParse(fields[8], NumberStyles.Float, CultureInfo.InvariantCulture, out var z)
            || !float.TryParse(fields[9], NumberStyles.Float, CultureInfo.InvariantCulture, out var yaw)
            || fields[10] is not ("0" or "1")) return false;
        var candidate = new PlayerHidingPresentationSnapshot((PlayerHidingPhase)phase, (PlayerHidingKind)kind,
            entity, hierarchy, transition, elapsed, x, y, z, yaw);
        if (!IsValid(candidate)) return false;
        value = candidate; animate = fields[10] == "1"; return true;
    }

    private static bool Coordinate(float value) => float.IsFinite(value) && MathF.Abs(value) <= PlayerHidingCodec.MaximumCoordinate;
}
