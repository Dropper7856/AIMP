using System.Globalization;
using System.Text;

namespace AIMP.Protocol;

public static class PlayerName
{
    public static bool TryNormalize(string? value, out string normalized, out string error)
    {
        try
        {
            normalized = (value ?? string.Empty).Trim().Normalize(NormalizationForm.FormC);
        }
        catch (ArgumentException)
        {
            normalized = string.Empty;
            error = "The name contains an invalid Unicode sequence.";
            return false;
        }

        if (normalized.Length is < 1 or > ProtocolConstants.MaximumPlayerNameLength)
        {
            error = $"The name must contain 1 to {ProtocolConstants.MaximumPlayerNameLength} characters.";
            return false;
        }

        foreach (var rune in normalized.EnumerateRunes())
        {
            var category = Rune.GetUnicodeCategory(rune);
            if (category is UnicodeCategory.Control or UnicodeCategory.Format
                or UnicodeCategory.LineSeparator or UnicodeCategory.ParagraphSeparator
                or UnicodeCategory.PrivateUse or UnicodeCategory.Surrogate)
            {
                error = "The name contains an unsupported character.";
                return false;
            }
        }

        error = string.Empty;
        return true;
    }
}
