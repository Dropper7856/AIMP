using System.Buffers.Binary;
using System.Security.Cryptography;
using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class AISyncExecutionCodecTests
{
    internal static Task CanonicalFixtureAsync()
    {
        // Independently authored bytes shared with native tests, never generated
        // by Encode. Hash is over decoded binary, not whitespace in the hex file.
        var fixture = ReadFixture();
        AssertEx.Equal(232, fixture.Length);
        AssertEx.Equal("A17B7B924EA57C731112E7BBC0E9B38A5FB2F557F8AF28A04C66FD11588835CF",
            Convert.ToHexString(SHA256.HashData(fixture)));
        AssertEx.SequenceEqual(fixture, AISyncExecutionCodec.EncodeBatch(Batch(Turn())));
        var decoded = AISyncExecutionCodec.DecodeBatch(fixture, Scope());
        AssertEx.Equal(Scope(), decoded.Scope);
        AssertEx.Equal(1UL, decoded.BatchId);
        AssertEx.Equal(Turn(), decoded.Records.Single());
        AssertEx.SequenceEqual(Convert.FromHexString("33221100554477668899AABBCCDDEEFF"), fixture.AsSpan(16, 16).ToArray());
        AssertEx.SequenceEqual(AISyncTransactionCodec.Encode(Turn()), fixture[72..]);
        return Task.CompletedTask;
    }

    internal static Task RoundTripsAsync()
    {
        foreach(var kind in new[]{AISyncExecutionCodec.ForcedRelocateKind,AISyncExecutionCodec.DirectRelocateKind,AISyncExecutionCodec.DirectForcedRelocateKind})
        foreach(var opcode in new[]{AISyncTransactionOpcode.Begin,AISyncTransactionOpcode.Submit})
            RoundTrip(Turn() with { Opcode=opcode,NativeKind=kind,
                NativeAdmission=1,Mode=0,Distance=0,Aux=1.25F,GoalX=-33.439754F,GoalZ=-35.993092F });
        AssertEx.Throws<ProtocolException>(()=>AISyncExecutionCodec.EncodeBatch(Batch(Turn() with {
            Opcode=AISyncTransactionOpcode.Update,NativeKind=AISyncExecutionCodec.ForcedRelocateKind,NativeAdmission=1,Distance=0 })));
        foreach (var opcode in new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit, AISyncTransactionOpcode.Update })
        {
            for (uint mode = 0; mode <= 3; mode++)
            {
                RoundTrip(Turn() with { Opcode = opcode, Mode = mode });
                RoundTrip(Heading() with { Opcode = opcode, Mode = mode, Aux = -6.4F, Distance = 6.4F });
            }
            for (uint mode = 0; mode <= 2; mode++) RoundTrip(Stance() with { Opcode = opcode, Mode = mode });
        }
        foreach (var opcode in new[] { AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop,
            AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
            foreach (var kind in new[] { 4U, 7U })
                RoundTrip(Stance() with { Opcode = opcode, NativeKind = kind, Mode = 0 });
        RoundTrip(Turn() with { GoalX = -100000F, GoalY = 100000F, GoalZ = float.Epsilon, Distance = 0 });
        RoundTrip(Heading() with { Aux = 6.4F });
        var maximum = Batch(Enumerable.Range(1, 16).Select(i => Turn() with
        {
            CommandId = (ulong)i, GlobalSequence = (ulong)i, OwnerSequence = (ulong)i
        }).ToArray());
        var bytes = AISyncExecutionCodec.EncodeBatch(maximum);
        AssertEx.Equal(AISyncExecutionCodec.BatchHeaderBytes + 16 * AISyncTransactionCodec.RecordBytes, bytes.Length);
        AssertEx.SequenceEqual(maximum.Records, AISyncExecutionCodec.DecodeBatch(bytes).Records);
        return Task.CompletedTask;
    }

    internal static Task SchedulerControlAsync()
    {
        const uint profile = (uint)AISyncExecutionProfiles.SchedulerControl;
        AssertEx.Equal(256U, profile);
        AssertEx.Equal(503U, (uint)AISyncExecutionProfiles.All);
        AssertEx.Equal(511U, (uint)AISyncExecutionProfiles.Known);
        foreach (var controlKind in new[] { AISyncExecutionControlKind.Offer, AISyncExecutionControlKind.Ready })
        {
            var control = new AISyncExecutionControl(Scope(), controlKind, profile);
            AssertEx.Equal(control, AISyncExecutionControlCodec.Decode(AISyncExecutionControlCodec.Encode(control)));
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionControlCodec.Encode(control with { Value = 512 }));
        }
        var movement = new AISyncMovementBundle(1, 1, 0, AISyncMovementNavigation.LocalNative, default, default, default);
        var intentWords = new uint[AISyncNativeIntentCodec.WordCount];
        intentWords[0] = 1;
        var intent = new AISyncNativeIntent(intentWords);
        foreach (var kind in new[] { 6U, 11U })
        {
            var record = Stance() with { NativeKind = kind, Mode = 0 };
            AssertEx.Equal(profile, AISyncExecutionControlCodec.ProfileForKind(kind));
            AssertEx.True(AISyncExecutionControlCodec.Allows(profile, record));
            AssertEx.True(AISyncExecutionControlCodec.Allows((uint)AISyncExecutionProfiles.All, record));
            AssertEx.False(AISyncExecutionControlCodec.Allows((uint)AISyncExecutionProfiles.Known & ~profile, record));
            AssertEx.False(AISyncExecutionControlCodec.Allows(0, record));
            foreach (var opcode in new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit, AISyncTransactionOpcode.Update })
            for (uint admission = 0; admission <= 2; admission++)
            for (uint retention = 0; retention <= 3; retention++)
            {
                var operation = record with { Opcode = opcode, NativeAdmission = admission, ScalarRetention = retention };
                RoundTrip(operation);
                var version = admission == 0 && retention == 3 ? AISyncExecutionCodec.Version : AISyncExecutionCodec.MovementVersion;
                var stride = version == AISyncExecutionCodec.Version ? AISyncTransactionCodec.RecordBytes : AISyncExecutionCodec.ExtendedRecordBytes;
                var batch = AISyncExecutionCodec.EncodeBatch(Batch(operation));
                AssertEx.Equal(version, BinaryPrimitives.ReadUInt16LittleEndian(batch.AsSpan(4)));
                AssertEx.Equal(AISyncExecutionCodec.BatchHeaderBytes + stride, batch.Length);
                AssertEx.Equal(stride, AISyncExecutionCodec.EncodeOperation(operation).Length);
                foreach (var stage in Enum.GetValues<AISyncExecutionStage>())
                {
                    var receipt = Receipt() with { Operation = operation, Stage = stage };
                    var bytes = AISyncExecutionCodec.EncodeReceipt(receipt);
                    AssertEx.Equal(version, BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4)));
                    AssertEx.Equal(AISyncExecutionCodec.HeaderBytes + stride + 8, bytes.Length);
                    AssertEx.Equal(receipt, AISyncExecutionCodec.DecodeReceipt(bytes, Scope()));
                }
            }
            foreach (var opcode in new[] { AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop,
                AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
            for (uint admission = 0; admission <= 2; admission++)
            {
                var terminal = record with { Opcode = opcode, NativeAdmission = admission };
                RoundTrip(terminal);
                var receipt = Receipt() with { Operation = terminal, Stage = AISyncExecutionStage.Released };
                AssertEx.Equal(receipt, AISyncExecutionCodec.DecodeReceipt(AISyncExecutionCodec.EncodeReceipt(receipt), Scope()));
                for (uint retention = 0; retention < 3; retention++)
                    AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.ValidateExecutable(terminal with { ScalarRetention = retention }));
            }
            // Every float is unused, including its sign bit: only canonical +0 is accepted.
            foreach (var bits in new[] { 1U, 0x80000001U, 0x80000000U, 0x7F800000U, 0xFF800000U, 0x7FC00001U })
            {
                var value = BitConverter.UInt32BitsToSingle(bits);
                foreach (var dirty in new[] { record with { GoalX = value }, record with { GoalY = value },
                    record with { GoalZ = value }, record with { Distance = value }, record with { Stop = value }, record with { Aux = value } })
                    RejectModel(dirty);
            }
            foreach (var flag in new[] { AISyncTransactionFlags.HasGoal, AISyncTransactionFlags.HasGait, AISyncTransactionFlags.HasStop,
                AISyncTransactionFlags.Accepted, AISyncTransactionFlags.Incomplete, AISyncTransactionFlags.ObservationOnly,
                (AISyncTransactionFlags)0x80000000U })
                RejectModel(record with { Flags = flag });
            foreach (var dirty in new[] { record with { Mode = 1 }, record with { Gait = 1 },
                record with { NativeAdmission = 3 }, record with { ScalarRetention = 4 } }) RejectModel(dirty);
            for (uint status = 1; status <= 6; status++) RejectModel(record with { NativeStatus = status });
            foreach (var opcode in new[] { AISyncTransactionOpcode.Oracle, AISyncTransactionOpcode.CoverageLost })
                RejectModel(record with { Opcode = opcode, Flags = AISyncTransactionFlags.ObservationOnly });
            foreach (var opcode in new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit,
                AISyncTransactionOpcode.Update, AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop,
                AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
            {
                RejectModel(record with { Opcode = opcode, Mode = 1 });
                RejectModel(record with { Opcode = opcode, Flags = AISyncTransactionFlags.HasGoal });
                foreach (var dirty in new[] { record with { Opcode = opcode, Movement = movement },
                    record with { Opcode = opcode, NativeIntent = intent } })
                {
                    AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.ValidateExecutable(dirty));
                    AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeBatch(Batch(dirty)));
                    AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { Operation = dirty }));
                }
            }
            // Exercise received v2 extension validation, which v1 cannot carry.
            var extended = record with { NativeAdmission = 1 };
            var packet = AISyncExecutionCodec.EncodeBatch(Batch(extended));
            var receiptPacket = AISyncExecutionCodec.EncodeReceipt(Receipt() with { Operation = extended });
            RejectBatch(WithU32(packet, 72 + 160, 3));
            RejectBatch(WithU32(packet, 72 + 164, 4));
            RejectReceipt(WithU32(receiptPacket, 64 + 160, 3));
            RejectReceipt(WithU32(receiptPacket, 64 + 164, 4));
            AISyncMovementCodec.Encode(movement).CopyTo(packet, 72 + 168);
            AISyncMovementCodec.Encode(movement).CopyTo(receiptPacket, 64 + 168);
            RejectBatch(packet);
            RejectReceipt(receiptPacket);
            // A well-formed v3 envelope must still reject an intent attached to kind6/11.
            packet = Append(AISyncExecutionCodec.EncodeBatch(Batch(extended)), AISyncNativeIntentCodec.Bytes);
            BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4), AISyncExecutionCodec.NativeIntentVersion);
            BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(8), (uint)packet.Length);
            AISyncNativeIntentCodec.Encode(intent).CopyTo(packet, 72 + AISyncExecutionCodec.ExtendedRecordBytes);
            RejectBatch(packet);
        }
        // The shared vtable does not authorize the pose-bearing kind8 or callback-bearing kind10.
        foreach (var kind in new[] { 8U, 10U })
        {
            AssertEx.Equal(0U, AISyncExecutionControlCodec.ProfileForKind(kind));
            RejectModel(Stance() with { NativeKind = kind, Mode = 0 });
        }
        return Task.CompletedTask;
    }

    internal static Task MalformedPacketsAsync()
    {
        var batch = ReadFixture();
        var receipt = AISyncExecutionCodec.EncodeReceipt(Receipt());
        foreach (var input in new[] { batch, receipt })
        {
            for (var offset = 0; offset < 16; offset++)
            {
                var malformed = input.ToArray();
                malformed[offset] ^= 0x80;
                if (input == batch) RejectBatch(malformed);
                else RejectReceipt(malformed);
            }
            foreach (var offset in new[] { 16, 32, 40, 48, 56 })
            {
                var malformed = input.ToArray();
                malformed.AsSpan(offset, offset == 16 ? 16 : 8).Clear();
                if (input == batch) RejectBatch(malformed);
                else RejectReceipt(malformed);
            }
        }
        foreach (var count in new[] { 0U, 2U, 16U, 17U, uint.MaxValue }) RejectBatch(WithU32(batch, 64, count));
        foreach (var reserved in new[] { 1U, 0x80000000U, uint.MaxValue }) RejectBatch(WithU32(batch, 68, reserved));
        foreach (var kind in new[] { 0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, uint.MaxValue })
            RejectBatch(WithU32(batch, 72 + 120, kind));
        RejectBatch(receipt);
        RejectReceipt(batch);
        foreach (var stage in new[] { 0U, 9U, uint.MaxValue }) RejectReceipt(WithU32(receipt, 224, stage));
        foreach (var result in new[] { 17U, 0x100U, 0x110U, uint.MaxValue }) RejectReceipt(WithU32(receipt, 228, result));
        foreach (var value in new[] { Batch() with { BatchId = 0 }, Batch(),
            Batch(new AISyncTransaction[17]), Batch(Turn()) with { Records = null! } })
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeBatch(value));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { BatchId = 0 }));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { Stage = 0 }));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { Result = (AISyncExecutionResult)17 }));
        return Task.CompletedTask;
    }

    internal static Task AllTruncationsAsync()
    {
        foreach (var packet in new[] { ReadFixture(), AISyncExecutionCodec.EncodeBatch(Batch(
            Enumerable.Repeat(Turn(), 16).ToArray())) })
        {
            for (var length = 0; length < packet.Length; length++) RejectBatch(packet[..length]);
            foreach (var extra in new[] { 1, 160, 1024 * 1024 }) RejectBatch(Append(packet, extra));
        }
        var receipt = AISyncExecutionCodec.EncodeReceipt(Receipt());
        for (var length = 0; length < receipt.Length; length++) RejectReceipt(receipt[..length]);
        foreach (var extra in new[] { 1, 160, 1024 * 1024 }) RejectReceipt(Append(receipt, extra));
        return Task.CompletedTask;
    }

    internal static Task ScopeAndIdentityAsync()
    {
        var scope = Scope();
        var batch = ReadFixture();
        var receipt = AISyncExecutionCodec.EncodeReceipt(Receipt());
        foreach (var wrong in new[]
        {
            scope with { Session = Guid.Parse("00112233-4455-6677-8899-aabbccddee00") },
            scope with { AuthorityEpoch = scope.AuthorityEpoch ^ (1UL << 63) },
            scope with { WorldEpoch = scope.WorldEpoch ^ (1UL << 63) },
            scope with { ConsumerEpoch = scope.ConsumerEpoch ^ (1UL << 63) }
        })
        {
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.RequireCurrentBinding(scope, wrong));
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeBatch(batch, wrong));
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeReceipt(receipt, wrong));
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeBatch(Batch(Turn()) with { Scope = wrong }));
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { Scope = wrong }));
        }
        foreach (var wrong in new[] { scope with { Session = Guid.Empty }, scope with { AuthorityEpoch = 0 },
            scope with { WorldEpoch = 0 }, scope with { ConsumerEpoch = 0 } })
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.RequireCurrentBinding(scope, wrong));
        // Mismatch in either envelope or ANY body, including a later record.
        foreach (var offset in new[] { 16 + 15, 32 + 7, 40 + 7, 48 + 7, 72 + 8 + 15, 72 + 24 + 7, 72 + 32 + 7, 72 + 40 + 7 })
        {
            var malformed = batch.ToArray();
            malformed[offset] ^= 0x80;
            RejectBatch(malformed);
        }
        var two = AISyncExecutionCodec.EncodeBatch(Batch(Turn(), Stance()));
        two[72 + 160 + 40 + 7] ^= 0x80;
        RejectBatch(two);
        var wrongSecond = Stance() with { ConsumerEpoch = scope.ConsumerEpoch + 1 };
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeBatch(Batch(Turn(), wrongSecond)));
        foreach (var invalid in new[]
        {
            Turn() with { OwnerEpoch = 0 }, Turn() with { EntityA = 0, EntityB = 0 },
            Turn() with { Birth = 0 }, Turn() with { CharacterClass = 0 }, Turn() with { CommandId = 0 },
            Turn() with { ParentCommandId = Turn().CommandId }, Turn() with { GlobalSequence = 0 },
            Turn() with { OwnerSequence = 0 }, Turn() with { ParameterRevision = 0 },
            Turn() with { Capability = (AISyncTransactionCapability)2 }, Turn() with { Opcode = 0 }
        }) RejectModel(invalid);
        return Task.CompletedTask;
    }

    internal static Task ObservationAndProfilesAsync()
    {
        foreach (var flag in new[] { AISyncTransactionFlags.Accepted, AISyncTransactionFlags.Incomplete,
            AISyncTransactionFlags.ObservationOnly, AISyncTransactionFlags.HasGait, AISyncTransactionFlags.HasStop,
            (AISyncTransactionFlags)64, (AISyncTransactionFlags)0x80000000 })
            foreach (var opcode in new[] { AISyncTransactionOpcode.Begin, AISyncTransactionOpcode.Submit,
                AISyncTransactionOpcode.Update, AISyncTransactionOpcode.Release })
                RejectModel(Turn() with { Opcode = opcode, Flags = AISyncTransactionFlags.HasGoal | flag });
        foreach (var opcode in new[] { AISyncTransactionOpcode.Oracle, AISyncTransactionOpcode.CoverageLost })
            RejectModel(Turn() with { Opcode = opcode, Flags = AISyncTransactionFlags.ObservationOnly });
        var observation = Turn() with { Flags = AISyncTransactionFlags.HasGoal | AISyncTransactionFlags.Incomplete
            | AISyncTransactionFlags.ObservationOnly };
        var rawObservation = AISyncTransactionCodec.Encode(observation);
        RejectModel(observation);
        AssertEx.SequenceEqual(rawObservation, AISyncTransactionCodec.Encode(observation)); // never strips flags
        foreach (var kind in new[] { 0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, uint.MaxValue })
            RejectModel(Turn() with { NativeKind = kind });
        foreach (var invalid in new[]
        {
            Turn() with { NativeStatus = 1 }, Turn() with { Gait = 1 }, Turn() with { Mode = 4 },
            Turn() with { Distance = -float.Epsilon }, Turn() with { Distance = MathF.BitIncrement(6.4F) },
            Turn() with { Stop = float.Epsilon }, Turn() with { Aux = float.Epsilon },
            Turn() with { GoalX = MathF.BitIncrement(100000F) }, Turn() with { GoalY = -MathF.BitIncrement(100000F) },
            Turn() with { GoalZ = MathF.BitIncrement(100000F) },
            Heading() with { GoalX = float.Epsilon }, Heading() with { GoalY = float.Epsilon }, Heading() with { GoalZ = float.Epsilon },
            Heading() with { Aux = MathF.BitIncrement(6.4F) }, Heading() with { Aux = -MathF.BitIncrement(6.4F) },
            Stance() with { Mode = 3 }, Stance() with { Flags = AISyncTransactionFlags.HasGoal },
            Stance() with { GoalX = float.Epsilon }, Stance() with { GoalY = float.Epsilon }, Stance() with { GoalZ = float.Epsilon },
            Stance() with { Distance = float.Epsilon }, Stance() with { Stop = float.Epsilon }, Stance() with { Aux = float.Epsilon }
        }) RejectModel(invalid);
        foreach (var opcode in new[] { AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop,
            AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
        {
            RejectModel(Turn() with { Opcode = opcode });
            foreach (var dirty in new[] { Stance() with { Mode = 1 }, Stance() with { Flags = AISyncTransactionFlags.HasGoal },
                Stance() with { GoalX = float.Epsilon }, Stance() with { GoalY = float.Epsilon }, Stance() with { GoalZ = float.Epsilon },
                Stance() with { Distance = float.Epsilon }, Stance() with { Stop = float.Epsilon }, Stance() with { Aux = float.Epsilon } })
                RejectModel(dirty with { Opcode = opcode });
        }
        return Task.CompletedTask;
    }

    internal static Task NumericValidationAsync()
    {
        var fixture = ReadFixture();
        foreach (var offset in new[] { 136, 140, 144, 148, 152, 156 })
            foreach (var bits in new[] { 0x7F800000U, 0xFF800000U, 0x7FC00001U, 0x7F800001U })
                RejectBatch(WithU32(fixture, 72 + offset, bits));
        var negativeZero = BitConverter.UInt32BitsToSingle(0x80000000U);
        foreach (var value in new[]
        {
            Turn() with { Stop = negativeZero }, Turn() with { Aux = negativeZero },
            Heading() with { GoalX = negativeZero }, Heading() with { GoalY = negativeZero }, Heading() with { GoalZ = negativeZero },
            Stance() with { GoalX = negativeZero }, Stance() with { GoalY = negativeZero }, Stance() with { GoalZ = negativeZero },
            Stance() with { Distance = negativeZero }, Stance() with { Stop = negativeZero }, Stance() with { Aux = negativeZero }
        }) RejectModel(value);
        return Task.CompletedTask;
    }

    internal static Task ReceiptsAsync()
    {
        foreach (var stage in Enum.GetValues<AISyncExecutionStage>())
            foreach (var result in Enum.GetValues<AISyncExecutionResult>())
            {
                var receipt = Receipt() with { Stage = stage, Result = result };
                var bytes = AISyncExecutionCodec.EncodeReceipt(receipt);
                AssertEx.Equal(232, bytes.Length);
                AssertEx.Equal(receipt, AISyncExecutionCodec.DecodeReceipt(bytes, Scope()));
                AssertEx.SequenceEqual(AISyncTransactionCodec.Encode(receipt.Operation), bytes.AsSpan(64, 160).ToArray());
                AssertEx.Equal((uint)stage, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(224)));
                AssertEx.Equal((uint)result, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(228)));
            }
        var waiting = Receipt() with { Stage = AISyncExecutionStage.Waiting, Result = AISyncExecutionResult.OwnerBusy };
        AssertEx.Equal(AISyncExecutionStage.Waiting,
            AISyncExecutionCodec.DecodeReceipt(AISyncExecutionCodec.EncodeReceipt(waiting)).Stage);
        // Full op echo distinguishes revisions, Submit versus Update, and payloads.
        var original = AISyncExecutionCodec.EncodeReceipt(Receipt());
        foreach (var operation in new[] { Turn() with { Opcode = AISyncTransactionOpcode.Submit },
            Turn() with { ParameterRevision = 2 }, Turn() with { GoalX = 2 }, Turn() with { OwnerSequence = 2 },
            Turn() with { OwnerEpoch = 2 }, Turn() with { CommandId = 2 }, Turn() with { NativeKind = 7, Flags = 0,
                Mode = 0, GoalX = 0, GoalY = 0, GoalZ = 0, Distance = 0 } })
        {
            var bytes = AISyncExecutionCodec.EncodeReceipt(Receipt() with { Operation = operation });
            AssertEx.False(original.SequenceEqual(bytes));
            AssertEx.Equal(operation, AISyncExecutionCodec.DecodeReceipt(bytes).Operation);
        }
        return Task.CompletedTask;
    }

    private static void RejectModel(AISyncTransaction value)
    {
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.ValidateExecutable(value));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeBatch(Batch(value)));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.EncodeReceipt(Receipt() with { Operation = value }));
        // When the base record is legal, also verify the received wire path.
        byte[] record;
        try { record = AISyncTransactionCodec.Encode(value); }
        catch (ProtocolException) { return; }
        var packet = ReadFixture();
        record.CopyTo(packet, 72);
        RejectBatch(packet);
        var receipt = AISyncExecutionCodec.EncodeReceipt(Receipt());
        record.CopyTo(receipt, 64);
        RejectReceipt(receipt);
    }

    private static void RejectBatch(byte[] bytes)
    {
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeBatch(bytes));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeBatch(bytes, Scope()));
    }

    private static void RejectReceipt(byte[] bytes)
    {
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeReceipt(bytes));
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeReceipt(bytes, Scope()));
    }

    private static void RoundTrip(AISyncTransaction record)
    {
        AISyncExecutionCodec.ValidateExecutable(record, Scope());
        var bytes = AISyncExecutionCodec.EncodeBatch(Batch(record));
        var decoded = AISyncExecutionCodec.DecodeBatch(bytes, Scope());
        AssertEx.Equal(record, decoded.Records.Single());
        AssertEx.SequenceEqual(bytes, AISyncExecutionCodec.EncodeBatch(decoded));
    }

    private static byte[] WithU32(byte[] source, int offset, uint value)
    {
        var bytes = source.ToArray();
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
        return bytes;
    }

    private static byte[] Append(byte[] source, int count)
    {
        var bytes = new byte[source.Length + count];
        source.CopyTo(bytes, 0);
        return bytes;
    }

    private static AISyncExecutionScope Scope() => new(Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"),
        0x0102030405060708UL, 0x1112131415161718UL, 0x2122232425262728UL);
    private static AISyncExecutionBatch Batch(params AISyncTransaction[] records) => new(Scope(), 1, records);
    private static AISyncExecutionReceipt Receipt() => new(Scope(), 1, Turn(), AISyncExecutionStage.Received, AISyncExecutionResult.Ok);
    private static AISyncTransaction Turn() => new()
    {
        Session = Scope().Session, AuthorityEpoch = Scope().AuthorityEpoch,
        WorldEpoch = Scope().WorldEpoch, ConsumerEpoch = Scope().ConsumerEpoch,
        OwnerEpoch = 0x3132333435363738UL, EntityA = 0x41424344U, EntityB = 0x51525354U,
        Birth = 0x61626364U, CharacterClass = 2, CommandId = 0x7172737475767778UL,
        GlobalSequence = 0x8182838485868788UL, OwnerSequence = 0x9192939495969798UL,
        Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
        ParameterRevision = 1, Flags = AISyncTransactionFlags.HasGoal, NativeKind = 4, Mode = 3,
        GoalX = 1.25F, GoalY = -2.5F, GoalZ = 3.75F, Distance = 0.5F
    };
    private static AISyncTransaction Heading() => Turn() with { Flags = 0, GoalX = 0, GoalY = 0, GoalZ = 0, Aux = 1.5F };
    private static AISyncTransaction Stance() => Heading() with { NativeKind = 7, Mode = 2, Distance = 0, Aux = 0 };

    private static byte[] ReadFixture()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Directory.GetCurrentDirectory() })
            for (var current = new DirectoryInfo(start); current is not null; current = current.Parent)
            {
                var path = Path.Combine(current.FullName, "tests", "fixtures", "aisync-execution-v1.hex");
                if (File.Exists(path)) return Convert.FromHexString(string.Concat(
                    File.ReadAllText(path).Where(value => !char.IsWhiteSpace(value))));
            }
        throw new FileNotFoundException("Shared tests/fixtures/aisync-execution-v1.hex was not found.");
    }
}
