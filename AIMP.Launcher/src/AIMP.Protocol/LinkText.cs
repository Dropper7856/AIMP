using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace AIMP.Protocol;

/// <summary>
/// Detects web addresses and IP addresses in names that other people choose:
/// Steam persona names, Steam lobby names/maps and AIMP player/server names.
///
/// Malware families (for example Vidar) put C2 addresses such as
/// "https://65.109.243.177|" in Steam profile names. Anything like that shown in
/// the server browser, Friends page, invites or lobby can point players at a
/// malware host, so such names are refused or hidden. The check also catches
/// defanged ("1.2.3[.]4", "hxxps") and look-alike (full-width) spellings.
/// </summary>
public static class LinkText
{
    private static readonly TimeSpan MatchTimeout = TimeSpan.FromMilliseconds(100);

    // Scheme ("https:/", "steam://", "ftp://") or a "www." prefix.
    private static readonly Regex Scheme = new(
        @"(?<![a-z0-9])[a-z][a-z0-9+.\-]{1,15}:/|(?<![a-z0-9])www\.",
        RegexOptions.CultureInvariant, MatchTimeout);

    // Dotted IPv4, spaces around the dots allowed ("65 . 109 . 243 . 177").
    private static readonly Regex Ipv4 = new(
        @"(?<!\d)\d{1,3}(?:\s*\.\s*\d{1,3}){3}(?!\d)",
        RegexOptions.CultureInvariant, MatchTimeout);

    // IPv6: four or more groups, or a "::" compression.
    private static readonly Regex Ipv6 = new(
        @"(?<![0-9a-z:])(?:[0-9a-f]{1,4}:){3,7}[0-9a-f]{1,4}(?![0-9a-z:])|(?<![0-9a-z:])[0-9a-f]{1,4}(?::[0-9a-f]{1,4})*::(?:[0-9a-f]{1,4}(?::[0-9a-f]{1,4})*)?(?![0-9a-z:])",
        RegexOptions.CultureInvariant, MatchTimeout);

    // host.tld for common and commonly abused top-level domains.
    private static readonly Regex Domain = new(
        @"(?<![a-z0-9\-])[a-z0-9](?:[a-z0-9\-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9\-]{0,61}[a-z0-9])?)*\." +
        @"(?:com|net|org|info|biz|io|co|me|xyz|top|site|online|club|shop|store|app|dev|gg|tk|ml|ga|cf|gq|ru|su|cn|pw|cc|ws|" +
        @"link|click|live|icu|buzz|fun|space|website|tech|pro|vip|win|bid|loan|work|cam|rest|monster|sbs|cfd|ly|to|tv|us|uk|de|" +
        @"fr|nl|eu|onion|zip|mov|lol|today|best|life|world|cloud|host|page|run|sh|gl|gd|is|im)(?![a-z0-9\-])",
        RegexOptions.CultureInvariant, MatchTimeout);

    /// <summary>True when the text contains a URL, a domain name or an IP address.</summary>
    public static bool ContainsAddress(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return false;
        var folded = Fold(text);
        try
        {
            return Scheme.IsMatch(folded) || Ipv4.IsMatch(folded) || Ipv6.IsMatch(folded) || Domain.IsMatch(folded);
        }
        catch (RegexMatchTimeoutException)
        {
            return true; // pathological input: treat as unsafe
        }
    }

    // Look-alikes and defanging folded to plain ASCII: full-width letters/digits
    // (NFKC), ideographic and other dot variants, "[.]", "(dot)", "hxxp".
    internal static string Fold(string text)
    {
        string normalized;
        try { normalized = text.Normalize(NormalizationForm.FormKC); }
        catch (ArgumentException) { normalized = text; }

        var builder = new StringBuilder(normalized.Length);
        foreach (var rune in normalized.EnumerateRunes())
        {
            var category = Rune.GetUnicodeCategory(rune);
            if (category is UnicodeCategory.Format or UnicodeCategory.Control or UnicodeCategory.NonSpacingMark) continue;
            builder.Append(rune.Value switch
            {
                0x3002 or 0xFF61 or 0x2024 or 0x2027 or 0x00B7 or 0x2219 or 0x22C5 or 0x30FB or 0x0701 => ".",
                0xFF1A or 0x02D0 or 0x2236 => ":",
                0x2044 or 0x2215 or 0x29F8 => "/",
                _ => rune.ToString()
            });
        }

        var folded = builder.ToString().ToLowerInvariant();
        foreach (var dot in DefangedDots) folded = folded.Replace(dot, ".", StringComparison.Ordinal);
        return folded.Replace("hxxp", "http", StringComparison.Ordinal)
            .Replace("h**p", "http", StringComparison.Ordinal);
    }

    private static readonly string[] DefangedDots =
    {
        "[.]", "(.)", "{.}", "<.>", "[dot]", "(dot)", "{dot}", "<dot>", " dot ", "[d]", "\\."
    };
}
