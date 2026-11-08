namespace AIMP.Launcher;

internal interface ISteamRelay : IDisposable
{
    event Action<string, string>? JoinRequested;
    ulong LocalSteamId { get; }
    Task<bool> ActivateForMenuAsync(int gameProcessId);
    Task<bool> StartHostingAsync(int lobbyPort, string statusText);
    Task StopHostingAsync();
    Task<int> OpenClientTunnelAsync(ulong hostSteamId, CancellationToken cancellationToken);
    void CloseAllTunnels(string reason);
}
