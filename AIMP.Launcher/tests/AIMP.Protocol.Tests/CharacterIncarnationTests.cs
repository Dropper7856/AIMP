using AIMP.Protocol;
using AIMP.Launcher;

namespace AIMP.Protocol.Tests;

internal static class CharacterIncarnationTests
{
    internal static Task CodecAsync()
    {
        var fixture = Fixture();
        var value = new NativeCharacterIncarnationState(7,0x0102030405060708UL,0x11223344,0x55667788,1);
        AssertEx.Equal(value, NativeCharacterIncarnationCodec.Decode(fixture));
        AssertEx.SequenceEqual(fixture, NativeCharacterIncarnationCodec.Encode(value));
        var empty = new NativeCharacterIncarnationState(0,0,0,0);
        AssertEx.Equal(empty, NativeCharacterIncarnationCodec.Decode(NativeCharacterIncarnationCodec.Encode(empty)));
        for (var i = 0; i < fixture.Length; ++i)
            AssertEx.Throws<ProtocolException>(() => NativeCharacterIncarnationCodec.Decode(fixture.AsSpan(0,i)));
        AssertEx.Throws<ProtocolException>(() => NativeCharacterIncarnationCodec.Decode(fixture.Concat(new byte[1]).ToArray()));
        foreach (var offset in new[] {0,4,33,36})
        {
            var bad = fixture.ToArray(); bad[offset] ^= 0x80;
            AssertEx.Throws<ProtocolException>(() => NativeCharacterIncarnationCodec.Decode(bad));
        }
        foreach (var bad in new[] { value with {Serial=0}, value with {NativeGeneration=0}, value with {Character=0},
            value with {Character=uint.MaxValue}, value with {RuntimeId=0}, value with {Origin=2}, empty with {Origin=1} })
            AssertEx.Throws<ProtocolException>(() => NativeCharacterIncarnationCodec.Encode(bad));
        AssertEx.Equal((ushort)44, (ushort)MessageType.CharacterIncarnationState);
        return Task.CompletedTask;
    }

    private static byte[] Fixture()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var path = Path.Combine(directory.FullName,"tests","fixtures","native-character-incarnation-v1.hex");
            if (File.Exists(path)) return Convert.FromHexString(string.Concat(File.ReadAllText(path).Where(c => !char.IsWhiteSpace(c))));
        }
        throw new FileNotFoundException("Shared native character incarnation fixture not found.");
    }
    internal static Task BindingAsync()
    {
        var connection = Guid.NewGuid();
        var state = NativeRuntimeSnapshot.Empty with { IpcConnected=true, LevelGeneration=6 };
        var life = new NativeCharacterIncarnationState(7,9,0x100000,15,1);
        state = NativeCharacterIncarnationBinding.Apply(state,life,connection);
        AssertEx.False(NativeCharacterIncarnationBinding.MatchesCurrent(state,connection,7));
        state = NativeCharacterIncarnationBinding.RetainForGeneration(state,7) with { LevelGeneration=7 };
        AssertEx.True(NativeCharacterIncarnationBinding.MatchesCurrent(state,connection,7),"A matching lifetime must survive a delayed level update.");
        AssertEx.False(NativeCharacterIncarnationBinding.MatchesCurrent(state,Guid.NewGuid(),7));
        AssertEx.False(NativeCharacterIncarnationBinding.MatchesCurrent(state,connection,8));
        AssertEx.False(NativeCharacterIncarnationBinding.MatchesCurrent(state with {IpcConnected=false},connection,7));
        var next = NativeCharacterIncarnationBinding.RetainForGeneration(state,8);
        AssertEx.Equal(0UL,next.LocalCharacterSerial);
        AssertEx.Equal(Guid.Empty,next.LocalCharacterConnectionIdentity);
        AssertEx.Equal(0UL,NativeCharacterIncarnationBinding.Clear(state).LocalCharacterSerial);
        var unavailable = NativeCharacterIncarnationBinding.Apply(state,new(0,0,0,0),connection);
        AssertEx.False(NativeCharacterIncarnationBinding.MatchesCurrent(unavailable,connection,7));
        return Task.CompletedTask;
    }
    internal static Task RemoteCreationAsync()
    {
        var value=new CaptureStatePayload(ProtocolConstants.ExperimentalSchemaVersion,Guid.NewGuid(),7,Guid.NewGuid(),
            false,false,false,0,false,false,false,0,"creation",RemoteCharacterState:"Ready",
            RemoteCharacterSpawnKey:73,RemoteCharacterAddress:0x100000,RemoteCharacterRecordAddress:0x200000,
            CharacterCreateHookInstalled:true,RemoteCharacterCreationSerial:1234567890123UL,RemoteCharacterRuntimeId:73);
        ModelValidation.Validate(value);
        AssertEx.Equal(value,JsonPayload.Deserialize<CaptureStatePayload>(JsonPayload.Serialize(value)));
        foreach(var bad in new[]{value with {RemoteCharacterCreationSerial=0},value with {RemoteCharacterRuntimeId=0},
            value with {RemoteCharacterState="Pending",RemoteCharacterAddress=0,RemoteCharacterRecordAddress=0},
            value with {LevelGeneration=0}})
            AssertEx.Throws<ProtocolException>(()=>ModelValidation.Validate(bad));
        ModelValidation.Validate(value with {RemoteCharacterCreationSerial=0,RemoteCharacterRuntimeId=0});
        return Task.CompletedTask;
    }
}
