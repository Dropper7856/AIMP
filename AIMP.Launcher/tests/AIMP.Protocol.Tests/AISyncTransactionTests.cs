using System.Buffers.Binary;
using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class AISyncTransactionTests
{
    internal static Task CanonicalFixtureAsync()
    {
        // Shared checked-in input, also consumed independently by C++ tests.
        // Never create it from Encode: that would conceal a common offset bug.
        var fixture = ReadFixture();
        var expected = CanonicalValue();
        AssertEx.Equal(160, fixture.Length);
        AssertEx.Equal(expected, AISyncTransactionCodec.Decode(fixture));
        AssertEx.SequenceEqual(fixture, AISyncTransactionCodec.Encode(expected));
        AssertEx.SequenceEqual(expected.Session.ToByteArray(), fixture.AsSpan(8, 16).ToArray());
        AssertEx.SequenceEqual(Convert.FromHexString("33221100554477668899AABBCCDDEEFF"),
            fixture.AsSpan(8, 16).ToArray());
        AssertEx.Equal(Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"),
            AISyncTransactionCodec.Decode(fixture).Session);
        var destination = Enumerable.Repeat((byte)0xCD, 162).ToArray();
        AISyncTransactionCodec.Encode(expected, destination.AsSpan(1, 160));
        AssertEx.SequenceEqual(fixture, destination.AsSpan(1, 160).ToArray());
        AssertEx.Equal((byte)0xCD, destination[0]);
        AssertEx.Equal((byte)0xCD, destination[^1]);
        return Task.CompletedTask;
    }

    internal static Task MalformedRecordsAsync()
    {
        var fixture = ReadFixture();
        for (var length = 0; length < 160; ++length)
        {
            var truncated = fixture[..length];
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Decode(truncated));
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Validate(truncated));
        }
        foreach (var length in new[] { 161, 320, 1024 * 1024 })
        {
            var oversized = new byte[length];
            fixture.CopyTo(oversized, 0);
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Decode(oversized));
        }
        for (var offset = 0; offset < 8; ++offset)
        {
            var malformed = fixture.ToArray();
            malformed[offset] ^= 0x80;
            AssertRejected(malformed);
        }
        var emptySession = fixture.ToArray();
        emptySession.AsSpan(8, 16).Clear();
        AssertRejected(emptySession);
        foreach (var offset in new[] { 24, 32, 40, 48, 72, 88, 96 })
        {
            var malformed = fixture.ToArray();
            malformed.AsSpan(offset, 8).Clear();
            AssertRejected(malformed);
        }
        foreach (var offset in new[] { 64, 68, 112 }) AssertRejected(WithU32(fixture, offset, 0));
        var selfParent = fixture.ToArray();
        fixture.AsSpan(72, 8).CopyTo(selfParent.AsSpan(80, 8));
        AssertRejected(selfParent);
        var emptyEntity = fixture.ToArray();
        emptyEntity.AsSpan(56, 8).Clear();
        AssertRejected(emptyEntity);
        foreach (var opcode in new[] { 0U, 10U, uint.MaxValue })
            AssertRejected(WithU32(fixture, 104, opcode));
        foreach (var capability in new[] { 0U, 2U, uint.MaxValue })
            AssertRejected(WithU32(fixture, 108, capability));
        for (var bit = 6; bit < 32; ++bit)
            AssertRejected(WithU32(fixture, 116, 7U | (1U << bit)));
        foreach (var offset in new[] { 136, 140, 144, 148, 152, 156 })
            foreach (var bits in new[] { 0x7F800000U, 0xFF800000U, 0x7FC00001U, 0x7F800001U })
                AssertRejected(WithU32(fixture, offset, bits));
        AssertRejected(WithU32(fixture, 148, BitConverter.SingleToUInt32Bits(-float.Epsilon)));
        AssertRejected(WithU32(fixture, 152, BitConverter.SingleToUInt32Bits(-float.Epsilon)));
        foreach (var opcode in new[] { AISyncTransactionOpcode.Oracle, AISyncTransactionOpcode.CoverageLost })
        {
            var malformed = WithU32(fixture, 104, (uint)opcode);
            AssertRejected(malformed); // observations require the explicit flag
        }
        AssertInvalidModels();
        return Task.CompletedTask;
    }

    internal static Task LimitsAsync()
    {
        var canonical = CanonicalValue();
        // Bounds are the portable schema's bounds, not the old gameplay codec's
        // class whitelist, coordinate budgets or limits on native diagnostics.
        var maximum = canonical with
        {
            AuthorityEpoch = ulong.MaxValue, WorldEpoch = ulong.MaxValue,
            ConsumerEpoch = ulong.MaxValue, OwnerEpoch = ulong.MaxValue,
            EntityA = uint.MaxValue, EntityB = uint.MaxValue, Birth = uint.MaxValue,
            CharacterClass = uint.MaxValue, CommandId = ulong.MaxValue,
            ParentCommandId = ulong.MaxValue - 1, GlobalSequence = ulong.MaxValue,
            OwnerSequence = ulong.MaxValue, ParameterRevision = uint.MaxValue,
            NativeKind = uint.MaxValue, Gait = uint.MaxValue, Mode = uint.MaxValue,
            NativeStatus = uint.MaxValue, GoalX = float.MaxValue, GoalY = -float.MaxValue,
            GoalZ = float.Epsilon, Distance = float.MaxValue, Stop = float.MaxValue,
            Aux = -float.MaxValue
        };
        AssertRoundTrip(maximum);
        var minimum = canonical with
        {
            Session = new Guid("00000000-0000-0000-0000-000000000001"),
            AuthorityEpoch = 1, WorldEpoch = 1, ConsumerEpoch = 1, OwnerEpoch = 1,
            EntityA = 1, EntityB = 0, Birth = 1, CharacterClass = 1, CommandId = 1,
            ParentCommandId = 0, GlobalSequence = 1, OwnerSequence = 1,
            ParameterRevision = 1, Flags = AISyncTransactionFlags.None,
            NativeKind = 0, Gait = 0, Mode = 0, NativeStatus = 0,
            GoalX = 0, GoalY = 0, GoalZ = 0, Distance = 0, Stop = 0, Aux = 0
        };
        AssertRoundTrip(minimum);
        AssertRoundTrip(minimum with { EntityA = 0, EntityB = 1 });
        foreach (var opcode in Enum.GetValues<AISyncTransactionOpcode>())
            for (uint flags = 0; flags <= 63; ++flags)
            {
                var observation = opcode is AISyncTransactionOpcode.Oracle or AISyncTransactionOpcode.CoverageLost;
                var value = canonical with
                {
                    Opcode = opcode,
                    Flags = (AISyncTransactionFlags)flags | (observation ? AISyncTransactionFlags.ObservationOnly : 0),
                    CommandId = observation ? 0UL : canonical.CommandId,
                    ParameterRevision = observation ? 0U : 1U
                };
                AssertRoundTrip(value);
            }
        AssertRoundTrip(canonical with { Opcode = AISyncTransactionOpcode.Oracle,
            Flags = AISyncTransactionFlags.ObservationOnly }); // attributed observations also valid
        var negativeZero = BitConverter.UInt32BitsToSingle(0x80000000U);
        var signedZero = AISyncTransactionCodec.Encode(minimum with
        {
            GoalX = negativeZero, GoalY = negativeZero, GoalZ = negativeZero,
            Distance = negativeZero, Stop = negativeZero, Aux = negativeZero
        });
        AssertEx.SequenceEqual(signedZero, AISyncTransactionCodec.Encode(AISyncTransactionCodec.Decode(signedZero)));
        for (var offset = 136; offset < 160; offset += 4)
            AssertEx.Equal(0x80000000U, BinaryPrimitives.ReadUInt32LittleEndian(signedZero.AsSpan(offset)));
        foreach (var length in new[] { 0, 159, 161, 320 })
        {
            var destination = Enumerable.Repeat((byte)0xCD, length).ToArray();
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Encode(canonical, destination));
            AssertEx.True(destination.All(value => value == 0xCD));
        }
        var unchanged = Enumerable.Repeat((byte)0xCD, 160).ToArray();
        AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Encode(default, unchanged));
        AssertEx.True(unchanged.All(value => value == 0xCD));
        return Task.CompletedTask;
    }

    internal static Task GeneratedRoundTripsAsync()
    {
        // Fixed xorshift seed makes the corpus reproducible across runtime versions.
        // Random finite IEEE bit patterns include subnormals and extreme exponents.
        uint state = 0xA17C2531U;
        Span<byte> sessionBytes = stackalloc byte[16];
        Span<byte> encoded = stackalloc byte[160];
        for (var index = 0; index < 10_000; ++index)
        {
            for (var offset = 0; offset < 16; offset += 4)
                BinaryPrimitives.WriteUInt32LittleEndian(sessionBytes[offset..], Next(ref state));
            var opcode = (AISyncTransactionOpcode)(1 + index % 9);
            var observation = opcode is AISyncTransactionOpcode.Oracle or AISyncTransactionOpcode.CoverageLost;
            var value = new AISyncTransaction
            {
                Session = new Guid(sessionBytes), AuthorityEpoch = Next64(ref state),
                WorldEpoch = Next64(ref state), ConsumerEpoch = Next64(ref state),
                OwnerEpoch = Next64(ref state), EntityA = Next(ref state), EntityB = Next(ref state),
                Birth = Next(ref state), CharacterClass = Next(ref state),
                CommandId = observation && index % 2 == 0 ? 0 : Next64(ref state),
                ParentCommandId = index % 3 == 0 ? 0 : Next64(ref state),
                GlobalSequence = Next64(ref state), OwnerSequence = Next64(ref state),
                Opcode = opcode, Capability = AISyncTransactionCapability.Movement,
                ParameterRevision = observation && index % 2 == 0 ? 0 : Next(ref state),
                Flags = (AISyncTransactionFlags)(Next(ref state) & 63)
                    | (observation ? AISyncTransactionFlags.ObservationOnly : 0),
                NativeKind = Next(ref state), Gait = Next(ref state), Mode = Next(ref state),
                NativeStatus = Next(ref state), GoalX = NextFloat(ref state),
                GoalY = NextFloat(ref state), GoalZ = NextFloat(ref state),
                Distance = MathF.Abs(NextFloat(ref state)), Stop = MathF.Abs(NextFloat(ref state)),
                Aux = NextFloat(ref state)
            };
            AISyncTransactionCodec.Encode(value, encoded);
            var decoded = AISyncTransactionCodec.Decode(encoded);
            AssertEx.Equal(value, decoded, $"Generated record {index} differs.");
            AssertEx.SequenceEqual(encoded.ToArray(), AISyncTransactionCodec.Encode(decoded));
        }
        return Task.CompletedTask;
    }

    private static void AssertInvalidModels()
    {
        var valid = CanonicalValue();
        var invalid = new[]
        {
            default, valid with { Session = Guid.Empty }, valid with { AuthorityEpoch = 0 },
            valid with { WorldEpoch = 0 }, valid with { ConsumerEpoch = 0 }, valid with { OwnerEpoch = 0 },
            valid with { EntityA = 0, EntityB = 0 }, valid with { Birth = 0 },
            valid with { CharacterClass = 0 }, valid with { CommandId = 0 },
            valid with { ParentCommandId = valid.CommandId },
            valid with { GlobalSequence = 0 }, valid with { OwnerSequence = 0 },
            valid with { ParameterRevision = 0 }, valid with { Opcode = 0 },
            valid with { Opcode = (AISyncTransactionOpcode)10 }, valid with { Capability = 0 },
            valid with { Flags = (AISyncTransactionFlags)64 }, valid with { GoalX = float.NaN },
            valid with { GoalY = float.PositiveInfinity }, valid with { GoalZ = float.NegativeInfinity },
            valid with { Distance = -1 }, valid with { Stop = -1 }, valid with { Aux = float.NaN },
            valid with { Opcode = AISyncTransactionOpcode.Oracle },
            valid with { Opcode = AISyncTransactionOpcode.CoverageLost }
        };
        foreach (var value in invalid)
        {
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Validate(value));
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Encode(value));
            var destination = Enumerable.Repeat((byte)0xCD, 160).ToArray();
            AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Encode(value, destination));
            AssertEx.True(destination.All(item => item == 0xCD));
        }
    }

    private static void AssertRejected(byte[] bytes)
    {
        AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Decode(bytes));
        AssertEx.Throws<ProtocolException>(() => AISyncTransactionCodec.Validate(bytes));
    }

    private static void AssertRoundTrip(AISyncTransaction value)
    {
        var bytes = AISyncTransactionCodec.Encode(value);
        AISyncTransactionCodec.Validate(bytes);
        AssertEx.Equal(value, AISyncTransactionCodec.Decode(bytes));
        AssertEx.SequenceEqual(bytes, AISyncTransactionCodec.Encode(AISyncTransactionCodec.Decode(bytes)));
    }

    private static byte[] WithU32(byte[] fixture, int offset, uint value)
    {
        var bytes = fixture.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        return bytes;
    }

    private static uint Next(ref uint state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    private static ulong Next64(ref uint state) => ((ulong)Next(ref state) << 32) | Next(ref state);

    private static float NextFloat(ref uint state)
    {
        var value = BitConverter.UInt32BitsToSingle(Next(ref state));
        return float.IsFinite(value) ? value : 0;
    }

    private static byte[] ReadFixture()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Directory.GetCurrentDirectory() })
            for (var current = new DirectoryInfo(start); current is not null; current = current.Parent)
            {
                var path = Path.Combine(current.FullName, "tests", "fixtures", "aisync-transaction-v1.hex");
                if (!File.Exists(path)) continue;
                var hex = string.Concat(File.ReadAllText(path).Where(value => !char.IsWhiteSpace(value)));
                return Convert.FromHexString(hex);
            }
        throw new FileNotFoundException("Shared tests/fixtures/aisync-transaction-v1.hex was not found.");
    }

    private static AISyncTransaction CanonicalValue() => new()
    {
        Session = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"),
        AuthorityEpoch = 0x0102030405060708UL, WorldEpoch = 0x1112131415161718UL,
        ConsumerEpoch = 0x2122232425262728UL, OwnerEpoch = 0x3132333435363738UL,
        EntityA = 0x41424344U, EntityB = 0x51525354U, Birth = 0x61626364U, CharacterClass = 2,
        CommandId = 0x7172737475767778UL, ParentCommandId = 0,
        GlobalSequence = 0x8182838485868788UL, OwnerSequence = 0x9192939495969798UL,
        Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
        ParameterRevision = 1, Flags = AISyncTransactionFlags.HasGoal | AISyncTransactionFlags.HasGait
            | AISyncTransactionFlags.HasStop, NativeKind = 1, Gait = 2, Mode = 0, NativeStatus = 0,
        GoalX = 1.25F, GoalY = -2.5F, GoalZ = 3.75F, Distance = 4.5F, Stop = 0.5F, Aux = -0.25F
    };
}
