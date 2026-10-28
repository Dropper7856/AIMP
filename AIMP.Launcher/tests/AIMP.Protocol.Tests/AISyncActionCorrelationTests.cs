using System.Buffers.Binary;

namespace AIMP.Protocol.Tests;

internal static class AISyncActionCorrelationTests
{
    internal static Task ContractAsync()
    {
        var sessionBytes=new byte[16];sessionBytes[0]=7;sessionBytes[15]=31;
        var session=new Guid(sessionBytes);var state=AISyncCodecTests.ValidState(session);
        W32(state,16,3);W32(state,20,1);W32(state,24,2);W32(state,28,4);W32(state,32,3);
        W32(state,40,63);W32(state,240,9);W32(state,244,0xEADB58);W32(state,248,0x3B);W32(state,252,1);
        AssertEx.True(AISyncActionCorrelation.Read(state) is null);
        string? path=null;
        for(var at=new DirectoryInfo(AppContext.BaseDirectory);at is not null;at=at.Parent) {
            var candidate=Path.Combine(at.FullName,"tests","fixtures","aisync-action-reference-v1.hex");
            if(File.Exists(candidate)){path=candidate;break;}
        }
        AssertEx.True(path is not null);
        var fixture=Convert.FromHexString(string.Concat(File.ReadAllText(path!).Where(c=>!char.IsWhiteSpace(c))));
        AssertEx.Equal(88,fixture.Length);fixture.CopyTo(state,AISyncActionCorrelation.Offset);
        AISyncCodec.ValidateNative(state);
        var reference=AISyncActionCorrelation.Read(state)!.Value;
        AssertEx.Equal(1UL,reference.Source.ConsumerEpoch);AssertEx.Equal(9U,reference.ExecutionRevision);
        var consumer=reference.Source with {ConsumerEpoch=42};
        var operation=new AISyncTransaction {
            Session=session,AuthorityEpoch=2,WorldEpoch=3,ConsumerEpoch=42,OwnerEpoch=5,
            EntityA=1,EntityB=2,Birth=3,CharacterClass=4,Capability=AISyncTransactionCapability.Movement,
            CommandId=1,GlobalSequence=2,OwnerSequence=2,ParameterRevision=1,NativeKind=5,
            Opcode=AISyncTransactionOpcode.Submit,Mode=0x3B };
        var batch=new AISyncExecutionBatch(consumer,1,new[]{operation});
        var decoded=AISyncExecutionCodec.DecodeBatch(AISyncExecutionCodec.EncodeBatch(batch),consumer).Records[0];
        AssertEx.True(AISyncActionCorrelation.Matches(state,consumer,decoded));
        foreach(var wrong in new[]{operation with {CommandId=2},operation with {Birth=4},operation with {EntityA=2},
            operation with {CharacterClass=3},operation with {OwnerEpoch=6},operation with {ParameterRevision=2},
            operation with {GlobalSequence=3},operation with {OwnerSequence=3},operation with {Mode=0x42},
            operation with {NativeKind=7},operation with {ConsumerEpoch=41}})
            AssertEx.False(AISyncActionCorrelation.Matches(state,consumer,wrong));
        AssertEx.False(AISyncActionCorrelation.Matches(state,consumer with {AuthorityEpoch=3},operation));
        var collision=sessionBytes.ToArray();collision[15]^=1;var otherSession=new Guid(collision);
        AssertEx.Equal(AISyncCodec.SessionCookie(session),AISyncCodec.SessionCookie(otherSession));
        AssertEx.False(AISyncActionCorrelation.Matches(state,consumer with {Session=otherSession},operation with {Session=otherSession}));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.EncodeNetwork(otherSession,state));
        AssertEx.False(AISyncCodec.BelongsTo(state,otherSession,3));
        foreach(var offset in new[]{692,700,708,716,724,732,740,748,752,756,760}) {
            var invalid=state.ToArray();invalid.AsSpan(offset,offset<748?8:4).Clear();
            AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(invalid));
        }
        var old=state.ToArray();W32(old,0,13);AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(old));
        AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(state.AsSpan(0,676)));
        var stale=state.ToArray();W32(stale,240,10);AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(stale));
        var paired=state.ToArray();otherSession.TryWriteBytes(paired.AsSpan(644,16));
        W64(paired,660,2);W64(paired,668,1);AssertEx.Throws<ProtocolException>(()=>AISyncCodec.ValidateNative(paired));
        AssertEx.SequenceEqual(state,AISyncCodec.DecodeNetwork(AISyncCodec.EncodeNetwork(session,state),session).ToArray());
        return Task.CompletedTask;
    }
    private static void W32(byte[] b,int at,uint value)=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(at),value);
    private static void W64(byte[] b,int at,ulong value)=>BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(at),value);
}
