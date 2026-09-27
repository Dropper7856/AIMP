using System.Buffers.Binary;

namespace AIMP.Injector32;

internal static class PeImage
{
    private const ushort ImageFileMachineI386 = 0x014c;
    private const ushort ImageFileDll = 0x2000;
    private const ushort DosSignature = 0x5A4D;
    private const uint PeSignature = 0x00004550;
    private const int DosHeaderSize = 64;
    private const int PeHeaderSize = 24;

    public static void EnsureX86Dll(string path)
    {
        EnsureFileExists(path);
        using var stream = OpenImage(path);
        var dosHeader = ReadDosHeader(stream);
        var peOffset = ReadPeOffset(dosHeader, stream.Length);
        var peHeader = ReadPeHeader(stream, peOffset);
        ValidateMachine(peHeader);
        ValidateDllFlag(peHeader);
    }

    private static void EnsureFileExists(string path)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("The DLL to inject was not found.", path);
        }
    }

    private static FileStream OpenImage(string path) =>
        new(path, FileMode.Open, FileAccess.Read, FileShare.Read);

    private static byte[] ReadDosHeader(Stream stream)
    {
        var header = ReadBytes(stream, DosHeaderSize);
        if (BinaryPrimitives.ReadUInt16LittleEndian(header) != DosSignature)
        {
            throw new BadImageFormatException("The DOS header is invalid.");
        }

        return header;
    }

    private static int ReadPeOffset(ReadOnlySpan<byte> dosHeader, long imageLength)
    {
        var offset = BinaryPrimitives.ReadInt32LittleEndian(dosHeader[0x3C..]);
        if (offset < DosHeaderSize || offset > imageLength - PeHeaderSize)
        {
            throw new BadImageFormatException("The PE header offset is invalid.");
        }

        return offset;
    }

    private static byte[] ReadPeHeader(Stream stream, int offset)
    {
        stream.Position = offset;
        var header = ReadBytes(stream, PeHeaderSize);
        if (BinaryPrimitives.ReadUInt32LittleEndian(header) != PeSignature)
        {
            throw new BadImageFormatException("The PE signature is invalid.");
        }

        return header;
    }

    private static void ValidateMachine(ReadOnlySpan<byte> header)
    {
        var machine = BinaryPrimitives.ReadUInt16LittleEndian(header[4..]);
        if (machine != ImageFileMachineI386)
        {
            throw new BadImageFormatException(
                $"The DLL must be x86; found machine 0x{machine:X4}.");
        }
    }

    private static void ValidateDllFlag(ReadOnlySpan<byte> header)
    {
        var characteristics = BinaryPrimitives.ReadUInt16LittleEndian(header[22..]);
        if ((characteristics & ImageFileDll) == 0)
        {
            throw new BadImageFormatException("The PE image is not marked as a DLL.");
        }
    }

    private static byte[] ReadBytes(Stream stream, int count)
    {
        var bytes = new byte[count];
        try
        {
            stream.ReadExactly(bytes);
            return bytes;
        }
        catch (EndOfStreamException exception)
        {
            throw new BadImageFormatException("The PE image is truncated.", exception);
        }
    }
}
