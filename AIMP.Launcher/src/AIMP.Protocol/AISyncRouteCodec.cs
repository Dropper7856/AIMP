using System.Buffers.Binary;
using System.Text;

namespace AIMP.Protocol;

public readonly record struct AISyncRouteKey(AISyncExecutionScope Scope, ulong OwnerEpoch,
    uint EntityA, uint EntityB, uint Birth, uint CharacterClass, AISyncTransactionCapability Capability,
    ulong CommandId, uint ParameterRevision, uint RouteRevision, uint Part = 0);
public sealed record AISyncRouteResource(string Level, uint State, uint FormatVersion,
    byte[] ImmutableDigest, uint[] HeaderTuple);
public sealed record AISyncRouteNode(uint Type, uint ResourceIndex, uint Ordinal, float[] Vectors)
{ public uint Markers { get; init; } }
public enum AISyncRouteFailure : uint { None, Unavailable, Unsupported, Capacity, InvalidStream, NativeFault }
public sealed record AISyncRouteGraph(AISyncRouteKey Key, float[] Auxiliary, uint[] Statuses,
    AISyncRouteResource[] Resources, AISyncRouteNode[] Nodes)
{
    public AISyncRouteFailure Failure { get; init; }
    public float[] Goal { get; init; } = new float[3];
}

// AISR is a value-only solved route, not a stream of actor coordinates. Native
// IDs/salts, pointers, mesh owners and R+08 local invalidation token are absent.
// Parsing proves neither a resource lifetime nor native installation permission.
public static class AISyncRouteCodec
{
    public const uint Magic = 0x52534941;
    public const ushort Version = 4;
    public const int HeaderBytes = 160, ResourceBytes = 148, NodeBytes = 52;
    public const int MaxResources = 8, MaxNodes = 256;
    public const int MaxBytes = HeaderBytes + MaxResources * ResourceBytes + MaxNodes * NodeBytes;

