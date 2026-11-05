using AIMP.Launcher;
using System.Reflection;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class PlayerTorchTests
{
    private static readonly Guid Session = Guid.Parse("d4b36fc5-c75c-479f-911b-bcc509efb874");
    private static readonly DateTimeOffset Now = DateTimeOffset.FromUnixTimeMilliseconds(1_800_000_000_000);
    private static readonly PlayerTorchSnapshot On = new(true), Off = new(false);
    private static readonly PlayerPostureSnapshot Crouched = new(true, false, false, 0, 0, 0, 0, 0, 0, false);

    private static PlayerPositionSnapshotPayload Position(ulong sequence, PlayerTorchSnapshot? torch,
        PlayerPostureSnapshot? posture = null) =>
        new(Session, 2, 1, sequence, Now.ToUnixTimeMilliseconds() + (long)sequence,
            10, 20, 30, 0.5F, PlayerMoveType.Walk, 1.15073F, posture, torch);

    internal static Task WireAsync()
    {
        AssertEx.Equal((ushort)30, ProtocolConstants.Version);
        foreach (var torch in new PlayerTorchSnapshot?[] { null, Off, On, new(true, 0, 0.6F, 0.8F), new(true, 0, -1, 0) })
        {
            var position = Position(1, torch, Crouched);
            ModelValidation.Validate(position);
            AssertEx.Equal(position, JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(position)));
        }
        var omitted = Encoding.UTF8.GetString(JsonPayload.Serialize(Position(1, null)))
            .Replace(",\"torch\":null", "", StringComparison.Ordinal);
        AssertEx.True(JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(Encoding.UTF8.GetBytes(omitted)).Torch is null);
        foreach (var json in new[] { "{}", "{\"enabled\":null}", "{\"enabled\":1}", "{\"enabled\":\"true\"}", "{\"enabled\":true,\"extra\":0}" })
            AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<PlayerTorchSnapshot>(Encoding.UTF8.GetBytes(json)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate((PlayerTorchSnapshot?)null));
        foreach (var invalid in new PlayerTorchSnapshot[] {
            new(true, 0, null, 1), new(true, 0, 0, 0), new(true, 2, 0, 0),
            new(true, float.NaN, 0, 1), new(true, 0, float.PositiveInfinity, 1), new(false, 0, 0, 1) })
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(invalid));
        var frame = FrameCodec.Encode(new AimpFrame(MessageType.PlayerPositionSnapshot, 0, 1,
            JsonPayload.Serialize(Position(1, On))));
        frame[4] = 8; frame[5] = 0;
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(frame));
        return Task.CompletedTask;
    }

    internal static Task CaptureAsync()
    {
        const int character = 0x100000, equipment = 0x200000;
        var memory = new Dictionary<nint, byte[]>
        {
            [character + LocalPlayerTorchReader.EnabledOffset] = new byte[] { 1 },
            [character + LocalPlayerTorchReader.EquipmentOffset] = BitConverter.GetBytes(equipment),
            [equipment + LocalPlayerTorchReader.LightOffset] = BitConverter.GetBytes(0x300000),
            [equipment + LocalPlayerTorchReader.EmittedOffset] = new byte[] { 1 }
        };
        var reads = 0;
        var tear = false;
        var reader = new LocalPlayerTorchReader((address, output) =>
        {
            if (!memory.TryGetValue(address, out var value) || value.Length != output.Length) return false;
            value.CopyTo(output, 0);
            if (tear && ++reads == 4) memory[character + LocalPlayerTorchReader.EnabledOffset][0] = 0;
            return true;
        });
        AssertEx.True(reader.TryCapture(character, out var on));
        AssertEx.Equal(On, on!);
        memory[equipment + LocalPlayerTorchReader.EmittedOffset][0] = 0;
        AssertEx.True(reader.TryCapture(character, out var suppressed));
        AssertEx.Equal(Off, suppressed!); // requested on, native beam suppressed
        memory[equipment + LocalPlayerTorchReader.EmittedOffset][0] = 1;
        memory[equipment + LocalPlayerTorchReader.LightOffset] = BitConverter.GetBytes(0);
        AssertEx.True(reader.TryCapture(character, out var absent));
        AssertEx.Equal(Off, absent!);
        memory[equipment + LocalPlayerTorchReader.LightOffset] = BitConverter.GetBytes(0x300000);
        tear = true;
        AssertEx.False(reader.TryCapture(character, out var torn));
        AssertEx.True(torn is null);
        memory.Remove(character + LocalPlayerTorchReader.EquipmentOffset);
        AssertEx.False(reader.TryCapture(character, out var unreadable));
        AssertEx.True(unreadable is null); // unavailable never invents an off edge
        AssertAimCapture();
        return Task.CompletedTask;
    }

    private static void AssertAimCapture()
    {
        const int module = 0x400000, character = 0x100000, equipment = 0x200000;
        const int engine = 0x500000, manager = 0x600000, camera = 0x700000;
        var memory = new Dictionary<nint, byte[]>
        {
            [character + LocalPlayerTorchReader.EnabledOffset] = new byte[] { 1 },
            [character + LocalPlayerTorchReader.EquipmentOffset] = BitConverter.GetBytes(equipment),
            [equipment + LocalPlayerTorchReader.LightOffset] = BitConverter.GetBytes(0x300000),
            [equipment + LocalPlayerTorchReader.EmittedOffset] = new byte[] { 1 },
            [module + LocalPlayerTorchReader.EngineGlobalRva] = BitConverter.GetBytes(engine),
            [engine + LocalPlayerTorchReader.CameraManagerOffset] = BitConverter.GetBytes(manager),
            [manager + LocalPlayerTorchReader.ActiveCameraOffset] = BitConverter.GetBytes(camera),
            [manager + LocalPlayerTorchReader.FallbackCameraOffset] = BitConverter.GetBytes(camera)
        };
        var reader = new LocalPlayerTorchReader((address, output) =>
        {
            if (!memory.TryGetValue(address, out var data) || data.Length != output.Length) return false;
            data.CopyTo(output, 0); return true;
        });
        void Rotate(float x, float y, float z, float w) =>
            memory[camera + LocalPlayerTorchReader.CameraRotationOffset] =
                new[] { x, y, z, w }.SelectMany(BitConverter.GetBytes).ToArray();
        Rotate(0, 0, 0, 1);
        AssertEx.True(reader.TryCapture(character, out var forward, module));
        AssertEx.Equal(new PlayerTorchSnapshot(true, 0, 0, 1), forward!);
        // Looking straight up is valid, independent of body yaw; fallback camera is used by retail too.
        Rotate(-MathF.Sqrt(0.5F), 0, 0, MathF.Sqrt(0.5F));
        memory[manager + LocalPlayerTorchReader.ActiveCameraOffset] = BitConverter.GetBytes(0);
        AssertEx.True(reader.TryCapture(character, out var up, module));
        AssertEx.True(up!.AimY > 0.999F && MathF.Abs(up.AimZ!.Value) < 0.0001F);
        var arguments = RemoteMovementCommandFactory.Create(0x200000, new RemoteMovementPlan(
            RemoteMovementAction.Stop, 0, 0, 1.1F, 10, 20, 30, 0.5F, 0, PlayerMoveType.Walk, 1.15073F, Torch: up));
        ModelValidation.Validate(Command(arguments));
        AssertEx.True(PlayerTorchCodec.TryDecodeDirection(arguments["playerTorchDirection"], out var decoded));
        AssertEx.Equal(up, decoded!);
        // Exhaustion clears requested before the equipment show/hide catches up.
        memory[character + LocalPlayerTorchReader.EnabledOffset][0] = 0;
        AssertEx.True(reader.TryCapture(character, out var depleted, module));
        AssertEx.Equal(Off, depleted!);
        memory[character + LocalPlayerTorchReader.EnabledOffset][0] = 1;
        Rotate(float.NaN, 0, 0, 1);
        AssertEx.True(reader.TryCapture(character, out var invalidCamera, module));
        AssertEx.Equal(On, invalidCamera!); // camera unavailability must not swallow the real state
    }

    internal static Task CommandValidationAsync()
    {
        foreach (var torch in new[] { Off, On })
        {
            var plan = new RemoteMovementPlan(RemoteMovementAction.Stop, 0, 0, 1.1F,
                10, 20, 30, 0.5F, 0, PlayerMoveType.Walk, 1.15073F, Crouched, torch);
            var arguments = RemoteMovementCommandFactory.Create(0x200000, plan);
            AssertEx.Equal(13, arguments.Count);
            AssertEx.Equal(torch.Enabled ? "1" : "0", arguments["playerTorch"]);
            var command = Command(arguments);
            ModelValidation.Validate(command);
            AssertInvalidArguments(command, arguments);
            var torchOnly = RemoteMovementCommandFactory.Create(0x200000, plan with { Posture = null });
            AssertEx.False(torchOnly.ContainsKey("playerPosture"));
            ModelValidation.Validate(Command(torchOnly));
            ModelValidation.Validate(Command(RemoteMovementCommandFactory.Create(0x200000,
                plan with { Action = RemoteMovementAction.Move, DirectionX = 1.15073F })));
        }
        AssertEx.False(RemoteMovementCommandFactory.CreateStop(0x200000).ContainsKey("playerTorch"));
        var aimed = new Dictionary<string, string>(RemoteMovementCommandFactory.Create(0x200000,
            new RemoteMovementPlan(RemoteMovementAction.Stop, 0, 0, 1.1F,
                10, 20, 30, 0.5F, 0, PlayerMoveType.Walk, 1.15073F, Crouched, On)))
            { ["playerTorchDirection"] = "0,0.6,0.8" };
        ModelValidation.Validate(Command(aimed));
        foreach (var text in new[] { "0,0", "0,0,0", "NaN,0,1", "2,0,0", "0,0,1,0", "0,,1" })
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Command(
                new Dictionary<string, string>(aimed) { ["playerTorchDirection"] = text })));
        aimed["playerTorch"] = "0";
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Command(aimed)));
        return Task.CompletedTask;
    }

    private static GameCommandExecutePayload Command(IReadOnlyDictionary<string, string> arguments) =>
        new(1, Guid.NewGuid(), 1, Guid.NewGuid(), GameCommandNames.CharacterDirectMove, arguments);

    private static void AssertInvalidArguments(GameCommandExecutePayload command, IReadOnlyDictionary<string, string> arguments)
    {
        foreach (var text in new[] { "", "true", "false", "2", "-1", "+1", "01", " 1", "1 ", "1\0", "1\n", new string('1', 512) })
        {
            var bad = new Dictionary<string, string>(arguments) { ["playerTorch"] = text };
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = bad }));
        }
        foreach (var key in new[] { "replicationPositionX", "replicationYaw", "moveType", "directionSpace" })
        {
            var bad = new Dictionary<string, string>(arguments);
            bad.Remove(key);
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = bad }));
        }
        var malformedPosture = new Dictionary<string, string>(arguments) { ["playerPosture"] = "invalid" };
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = malformedPosture }));
        var bare = new Dictionary<string, string>(RemoteMovementCommandFactory.CreateStop(0x200000)) { ["playerTorch"] = "1" };
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = bare }));
    }

    internal static Task MovementAsync()
    {
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(1, null), Now));
        track.BindCharacter(0x200000);
        var unknown = Apply(track, Now);
        AssertEx.True(unknown.Torch is null);
        AssertEx.False(RemoteMovementCommandFactory.Create(0x200000, unknown).ContainsKey("playerTorch"));
        AssertEx.True(track.TryAccept(Position(2, On), Now.AddMilliseconds(1)));
        AssertEx.True(track.Snapshot()!.TorchPending);
        var enabled = Apply(track, Now.AddMilliseconds(1));
        AssertEx.Equal(RemoteMovementAction.Stop, enabled.Action);
        AssertEx.Equal(On, enabled.Torch!); // posture was never available
        AssertEx.False(track.Snapshot()!.TorchPending);
        AssertEx.True(track.TryAccept(Position(3, null), Now.AddMilliseconds(2)));
        var unavailable = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(2));
        AssertEx.Equal(On, unavailable.Torch!);
        AssertEx.Equal(RemoteMovementAction.None, unavailable.Action);
        AssertEx.True(track.TryAccept(Position(4, Off), Now.AddMilliseconds(3)));
        AssertEx.Equal(Off, Apply(track, Now.AddMilliseconds(3)).Torch!);
        AssertEx.True(track.TryAccept(Position(5, null), Now.AddMilliseconds(4)));
        AssertEx.Equal(Off, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(4)).Torch!);
        AssertMovingEdge();
        AssertAimUpdates();
        return Task.CompletedTask;
    }

    private static void AssertAimUpdates()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, new(true, 0, 0, 1)), Now);
        track.BindCharacter(0x200000);
        Apply(track, Now);
        track.TryAccept(Position(2, new(true, 0, 0.6F, 0.8F)), Now.AddMilliseconds(16));
        AssertEx.False(track.Snapshot()!.TorchPending);
        AssertEx.Equal(RemoteMovementAction.None, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(16)).Action);
        track.TryAccept(Position(3, new(true, 0, 1, 0)), Now.AddMilliseconds(34));
        var vertical = Apply(track, Now.AddMilliseconds(34));
        AssertEx.Equal(new PlayerTorchSnapshot(true, 0, 1, 0), vertical.Torch!);
        var queue = new PlayerPostureDispatchQueue();
        queue.Bind(Crouched, On);
        queue.Complete(Crouched, On);
        for (var i = 0; i < 100; ++i) queue.Observe(Crouched, new(true, 0, MathF.Sin(i), MathF.Cos(i)));
        AssertEx.Equal(0, queue.Count); // looking around does not consume ordered edge capacity
        queue.Observe(Crouched, Off);
        AssertEx.Equal(1, queue.Count);
        AssertEx.Equal(Off, queue.NextTorch!);
    }

    private static RemoteMovementPlan Apply(RemotePositionTrack track, DateTimeOffset now)
    {
        var plan = RemoteMovementPlanner.Create(track.Snapshot()!, now);
        AssertEx.True(plan.Action != RemoteMovementAction.None);
        ModelValidation.Validate(Command(RemoteMovementCommandFactory.Create(0x200000, plan)));
        track.MarkCommand(plan, now);
        return plan;
    }

    private static void AssertMovingEdge()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, Off), Now);
        track.BindCharacter(0x200000);
        Apply(track, Now);
        track.TryAccept(Position(2, Off) with { X = 10.1F }, Now.AddMilliseconds(16));
        AssertEx.Equal(RemoteMovementAction.Move, Apply(track, Now.AddMilliseconds(16)).Action);
        track.TryAccept(Position(3, On) with { X = 10.1F }, Now.AddMilliseconds(17));
        var plan = Apply(track, Now.AddMilliseconds(17));
        AssertEx.Equal(RemoteMovementAction.Move, plan.Action); // bypass the 25 ms command interval
        AssertEx.Equal(On, plan.Torch!);
    }

    internal static Task EdgesAndBindingAsync()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, Off), Now);
        track.TryAccept(Position(2, On), Now);
        track.TryAccept(Position(3, null), Now);
        track.BindCharacter(0x200000); // establish the latest baseline, discarding pre-spawn edges
        AssertEx.Equal(On, Apply(track, Now).Torch!);
        track.TryAccept(Position(4, Off, Crouched), Now.AddMilliseconds(1));
        track.TryAccept(Position(5, On), Now.AddMilliseconds(2));
        track.TryAccept(Position(6, Off, Crouched with { Crouched = false }), Now.AddMilliseconds(3));
        var first = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(4));
        AssertEx.Equal(Off, first.Torch!);
        AssertEx.Equal(Crouched, first.Posture!);
        AssertEx.Equal(first, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(4))); // failed IPC retains the edge
        track.MarkCommand(first, Now.AddMilliseconds(4));
        var second = Apply(track, Now.AddMilliseconds(5));
        AssertEx.Equal(On, second.Torch!);
        AssertEx.Equal(Crouched, second.Posture!); // null posture cannot erase the crouch
        var third = Apply(track, Now.AddMilliseconds(6));
        AssertEx.Equal(Off, third.Torch!);
        AssertEx.False(third.Posture!.Crouched);
        track.TryAccept(Position(7, On), Now.AddMilliseconds(7));
        track.TryAccept(Position(8, Off), Now.AddMilliseconds(8));
        track.BindCharacter(0x300000);
        AssertEx.Equal(Off, Apply(track, Now.AddMilliseconds(9)).Torch!);
        AssertEx.False(track.Snapshot()!.TorchPending);
        track.TryAccept(Position(9, On), Now.AddMilliseconds(10));
        AssertEx.Equal(On, Apply(track, Now.AddSeconds(1)).Torch!); // pending state survives movement timeout
        AssertBoundedQueue();
        return Task.CompletedTask;
    }

    private static void AssertBoundedQueue()
    {
        var queue = new PlayerPostureDispatchQueue();
        queue.Bind(null, On);
        for (var i = 0; i < 100; ++i) queue.Observe(null, i % 2 == 0 ? Off : On);
        queue.Observe(null, null);
        AssertEx.Equal(PlayerPostureDispatchQueue.Capacity, queue.Count);
        // A completion that does not match the head edge must not consume it.
        queue.Complete(null, queue.NextTorch == On ? Off : On);
        AssertEx.Equal(PlayerPostureDispatchQueue.Capacity, queue.Count);
        while (queue.Count > 1) queue.Complete(queue.Next, queue.NextTorch);
        AssertEx.Equal(On, queue.NextTorch!);
        queue.Bind(null);
        AssertEx.True(queue.NextTorch is null);
        AssertEx.False(queue.HasPending);
    }

    internal static Task CoordinatorAsync()
    {
        using var fixture = new CoordinatorFixture();
        var coordinator = fixture.Coordinator;
        var profile = new LocalPlayerMovementProfile(PlayerMoveType.Walk, PlayerMoveType.Walk, 1, 1, 2, 3, Torch: On);
        fixture.Call("UpdateObservedTransform", Now, true, new LocalPlayerPosition(10, 20, 30), true, 0.5F, false, profile);
        AssertEx.Equal(On, fixture.Get<PlayerTorchSnapshot>("_torch")); // independent of posture and movement-profile availability
        QueueLocal(fixture, Position(1, On));
        QueueLocal(fixture, Position(2, Off, Crouched));
        QueueLocal(fixture, Position(3, On));
        QueueLocal(fixture, Position(4, null)); // replaceable position cannot swallow either edge
        AssertEx.Equal(On, TakeLocal(fixture)!.Torch!);
        AssertEx.Equal(Off, TakeLocal(fixture)!.Torch!);
        var enabled = TakeLocal(fixture)!;
        AssertEx.Equal(On, enabled.Torch!);
        AssertEx.Equal(Crouched, enabled.Posture!);
        AssertEx.Equal(4UL, TakeLocal(fixture)!.Sequence);
        AssertEx.True(TakeLocal(fixture) is null);
        QueueLocal(fixture, Position(5, new(true, 0, 0, 1)));
        QueueLocal(fixture, Position(6, new(true, 0, 1, 0)));
        AssertEx.Equal(6UL, TakeLocal(fixture)!.Sequence); // direction updates coalesce, on/off edges do not
        AssertEx.True(TakeLocal(fixture) is null);
        AssertCoordinatorResets(fixture);
        return Task.CompletedTask;
    }

    private static void QueueLocal(CoordinatorFixture fixture, PlayerPositionSnapshotPayload value)
    {
        fixture.Set("_pendingLocalPosition", value);
        fixture.Call("QueueLocalPostureEdge", value);
    }

    private static PlayerPositionSnapshotPayload? TakeLocal(CoordinatorFixture fixture) =>
        (PlayerPositionSnapshotPayload?)fixture.Call("TakePendingLocalPosition");

    private static void AssertCoordinatorResets(CoordinatorFixture fixture)
    {
        foreach (var reset in new[] { "RemoveRemotePeer", "ResetRemoteSpawnState", "ClearSessionState" })
        {
            var tracks = fixture.Get<Dictionary<ulong, RemotePositionTrack>>("_remotePositions");
            var track = new RemotePositionTrack();
            track.TryAccept(Position(50, On), Now);
            track.BindCharacter(0x200000);
            tracks[2] = track;
            if (reset == "RemoveRemotePeer") fixture.Call(reset, 2UL);
            else fixture.Call(reset); // round/generation reset and disconnect own fresh track state
            AssertEx.False(tracks.ContainsKey(2));
            var next = new RemotePositionTrack();
            next.TryAccept(Position(1, null) with { LevelGeneration = 2 }, Now);
            next.BindCharacter(0x300000);
            AssertEx.True(Apply(next, Now).Torch is null); // never invent false or inherit the old on state
        }
        QueueLocal(fixture, Position(1, On));
        fixture.Call("ClearPendingLocalPositions");
        AssertEx.True(TakeLocal(fixture) is null);
        QueueLocal(fixture, Position(1, On));
        AssertEx.Equal(On, TakeLocal(fixture)!.Torch!); // reset also clears local dedupe
    }

    private sealed class CoordinatorFixture : IDisposable
    {
        private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
        private readonly TemporaryDirectory _directory = new();
        private readonly LauncherDiagnostics _diagnostics;
        private readonly NativeRuntimeController _controller;
        private readonly ScaleformBridgeListener _bridge;
        internal MultiplayerSessionCoordinator Coordinator { get; }

        internal CoordinatorFixture()
        {
            _diagnostics = new LauncherDiagnostics(_directory.Combine("logs"));
            var settings = new LauncherSettingsStore(_directory.Combine("settings.json"));
            _controller = new NativeRuntimeController(_diagnostics, settings);
            _bridge = new ScaleformBridgeListener(_diagnostics);
            Coordinator = new MultiplayerSessionCoordinator(_controller, settings, _bridge, new PerformanceIsolationState());
        }

        internal object? Call(string method, params object?[] arguments) =>
            typeof(MultiplayerSessionCoordinator).GetMethod(method, PrivateInstance)!.Invoke(Coordinator, arguments);
        internal T Get<T>(string field) =>
            (T)typeof(MultiplayerSessionCoordinator).GetField(field, PrivateInstance)!.GetValue(Coordinator)!;
        internal void Set(string field, object value) =>
            typeof(MultiplayerSessionCoordinator).GetField(field, PrivateInstance)!.SetValue(Coordinator, value);

        public void Dispose()
        {
            Coordinator.Dispose();
            _bridge.Dispose();
            _controller.Dispose();
            _diagnostics.Dispose();
            _directory.Dispose();
        }
    }
}
