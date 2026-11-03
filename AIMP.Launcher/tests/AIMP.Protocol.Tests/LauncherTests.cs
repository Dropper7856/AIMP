using AIMP.Launcher;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace AIMP.Protocol.Tests;

internal static class LauncherTests
{
    internal static Task NativeManifestVersionAgreementAsync()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var header = Path.Combine(directory.FullName, "AIMP.Native", "include", "aimp", "WorldReplicationTypes.h");
            if (!File.Exists(header)) continue;
            var native = System.Text.RegularExpressions.Regex.Match(File.ReadAllText(header),
                @"kWorldManifestCompilerVersion\s*=\s*0x([0-9A-Fa-f]+)U");
            AssertEx.True(native.Success, "Native WorldManifest compiler-version constant was not found.");
            AssertEx.Equal(WorldManifestConstants.CompilerVersion,
                Convert.ToUInt32(native.Groups[1].Value, 16),
                "The managed compiler and native manifest reader must accept the same generation.");
            return Task.CompletedTask;
        }
        throw new InvalidOperationException("Native WorldManifest version header was not found.");
    }

    internal static Task NativeBuildVersionAgreementAsync()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var header = Path.Combine(directory.FullName, "AIMP.Native", "include", "aimp", "ProductVersion.h");
            if (!File.Exists(header)) continue;
            AssertEx.True(File.ReadAllText(header).Contains($"\"{ProductInfo.ExpectedDllVersion}\"", StringComparison.Ordinal),
                $"Launcher expects {ProductInfo.ExpectedDllVersion}, which differs from the native build version.");
            return Task.CompletedTask;
        }
        throw new InvalidOperationException("Native product-version header was not found.");
    }

    internal static Task ManifestCacheIdentityAsync()
    {
        // TECH_COMMS: the compiler found this cache while lobby preflight searched
        // a different name because its SHA256 text was uppercase.
        const string pak = "b8f248e111e5ad956f5dede067b329f7eb81fda579f5db837cbc8579cd1c528a";
        var lower = WorldManifestCompiler.ComputeMapContentId(pak, string.Empty, true, false);
        var upper = WorldManifestCompiler.ComputeMapContentId(pak.ToUpperInvariant(), string.Empty, true, false);
        AssertEx.Equal("42E90BA4BB09E8F281D7423B1B1DF7FD", lower.ToString());
        AssertEx.Equal(lower, upper);
        AssertEx.Equal(WorldManifestCompiler.GetDefaultCachePath(lower), WorldManifestCompiler.GetDefaultCachePath(upper));
        AssertEx.True(lower != WorldManifestCompiler.ComputeMapContentId(new string('0', 64), string.Empty, true, false));
        return Task.CompletedTask;
    }

    internal static Task LobbyReadinessStatusAsync()
    {
        var session = Guid.NewGuid();
        var token = Guid.NewGuid();
        var host = new LobbyPlayer(1, Guid.NewGuid(), "Host", true, true);
        var client = new LobbyPlayer(2, Guid.NewGuid(), "Client", false, true);
        var identity = new WorldManifestIdentity(WorldManifestConstants.FormatVersion, WorldManifestConstants.CompilerVersion, new WorldEntityId(1, 2));
        var ready = new LobbyPreparationPayload(session, 1, "TECH_COMMS", token, identity, new string('a', 64), true, "");
        var lobby = new LobbySnapshotPayload(session, "Test", "TECH_COMMS", "Lobby", 2, 0,
            new[] { host, client }, token, new[] { ready }, true);
        AssertEx.Equal("Verifying Client's level", LobbyReadiness.Status(lobby));
        var peer = ready with { PeerId = 2 };
        AssertEx.Equal("Ready to start", LobbyReadiness.Status(lobby with { Preparations = new[] { ready, peer } }));
        AssertEx.Equal("Client: Missing manifest", LobbyReadiness.Status(lobby with
            { Preparations = new[] { ready, peer with { Ready = false, Error = "Missing manifest" } } }));
        AssertEx.Equal("Ready to start", LobbyReadiness.Status(lobby with
            { Preparations = new[] { ready, peer with { ManifestSha256 = new string('b', 64) } } }));
        AssertEx.Equal("Ready to start", LobbyReadiness.Status(lobby with
            { Preparations = new[] { ready, peer with { Level = "tech_comms", Manifest = identity with { MapContentId = new WorldEntityId(3,4) } } } }));
        AssertEx.Equal("Level mismatch: Client", LobbyReadiness.Status(lobby with
            { Preparations = new[] { ready, peer with { Level = "TECH_HUB" } } }));
        AssertEx.Equal("Verifying Client's level", LobbyReadiness.Status(lobby with
            { Preparations = new[] { ready, peer with { SessionId = Guid.NewGuid() } } }));
        return Task.CompletedTask;
    }

    internal static Task ProcessSelectionAsync()
    {
        var mainWindow = Target(10, 100, true);
        var newerBackground = Target(20, 300, false);
        var selected = AiProcessDiscovery.SelectCandidate(
            new[] { newerBackground, mainWindow });
        AssertEx.Equal(mainWindow, selected);

        var newest = Target(30, 500, false);
        selected = AiProcessDiscovery.SelectCandidate(
            new[] { newerBackground, newest });
        AssertEx.Equal(newest, selected);

        var invalid = newest with { ExecutablePath = @"C:\Game\Other.exe" };
        AssertEx.Throws<InvalidOperationException>(() =>
            AiProcessDiscovery.SelectCandidate(new[] { invalid }));
        return Task.CompletedTask;
    }

    internal static Task NativeDllPriorityAsync()
    {
        using var temporary = new TemporaryDirectory();
        var root = temporary.DirectoryPath;
        var launcher = temporary.Combine("AIMP.Launcher", "bin");
        var alpha = temporary.Combine("AIMP.Native", "out", "build", "alpha");
        var zeta = temporary.Combine("AIMP.Native", "out", "build", "zeta");
        Directory.CreateDirectory(launcher);
        Directory.CreateDirectory(alpha);
        Directory.CreateDirectory(zeta);

        var candidates = NativeDllResolver.BuildCandidates(launcher, root);
        AssertCandidateOrder(candidates, launcher, alpha, zeta);
        AssertResolutionPriority(launcher, alpha);
        AssertRecentReleaseBuild(launcher, alpha, zeta);
        return Task.CompletedTask;
    }

    internal static Task NativeModuleMatchingAsync()
    {
        var expected = Path.GetFullPath(@"C:\AIMP\AIMP.NetZ.dll");
        AssertEx.True(NativeModuleInspector.IsMatch(
            "AIMP.NetZ.dll",
            @"C:\Other\AIMP.NetZ.dll",
            expected));
        AssertEx.True(NativeModuleInspector.IsMatch(
            "renamed.dll",
            expected.ToUpperInvariant(),
            expected));
        AssertEx.False(NativeModuleInspector.IsMatch(
            "other.dll",
            @"C:\Other\other.dll",
            expected));
        return Task.CompletedTask;
    }

    internal static Task NativeDllDeploymentAsync()
    {
        var source = RequiredEnvironment("AIMP_TEST_NATIVE_DLL_SOURCE");
        var deployed = RequiredEnvironment("AIMP_TEST_NATIVE_DLL_DEPLOYED");
        AssertEx.True(File.Exists(source), $"Native build DLL is missing: {source}");
        AssertEx.True(File.Exists(deployed), $"Launcher DLL is missing: {deployed}");
        using var sourceStream = File.OpenRead(source);
        using var deployedStream = File.OpenRead(deployed);
        var sourceHash = Convert.ToHexString(SHA256.HashData(sourceStream));
        var deployedHash = Convert.ToHexString(SHA256.HashData(deployedStream));
        AssertEx.Equal(sourceHash, deployedHash,
            "The launcher contains a stale AIMP.NetZ.dll.");
        return Task.CompletedTask;
    }

    internal static async Task DiagnosticsExportAsync()
    {
        using var temporary = new TemporaryDirectory();
        using var diagnostics = new LauncherDiagnostics(temporary.Combine("logs"));
        diagnostics.Log("Test", "Engine thread captured.");
        diagnostics.Flush();

        var archivePath = temporary.Combine("exports", "diagnostics.zip");
        var snapshot = DiagnosticSnapshot();
        var result = await RuntimeDiagnosticsExporter.ExportAsync(
            archivePath,
            snapshot,
            diagnostics,
            null,
            CancellationToken.None).ConfigureAwait(false);

        AssertEx.Equal(Path.GetFullPath(archivePath), result);
        using var archive = ZipFile.OpenRead(result);
        AssertEx.SequenceEqual(
            new[] { "logs/launcher.log", "runtime-diagnostics.json" },
            archive.Entries.Select(value => value.FullName).Order(StringComparer.Ordinal));
        AssertDiagnosticReport(archive, snapshot);
    }

    internal static Task LocalPlayerPositionToolsAsync()
    {
        var addresses = LocalPlayerPositionMemoryLayout.Resolve(0x400000);
        AssertEx.Equal((nint)0x1707AC8, addresses.X);
        AssertEx.Equal((nint)0x1707AD8, addresses.Y);
        AssertEx.Equal((nint)0x1707AD0, addresses.Z);
        var bytes = new byte[LocalPlayerPositionMemoryLayout.BlockSize];
        BitConverter.GetBytes(1.25f).CopyTo(bytes, 0);
        BitConverter.GetBytes(-2.5f).CopyTo(bytes, 0x08);
        BitConverter.GetBytes(42.0f).CopyTo(bytes, 0x10);
        AssertEx.True(LocalPlayerPositionMemoryLayout.TryDecode(bytes, out var position));
        AssertEx.Equal(new LocalPlayerPosition(1.25f, 42.0f, -2.5f), position);
        AssertEx.Equal("X=1.250  Y=42.000  Z=-2.500", position.ToDisplayText());
        AssertLocalPlayerYawTools(position);
        AssertLocalPlayerMovementProfileTools();
        return Task.CompletedTask;
    }

    private static void AssertLocalPlayerYawTools(LocalPlayerPosition position)
    {
        var pointerAddress = LocalPlayerYawMemoryLayout.ResolveCharacterBasePointer(0x400000);
        AssertEx.Equal((nint)0x1707A48, pointerAddress);
        var pointerBytes = BitConverter.GetBytes(0x2BA54150u);
        AssertEx.True(LocalPlayerYawMemoryLayout.TryDecodeCharacterBase(pointerBytes, out var character));
        AssertEx.Equal((nint)0x2BA54150, character);
        AssertEx.Equal((nint)0x2BA542C4, LocalPlayerYawMemoryLayout.ResolveYaw(character));
        var yawBytes = BitConverter.GetBytes(2.136945248f);
        AssertEx.True(LocalPlayerYawMemoryLayout.TryDecodeYaw(yawBytes, out var yaw));
        AssertEx.Equal("X=1.250  Y=42.000  Z=-2.500  Yaw=2.137 rad", position.ToDisplayText(yaw));
        AssertEx.False(LocalPlayerYawMemoryLayout.TryDecodeYaw(BitConverter.GetBytes(4.0f), out _));
    }

    private static void AssertLocalPlayerMovementProfileTools()
    {
        var profile = new LocalPlayerMovementProfile(
            PlayerMoveType.Walk,
            PlayerMoveType.Walk,
            0.60F,
            1.15F,
            2.00F,
            3.20F);
        AssertEx.Equal(0.60F, profile.SpeedFor(PlayerMoveType.SlowWalk));
        AssertEx.Equal(3.20F, profile.SpeedFor(PlayerMoveType.Run));
        AssertEx.Equal(PlayerMoveType.Walk, profile.ResolveEffectiveMoveType(1.20F));
        AssertEx.Equal(PlayerMoveType.FastWalk, profile.ResolveEffectiveMoveType(2.05F));
        AssertEx.Equal(PlayerMoveType.Run, profile.ResolveEffectiveMoveType(3.00F));

        var requestedRun = profile with { RequestedMoveType = PlayerMoveType.Run };
        AssertEx.Equal(PlayerMoveType.Run, requestedRun.ResolveEffectiveMoveType(1.20F));
    }

    internal static Task LauncherIntegrationToolsAsync()
    {
        using var temporary = new TemporaryDirectory();
        var game = temporary.Combine("game");
        Directory.CreateDirectory(Path.Combine(game, "DATA"));
        File.WriteAllBytes(Path.Combine(game, "AI.exe"), new byte[] { 1 });
        File.WriteAllBytes(Path.Combine(game, "DATA", "UI.PAK"), new byte[] { 2 });
        AssertEx.True(GameInstallation.TryResolve(game, out var paths, out var error), error);
        AssertEx.Equal(Path.GetFullPath(game), paths.GameDirectory);
        AssertSettingsRoundTrip(temporary, paths.GameDirectory);
        AssertBridgeProtocol();
        AssertMultiplayerTools();
        AssertUiAssets();
        return Task.CompletedTask;
    }

    private static void AssertMultiplayerTools()
    {
        AssertEx.True(MultiplayerSettingsValidation.TryValidate(
            "Ripley", "10.0.0.5", 28015, out var name, out var host, out var port, out var error),
            error);
        AssertEx.Equal("Ripley", name);
        AssertEx.Equal("10.0.0.5", host);
        AssertEx.Equal(28015, port);
        var compatibility = LobbyCompatibility.Create(LauncherSettings.Empty);
        AssertEx.True(compatibility.LauncherVersion.Length <= 64);
        AssertEx.True(compatibility.DllVersion.Length <= 64);
        AssertEx.True(compatibility.ProfileId.Length <= 128);
        ModelValidation.Validate(compatibility);
        var networkPosition = RemoteSpawnPlanner.CreateNetworkPosition(
            7, new LocalPlayerPosition(1.0F, 2.0F, 3.0F));
        var liveSnapshot = new PlayerPositionSnapshotPayload(
            Guid.NewGuid(), 7, 3, 99, 123456, 4.0F, 5.0F, 6.0F, 0.5F,
            PlayerMoveType.Walk, RemoteMovementPlanner.LegacyWalkFallbackMagnitude);
        var liveNetworkPosition = RemoteSpawnPlanner.CreateNetworkPosition(liveSnapshot);
        AssertEx.Equal(4.0F, liveNetworkPosition.X);
        AssertEx.Equal(5.0F, liveNetworkPosition.Y);
        AssertEx.Equal(6.0F, liveNetworkPosition.Z);
        var spawn = RemoteSpawnPlanner.Create(networkPosition);
        AssertEx.Equal("1.000", spawn["x"]);
        AssertEx.Equal("2.000", spawn["y"]);
        AssertEx.Equal("3.000", spawn["z"]);
        var leftMessage = RemoteSpawnPlanner.CreateLeftMessage("Ripley");
        AssertEx.Equal("Ripley left the game", leftMessage["mainText"]);
        var joinMessage = RemoteSpawnPlanner.CreateJoinMessage("Ripley");
        AssertEx.Equal("AIMP", joinMessage["headerText"]);
        AssertEx.Equal("Ripley join the game", joinMessage["mainText"]);
        AssertEx.Equal("5.0", joinMessage["duration"]);
        AssertEx.Equal(PopupMessageIcons.Alert, joinMessage["icon"]);
        AssertEx.Equal(PopupMessageSoundEvents.ObjectiveNew, joinMessage["soundEvent"]);
        AssertRemoteSpawnRoster();
        AssertLauncherSpawnCommand();
        AssertGameplayReadinessGate();
        AssertPositionReplicationTools();
    }

    private static void AssertRemoteSpawnRoster()
    {
        var local = new LobbyPlayer(1, Guid.NewGuid(), "Local", true, true);
        var remoteA = new LobbyPlayer(2, Guid.NewGuid(), "Remote A", false, true);
        var remoteB = new LobbyPlayer(3, Guid.NewGuid(), "Remote B", false, true);
        var lobby = new LobbySnapshotPayload(Guid.NewGuid(), "Test", "Map", "Starting",
            4, 1, new[] { local, remoteA, remoteB });
        var positions = new[]
        {
            new PlayerSpawnPositionPayload(1, 10, 20, 30),
            new PlayerSpawnPositionPayload(2, 40, 50, 60),
            new PlayerSpawnPositionPayload(3, 70, 80, 90)
        };
        var existing = new Dictionary<ulong, uint> { [2] = 0x2C000000 };
        var pending = RemoteSpawnPlanner.SelectPendingPlayers(lobby, positions, 1, existing);
        AssertEx.SequenceEqual(new[] { 3UL }, pending.Select(value => value.Player.PeerId));
        AssertEx.Equal(70F, pending.Single().Position.X);
        AssertEx.True(RemoteSpawnPlanner.MatchesLobby(positions, lobby.Players));
        AssertEx.False(RemoteSpawnPlanner.AllRemotePlayersReady(
            lobby, 1, new HashSet<ulong> { 2 }));
        AssertEx.True(RemoteSpawnPlanner.AllRemotePlayersReady(
            lobby, 1, new HashSet<ulong> { 2, 3 }));
    }

    private static void AssertLauncherSpawnCommand()
    {
        var command = GameCommandCatalog.Commands.Single(value =>
            value.Name == LauncherGameCommandNames.SpawnConnectedPlayers);
        AssertEx.Equal(GameCommandExecutionTarget.Multiplayer, command.Target);
        AssertEx.Equal(0, command.Parameters.Count);

        var worldProbe = GameCommandCatalog.Commands.Single(value =>
            value.Name == LauncherGameCommandNames.WorldNetProbe);
        AssertEx.Equal(GameCommandExecutionTarget.Multiplayer, worldProbe.Target);
        AssertEx.Equal(1, worldProbe.Parameters.Count);
        AssertEx.Equal("manifestPath", worldProbe.Parameters[0].Key);


    }

    private static void AssertGameplayReadinessGate()
    {
        var gate = new GameplaySpawnReadinessGate();
        var start = new DateTimeOffset(2026, 8, 5, 12, 0, 0, TimeSpan.Zero);
        var position = new LocalPlayerPosition(1.0F, 2.0F, 3.0F);
        gate.MarkGameplayUiLoaded(start);
        ObservePositionSamples(gate, start, position, 39);
        AssertEx.False(gate.IsReady(start.AddSeconds(1.95)));
        gate.Observe(start.AddSeconds(2), true, position);
        AssertEx.True(gate.IsReady(start.AddSeconds(2)));
        gate.Observe(start.AddSeconds(2.05), true, new LocalPlayerPosition(20, 2, 3));
        AssertEx.False(gate.IsReady(start.AddSeconds(2.05)));
    }

    private static void AssertPositionReplicationTools()
    {
        var bytes = new byte[RemoteCharacterPositionMemoryLayout.BlockSize];
        BitConverter.GetBytes(1.25F).CopyTo(bytes, 0);
        BitConverter.GetBytes(2.5F).CopyTo(bytes, 4);
        BitConverter.GetBytes(-3.75F).CopyTo(bytes, 8);
        AssertEx.True(RemoteCharacterPositionMemoryLayout.TryDecode(bytes, out var decoded));
        AssertEx.Equal(new LocalPlayerPosition(1.25F, 2.5F, -3.75F), decoded);
        AssertRemoteMovementPlan();
    }

    private static void AssertRemoteMovementPlan()
    {
        AssertEx.Equal(TimeSpan.FromMilliseconds(33),
            RemoteMovementPlanner.MovementHeartbeat);
        AssertEx.Equal(TimeSpan.FromMilliseconds(33),
            RemoteMovementPlanner.OrientationHeartbeat);
        AssertEx.Equal(TimeSpan.FromMilliseconds(25),
            RemoteMovementPlanner.MinimumCommandInterval);
        AssertEx.True(MathF.Abs(
            RemoteMovementPlanner.LegacyWalkFallbackMagnitude - 1.15073F) < 0.000001F);
        AssertEx.True(MathF.Abs(
            RemoteMovementPlanner.BaseWalkIntensity - 1.100F) < 0.000001F);
        var session = Guid.NewGuid();
        var now = new DateTimeOffset(2026, 8, 6, 12, 0, 0, TimeSpan.Zero);
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(session, 1, 0, 0), now));
        AssertEx.True(track.TryAccept(Position(session, 2, 1, 0), now.AddMilliseconds(50)));
        var plan = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(50));
        AssertEx.Equal(RemoteMovementAction.Move, plan.Action);
        AssertEx.True(plan.DirectionX > 0);
        AssertEx.True(MathF.Abs(plan.DirectionZ) < 0.0001F);
        AssertEx.True(MathF.Abs(
            MathF.Sqrt(plan.DirectionX * plan.DirectionX +
                plan.DirectionZ * plan.DirectionZ) -
            RemoteMovementPlanner.LegacyWalkFallbackMagnitude) < 0.0001F);
        AssertEx.Equal(RemoteMovementPlanner.BaseWalkIntensity, plan.Intensity);
        AssertEx.Equal(1F, plan.PositionX);
        AssertEx.Equal(0F, plan.PositionY);
        AssertEx.Equal(0F, plan.PositionZ);
        AssertMovementArguments(plan);
        track.MarkCommand(plan, now.AddMilliseconds(50));

        var fastTrack = new RemotePositionTrack();
        AssertEx.True(fastTrack.TryAccept(
            Position(session, 101, 0, 0, 0F, PlayerMoveType.FastWalk, 2.25F), now));
        AssertEx.True(fastTrack.TryAccept(
            Position(session, 102, 0.10F, 0, 0F, PlayerMoveType.FastWalk, 2.25F),
            now.AddMilliseconds(50)));
        var fastPlan = RemoteMovementPlanner.Create(
            fastTrack.Snapshot()!, now.AddMilliseconds(50));
        AssertEx.Equal(RemoteMovementAction.Move, fastPlan.Action);
        AssertEx.Equal(PlayerMoveType.FastWalk, fastPlan.MoveType);
        AssertEx.True(MathF.Abs(fastPlan.NativeMoveSpeed - 2.25F) < 0.0001F);
        AssertEx.True(MathF.Abs(
            MathF.Sqrt(fastPlan.DirectionX * fastPlan.DirectionX +
                fastPlan.DirectionZ * fastPlan.DirectionZ) - 2.25F) < 0.0001F);
        AssertMovementArguments(fastPlan);

        var unchanged = RemoteMovementPlanner.Create(
            track.Snapshot()!, now.AddMilliseconds(60));
        AssertEx.Equal(RemoteMovementAction.None, unchanged.Action);

        var heartbeat = RemoteMovementPlanner.Create(
            track.Snapshot()!, now.AddMilliseconds(83));
        AssertEx.Equal(RemoteMovementAction.Move, heartbeat.Action);
        track.MarkCommand(heartbeat, now.AddMilliseconds(83));

        AssertEx.True(track.TryAccept(Position(session, 3, 0, 0), now.AddMilliseconds(200)));
        var reverse = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(200));
        AssertEx.Equal(RemoteMovementAction.Move, reverse.Action);
        AssertEx.True(reverse.DirectionX < 0);
        track.MarkCommand(reverse, now.AddMilliseconds(200));

        var stale = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddSeconds(1));
        AssertEx.Equal(RemoteMovementAction.Stop, stale.Action);

        AssertEx.True(track.TryAccept(Position(session, 4, 0, 0), now.AddMilliseconds(216)));
        AssertEx.True(track.TryAccept(Position(session, 5, 0, 0), now.AddMilliseconds(232)));
        AssertEx.True(track.TryAccept(Position(session, 6, 0, 0), now.AddMilliseconds(248)));
        AssertEx.True(track.TryAccept(Position(session, 7, 0, 0), now.AddMilliseconds(264)));
        var stopped = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(264));
        AssertEx.Equal(RemoteMovementAction.Stop, stopped.Action);

        AssertRemoteMovementJitterScenario(session, now);
        AssertRemoteMovementDiscontinuity(session, now);
        AssertRemoteYawScenario(session, now);
    }

    private static void AssertRemoteMovementJitterScenario(
        Guid session,
        DateTimeOffset now)
    {
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(session, 50, 0.00F, 0.00F), now));
        AssertEx.True(track.TryAccept(Position(session, 51, 0.08F, 0.00F),
            now.AddMilliseconds(43)));
        AssertEx.True(track.TryAccept(Position(session, 53, 0.24F, 0.01F),
            now.AddMilliseconds(121)));
        AssertEx.False(track.TryAccept(Position(session, 52, 0.16F, 0.00F),
            now.AddMilliseconds(130)));
        AssertEx.True(track.TryAccept(Position(session, 54, 0.32F, 0.01F),
            now.AddMilliseconds(177)));

        var plan = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(190));
        AssertEx.True(plan.Action is RemoteMovementAction.Move or RemoteMovementAction.None);
        AssertEx.True(float.IsFinite(plan.DirectionX));
        AssertEx.True(float.IsFinite(plan.DirectionZ));
        AssertEx.True(float.IsFinite(plan.PositionX));
        AssertEx.True(float.IsFinite(plan.PositionY));
        AssertEx.True(float.IsFinite(plan.PositionZ));
        AssertEx.True(plan.DirectionX > 0F);
        AssertEx.Equal(RemoteMovementPlanner.BaseWalkIntensity, plan.Intensity);
    }

    private static void AssertRemoteMovementDiscontinuity(
        Guid session,
        DateTimeOffset now)
    {
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(session, 60, 0.0F, 0.0F), now));
        AssertEx.True(track.TryAccept(Position(session, 61, 5.0F, 0.0F),
            now.AddMilliseconds(50)));
        var plan = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(50));
        AssertEx.Equal(RemoteMovementAction.Stop, plan.Action);
        AssertEx.Equal(5.0F, plan.PositionX);
        var arguments = RemoteMovementCommandFactory.Create(0x2C000000, plan);
        AssertEx.Equal("0", arguments["frameCount"]);
        AssertEx.Equal("5.000000", arguments["replicationPositionX"]);
        AssertEx.Equal("0.000000", arguments["replicationPositionY"]);
        AssertEx.Equal("0.000000", arguments["replicationPositionZ"]);
    }


    private static void AssertRemoteYawScenario(
        Guid session,
        DateTimeOffset now)
    {
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(session, 70, 0F, 0F, 0.10F), now));
        var initial = RemoteMovementPlanner.Create(track.Snapshot()!, now);
        AssertEx.Equal(RemoteMovementAction.Stop, initial.Action);
        track.MarkCommand(initial, now);
        AssertEx.True(track.TryAccept(Position(session, 71, 0F, 0F, 0.60F),
            now.AddMilliseconds(50)));
        var rotated = RemoteMovementPlanner.Create(track.Snapshot()!, now.AddMilliseconds(50));
        AssertEx.Equal(RemoteMovementAction.Stop, rotated.Action);
        AssertEx.Equal(0.60F, rotated.Yaw);
    }

    private static PlayerPositionSnapshotPayload Position(
        Guid session,
        ulong sequence,
        float x,
        float z,
        float yaw = 0F,
        PlayerMoveType moveType = PlayerMoveType.Walk,
        float nativeMoveSpeed = RemoteMovementPlanner.LegacyWalkFallbackMagnitude) =>
        new(session, 2, 1, sequence,
            1_700_000_000_000 + (long)sequence * 50L,
            x, 0, z, yaw, moveType, nativeMoveSpeed);

    private static void AssertMovementArguments(RemoteMovementPlan plan)
    {
        var arguments = RemoteMovementCommandFactory.Create(0x2C000000, plan);
        AssertEx.Equal("0x2C000000", arguments["characterAddress"]);
        AssertEx.Equal("3600", arguments["frameCount"]);
        AssertEx.Equal("world", arguments["directionSpace"]);
        AssertEx.Equal(plan.Intensity.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture),
            arguments["intensity"]);
        AssertEx.Equal(plan.PositionX.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture),
            arguments["replicationPositionX"]);
        AssertEx.Equal(plan.PositionY.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture),
            arguments["replicationPositionY"]);
        AssertEx.Equal(plan.PositionZ.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture),
            arguments["replicationPositionZ"]);
        AssertEx.Equal(plan.Yaw.ToString("0.000000", System.Globalization.CultureInfo.InvariantCulture),
            arguments["replicationYaw"]);
        AssertEx.Equal(((int)plan.MoveType).ToString(
            System.Globalization.CultureInfo.InvariantCulture), arguments["moveType"]);

        var stop = RemoteMovementCommandFactory.CreateStop(0x2C000000);
        AssertEx.Equal("0", stop["frameCount"]);
        AssertEx.Equal("world", stop["directionSpace"]);
        AssertEx.False(stop.ContainsKey("replicationPositionX"));
        AssertEx.False(stop.ContainsKey("replicationPositionY"));
        AssertEx.False(stop.ContainsKey("replicationPositionZ"));
        AssertEx.False(stop.ContainsKey("replicationYaw"));
        var yawStop = RemoteMovementCommandFactory.CreateStop(0x2C000000, plan.Yaw);
        AssertEx.Equal(plan.Yaw.ToString("0.000000",
            System.Globalization.CultureInfo.InvariantCulture), yawStop["replicationYaw"]);
        var authoritativeStop = RemoteMovementCommandFactory.Create(
            0x2C000000, plan with { Action = RemoteMovementAction.Stop });
        AssertEx.Equal("0", authoritativeStop["frameCount"]);
        AssertEx.Equal(plan.PositionX.ToString("0.000000",
            System.Globalization.CultureInfo.InvariantCulture),
            authoritativeStop["replicationPositionX"]);
        AssertEx.Equal(plan.PositionY.ToString("0.000000",
            System.Globalization.CultureInfo.InvariantCulture),
            authoritativeStop["replicationPositionY"]);
        AssertEx.Equal(plan.PositionZ.ToString("0.000000",
            System.Globalization.CultureInfo.InvariantCulture),
            authoritativeStop["replicationPositionZ"]);
        AssertEx.Equal(((int)plan.MoveType).ToString(
            System.Globalization.CultureInfo.InvariantCulture),
            authoritativeStop["moveType"]);
    }

    private static void ObservePositionSamples(
        GameplaySpawnReadinessGate gate,
        DateTimeOffset start,
        LocalPlayerPosition position,
        int count)
    {
        for (var index = 1; index <= count; index++)
            gate.Observe(start.AddMilliseconds(index * 50), true, position);
    }

    private static void AssertSettingsRoundTrip(TemporaryDirectory temporary, string game)
    {
        var store = new LauncherSettingsStore(temporary.Combine("settings.json"));
        var playerId = Guid.NewGuid();
        store.Save(new LauncherSettings(game, "Ripley", "10.0.0.5", 28015, playerId));
        var loaded = store.Load();
        AssertEx.Equal(game, loaded.GameDirectory);
        AssertEx.Equal("Ripley", loaded.PlayerName);
        AssertEx.Equal("10.0.0.5", loaded.HostAddress);
        AssertEx.Equal(28015, loaded.Port);
        AssertEx.Equal(playerId, loaded.PlayerId);
        AssertEx.Equal(new VoiceSettings(), loaded.Voice!);
        var voice = new VoiceSettings(false, "voiceActivity", "persistent-interface", 0xA3, 0.1f, 1.5f, 0.4f);
        store.Save(loaded with { Voice = voice, DeveloperMode = true });
        loaded = store.Load();
        AssertEx.Equal(voice, loaded.Voice!);
        AssertEx.True(loaded.DeveloperMode);
        AssertEx.True(voice.TryValidate(out _));
        foreach (var invalid in new[]
        {
            voice with { Mode = "unknown" }, voice with { MicrophoneId = "" },
            voice with { PushToTalkKey = 256 }, voice with { ActivationThreshold = float.NaN },
            voice with { InputGain = 4.1f }, voice with { PlaybackVolume = -1 },
            voice with { PlaybackVolume = 3.01f }, voice with { PlaybackVolume = float.NaN }
        }) AssertEx.False(invalid.TryValidate(out _));
        var amplifiedVoice = voice with { PlaybackVolume = 3f };
        AssertEx.True(amplifiedVoice.TryValidate(out _));
        AssertEx.Equal(3f, amplifiedVoice.Normalize().PlaybackVolume);
        store.Save(loaded with { Voice = amplifiedVoice });
        AssertEx.Equal(3f, store.Load().Voice!.PlaybackVolume);
        AssertEx.Equal(3f, (voice with { PlaybackVolume = 4f }).Normalize().PlaybackVolume);
        AssertEx.Equal(0.02f, (voice with { ActivationThreshold = float.NaN }).Normalize().ActivationThreshold);

        File.WriteAllText(temporary.Combine("legacy-settings.json"),
            JsonSerializer.Serialize(new
            {
                GameDirectory = game,
                PlayerName = "Legacy",
                HostAddress = "127.0.0.1",
                Port = 28015,
                PlayerId = playerId
            }));
        var legacyStore = new LauncherSettingsStore(temporary.Combine("legacy-settings.json"));
        AssertEx.Equal(new VoiceSettings(), legacyStore.Load().Voice!);

        // Settings written while WorldSync was a user toggle still load; the
        // retired properties are ignored because WorldSync is always on.
        File.WriteAllText(temporary.Combine("toggle-settings.json"),
            JsonSerializer.Serialize(new
            {
                GameDirectory = game,
                PlayerName = "Toggle",
                WorldSyncEnabled = false,
                WorldSync3EmptyCallbackEnabled = false,
                DeveloperMode = true
            }));
        var toggleSettings = new LauncherSettingsStore(temporary.Combine("toggle-settings.json")).Load();
        AssertEx.Equal("Toggle", toggleSettings.PlayerName);
        AssertEx.True(toggleSettings.DeveloperMode);
    }

    private static void AssertBridgeProtocol()
    {
        var protocol = new ScaleformBridgeProtocol();
        var message = protocol.Parse("CREATE_LOBBY|serverName=Test%7CLobby|maxPlayers=4");
        AssertEx.Equal("CREATE_LOBBY", message.Type);
        AssertEx.Equal("Test|Lobby", message.Get("serverName"));
        AssertEx.Equal("4", message.Get("maxPlayers"));
        var built = protocol.Build("CONFIG", new Dictionary<string, string>
        { ["playerName"] = "Ripley|Host", ["hostIp"] = "10.0.0.5" });
        AssertEx.True(built.Contains("playerName=Ripley%7CHost", StringComparison.Ordinal));
    }

    private static void AssertUiAssets()
    {
        var assets = UiBridgeAssetCatalog.Resolve(@"C:\AIMP");
        AssertEx.Equal(5, assets.Count);
        AssertEx.SequenceEqual(new[] { "CHALLENGEMAPOVERLAY.GFX", "LOADINGSCREEN.GFX",
            "NEWFRONTENDMENU.GFX", "PAUSEMENU.GFX", "PICKUPOVERLAY.GFX" },
            assets.Select(value => Path.GetFileName(value.SourcePath)).Order(StringComparer.Ordinal));
        AssertEx.True(assets.All(value => value.PakPath.StartsWith("DATA/UI/",
            StringComparison.Ordinal)));
    }

    internal static Task CommandResultDiagnosticsAsync()
    {
        var result = new GameCommandResultPayload(1, Guid.NewGuid(), 0, Guid.NewGuid(),
            GameCommandNames.PopupMessage, "INVALID_COMMAND",
            "The command is invalid or its native function is unresolved.", false);
        var text = NativeRuntimeController.CommandResultText(result);
        AssertEx.True(text.Contains("native function is unresolved", StringComparison.Ordinal));
        AssertEx.True(text.Contains("Engine-thread readiness is not the blocker",
            StringComparison.Ordinal));
        return Task.CompletedTask;
    }

    internal static Task CharacterMovementToolsAsync()
    {
        AssertEx.Equal(0x2BF769A0U,
            CharacterAddressCalculator.Calculate(0x2BF79F10U));
        AssertEx.Throws<InvalidOperationException>(() =>
            CharacterAddressCalculator.Calculate(0x1000U));
        return Task.CompletedTask;
    }

    private static AiProcessTarget Target(int processId, long started, bool mainWindow) =>
        new(processId, @"C:\Game\AI.exe", 0x400000, started, mainWindow);

    private static string RequiredEnvironment(string name) =>
        Environment.GetEnvironmentVariable(name) is { Length: > 0 } value
            ? Path.GetFullPath(value)
            : throw new InvalidOperationException($"Required test path is missing: {name}");

    private static void AssertCandidateOrder(
        IReadOnlyList<string> candidates,
        string launcher,
        string alpha,
        string zeta)
    {
        var expected = new[]
        {
            Path.Combine(launcher, "AIMP.NetZ.dll"),
            Path.Combine(launcher, "Native", "AIMP.NetZ.dll"),
            Path.Combine(alpha, "bin", "Release", "AIMP.NetZ.dll"),
            Path.Combine(alpha, "Release", "AIMP.NetZ.dll"),
            Path.Combine(zeta, "bin", "Release", "AIMP.NetZ.dll"),
            Path.Combine(zeta, "Release", "AIMP.NetZ.dll"),
            Path.Combine(alpha, "bin", "Debug", "AIMP.NetZ.dll"),
            Path.Combine(alpha, "Debug", "AIMP.NetZ.dll")
        };
        AssertEx.SequenceEqual(expected, candidates.Take(expected.Length));
    }

    private static void AssertResolutionPriority(string launcher, string alpha)
    {
        var release = Path.Combine(alpha, "bin", "Release", "AIMP.NetZ.dll");
        CreateFile(release);
        AssertEx.Equal(release, NativeDllResolver.Resolve(launcher));

        var native = Path.Combine(launcher, "Native", "AIMP.NetZ.dll");
        CreateFile(native);
        AssertEx.Equal(native, NativeDllResolver.Resolve(launcher));

        var local = Path.Combine(launcher, "AIMP.NetZ.dll");
        CreateFile(local);
        AssertEx.Equal(local, NativeDllResolver.Resolve(launcher));
    }

    private static void CreateFile(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, new byte[] { 1 });
    }

    private static void AssertRecentReleaseBuild(string launcher, string alpha, string zeta)
    {
        var older = Path.Combine(alpha, "bin", "Release", "AIMP.NetZ.dll");
        var newer = Path.Combine(zeta, "bin", "Release", "AIMP.NetZ.dll");
        var debug = Path.Combine(alpha, "bin", "Debug", "AIMP.NetZ.dll");
        var local = Path.Combine(launcher, "AIMP.NetZ.dll");
        CreateFile(newer); CreateFile(debug);
        File.WriteAllBytes(newer, new byte[] { 2 });
        var time = new DateTime(2026, 10, 13, 12, 0, 0, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(older, time.AddDays(-1));
        File.SetLastWriteTimeUtc(newer, time);
        File.SetLastWriteTimeUtc(debug, time.AddDays(1));
        AssertEx.Equal(newer, NativeDllResolver.Resolve(launcher));
        // Release still precedes a newer Debug, and matching local bytes win.
        File.Copy(newer, local, overwrite: true);
        AssertEx.Equal(local, NativeDllResolver.Resolve(launcher));
        // Equal timestamps retain deterministic alphabetical fallback.
        File.WriteAllBytes(local, new byte[] { 3 });
        File.SetLastWriteTimeUtc(older, time);
        AssertEx.Equal(older, NativeDllResolver.Resolve(launcher));
    }

    private static NativeRuntimeSnapshot DiagnosticSnapshot() => new(
        true,
        4242,
        @"C:\Game\AI.exe",
        0x400000,
        @"C:\AIMP\AIMP.NetZ.dll",
        true,
        true,
        ProductInfo.ExpectedDllVersion,
        true,
        true,
        false,
        true,
        true,
        37,
        "MainThreadCharacterProcessing",
        DateTimeOffset.Parse("2026-01-02T03:04:05Z"),
        DateTimeOffset.Parse("2026-01-02T03:04:06Z"),
        100,
        200,
        2,
        "PopupMessage",
        "Command queued",
        "none",
        7,
        @"C:\AIMP\native.log");

    private static void AssertDiagnosticReport(
        ZipArchive archive,
        NativeRuntimeSnapshot snapshot)
    {
        var entry = archive.GetEntry("runtime-diagnostics.json")
            ?? throw new InvalidOperationException("Diagnostic report is missing.");
        using var stream = entry.Open();
        using var document = JsonDocument.Parse(stream);
        var report = document.RootElement;
        AssertEx.Equal(ProductInfo.LauncherVersion,
            report.GetProperty("LauncherVersion").GetString()!);
        AssertEx.Equal(snapshot.NativeDllVersion,
            report.GetProperty("NativeDllVersion").GetString()!);
        AssertEx.Equal(snapshot.ProcessId, report.GetProperty("AiExePid").GetInt32());
        AssertEx.Equal("0x400000", report.GetProperty("AiExeModuleBase").GetString()!);
        AssertEx.True(report.GetProperty("EngineThreadReady").GetBoolean());
        AssertEx.True(report.GetProperty(
            "CharacterReadinessProducerHookInstalled").GetBoolean());
        AssertEx.Equal(snapshot.EngineThreadId,
            report.GetProperty("EngineThreadId").GetInt32());
        AssertEx.Equal(snapshot.PendingCommandCount,
            report.GetProperty("PendingCommandCount").GetInt32());
        AssertEx.True(report.GetProperty("LauncherLogs").GetArrayLength() >= 2);
    }

}
