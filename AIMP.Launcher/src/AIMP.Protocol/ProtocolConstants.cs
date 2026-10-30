namespace AIMP.Protocol;

public static class ProtocolConstants
{
    public const ushort Version = 27;
    public const int HeaderSize = 22;
    public const int MaximumPayloadSize = 1024 * 1024;
    public const int MaximumDiscoveryPayloadSize = 4096;
    public const int MaximumPlayerNameLength = 24;
    public const int MaximumServerNameLength = 48;
    public const int DefaultGamePort = 28015;
    public const int DefaultDiscoveryPort = 28016;
    public const int DefaultLobbyCapacity = 4;
    public const int MinimumLobbyCapacity = 2;
    public const int MaximumLobbyCapacity = 4;
    public const int ExperimentalSchemaVersion = 1;
    public const int MaximumExperimentalListItems = 64;
    public const int MaximumExperimentalTextLength = 1024;
    public const string MagicText = "AIMP";
    public const string DiscoveryMagicText = "AIMPDISC";
    // Documentation reference for the historical profile. This digest is never a
    // global allowlist and does not decide by itself whether injection is allowed.
    public const string HistoricalAiSha256 =
        "00D6BB846AC5A78916BAF363EB148AA255A23BF216A271CFD23A0ECE3C368364";

    public static string PipeName(int processId) => $"AIMP.NetZ.{processId}";
}
