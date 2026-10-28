using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct AISyncActionReference(AISyncExecutionScope Source, ulong OwnerEpoch,
    ulong CommandId, ulong GlobalSequence, ulong OwnerSequence, uint ParameterRevision,
    uint NativeKind, uint ExecutionRevision, uint CycleState);

// Observation only. Neither reading a reference nor matching an AIEX operation
// grants actor ownership, proves native entry or starts an animation.
public static class AISyncActionCorrelation
{
    public const int Offset = 676, Bytes = 88;
    internal static AISyncActionReference? ReadRaw(ReadOnlySpan<byte> state)
    {
        var b=state.Slice(Offset,Bytes);
        if(b.IndexOfAnyExcept((byte)0)<0)return null;
        return new(new(new Guid(b[..16]),U64(b,16),U64(b,24),U64(b,32)),
            U64(b,40),U64(b,48),U64(b,56),U64(b,64),U32(b,72),U32(b,76),U32(b,80),U32(b,84));
    }
    public static AISyncActionReference? Read(ReadOnlySpan<byte> state)
    { AISyncCodec.ValidateNative(state);return ReadRaw(state); }

    internal static void ValidateState(ReadOnlySpan<byte> state)
    {
        if(ReadRaw(state) is not {} value)return;
        var pairSession=new Guid(state.Slice(644,16));
        if(value.Source.ConsumerEpoch!=1||value.NativeKind!=5||value.ExecutionRevision==0||
            value.ExecutionRevision!=U32(state,240)||value.CycleState!=U32(state,248)||
            (U32(state,40)&(1U<<5))==0||(U32(state,252)&5U)==0||
            value.Source.WorldEpoch!=U32(state,16)||AISyncCodec.SessionCookie(value.Source.Session)!=U64(state,8)||
            (pairSession!=Guid.Empty&&pairSession!=value.Source.Session))
            throw new ProtocolException("Invalid AISync action reservation reference.");
        AISyncExecutionCodec.ValidateExecutable(new AISyncTransaction {
            Session=value.Source.Session,AuthorityEpoch=value.Source.AuthorityEpoch,WorldEpoch=value.Source.WorldEpoch,
            ConsumerEpoch=value.Source.ConsumerEpoch,OwnerEpoch=value.OwnerEpoch,
            EntityA=U32(state,20),EntityB=U32(state,24),Birth=U32(state,32),CharacterClass=U32(state,28),
            Capability=AISyncTransactionCapability.Movement,CommandId=value.CommandId,
            GlobalSequence=value.GlobalSequence,OwnerSequence=value.OwnerSequence,ParameterRevision=value.ParameterRevision,
            NativeKind=value.NativeKind,Opcode=AISyncTransactionOpcode.Submit,Mode=value.CycleState });
    }
    // The consumer scope is the authenticated current peer binding. The source
    // scope's consumer=1 is never used as the peer's consumer epoch.
    public static bool Matches(ReadOnlySpan<byte> state,AISyncExecutionScope consumer,AISyncTransaction operation)
    {
        if(Read(state) is not {} value)return false;
        try {AISyncExecutionCodec.ValidateExecutable(operation,consumer);}
        catch(ProtocolException) {return false;}
        return value.Source.Session==consumer.Session&&value.Source.AuthorityEpoch==consumer.AuthorityEpoch&&
            value.Source.WorldEpoch==consumer.WorldEpoch&&operation.OwnerEpoch==value.OwnerEpoch&&
            operation.EntityA==U32(state,20)&&operation.EntityB==U32(state,24)&&operation.Birth==U32(state,32)&&
            operation.CharacterClass==U32(state,28)&&operation.Capability==AISyncTransactionCapability.Movement&&
            operation.CommandId==value.CommandId&&operation.ParameterRevision==value.ParameterRevision&&
            operation.GlobalSequence==value.GlobalSequence&&operation.OwnerSequence==value.OwnerSequence&&
            operation.NativeKind==value.NativeKind&&operation.Mode==value.CycleState&&
            operation.Opcode is AISyncTransactionOpcode.Submit or AISyncTransactionOpcode.Update;
    }
    private static uint U32(ReadOnlySpan<byte> b,int at)=>BinaryPrimitives.ReadUInt32LittleEndian(b[at..]);
    private static ulong U64(ReadOnlySpan<byte> b,int at)=>BinaryPrimitives.ReadUInt64LittleEndian(b[at..]);
}
