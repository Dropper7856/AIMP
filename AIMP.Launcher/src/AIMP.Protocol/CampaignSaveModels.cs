using System.Security.Cryptography;

namespace AIMP.Protocol;

public sealed record CampaignLaunchDescriptor(
    string Action, string? SaveId, string CampaignId, string Difficulty,
    string Label, string? Sha256 = null, long ByteLength = 0);

public static class CampaignSaveValidation
{
    public const int MaximumBundleBytes = 16 * 1024 * 1024;
    public const int ChunkBytes = 32 * 1024;

    public static void Validate(CampaignLaunchDescriptor value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.Action is not ("new" or "continue" or "level")
            || !Text(value.CampaignId, 128) || !Text(value.Difficulty, 64)
            || !Text(value.Label, 256))
            throw new ProtocolException("Invalid campaign launch descriptor.");
        if (value.Action == "continue")
        {
            if (!Guid.TryParseExact(value.SaveId, "N", out _) || value.Sha256 is not { Length: 64 }
                || !value.Sha256.All(Uri.IsHexDigit) || value.ByteLength is < 1 or > MaximumBundleBytes)
                throw new ProtocolException("Invalid campaign save identity or size.");
        }
        else if (value.SaveId is not null || value.Sha256 is not null || value.ByteLength != 0)
            throw new ProtocolException("Only continue launches may carry a save bundle.");
    }

    public static void ValidateBundle(CampaignLaunchDescriptor value, ReadOnlySpan<byte> bytes)
    {
        Validate(value);
        if (value.Action != "continue" || bytes.Length != value.ByteLength
            || !string.Equals(Convert.ToHexString(SHA256.HashData(bytes)), value.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new ProtocolException("Campaign save bundle hash or size mismatch.");
    }

    private static bool Text(string? value, int limit) => !string.IsNullOrWhiteSpace(value)
        && value.Length <= limit && !value.Any(char.IsControl);
}

internal sealed record CampaignSaveRequest(Guid SessionId, string SaveId, Guid RequestId);
internal sealed record CampaignSaveChunk(Guid SessionId, string SaveId, Guid RequestId, int Offset, byte[] Data);
