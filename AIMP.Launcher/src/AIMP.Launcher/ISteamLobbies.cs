namespace AIMP.Launcher;

/// <summary>Who can see a hosted lobby. Direct IP / port forwarding are unaffected by this setting.</summary>
internal enum SteamLobbyVisibility
{
    /// <summary>Solo: one slot, not on Steam, nobody can join.</summary>
    Private,
    /// <summary>Listed in the server browser of the host's Steam friends; only friends get through the relay.</summary>
    Friends,
    /// <summary>Listed in everyone's server browser.</summary>
    Public
}

internal sealed record SteamLobbyAdvert(string Name, string Mode, string Map, int Players, int MaxPlayers, bool Joinable);

internal sealed record SteamLobbyListing(
    ulong LobbyId, ulong HostSteamId, string Name, string Mode, string Map,
    int Players, int MaxPlayers, bool Joinable, bool Compatible, bool FromFriend);

internal sealed record SteamFriendEntry(
    ulong SteamId, string Name, string Status, bool RunningAimp, ulong LobbyId, bool CanJoin);

/// <summary>A lobby invite. <see cref="AcceptedInSteam"/> is true when the player
/// already accepted it in the Steam client (chat / overlay).</summary>
internal sealed record SteamLobbyInvite(ulong FromSteamId, string FromName, ulong LobbyId, DateTime ReceivedUtc,
    bool AcceptedInSteam = false);

/// <summary>Steam lobbies (server browser), friends list and invites. Implemented by
/// <see cref="SteamRelay"/>; the Steam lobby is only an advert + invite channel, game
/// traffic still goes through the relay tunnel to the host's normal lobby port.</summary>
internal interface ISteamLobbies
{
    /// <summary>Raised on a worker thread when a friend invites us to an AIMP lobby.</summary>
    event Action<SteamLobbyInvite>? InviteReceived;
    Task<bool> StartLobbyHostingAsync(int lobbyPort, SteamLobbyVisibility visibility, SteamLobbyAdvert advert);
    Task<bool> UpdateHostedLobbyAsync(SteamLobbyAdvert advert);
    Task<ulong> JoinLobbyAsync(ulong lobbyId);
    Task<IReadOnlyList<SteamLobbyListing>> FindLobbiesAsync();
    Task<IReadOnlyList<SteamFriendEntry>> GetFriendsAsync();
    Task<ulong> GetFriendLobbyAsync(ulong friendId);
    Task<string> InviteFriendAsync(ulong friendId);
    IReadOnlyList<SteamLobbyInvite> GetInvites();
    SteamLobbyInvite? TakeInvite(ulong fromSteamId);
    /// <summary>Shuts the Spacewar session down after Alien: Isolation exits or crashes, so
    /// Steam stops showing the player in-game. The next ready game menu activates it again.</summary>
    Task ReleaseAfterGameExitAsync();
}
