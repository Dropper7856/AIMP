using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct WorldCathodeRuntimeKey(uint EntityA, uint EntityB)
{
    public bool IsValid => EntityA != 0;
}

public sealed record WorldCathodeTypedMessagePayload(
    uint MessageType,
    ulong ObservationId,
    WorldCathodeRuntimeKey SourceRuntimeKey,
    WorldCathodeRuntimeKey TargetRuntimeKey,
    double RelativeDelaySeconds,
    byte[] Data);

public static class WorldCathodeBusCodec
{
    public const uint TypedMessageOperation = 0x000C0801U;
    public const byte PayloadVersion = 2;
    public const byte IdentityPayloadVersion = 3;
    public const byte TypedMessageKind = 2;
    public const int TypedMessageHeaderBytes = 48;
    public const int TypedMessageIdentityHeaderBytes = 80;
    public const int MaximumTypedMessageDataBytes =
        WorldReplicationConstants.MaximumPayloadBytes - TypedMessageHeaderBytes;
    public const int MaximumTypedMessageIdentityDataBytes =
        WorldReplicationConstants.MaximumPayloadBytes - TypedMessageIdentityHeaderBytes;

    public static byte[] EncodeTypedMessage(WorldCathodeTypedMessagePayload value)
    {
        ArgumentNullException.ThrowIfNull(value);
        ValidateValue(value);
        var result = new byte[TypedMessageHeaderBytes + value.Data.Length];
        result[0] = PayloadVersion;
        result[1] = TypedMessageKind;
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(4, 4), value.MessageType);
        BinaryPrimitives.WriteUInt64LittleEndian(result.AsSpan(8, 8), value.ObservationId);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(16, 4), value.SourceRuntimeKey.EntityA);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(20, 4), value.SourceRuntimeKey.EntityB);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(24, 4), value.TargetRuntimeKey.EntityA);
        BinaryPrimitives.WriteUInt32LittleEndian(result.AsSpan(28, 4), value.TargetRuntimeKey.EntityB);
        BinaryPrimitives.WriteUInt64LittleEndian(
            result.AsSpan(32, 8), BitConverter.DoubleToUInt64Bits(value.RelativeDelaySeconds));
        BinaryPrimitives.WriteUInt16LittleEndian(result.AsSpan(40, 2), checked((ushort)value.Data.Length));
        value.Data.CopyTo(result.AsSpan(TypedMessageHeaderBytes));
        return result;
    }

    public static WorldCathodeTypedMessagePayload DecodeTypedMessage(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < TypedMessageHeaderBytes
            || payload.Length > WorldReplicationConstants.MaximumPayloadBytes
            || payload[1] != TypedMessageKind
            || payload[2] != 0 || payload[3] != 0
            || BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(42, 2)) != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(44, 4)) != 0)
            throw new ProtocolException("World CATHODE typed-message payload header is invalid.");

        var headerBytes = payload[0] switch
        {
            PayloadVersion => TypedMessageHeaderBytes,
            IdentityPayloadVersion => TypedMessageIdentityHeaderBytes,
            _ => throw new ProtocolException("World CATHODE typed-message payload version is invalid."),
        };
        if (payload.Length < headerBytes)
            throw new ProtocolException("World CATHODE typed-message payload header is invalid.");

        if (payload[0] == IdentityPayloadVersion)
        {
            var sourceHigh = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(48, 8));
            var sourceLow = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(56, 8));
            var targetHigh = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(64, 8));
            var targetLow = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(72, 8));
            if ((sourceHigh | sourceLow) == 0 || (targetHigh | targetLow) == 0)
                throw new ProtocolException("World CATHODE typed-message stable identity is invalid.");
        }

        var length = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(40, 2));
        var maximumDataBytes = WorldReplicationConstants.MaximumPayloadBytes - headerBytes;
        if (length > maximumDataBytes || payload.Length != headerBytes + length)
            throw new ProtocolException("World CATHODE typed-message payload length is invalid.");
        var value = new WorldCathodeTypedMessagePayload(
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(8, 8)),
            new WorldCathodeRuntimeKey(
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(20, 4))),
            new WorldCathodeRuntimeKey(
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(24, 4)),
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(28, 4))),
            BitConverter.UInt64BitsToDouble(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(32, 8))),
            payload.Slice(headerBytes, length).ToArray());
        ValidateValue(value);
        return value;
    }

    public static void ValidateIntent(WorldIntent value)
    {
        if (value.Operation != TypedMessageOperation) return;
        if (value.RequestedKind != WorldMutationKind.Event)
            throw new ProtocolException("CATHODE typed messages must use WorldEvent semantics.");
        _ = DecodeTypedMessage(value.Payload);
    }

    public static void ValidateMutation(WorldMutation value)
    {
        if (value.Operation != TypedMessageOperation) return;
        if (value.Kind != WorldMutationKind.Event)
            throw new ProtocolException("CATHODE typed messages cannot use replayable State/Timeline semantics.");
        _ = DecodeTypedMessage(value.Payload);
    }

    private static void ValidateValue(WorldCathodeTypedMessagePayload value)
    {
        if (value.ObservationId == 0 || !value.SourceRuntimeKey.IsValid
            || !value.TargetRuntimeKey.IsValid
            || !double.IsFinite(value.RelativeDelaySeconds)
            || value.RelativeDelaySeconds is < 0 or > 600
            || value.Data is null || value.Data.Length > MaximumTypedMessageDataBytes)
            throw new ProtocolException("World CATHODE typed-message payload is invalid.");
    }
}
