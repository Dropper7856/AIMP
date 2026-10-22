using System.Globalization;

namespace AIMP.Protocol;

public static class PlayerPostureCodec
{
    public static bool IsVentTraversal(byte type) => type is 4 or 5 or 7 or 8;

    public static bool IsValid(PlayerPostureSnapshot value) =>
        ((value.VentIdA == 0) == (value.VentIdB == 0))
        && (!value.InVent || value.VentIdA != 0)
        && (value.TraversalRevision == 0
            ? !value.TraversalActive && value.TraversalIndex == 0
                && value.TraversalType == 0 && value.TraversalOwner == 0
            : value.TraversalIndex != 0 && IsVentTraversal(value.TraversalType)
                && value.TraversalOwner != uint.MaxValue);

    public static string Encode(PlayerPostureSnapshot value)
    {
        ModelValidation.Validate(value);
        uint flags = (value.Crouched ? 1U : 0U) | (value.DeepCrouched ? 2U : 0U)
            | (value.InVent ? 4U : 0U) | (value.TraversalActive ? 8U : 0U);
        return FormattableString.Invariant($"{flags},{value.VentIdA},{value.VentIdB},{value.TraversalRevision},{value.TraversalIndex},{value.TraversalType},{value.TraversalOwner}");
    }

    public static bool TryDecode(string? text, out PlayerPostureSnapshot? value)
    {
        value = null;
        if (text is null || text.Length > 128) return false;
        var parts = text.Split(',');
        if (parts is not { Length: 7 }) return false;
        var numbers = new ulong[7];
        for (var i = 0; i < numbers.Length; i++)
            if (!ulong.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out numbers[i])) return false;
        if (numbers[0] > 15 || numbers[1] > uint.MaxValue || numbers[2] > uint.MaxValue
            || numbers[4] > ushort.MaxValue || numbers[5] > byte.MaxValue || numbers[6] > uint.MaxValue) return false;
        var flags = (uint)numbers[0];
        var candidate = new PlayerPostureSnapshot((flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0,
            (uint)numbers[1], (uint)numbers[2], numbers[3], (ushort)numbers[4], (byte)numbers[5], (uint)numbers[6], (flags & 8) != 0);
        if (!IsValid(candidate)) return false;
        value = candidate;
        return true;
    }
}
