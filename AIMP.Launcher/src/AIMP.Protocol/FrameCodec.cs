using System.Buffers;
using System.Buffers.Binary;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Text;

namespace AIMP.Protocol;

public static class FrameCodec
{
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes(ProtocolConstants.MagicText);

    public static byte[] Encode(AimpFrame frame)
    {
        var result = GC.AllocateUninitializedArray<byte>(EncodedLength(frame));
        EncodeInto(frame, result);
        return result;
    }

    private static int EncodedLength(AimpFrame frame)
    {
        ValidatePayloadLength(frame.Payload.Length);
        if (!Enum.IsDefined(frame.Type))
            throw new ProtocolException($"Unknown AIMP message type: {(ushort)frame.Type}.");
        if (frame.Flags != 0)
            throw new ProtocolException($"Unknown AIMP flags: 0x{frame.Flags:X4}.");
        return checked(ProtocolConstants.HeaderSize + frame.Payload.Length);
    }

    private static void EncodeInto(AimpFrame frame, Span<byte> result)
    {
        var required = EncodedLength(frame);
        if (result.Length < required)
            throw new ArgumentException("The destination frame buffer is too small.", nameof(result));
        Magic.AsSpan().CopyTo(result);
        BinaryPrimitives.WriteUInt16LittleEndian(result.Slice(4, 2), ProtocolConstants.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(result.Slice(6, 2), (ushort)frame.Type);
        BinaryPrimitives.WriteUInt16LittleEndian(result.Slice(8, 2), frame.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(result.Slice(10, 4), (uint)frame.Payload.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(result.Slice(14, 8), frame.Sequence);
        frame.Payload.Span.CopyTo(result[ProtocolConstants.HeaderSize..required]);
    }

    public static AimpFrame Decode(ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length < ProtocolConstants.HeaderSize)
        {
            throw new ProtocolException("The AIMP header is truncated.");
        }

        ValidateHeader(encoded[..ProtocolConstants.HeaderSize], out var type, out var flags,
            out var payloadLength, out var sequence);

        if (encoded.Length != ProtocolConstants.HeaderSize + payloadLength)
        {
            throw new ProtocolException("The frame length does not match the header.");
        }

        return new AimpFrame(type, flags, sequence,
            encoded[ProtocolConstants.HeaderSize..].ToArray());
    }

    public static async ValueTask WriteAsync(
        Stream stream,
        AimpFrame frame,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        var length = EncodedLength(frame);
        var rented = ArrayPool<byte>.Shared.Rent(length);
        try
        {
            EncodeInto(frame, rented.AsSpan(0, length));
            await stream.WriteAsync(rented.AsMemory(0, length), cancellationToken)
                .ConfigureAwait(false);

            // NetworkStream and PipeStream are already unbuffered transport
            // streams.  Flushing every AIMP frame adds no TCP value and on a
            // Windows named pipe can become a consumption barrier.
            if (stream is not NetworkStream && stream is not PipeStream)
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(rented);
        }
    }

    public static async ValueTask<AimpFrame> ReadAsync(
        Stream stream,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        var header = ArrayPool<byte>.Shared.Rent(ProtocolConstants.HeaderSize);
        try
        {
            await ReadExactlyAsync(
                stream, header.AsMemory(0, ProtocolConstants.HeaderSize), cancellationToken)
                .ConfigureAwait(false);

            ValidateHeader(header.AsSpan(0, ProtocolConstants.HeaderSize),
                out var type, out var flags, out var payloadLength, out var sequence);
            var payload = payloadLength == 0
                ? Array.Empty<byte>()
                : GC.AllocateUninitializedArray<byte>(payloadLength);
            if (payload.Length != 0)
                await ReadExactlyAsync(stream, payload, cancellationToken).ConfigureAwait(false);

            return new AimpFrame(type, flags, sequence, payload);
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(header);
        }
    }

    private static void ValidateHeader(
        ReadOnlySpan<byte> header,
        out MessageType type,
        out ushort flags,
        out int payloadLength,
        out ulong sequence)
    {
        if (!header[..4].SequenceEqual(Magic))
        {
            throw new ProtocolException("The AIMP magic value is invalid.");
        }

        var version = BinaryPrimitives.ReadUInt16LittleEndian(header.Slice(4, 2));
        if (version != ProtocolConstants.Version)
        {
            throw new ProtocolException($"Incompatible AIMP version: {version} (expected {ProtocolConstants.Version}).");
        }

        type = (MessageType)BinaryPrimitives.ReadUInt16LittleEndian(header.Slice(6, 2));
        if (!Enum.IsDefined(type))
        {
            throw new ProtocolException($"Unknown AIMP message type: {(ushort)type}.");
        }

        flags = BinaryPrimitives.ReadUInt16LittleEndian(header.Slice(8, 2));
        if (flags != 0)
        {
            throw new ProtocolException($"Unknown AIMP flags: 0x{flags:X4}.");
        }
        var encodedLength = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(10, 4));
        if (encodedLength > ProtocolConstants.MaximumPayloadSize)
        {
            throw new ProtocolException($"Payload is too large: {encodedLength} bytes.");
        }

        payloadLength = checked((int)encodedLength);
        sequence = BinaryPrimitives.ReadUInt64LittleEndian(header.Slice(14, 8));
    }

    private static async ValueTask ReadExactlyAsync(
        Stream stream,
        Memory<byte> destination,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < destination.Length)
        {
            var count = await stream.ReadAsync(destination[offset..], cancellationToken)
                .ConfigureAwait(false);
            if (count == 0)
            {
                throw new EndOfStreamException("The stream closed while reading an AIMP frame.");
            }

            offset += count;
        }
    }

    private static void ValidatePayloadLength(int length)
    {
        if (length is < 0 or > ProtocolConstants.MaximumPayloadSize)
        {
            throw new ProtocolException($"Payload exceeds the limit: {length} bytes.");
        }
    }
}
