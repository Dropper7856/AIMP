using System.Buffers.Binary;

namespace AIMP.Protocol;

public readonly record struct AISyncExecutionScope(Guid Session, ulong AuthorityEpoch, ulong WorldEpoch, ulong ConsumerEpoch);
public sealed record AISyncExecutionBatch(AISyncExecutionScope Scope, ulong BatchId, AISyncTransaction[] Records);
public enum AISyncExecutionStage : uint
{
    Received = 1, Waiting, Entered, Admitted, Rejected, Unknown, Released, Superseded
}

// Numeric values match aimp::ai_sync::transactions::Result; no native address,
// vtable, process-local handle or engine status grants executable authority.
public enum AISyncExecutionResult : uint
{
    Ok = 0, InvalidRecord, InvalidEncoding, WrongScope, StaleOwner,
    Duplicate, Conflict, GlobalGap, OwnerGap, MissingBegin,
    InvalidTransition, RevisionMismatch, OwnerBusy, ObservationRejected,
    CoverageLost, NativeAlreadyEntered, OutcomeUnknown
}

public sealed record AISyncExecutionReceipt(AISyncExecutionScope Scope, ulong BatchId,
    AISyncTransaction Operation, AISyncExecutionStage Stage, AISyncExecutionResult Result);

/// <summary>
/// AIEX v1 is an executable contract, independent of the shadow transport.
/// All integers/floats are little endian; GUID bytes use Guid.ToByteArray order.
/// Common header (64 bytes): magic:u32@0, version:u16@4, kind:u16@6,
/// totalBytes:u32@8, reserved:u32@12=0, session:16@16, authority:u64@32,
/// world:u64@40, consumer:u64@48, batchId:u64@56 (nonzero).
/// Batch kind=1 adds count:u32@64 (1..16), reserved:u32@68=0, then exact
/// 160-byte AISyncTransactionCodec records at 72. Receipt kind=2 adds the exact
/// executable operation at 64, stage:u32@224 and result:u32@228 (232 total).
/// V2 extends each record with admission, retention and a 244-byte movement
/// bundle. V3 appends 144 bytes of native intent to EVERY record; nonintent
/// extensions are all zero. Receipts echo the same full versioned operation.
/// Scope is an authorization binding reference, never proof of sender authority.
/// A receiver must authenticate the Host and compare its CURRENT binding again
/// immediately before native entry; decoding cannot establish native admission.
/// Received/Waiting are not Entered/Admitted, and no Applied claim exists here.
/// </summary>
public static class AISyncExecutionCodec
{
    public const uint Magic = 0x58454941U; // AIEX
    public const ushort Version = 1;
    public const ushort MovementVersion = 2;
    public const ushort NativeIntentVersion = 3;
    public const int HeaderBytes = 64;
    public const int BatchHeaderBytes = 72;
    public const int MaxBatchRecords = 16;
    public const int ExtendedRecordBytes = AISyncTransactionCodec.RecordBytes + 8 + AISyncMovementCodec.Bytes;
    public const int NativeIntentRecordBytes = ExtendedRecordBytes + AISyncNativeIntentCodec.Bytes;
    public const int MaxBatchBytes = BatchHeaderBytes + MaxBatchRecords * NativeIntentRecordBytes;
    public const int ReceiptBytes = HeaderBytes + AISyncTransactionCodec.RecordBytes + 8;
    public const int MaximumReceiptBytes = HeaderBytes + NativeIntentRecordBytes + 8;
    public const uint NativeIntentKind = AISyncNativeIntentCodec.Kind;
    public const uint MarkerKind = 2, MarkerProfile = 1024;
    public const uint ActionKind = 5, ActionNoState = 0x5D, PlacementKind = 8, DirectedKind = 9,
        RelocateKind = 0x100, BackstageKind = 0x101, FrontstageKind = 0x102, ForcedRelocateKind = 0x103, DirectRelocateKind = 0x104, DirectForcedRelocateKind = 0x105;
    public const float MaxCoordinate = 100000F;
    public const float MaxTurnAngle = 6.4F;

