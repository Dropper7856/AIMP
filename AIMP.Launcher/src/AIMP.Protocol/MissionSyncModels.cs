using System.Text.Json.Serialization;

namespace AIMP.Protocol;

// A portable authored mission-graph ingress. Sequence is the sender's proposal
// sequence on proposal messages, and the Host's global order on committed ones.
public sealed record MissionSignalPayload(
    [property: JsonRequired] Guid SessionId,
    [property: JsonRequired] ulong PeerId,
    [property: JsonRequired] int LevelGeneration,
    [property: JsonRequired] ulong Sequence,
    [property: JsonRequired] string EntityId,
    [property: JsonRequired] uint Signal,
    [property: JsonRequired] byte InterfaceSelector,
    ulong Occurrence = 0);

public static class MissionSignalValidation
{
    public const int MaximumFrontiersPerGeneration = 65536;
    public const int MaximumSignalsPerGeneration = 1_000_000;

    public static void Validate(MissionSignalPayload? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.LevelGeneration <= 0 || value.Sequence == 0
            || !ModelValidation.IsWorldEntityIdText(value.EntityId)
            || value.Signal == 0 || value.InterfaceSelector is not (1 or 2 or 3 or 4)
            || value.Occurrence > MaximumSignalsPerGeneration)
            throw new ProtocolException("The mission signal is invalid.");
    }
}
