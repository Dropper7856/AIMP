using System.Buffers.Binary;

namespace AIMP.Protocol;

public enum AISyncTransactionOpcode : uint
{
    Begin = 1,
    Update = 2,
    Submit = 3,
    Cancel = 4,
    Stop = 5,
    Complete = 6,
    Release = 7,
    Oracle = 8,
    CoverageLost = 9
}

public enum AISyncTransactionCapability : uint
{
    Movement = 1
}

[Flags]
public enum AISyncTransactionFlags : uint
{
    None = 0,
    HasGoal = 1,
    HasGait = 2,
    HasStop = 4,
    Accepted = 8,
    Incomplete = 16,
    ObservationOnly = 32
}

/// <summary>
/// P0/P1 shadow record mirrored by aimp/AISyncTransactions.h. Native objects,
/// pointers and process-local handles are never identities in this record.
/// </summary>
public readonly record struct AISyncTransaction
{
    public AISyncTransaction() { }
    public Guid Session { get; init; }
    public ulong AuthorityEpoch { get; init; }
    public ulong WorldEpoch { get; init; }
    public ulong ConsumerEpoch { get; init; }
    public ulong OwnerEpoch { get; init; }
    public uint EntityA { get; init; }
    public uint EntityB { get; init; }
    public uint Birth { get; init; }
    public uint CharacterClass { get; init; }
    public ulong CommandId { get; init; }
    public ulong ParentCommandId { get; init; }
    public ulong GlobalSequence { get; init; }
    public ulong OwnerSequence { get; init; }
    public AISyncTransactionOpcode Opcode { get; init; }
    public AISyncTransactionCapability Capability { get; init; }
    public uint ParameterRevision { get; init; }
    public AISyncTransactionFlags Flags { get; init; }
    public uint NativeKind { get; init; }
    public uint Gait { get; init; }
    public uint Mode { get; init; }
    public uint NativeStatus { get; init; }
    public float GoalX { get; init; }
    public float GoalY { get; init; }
    public float GoalZ { get; init; }
    public float Distance { get; init; }
    public float Stop { get; init; }
    public float Aux { get; init; }
    // Executable AIEX v2 extension, outside the historical AITX logical carrier.
    public AISyncMovementBundle Movement { get; init; }
    public uint NativeAdmission { get; init; }
    public uint ScalarRetention { get; init; } = 3;
    public AISyncNativeIntent? NativeIntent { get; init; } // AIEX v3 only.
}

/// <summary>
/// Canonical single-record little-endian codec, independent of lobby framing
/// and protocol negotiation. Byte-oriented entry points require exactly 160 bytes.
/// </summary>
public static class AISyncTransactionCodec
{
    public const uint Magic = 0x58544941U;
    public const ushort Version = 1;
    public const int RecordBytes = 160;
    public const AISyncTransactionFlags KnownFlags = AISyncTransactionFlags.HasGoal
        | AISyncTransactionFlags.HasGait | AISyncTransactionFlags.HasStop
        | AISyncTransactionFlags.Accepted | AISyncTransactionFlags.Incomplete
        | AISyncTransactionFlags.ObservationOnly;

    public static byte[] Encode(AISyncTransaction value)
    {
        Validate(value);
        var bytes = new byte[RecordBytes];
        Write(value, bytes);
        return bytes;
    }

    public static void Encode(AISyncTransaction value, Span<byte> destination)
    {
        if (destination.Length != RecordBytes)
            throw new ProtocolException("AISync transaction destination must be exactly 160 bytes.");
        Validate(value);
        Write(value, destination);
    }

