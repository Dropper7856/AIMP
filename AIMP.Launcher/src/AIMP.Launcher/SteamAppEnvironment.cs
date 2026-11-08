using System.Diagnostics;

namespace AIMP.Launcher;

internal static class SteamAppEnvironment
{
    private static readonly object Gate = new();

    internal static void Initialize(Action initialize)
    {
        lock (Gate)
        {
            var appId = Environment.GetEnvironmentVariable("SteamAppId");
            var gameId = Environment.GetEnvironmentVariable("SteamGameId");
            try { initialize(); }
            finally
            {
                Environment.SetEnvironmentVariable("SteamAppId", appId);
                Environment.SetEnvironmentVariable("SteamGameId", gameId);
            }
        }
    }

    internal static void PrepareGameProcess(ProcessStartInfo info)
    {
        lock (Gate)
        {
            // Materialize the child's environment outside SteamClient.Init's mutation.
            // Also discard Spacewar IDs inherited from an external launcher.
            foreach (var key in new[] { "SteamAppId", "SteamGameId" })
                if (info.Environment.TryGetValue(key, out var value) && value == "480")
                    info.Environment.Remove(key);
        }
    }
}
