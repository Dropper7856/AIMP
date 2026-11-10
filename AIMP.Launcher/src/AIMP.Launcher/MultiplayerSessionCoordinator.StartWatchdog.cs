namespace AIMP.Launcher;

internal sealed partial class MultiplayerSessionCoordinator
{
    // A campaign start leaves the frontend within seconds: once the level loads,
    // the menu movie stops sending MENU_STATE. If the menu is still reporting long
    // after START_GAME while the engine sits in the frontend, the game refused the
    // start (retail "An error occurred" on a checkpoint load). The menu then stays
    // locked in "Starting game" with every lobby control disabled; closing the
    // session sends SESSION_CLOSED, which returns the menu to normal.
    // Survival is excluded: its menu stays up during character selection.
    internal static readonly TimeSpan CampaignStartMenuTimeout = TimeSpan.FromSeconds(20);

    // Called for each MENU_STATE with the command gate held.
    private async Task<bool> CloseCampaignStartStuckAtMenuAsync()
    {
        string? level;
        lock (_stateGate) level = CampaignStartStuckAtMenuLocked(_controller.Snapshot, DateTimeOffset.UtcNow);
        if (level is null) return false;
        _diagnostics.Log("Multiplayer", $"The game is still in the main menu {CampaignStartMenuTimeout.TotalSeconds:0} s after "
            + $"starting {level}: the campaign did not load. Closing the session so the menu can be used again.");
        await StopNetworkAsync("The game could not load the campaign. The lobby was closed.").ConfigureAwait(false);
        return true;
    }

    // Caller holds _stateGate. Returns the level of a stuck campaign start, else null.
    private string? CampaignStartStuckAtMenuLocked(NativeRuntimeSnapshot runtime, DateTimeOffset now)
    {
        if (_start is not { } start || _uiGameplayLoaded || _loadingHoldStart is null) return null;
        if (LevelGameMode(start.Level) != "Campaign" || now - _loadingHoldStartedUtc < CampaignStartMenuTimeout) return null;
        var atFrontend = runtime.CurrentLevelName.EndsWith("Frontend", StringComparison.OrdinalIgnoreCase)
            && !runtime.LevelTransitioning;
        return atFrontend ? start.Level : null;
    }
}
