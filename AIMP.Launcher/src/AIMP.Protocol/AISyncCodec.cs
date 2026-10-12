using System.Buffers.Binary;

namespace AIMP.Protocol;

// Generic high-level NPC authority, not a serialized Behaviour Tree, animation
// stream or WorldMutation. Fixed-size payloads carry no process addresses,
// ObjectRef pointers, clips, audio events or mutable BT state. Retail/CATHODE
// regenerate presentation locally; resolved paired health/outcomes come from Host.
public static class AISyncCodec
{
    public const uint StateVersion = 11;
    public const int NativeStateBytes = 288;
    public const int NativeImpactBytes = 64;
    public const int ConfigurationBytes = 552;
    public const int NetworkStateBytes = 16 + NativeStateBytes;
    public const int NetworkImpactBytes = 16 + NativeImpactBytes;
    public const int MaximumActors = 32;
    private const uint PoseValid = 1U << 0;
    private const uint Moving = 1U << 1;
    private const uint ActionValid = 1U << 2;
    private const uint InteractionValid = 1U << 3;
    private const uint RoleTaskValid = 1U << 4;
    private const uint ExecutionValid = 1U << 5;
    private const uint KnownStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid | ExecutionValid;
    private const uint ExecutionTransition = 1U << 0;
    private const uint ExecutionCertified = 1U << 1;
    private const uint ExecutionPaired = 1U << 2;
    private const uint KnownExecutionFlags = ExecutionTransition | ExecutionCertified | ExecutionPaired;
    private const uint KnownLifecycleFlags = 63U; // includes Character backstage (bit 5)
    private const uint InteractionPairValid = 1U << 2;
    private const uint KnownInteractionFlags = 63U; // includes retained source cycle (bit 5)

    public static ulong SessionCookie(Guid session)
    {
        Span<byte> bytes = stackalloc byte[16];
        session.TryWriteBytes(bytes);
        var cookie = BinaryPrimitives.ReadUInt64LittleEndian(bytes);
        return cookie == 0 ? 1UL : cookie;
    }