    public static byte[] EncodeBatch(AISyncExecutionBatch value)
    {
        ArgumentNullException.ThrowIfNull(value);
        ValidateScope(value.Scope);
        if (value.BatchId == 0 || value.Records is null || value.Records.Length is < 1 or > MaxBatchRecords)
            throw new ProtocolException("Invalid AISync executable batch identity or count.");
        foreach (var record in value.Records) ValidateExecutable(record, value.Scope);
        var version = value.Records.Any(Intent) ? NativeIntentVersion : value.Records.Any(Extended) ? MovementVersion : Version;
        var stride = Stride(version);
        var bytes = Header(value.Scope, value.BatchId, 1, BatchHeaderBytes + value.Records.Length * stride, version);
        W32(bytes, 64, (uint)value.Records.Length);
        for (var i = 0; i < value.Records.Length; i++)
            WriteOperation(value.Records[i], bytes.AsSpan(BatchHeaderBytes + i * stride, stride));
        return bytes;
    }

    /// <summary>
    /// Reads the bounded envelope only, before body allocation. This does not
    /// validate records, authenticate a sender, acknowledge receipt or allow entry.
    /// </summary>
    public static AISyncExecutionScope DecodeBatchScope(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < BatchHeaderBytes || bytes.Length > MaxBatchBytes)
            throw new ProtocolException("Invalid AISync executable batch size.");
        var (scope, _) = ReadHeader(bytes, 1, bytes.Length);
        var count = U32(bytes, 64);
        if (count is < 1 or > MaxBatchRecords || U32(bytes, 68) != 0
            || bytes.Length != BatchHeaderBytes + count * Stride(bytes))
            throw new ProtocolException("Invalid AISync executable batch count or reserved field.");
        return scope;
    }

    public static AISyncExecutionBatch DecodeBatch(ReadOnlySpan<byte> bytes)
    {
        var scope = DecodeBatchScope(bytes);
        var records = new AISyncTransaction[(int)U32(bytes, 64)];
        for (var i = 0; i < records.Length; i++)
        {
            records[i] = ReadOperation(bytes.Slice(BatchHeaderBytes + i * Stride(bytes), Stride(bytes)));
            ValidateExecutable(records[i], scope);
        }
        return new(scope, U64(bytes, 56), records);
    }

    public static AISyncExecutionBatch DecodeBatch(ReadOnlySpan<byte> bytes, AISyncExecutionScope currentBinding)
    {
        RequireCurrentBinding(DecodeBatchScope(bytes), currentBinding);
        return DecodeBatch(bytes);
    }

    public static byte[] EncodeReceipt(AISyncExecutionReceipt value)
    {
        ArgumentNullException.ThrowIfNull(value);
        ValidateExecutable(value.Operation, value.Scope);
        ValidateReceiptStatus(value.Stage, value.Result);
        var extended = Extended(value.Operation);
        var version = Intent(value.Operation) ? NativeIntentVersion : extended ? MovementVersion : Version;
        var stride = Stride(version);
        var bytes = Header(value.Scope, value.BatchId, 2, HeaderBytes + stride + 8, version);
        WriteOperation(value.Operation, bytes.AsSpan(HeaderBytes, stride));
        W32(bytes, HeaderBytes + stride, (uint)value.Stage);
        W32(bytes, HeaderBytes + stride + 4, (uint)value.Result);
        return bytes;
    }

    public static AISyncExecutionReceipt DecodeReceipt(ReadOnlySpan<byte> bytes)
    {
        var (scope, batchId) = ReadHeader(bytes, 2, bytes.Length);
        var stride = Stride(bytes);
        if (bytes.Length != HeaderBytes + stride + 8) throw new ProtocolException("Invalid executable receipt size.");
        var operation = ReadOperation(bytes.Slice(HeaderBytes, stride));
        ValidateExecutable(operation, scope);
        var stage = (AISyncExecutionStage)U32(bytes, HeaderBytes + stride);
        var result = (AISyncExecutionResult)U32(bytes, HeaderBytes + stride + 4);
        ValidateReceiptStatus(stage, result);
        return new(scope, batchId, operation, stage, result);
    }

    // Envelope inspection only. A retired body must not fence a fresh binding.
    public static AISyncExecutionScope DecodeReceiptScope(ReadOnlySpan<byte> bytes)
    {
        var (scope, _) = ReadHeader(bytes, 2, bytes.Length);
        if (bytes.Length != HeaderBytes + Stride(bytes) + 8)
            throw new ProtocolException("Invalid executable receipt size.");
        return scope;
    }

    public static AISyncExecutionReceipt DecodeReceipt(ReadOnlySpan<byte> bytes, AISyncExecutionScope currentBinding)
    {
        var receipt = DecodeReceipt(bytes);
        RequireCurrentBinding(receipt.Scope, currentBinding);
        return receipt;
    }

    public static void RequireCurrentBinding(AISyncExecutionScope received, AISyncExecutionScope currentBinding)
    {
        ValidateScope(received);
        ValidateScope(currentBinding);
        if (received != currentBinding)
            throw new ProtocolException("AISync executable scope does not match the current binding.");
    }

    public static void ValidateExecutable(AISyncTransaction value, AISyncExecutionScope scope)
    {
        ValidateScope(scope);
        RequireCurrentBinding(new(value.Session, value.AuthorityEpoch, value.WorldEpoch, value.ConsumerEpoch), scope);
        ValidateExecutable(value);
    }

    /// <summary>
    /// Begin/Submit/Update profiles: kind4 turn uses Mode bit0 style/bit1 aim
    /// (0..3), HasGoal selects XYZ turn point (Aux=+0) versus Aux heading
    /// [-6.4,6.4] (XYZ=+0), Distance is turn tolerance [0,6.4]. Coordinates
    /// are finite with abs&lt;=100000. Stop=+0, Gait=0, no HasGait/HasStop.
    /// Kind7 stance uses Mode 0..2, flags=0, all floats=+0, Gait=0.
    /// Kind8 scheduler placement uses HasGoal, XYZ position, Aux yaw[-6.4,6.4],
    /// Mode branch0/1, Distance/Stop=+0, Gait=0, no extension. It preserves
    /// native admission, retention and lifecycle; it is not a direct placement call.
    /// Kind6/11 scheduler control uses flags/mode/gait=0, all floats=+0,
    /// with no movement or native intent payload.
    /// All executable records reserve NativeStatus=0 and forbid Accepted,
    /// Incomplete, ObservationOnly, Oracle and CoverageLost. Terminal operations
    /// Cancel/Stop/Complete/Release retain the native kind but require flags/mode/gait=0
    /// and all float bits=0. Unknown/irrelevant fields are rejected, never erased.
    /// The base codec supplies full identity, owner and finite-float validation.
    /// </summary>
    public static void ValidateExecutable(AISyncTransaction value)
    {
        AISyncTransactionCodec.Validate(value);
        if (value.Opcode is AISyncTransactionOpcode.Oracle or AISyncTransactionOpcode.CoverageLost
            || (value.Flags & ~AISyncTransactionFlags.HasGoal) != 0
            || (value.NativeKind != MarkerKind && AISyncExecutionControlCodec.ProfileForKind(value.NativeKind) == 0) || value.NativeStatus != 0
            || (value.Gait != 0 && value.NativeKind != ActionKind)
            || value.NativeAdmission > 2 || value.ScalarRetention > 3)
            throw new ProtocolException("Unsupported AISync executable operation, profile or flags.");
        if (value.NativeKind == MarkerKind)
        {
            if (value.Flags != 0 || value.Mode != 0 || value.Gait != 0 || !EmptyPayload(value) || value.ScalarRetention != 3)
                throw new ProtocolException("Marker requires canonical scalar parameters.");
            if (value.Opcode is AISyncTransactionOpcode.Begin or AISyncTransactionOpcode.Submit or AISyncTransactionOpcode.Update)
            {
                if (value.NativeIntent?.ContractId != AISyncNativeIntentCodec.MarkerContract)
                    throw new ProtocolException("Marker requires contract24.");
                AISyncNativeIntentCodec.Validate(value.NativeIntent);
                var m = value.Movement;
                AISyncMovementCodec.Validate(m);
                if (AISyncMovementCodec.IsEmpty(m) || m.Version != 1 || m.Parts != 1
                    || m.Navigation != AISyncMovementNavigation.HostRoute || m.PrimaryFlags != 0)
                    throw new ProtocolException("Marker requires a single HostRoute movement bundle.");
                // BEFORE is authored before0044D190; AFTER may change retained only.
                var geometry = AISyncMovementCodec.Encode(m);
                if (!geometry.AsSpan(20, 68).SequenceEqual(geometry.AsSpan(92, 68)))
                    throw new ProtocolException("Marker geometry changed across admission.");
            }
            else if (value.NativeIntent is not null || !AISyncMovementCodec.IsEmpty(value.Movement))
                throw new ProtocolException("Marker terminal requires empty extensions.");
            return;
        }
        if (value.NativeKind == NativeIntentKind)
        {
            if (value.NativeIntent?.ContractId == AISyncNativeIntentCodec.MarkerContract)
                throw new ProtocolException("Contract24 is reserved for native kind2.");
            if (value.NativeAdmission != 1 || value.ScalarRetention != 3 || value.Flags != 0 || value.Mode != 0
                || value.Gait != 0 || !EmptyPayload(value) || !AISyncMovementCodec.IsEmpty(value.Movement))
                throw new ProtocolException("Native intent requires canonical append-call parameters.");
            if (value.Opcode is AISyncTransactionOpcode.Begin or AISyncTransactionOpcode.Submit)
            {
                if (value.NativeIntent is null) throw new ProtocolException("Native intent requires its exact call payload.");
                AISyncNativeIntentCodec.Validate(value.NativeIntent);
                return;
            }
            if (value.Opcode is not (AISyncTransactionOpcode.Complete or AISyncTransactionOpcode.Release) || value.NativeIntent is not null)
                throw new ProtocolException("Native intent only supports Begin/Submit then empty Complete/Release.");
            return;
        }
        if (value.NativeIntent is not null) throw new ProtocolException("Unexpected native intent extension.");
        if (value.Opcode is AISyncTransactionOpcode.Cancel or AISyncTransactionOpcode.Stop
            or AISyncTransactionOpcode.Complete or AISyncTransactionOpcode.Release)
        {
            if (value.Flags != 0 || value.Mode != 0 || value.Gait != 0 || !EmptyPayload(value) || !AISyncMovementCodec.IsEmpty(value.Movement) || value.ScalarRetention != 3)
                throw new ProtocolException("AISync terminal operations require canonical empty parameters.");
            return;
        }
        if (value.NativeKind is 1 or 3)
        {
            if (value.Flags != 0 || value.Mode != 0 || !EmptyPayload(value) || AISyncMovementCodec.IsEmpty(value.Movement) || value.ScalarRetention != 3)
                throw new ProtocolException("Move requires its complete executable bundle.");
            AISyncMovementCodec.Validate(value.Movement);
            if (value.NativeKind == 1 ? value.Movement.PrimaryFlags != 0 : value.Movement.Parts != 1)
                throw new ProtocolException("Invalid movement bundle composition for native kind.");
            return;
        }
        if (!AISyncMovementCodec.IsEmpty(value.Movement)) throw new ProtocolException("Unexpected movement extension.");
        if (value.NativeKind is 6 or 11)
        {
            // Factories 006823D0/00682390 use only base request data through +18.
            if (value.Flags != 0 || value.Mode != 0 || value.Gait != 0 || !EmptyPayload(value))
                throw new ProtocolException("Invalid AISync executable scheduler control parameters.");
            return;
        }
        if (value.NativeKind == ActionKind)
        {
            // Native kind5 scheduler hold: +1C action cycle state, +20/+21/+22 flags in Gait bits0..2.
            if (value.Flags != 0 || value.Mode > 0xFF || value.Mode == ActionNoState || value.Gait > 7 || !EmptyPayload(value))
                throw new ProtocolException("Invalid AISync executable action parameters.");
            return;
        }
        if (value.NativeKind == PlacementKind)
        {
            if (value.Flags != AISyncTransactionFlags.HasGoal || value.Mode > 1
                || MathF.Abs(value.GoalX) > MaxCoordinate || MathF.Abs(value.GoalY) > MaxCoordinate
                || MathF.Abs(value.GoalZ) > MaxCoordinate || MathF.Abs(value.Aux) > MaxTurnAngle
                || !PositiveZero(value.Distance) || !PositiveZero(value.Stop))
                throw new ProtocolException("Invalid AISync executable scheduler placement parameters.");
            return;
        }
        if (value.NativeKind is RelocateKind or ForcedRelocateKind or DirectRelocateKind or DirectForcedRelocateKind)
        {
            if(value.Opcode == AISyncTransactionOpcode.Update || value.NativeAdmission != 1)
                throw new ProtocolException("Lifecycle calls are one-shot append events.");
            // Native 007C6330: Goal=position, Aux=yaw, Mode=placement mode.
            if (value.Flags != AISyncTransactionFlags.HasGoal || value.Mode > 15 || MathF.Abs(value.GoalX) > MaxCoordinate
                || MathF.Abs(value.GoalY) > MaxCoordinate || MathF.Abs(value.GoalZ) > MaxCoordinate
                || MathF.Abs(value.Aux) > MaxTurnAngle || !PositiveZero(value.Distance) || !PositiveZero(value.Stop))
                throw new ProtocolException("Invalid AISync executable relocation parameters.");
            return;
        }
        if (value.NativeKind is BackstageKind or FrontstageKind)
        {
            if(value.Opcode == AISyncTransactionOpcode.Update || value.NativeAdmission != 1)
                throw new ProtocolException("Lifecycle calls are one-shot append events.");
            // Native 0078B140: Mode = backstage flag. HasGoal carries the Host
            // Character pose at that transition: Goal=position, Aux=yaw.
            if (value.Mode > (value.NativeKind == BackstageKind ? 1U : 0U))
                throw new ProtocolException("Invalid AISync executable backstage parameters.");
            if (value.Flags == 0 && EmptyPayload(value)) return;
            if (value.Flags != AISyncTransactionFlags.HasGoal || MathF.Abs(value.GoalX) > MaxCoordinate
                || MathF.Abs(value.GoalY) > MaxCoordinate || MathF.Abs(value.GoalZ) > MaxCoordinate
                || MathF.Abs(value.Aux) > MaxTurnAngle || !PositiveZero(value.Distance) || !PositiveZero(value.Stop))
                throw new ProtocolException("Invalid AISync executable backstage parameters.");
            return;
        }
        if (value.NativeKind == DirectedKind)
        {
            // Native kind9: Aux=heading, Distance=tolerance, Stop=travel distance.
            if (value.Flags != 0 || value.Mode != 0 || !PositiveZero(value.GoalX) || !PositiveZero(value.GoalY)
                || !PositiveZero(value.GoalZ) || MathF.Abs(value.Aux) > MaxTurnAngle
                || value.Distance > MaxTurnAngle || value.Stop > 1000F)
                throw new ProtocolException("Invalid AISync executable directed parameters.");
            return;
        }
        if (value.NativeKind == 7)
        {
            if (value.Flags != 0 || value.Mode > 2 || !EmptyPayload(value))
                throw new ProtocolException("Invalid AISync executable stance parameters.");
            return;
        }
        if (value.Mode > 3 || value.Distance > MaxTurnAngle || !PositiveZero(value.Stop)
            || MathF.Abs(value.GoalX) > MaxCoordinate || MathF.Abs(value.GoalY) > MaxCoordinate
            || MathF.Abs(value.GoalZ) > MaxCoordinate)
            throw new ProtocolException("Invalid AISync executable turn parameters.");
        if ((value.Flags & AISyncTransactionFlags.HasGoal) != 0
            ? !PositiveZero(value.Aux)
            : !PositiveZero(value.GoalX) || !PositiveZero(value.GoalY) || !PositiveZero(value.GoalZ)
                || MathF.Abs(value.Aux) > MaxTurnAngle)
            throw new ProtocolException("Invalid AISync executable turn point or heading.");
    }

    private static bool PositiveZero(float value) => BitConverter.SingleToUInt32Bits(value) == 0;
    private static int Stride(ushort version) => version == NativeIntentVersion ? NativeIntentRecordBytes
        : version == MovementVersion ? ExtendedRecordBytes : AISyncTransactionCodec.RecordBytes;
    private static int Stride(ReadOnlySpan<byte> bytes) => Stride(U16(bytes, 4));
    private static bool Intent(AISyncTransaction value) => value.NativeKind is MarkerKind or NativeIntentKind || value.NativeIntent is not null;
    private static bool Extended(AISyncTransaction value) => value.NativeKind is MarkerKind or 1 or 3 || value.NativeAdmission != 0 || value.ScalarRetention != 3;
    public static byte[] EncodeOperation(AISyncTransaction operation)
    {
        ValidateExecutable(operation);
        var bytes = new byte[Intent(operation) ? NativeIntentRecordBytes : Extended(operation) ? ExtendedRecordBytes : AISyncTransactionCodec.RecordBytes];
        WriteOperation(operation, bytes); return bytes;
    }
    private static void WriteOperation(AISyncTransaction value, Span<byte> bytes)
    {
        AISyncTransactionCodec.Encode(value, bytes[..AISyncTransactionCodec.RecordBytes]);
        if (bytes.Length is ExtendedRecordBytes or NativeIntentRecordBytes)
        {
            W32(bytes,160,value.NativeAdmission);W32(bytes,164,value.ScalarRetention);
            AISyncMovementCodec.Encode(value.Movement).CopyTo(bytes.Slice(AISyncTransactionCodec.RecordBytes+8, AISyncMovementCodec.Bytes));
        }
        else if (Extended(value)) throw new ProtocolException("Native directive requires execution envelope v2.");
        if (bytes.Length == NativeIntentRecordBytes)
            AISyncNativeIntentCodec.Encode(value.NativeIntent, bytes[ExtendedRecordBytes..]);
        else if (Intent(value)) throw new ProtocolException("Native intent requires execution envelope v3.");
    }
    private static AISyncTransaction ReadOperation(ReadOnlySpan<byte> bytes)
    {
        var value = AISyncTransactionCodec.Decode(bytes[..AISyncTransactionCodec.RecordBytes]);
        if (bytes.Length is ExtendedRecordBytes or NativeIntentRecordBytes)
            value = value with { NativeAdmission=U32(bytes,160),ScalarRetention=U32(bytes,164),
                Movement = AISyncMovementCodec.Decode(bytes.Slice(AISyncTransactionCodec.RecordBytes+8, AISyncMovementCodec.Bytes)) };
        else if (value.NativeKind is MarkerKind or 1 or 3) throw new ProtocolException("Move requires execution envelope v2.");
        if (bytes.Length == NativeIntentRecordBytes)
            value = value with { NativeIntent = AISyncNativeIntentCodec.Decode(bytes[ExtendedRecordBytes..]) };
        else if (value.NativeKind is MarkerKind or NativeIntentKind) throw new ProtocolException("Native intent requires execution envelope v3.");
        return value;
    }
    private static bool EmptyPayload(AISyncTransaction value) => PositiveZero(value.GoalX)
        && PositiveZero(value.GoalY) && PositiveZero(value.GoalZ) && PositiveZero(value.Distance)
        && PositiveZero(value.Stop) && PositiveZero(value.Aux);

    private static void ValidateReceiptStatus(AISyncExecutionStage stage, AISyncExecutionResult result)
    {
        if (stage is < AISyncExecutionStage.Received or > AISyncExecutionStage.Superseded
            || result > AISyncExecutionResult.OutcomeUnknown)
            throw new ProtocolException("Unknown AISync executable receipt stage or result.");
    }

    private static void ValidateScope(AISyncExecutionScope scope)
    {
        if (scope.Session == Guid.Empty || scope.AuthorityEpoch == 0 || scope.WorldEpoch == 0 || scope.ConsumerEpoch == 0)
            throw new ProtocolException("AISync executable scope requires the full session and nonzero epochs.");
    }

    private static byte[] Header(AISyncExecutionScope scope, ulong batchId, ushort kind, int length, ushort version = Version)
    {
        ValidateScope(scope);
        if (batchId == 0) throw new ProtocolException("AISync executable batch identity must be nonzero.");
        var bytes = new byte[length];
        W32(bytes, 0, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), version);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), kind);
        W32(bytes, 8, (uint)length);
        scope.Session.TryWriteBytes(bytes.AsSpan(16, 16), bigEndian: false, out _);
        W64(bytes, 32, scope.AuthorityEpoch);
        W64(bytes, 40, scope.WorldEpoch);
        W64(bytes, 48, scope.ConsumerEpoch);
        W64(bytes, 56, batchId);
        return bytes;
    }

    private static (AISyncExecutionScope Scope, ulong BatchId) ReadHeader(ReadOnlySpan<byte> bytes, ushort kind, int length)
    {
        if (bytes.Length < HeaderBytes || bytes.Length != length || U32(bytes, 0) != Magic
            || U16(bytes, 4) is not (Version or MovementVersion or NativeIntentVersion) || U16(bytes, 6) != kind || U32(bytes, 8) != length || U32(bytes, 12) != 0)
            throw new ProtocolException("Invalid AISync executable header, length or reserved field.");
        var scope = new AISyncExecutionScope(new Guid(bytes.Slice(16, 16), bigEndian: false),
            U64(bytes, 32), U64(bytes, 40), U64(bytes, 48));
        ValidateScope(scope);
        var batchId = U64(bytes, 56);
        if (batchId == 0) throw new ProtocolException("Invalid AISync executable batch identity.");
        return (scope, batchId);
    }

    private static ushort U16(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(bytes[offset..]);
    private static uint U32(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt32LittleEndian(bytes[offset..]);
    private static ulong U64(ReadOnlySpan<byte> bytes, int offset) => BinaryPrimitives.ReadUInt64LittleEndian(bytes[offset..]);
    private static void W32(Span<byte> bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(bytes[offset..], value);
    private static void W64(Span<byte> bytes, int offset, ulong value) => BinaryPrimitives.WriteUInt64LittleEndian(bytes[offset..], value);
}
