using System.Globalization;

namespace AIMP.Protocol;

// Move-command form: "0" when visible, "1,x,y,z" while hidden at that anchor.
public static class PlayerHidingCodec
{
    public const float MaximumCoordinate = 1_000_000F;

    public static bool IsValid(PlayerHidingSnapshot value)
    {
        if (!value.Hidden) return value.AnchorX is null && value.AnchorY is null && value.AnchorZ is null;
        return value.AnchorX is { } x && value.AnchorY is { } y && value.AnchorZ is { } z
            && ValidCoordinate(x) && ValidCoordinate(y) && ValidCoordinate(z);
    }

    public static string Encode(PlayerHidingSnapshot value)
    {
        ModelValidation.Validate(value);
        return value.Hidden
            ? FormattableString.Invariant($"1,{value.AnchorX:R},{value.AnchorY:R},{value.AnchorZ:R}")
            : "0";
    }

    public static bool TryDecode(string? text, out PlayerHidingSnapshot? value)
    {
        value = null;
        if (text is null || text.Length > 96) return false;
        if (text == "0")
        {
            value = new PlayerHidingSnapshot(false);
            return true;
        }
        var fields = text.Split(',');
        if (fields.Length != 4 || fields[0] != "1"
            || !float.TryParse(fields[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !float.TryParse(fields[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var y)
            || !float.TryParse(fields[3], NumberStyles.Float, CultureInfo.InvariantCulture, out var z)) return false;
        var candidate = new PlayerHidingSnapshot(true, x, y, z);
        if (!IsValid(candidate)) return false;
        value = candidate;
        return true;
    }

    private static bool ValidCoordinate(float value) => float.IsFinite(value) && MathF.Abs(value) <= MaximumCoordinate;
}
