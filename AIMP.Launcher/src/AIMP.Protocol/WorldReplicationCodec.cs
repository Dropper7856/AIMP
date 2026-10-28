using System.Buffers.Binary;
using System.Security.Cryptography;

namespace AIMP.Protocol;

public static class WorldReplicationCodec
{
    private const uint Magic = 0x31525741U; // ASCII AWR1
    private const byte IntentType = 1;
    private const byte MutationType = 2;
    private const byte ResultType = 3;
    private const byte ContextType = 4;
    private const byte NetworkIntentType = 5;
    private const byte NetworkMutationType = 6;
    private const byte NetworkAckType = 7;
    private const int PrefixBytes = 8;

    public static byte[] EncodeIntent(WorldIntent value)
    {
        WorldReplicationValidation.Validate(value);
        var result = new byte[76 + value.Payload.Length];
        WritePrefix(result, IntentType);
        var offset = PrefixBytes;
        WriteManifest(result, ref offset, value.Manifest);
        WriteEntity(result, ref offset, value.Entity);
        WriteUInt32(result, ref offset, value.LevelGeneration);
        WriteUInt64(result, ref offset, value.SourcePeer);
        WriteUInt64(result, ref offset, value.ClientSequence);
        result[offset++] = (byte)value.RequestedKind;
        result[offset++] = 0;
        WriteUInt32(result, ref offset, value.Operation);
        WriteUInt16(result, ref offset, checked((ushort)value.Payload.Length));
        value.Payload.AsSpan().CopyTo(result.AsSpan(offset));
        return result;
    }

