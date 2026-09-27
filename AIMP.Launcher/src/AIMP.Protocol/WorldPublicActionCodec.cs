using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct WorldPublicContextFrame(
    WorldCathodeRuntimeKey RuntimeKey,
    uint ConstructorFlags,
    uint InputShortGuid,
    ulong Invocation,
    ushort PayloadOffset,
    ushort PayloadSize,
    uint DescriptorDelayBits);

public readonly record struct WorldPublicAction(
    uint PublicInput, ulong ObservationId, bool RetailRoot,
    WorldEntityId Target, double RelativeDelaySeconds, byte Flags,
    byte ContextKind = 0, byte ActorKind = 0,
    WorldEntityId ActorEntity = default,
    WorldPublicContextFrame[]? ContextFrames = null,
    byte[]? ContextPayload = null,
    byte RootKind = 1, ulong CausalSerial = 0, ulong SegmentSerial = 0,
    ushort RootOrdinal = 0, ushort RootCount = 1, ulong AuthorityTransaction = 0, byte InterfaceSelector = 0,
    uint SchedulerParameterShortGuid = 0);

public static class WorldPublicActionCodec
{
    public const byte NonRootVersion = 1;
    public const byte RetailRootVersion = 13;
    public const byte PreviousRetailRootVersion = 12;
    public const byte Kind = 7;
    public const byte ExternalInvocation = 1;
    public const byte DetachedCallback = 2;
    public const byte AwaitAuthority = 4;
    public const byte KnownFlags = ExternalInvocation | DetachedCallback | AwaitAuthority;
    public const int NonRootPayloadBytes = 16;
    public const int RootHeaderBytes = 96;
    public const int ContextFrameBytes = 32;
    public const int MaximumContextDepth = 16;
    public const int MaximumPayloadBytes = 4096;
    public const int MaximumContextPayloadBytes = MaximumPayloadBytes - RootHeaderBytes - ContextFrameBytes;
    public const byte AutonomousContext = 1;
    public const byte NativeContextChain = 2;
    public const ushort MaximumTransactionRoots = 32;
    public const byte CompositeInputRoot = 1;
    public const byte EmittedOutputRoot = 2;
    public const byte ScheduledInputRoot = 3;
    public const byte SchedulerEdgeRoot = 4;
    public const byte TypedMessageRoot = 5;
    public const byte CanonicalCompositeInputRoot = 6;
    public const byte DoorLockStateRoot = 7;
    public const uint DoorRequestLockInput = 0x5D7280CF;
    public const uint DoorRequestUnlockInput = 0x440FBB74;
    public const byte AuthorityActor = 0;
    public const byte SourcePlayerActor = 1;
    public const byte WorldEntityActor = 2;

    public static bool TryDecode(ReadOnlySpan<byte> payload, out uint publicInput,
        out ulong observationId, out bool retailRoot)
    {
        var valid = TryDecode(payload, out var action);
        publicInput = action.PublicInput;
        observationId = action.ObservationId;
        retailRoot = action.RetailRoot;
        return valid;
    }

