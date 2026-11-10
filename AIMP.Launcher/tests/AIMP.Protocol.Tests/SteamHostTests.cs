using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

// The Steam helper process talks to the launcher over JSON lines on stdin/stdout.
internal static class SteamHostTests
{
    internal static Task RoundTripsAsync()
    {
        var listing = new SteamLobbyListing(109775241000000001UL, 76561198000000001UL, "Name | x", "Campaign",
            "BSP_TORRENS", 1, 4, true, true, FromFriend: false);
        var invite = new SteamLobbyInvite(76561198000000002UL, "Friend", 109775241000000002UL,
            new DateTime(2026, 11, 10, 12, 0, 0, DateTimeKind.Utc), AcceptedInSteam: true);
        var hosting = new SteamHostHosting(28015, "Hosting", SteamLobbyVisibility.Friends,
            new SteamLobbyAdvert("Lobby", "Survival", "Map", 1, 4, true));
        AssertEx.Equal(listing, RoundTrip(new[] { listing })[0]);
        AssertEx.Equal(invite, RoundTrip(invite));
        AssertEx.Equal(hosting, RoundTrip(hosting));
        AssertEx.Equal(ulong.MaxValue, RoundTrip(ulong.MaxValue));
        return Task.CompletedTask;
    }

    internal static Task KeepsErrorKindsAsync()
    {
        foreach (var error in new Exception[]
                 {
                     new TimeoutException("t"), new InvalidOperationException("i"), new IOException("o"),
                     new ArgumentException("a"), new NotSupportedException("n")
                 })
        {
            var reply = SteamHostProtocol.Parse(SteamHostProtocol.Serialize(new SteamHostMessage
                { Id = 7, Error = error.Message, ErrorKind = SteamHostProtocol.KindOf(error) }));
            var rebuilt = SteamHostProtocol.ToException(reply);
            var expected = error is NotSupportedException ? typeof(InvalidOperationException) : error.GetType();
            AssertEx.Equal(expected, rebuilt.GetType());
            AssertEx.Equal(error.Message, rebuilt.Message);
        }
        return Task.CompletedTask;
    }

    internal static Task IgnoresNormalArgumentsAsync()
    {
        AssertEx.False(SteamHostServer.TryRun(Array.Empty<string>(), out _));
        AssertEx.False(SteamHostServer.TryRun(new[] { "--urls", "http://127.0.0.1:5000" }, out _));
        AssertEx.False(SteamHostServer.TryRun(new[] { SteamHostProtocol.Argument }, out _));
        AssertEx.True(SteamHostServer.TryRun(new[] { SteamHostProtocol.Argument, "abc" }, out var code));
        AssertEx.Equal(2, code);
        return Task.CompletedTask;
    }

    private static T RoundTrip<T>(T value) where T : notnull
    {
        var line = SteamHostProtocol.Serialize(new SteamHostMessage { Id = 1, Ok = true, Data = SteamHostProtocol.ToElement(value) });
        return SteamHostProtocol.Read<T>(SteamHostProtocol.Parse(line).Data);
    }
}
