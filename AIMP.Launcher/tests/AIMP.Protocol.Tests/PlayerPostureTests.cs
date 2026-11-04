using AIMP.Launcher;
using System.Globalization;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class PlayerPostureTests
{
    private static readonly Guid Session = Guid.Parse("8c6d16a5-f671-44dc-a925-58ee75b75f12");
    private static readonly DateTimeOffset Now = DateTimeOffset.FromUnixTimeMilliseconds(1_800_000_000_000);
    private static PlayerPostureSnapshot Posture(ulong revision = 0, bool active = false) =>
        new(true, false, false, 0, 0, revision, revision == 0 ? (ushort)0 : (ushort)2,
            revision == 0 ? (byte)0 : (byte)4, revision == 0 ? 0U : 123U, active);
    private static PlayerPositionSnapshotPayload Position(ulong sequence, PlayerPostureSnapshot? posture) =>
        new(Session, 2, 1, sequence, Now.ToUnixTimeMilliseconds() + (long)sequence,
            10, 20, 30, 0.5F, PlayerMoveType.Walk, 1.15073F, posture);

    internal static Task WireAsync()
    {
        AssertEx.Equal((ushort)29, ProtocolConstants.Version);
        var posture = Posture(ulong.MaxValue, true) with
        {
            DeepCrouched = true, InVent = true, VentIdA = uint.MaxValue, VentIdB = 42,
            TraversalIndex = ushort.MaxValue, TraversalType = 8
        };
        var oldCulture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            AssertEx.Equal("15,4294967295,42,18446744073709551615,65535,8,123", PlayerPostureCodec.Encode(posture));
        }
        finally { CultureInfo.CurrentCulture = oldCulture; }
        AssertEx.True(PlayerPostureCodec.TryDecode(PlayerPostureCodec.Encode(posture), out var decoded));
        AssertEx.Equal(posture, decoded!);
        var snapshot = Position(1, posture);
        AssertEx.Equal(snapshot, JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(snapshot)));
        var legacy = Encoding.UTF8.GetString(JsonPayload.Serialize(Position(1, null))).Replace(",\"posture\":null", "", StringComparison.Ordinal);
        AssertEx.True(JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(Encoding.UTF8.GetBytes(legacy)).Posture is null);
        AssertInvalidCsv();
        AssertInvalidModels();
        AssertReceiveWindow();
        return Task.CompletedTask;
    }

    private static void AssertInvalidCsv()
    {
        foreach (var text in new[]
        {
            "", "0,0,0,0,0,0", "0,0,0,0,0,0,0,0", "16,0,0,0,0,0,0",
            "-1,0,0,0,0,0,0", "+1,0,0,0,0,0,0", " 1,0,0,0,0,0,0",
            "1,4294967296,0,0,0,0,0", "1,0,0,18446744073709551616,0,0,0",
            "1,0,0,1,65536,4,1", "1,0,0,1,1,256,1", "1,0,0,1,1,4,4294967296",
            "8,0,0,0,0,0,0", "1,0,0,1,0,4,123", "1,0,0,1,2,6,123",
            "1,0,0,1,2,4,4294967295"
        }) AssertEx.False(PlayerPostureCodec.TryDecode(text, out _), text);
    }

    private static void AssertInvalidModels()
    {
        foreach (var posture in new[]
        {
            Posture() with { TraversalActive = true }, Posture() with { TraversalIndex = 2 },
            Posture(1) with { TraversalType = 3 }, Posture(1) with { TraversalOwner = uint.MaxValue },
            Posture() with { InVent = true }, Posture() with { VentIdA = 1 }
        }) AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Position(1, posture)));
        foreach (var type in new byte[] { 4, 5, 7, 8 })
            ModelValidation.Validate(Position(1, Posture(1) with { TraversalType = type }));
        ModelValidation.Validate(Position(1, Posture(1) with { Crouched = false, DeepCrouched = true, TraversalOwner = 0 }));
        var missingOwner = Encoding.UTF8.GetString(JsonPayload.Serialize(Position(1, Posture(1))))
            .Replace(",\"traversalOwner\":123", "", StringComparison.Ordinal);
        AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(Encoding.UTF8.GetBytes(missingOwner)));
        var bytes = JsonPayload.Serialize(Position(1, Posture()));
        var malformed = Encoding.UTF8.GetString(bytes).Replace("\"crouched\":true", "\"crouched\":2", StringComparison.Ordinal);
        AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(Encoding.UTF8.GetBytes(malformed)));
        var frame = FrameCodec.Encode(new AimpFrame(MessageType.PlayerPositionSnapshot, 0, 1, bytes));
        frame[4] = 7;
        frame[5] = 0;
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(frame));
    }

    private static void AssertReceiveWindow()
    {
        var window = new PlayerPositionReceiveWindow();
        AssertEx.True(window.TryAccept(Position(1, Posture(10, true))));
        AssertEx.True(window.TryAccept(Position(2, Posture(10))));
        AssertEx.False(window.TryAccept(Position(3, Posture(10, true))));
        AssertEx.False(window.TryAccept(Position(3, Posture(9))));
        AssertEx.False(window.TryAccept(Position(3, Posture(10) with { TraversalOwner = 456 })));
        AssertEx.False(window.TryAccept(Position(3, Posture(10) with { TraversalType = 5 })));
        AssertEx.False(window.TryAccept(Position(3, Posture(10) with { TraversalIndex = 3 })));
        AssertEx.True(window.TryAccept(Position(3, Posture(11)))); // completed between publishes
        AssertEx.False(window.TryAccept(Position(3, Posture(12))));
        AssertEx.False(window.TryAccept(Position(4, Posture(12)) with { PeerId = 3 }));
        AssertEx.False(window.TryAccept(Position(4, Posture(12)) with { SessionId = Guid.NewGuid() }));
        AssertEx.True(window.TryAccept(Position(1, Posture(1, true)) with { LevelGeneration = 2 }));
        AssertEx.False(window.TryAccept(Position(5, Posture(12))));
        AssertEx.True(window.TryAccept(Position(2, Posture()) with { LevelGeneration = 2 }));
        AssertEx.False(window.TryAccept(Position(3, Posture(1, true)) with { LevelGeneration = 2 }));
        AssertEx.True(window.TryAccept(Position(3, Posture(2, true)) with { LevelGeneration = 2 }));
        AssertEx.True(window.TryAccept(Position(4, null) with { LevelGeneration = 2 }));
        AssertEx.True(window.TryAccept(Position(5, Posture(2, true)) with { LevelGeneration = 2 }));
    }

    internal static Task MovementAsync()
    {
        var track = new RemotePositionTrack();
        AssertEx.True(track.TryAccept(Position(1, Posture(9)), Now));
        track.BindCharacter(0x200000);
        var baseline = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(9UL, baseline.Posture!.TraversalRevision); // native establishes its real baseline
        AssertEx.False(baseline.Posture.TraversalActive);
        AssertStationaryCommand(baseline);
        track.MarkCommand(baseline, Now);
        track.TryAccept(Position(2, Posture(9)), Now.AddMilliseconds(1));
        AssertEx.False(track.Snapshot()!.PosturePending); // actual historical heartbeat cannot become a new event
        AssertEx.Equal(RemoteMovementAction.None, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(1)).Action);
        AssertOrderedEdges(track);
        AssertBindAndOverflow();
        return Task.CompletedTask;
    }

    private static void AssertStationaryCommand(RemoteMovementPlan plan)
    {
        AssertEx.Equal(RemoteMovementAction.Stop, plan.Action);
        var arguments = RemoteMovementCommandFactory.Create(0x200000, plan);
        AssertEx.Equal("0", arguments["frameCount"]);
        AssertEx.Equal("1", arguments["moveType"]);
        AssertEx.Equal("1,0,0,9,2,4,123", arguments["playerPosture"]);
        var command = new GameCommandExecutePayload(1, Guid.NewGuid(), 1, Guid.NewGuid(),
            GameCommandNames.CharacterDirectMove, arguments);
        ModelValidation.Validate(command);
        var bad = new Dictionary<string, string>(arguments) { ["playerPosture"] = "16,0,0,0,0,0,0" };
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = bad }));
        bad = new Dictionary<string, string>(arguments) { ["moveType"] = "4" };
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = bad }));
        AssertEx.False(RemoteMovementCommandFactory.CreateStop(0x200000).ContainsKey("playerPosture"));
    }

    private static void AssertOrderedEdges(RemotePositionTrack track)
    {
        var entry = Posture(10, true) with { InVent = true, VentIdA = 44, VentIdB = 55 };
        track.TryAccept(Position(3, entry), Now.AddMilliseconds(2));
        track.TryAccept(Position(4, entry with { TraversalActive = false }), Now.AddMilliseconds(3));
        track.TryAccept(Position(5, Posture(10)), Now.AddMilliseconds(4));
        var first = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(5));
        AssertEx.Equal(entry, first.Posture!);
        // Rejection/exception leaves this exact edge pending; only successful IPC calls MarkCommand.
        AssertEx.Equal(first, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(5)));
        track.MarkCommand(first, Now.AddMilliseconds(5));
        var completion = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(6));
        AssertEx.False(completion.Posture!.TraversalActive);
        AssertEx.True(completion.Posture.InVent);
        track.MarkCommand(completion, Now.AddMilliseconds(6));
        var exit = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(7));
        AssertEx.False(exit.Posture!.InVent);
        AssertEx.Equal(10UL, exit.Posture.TraversalRevision);
        track.MarkCommand(exit, Now.AddMilliseconds(7));
        AssertEx.False(track.Snapshot()!.PosturePending);
        var uncrouch = Posture(10) with { Crouched = false };
        track.TryAccept(Position(6, uncrouch), Now.AddMilliseconds(8));
        var idle = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(9));
        AssertEx.Equal(RemoteMovementAction.Stop, idle.Action);
        AssertEx.False(idle.Posture!.Crouched);
        AssertEx.Equal(PlayerMoveType.Walk, idle.MoveType);
        AssertEx.False(track.TryAccept(Position(7, Posture(10, true)), Now));
        track.MarkCommand(idle, Now.AddMilliseconds(9));
        AssertEx.True(track.TryAccept(Position(7, null), Now.AddMilliseconds(10)));
        AssertEx.Equal(uncrouch, RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddMilliseconds(11)).Posture!);
        AssertEx.False(track.Snapshot()!.PosturePending);
    }

    private static void AssertBindAndOverflow()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, Posture(20, true)), Now);
        track.TryAccept(Position(2, Posture(20)), Now);
        track.TryAccept(Position(3, null), Now);
        track.BindCharacter(0x200000); // completion before spawn: never replay the queued old active edge
        AssertEx.False(RemoteMovementPlanner.Create(track.Snapshot()!, Now).Posture!.TraversalActive);
        AssertEx.Equal(20UL, RemoteMovementPlanner.Create(track.Snapshot()!, Now).Posture!.TraversalRevision);
        track.TryAccept(Position(4, Posture(21, true)), Now);
        track.BindCharacter(0x300000);
        AssertEx.True(RemoteMovementPlanner.Create(track.Snapshot()!, Now).Posture!.TraversalActive);
        var stale = RemoteMovementPlanner.Create(track.Snapshot()!, Now.AddSeconds(1));
        AssertEx.Equal(RemoteMovementAction.Stop, stale.Action);
        AssertEx.True(stale.Posture!.TraversalActive); // pending posture survives movement timeout
        var queue = new PlayerPostureDispatchQueue();
        queue.Bind(Posture(30));
        for (ulong i = 31; i < 100; i++) queue.Observe(Posture(i, true));
        queue.Observe(Posture(99));
        AssertEx.Equal(PlayerPostureDispatchQueue.Capacity, queue.Count);
        while (queue.Count > 1) queue.Complete(queue.Next);
        AssertEx.Equal(Posture(99), queue.Next!);
        queue.Complete(queue.Next);
        AssertEx.False(queue.HasPending);
    }

    internal static Task CaptureAsync()
    {
        var memory = new PostureMemory();
        var reader = new LocalPlayerPostureReader(memory.Read);
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var first));
        AssertEx.Equal(Posture(1, true), first!);
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var duplicate));
        AssertEx.Equal(first!, duplicate!);
        memory.Flags = new byte[] { 0, 1, 0, 0, 0, 1 };
        memory.VentA = 44;
        memory.VentB = 55;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var deepVent));
        AssertEx.False(deepVent!.Crouched);
        AssertEx.True(deepVent.DeepCrouched);
        AssertEx.True(deepVent.InVent);
        AssertEx.Equal(44U, deepVent.VentIdA);
        AssertEx.Equal(55U, deepVent.VentIdB);
        memory.Flags = new byte[] { 1, 0, 0, 0, 0, 0 };
        memory.VentA = memory.VentB = 0;
        memory.State = 1;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var complete));
        AssertEx.Equal(Posture(1), complete!);
        memory.State = 0x16;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var repeated));
        AssertEx.Equal(2UL, repeated!.TraversalRevision);
        AssertCaptureRejections(memory, reader);
        reader.Reset();
        memory.State = 1;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var reset));
        AssertEx.Equal(0UL, reset!.TraversalRevision);
        memory.State = 0x16;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 2, out var nextLifecycle));
        AssertEx.True(nextLifecycle!.TraversalRevision > repeated.TraversalRevision);
        AssertTraversalDecode();
        return Task.CompletedTask;
    }

    private static void AssertCaptureRejections(PostureMemory memory, LocalPlayerPostureReader reader)
    {
        memory.TearOnStateRead = true;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out _));
        memory.TearOnStateRead = false;
        memory.State = 0x16;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out _, () => false));
        memory.WrongCycleOwner = true;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out _));
        memory.WrongCycleOwner = false;
        memory.Unavailable = true;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out var unavailable));
        AssertEx.True(unavailable is null);
        memory.Unavailable = false;
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var resumed));
        AssertEx.Equal(2UL, resumed!.TraversalRevision); // temporary read loss keeps the ongoing traversal
        memory.RecordIndex = 3;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out _));
        memory.RecordIndex = 2;
        memory.Count = 1;
        AssertEx.False(reader.TryCapture(memory.Module, memory.Character, 1, out _));
        memory.Count = 2;
        memory.RecordType = 6; // a non-vent traversal never creates a vent event
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var nonVent));
        AssertEx.False(nonVent!.TraversalActive);
        AssertEx.Equal(2UL, nonVent.TraversalRevision);
        memory.RecordType = 4;
        memory.State = 0x10; // decimal 16 is not the retail traversal state (0x16 / decimal 22).
        AssertEx.True(reader.TryCapture(memory.Module, memory.Character, 1, out var notTraversal));
        AssertEx.False(notTraversal!.TraversalActive);
        memory.State = 0x16;
    }

    private static void AssertTraversalDecode()
    {
        AssertEx.True(LocalPlayerPostureMemoryLayout.TryResolveTraversal(0x200000, 1, out var first));
        AssertEx.Equal((nint)0x200000, first);
        AssertEx.True(LocalPlayerPostureMemoryLayout.TryResolveTraversal(first, 2, out var second));
        AssertEx.Equal(first + 0x10C, second);
        AssertEx.False(LocalPlayerPostureMemoryLayout.TryResolveTraversal(first, 0, out _));
        AssertEx.False(LocalPlayerPostureMemoryLayout.TryResolveTraversal(unchecked((nint)(long)uint.MaxValue), 2, out _));
        var bytes = new byte[8];
        BitConverter.GetBytes((ushort)2).CopyTo(bytes, 0);
        bytes[2] = 4;
        BitConverter.GetBytes(123U).CopyTo(bytes, 4);
        AssertEx.True(LocalPlayerPostureMemoryLayout.TryDecodeTraversal(bytes, 2, out var key));
        AssertEx.Equal(new PlayerTraversalKey(2, 4, 123), key);
        AssertEx.False(LocalPlayerPostureMemoryLayout.TryDecodeTraversal(bytes, 1, out _));
        AssertEx.False(LocalPlayerPostureMemoryLayout.TryDecodeTraversal(bytes.AsSpan(0, 7), 2, out _));
        Array.Clear(bytes, 4, 4);
        AssertEx.True(LocalPlayerPostureMemoryLayout.TryDecodeTraversal(bytes, 2, out var zeroOwner));
        AssertEx.Equal(0U, zeroOwner.Owner);
        BitConverter.GetBytes(uint.MaxValue).CopyTo(bytes, 4);
        AssertEx.False(LocalPlayerPostureMemoryLayout.TryDecodeTraversal(bytes, 2, out _));
    }

    private sealed class PostureMemory
    {
        internal nint Module = 0x400000, Character = 0x200000, Cycle = 0x300000;
        private readonly nint _root = 0x4000000, _nav = 0x5000000, _records = 0x6000000;
        internal byte State = 0x16, RecordType = 4;
        internal ushort RecordIndex = 2;
        internal uint Count = 2;
        internal byte[] Flags = new byte[] { 1, 0, 0, 0, 0, 0 };
        internal uint VentA, VentB;
        internal bool TearOnStateRead, WrongCycleOwner, Unavailable;

        internal bool Read(nint address, byte[] bytes)
        {
            if (Unavailable) return false;
            byte[]? value = Resolve(address);
            if (value is null || value.Length != bytes.Length) return false;
            value.CopyTo(bytes, 0);
            if (TearOnStateRead && address == Cycle + 0x1A) State = State == 0x16 ? (byte)1 : (byte)0x16;
            return true;
        }

        private byte[]? Resolve(nint address)
        {
            if (address == Character + 0x2AF) return Flags;
            if (address == Character + 0x4290) return VentPair();
            if (address == Character + 0x36C) return Pointer(Cycle);
            if (address == Cycle + 4) return Pointer(WrongCycleOwner ? Character + 4 : Character);
            if (address == Cycle + 0x1A) return new[] { State };
            if (address == Cycle + 0x42) return BitConverter.GetBytes((ushort)2);
            if (address == Module + 0x12F0C88) return Pointer(_root);
            if (address == _root + 0x20) return Pointer(_nav);
            if (address == _nav + 0x23EC) return Pointer(_records);
            if (address == _nav + 0x23F0) return BitConverter.GetBytes(Count);
            if (address == _records + 0x10C) return Record();
            return null;
        }

        private byte[] Record()
        {
            var bytes = new byte[8];
            BitConverter.GetBytes(RecordIndex).CopyTo(bytes, 0);
            bytes[2] = RecordType;
            BitConverter.GetBytes(123U).CopyTo(bytes, 4);
            return bytes;
        }

        private byte[] VentPair()
        {
            var bytes = new byte[8];
            BitConverter.GetBytes(VentA).CopyTo(bytes, 0);
            BitConverter.GetBytes(VentB).CopyTo(bytes, 4);
            return bytes;
        }

        private static byte[] Pointer(nint value) => BitConverter.GetBytes((uint)value);
    }
}
