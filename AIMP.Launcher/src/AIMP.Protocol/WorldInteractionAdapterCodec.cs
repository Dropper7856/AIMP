using System.Buffers.Binary;

namespace AIMP.Protocol;

public enum WorldInteractionSourceCategory : byte
{
    GenericWorld = 0,
    Interaction = 1,
    Pickup = 2,
    Rewire = 3
}

public readonly record struct WorldObservedInputEdgePayload(
    WorldInteractionSourceCategory SourceCategory,
    uint SchedulerParameterShortGuid,
    uint TargetInputShortGuid,
    ulong ObservationId,
    WorldEntityId SourceEntity,
    float RelativeDelaySeconds);

public readonly record struct WorldObservedCharacterCallbackInputEdgePayload(
    WorldInteractionSourceCategory SourceCategory,
    uint SchedulerParameterShortGuid,
    uint TargetInputShortGuid,
    uint CallbackInputShortGuid,
    uint CallbackConstructorFlags,
    uint CallbackExecutionVtableRva,
    uint CallbackFamilyCallerRva,
    ulong ObservationId,
    WorldEntityId SourceEntity,
    WorldEntityId CallbackEntity,
    float RelativeDelaySeconds);

public readonly record struct WorldObservedCallbackInputEdgePayload(
    WorldInteractionSourceCategory SourceCategory,
    uint SchedulerParameterShortGuid,
    uint TargetInputShortGuid,
    uint CallbackConstructorFlags,
    uint CallbackPayloadSize,
    uint CallbackFamilyCallerRva,
    ulong ObservationId,
    WorldEntityId SourceEntity,
    float RelativeDelaySeconds);

/// <summary>
/// Bounded transport contract for a parameterless CATHODE edge already
/// emitted by the retail scheduler. v12.8 uses GenericWorld for the
/// active path, so Doors, lifts, buttons and other World-domain graph entities
/// do not require per-object network adapters.
/// </summary>
public static class WorldInteractionAdapterCodec
{
    public const uint ObservedInputEdgeOperation = 0x000C0601U;
    public const uint ObservedCallbackInputEdgeOperation = 0x000C0603U;
    public const uint ObservedCharacterCallbackInputEdgeOperation = 0x000C0604U;
    public const byte PayloadVersion = 1;
    public const byte ObservedInputEdgeKind = 1;
    public const byte ObservedCallbackInputEdgeKind = 5;
    public const byte ObservedCharacterCallbackInputEdgeKind = 6;
    public const int PayloadBytes = 48;
    public const int CallbackPayloadBytes = 56;
    public const int CharacterCallbackPayloadBytes = 80;
    public const uint CallbackReferenceFamilyCallerRva = 0x004BE280U;
    public const uint CallbackReferenceConstructorFlags = 0x80000000U;
    public const uint CharacterCallbackInputShortGuid = 0x6575CA9BU;
    public const uint CharacterCallbackConstructorFlags = 0x12000000U;
    public const uint CharacterCallbackExecutionVtableRva = 0x00DD08D8U;
    public const uint CharacterCallbackFamilyCallerRva = 0x004BE280U;
    public const float MaximumRelativeDelaySeconds = 600.0F;