    public static void ValidateNative(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeStateBytes || U32(bytes, 0) != StateVersion || U32(bytes, 4) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == 0 || U32(bytes, 16) == 0
            || (U32(bytes, 20) == 0 && U32(bytes, 24) == 0) || U32(bytes, 32) == 0
            || U32(bytes, 36) == 0
            || !AllowedCharacterClass(U32(bytes, 28)))
            throw new ProtocolException("Invalid AISync authority snapshot v11.");
        ValidateAuthorityState(bytes);
    }

    private static void ValidateAuthorityState(ReadOnlySpan<byte> bytes)
    {
        var navigationFlags = U32(bytes, 260);
        var stoppingDistance = Float(bytes, 276);
        if ((navigationFlags & ~1U) != 0 || U32(bytes, 280) > 3
            || !float.IsFinite(stoppingDistance) || stoppingDistance is < -1F or > 1000F
            || navigationFlags != 0 && (U32(bytes, 256) == 0 || U32(bytes, 284) == 0))
            throw new ProtocolException("Invalid AISync native navigation goal.");
        for (var offset = 264; offset <= 272; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync native navigation coordinates.");
        var flags = U32(bytes, 40);
        var locomotionIntensity = Float(bytes, 72);
        var locomotionRevision = U32(bytes, 76);
        if ((flags & ~KnownStateFlags) != 0 || U32(bytes, 68) > 3
            || !float.IsFinite(locomotionIntensity) || locomotionIntensity is < 0F or > 64F
            || ((flags & Moving) != 0 && locomotionRevision == 0)
            || U32(bytes, 92) > 10 || U32(bytes, 96) > 64
            || (flags & Moving) != 0 && (flags & PoseValid) == 0)
            throw new ProtocolException("Invalid AISync Character state.");
        ValidatePose(bytes);
        if ((flags & ActionValid) != 0
            && (U32(bytes, 80) == 0 || U32(bytes, 88) == 0 || U32(bytes, 96) == 0))
            throw new ProtocolException("Invalid AISync action identity.");
        var interactionVariant = I32(bytes, 132);
        var interactionFlags = U32(bytes, 112);
        var targetPeer = BinaryPrimitives.ReadUInt64LittleEndian(bytes[172..]);
        var transitionVariant = I32(bytes, 180);
        if ((interactionFlags & ~KnownInteractionFlags) != 0
            || U32(bytes, 116) > 2 || (U32(bytes, 120) & ~0xFFFFU) != 0
            || interactionVariant is < -1 or > 0xFFFF || transitionVariant is < -1 or > 0xFFFF)
            throw new ProtocolException("Invalid AISync Character interaction.");
        for (var offset = 136; offset <= 144; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync interaction target transform.");
        if ((flags & InteractionValid) != 0
            && (U32(bytes, 100) == 0 || U32(bytes, 104) == 0 && U32(bytes, 108) == 0
                || U32(bytes, 184) == 0 || (interactionFlags & InteractionPairValid) == 0
                || ((interactionFlags & 2U) != 0) != (targetPeer != 0)))
            throw new ProtocolException("Invalid AISync interaction identity.");
        if ((flags & InteractionValid) == 0 && U32(bytes, 116) != 0)
            throw new ProtocolException("AISync transition requires an interaction event.");
        if ((interactionFlags & 32U) != 0 && ((flags & InteractionValid) == 0
            || U32(bytes, 116) == 0 || (interactionFlags & InteractionPairValid) == 0))
            throw new ProtocolException("AISync retained source requires an identified paired transition.");
        if (U32(bytes, 116) != 0 && U32(bytes, 188) == 0)
            throw new ProtocolException("AISync transition requires its own revision.");
        if ((flags & RoleTaskValid) != 0
            && (U32(bytes, 148) == 0 || U32(bytes, 152) == 0))
            throw new ProtocolException("Invalid AISync role task identity.");
        if ((U32(bytes, 168) & ~KnownLifecycleFlags) != 0
            || U32(bytes, 168) != 0 && U32(bytes, 164) == 0)
            throw new ProtocolException("Invalid AISync lifecycle flags.");
        var outcomeRevision = U32(bytes, 192);
        var outcomeFlags = U32(bytes, 196);
        var kind = U32(bytes, 204);
        var hpBefore = U32(bytes, 208);
        var hpPrepared = U32(bytes, 212);
        var hpReaction = U32(bytes, 216);
        var healthReaction = U32(bytes, 220);
        if ((outcomeFlags & ~63U) != 0 || U32(bytes, 200) > 0xFFFF
            || kind is not (0 or 1 or 3) || hpBefore > 0xFFFF || hpPrepared > 0xFFFF
            || hpReaction > 0xFFFF || healthReaction > 4
            || (outcomeRevision == 0) != (outcomeFlags == 0)
            || (outcomeFlags & 32U) != 0 && (outcomeFlags & 2U) == 0
            || ((outcomeFlags & 16U) != 0) != (outcomeRevision != 0 && hpReaction == 0 && healthReaction == 4)
            || outcomeRevision != 0 && ((flags & InteractionValid) == 0 || targetPeer == 0
                || U32(bytes, 116) == 0 || (interactionFlags & 16U) == 0
                || (outcomeFlags & 1U) == 0 || hpBefore == 0 || hpPrepared > hpBefore))
            throw new ProtocolException("Invalid AISync native attack outcome.");
        for (var offset = 224; offset <= 236; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync native outcome geometry.");
        var executionRevision = U32(bytes, 240);
        var executionType = U32(bytes, 244);
        var executionState = U32(bytes, 248);
        var executionFlags = U32(bytes, 252);
        if (executionState > 0xFF || (executionFlags & ~KnownExecutionFlags) != 0
            || executionFlags != 0 && (flags & ExecutionValid) == 0
            || (executionFlags & ExecutionCertified) != 0 && (executionFlags & ExecutionTransition) == 0
            || (flags & ExecutionValid) != 0 && (executionRevision == 0 || executionType == 0))
            throw new ProtocolException("Invalid AISync execution-cycle authority.");
    }

    private static void ValidatePose(ReadOnlySpan<byte> bytes)
    {
        for (var offset = 44; offset <= 52; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync Character transform.");
        var yaw = Float(bytes, 56);
        var velocityX = Float(bytes, 60);
        var velocityZ = Float(bytes, 64);
        if (!float.IsFinite(yaw) || Math.Abs(yaw) > 3.3F
            || !float.IsFinite(velocityX) || Math.Abs(velocityX) > 20F
            || !float.IsFinite(velocityZ) || Math.Abs(velocityZ) > 20F)
            throw new ProtocolException("Invalid AISync Character motion.");
    }

    public static bool BelongsTo(ReadOnlySpan<byte> bytes, Guid session, int worldGeneration) =>
        bytes.Length == NativeStateBytes && session != Guid.Empty && worldGeneration > 0
        && BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == SessionCookie(session)
        && U32(bytes, 16) == (uint)worldGeneration;

    public static ulong ActorKey(ReadOnlySpan<byte> bytes) =>
        ((ulong)U32(bytes, 20) << 32) | U32(bytes, 24);

    // Irreversible native results cannot be merged like motion snapshots.
    public static (uint Birth, uint Pair, uint Revision) OutcomeKey(ReadOnlySpan<byte> bytes) =>
        (U32(bytes, 32), U32(bytes, 184), U32(bytes, 192));

    public static byte[] EncodeNetwork(Guid session, ReadOnlySpan<byte> native)
    {
        ValidateNative(native);
        if (session == Guid.Empty || BinaryPrimitives.ReadUInt64LittleEndian(native[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync session identity mismatch.");
        var bytes = new byte[NetworkStateBytes];
        session.TryWriteBytes(bytes.AsSpan(0, 16));
        native.CopyTo(bytes.AsSpan(16));
        return bytes;
    }

    public static ReadOnlyMemory<byte> DecodeNetwork(ReadOnlyMemory<byte> bytes, Guid session)
    {
        if (bytes.Length != NetworkStateBytes || new Guid(bytes.Span[..16]) != session)
            throw new ProtocolException("AISync network session mismatch.");
        var native = bytes[16..];
        ValidateNative(native.Span);
        if (BinaryPrimitives.ReadUInt64LittleEndian(native.Span[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync cookie mismatch.");
        return native;
    }

    public static void ValidateNativeImpact(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != NativeImpactBytes || U32(bytes, 0) != 1 || U32(bytes, 4) == 0
            || BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == 0 || U32(bytes, 16) == 0
            || U32(bytes, 20) == 0 || BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..]) == 0
            || (U32(bytes, 32) == 0 && U32(bytes, 36) == 0) || !AllowedCharacterClass(U32(bytes, 40))
            || U32(bytes, 44) == 0 || U32(bytes, 48) is 0 or > 0xFFFF
            || U32(bytes, 56) is not (1 or 2) || U32(bytes, 60) != 0)
            throw new ProtocolException("Invalid AISync client impact.");
    }

    public static bool ImpactBelongsTo(ReadOnlySpan<byte> bytes, Guid session, int worldGeneration) =>
        bytes.Length == NativeImpactBytes && session != Guid.Empty && worldGeneration > 0
        && BinaryPrimitives.ReadUInt64LittleEndian(bytes[8..]) == SessionCookie(session)
        && U32(bytes, 16) == (uint)worldGeneration;

    public static ulong ImpactSourcePeer(ReadOnlySpan<byte> bytes) =>
        BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..]);

    public static uint ImpactSequence(ReadOnlySpan<byte> bytes) => U32(bytes, 20);

    public static byte[] EncodeImpactNetwork(Guid session, ReadOnlySpan<byte> native)
    {
        ValidateNativeImpact(native);
        if (session == Guid.Empty || BinaryPrimitives.ReadUInt64LittleEndian(native[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync impact session identity mismatch.");
        var bytes = new byte[NetworkImpactBytes];
        session.TryWriteBytes(bytes.AsSpan(0, 16));
        native.CopyTo(bytes.AsSpan(16));
        return bytes;
    }

    public static ReadOnlyMemory<byte> DecodeImpactNetwork(ReadOnlyMemory<byte> bytes, Guid session)
    {
        if (bytes.Length != NetworkImpactBytes || new Guid(bytes.Span[..16]) != session)
            throw new ProtocolException("AISync impact network session mismatch.");
        var native = bytes[16..];
        ValidateNativeImpact(native.Span);
        if (BinaryPrimitives.ReadUInt64LittleEndian(native.Span[8..]) != SessionCookie(session))
            throw new ProtocolException("AISync impact cookie mismatch.");
        return native;
    }

    public static byte[] Configuration(uint role, int nativeGeneration, int worldGeneration, Guid session,
        ulong localPeer, IReadOnlyDictionary<ulong, uint> participants)
    {
        if (role > 3 || nativeGeneration < 0 || worldGeneration < 0)
            throw new ArgumentOutOfRangeException(nameof(role));
        if (role != 0 && (session == Guid.Empty || localPeer == 0 ||
            (role != 3 && (nativeGeneration == 0 || worldGeneration == 0))))
            throw new ProtocolException("AISync configuration needs a live generation.");
        if (participants.Count > MaximumActors)
            throw new ProtocolException("Too many AISync participant bindings.");
        var bytes = new byte[ConfigurationBytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 2);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(4), role);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), (uint)nativeGeneration);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), (uint)worldGeneration);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16), role == 0 ? 0 : SessionCookie(session));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(24), localPeer);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(32), (uint)participants.Count);
        var offset = 40;
        var characters = new HashSet<uint>();
        foreach (var binding in participants.OrderBy(value => value.Key))
        {
            if (binding.Key == 0 || binding.Key == localPeer || binding.Value < 0x10000
                || !characters.Add(binding.Value))
                throw new ProtocolException("Invalid local AISync participant binding.");
            BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(offset), binding.Key);
            BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset + 8), binding.Value);
            offset += 16;
        }
        return bytes;
    }

    private static bool AllowedCharacterClass(uint value) => value is 1 or 3 or 4 or 6 or 9;

    private static uint U32(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static int I32(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadInt32LittleEndian(bytes[offset..]);
    private static float Float(ReadOnlySpan<byte> bytes, int offset) =>
        BitConverter.Int32BitsToSingle((int)U32(bytes, offset));
}
