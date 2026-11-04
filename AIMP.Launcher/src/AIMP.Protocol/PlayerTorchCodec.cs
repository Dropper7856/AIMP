using System.Globalization;

namespace AIMP.Protocol;

public static class PlayerTorchCodec
{
    public static bool IsValid(PlayerTorchSnapshot value)
    {
        if (value.AimX is null && value.AimY is null && value.AimZ is null) return true;
        return value.Enabled && value.AimX is { } x && value.AimY is { } y && value.AimZ is { } z
            && float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z)
            && x * x + y * y + z * z is >= 0.99F and <= 1.01F;
    }

    public static string EncodeDirection(PlayerTorchSnapshot value) =>
        FormattableString.Invariant($"{value.AimX:R},{value.AimY:R},{value.AimZ:R}");

    public static bool TryDecodeDirection(string? text, out PlayerTorchSnapshot? value)
    {
        value = null;
        if (text is null || text.Length > 96) return false;
        var fields = text.Split(',');
        if (fields.Length != 3
            || !float.TryParse(fields[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !float.TryParse(fields[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var y)
            || !float.TryParse(fields[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var z)) return false;
        var candidate = new PlayerTorchSnapshot(true, x, y, z);
        if (!IsValid(candidate)) return false;
        value = candidate;
        return true;
    }
}
