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
        var host = lobby.Players.FirstOrDefault(p => p.IsHost);
        var reports = lobby.Preparations ?? Array.Empty<LobbyPreparationPayload>();
        var hostReport = reports.FirstOrDefault(p => p.PeerId == host?.PeerId && p.PreparationToken == lobby.PreparationToken);
        foreach (var player in lobby.Players)
        {
            var report = reports.FirstOrDefault(p => p.PeerId == player.PeerId && p.PreparationToken == lobby.PreparationToken);
            if (report is null) return $"Verifying {player.DisplayName}'s level";
            if (!report.Ready) return $"{player.DisplayName}: {report.Error}";
            if (hostReport is { Ready: true } && (report.Manifest != hostReport.Manifest
                || !string.Equals(report.ManifestSha256, hostReport.ManifestSha256, StringComparison.OrdinalIgnoreCase)))
                return $"Level manifest mismatch: {player.DisplayName}";
        }
        return string.Empty;
    }
}
