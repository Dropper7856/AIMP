using System.Text.Json.Serialization;

namespace AIMP.Protocol;

public sealed record NativeMissionSignalPayload(
    [property: JsonRequired] int LevelGeneration,
    [property: JsonRequired] ulong Sequence,
    [property: JsonRequired] string EntityId,
    [property: JsonRequired] uint Signal,
    [property: JsonRequired] byte InterfaceSelector);