    public static byte[] EncodeObservedInputEdge(WorldObservedInputEdgePayload value)
    {
        ValidateValue(value);
        var payload = new byte[PayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = ObservedInputEdgeKind;
        payload[2] = (byte)value.SourceCategory;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), value.SchedulerParameterShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), value.TargetInputShortGuid);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(16, 8), value.ObservationId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(24, 8), value.SourceEntity.High);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(32, 8), value.SourceEntity.Low);
        BinaryPrimitives.WriteUInt32LittleEndian(
            payload.AsSpan(40, 4),
            BitConverter.SingleToUInt32Bits(value.RelativeDelaySeconds));
        return payload;
    }

    public static WorldObservedInputEdgePayload DecodeObservedInputEdge(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != PayloadBytes
            || payload[0] != PayloadVersion
            || payload[1] != ObservedInputEdgeKind
            || payload[3] != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4)) != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(44, 4)) != 0)
            throw new ProtocolException("World CATHODE edge payload header is invalid.");

        var value = new WorldObservedInputEdgePayload(
            (WorldInteractionSourceCategory)payload[2],
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(16, 8)),
            new WorldEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(24, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(32, 8))),
            BitConverter.UInt32BitsToSingle(
                BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(40, 4))));
        ValidateValue(value);
        return value;
    }

    public static byte[] EncodeObservedCharacterCallbackInputEdge(
        WorldObservedCharacterCallbackInputEdgePayload value)
    {
        ValidateCharacterCallbackValue(value);
        var payload = new byte[CharacterCallbackPayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = ObservedCharacterCallbackInputEdgeKind;
        payload[2] = (byte)value.SourceCategory;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), value.SchedulerParameterShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), value.TargetInputShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12, 4), value.CallbackInputShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), value.CallbackConstructorFlags);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), value.CallbackExecutionVtableRva);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(24, 4), value.CallbackFamilyCallerRva);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(32, 8), value.ObservationId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(40, 8), value.SourceEntity.High);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(48, 8), value.SourceEntity.Low);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(56, 8), value.CallbackEntity.High);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(64, 8), value.CallbackEntity.Low);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(72, 4),
            BitConverter.SingleToUInt32Bits(value.RelativeDelaySeconds));
        return payload;
    }

    public static WorldObservedCharacterCallbackInputEdgePayload DecodeObservedCharacterCallbackInputEdge(
        ReadOnlySpan<byte> payload)
    {
        if (payload.Length != CharacterCallbackPayloadBytes
            || payload[0] != PayloadVersion
            || payload[1] != ObservedCharacterCallbackInputEdgeKind
            || payload[3] != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(28, 4)) != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(76, 4)) != 0)
            throw new ProtocolException("World CATHODE Character callback edge payload header is invalid.");
        var value = new WorldObservedCharacterCallbackInputEdgePayload(
            (WorldInteractionSourceCategory)payload[2],
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(20, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(24, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(32, 8)),
            new WorldEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(40, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(48, 8))),
            new WorldEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(56, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(64, 8))),
            BitConverter.UInt32BitsToSingle(BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(72, 4))));
        ValidateCharacterCallbackValue(value);
        return value;
    }

    public static byte[] EncodeObservedCallbackInputEdge(WorldObservedCallbackInputEdgePayload value)
    {
        ValidateCallbackValue(value);
        var payload = new byte[CallbackPayloadBytes];
        payload[0] = PayloadVersion;
        payload[1] = ObservedCallbackInputEdgeKind;
        payload[2] = (byte)value.SourceCategory;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), value.SchedulerParameterShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), value.TargetInputShortGuid);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(12, 4), value.CallbackConstructorFlags);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16, 4), value.CallbackPayloadSize);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(20, 4), value.CallbackFamilyCallerRva);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(24, 8), value.ObservationId);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(32, 8), value.SourceEntity.High);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(40, 8), value.SourceEntity.Low);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(48, 4),
            BitConverter.SingleToUInt32Bits(value.RelativeDelaySeconds));
        return payload;
    }

    public static WorldObservedCallbackInputEdgePayload DecodeObservedCallbackInputEdge(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != CallbackPayloadBytes
            || payload[0] != PayloadVersion
            || payload[1] != ObservedCallbackInputEdgeKind
            || payload[3] != 0
            || BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(52, 4)) != 0)
            throw new ProtocolException("World CATHODE constructed callback edge payload header is invalid.");

        var value = new WorldObservedCallbackInputEdgePayload(
            (WorldInteractionSourceCategory)payload[2],
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(8, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(12, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(16, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(20, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(24, 8)),
            new WorldEntityId(
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(32, 8)),
                BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(40, 8))),
            BitConverter.UInt32BitsToSingle(BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(48, 4))));
        ValidateCallbackValue(value);
        return value;
    }

    public static void ValidateIntent(WorldIntent value)
    {
        if (value.Operation == ObservedInputEdgeOperation)
        {
            if (value.RequestedKind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE world edges must use WorldEvent semantics.");
            _ = DecodeObservedInputEdge(value.Payload);
            return;
        }
        if (value.Operation == ObservedCharacterCallbackInputEdgeOperation)
        {
            if (value.RequestedKind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE Character callback edges must use WorldEvent semantics.");
            _ = DecodeObservedCharacterCallbackInputEdge(value.Payload);
            return;
        }
        if (value.Operation == ObservedCallbackInputEdgeOperation)
        {
            if (value.RequestedKind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE callback edges must use WorldEvent semantics.");
            _ = DecodeObservedCallbackInputEdge(value.Payload);
        }
    }

    public static void ValidateMutation(WorldMutation value)
    {
        if (value.Operation == ObservedInputEdgeOperation)
        {
            if (value.Kind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE world edges cannot use replayable State/Timeline semantics.");
            _ = DecodeObservedInputEdge(value.Payload);
            return;
        }
        if (value.Operation == ObservedCharacterCallbackInputEdgeOperation)
        {
            if (value.Kind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE Character callback edges cannot use replayable State/Timeline semantics.");
            _ = DecodeObservedCharacterCallbackInputEdge(value.Payload);
            return;
        }
        if (value.Operation == ObservedCallbackInputEdgeOperation)
        {
            if (value.Kind != WorldMutationKind.Event)
                throw new ProtocolException("Observed CATHODE callback edges cannot use replayable State/Timeline semantics.");
            _ = DecodeObservedCallbackInputEdge(value.Payload);
        }
    }

    private static void ValidateValue(WorldObservedInputEdgePayload value)
    {
        if (value.SourceCategory != WorldInteractionSourceCategory.GenericWorld
            || value.SchedulerParameterShortGuid == 0
            || value.TargetInputShortGuid == 0
            || value.ObservationId == 0
            || !value.SourceEntity.IsValid
            || !float.IsFinite(value.RelativeDelaySeconds)
            || value.RelativeDelaySeconds < 0
            || value.RelativeDelaySeconds > MaximumRelativeDelaySeconds)
            throw new ProtocolException("World CATHODE edge payload is invalid.");
    }
    private static void ValidateCharacterCallbackValue(
        WorldObservedCharacterCallbackInputEdgePayload value)
    {
        if (value.SourceCategory != WorldInteractionSourceCategory.GenericWorld
            || value.SchedulerParameterShortGuid == 0
            || value.TargetInputShortGuid == 0
            || value.CallbackInputShortGuid != CharacterCallbackInputShortGuid
            || value.CallbackConstructorFlags != CharacterCallbackConstructorFlags
            || value.CallbackExecutionVtableRva != CharacterCallbackExecutionVtableRva
            || value.CallbackFamilyCallerRva != CharacterCallbackFamilyCallerRva
            || value.ObservationId == 0
            || !value.SourceEntity.IsValid
            || !value.CallbackEntity.IsValid
            || !float.IsFinite(value.RelativeDelaySeconds)
            || value.RelativeDelaySeconds < 0
            || value.RelativeDelaySeconds > MaximumRelativeDelaySeconds)
            throw new ProtocolException("World CATHODE Character callback edge payload is invalid.");
    }

    private static void ValidateCallbackValue(WorldObservedCallbackInputEdgePayload value)
    {
        if (value.SourceCategory != WorldInteractionSourceCategory.GenericWorld
            || value.SchedulerParameterShortGuid == 0
            || value.TargetInputShortGuid == 0
            || value.CallbackConstructorFlags != CallbackReferenceConstructorFlags
            || value.CallbackPayloadSize != 0
            || value.CallbackFamilyCallerRva != CallbackReferenceFamilyCallerRva
            || value.ObservationId == 0
            || !value.SourceEntity.IsValid
            || !float.IsFinite(value.RelativeDelaySeconds)
            || value.RelativeDelaySeconds < 0
            || value.RelativeDelaySeconds > MaximumRelativeDelaySeconds)
            throw new ProtocolException("World CATHODE callback edge payload is invalid.");
    }

}
