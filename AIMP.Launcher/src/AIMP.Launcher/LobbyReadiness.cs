using AIMP.Protocol;

namespace AIMP.Launcher;

internal static class LobbyReadiness
{
    internal static string Status(LobbySnapshotPayload lobby)
    {
        if (lobby.State != "Lobby") return lobby.State;
        if (lobby.Players.Count < 2) return "Waiting for another player";
        if (lobby.WorldPreparationRequired)
        {
            var preparation = PreparationStatus(lobby);
            if (preparation.Length != 0) return preparation;
        }
        var waiting = lobby.Players.FirstOrDefault(p => !p.IsHost && !p.Ready);
        return waiting is null ? "Ready to start" : $"Waiting for {waiting.DisplayName} to select Ready";
    }

    private static string PreparationStatus(LobbySnapshotPayload lobby)
    {
        var reports = lobby.Preparations ?? Array.Empty<LobbyPreparationPayload>();
        var hostPlayer = lobby.Players.FirstOrDefault(p => p.IsHost);
        var hostReport = hostPlayer is null ? null : reports.FirstOrDefault(p => p.SessionId == lobby.SessionId
            && p.PeerId == hostPlayer.PeerId && p.PreparationToken == lobby.PreparationToken && p.Ready);
        foreach (var player in lobby.Players)
        {
            var report = reports.FirstOrDefault(p => p.SessionId == lobby.SessionId && p.PeerId == player.PeerId
                && p.PreparationToken == lobby.PreparationToken);
            if (report is null) return $"Verifying {player.DisplayName}'s level";
            if (!report.Ready) return $"{player.DisplayName}: {report.Error}";
            if (!string.Equals(report.Level, lobby.Level, StringComparison.OrdinalIgnoreCase))
                return $"Level mismatch: {player.DisplayName}";
            if (hostReport is not null && !LobbyHost.SameWorldManifest(report, hostReport))
                return $"Level files differ: {player.DisplayName}";
        }
        return string.Empty;
    }
}
