namespace AIMP.Protocol;

public sealed record AimpFrame(
    MessageType Type,
    ushort Flags,
    ulong Sequence,
    ReadOnlyMemory<byte> Payload)
{
    public static AimpFrame Empty(MessageType type, ulong sequence = 0, ushort flags = 0) =>
        new(type, flags, sequence, ReadOnlyMemory<byte>.Empty);
}