    public static bool TryDecode(ReadOnlySpan<byte> payload, out WorldPublicAction action)
    {
        action = default;
        if (payload.Length < NonRootPayloadBytes || payload[1] != Kind)
            return false;

        var rootVersion = payload[0];
        var root = rootVersion == RetailRootVersion || rootVersion == PreviousRetailRootVersion;
        WorldEntityId target = default;
        WorldEntityId actor = default;
        byte contextKind = 0, actorKind = 0;
        WorldPublicContextFrame[] contextFrames = [];
        byte[] contextPayload = [];
        var delay = 0.0;
        byte rootKind = CompositeInputRoot, interfaceSelector = 0;
        ulong causalSerial = 0, segmentSerial = 0, authorityTransaction = 0;
        ushort rootOrdinal = 0, rootCount = 1;
        uint schedulerParameterShortGuid = 0;

        if (root)
        {
            if (payload.Length < RootHeaderBytes || payload.Length > MaximumPayloadBytes
                || (payload[3] & ~KnownFlags) != 0
                || (payload[3] & (ExternalInvocation | DetachedCallback)) == (ExternalInvocation | DetachedCallback))
                return false;

            contextKind = payload[40];
            actorKind = payload[41];
            var contextFrameCount = payload[42];
            interfaceSelector = payload[43];
            if (contextFrameCount > MaximumContextDepth)
                return false;
            var contextPayloadSize = BinaryPrimitives.ReadUInt16LittleEndian(payload[44..46]);
            var contextDescriptorBytes = BinaryPrimitives.ReadUInt16LittleEndian(payload[46..48]);
            rootKind = payload[2];
            var typedMessage = rootKind == TypedMessageRoot;
            var canonicalComposite = rootKind == CanonicalCompositeInputRoot;
            var doorLockState = rootKind == DoorLockStateRoot;
            if (((canonicalComposite || doorLockState) && rootVersion != RetailRootVersion) ||
                (!typedMessage && contextPayloadSize != 0) ||
                contextPayloadSize > MaximumContextPayloadBytes) return false;
            schedulerParameterShortGuid = BinaryPrimitives.ReadUInt32LittleEndian(payload[84..88]);
            causalSerial = BinaryPrimitives.ReadUInt64LittleEndian(payload[64..72]);
            segmentSerial = BinaryPrimitives.ReadUInt64LittleEndian(payload[72..80]);
            rootOrdinal = BinaryPrimitives.ReadUInt16LittleEndian(payload[80..82]);
            rootCount = BinaryPrimitives.ReadUInt16LittleEndian(payload[82..84]);
            authorityTransaction = BinaryPrimitives.ReadUInt64LittleEndian(payload[88..96]);
            if (rootKind is not (CompositeInputRoot or EmittedOutputRoot or ScheduledInputRoot or SchedulerEdgeRoot or TypedMessageRoot or CanonicalCompositeInputRoot or DoorLockStateRoot) ||
                (rootKind != EmittedOutputRoot ? interfaceSelector != 0 : interfaceSelector is not (1 or 2)) ||
                causalSerial == 0 || segmentSerial == 0 || rootCount == 0 ||
                rootCount > MaximumTransactionRoots || rootOrdinal >= rootCount ||
                (rootKind == SchedulerEdgeRoot ? schedulerParameterShortGuid == 0 : schedulerParameterShortGuid != 0) ||
                contextDescriptorBytes != contextFrameCount * ContextFrameBytes)
                return false;

            var expectedBytes = RootHeaderBytes + contextDescriptorBytes + contextPayloadSize;
            if (expectedBytes != payload.Length || contextPayloadSize > MaximumPayloadBytes - RootHeaderBytes - contextDescriptorBytes)
                return false;

            target = new(BinaryPrimitives.ReadUInt64LittleEndian(payload[16..24]),
                BinaryPrimitives.ReadUInt64LittleEndian(payload[24..32]));
            delay = BinaryPrimitives.ReadDoubleLittleEndian(payload[32..40]);
            if (!target.IsValid || !double.IsFinite(delay) || delay < 0 || delay > 600)
                return false;

            actor = new(BinaryPrimitives.ReadUInt64LittleEndian(payload[48..56]),
                BinaryPrimitives.ReadUInt64LittleEndian(payload[56..64]));
            if (actorKind == WorldEntityActor ? !actor.IsValid
                : actorKind > SourcePlayerActor || actor != default)
                return false;

            if (contextKind == AutonomousContext)
            {
                if (contextFrameCount != 0 || contextDescriptorBytes != 0
                    || (!typedMessage && contextPayloadSize != 0)
                    || (payload[3] & DetachedCallback) != 0)
                    return false;
                if (typedMessage && contextPayloadSize != 0)
                    contextPayload = payload.Slice(RootHeaderBytes, contextPayloadSize).ToArray();
            }
            else if (contextKind == NativeContextChain)
            {
                if (contextFrameCount == 0 || (payload[3] & ~AwaitAuthority) != DetachedCallback)
                    return false;

                contextFrames = new WorldPublicContextFrame[contextFrameCount];
                var canonicalPayloadOffset = 0;
                for (var i = 0; i < contextFrameCount; ++i)
                {
                    var offset = RootHeaderBytes + i * ContextFrameBytes;
                    var frameBytes = payload.Slice(offset, ContextFrameBytes);
                    var key = new WorldCathodeRuntimeKey(
                        BinaryPrimitives.ReadUInt32LittleEndian(frameBytes[0..4]),
                        BinaryPrimitives.ReadUInt32LittleEndian(frameBytes[4..8]));
                    var constructorFlags = BinaryPrimitives.ReadUInt32LittleEndian(frameBytes[8..12]);
                    var inputShortGuid = BinaryPrimitives.ReadUInt32LittleEndian(frameBytes[12..16]);
                    var invocation = BinaryPrimitives.ReadUInt64LittleEndian(frameBytes[16..24]);
                    var payloadOffset = BinaryPrimitives.ReadUInt16LittleEndian(frameBytes[24..26]);
                    var payloadSize = BinaryPrimitives.ReadUInt16LittleEndian(frameBytes[26..28]);
                    var descriptorDelayBits = BinaryPrimitives.ReadUInt32LittleEndian(frameBytes[28..32]);
                    var descriptorDelay = BitConverter.UInt32BitsToSingle(descriptorDelayBits);
                    if (!float.IsFinite(descriptorDelay) || descriptorDelay < 0 ||
                        !key.IsValid || inputShortGuid == 0 || invocation == 0
                        || payloadOffset != canonicalPayloadOffset
                        || payloadOffset + payloadSize > contextPayloadSize)
                        return false;
                    canonicalPayloadOffset += payloadSize;
                    contextFrames[i] = new(key, constructorFlags, inputShortGuid, invocation,
                        payloadOffset, payloadSize, descriptorDelayBits);
                }
                if (canonicalPayloadOffset != contextPayloadSize)
                    return false;
                if (contextPayloadSize != 0)
                    contextPayload = payload.Slice(RootHeaderBytes + contextDescriptorBytes,
                        contextPayloadSize).ToArray();
            }
            else return false;
        }
        else if (payload[0] != NonRootVersion || payload.Length != NonRootPayloadBytes || payload[2] != 0 || payload[3] != 0)
            return false;

        var input = BinaryPrimitives.ReadUInt32LittleEndian(payload[4..8]);
        var observation = BinaryPrimitives.ReadUInt64LittleEndian(payload[8..16]);
        if (observation == 0 || (input == 0 && (!root || rootKind != TypedMessageRoot))) return false;
        if (root && rootKind == ScheduledInputRoot &&
            (contextKind != NativeContextChain || contextFrames.Length == 0 ||
                contextFrames[0].InputShortGuid != input || delay != 0.0)) return false;
        if (root && rootKind == SchedulerEdgeRoot &&
            (contextKind != AutonomousContext || contextFrames.Length != 0 ||
                contextPayload.Length != 0 || schedulerParameterShortGuid == 0)) return false;
        if (root && rootKind == TypedMessageRoot &&
            (contextKind != AutonomousContext || contextFrames.Length != 0 ||
                schedulerParameterShortGuid != 0)) return false;
        if (root && rootKind == CanonicalCompositeInputRoot &&
            (rootVersion != RetailRootVersion || contextKind != AutonomousContext ||
                contextFrames.Length != 0 || contextPayload.Length != 0 ||
                schedulerParameterShortGuid != 0 || delay != 0.0 ||
                payload[3] != 0)) return false;
        if (root && rootKind == DoorLockStateRoot &&
            (rootVersion != RetailRootVersion || contextKind != AutonomousContext ||
                contextFrames.Length != 0 || contextPayload.Length != 0 ||
                schedulerParameterShortGuid != 0 || delay != 0.0 ||
                payload[3] != 0 ||
                input is not (DoorRequestLockInput or DoorRequestUnlockInput))) return false;
        action = new(input, observation, root, target, delay, payload[3],
            contextKind, actorKind, actor, contextFrames, contextPayload,
            rootKind, causalSerial, segmentSerial, rootOrdinal, rootCount, authorityTransaction, interfaceSelector, schedulerParameterShortGuid);
        return true;
    }
}
