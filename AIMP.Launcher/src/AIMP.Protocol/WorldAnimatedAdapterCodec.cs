using System.Buffers.Binary;

namespace AIMP.Protocol;

public enum WorldDoorAction : byte
{
    Open = 1,
    Close = 2,
    Lock = 3,
    Unlock = 4,
    ForceOpen = 5,
    ForceClose = 6
}

public enum WorldAnimatedTargetCategory : byte
{
    PlayEnvironmentAnimation = 1,
    AnimatedModelAttachmentNode = 2
}

public readonly record struct WorldDoorActionPayload(
    WorldDoorAction Action,
    ulong ObservationId);

public readonly record struct WorldAnimatedInputEdgePayload(
    WorldAnimatedTargetCategory TargetCategory,
    uint SchedulerParameterShortGuid,
    uint TargetInputShortGuid,
    ulong ObservationId,
    WorldEntityId SourceEntity,
    float RelativeDelaySeconds);

/// <summary>
/// v12.7 bounded contracts for Door actions and parameterless CATHODE edges
/// entering evidence-backed animated-world targets. Both are one-shot Events;
/// neither format carries transforms, physics state, callbacks or typed values.
/// </summary>
public static class WorldAnimatedAdapterCodec
{
    public const uint DoorActionOperation = 0x000C0701U;
    public const uint AnimatedInputEdgeOperation = 0x000C0702U;
    public const byte PayloadVersion = 1;
    public const byte AnimatedInputEdgeKind = 1;
    public const int DoorActionPayloadBytes = 16;
    public const int AnimatedInputEdgePayloadBytes = 48;
    public const float MaximumRelativeDelaySeconds = 600.0F;

    public static byte[] EncodeDoorAction(WorldDoorActionPayload value)
    {
        ValidateDoorActionValue(value);
        var payload = new byte[DoorActionPayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = (byte)value.Action;
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(8, 8), value.ObservationId);
        return payload;
    }

    public static WorldDoorActionPayload DecodeDoorAction(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != DoorActionPayloadBytes || payload[0] != PayloadVersion
            || payload[2] != 0 || payload[3] != 0 || payload[4] != 0
            || payload[5] != 0 || payload[6] != 0 || payload[7] != 0)
            throw new ProtocolException("World Door action payload header is invalid.");

        var value = new WorldDoorActionPayload(
            (WorldDoorAction)payload[1],
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(8, 8)));
        ValidateDoorActionValue(value);
        return value;
    }

    public static byte[] EncodeAnimatedInputEdge(WorldAnimatedInputEdgePayload value)
    {
        ValidateAnimatedInputEdgeValue(value);
        var payload = new byte[AnimatedInputEdgePayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = AnimatedInputEdgeKind;
        payload[2] = (byte)value.TargetCategory;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), value.SchedulerParameterShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), value.TargetInputShortGuid);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(16, 8), value.ObservationId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(24, 8), value.SourceEntity.High);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(32, 8), value.SourceEntity.Low);
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(40, 4), BitConverter.SingleToUInt32Bits(value.RelativeDelaySeconds));
        return payload;
    }

    public static WorldAnimatedInputEdgePayload DecodeAnimatedInputEdge(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != AnimatedInputEdgePayloadBytes || payload[0] != PayloadVersion
            || payload[1] != AnimatedInputEdgeKind || payload[3] != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4)) != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(44, 4)) != 0)
            throw new ProtocolException("World animated input edge payload header is invalid.");

        var value = new WorldAnimatedInputEdgePayload(
            (WorldAnimatedTargetCategory)payload[2],
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(16, 8)),
            new WorldEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(24, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(32, 8))),
            BitConverter.UInt32BitsToSingle(
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(40, 4))));
        ValidateAnimatedInputEdgeValue(value);
        return value;
    }

    public static void ValidateIntent(WorldIntent value)
    {
        if (value.Operation == DoorActionOperation)
        {
            RequireEvent(value.RequestedKind, "Door actions");
            _ = DecodeDoorAction(value.Payload);
        }
        else if (value.Operation == AnimatedInputEdgeOperation)
        {
            RequireEvent(value.RequestedKind, "Animated-world input edges");
            _ = DecodeAnimatedInputEdge(value.Payload);
        }
    }

    public static void ValidateMutation(WorldMutation value)
    {
        if (value.Operation == DoorActionOperation)
        {
            RequireEvent(value.Kind, "Door actions");
            _ = DecodeDoorAction(value.Payload);
        }
        else if (value.Operation == AnimatedInputEdgeOperation)
        {
            RequireEvent(value.Kind, "Animated-world input edges");
            _ = DecodeAnimatedInputEdge(value.Payload);
        }
    }

    private static void RequireEvent(WorldMutationKind kind, string label)
    {
        if (kind != WorldMutationKind.Event)
            throw new ProtocolException($"{label} must use one-shot WorldEvent semantics.");
    }

    private static void ValidateDoorActionValue(WorldDoorActionPayload value)
    {
        if (!Enum.IsDefined(value.Action) || value.ObservationId == 0)
            throw new ProtocolException("World Door action payload is invalid.");
    }

    private static void ValidateAnimatedInputEdgeValue(WorldAnimatedInputEdgePayload value)
    {
        if (!Enum.IsDefined(value.TargetCategory)
            || value.SchedulerParameterShortGuid == 0
            || value.TargetInputShortGuid == 0
            || value.ObservationId == 0
            || !value.SourceEntity.IsValid
            || !float.IsFinite(value.RelativeDelaySeconds)
            || value.RelativeDelaySeconds < 0
            || value.RelativeDelaySeconds > MaximumRelativeDelaySeconds)
            throw new ProtocolException("World animated input edge payload is invalid.");
    }
}
