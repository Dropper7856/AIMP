using System.Buffers.Binary;
using System.Security.Cryptography;

namespace AIMP.Launcher;

/// <summary>
/// Recreates the Alien behavior file from the user's *own* exact retail BML and
/// a verified copy/insert delta. The repository deliberately does not contain
/// the original or the complete modified game BML.
/// This operation happens in lobby preflight, not the launcher's global install.
/// </summary>
internal static class AlienPvPBehaviorInstaller
{
    internal const string RetailSha256 = "334CB41B70468A1DCC6C7054F391C7715C06FA7F287AB5468D24D0BEDE30D46D";
    internal const string PvPSha256 = "0307A8346C242BFB0F7253D0E4DE6B5BF73426A3B73891408BAF5530AA0671E5";
    private const string PatchResource = "AIMP.Launcher.AlienPvP.alien_behavior.apb";
    private const int MaximumBmlBytes = 1024 * 1024;
    private const int MaximumCommands = 10_000;

    private static string Resolve(string gameDirectory) =>
        Path.Combine(Path.GetFullPath(gameDirectory), "DATA", "BINARY_BEHAVIOR", "_DIRECTORY_CONTENTS.BML");

    internal static void Prepare(string gameDirectory)
    {
        var path = Resolve(gameDirectory);
        var backup = path + ".aimp-original";
        if (!File.Exists(path))
            throw new FileNotFoundException("The game's original Alien behavior BML is missing.", path);
        var current = File.ReadAllBytes(path);
        var hash = Hash(current);
        if (hash == PvPSha256)
        {
            if (!File.Exists(backup) || Hash(File.ReadAllBytes(backup)) != RetailSha256)
                throw new InvalidDataException("An Alien PvP BML is installed without a valid original backup. Repair the installation.");
            return;
        }
        if (hash != RetailSha256)
            throw new InvalidDataException("Alien PvP needs an unmodified supported BML. Another mod or game version changed DATA/BINARY_BEHAVIOR.");
        if (File.Exists(backup) && Hash(File.ReadAllBytes(backup)) != RetailSha256)
            throw new InvalidDataException("The existing Alien PvP original backup has an unknown hash.");
        var patched = BuildFromRetail(current);
        // Do not rewrite or discard an existing verified backup.
        if (!File.Exists(backup)) Publish(backup, current);
        Publish(path, patched);
        if (Hash(File.ReadAllBytes(path)) != PvPSha256)
            throw new IOException("Alien PvP behavior installation did not verify.");
    }

    internal static void Restore(string gameDirectory)
    {
        var path = Resolve(gameDirectory);
        var backup = path + ".aimp-original";
        if (!File.Exists(path)) return;
        var hash = Hash(File.ReadAllBytes(path));
        if (hash == RetailSha256) return;
        // Never overwrite an unrelated mod or a damaged file.
        if (hash != PvPSha256)
            throw new InvalidDataException("Alien PvP cannot restore an unknown behavior BML.");
        if (!File.Exists(backup))
            throw new FileNotFoundException("Alien PvP original BML backup is missing.", backup);
        var original = File.ReadAllBytes(backup);
        if (Hash(original) != RetailSha256)
            throw new InvalidDataException("Alien PvP cannot restore a corrupted backup.");
        Publish(path, original);
    }

    internal static byte[] BuildFromRetail(byte[] original)
    {
        if (Hash(original) != RetailSha256)
            throw new InvalidDataException("The behavior patch's retail base hash does not match this installation.");
        using var resource = typeof(AlienPvPBehaviorInstaller).Assembly.GetManifestResourceStream(PatchResource)
            ?? throw new InvalidDataException("Alien PvP behavior delta is missing from the launcher build.");
        using var buffer = new MemoryStream();
        resource.CopyTo(buffer);
        var result = ApplyDelta(original, buffer.ToArray());
        if (Hash(result) != PvPSha256)
            throw new InvalidDataException("The reconstructed Alien behavior BML has an unexpected SHA-256.");
        return result;
    }

    // Format APB1: u32 command count, followed by bounded copy/insert commands:
    //   0 = copy {u32 offset, u32 length} from exact retail file;
    //   1 = insert {u32 length, bytes} owned by the delta.
    internal static byte[] ApplyDelta(ReadOnlySpan<byte> retail, ReadOnlySpan<byte> delta)
    {
        if (delta.Length < 8 || delta[0] != (byte)'A' || delta[1] != (byte)'P'
            || delta[2] != (byte)'B' || delta[3] != (byte)'1')
            throw new InvalidDataException("Invalid Alien PvP delta magic.");
        var count = BinaryPrimitives.ReadUInt32LittleEndian(delta[4..]);
        if (count is 0 or > MaximumCommands)
            throw new InvalidDataException("Alien PvP delta command count is invalid.");
        var pos = 8;
        using var output = new MemoryStream();
        for (var i = 0U; i < count; i++)
        {
            if (delta.Length - pos < 5)
                throw new InvalidDataException("Truncated Alien PvP delta command.");
            var kind = delta[pos++];
            var offsetOrLength = BinaryPrimitives.ReadUInt32LittleEndian(delta[pos..]);
            pos += 4;
            if (kind == 0)
            {
                if (delta.Length - pos < 4)
                    throw new InvalidDataException("Truncated Alien PvP copy range.");
                var length = BinaryPrimitives.ReadUInt32LittleEndian(delta[pos..]);
                pos += 4;
                if (offsetOrLength > retail.Length || length > retail.Length - offsetOrLength
                    || output.Length + length > MaximumBmlBytes)
                    throw new InvalidDataException("Alien PvP copy range exceeds the verified retail source.");
                output.Write(retail.Slice((int)offsetOrLength, (int)length));
            }
            else if (kind == 1)
            {
                if (offsetOrLength > delta.Length - pos || output.Length + offsetOrLength > MaximumBmlBytes)
                    throw new InvalidDataException("Alien PvP insert exceeds the delta bounds.");
                output.Write(delta.Slice(pos, (int)offsetOrLength));
                pos += (int)offsetOrLength;
            }
            else throw new InvalidDataException("Unknown Alien PvP delta operation.");
        }
        if (pos != delta.Length)
            throw new InvalidDataException("Alien PvP delta contains trailing bytes.");
        return output.ToArray();
    }

    private static void Publish(string destination, byte[] data)
    {
        var temporary = destination + ".aimp-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllBytes(temporary, data);
            File.Move(temporary, destination, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string Hash(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
}
