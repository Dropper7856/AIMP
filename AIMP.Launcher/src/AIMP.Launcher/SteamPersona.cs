using System.Globalization;
using System.Text;
using Microsoft.Win32;

namespace AIMP.Launcher;

/// <summary>
/// Reads the Steam display name (PersonaName) of the signed-in account from
/// Steam's local files. The account login name (AccountName) is never read.
/// The persona is the one Steam recorded at its last sign-in.
/// </summary>
internal static class SteamPersona
{
    private const ulong SteamId64Base = 76561197960265728UL;

    internal static string? Resolve()
    {
        try
        {
            using var steam = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            if (steam?.GetValue("SteamPath") is not string steamPath) return null;
            using var process = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess");
            var activeAccountId = process?.GetValue("ActiveUser") is int value ? unchecked((uint)value) : 0U;
            var path = Path.Combine(steamPath, "config", "loginusers.vdf");
            return File.Exists(path) ? Resolve(File.ReadAllText(path), activeAccountId) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException
            or System.Security.SecurityException)
        {
            return null;
        }
    }

    // Signed-in account first; otherwise Steam's MostRecent flag, then the
    // latest sign-in timestamp.
    internal static string? Resolve(string loginUsersVdf, uint activeAccountId)
    {
        var users = ParseUsers(loginUsersVdf);
        var active = activeAccountId == 0 ? null
            : users.GetValueOrDefault((SteamId64Base + activeAccountId).ToString(CultureInfo.InvariantCulture));
        var selected = active ?? users.Values
            .OrderByDescending(u => u.GetValueOrDefault("MostRecent") == "1")
            .ThenByDescending(u => long.TryParse(u.GetValueOrDefault("Timestamp"), out var t) ? t : 0)
            .FirstOrDefault();
        return Sanitize(selected?.GetValueOrDefault("PersonaName"));
    }

    // Steam allows 32 characters; the protocol allows 24 without control or
    // format characters. Truncate on text elements so no character is split.
    internal static string? Sanitize(string? persona)
    {
        if (string.IsNullOrWhiteSpace(persona)) return null;
        var kept = new StringBuilder();
        foreach (var rune in persona.Trim().EnumerateRunes())
            if (Rune.GetUnicodeCategory(rune) is not (UnicodeCategory.Control or UnicodeCategory.Format
                or UnicodeCategory.LineSeparator or UnicodeCategory.ParagraphSeparator
                or UnicodeCategory.PrivateUse or UnicodeCategory.Surrogate))
                kept.Append(rune.ToString());
        var text = kept.ToString().Trim();
        var elements = StringInfo.GetTextElementEnumerator(text);
        var truncated = new StringBuilder();
        while (elements.MoveNext() && truncated.Length + elements.GetTextElement().Length
               <= AIMP.Protocol.ProtocolConstants.MaximumPlayerNameLength)
            truncated.Append(elements.GetTextElement());
        return AIMP.Protocol.PlayerName.TryNormalize(truncated.ToString(), out var name, out _) ? name : null;
    }

    // loginusers.vdf: "users" { "<SteamID64>" { "Key" "Value" ... } ... }
    private static Dictionary<string, Dictionary<string, string>> ParseUsers(string vdf)
    {
        var users = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
        var tokens = Tokenize(vdf).ToList();
        for (var i = 0; i + 1 < tokens.Count; i++)
        {
            if (tokens[i] is not { Quoted: true } id || tokens[i + 1].Text != "{" || !ulong.TryParse(id.Text, out _)) continue;
            var fields = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (i += 2; i + 1 < tokens.Count && tokens[i].Text != "}"; i += 2)
                if (tokens[i].Quoted && tokens[i + 1].Quoted) fields[tokens[i].Text] = tokens[i + 1].Text;
            users[id.Text] = fields;
        }
        return users;
    }

    private static IEnumerable<(string Text, bool Quoted)> Tokenize(string vdf)
    {
        for (var i = 0; i < vdf.Length; i++)
        {
            if (vdf[i] is '{' or '}') yield return (vdf[i].ToString(), false);
            if (vdf[i] != '"') continue;
            var value = new StringBuilder();
            for (i++; i < vdf.Length && vdf[i] != '"'; i++)
                value.Append(vdf[i] == '\\' && i + 1 < vdf.Length ? vdf[++i] : vdf[i]);
            yield return (value.ToString(), true);
        }
    }
}
