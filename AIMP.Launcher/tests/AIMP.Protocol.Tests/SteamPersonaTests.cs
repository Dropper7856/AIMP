using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class SteamPersonaTests
{
    private const string LoginUsers = """
        "users"
        {
            "76561198000000001"
            {
                "AccountName"       "first_login"
                "PersonaName"       "First Display"
                "MostRecent"        "0"
                "Timestamp"         "1700000000"
            }
            "76561198000000002"
            {
                "AccountName"       "second_login"
                "PersonaName"       "Second \"Quoted\" Display"
                "MostRecent"        "1"
                "Timestamp"         "1600000000"
            }
        }
        """;

    internal static Task SelectionAsync()
    {
        // ActiveUser is the 32-bit account id: SteamID64 - 76561197960265728.
        AssertEx.Equal("First Display", SteamPersona.Resolve(LoginUsers, 39734273U) ?? "<null>");
        AssertEx.Equal("Second \"Quoted\" Display", SteamPersona.Resolve(LoginUsers, 0U) ?? "<null>");
        AssertEx.Equal("Second \"Quoted\" Display", SteamPersona.Resolve(LoginUsers, 12345U) ?? "<null>",
            "An unknown active account falls back to the most recent sign-in.");
        AssertEx.True(SteamPersona.Resolve("\"users\" { }", 0U) is null);
        AssertEx.True(SteamPersona.Resolve(LoginUsers.Replace("PersonaName", "Other"), 39734273U) is null,
            "The login AccountName is never used as a display name.");
        AssertEx.Equal("Ripley", SteamPersona.Sanitize("  Rip\u200Bley\t ") ?? "<null>");
        AssertEx.Equal(new string('x', ProtocolConstants.MaximumPlayerNameLength),
            SteamPersona.Sanitize(new string('x', 32)) ?? "<null>");
        AssertEx.Equal(new string('a', 22) + "😀", SteamPersona.Sanitize(new string('a', 22) + "😀😀") ?? "<null>",
            "Truncation never splits a character.");
        AssertEx.True(SteamPersona.Sanitize("\u200B\t") is null);
        return Task.CompletedTask;
    }

    internal static Task ImposedNameAsync()
    {
        using var temporary = new TemporaryDirectory();
        var path = temporary.Combine("settings.json");
        string? persona = "Steam Display";
        var store = new LauncherSettingsStore(path, steamPersona: () => persona);
        AssertEx.Equal("Steam Display", store.Load().PlayerName);
        store.Save(new LauncherSettings("C:\\Game", "Typed Name"));
        AssertEx.Equal("Steam Display", store.Load().PlayerName, "A typed name cannot replace the Steam name.");
        persona = "Renamed";
        AssertEx.Equal("Renamed", store.Load().PlayerName);
        persona = null;
        AssertEx.Equal("Steam Display", store.Load().PlayerName,
            "Without Steam, the last stored Steam name remains.");
        return Task.CompletedTask;
    }
}
