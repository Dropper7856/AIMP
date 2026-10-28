using System.Buffers.Binary;

namespace AIMP.Protocol;

// One value carrier in the SAME causal sender as requests, routes and native
// lifecycle calls. It is not a second snapshot publisher or a movement writer.
public sealed record AISyncCausalState(AISyncExecutionScope Scope, ulong Serial, ulong Through, byte[] State);
public static class AISyncCausalStateCodec
{
    public const uint Magic=0x45534941;
    public const int HeaderBytes=72, Bytes=HeaderBytes+AISyncCodec.NativeStateBytes;
    public static AISyncCausalState Decode(ReadOnlySpan<byte> bytes)
    {
        if(bytes.Length!=Bytes||BinaryPrimitives.ReadUInt32LittleEndian(bytes)!=Magic||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes[4..])!=1||BinaryPrimitives.ReadUInt16LittleEndian(bytes[6..])!=HeaderBytes||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[8..])!=Bytes||BinaryPrimitives.ReadUInt32LittleEndian(bytes[12..])!=0)
            throw new ProtocolException("Invalid causal AISync event envelope.");
        var scope=new AISyncExecutionScope(new Guid(bytes.Slice(16,16)),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[32..]),BinaryPrimitives.ReadUInt64LittleEndian(bytes[40..]),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[48..]));
        AISyncExecutionCodec.RequireCurrentBinding(scope,scope);
        var serial=BinaryPrimitives.ReadUInt64LittleEndian(bytes[56..]);
        var through=BinaryPrimitives.ReadUInt64LittleEndian(bytes[64..]);
        var state=bytes[HeaderBytes..];AISyncCodec.ValidateNative(state);
        if(!AISyncCodec.MatchesSnapshotSessions(state,scope.Session)||serial==0||BinaryPrimitives.ReadUInt64LittleEndian(state[8..])!=AISyncCodec.SessionCookie(scope.Session)||
            BinaryPrimitives.ReadUInt32LittleEndian(state[16..])!=scope.WorldEpoch)
            throw new ProtocolException("Causal AISync event differs from its current world.");
        return new(scope,serial,through,state.ToArray());
    }
    public static byte[] Encode(AISyncCausalState value)
    {
        var bytes=new byte[Bytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes,Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4),1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6),HeaderBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8),Bytes);
        value.Scope.Session.TryWriteBytes(bytes.AsSpan(16,16));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32),value.Scope.AuthorityEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(40),value.Scope.WorldEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(48),value.Scope.ConsumerEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(56),value.Serial);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(64),value.Through);
        if(value.State.Length!=AISyncCodec.NativeStateBytes)throw new ProtocolException("Invalid causal AISync event body.");
        value.State.CopyTo(bytes,HeaderBytes);_=Decode(bytes);return bytes;
    }
}
