using System.Buffers.Binary;

namespace AIMP.Protocol;

public enum AISyncExecutionControlKind : ushort { Offer = 1, Ready, Recovery, Close, Checkpoint, CheckpointAck }
[Flags]
public enum AISyncExecutionProfiles : uint {
    Turn = 1, Stance = 2, Move = 4, AuthoritativeRoutes = 8, Action = 16, Directed = 32, Lifecycle = 64, NativeIntent = 128, SchedulerControl = 256,
    All = Turn | Stance | Move | Action | Directed | Lifecycle | NativeIntent | SchedulerControl, Known = All | AuthoritativeRoutes
}
public enum AISyncExecutionRecovery : uint { RestartRequired = 1, Capacity, InvalidStream, NativeUnavailable, TransportLost }
public sealed record AISyncExecutionControl(AISyncExecutionScope Scope, AISyncExecutionControlKind Kind, uint Value, ulong Through = 0);

// AIEC is a negotiation/binding control, never permission to own a native actor.
public static class AISyncExecutionControlCodec
{
    public const int Bytes = 64;
    public const uint Magic = 0x43454941;
    public static byte[] Encode(AISyncExecutionControl control)
    {
        Validate(control);
        var bytes = new byte[Bytes];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6), (ushort)control.Kind);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(8), Bytes);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12), control.Value);
        control.Scope.Session.TryWriteBytes(bytes.AsSpan(16, 16));
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(32), control.Scope.AuthorityEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(40), control.Scope.WorldEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(48), control.Scope.ConsumerEpoch);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(56), control.Through);
        return bytes;
    }
    public static AISyncExecutionControl Decode(ReadOnlySpan<byte> bytes)
    {
        var scope = DecodeScope(bytes);
        var control = new AISyncExecutionControl(scope, (AISyncExecutionControlKind)BinaryPrimitives.ReadUInt16LittleEndian(bytes[6..]),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes[12..]), BinaryPrimitives.ReadUInt64LittleEndian(bytes[56..]));
        Validate(control);
        return control;
    }
    // Validate framing and identity before reading scope-specific semantics.
    public static AISyncExecutionScope DecodeScope(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != Bytes || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != Magic
            || BinaryPrimitives.ReadUInt16LittleEndian(bytes[4..]) != 1
            || BinaryPrimitives.ReadUInt32LittleEndian(bytes[8..]) != Bytes)
            throw new ProtocolException("Invalid AISync execution control envelope.");
        var scope = new AISyncExecutionScope(new Guid(bytes.Slice(16, 16)),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[32..]), BinaryPrimitives.ReadUInt64LittleEndian(bytes[40..]),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes[48..]));
        AISyncExecutionCodec.RequireCurrentBinding(scope, scope);
        return scope;
    }
    private static void Validate(AISyncExecutionControl control)
    {
        ArgumentNullException.ThrowIfNull(control);
        AISyncExecutionCodec.RequireCurrentBinding(control.Scope, control.Scope);
        var checkpoint = control.Kind is AISyncExecutionControlKind.Checkpoint or AISyncExecutionControlKind.CheckpointAck;
        if (checkpoint ? control.Through == 0 : control.Through != 0)
            throw new ProtocolException("Invalid AISync checkpoint watermark.");
        var valid = control.Kind switch
        {
            AISyncExecutionControlKind.Offer or AISyncExecutionControlKind.Ready =>
                control.Value != 0 && (control.Value & ~(uint)AISyncExecutionProfiles.Known) == 0
                    && ((control.Value & 8U) == 0 || (control.Value & 4U) != 0),
            AISyncExecutionControlKind.Recovery => control.Value is >= 1 and <= 5,
            AISyncExecutionControlKind.Close => control.Value == 0,
            AISyncExecutionControlKind.Checkpoint or AISyncExecutionControlKind.CheckpointAck => control.Value == 0,
            _ => false
        };
        if (!valid) throw new ProtocolException("Invalid AISync execution control kind or value.");
    }
    public static uint ProfileForKind(uint kind) => kind switch
    {
        1 or 3 => (uint)AISyncExecutionProfiles.Move,
        4 => (uint)AISyncExecutionProfiles.Turn,
        6 or 11 => (uint)AISyncExecutionProfiles.SchedulerControl,
        7 => (uint)AISyncExecutionProfiles.Stance,
        AISyncExecutionCodec.ActionKind => (uint)AISyncExecutionProfiles.Action,
        AISyncExecutionCodec.DirectedKind => (uint)AISyncExecutionProfiles.Directed,
        AISyncNativeIntentCodec.Kind => (uint)AISyncExecutionProfiles.NativeIntent,
        AISyncExecutionCodec.RelocateKind or AISyncExecutionCodec.BackstageKind or AISyncExecutionCodec.FrontstageKind or AISyncExecutionCodec.ForcedRelocateKind or AISyncExecutionCodec.DirectRelocateKind or AISyncExecutionCodec.DirectForcedRelocateKind => (uint)AISyncExecutionProfiles.Lifecycle,
        _ => 0
    };
    public static bool Allows(uint profiles, AISyncTransaction record) =>
        (profiles & ProfileForKind(record.NativeKind)) != 0
        && (record.Movement.Navigation != AISyncMovementNavigation.HostRoute || (profiles & 8U) != 0);
}