    public static WorldIntent DecodeIntent(ReadOnlySpan<byte> payload)
    {
        var offset = ReadPrefix(payload, IntentType);
        var manifest = ReadManifest(payload, ref offset);
        var entity = ReadEntity(payload, ref offset);
        var generation = ReadUInt32(payload, ref offset);
        var sourcePeer = ReadUInt64(payload, ref offset);
        var clientSequence = ReadUInt64(payload, ref offset);
        Ensure(payload, offset, 2);
        var requestedKind = (WorldMutationKind)payload[offset++];
        if (payload[offset++] != 0) throw new ProtocolException("World intent reserved byte is invalid.");
        var operation = ReadUInt32(payload, ref offset);
        var bytes = ReadPayload(payload, ref offset);
        EnsureConsumed(payload, offset);
        var value = new WorldIntent(manifest, entity, generation, sourcePeer,
            clientSequence, requestedKind, operation, bytes);
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeMutation(WorldMutation value)
    {
        WorldReplicationValidation.Validate(value);
        var result = new byte[96 + value.Payload.Length];
        WritePrefix(result, MutationType);
        var offset = PrefixBytes;
        WriteManifest(result, ref offset, value.Manifest);
        WriteEntity(result, ref offset, value.Entity);
        WriteUInt32(result, ref offset, value.LevelGeneration);
        result[offset++] = (byte)value.Kind;
        result[offset++] = (byte)value.Flags;
        WriteUInt16(result, ref offset, 0);
        WriteUInt64(result, ref offset, value.Sequence);
        WriteUInt64(result, ref offset, value.Revision);
        WriteUInt64(result, ref offset, value.TimelineTick);
        WriteUInt64(result, ref offset, value.SourcePeer);
        WriteUInt32(result, ref offset, value.Operation);
        WriteUInt16(result, ref offset, checked((ushort)value.Payload.Length));
        WriteUInt16(result, ref offset, 0);
        value.Payload.AsSpan().CopyTo(result.AsSpan(offset));
        return result;
    }

    public static WorldMutation DecodeMutation(ReadOnlySpan<byte> payload)
    {
        var offset = ReadPrefix(payload, MutationType);
        var manifest = ReadManifest(payload, ref offset);
        var entity = ReadEntity(payload, ref offset);
        var generation = ReadUInt32(payload, ref offset);
        Ensure(payload, offset, 2);
        var kind = (WorldMutationKind)payload[offset++];
        var flags = (WorldMutationFlags)payload[offset++];
        if (ReadUInt16(payload, ref offset) != 0) throw new ProtocolException("World mutation reserved field is invalid.");
        var sequence = ReadUInt64(payload, ref offset);
        var revision = ReadUInt64(payload, ref offset);
        var timelineTick = ReadUInt64(payload, ref offset);
        var sourcePeer = ReadUInt64(payload, ref offset);
        var operation = ReadUInt32(payload, ref offset);
        var length = ReadUInt16(payload, ref offset);
        if (ReadUInt16(payload, ref offset) != 0) throw new ProtocolException("World mutation reserved field is invalid.");
        var bytes = ReadPayload(payload, ref offset, length);
        EnsureConsumed(payload, offset);
        var value = new WorldMutation(manifest, kind, flags, entity, generation,
            sequence, revision, timelineTick, sourcePeer, operation, bytes);
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeResult(WorldMutationResult value)
    {
        WorldReplicationValidation.Validate(value);
        var result = new byte[20];
        WritePrefix(result, ResultType);
        var offset = PrefixBytes;
        WriteUInt64(result, ref offset, value.Sequence);
        result[offset++] = (byte)value.Code;
        result[offset++] = value.EntityResolved ? (byte)1 : (byte)0;
        result[offset++] = value.Dispatched ? (byte)1 : (byte)0;
        result[offset] = 0;
        return result;
    }

    public static WorldMutationResult DecodeResult(ReadOnlySpan<byte> payload)
    {
        var offset = ReadPrefix(payload, ResultType);
        var sequence = ReadUInt64(payload, ref offset);
        Ensure(payload, offset, 4);
        var code = (WorldMutationResultCode)payload[offset++];
        var resolved = ReadBoolean(payload[offset++], "entityResolved");
        var dispatched = ReadBoolean(payload[offset++], "dispatched");
        if (payload[offset++] != 0) throw new ProtocolException("World result reserved byte is invalid.");
        EnsureConsumed(payload, offset);
        var value = new WorldMutationResult(sequence, code, resolved, dispatched);
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeNetworkContext(WorldNetworkContext value)
    {
        WorldReplicationValidation.Validate(value);
        var result = new byte[93];
        WritePrefix(result, ContextType);
        var offset = PrefixBytes;
        WriteGuid(result, ref offset, value.SessionId);
        WriteUInt64(result, ref offset, value.PeerId);
        WriteInt32(result, ref offset, value.WorldGeneration);
        WriteManifest(result, ref offset, value.Manifest);
        Convert.FromHexString(value.ManifestSha256).AsSpan().CopyTo(result.AsSpan(offset, 32));
        offset += 32;
        result[offset] = value.Ready ? (byte)1 : (byte)0;
        return result;
    }

    public static WorldNetworkContext DecodeNetworkContext(ReadOnlySpan<byte> payload)
    {
        var offset = ReadPrefix(payload, ContextType);
        var sessionId = ReadGuid(payload, ref offset);
        var peerId = ReadUInt64(payload, ref offset);
        var generation = ReadInt32(payload, ref offset);
        var manifest = ReadManifest(payload, ref offset);
        Ensure(payload, offset, 33);
        var sha = Convert.ToHexString(payload.Slice(offset, 32));
        offset += 32;
        var ready = ReadBoolean(payload[offset++], "ready");
        EnsureConsumed(payload, offset);
        var value = new WorldNetworkContext(sessionId, peerId, generation, manifest, sha, ready);
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeNetworkIntent(WorldNetworkIntent value)
    {
        WorldReplicationValidation.Validate(value);
        return EncodeNetworkEnvelope(NetworkIntentType, value.SessionId,
            value.WorldGeneration, EncodeIntent(value.Intent));
    }

    public static WorldNetworkIntent DecodeNetworkIntent(ReadOnlySpan<byte> payload)
    {
        DecodeNetworkEnvelope(payload, NetworkIntentType, out var session, out var generation, out var innerOffset, out var innerLength);
        var value = new WorldNetworkIntent(session, generation, DecodeIntent(payload.Slice(innerOffset, innerLength)));
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeNetworkMutation(WorldNetworkMutation value)
    {
        WorldReplicationValidation.Validate(value);
        return EncodeNetworkEnvelope(NetworkMutationType, value.SessionId,
            value.WorldGeneration, EncodeMutation(value.Mutation));
    }

    public static WorldNetworkMutation DecodeNetworkMutation(ReadOnlySpan<byte> payload)
    {
        DecodeNetworkEnvelope(payload, NetworkMutationType, out var session, out var generation, out var innerOffset, out var innerLength);
        var value = new WorldNetworkMutation(session, generation, DecodeMutation(payload.Slice(innerOffset, innerLength)));
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static byte[] EncodeNetworkAck(WorldNetworkMutationAck value)
    {
        WorldReplicationValidation.Validate(value);
        var inner = EncodeResult(value.Result);
        var result = new byte[PrefixBytes + 16 + 8 + 4 + 2 + inner.Length];
        WritePrefix(result, NetworkAckType);
        var offset = PrefixBytes;
        WriteGuid(result, ref offset, value.SessionId);
        WriteUInt64(result, ref offset, value.PeerId);
        WriteInt32(result, ref offset, value.WorldGeneration);
        WriteUInt16(result, ref offset, checked((ushort)inner.Length));
        inner.AsSpan().CopyTo(result.AsSpan(offset));
        return result;
    }

    public static WorldNetworkMutationAck DecodeNetworkAck(ReadOnlySpan<byte> payload)
    {
        var offset = ReadPrefix(payload, NetworkAckType);
        var sessionId = ReadGuid(payload, ref offset);
        var peerId = ReadUInt64(payload, ref offset);
        var generation = ReadInt32(payload, ref offset);
        var length = ReadUInt16(payload, ref offset);
        Ensure(payload, offset, length);
        var result = DecodeResult(payload.Slice(offset, length));
        offset += length;
        EnsureConsumed(payload, offset);
        var value = new WorldNetworkMutationAck(sessionId, peerId, generation, result);
        WorldReplicationValidation.Validate(value);
        return value;
    }

    public static string ComputeManifestSha256(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    public static string ComputeManifestCompatibilitySha256(WorldManifest manifest) =>
        WorldManifestBinaryCodec.ComputeRuntimeCompatibilitySha256(manifest);

    private static byte[] EncodeNetworkEnvelope(byte type, Guid sessionId, int generation, byte[] inner)
    {
        var result = new byte[PrefixBytes + 16 + 4 + 2 + inner.Length];
        WritePrefix(result, type);
        var offset = PrefixBytes;
        WriteGuid(result, ref offset, sessionId);
        WriteInt32(result, ref offset, generation);
        WriteUInt16(result, ref offset, checked((ushort)inner.Length));
        inner.AsSpan().CopyTo(result.AsSpan(offset));
        return result;
    }

    private static void DecodeNetworkEnvelope(
        ReadOnlySpan<byte> payload,
        byte expectedType,
        out Guid sessionId,
        out int generation,
        out int innerOffset,
        out int innerLength)
    {
        var offset = ReadPrefix(payload, expectedType);
        sessionId = ReadGuid(payload, ref offset);
        generation = ReadInt32(payload, ref offset);
        innerLength = ReadUInt16(payload, ref offset);
        Ensure(payload, offset, innerLength);
        innerOffset = offset;
        offset += innerLength;
        EnsureConsumed(payload, offset);
    }

    private static void WritePrefix(Span<byte> destination, byte type)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination[..4], Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(destination.Slice(4, 2), WorldReplicationConstants.SchemaVersion);
        destination[6] = type;
        destination[7] = 0;
    }

    private static int ReadPrefix(ReadOnlySpan<byte> payload, byte expectedType)
    {
        Ensure(payload, 0, PrefixBytes);
        if (BinaryPrimitives.ReadUInt32LittleEndian(payload[..4]) != Magic)
            throw new ProtocolException("World replication magic is invalid.");
        if (BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(4, 2)) != WorldReplicationConstants.SchemaVersion)
            throw new ProtocolException("World replication schema is incompatible.");
        if (payload[6] != expectedType || payload[7] != 0)
            throw new ProtocolException("World replication message kind is invalid.");
        return PrefixBytes;
    }

    private static void WriteManifest(Span<byte> destination, ref int offset, WorldManifestIdentity value)
    {
        WriteUInt32(destination, ref offset, value.FormatVersion);
        WriteUInt32(destination, ref offset, value.CompilerVersion);
        WriteEntity(destination, ref offset, value.MapContentId);
    }

    private static WorldManifestIdentity ReadManifest(ReadOnlySpan<byte> source, ref int offset) => new(
        ReadUInt32(source, ref offset), ReadUInt32(source, ref offset), ReadEntity(source, ref offset));

    private static void WriteEntity(Span<byte> destination, ref int offset, WorldEntityId value)
    {
        WriteUInt64(destination, ref offset, value.High);
        WriteUInt64(destination, ref offset, value.Low);
    }

    private static WorldEntityId ReadEntity(ReadOnlySpan<byte> source, ref int offset) => new(
        ReadUInt64(source, ref offset), ReadUInt64(source, ref offset));

    private static void WriteGuid(Span<byte> destination, ref int offset, Guid value)
    {
        if (!value.TryWriteBytes(destination.Slice(offset, 16)))
            throw new ProtocolException("Could not encode session identity.");
        offset += 16;
    }

    private static Guid ReadGuid(ReadOnlySpan<byte> source, ref int offset)
    {
        Ensure(source, offset, 16);
        var result = new Guid(source.Slice(offset, 16));
        offset += 16;
        return result;
    }

    private static void WriteUInt16(Span<byte> destination, ref int offset, ushort value)
    {
        BinaryPrimitives.WriteUInt16LittleEndian(destination.Slice(offset, 2), value); offset += 2;
    }
    private static ushort ReadUInt16(ReadOnlySpan<byte> source, ref int offset)
    {
        Ensure(source, offset, 2); var value = BinaryPrimitives.ReadUInt16LittleEndian(source.Slice(offset, 2)); offset += 2; return value;
    }
    private static void WriteUInt32(Span<byte> destination, ref int offset, uint value)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination.Slice(offset, 4), value); offset += 4;
    }
    private static uint ReadUInt32(ReadOnlySpan<byte> source, ref int offset)
    {
        Ensure(source, offset, 4); var value = BinaryPrimitives.ReadUInt32LittleEndian(source.Slice(offset, 4)); offset += 4; return value;
    }
    private static void WriteInt32(Span<byte> destination, ref int offset, int value)
    {
        BinaryPrimitives.WriteInt32LittleEndian(destination.Slice(offset, 4), value); offset += 4;
    }
    private static int ReadInt32(ReadOnlySpan<byte> source, ref int offset)
    {
        Ensure(source, offset, 4); var value = BinaryPrimitives.ReadInt32LittleEndian(source.Slice(offset, 4)); offset += 4; return value;
    }
    private static void WriteUInt64(Span<byte> destination, ref int offset, ulong value)
    {
        BinaryPrimitives.WriteUInt64LittleEndian(destination.Slice(offset, 8), value); offset += 8;
    }
    private static ulong ReadUInt64(ReadOnlySpan<byte> source, ref int offset)
    {
        Ensure(source, offset, 8); var value = BinaryPrimitives.ReadUInt64LittleEndian(source.Slice(offset, 8)); offset += 8; return value;
    }

    private static byte[] ReadPayload(ReadOnlySpan<byte> source, ref int offset)
    {
        var length = ReadUInt16(source, ref offset);
        return ReadPayload(source, ref offset, length);
    }

    private static byte[] ReadPayload(ReadOnlySpan<byte> source, ref int offset, int length)
    {
        if (length is < 0 or > WorldReplicationConstants.MaximumPayloadBytes)
            throw new ProtocolException("World payload exceeds the bound.");
        Ensure(source, offset, length);
        var result = source.Slice(offset, length).ToArray();
        offset += length;
        return result;
    }

    private static bool ReadBoolean(byte value, string field) => value switch
    {
        0 => false,
        1 => true,
        _ => throw new ProtocolException($"World replication {field} boolean is invalid.")
    };

    private static void Ensure(ReadOnlySpan<byte> source, int offset, int length)
    {
        if (offset < 0 || length < 0 || offset > source.Length - length)
            throw new ProtocolException("World replication payload is truncated.");
    }

    private static void EnsureConsumed(ReadOnlySpan<byte> source, int offset)
    {
        if (offset != source.Length) throw new ProtocolException("World replication payload has trailing bytes.");
    }
}

public static class WorldReplicationValidation
{
    public static void Validate(WorldManifestIdentity value)
    {
        if (!value.IsValid) throw new ProtocolException("World manifest identity is invalid.");
    }

    public static void Validate(WorldIntent? value)
    {
        if (value is null) throw new ProtocolException("World intent is missing.");
        Validate(value.Manifest);
        ValidateEntity(value.Entity);
        ValidatePayload(value.Payload);
        if (value.LevelGeneration == 0 || value.ClientSequence == 0 || value.Operation == 0
            || value.RequestedKind is WorldMutationKind.Intent
            || !Enum.IsDefined(value.RequestedKind))
            throw new ProtocolException("World intent is invalid.");
        CathodePrimitiveCodec.ValidateIntent(value);
        WorldInteractionAdapterCodec.ValidateIntent(value);
        WorldCathodeBusCodec.ValidateIntent(value);
        WorldAnimatedAdapterCodec.ValidateIntent(value);
    }

    public static void Validate(WorldMutation? value)
    {
        if (value is null) throw new ProtocolException("World mutation is missing.");
        Validate(value.Manifest);
        ValidateEntity(value.Entity);
        ValidatePayload(value.Payload);
        const WorldMutationFlags known = WorldMutationFlags.Reliable | WorldMutationFlags.Replaceable
            | WorldMutationFlags.Replayable | WorldMutationFlags.SyntheticProbe;
        if (value.LevelGeneration == 0 || value.Sequence == 0 || value.TimelineTick == 0
            || value.SourcePeer == 0
            || value.Operation == 0 || value.Kind is WorldMutationKind.Intent
            || !Enum.IsDefined(value.Kind) || (value.Flags & ~known) != WorldMutationFlags.None
            || (value.Flags & WorldMutationFlags.Reliable) == WorldMutationFlags.None)
            throw new ProtocolException("World mutation is invalid.");
        if (value.Kind == WorldMutationKind.Event)
        {
            if (value.Revision != 0
                || (value.Flags & (WorldMutationFlags.Replaceable | WorldMutationFlags.Replayable))
                    != WorldMutationFlags.None)
                throw new ProtocolException("World events cannot carry replaceable/replay semantics.");
        }
        else if (value.Revision == 0
            || (value.Flags & (WorldMutationFlags.Replaceable | WorldMutationFlags.Replayable))
                != (WorldMutationFlags.Replaceable | WorldMutationFlags.Replayable))
        {
            throw new ProtocolException("World state/timeline mutations require revision and replay semantics.");
        }
        CathodePrimitiveCodec.ValidateMutation(value);
        WorldInteractionAdapterCodec.ValidateMutation(value);
        WorldCathodeBusCodec.ValidateMutation(value);
        WorldAnimatedAdapterCodec.ValidateMutation(value);
    }

    public static void Validate(WorldMutationResult? value)
    {
        if (value is null || value.Sequence == 0 || !Enum.IsDefined(value.Code))
            throw new ProtocolException("World mutation result is invalid.");
        if (value.Dispatched && !value.EntityResolved)
            throw new ProtocolException("A world mutation cannot be dispatched without resolving its entity.");
    }

    public static void Validate(WorldNetworkContext? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.WorldGeneration <= 0 || value.ManifestSha256.Length != 64
            || !value.ManifestSha256.All(Uri.IsHexDigit))
            throw new ProtocolException("World network context is invalid.");
        Validate(value.Manifest);
    }

    public static void Validate(WorldNetworkIntent? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.WorldGeneration <= 0)
            throw new ProtocolException("World network intent envelope is invalid.");
        Validate(value.Intent);
    }

    public static void Validate(WorldNetworkMutation? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.WorldGeneration <= 0)
            throw new ProtocolException("World network mutation envelope is invalid.");
        Validate(value.Mutation);
        if (value.Mutation.LevelGeneration != checked((uint)value.WorldGeneration))
            throw new ProtocolException("World network mutation generation is inconsistent.");
    }

    public static void Validate(WorldNetworkMutationAck? value)
    {
        if (value is null || value.SessionId == Guid.Empty || value.PeerId == 0
            || value.WorldGeneration <= 0)
            throw new ProtocolException("World network mutation acknowledgement is invalid.");
        Validate(value.Result);
    }

    public static bool Compatible(WorldNetworkContext left, WorldNetworkContext right) =>
        left.Ready && right.Ready
        && left.SessionId == right.SessionId
        && left.WorldGeneration == right.WorldGeneration;
        // WorldManifest identity/hash equality check DISABLED: two untouched copies of the
        // same level can legitimately produce different manifest hashes.
        // && left.Manifest == right.Manifest
        // && string.Equals(left.ManifestSha256, right.ManifestSha256, StringComparison.OrdinalIgnoreCase);

    private static void ValidateEntity(WorldEntityId value)
    {
        if (!value.IsValid) throw new ProtocolException("World entity identity is invalid.");
    }

    private static void ValidatePayload(byte[]? value)
    {
        if (value is null || value.Length > WorldReplicationConstants.MaximumPayloadBytes)
            throw new ProtocolException("World mutation payload exceeds the bound.");
    }
}
