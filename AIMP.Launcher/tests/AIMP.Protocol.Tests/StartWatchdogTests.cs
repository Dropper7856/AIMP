using System.Reflection;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

// A campaign start the game refuses leaves the menu locked in "Starting game";
// the watchdog closes the session once the engine is still in the frontend.
internal static class StartWatchdogTests
{
    private const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;

    internal static Task DetectsCampaignStuckAtMenuAsync()
    {
        using var fixture = new Fixture();
        var now = DateTimeOffset.UtcNow;
        var late = now - MultiplayerSessionCoordinator.CampaignStartMenuTimeout - TimeSpan.FromSeconds(1);
        fixture.Start("BSP_Torrens", late);
        AssertEx.Equal("BSP_Torrens", fixture.Check("Production\\Frontend", false, now) ?? "");
        AssertEx.True(fixture.Check("Production\\Frontend", true, now) is null, "A level transition is a load in progress.");
        AssertEx.True(fixture.Check("Production\\BSP_TORRENS", false, now) is null, "The level already left the frontend.");
        fixture.Start("BSP_Torrens", now.AddSeconds(-5));
        AssertEx.True(fixture.Check("Production\\Frontend", false, now) is null, "A fresh start gets time to load.");
        return Task.CompletedTask;
    }

    internal static Task IgnoresSurvivalAndGameplayAsync()
    {
        using var fixture = new Fixture();
        var now = DateTimeOffset.UtcNow;
        var late = now - MultiplayerSessionCoordinator.CampaignStartMenuTimeout - TimeSpan.FromSeconds(1);
        fixture.Start("ChallengeMap16", late);
        AssertEx.True(fixture.Check("Production\\Frontend", false, now) is null, "Survival keeps its menu for character selection.");
        fixture.Start("BSP_Torrens", late);
        fixture.Set("_uiGameplayLoaded", true);
        AssertEx.True(fixture.Check("Production\\Frontend", false, now) is null, "Gameplay UI means the level loaded.");
        return Task.CompletedTask;
    }

    private sealed class Fixture : IDisposable
    {
        private readonly TemporaryDirectory _directory = new();
        private readonly LauncherDiagnostics _diagnostics;
        private readonly NativeRuntimeController _controller;
        private readonly ScaleformBridgeListener _bridge;
        private readonly MultiplayerSessionCoordinator _coordinator;

        internal Fixture()
        {
            _diagnostics = new LauncherDiagnostics(_directory.Combine("logs"));
            var settings = new LauncherSettingsStore(_directory.Combine("settings.json"));
            _controller = new NativeRuntimeController(_diagnostics, settings);
            _bridge = new ScaleformBridgeListener(_diagnostics);
            _coordinator = new MultiplayerSessionCoordinator(_controller, settings, _bridge, new PerformanceIsolationState());
        }

        internal void Set(string field, object? value) =>
            typeof(MultiplayerSessionCoordinator).GetField(field, Private)!.SetValue(_coordinator, value);

        internal void Start(string level, DateTimeOffset startedUtc)
        {
            var start = new LobbyStartPayload(Guid.NewGuid(), 1, level, 1);
            Set("_start", start);
            Set("_loadingHoldStart", start);
            Set("_loadingHoldStartedUtc", startedUtc);
            Set("_uiGameplayLoaded", false);
        }

        internal string? Check(string currentLevel, bool transitioning, DateTimeOffset now)
        {
            var runtime = NativeRuntimeSnapshot.Empty with { CurrentLevelName = currentLevel, LevelTransitioning = transitioning };
            return (string?)typeof(MultiplayerSessionCoordinator).GetMethod("CampaignStartStuckAtMenuLocked", Private)!
                .Invoke(_coordinator, [runtime, now]);
        }

        public void Dispose()
        {
            _coordinator.Dispose();
            _bridge.Dispose();
            _controller.Dispose();
            _diagnostics.Dispose();
            _directory.Dispose();
        }
    }
}
