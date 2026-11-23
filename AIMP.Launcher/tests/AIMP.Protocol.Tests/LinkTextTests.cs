using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

// Steam names used as malware C2 dead drops ("https://65.109.243.177|") must be
// recognised and refused everywhere a name from another player is shown.
internal static class LinkTextTests
{
    private static readonly string[] Blocked =
    {
        "https://65.109.243.177|",
        "https://37.27.179.0|",
        "http://evil.example.com",
        "hxxps://65.109.243[.]177",
        "65.109.243[.]177",
        "65 . 109 . 243 . 177",
        "37(.)27(.)179(.)0",
        "\uFF16\uFF15\uFF0E\uFF11\uFF10\uFF19\uFF0E\uFF12\uFF14\uFF13\uFF0E\uFF11\uFF17\uFF17", // full-width IPv4
        "ｈｔｔｐｓ://x",
        "steam://connect/1.2.3.4",
        "www.free-skins",
        "get skins at freeskins.xyz",
        "malware[.]ru",
        "Join discord.gg",
        "2a01:4f9:c010::1",
        "2001:db8:0:0:0:0:2:1",
        "ftp://host",
        "1.2.3.4:27015",
    };

    private static readonly string[] Allowed =
    {
        "Ripley", "Amanda Ripley", "Mr. Smith", "Dr.Who", "xX_Alien_Xx", "Player 1",
        "Working Joe 2.0", "v1.2", "12:30", "Sevastopol", "Ripley's Lobby", "Survival",
        "Tech_Demo", "Ellen-Ripley", "Comms.Hub", "C:/", "[hidden name] #1234", "😀 Survivor",
        "Name contains a web/IP address - blocked",
    };

    internal static Task DetectsAddressesAsync()
    {
        foreach (var text in Blocked)
            AssertEx.True(LinkText.ContainsAddress(text), $"Expected '{text}' to be detected as an address.");
        foreach (var text in Allowed)
            AssertEx.False(LinkText.ContainsAddress(text), $"'{text}' was wrongly detected as an address.");
        AssertEx.False(LinkText.ContainsAddress(null));
        AssertEx.False(LinkText.ContainsAddress("   "));
        return Task.CompletedTask;
    }

    internal static Task NamesRefuseAddressesAsync()
    {
        AssertEx.False(PlayerName.TryNormalize("https://65.109.243.177|", out _, out var error));
        AssertEx.True(error.Contains("web address", StringComparison.Ordinal), error);
        AssertEx.True(PlayerName.TryNormalize("Ripley", out var normal, out _) && normal == "Ripley");
        AssertEx.True(SteamPersona.Sanitize("https://37.27.179.0|") is null,
            "A Steam persona carrying an address is not used as the player name.");
        AssertEx.True(SteamRelay.HasAddress("Lobby", "https://1.2.3.4"));
        AssertEx.False(SteamRelay.HasAddress("Lobby", "Sevastopol", "Survival"));
        AssertEx.False(SteamRelay.HasAddress(SteamRelay.HiddenName(76561198000001234UL)));

        var threw = false;
        try
        {
            _ = new LobbyHost(new LobbyHostOptions(
                "free skins at 65.109.243.177", "Ripley", Guid.NewGuid(), 0, 4,
                "Tech_Hub", new CompatibilityInfo(ProtocolConstants.Version,
                    "test-launcher", "test-dll", new string('A', 64), "test-profile", new string('B', 64))));
        }
        catch (ArgumentException exception) when (exception.Message.Contains("server name", StringComparison.Ordinal))
        {
            threw = true;
        }
        AssertEx.True(threw, "A lobby cannot advertise a server name with an IP address.");
        return Task.CompletedTask;
    }
}
