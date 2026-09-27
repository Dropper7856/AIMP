using System.Globalization;

namespace AIMP.Launcher;

internal static class CharacterAddressCalculator
{
    internal const uint CharacterOffset = 0x3570U;

    internal static uint Calculate(uint ecx)
    {
        if (ecx < CharacterOffset)
            throw new InvalidOperationException("ECX is smaller than the Character offset 0x3570.");
        return ecx - CharacterOffset;
    }

    internal static bool TryParseHex32(string text, out uint value)
    {
        var normalized = text.Trim();
        if (normalized.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            normalized = normalized[2..];
        return uint.TryParse(normalized, NumberStyles.HexNumber,
            CultureInfo.InvariantCulture, out value);
    }

    internal static string Format(uint address) => $"0x{address:X8}";
}
