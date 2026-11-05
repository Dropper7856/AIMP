using AIMP.Launcher;
using System.Text;

namespace AIMP.Protocol.Tests;

internal static class PlayerHidingTests
{
    private static readonly PlayerHidingSnapshot Hidden = new(true, 10, 20, 30), Visible = new(false);
    private static readonly DateTimeOffset Now = DateTimeOffset.FromUnixTimeMilliseconds(1_800_000_000_000);
    private static PlayerPositionSnapshotPayload Position(ulong sequence, PlayerHidingSnapshot? hiding) =>
        new(Guid.Parse("d4b36fc5-c75c-479f-911b-bcc509efb874"), 2, 1, sequence,
            Now.ToUnixTimeMilliseconds() + (long)sequence, 10 + sequence, 20, 30, 0,
            PlayerMoveType.Walk, 1.15F, CharacterIncarnation: 1, Hiding: hiding);

    internal static Task WireAsync()
    {
        foreach (var state in new PlayerHidingSnapshot?[] { null, Visible, Hidden, new(true, 0, 0, 0) })
        {
            var position = Position(1, state);
            ModelValidation.Validate(position);
            AssertEx.Equal(position, JsonPayload.Deserialize<PlayerPositionSnapshotPayload>(JsonPayload.Serialize(position)));
            if (state is null) continue;
            AssertEx.True(PlayerHidingCodec.TryDecode(PlayerHidingCodec.Encode(state), out var decoded));
            AssertEx.Equal(state, decoded!);
        }
        foreach (var state in new PlayerHidingSnapshot[] {
            new(true), new(true, 0, null, 1), new(true, float.NaN, 0, 0),
            new(true, 0, float.PositiveInfinity, 0), new(true, 1_000_001, 0, 0), new(false, 0, 0, 0) })
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(Position(1, state)));
        foreach (var text in new[] { "", "1", "true", "0,1,2,3", "1,1,2", "1,1,2,3,4", "1,NaN,0,0", "1,1000001,0,0" })
            AssertEx.False(PlayerHidingCodec.TryDecode(text, out _));
        foreach (var json in new[] { "{}", "{\"hidden\":null}", "{\"hidden\":1}", "{\"hidden\":false,\"extra\":0}" })
            AssertEx.Throws<ProtocolException>(() => JsonPayload.Deserialize<PlayerHidingSnapshot>(Encoding.UTF8.GetBytes(json)));
        var frame = FrameCodec.Encode(new AimpFrame(MessageType.PlayerPositionSnapshot, 0, 1,
            JsonPayload.Serialize(Position(1, Hidden))));
        frame[4] = 29; frame[5] = 0;
        AssertEx.Throws<ProtocolException>(() => FrameCodec.Decode(frame));
        return Task.CompletedTask;
    }

    internal static Task CaptureAsync()
    {
        const int module = 0x400000, character = 0x100000;
        byte flag = 0;
        var anchor = new float[] { 10, 20, 30 };
        bool tear = false, current = true, failPosition = false;
        var reader = new LocalPlayerHidingReader((address, output) =>
        {
            if (address == module + LocalPlayerHidingReader.HidingPositionRva && !failPosition)
            {
                anchor.SelectMany(BitConverter.GetBytes).ToArray().CopyTo(output, 0);
                if (tear) flag = 0;
                return true;
            }
            if (address != character + LocalPlayerHidingReader.PlayerHidingFlagOffset) return false;
            output[0] = flag;
            return true;
        });
        AssertEx.True(reader.TryCapture(module, character, 1, out var visible));
        AssertEx.Equal(Visible, visible!);
        flag = 1;
        AssertEx.True(reader.TryCapture(module, character, 1, out var hidden));
        AssertEx.Equal(Hidden, hidden!);
        anchor[0] = 50;
        AssertEx.True(reader.TryCapture(module, character, 1, out var latched));
        AssertEx.Equal(Hidden, latched!);
        AssertEx.True(reader.TryCapture(module, character, 2, out var newLevel));
        AssertEx.Equal(new PlayerHidingSnapshot(true, 50, 20, 30), newLevel!);
        reader.Reset(); tear = true;
        AssertEx.False(reader.TryCapture(module, character, 2, out var torn));
        AssertEx.True(torn is null);
        tear = false; flag = 1; current = false;
        AssertEx.False(reader.TryCapture(module, character, 2, out _, () => current));
        anchor[0] = 60; current = true;
        AssertEx.True(reader.TryCapture(module, character, 2, out var coherent, () => current));
        AssertEx.Equal(new PlayerHidingSnapshot(true, 60, 20, 30), coherent!);
        reader.Reset(); failPosition = true;
        AssertEx.False(reader.TryCapture(module, character, 2, out var missing));
        AssertEx.True(missing is null);
        return Task.CompletedTask;
    }

    internal static Task MovementAsync()
    {
        var track = new RemotePositionTrack();
        track.TryAccept(Position(1, Visible), Now);
        track.BindCharacter(0x200000);
        void Complete()
        {
            var plan = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
            track.MarkCommand(plan, Now);
        }
        Complete();
        track.TryAccept(Position(2, Hidden), Now);
        track.TryAccept(Position(3, null), Now); // unavailable samples preserve occupancy
        var enter = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(RemoteMovementAction.Stop, enter.Action);
        AssertEx.Equal(Hidden, enter.Hiding!);
        var arguments = RemoteMovementCommandFactory.Create(0x200000, enter);
        var command = new GameCommandExecutePayload(1, Guid.NewGuid(), 1, Guid.NewGuid(),
            GameCommandNames.CharacterDirectMove, arguments);
        ModelValidation.Validate(command);
        AssertEx.Equal("1,10,20,30", arguments["playerHiding"]);
        AssertEx.Equal("0", arguments["playerTorch"]);
        var lit = RemoteMovementCommandFactory.Create(0x200000,
            enter with { Torch = new PlayerTorchSnapshot(true, 0, 0, 1) });
        AssertEx.Equal("0", lit["playerTorch"]);
        AssertEx.False(lit.ContainsKey("playerTorchDirection"));
        foreach (var key in new[] { "replicationPositionX", "replicationYaw", "moveType", "directionSpace" })
        {
            var invalid = new Dictionary<string, string>(arguments);
            invalid.Remove(key);
            AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(command with { Arguments = invalid }));
        }
        // Rapid entry/exit must retain both edges while a command is in flight.
        track.TryAccept(Position(4, Visible), Now);
        track.MarkCommand(enter, Now);
        var exit = RemoteMovementPlanner.Create(track.Snapshot()!, Now);
        AssertEx.Equal(Visible, exit.Hiding!);
        AssertEx.Equal("0", RemoteMovementCommandFactory.Create(0x200000, exit)["playerHiding"]);
        track.MarkCommand(exit, Now);
        track.TryAccept(Position(5, Hidden), Now);
        track.BindCharacter(0x300000);
        AssertEx.Equal(Hidden, track.Snapshot()!.Hiding!);
        track.TryAccept(Position(6, null) with { CharacterIncarnation = 2 }, Now);
        AssertEx.True(track.Snapshot()!.Hiding is null);
        return Task.CompletedTask;
    }
}