    public static void ValidateKey(AISyncRouteKey key)
    {
        AISyncTransactionCodec.Validate(new AISyncTransaction {
            Session = key.Scope.Session, AuthorityEpoch = key.Scope.AuthorityEpoch,
            WorldEpoch = key.Scope.WorldEpoch, ConsumerEpoch = key.Scope.ConsumerEpoch,
            OwnerEpoch = key.OwnerEpoch, EntityA = key.EntityA, EntityB = key.EntityB,
            Birth = key.Birth, CharacterClass = key.CharacterClass, Capability = key.Capability,
            CommandId = key.CommandId, ParameterRevision = key.ParameterRevision,
            GlobalSequence = 1, OwnerSequence = 1, Opcode = AISyncTransactionOpcode.Begin });
        if (key.RouteRevision == 0) throw new ProtocolException("Missing AISync route revision.");
        if (key.Part > 1) throw new ProtocolException("Unsupported AISync route part.");
    }
    public static void Validate(AISyncRouteGraph value)
    {
        ArgumentNullException.ThrowIfNull(value); ValidateKey(value.Key);
        if(value.Failure!=AISyncRouteFailure.None) {
            if(value.Failure>AISyncRouteFailure.NativeFault||value.Resources is not {Length:0}||value.Nodes is not {Length:0}||
                value.Goal is not {Length:3}||value.Auxiliary is not {Length:6}||value.Statuses is not {Length:4}||
                value.Goal.Any(v=>BitConverter.SingleToUInt32Bits(v)!=0)||value.Auxiliary.Any(v=>BitConverter.SingleToUInt32Bits(v)!=0)||value.Statuses.Any(v=>v!=0))
                throw new ProtocolException("Invalid command-scoped route refusal.");
            return;
        }
        if (value.Goal is not { Length: 3 } || value.Goal.Any(v => !Coordinate(v)) ||
            value.Auxiliary is not { Length: 6 } || value.Statuses is not { Length: 4 } ||
            value.Resources is null || value.Resources.Length is < 1 or > MaxResources ||
            value.Nodes is null || value.Nodes.Length is < 1 or > MaxNodes ||
            value.Auxiliary.Any(v => !Coordinate(v)) || value.Statuses.Any(v => v != 0x40000000))
            throw new ProtocolException("Unsupported AISync full-route domain.");
        for (var i = 0; i < value.Resources.Length; i++)
        {
            var resource = value.Resources[i];
            if (resource is null || string.IsNullOrEmpty(resource.Level) || resource.Level.Length >= 96 ||
                resource.Level.Any(c => !(c is >= 'A' and <= 'Z' or >= '0' and <= '9' or '_' or '-')) ||
                resource.FormatVersion != 7 || resource.ImmutableDigest is not { Length: 32 } ||
                resource.ImmutableDigest.All(b => b == 0) || resource.HeaderTuple is not { Length: 3 })
                throw new ProtocolException("Invalid AISync route resource identity.");
            for (var j = 0; j < i; j++)
                if (ResourceEqual(resource, value.Resources[j])) throw new ProtocolException("Duplicate AISync route resource.");
        }
        var used = new bool[value.Resources.Length]; var firstUse = 0;
        foreach (var node in value.Nodes)
        {
            if(node is not null && !ValidMarkers(node.Markers))throw new ProtocolException("Unsupported route markers.");
            if (node is null || node.Type > 2 || node.ResourceIndex >= value.Resources.Length || node.Ordinal >= (1U << 22) ||
                node.Vectors is not { Length: 9 } || node.Vectors.Any(v => !Coordinate(v)))
                throw new ProtocolException("Invalid AISync route node.");
            if (!used[node.ResourceIndex])
            {
                if (node.ResourceIndex != firstUse++) throw new ProtocolException("Noncanonical AISync resource order.");
                used[node.ResourceIndex] = true;
            }
        }
        if (firstUse != value.Resources.Length) throw new ProtocolException("Unused AISync route resource.");
    }
    public static byte[] Encode(AISyncRouteGraph value)
    {
        Validate(value); var bytes = new byte[HeaderBytes + value.Resources.Length * ResourceBytes + value.Nodes.Length * NodeBytes];
        W32(bytes, 0, Magic); W16(bytes, 4, Version); W16(bytes, 6, HeaderBytes); W32(bytes, 8, (uint)bytes.Length);
        W32(bytes, 12, (uint)value.Resources.Length); W32(bytes, 16, (uint)value.Nodes.Length);
        var key = value.Key;
        W32(bytes, 20, key.RouteRevision); W64(bytes, 24, key.CommandId); key.Scope.Session.TryWriteBytes(bytes.AsSpan(32, 16));
        W64(bytes, 48, key.Scope.AuthorityEpoch); W64(bytes, 56, key.Scope.WorldEpoch); W64(bytes, 64, key.Scope.ConsumerEpoch);
        W64(bytes, 72, key.OwnerEpoch); W32(bytes, 80, key.EntityA); W32(bytes, 84, key.EntityB);
        W32(bytes, 88, key.Birth); W32(bytes, 92, key.CharacterClass); W32(bytes, 96, (uint)key.Capability);
        W32(bytes, 100, key.ParameterRevision);
        for (var i = 0; i < 6; i++) WF(bytes, 104 + 4 * i, value.Auxiliary[i]);
        for (var i = 0; i < 4; i++) W32(bytes, 128 + 4 * i, value.Statuses[i]);
        if(value.Failure!=AISyncRouteFailure.None)W32(bytes,128,0x80000000U|(uint)value.Failure);
        W32(bytes, 144, key.Part);
        for (var i=0;i<3;i++) WF(bytes,148+4*i,value.Goal[i]);
        var at = HeaderBytes;
        foreach (var resource in value.Resources)
        {
            Encoding.ASCII.GetBytes(resource.Level, bytes.AsSpan(at, 96));
            W32(bytes, at + 96, resource.State); W32(bytes, at + 100, resource.FormatVersion);
            resource.ImmutableDigest.CopyTo(bytes, at + 104);
            for (var j = 0; j < 3; j++) W32(bytes, at + 136 + j * 4, resource.HeaderTuple[j]);
            at += ResourceBytes;
        }
        foreach (var node in value.Nodes)
        {
            W32(bytes, at, node.Type); W32(bytes, at + 4, node.ResourceIndex); W32(bytes, at + 8, node.Ordinal);
            for (var j = 0; j < 9; j++) WF(bytes, at + 12 + j * 4, node.Vectors[j]);
            W32(bytes,at+48,node.Markers);
            at += NodeBytes;
        }
        return bytes;
    }
    // Bounded header inspection precedes body allocation and allows a stale
    // scope to be discarded without admitting any resources from its payload.
    public static AISyncRouteKey DecodeKey(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < HeaderBytes || bytes.Length > MaxBytes || U32(bytes, 0) != Magic ||
            U16(bytes, 4) != Version || U16(bytes, 6) != HeaderBytes || U32(bytes, 8) != bytes.Length)
            throw new ProtocolException("Invalid AISync route envelope.");
        var resources = U32(bytes, 12); var nodes = U32(bytes, 16);
        if ((resources==0)!=(nodes==0) || resources>MaxResources || nodes>MaxNodes ||
            bytes.Length != HeaderBytes + resources * ResourceBytes + nodes * NodeBytes)
            throw new ProtocolException("Invalid AISync route bounded counts.");
        var key = new AISyncRouteKey(new(new Guid(bytes.Slice(32, 16)), U64(bytes, 48), U64(bytes, 56), U64(bytes, 64)),
            U64(bytes, 72), U32(bytes, 80), U32(bytes, 84), U32(bytes, 88), U32(bytes, 92),
            (AISyncTransactionCapability)U32(bytes, 96), U64(bytes, 24), U32(bytes, 100), U32(bytes, 20), U32(bytes, 144));
        ValidateKey(key); return key;
    }
    public static AISyncRouteGraph Decode(ReadOnlySpan<byte> bytes, AISyncRouteKey expected)
    {
        ValidateKey(expected);
        if (DecodeKey(bytes) != expected) throw new ProtocolException("AISync route does not match its current command binding.");
        var auxiliary = new float[6]; var statuses = new uint[4];
        for (var i = 0; i < 6; i++) auxiliary[i] = F(bytes, 104 + i * 4);
        for (var i = 0; i < 4; i++) statuses[i] = U32(bytes, 128 + i * 4);
        var resources = new AISyncRouteResource[(int)U32(bytes, 12)]; var nodes = new AISyncRouteNode[(int)U32(bytes, 16)];
        var at = HeaderBytes;
        for (var i = 0; i < resources.Length; i++, at += ResourceBytes)
        {
            var level = bytes.Slice(at, 96); var end = level.IndexOf((byte)0);
            if (end <= 0 || level.Slice(end).IndexOfAnyExcept((byte)0) >= 0 ||
                level[..end].IndexOfAnyInRange((byte)128, byte.MaxValue) >= 0)
                throw new ProtocolException("Invalid AISync route level bytes.");
            resources[i] = new(Encoding.ASCII.GetString(level[..end]), U32(bytes, at + 96), U32(bytes, at + 100),
                bytes.Slice(at + 104, 32).ToArray(), new[] { U32(bytes, at + 136), U32(bytes, at + 140), U32(bytes, at + 144) });
        }
        for (var i = 0; i < nodes.Length; i++, at += NodeBytes)
        {
            var vectors = new float[9]; for (var j = 0; j < 9; j++) vectors[j] = F(bytes, at + 12 + j * 4);
            nodes[i] = new(U32(bytes, at), U32(bytes, at + 4), U32(bytes, at + 8), vectors) { Markers=U32(bytes,at+48) };
        }
        var failure=AISyncRouteFailure.None;
        if(resources.Length==0&&nodes.Length==0) {
            if((statuses[0]&0x80000000U)==0)throw new ProtocolException("Invalid route refusal marker.");
            failure=(AISyncRouteFailure)(statuses[0]&0x7FFFFFFFU);statuses[0]=0;
            if(failure==AISyncRouteFailure.None)throw new ProtocolException("Missing route refusal reason.");
        }
        var graph = new AISyncRouteGraph(expected, auxiliary, statuses, resources, nodes)
        { Failure=failure, Goal = new[] { F(bytes,148), F(bytes,152), F(bytes,156) } }; Validate(graph); return graph;
    }
    private static bool Coordinate(float v) => float.IsFinite(v) && MathF.Abs(v) <= 100000;
    private static bool ValidMarkers(uint markers) {
        var ended=false;
        for(var i=0;i<4;i++) {
            var tag=(markers>>(8*i))&255U;
            if(tag==0)ended=true;
            else if(ended||tag is not (2 or 3 or 4 or 7 or 8 or 9 or 11))return false;
        }
        return true;
    }
    private static bool ResourceEqual(AISyncRouteResource a, AISyncRouteResource b) => a.Level == b.Level && a.State == b.State &&
        a.FormatVersion == b.FormatVersion && a.ImmutableDigest.AsSpan().SequenceEqual(b.ImmutableDigest) && a.HeaderTuple.AsSpan().SequenceEqual(b.HeaderTuple);
    private static ushort U16(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt16LittleEndian(b[at..]);
    private static uint U32(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b[at..]);
    private static ulong U64(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt64LittleEndian(b[at..]);
    private static float F(ReadOnlySpan<byte> b, int at) => BitConverter.UInt32BitsToSingle(U32(b, at));
    private static void W16(Span<byte> b, int at, int v) => BinaryPrimitives.WriteUInt16LittleEndian(b[at..], (ushort)v);
    private static void W32(Span<byte> b, int at, uint v) => BinaryPrimitives.WriteUInt32LittleEndian(b[at..], v);
    private static void W64(Span<byte> b, int at, ulong v) => BinaryPrimitives.WriteUInt64LittleEndian(b[at..], v);
    private static void WF(Span<byte> b, int at, float v) => W32(b, at, BitConverter.SingleToUInt32Bits(v));
}
