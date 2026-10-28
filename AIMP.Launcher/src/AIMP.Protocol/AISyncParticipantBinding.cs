using System.Buffers.Binary;

namespace AIMP.Protocol;

// Never sent across the lobby: Character/runtime/creation serial are DLL-local.
public sealed record AISyncParticipantBinding(uint Character,uint Incarnation,uint RuntimeId,ulong CreationSerial);

internal static class AISyncParticipantBindingCodec
{
    internal static void Write(Span<byte> bytes,ulong peer,AISyncParticipantBinding value)
    {
        if(bytes.Length!=32||peer==0||value.Character<0x10000
            ||value.Incarnation==0&&(value.RuntimeId!=0||value.CreationSerial!=0)
            ||value.Incarnation!=0&&(value.RuntimeId==0||value.CreationSerial==0
                ||(value.Character&15)!=0||value.Character>uint.MaxValue-0x50))
            throw new ProtocolException("Invalid local participant construction binding.");
        bytes.Clear(); BinaryPrimitives.WriteUInt64LittleEndian(bytes,peer);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[8..],value.Character);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[12..],value.Incarnation);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[16..],value.RuntimeId);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[24..],value.CreationSerial);
    }
}
