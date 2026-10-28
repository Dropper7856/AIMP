namespace AIMP.Protocol.Tests;

internal static class AISyncRouteTests
{
    internal static AISyncRouteGraph Fixture() => new(
        new(new(Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"), 2, 3, 1), 4, 0x1234, 0x5678, 5, 6,
            AISyncTransactionCapability.Movement, 7, 2, 3),
        new[] { BitConverter.UInt32BitsToSingle(0x80000000), 1F, 2F, 3F, 4F, 5F },
        Enumerable.Repeat(0x40000000U, 4).ToArray(),
        new[] { new AISyncRouteResource("TECH_COMMS", 0, 7, Enumerable.Range(1, 32).Select(v => (byte)v).ToArray(), new uint[] { 10, 20, 30 }) },
        new[] { new AISyncRouteNode(2, 0, 7, Enumerable.Range(0, 9).Select(v => (float)v).ToArray()),
            new AISyncRouteNode(2, 0, 9, Enumerable.Range(9, 9).Select(v => (float)v).ToArray()) }) { Goal = new[] { 12.5F, -6, 3 } };

    internal static Task FixtureAsync()
    {
        var value = Fixture(); var encoded = AISyncRouteCodec.Encode(value);
        string? fixture = null;
        for (var path = new DirectoryInfo(AppContext.BaseDirectory); path is not null; path = path.Parent)
        {
            var candidate = Path.Combine(path.FullName, "tests", "fixtures", "aisync-route-v4.hex");
            if (File.Exists(candidate)) { fixture = candidate; break; }
        }
        AssertEx.True(fixture is not null, "Missing shared native/managed AISR fixture.");
        var expected = Convert.FromHexString(string.Concat(File.ReadAllText(fixture!).Where(c => !char.IsWhiteSpace(c))));
        AssertEx.SequenceEqual(expected, encoded);
        var decoded = AISyncRouteCodec.Decode(expected, value.Key);
        AssertEx.SequenceEqual(expected, AISyncRouteCodec.Encode(decoded));
        AssertEx.Equal(0x80000000U, BitConverter.SingleToUInt32Bits(decoded.Auxiliary[0]));
        AssertEx.Equal(7U, decoded.Nodes[0].Ordinal); AssertEx.Equal(9U, decoded.Nodes[1].Ordinal);
        var marked=value with {Nodes=new[]{value.Nodes[0] with {Markers=0x090807U},value.Nodes[1] with {Markers=11}}};
        var markedBytes=AISyncRouteCodec.Encode(marked);
        var markedFixture=Path.Combine(Path.GetDirectoryName(fixture!)!,"aisync-route-markers-v4.hex");
        AssertEx.SequenceEqual(Convert.FromHexString(string.Concat(File.ReadAllText(markedFixture).Where(c=>!char.IsWhiteSpace(c)))),markedBytes);
        AssertEx.Equal(0x090807U,AISyncRouteCodec.Decode(markedBytes,marked.Key).Nodes[0].Markers);
        foreach(var invalid in new uint[]{1,6,10,12,0x00090007U,255})
            AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Encode(marked with {Nodes=new[]{marked.Nodes[0] with {Markers=invalid},marked.Nodes[1]}}));
        var companion = value with { Key = value.Key with { Part = 1 } };
        var companionBytes = AISyncRouteCodec.Encode(companion);
        AssertEx.Equal(1U, AISyncRouteCodec.DecodeKey(companionBytes).Part);
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(companionBytes, value.Key));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Key = value.Key with { Part = 2 } }));
        var oldVersion = encoded.ToArray(); oldVersion[4] = 1;
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.DecodeKey(oldVersion));
        // Solved route sequence may revisit the same node. Neither codec sorts it.
        var repeated = value with { Nodes = new[] { value.Nodes[1], value.Nodes[0], value.Nodes[1] } };
        var roundtrip = AISyncRouteCodec.Decode(AISyncRouteCodec.Encode(repeated), repeated.Key);
        AssertEx.SequenceEqual(new uint[] { 9, 7, 9 }, roundtrip.Nodes.Select(n => n.Ordinal));
        var traversal=value with { Nodes=new[] {value.Nodes[0],value.Nodes[1] with {Type=1},value.Nodes[0]} };
        var specialBytes=AISyncRouteCodec.Encode(traversal);
        var specialFixture=Path.Combine(Path.GetDirectoryName(fixture!)!,"aisync-route-special-v4.hex");
        AssertEx.SequenceEqual(Convert.FromHexString(string.Concat(File.ReadAllText(specialFixture).Where(c=>!char.IsWhiteSpace(c)))),specialBytes);
        var special=AISyncRouteCodec.Decode(specialBytes,traversal.Key);
        AssertEx.SequenceEqual(new uint[]{2,1,2},special.Nodes.Select(n=>n.Type));
        AssertEx.SequenceEqual(new uint[]{7,9,7},special.Nodes.Select(n=>n.Ordinal));
        var refusal=new AISyncRouteGraph(value.Key,new float[6],new uint[4],Array.Empty<AISyncRouteResource>(),Array.Empty<AISyncRouteNode>()){Failure=AISyncRouteFailure.Unsupported};
        var refusalBytes=AISyncRouteCodec.Encode(refusal);
        var refusalFixture=Path.Combine(Path.GetDirectoryName(fixture!)!,"aisync-route-failure-v4.hex");
        AssertEx.SequenceEqual(Convert.FromHexString(string.Concat(File.ReadAllText(refusalFixture).Where(c=>!char.IsWhiteSpace(c)))),refusalBytes);
        AssertEx.Equal(AISyncRouteFailure.Unsupported,AISyncRouteCodec.Decode(refusalBytes,refusal.Key).Failure);
        for(var i=0;i<refusalBytes.Length;i++)AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Decode(refusalBytes.AsSpan(0,i),refusal.Key));
        foreach(var offset in new[]{104,128,132,148,12}) {
            var invalid=refusalBytes.ToArray();invalid[offset]^=0x80;AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Decode(invalid,refusal.Key));
        }
        AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Encode(refusal with {Failure=(AISyncRouteFailure)256}));
        AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Encode(refusal with {Goal=new[]{BitConverter.UInt32BitsToSingle(0x80000000),0F,0F}}));
        foreach(var reason in Enum.GetValues<AISyncRouteFailure>().Where(r=>r!=AISyncRouteFailure.None)) {
            var fault=refusal with {Failure=reason};AssertEx.Equal(reason,AISyncRouteCodec.Decode(AISyncRouteCodec.Encode(fault),fault.Key).Failure);
        }
        return Task.CompletedTask;
    }
    internal static Task BoundsAsync()
    {
        var value = Fixture(); var encoded = AISyncRouteCodec.Encode(value);
        for (var n = 0; n < encoded.Length; n++)
            AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(encoded.AsSpan(0, n), value.Key));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(encoded.Concat(new byte[] { 0 }).ToArray(), value.Key));
        foreach (var offset in new[] { 0, 4, 6, 8, 12, 16, 144 })
        {
            var changed = encoded.ToArray(); changed[offset] ^= 0x80;
            AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(changed, value.Key));
        }
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(encoded,
            value.Key with { Scope = value.Key.Scope with { ConsumerEpoch = 2 } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(encoded, value.Key with { RouteRevision = 4 }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Statuses = new uint[] { 0x40, 0x40000000, 0x40000000, 0x40000000 } }));
        var boundary=value with { Nodes = new[] { value.Nodes[0] with { Type = 0, Markers=3 } } };
        AssertEx.Equal(0U,AISyncRouteCodec.Decode(AISyncRouteCodec.Encode(boundary),boundary.Key).Nodes[0].Type);
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Nodes = new[] { value.Nodes[0] with { Type = 3 } } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Nodes = new[] { value.Nodes[0] with { Ordinal = 1U << 22 } } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Auxiliary = new[] { float.NaN, 0, 0, 0, 0, 0 } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Goal = new[] { float.NaN, 0, 0 } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Goal = new[] { 100001F, 0, 0 } }));
        var invalidGoal=encoded.ToArray(); BitConverter.GetBytes(float.PositiveInfinity).CopyTo(invalidGoal,148);
        AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.Decode(invalidGoal,value.Key));
        var v2=encoded.ToArray();v2[4]=2;AssertEx.Throws<ProtocolException>(()=>AISyncRouteCodec.DecodeKey(v2));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Resources = new[] { value.Resources[0] with { ImmutableDigest = new byte[32] } } }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(value with { Resources = new[] { value.Resources[0] with { Level = "TECH_é" } } }));
        var dirtyPadding = encoded.ToArray(); dirtyPadding[AISyncRouteCodec.HeaderBytes + 95] = (byte)'A';
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(dirtyPadding, value.Key));
        var highAscii = encoded.ToArray(); highAscii[AISyncRouteCodec.HeaderBytes] = 0xC0;
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Decode(highAscii, value.Key));
        var resources = Enumerable.Range(0, 8).Select(i => value.Resources[0] with { Level = "STATE_" + i }).ToArray();
        var nodes = Enumerable.Range(0, 256).Select(i => value.Nodes[0] with { ResourceIndex = (uint)(i % 8), Ordinal = (uint)i }).ToArray();
        var max = value with { Resources = resources, Nodes = nodes }; var maxBytes = AISyncRouteCodec.Encode(max);
        AssertEx.Equal(AISyncRouteCodec.MaxBytes, maxBytes.Length);
        AssertEx.SequenceEqual(maxBytes, AISyncRouteCodec.Encode(AISyncRouteCodec.Decode(maxBytes, max.Key)));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(max with { Nodes = nodes.Append(nodes[0]).ToArray() }));
        AssertEx.Throws<ProtocolException>(() => AISyncRouteCodec.Encode(max with { Nodes = new[] { nodes[1], nodes[0] } }));
        return Task.CompletedTask;
    }
    internal static Task GeneratedAsync()
    {
        var random = new Random(260); var value = Fixture();
        for (var iteration = 0; iteration < 2000; iteration++)
        {
            var nodes = Enumerable.Range(0, random.Next(1, 257)).Select(_ => new AISyncRouteNode((uint)random.Next(1,3), 0,
                (uint)random.Next(1 << 22), Enumerable.Range(0, 9).Select(_ => (float)(random.NextDouble() * 200000 - 100000)).ToArray())).ToArray();
            var graph = value with { Key = value.Key with { RouteRevision = (uint)iteration + 1 }, Nodes = nodes };
            var bytes = AISyncRouteCodec.Encode(graph);
            AssertEx.SequenceEqual(bytes, AISyncRouteCodec.Encode(AISyncRouteCodec.Decode(bytes, graph.Key)));
        }
        return Task.CompletedTask;
    }
}
