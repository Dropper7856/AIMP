using System.Buffers.Binary;
using System.Text;

namespace AIMP.Protocol;

public static class DiscoveryCodec
{
    private const int HeaderSize = 12;
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes(
        ProtocolConstants.DiscoveryMagicText);

    public static byte[] Encode(DiscoveryAnnouncement announcement)
    {
        ModelValidation.Validate(announcement);
        var payload = JsonPayload.Serialize(announcement);
        if (payload.Length > ProtocolConstants.MaximumDiscoveryPayloadSize)
        {
            throw new ProtocolException("The discovery announcement exceeds the protocol limit.");
        }

        var datagram = GC.AllocateUninitializedArray<byte>(HeaderSize + payload.Length);
        Magic.CopyTo(datagram, 0);
        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(8, 2),
            ProtocolConstants.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(10, 2),
            checked((ushort)payload.Length));
        payload.CopyTo(datagram, HeaderSize);
        return datagram;
    }

    public static DiscoveryAnnouncement Decode(ReadOnlySpan<byte> datagram)
    {
        if (datagram.Length < HeaderSize || !datagram[..8].SequenceEqual(Magic))
        {
            throw new ProtocolException("The discovery announcement is invalid.");
        }

        var version = BinaryPrimitives.ReadUInt16LittleEndian(datagram.Slice(8, 2));
        if (version != ProtocolConstants.Version)
        {
            throw new ProtocolException($"Incompatible discovery version: {version}.");
        }

        var payloadLength = BinaryPrimitives.ReadUInt16LittleEndian(datagram.Slice(10, 2));
        if (payloadLength > ProtocolConstants.MaximumDiscoveryPayloadSize
            || datagram.Length != HeaderSize + payloadLength)
        {
            throw new ProtocolException("The discovery announcement length is invalid.");
        }

        var announcement = JsonPayload.Deserialize<DiscoveryAnnouncement>(
            datagram.Slice(HeaderSize, payloadLength));
        ModelValidation.Validate(announcement);
        return announcement;
    }
}
