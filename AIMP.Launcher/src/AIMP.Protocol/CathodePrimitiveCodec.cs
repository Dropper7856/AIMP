using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct CathodeTriggerInputPayload(
    uint InputShortGuid,
    WorldEntityId? SourceEntity);

/// <summary>
/// v12.5 wire contract for the first evidence-backed generic CATHODE primitive.
/// This codec only describes a parameterless FunctionEntity input edge; it does
/// not know or emulate Door/Pickup/Rewire behavior.
/// </summary>
public static class CathodePrimitiveCodec
{
    public const uint TriggerInputOperation = 0x000C0501U;
    public const byte PayloadVersion = 1;
    public const byte TriggerInputKind = 1;
    public const byte SourceEntityFlag = 1 << 0;
    public const int TriggerInputPayloadBytes = 24;

    public static byte[] EncodeTriggerInput(
        uint inputShortGuid,
        WorldEntityId? sourceEntity = null)
    {
        if (inputShortGuid == 0)
            throw new ProtocolException("CATHODE TriggerInput requires a non-zero input ShortGuid.");
        if (sourceEntity.HasValue && !sourceEntity.Value.IsValid)
            throw new ProtocolException("CATHODE TriggerInput source WorldEntityId is invalid.");

        var payload = new byte[TriggerInputPayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = TriggerInputKind;
        payload[2] = sourceEntity.HasValue ? SourceEntityFlag : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), inputShortGuid);
        if (sourceEntity is { } source)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(8, 8), source.High);
            BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(16, 8), source.Low);
        }
        return payload;
    }

    public static CathodeTriggerInputPayload DecodeTriggerInput(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != TriggerInputPayloadBytes
            || payload[0] != PayloadVersion
            || payload[1] != TriggerInputKind
            || payload[3] != 0
            || (payload[2] & ~SourceEntityFlag) != 0)
        {
            throw new ProtocolException("CATHODE TriggerInput payload header is invalid.");
        }

        var inputShortGuid = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
        if (inputShortGuid == 0)
            throw new ProtocolException("CATHODE TriggerInput ShortGuid is invalid.");

        var source = new WorldEntityId(
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(8, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(16, 8)));
        var hasSource = (payload[2] & SourceEntityFlag) != 0;
        if (hasSource != source.IsValid)
            throw new ProtocolException("CATHODE TriggerInput source flag/entity mismatch.");

        return new CathodeTriggerInputPayload(
            inputShortGuid,
            hasSource ? source : null);
    }

    public static void ValidateIntent(WorldIntent value)
    {
        if (value.Operation != TriggerInputOperation) return;
        if (value.RequestedKind != WorldMutationKind.Event)
            throw new ProtocolException("CATHODE TriggerInput must use WorldEvent semantics.");
        _ = DecodeTriggerInput(value.Payload);
    }

    public static void ValidateMutation(WorldMutation value)
    {
        if (value.Operation != TriggerInputOperation) return;
        if (value.Kind != WorldMutationKind.Event)
            throw new ProtocolException("CATHODE TriggerInput cannot be replayed as State/Timeline.");
        _ = DecodeTriggerInput(value.Payload);
    }
}
