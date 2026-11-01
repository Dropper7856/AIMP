using AIMP.Launcher;
using System.Buffers.Binary;

namespace AIMP.Protocol.Tests;

internal static class AISyncParticipantBindingTests
{
    internal static Task ConfigurationAsync()
    {
        var session=Guid.Parse("00112233-4455-6677-8899-aabbccddeeff");
        var mappings=new Dictionary<ulong,uint>{{9,0x200000}};
        var local=new AISyncParticipantBinding(0x100000,3,15,9);
        var remote=new AISyncParticipantBinding(0x200000,4,77,10);
        var certificates=new Dictionary<ulong,AISyncParticipantBinding>{{9,remote}};
        var bytes=AISyncCodec.Configuration(2,7,5,session,7,mappings,localBinding:local,certifiedBindings:certificates);
        var fixture=Fixture(); AssertEx.Equal(1096,bytes.Length); AssertEx.SequenceEqual(fixture,bytes);
        AssertEx.Equal(3U,BinaryPrimitives.ReadUInt32LittleEndian(bytes));
        var legacy=AISyncCodec.Configuration(2,7,5,session,7,mappings);
        AssertEx.True(legacy.AsSpan(40,32).ToArray().All(b=>b==0));
        AssertEx.Equal(0U,BinaryPrimitives.ReadUInt32LittleEndian(legacy.AsSpan(84)));
        foreach(var bad in new[]{remote with {Incarnation=0},remote with {RuntimeId=0},remote with {CreationSerial=0},
            remote with {Character=0x200001},remote with {Character=uint.MaxValue}})
            AssertEx.Throws<ProtocolException>(()=>AISyncCodec.Configuration(2,7,5,session,7,mappings,
                certifiedBindings:new Dictionary<ulong,AISyncParticipantBinding>{{9,bad}}));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.Configuration(2,7,5,session,7,mappings,
            certifiedBindings:new Dictionary<ulong,AISyncParticipantBinding>{{10,remote}}));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.Configuration(2,7,5,session,7,mappings,localBinding:local with {Incarnation=0}));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.Configuration(2,7,5,session,7,mappings,localBinding:local with {Character=remote.Character}));
        return Task.CompletedTask;
    }

    internal static Task EvidenceAsync()
    {
        var connection=Guid.NewGuid();
        var state=NativeRuntimeSnapshot.Empty with {IpcConnected=true,ProcessId=7,LevelGeneration=2,
            RemoteCharacterState="Ready",RemoteCharacterSpawnKey=73,RemoteCharacterBase=0x200000,
            RemoteCharacterGeneration=2,RemoteCharacterCreationSerial=9,RemoteCharacterRuntimeId=15,
            RemoteCharacterConnectionIdentity=connection};
        var proof=AISyncParticipantEvidence.FromSpawn(4,0x200000,73,state,connection);
        AssertEx.True(proof is not null && proof.Matches(state,connection,2));
        AssertEx.True(AISyncParticipantEvidence.FromSpawn(0,0x200000,73,state,connection) is null);
        AssertEx.True(AISyncParticipantEvidence.FromSpawn(4,0x200010,73,state,connection) is null);
        AssertEx.True(AISyncParticipantEvidence.FromSpawn(4,0x200000,74,state,connection) is null);
        foreach(var bad in new[]{state with {RemoteCharacterCreationSerial=0},state with {RemoteCharacterRuntimeId=0},
            state with {RemoteCharacterGeneration=3},state with {IpcConnected=false},
            state with {RemoteCharacterConnectionIdentity=Guid.NewGuid()}})
            AssertEx.True(AISyncParticipantEvidence.FromSpawn(4,0x200000,73,bad,connection) is null);
        AssertEx.False(proof!.Matches(state,Guid.NewGuid(),2));
        AssertEx.False(proof.Matches(state with {ProcessId=8},connection,2));
        AssertEx.False(proof.Matches(state,connection,3));
        // Evidence stays immutable; observing a replacement never updates it.
        AssertEx.Equal(9UL,proof.Binding.CreationSerial);
        return Task.CompletedTask;
    }
    internal static Task PairCarrierAsync()
    {
        var session=Guid.NewGuid();var state=AISyncCodecTests.ValidState(session);
        BinaryPrimitives.WriteUInt32LittleEndian(state.AsSpan(640),12);
        AISyncCodec.ValidateNative(state);
        AssertEx.Equal(16U,AISyncCodec.StateVersion);AssertEx.Equal(840,state.Length);
        AssertEx.SequenceEqual(state,AISyncCodec.DecodeNetwork(AISyncCodec.EncodeNetwork(session,state),session).ToArray());
        var legacy=state.ToArray();legacy.AsSpan(640).Clear();AISyncCodec.ValidateNative(legacy);
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(state.AsSpan(0,640)));
        var oldVersion=state.ToArray();BinaryPrimitives.WriteUInt32LittleEndian(oldVersion,12);
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(oldVersion));
        var npc=state.ToArray();npc.AsSpan(172,8).Clear();BinaryPrimitives.WriteUInt32LittleEndian(npc.AsSpan(112),5);
        BinaryPrimitives.WriteUInt32LittleEndian(npc.AsSpan(544),1);BinaryPrimitives.WriteUInt32LittleEndian(npc.AsSpan(548),3);
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(npc));
        npc.AsSpan(640).Clear();AISyncCodec.ValidateNative(npc);
        var inactive=state.ToArray();BinaryPrimitives.WriteUInt32LittleEndian(inactive.AsSpan(40),0);
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(inactive));
        return Task.CompletedTask;
    }
    internal static Task PairAuthorityAsync()
    {
        var session=Guid.NewGuid();var state=AISyncCodecTests.ValidState(session);
        session.TryWriteBytes(state.AsSpan(644,16));
        BinaryPrimitives.WriteUInt64LittleEndian(state.AsSpan(660),7);
        BinaryPrimitives.WriteUInt64LittleEndian(state.AsSpan(668),1234567890123UL);
        AISyncCodec.ValidateNative(state);
        AssertEx.SequenceEqual(state,AISyncCodec.DecodeNetwork(AISyncCodec.EncodeNetwork(session,state),session).ToArray());
        foreach(var (offset,count) in new[]{(644,16),(660,8),(668,8)})
        {
            var bad=state.ToArray();bad.AsSpan(offset,count).Clear();
            AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(bad));
        }
        // Identical low 64-bit cookie does not establish the full GUID.
        var alias=state.ToArray();alias[659]^=1;AISyncCodec.ValidateNative(alias);
        AssertEx.False(AISyncCodec.BelongsTo(alias,session,7));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.EncodeNetwork(session,alias));
        var network=new byte[AISyncCodec.NetworkStateBytes];session.TryWriteBytes(network);alias.CopyTo(network,16);
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.DecodeNetwork(network,session));
        return Task.CompletedTask;
    }
    private static byte[] Fixture()
    {
        for(var directory=new DirectoryInfo(AppContext.BaseDirectory);directory is not null;directory=directory.Parent)
        {
            var path=Path.Combine(directory.FullName,"tests","fixtures","aisync-participant-config-v3.hex");
            if(File.Exists(path))return Convert.FromHexString(string.Concat(File.ReadAllText(path).Where(c=>!char.IsWhiteSpace(c))));
        }
        throw new FileNotFoundException("Shared participant configuration fixture missing.");
    }
}