    private static void Write(AISyncTransaction value, Span<byte> bytes)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes[4..], Version);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes[6..], RecordBytes);
        // Explicit .NET Guid byte order: identical to Guid.ToByteArray(),
        // including little-endian Data1/Data2/Data3, not RFC/network GUID order.
        value.Session.TryWriteBytes(bytes.Slice(8, 16), bigEndian: false, out _);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[24..], value.AuthorityEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[32..], value.WorldEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[40..], value.ConsumerEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[48..], value.OwnerEpoch);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[56..], value.EntityA);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[60..], value.EntityB);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[64..], value.Birth);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[68..], value.CharacterClass);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[72..], value.CommandId);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[80..], value.ParentCommandId);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[88..], value.GlobalSequence);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[96..], value.OwnerSequence);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[104..], (uint)value.Opcode);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[108..], (uint)value.Capability);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[112..], value.ParameterRevision);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[116..], (uint)value.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[120..], value.NativeKind);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[124..], value.Gait);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[128..], value.Mode);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes[132..], value.NativeStatus);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[136..], value.GoalX);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[140..], value.GoalY);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[144..], value.GoalZ);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[148..], value.Distance);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[152..], value.Stop);
        BinaryPrimitives.WriteSingleLittleEndian(bytes[156..], value.Aux);
    }

    public static AISyncTransaction Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != RecordBytes || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != Magic
            || BinaryPrimitives.ReadUInt16LittleEndian(bytes[4..]) != Version
            || BinaryPrimitives.ReadUInt16LittleEndian(bytes[6..]) != RecordBytes)
            throw new ProtocolException("Invalid AISync transaction header or record size.");
        var value = new AISyncTransaction
        {
            Session = new Guid(bytes.Slice(8, 16), bigEndian: false),
            AuthorityEpoch = BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..]),
            WorldEpoch = BinaryPrimitives.ReadUInt64LittleEndian(bytes[32..]),
            ConsumerEpoch = BinaryPrimitives.ReadUInt64LittleEndian(bytes[40..]),
            OwnerEpoch = BinaryPrimitives.ReadUInt64LittleEndian(bytes[48..]),
            EntityA = BinaryPrimitives.ReadUInt32LittleEndian(bytes[56..]),
            EntityB = BinaryPrimitives.ReadUInt32LittleEndian(bytes[60..]),
            Birth = BinaryPrimitives.ReadUInt32LittleEndian(bytes[64..]),
            CharacterClass = BinaryPrimitives.ReadUInt32LittleEndian(bytes[68..]),
            CommandId = BinaryPrimitives.ReadUInt64LittleEndian(bytes[72..]),
            ParentCommandId = BinaryPrimitives.ReadUInt64LittleEndian(bytes[80..]),
            GlobalSequence = BinaryPrimitives.ReadUInt64LittleEndian(bytes[88..]),
            OwnerSequence = BinaryPrimitives.ReadUInt64LittleEndian(bytes[96..]),
            Opcode = (AISyncTransactionOpcode)BinaryPrimitives.ReadUInt32LittleEndian(bytes[104..]),
            Capability = (AISyncTransactionCapability)BinaryPrimitives.ReadUInt32LittleEndian(bytes[108..]),
            ParameterRevision = BinaryPrimitives.ReadUInt32LittleEndian(bytes[112..]),
            Flags = (AISyncTransactionFlags)BinaryPrimitives.ReadUInt32LittleEndian(bytes[116..]),
            NativeKind = BinaryPrimitives.ReadUInt32LittleEndian(bytes[120..]),
            Gait = BinaryPrimitives.ReadUInt32LittleEndian(bytes[124..]),
            Mode = BinaryPrimitives.ReadUInt32LittleEndian(bytes[128..]),
            NativeStatus = BinaryPrimitives.ReadUInt32LittleEndian(bytes[132..]),
            GoalX = BinaryPrimitives.ReadSingleLittleEndian(bytes[136..]),
            GoalY = BinaryPrimitives.ReadSingleLittleEndian(bytes[140..]),
            GoalZ = BinaryPrimitives.ReadSingleLittleEndian(bytes[144..]),
            Distance = BinaryPrimitives.ReadSingleLittleEndian(bytes[148..]),
            Stop = BinaryPrimitives.ReadSingleLittleEndian(bytes[152..]),
            Aux = BinaryPrimitives.ReadSingleLittleEndian(bytes[156..])
        };
        Validate(value);
        return value;
    }

    public static void Validate(ReadOnlySpan<byte> bytes) => _ = Decode(bytes);

    public static void Validate(AISyncTransaction value)
    {
        if (value.Session == Guid.Empty || value.AuthorityEpoch == 0 || value.WorldEpoch == 0
            || value.ConsumerEpoch == 0 || value.OwnerEpoch == 0
            || (value.EntityA == 0 && value.EntityB == 0) || value.Birth == 0
            || value.CharacterClass == 0
            || value.CommandId != 0 && value.ParentCommandId == value.CommandId
            || value.GlobalSequence == 0 || value.OwnerSequence == 0)
            throw new ProtocolException("Invalid AISync transaction identity.");
        if (value.Opcode is < AISyncTransactionOpcode.Begin or > AISyncTransactionOpcode.CoverageLost
            || value.Capability != AISyncTransactionCapability.Movement
            || (value.Flags & ~KnownFlags) != 0)
            throw new ProtocolException("Unknown AISync transaction operation, capability or flags.");
        var observation = value.Opcode is AISyncTransactionOpcode.Oracle or AISyncTransactionOpcode.CoverageLost;
        if (observation ? (value.Flags & AISyncTransactionFlags.ObservationOnly) == 0
            : value.CommandId == 0 || value.ParameterRevision == 0)
            throw new ProtocolException("Invalid AISync transaction command or observation identity.");
        // Keep these bounds identical to native Validate(Record). Diagnostic
        // integers have no engine bounds; admission rules belong to the ledger.
        if (!float.IsFinite(value.GoalX) || !float.IsFinite(value.GoalY) || !float.IsFinite(value.GoalZ)
            || !float.IsFinite(value.Distance) || !float.IsFinite(value.Stop) || !float.IsFinite(value.Aux)
            || value.Distance < 0 || value.Stop < 0 || value.NativeAdmission > 2 || value.ScalarRetention > 3)
            throw new ProtocolException("Invalid AISync transaction numeric parameters.");
    }
}
