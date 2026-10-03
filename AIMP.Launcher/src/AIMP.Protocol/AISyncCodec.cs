using System.Buffers.Binary;

namespace AIMP.Protocol;

// Generic high-level NPC authority, not a serialized Behaviour Tree, animation
// stream or WorldMutation. Fixed-size payloads carry no process addresses,
// ObjectRef pointers, clips, audio events or mutable BT state. Retail/CATHODE
// regenerate downstream presentation and consequences on each process.
public static class AISyncCodec
{
    public const uint StateVersion = 6;
    public const int NativeStateBytes = 184;
    public const int ConfigurationBytes = 552;
    public const int NetworkStateBytes = 16 + NativeStateBytes;
    public const int MaximumActors = 32;
    private const uint PoseValid = 1U << 0;
    private const uint Moving = 1U << 1;
    private const uint ActionValid = 1U << 2;
    private const uint InteractionValid = 1U << 3;
    private const uint RoleTaskValid = 1U << 4;
    private const uint KnownStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid;
    private const uint KnownLifecycleFlags = 31U;
    private const uint InteractionPairValid = 1U << 2;
    private const uint KnownInteractionFlags = 15U;

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
            throw new ProtocolException("Invalid AISync authority snapshot v6.");
        ValidateAuthorityState(bytes);
    }

    private static void ValidateAuthorityState(ReadOnlySpan<byte> bytes)
    {
        var flags = U32(bytes, 40);
        if ((flags & ~KnownStateFlags) != 0 || U32(bytes, 68) > 3
            || U32(bytes, 84) > 10 || U32(bytes, 88) > 64
            || (flags & Moving) != 0 && (flags & PoseValid) == 0)
            throw new ProtocolException("Invalid AISync Character state.");
        ValidatePose(bytes);
        if ((flags & ActionValid) != 0
            && (U32(bytes, 72) == 0 || U32(bytes, 80) == 0 || U32(bytes, 88) == 0))
            throw new ProtocolException("Invalid AISync action identity.");
        var interactionVariant = I32(bytes, 124);
        var interactionFlags = U32(bytes, 104);
        var targetPeer = BinaryPrimitives.ReadUInt64LittleEndian(bytes[164..]);
        var transitionVariant = I32(bytes, 172);
        if ((interactionFlags & ~KnownInteractionFlags) != 0
            || U32(bytes, 108) > 2 || (U32(bytes, 112) & ~0xFFFFU) != 0
            || interactionVariant is < -1 or > 0xFFFF || transitionVariant is < -1 or > 0xFFFF)
            throw new ProtocolException("Invalid AISync Character interaction.");
        for (var offset = 128; offset <= 136; offset += 4)
            if (!float.IsFinite(Float(bytes, offset)) || Math.Abs(Float(bytes, offset)) > 100000)
                throw new ProtocolException("Invalid AISync interaction target transform.");
        if ((flags & InteractionValid) != 0
            && (U32(bytes, 92) == 0 || U32(bytes, 96) == 0 && U32(bytes, 100) == 0
                || U32(bytes, 176) == 0 || (interactionFlags & InteractionPairValid) == 0
                || ((interactionFlags & 2U) != 0) != (targetPeer != 0)))
            throw new ProtocolException("Invalid AISync interaction identity.");
        if ((flags & InteractionValid) == 0 && U32(bytes, 108) != 0)
            throw new ProtocolException("AISync transition requires an interaction event.");
        if (U32(bytes, 108) != 0 && U32(bytes, 180) == 0)
            throw new ProtocolException("AISync transition requires its own revision.");
        if ((flags & RoleTaskValid) != 0
            && (U32(bytes, 140) == 0 || U32(bytes, 144) == 0))
            throw new ProtocolException("Invalid AISync role task identity.");
        if ((U32(bytes, 160) & ~KnownLifecycleFlags) != 0
            || U32(bytes, 160) != 0 && U32(bytes, 156) == 0)
            throw new ProtocolException("Invalid AISync lifecycle flags.");
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
