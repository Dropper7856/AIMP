using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using AIMP.Protocol;

namespace AIMP.Launcher;

public sealed record CampaignSaveMetadata(string Id, string Label, string Level, string Difficulty,
    string CampaignId, string Sha256, long ByteLength, DateTimeOffset CreatedUtc);
public sealed record CampaignSaveBundle(CampaignSaveMetadata Metadata, byte[] Bytes);

// All paths supplied to this class must belong to launcher/native-owned multiplayer storage.
// Staging is additionally confined to the constructor's storage root.
public sealed class CampaignSaveStore
{
    private const int MaximumFiles = 64, MaximumFileBytes = 4 * 1024 * 1024;
    private readonly string _root;
    private static readonly Regex SaveName = new("\\A(PROGRESSION|CHSV[0-9]{2}|MSSV[0-9]{2}|SPSV[0-9]{2})\\.ais\\z",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    public CampaignSaveStore(string rootPath)
    {
        _root = Path.GetFullPath(rootPath);
        RejectPersonalSavePath(_root);
        RejectReparsePoints(_root);
        Directory.CreateDirectory(_root);
        RejectReparsePoints(_root);
    }

    public IReadOnlyList<CampaignSaveMetadata> List()
    {
        RejectReparsePoints(_root);
        var result = new List<CampaignSaveMetadata>();
        foreach (var path in Directory.EnumerateFiles(_root, "*.json"))
        {
            var id = Path.GetFileNameWithoutExtension(path);
            try { result.Add(Read(id).Metadata); }
            catch (Exception error) when (error is IOException or InvalidDataException or JsonException
                or ArgumentException or ProtocolException) { /* Corrupt/incomplete snapshots are not offered. */ }
        }
        return result.OrderByDescending(value => value.CreatedUtc).ToArray();
    }

    public CampaignSaveMetadata CreateSnapshot(string profileDirectory, string campaignId, string difficulty, string level)
    {
        var profile = Path.GetFullPath(profileDirectory);
        RejectPersonalSavePath(profile);
        RejectReparsePoints(profile);
        var files = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        string? newestCheckpoint = null;
        var newestWrite = DateTime.MinValue;
        long totalBytes = 0;
        foreach (var path in Directory.EnumerateFileSystemEntries(profile))
        {
            var name = Path.GetFileName(path);
            if (!SaveName.IsMatch(name)) continue;
            RejectReparsePoints(path);
            if ((File.GetAttributes(path) & FileAttributes.Directory) != 0)
                throw new InvalidDataException("A save entry is a directory.");
            if (files.Count >= MaximumFiles) throw new InvalidDataException("Too many save files.");
            using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            if (input.Length is < 1 or > MaximumFileBytes) throw new InvalidDataException("Save file size is invalid.");
            if ((totalBytes += input.Length) > CampaignSaveValidation.MaximumBundleBytes)
                throw new InvalidDataException("Save data is too large.");
            var bytes = new byte[checked((int)input.Length)];
            input.ReadExactly(bytes);
            var key = name[..^4].ToUpperInvariant() + ".ais";
            if (!files.TryAdd(key, bytes)) throw new InvalidDataException("Duplicate save file.");
            var written = File.GetLastWriteTimeUtc(path);
            if (key.StartsWith("CHSV", StringComparison.Ordinal) && written >= newestWrite)
            { newestWrite = written; newestCheckpoint = key; }
        }
        // The newest checkpoint, never the caller's UI level, is authoritative.
        var levels = ValidateFiles(files);
        var savedLevel = levels[newestCheckpoint ?? throw new InvalidDataException("No checkpoint was written.")];
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, true))
            foreach (var file in files.OrderBy(pair => pair.Key, StringComparer.Ordinal))
            {
                using var output = zip.CreateEntry(file.Key, CompressionLevel.NoCompression).Open();
                output.Write(file.Value);
            }
        var bundle = buffer.ToArray();
        if (bundle.Length > CampaignSaveValidation.MaximumBundleBytes) throw new InvalidDataException("Save bundle is too large.");
        var label = DateTime.Now.ToString("yyyy-MM-dd HH:mm", System.Globalization.CultureInfo.InvariantCulture);
        var metadata = new CampaignSaveMetadata(Guid.NewGuid().ToString("N"), label, savedLevel,
            difficulty, campaignId, Convert.ToHexString(SHA256.HashData(bundle)), bundle.Length, DateTimeOffset.UtcNow);
        CampaignSaveValidation.Validate(Descriptor(metadata));
        var zipPath = Path.Combine(_root, metadata.Id + ".zip");
        using (var output = new FileStream(zipPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        { output.Write(bundle); output.Flush(true); }
        // Metadata is the commit marker; snapshots are never overwritten.
        using (var output = new FileStream(Path.Combine(_root, metadata.Id + ".json"), FileMode.CreateNew, FileAccess.Write, FileShare.None))
            JsonSerializer.Serialize(output, metadata);
        return metadata;
    }

    public CampaignSaveBundle Read(string id)
    {
        if (!Guid.TryParseExact(id, "N", out _)) throw new ArgumentException("Invalid snapshot id.", nameof(id));
        var metadataPath = Path.Combine(_root, id + ".json");
        var zipPath = Path.Combine(_root, id + ".zip");
        RejectReparsePoints(metadataPath); RejectReparsePoints(zipPath);
        if (new FileInfo(metadataPath).Length > 8192) throw new InvalidDataException("Snapshot metadata is too large.");
        var metadata = JsonSerializer.Deserialize<CampaignSaveMetadata>(File.ReadAllText(metadataPath))
            ?? throw new InvalidDataException("Missing snapshot metadata.");
        if (metadata.Id != id)
            throw new InvalidDataException("Snapshot identity or length mismatch.");
        CampaignSaveValidation.Validate(Descriptor(metadata));
        using var input = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (input.Length != metadata.ByteLength) throw new InvalidDataException("Snapshot length mismatch.");
        var bytes = new byte[checked((int)metadata.ByteLength)];
        input.ReadExactly(bytes);
        CampaignSaveValidation.ValidateBundle(Descriptor(metadata), bytes);
        if (!ValidateFiles(Unpack(bytes)).ContainsValue(metadata.Level)) throw new InvalidDataException("Snapshot level mismatch.");
        return new CampaignSaveBundle(metadata, bytes);
    }

    public void Delete(string id)
    {
        if (!Guid.TryParseExact(id, "N", out _)) throw new ArgumentException("Invalid snapshot id.", nameof(id));
        RejectReparsePoints(_root);
        // The metadata is the commit marker: remove it first.
        File.Delete(Path.Combine(_root, id + ".json"));
        File.Delete(Path.Combine(_root, id + ".zip"));
    }

    public void Stage(byte[] bundle, string destination)
    {
        ArgumentNullException.ThrowIfNull(bundle);
        var files = Unpack(bundle); ValidateFiles(files);
        var target = Path.GetFullPath(destination);
        if (!target.StartsWith(_root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Staging must remain under the multiplayer storage root.");
        RejectPersonalSavePath(target);
        var parent = Path.GetDirectoryName(target)!;
        RejectReparsePoints(parent);
        if (Directory.Exists(target) || File.Exists(target)) throw new IOException("Staging destination must be fresh.");
        Directory.CreateDirectory(parent); RejectReparsePoints(parent);
        var temporary = Path.Combine(parent, ".campaign-stage-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            foreach (var file in files) File.WriteAllBytes(Path.Combine(temporary, file.Key), file.Value);
            Directory.Move(temporary, target);
        }
        finally { if (Directory.Exists(temporary)) Directory.Delete(temporary, true); }
    }

    private static Dictionary<string, byte[]> Unpack(byte[] bundle)
    {
        if (bundle.Length is < 1 or > CampaignSaveValidation.MaximumBundleBytes) throw new InvalidDataException("Invalid bundle size.");
        using var stream = new MemoryStream(bundle, false);
        using var zip = new ZipArchive(stream, ZipArchiveMode.Read);
        if (zip.Entries.Count is < 1 or > MaximumFiles) throw new InvalidDataException("Invalid save file count.");
        var files = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var entry in zip.Entries)
        {
            if (!SaveName.IsMatch(entry.FullName) || entry.FullName.Contains('/') || entry.FullName.Contains('\\')
                || entry.Length is < 1 or > MaximumFileBytes || (entry.ExternalAttributes & 0x400) != 0
                || ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000
                || (total += entry.Length) > CampaignSaveValidation.MaximumBundleBytes)
                throw new InvalidDataException("Invalid save archive entry.");
            using var input = entry.Open();
            var bytes = new byte[checked((int)entry.Length)]; input.ReadExactly(bytes);
            if (input.ReadByte() != -1 || !files.TryAdd(entry.FullName, bytes)) throw new InvalidDataException("Duplicate or oversized save entry.");
        }
        return files;
    }

    // Returns the saved level of every checkpoint slot (CHSV##.ais).
    private static Dictionary<string, string> ValidateFiles(Dictionary<string, byte[]> files)
    {
        if (files.Values.Sum(value => (long)value.Length) > CampaignSaveValidation.MaximumBundleBytes)
            throw new InvalidDataException("Save data is too large.");
        foreach (var bytes in files.Values)
            if (bytes.Length < 4 || !bytes.AsSpan(0, 4).SequenceEqual("IAAC"u8)) throw new InvalidDataException("Invalid IAAC save header.");
        var levels = files.Where(pair => pair.Key.StartsWith("CHSV", StringComparison.OrdinalIgnoreCase))
            .ToDictionary(pair => pair.Key, pair => CheckpointLevel(pair.Value), StringComparer.OrdinalIgnoreCase);
        if (levels.Count == 0) throw new InvalidDataException("A checkpoint save is required.");
        return levels;
    }

    private static string CheckpointLevel(byte[] checkpoint)
    {
        if (checkpoint.Length <= 0x20) throw new InvalidDataException("Missing checkpoint level.");
        var header = checkpoint.AsSpan(0x20, Math.Min(128, checkpoint.Length - 0x20));
        var end = header.IndexOf((byte)0);
        if (end < 1) throw new InvalidDataException("Checkpoint level is not bounded or terminated.");
        var text = Encoding.ASCII.GetString(header[..end]);
        const string prefix = "Production\\";
        if (!text.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Invalid checkpoint level prefix.");
        var level = text[prefix.Length..];
        if (level.Length == 0 || level.Any(value => !char.IsAsciiLetterOrDigit(value) && value is not ('_' or '-')))
            throw new InvalidDataException("Invalid checkpoint level name.");
        return level;
    }

    private static CampaignLaunchDescriptor Descriptor(CampaignSaveMetadata value) =>
        new("continue", value.Id, value.CampaignId, value.Difficulty, value.Label, value.Sha256, value.ByteLength);

    private static void RejectPersonalSavePath(string path)
    {
        if (path.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Any(part =>
            part.Equals("Steam", StringComparison.OrdinalIgnoreCase) || part.Equals("userdata", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("Personal Steam save locations are forbidden.");
    }

    // Only the store root and what lies beneath it are AIMP-owned. System
    // ancestors such as %LOCALAPPDATA% can legitimately be junctions (a
    // relocated AppData), which previously disabled campaign saves entirely.
    // A path outside the root is still checked across its whole ancestry.
    private void RejectReparsePoints(string path)
    {
        var full = Path.GetFullPath(path);
        var rooted = full.Equals(_root, StringComparison.OrdinalIgnoreCase)
            || full.StartsWith(Path.TrimEndingDirectorySeparator(_root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        var boundary = rooted ? Path.GetDirectoryName(_root) : null;
        for (string? current = full; current is not null
            && (boundary is null || !current.Equals(boundary, StringComparison.OrdinalIgnoreCase));
            current = Path.GetDirectoryName(current))
            if ((Directory.Exists(current) || File.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Reparse points are forbidden in save paths.");
    }
}
