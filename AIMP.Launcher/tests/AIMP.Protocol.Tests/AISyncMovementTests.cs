using System.Buffers.Binary;
using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class AISyncMovementTests
{
    internal static AISyncTransaction Move()
    {
        var p = new AISyncMovementParameters(1, 2, 1.25F, -2.5F, 3.75F, -1, -1,
            0, 0, 0, 0, 50, 0.25F, -0.5F, 8, 1.15F, 0x21, 0x10);
        return new AISyncTransaction
        {
            Session = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"), AuthorityEpoch = 1,
            WorldEpoch = 2, ConsumerEpoch = 3, OwnerEpoch = 1, EntityA = 11, EntityB = 22,
            Birth = 1, CharacterClass = 2, CommandId = 100, GlobalSequence = 1, OwnerSequence = 1,
            Opcode = AISyncTransactionOpcode.Begin, Capability = AISyncTransactionCapability.Movement,
            ParameterRevision = 1, NativeKind = 1,
            Movement = new(1, 2, 3, AISyncMovementNavigation.LocalNative, p,
                p with { Flags90 = 1, Flags91 = 0x11, StopDistance = 0.5F }, p with { Retained = 0 }, 2, 1)
        };
    }

    internal static Task WireAsync()
    {
        var move = Move(); var scope = new AISyncExecutionScope(move.Session, 1, 2, 3);
        var packet = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { move }));
        AssertEx.Equal(484, packet.Length); AssertEx.Equal((ushort)2, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(4)));
        AssertEx.Equal(move, AISyncExecutionCodec.DecodeBatch(packet, scope).Records.Single());
        AssertEx.SequenceEqual(ReadFixture(), packet);
        var receipt = new AISyncExecutionReceipt(scope, 1, move, AISyncExecutionStage.Waiting, AISyncExecutionResult.OwnerBusy);
        var bytes = AISyncExecutionCodec.EncodeReceipt(receipt);
        AssertEx.Equal(AISyncExecutionCodec.MaximumReceiptBytes, bytes.Length); AssertEx.Equal(receipt, AISyncExecutionCodec.DecodeReceipt(bytes, scope));
        foreach (var length in Enumerable.Range(0, packet.Length))
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeBatch(packet[..length]));
        foreach (var length in Enumerable.Range(0, bytes.Length))
            AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeReceipt(bytes[..length]));
        var maximum = new AISyncExecutionBatch(scope, 1, Enumerable.Repeat(move, 16).ToArray());
        AssertEx.Equal(AISyncExecutionCodec.MaxBatchBytes, AISyncExecutionCodec.EncodeBatch(maximum).Length);
        var stop = move with { NativeKind = 3, Movement = move.Movement with { Parts = 1, Companion = default,
            CompanionFlags = 0, CompanionRetainedAfter = 0, PrimaryFlags = 2, After = move.Movement.Before } };
        AssertEx.Equal(stop, AISyncExecutionCodec.DecodeBatch(AISyncExecutionCodec.EncodeBatch(new(scope, 2, new[] { stop }))).Records.Single());
        var stopReceipt = receipt with { Operation = stop };
        AssertEx.Equal(stopReceipt, AISyncExecutionCodec.DecodeReceipt(AISyncExecutionCodec.EncodeReceipt(stopReceipt)));
        foreach (var opcode in new[] { AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop,
            AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
        {
            var terminal = move with { Opcode = opcode, Movement = default };
            AssertEx.Equal(terminal, AISyncExecutionCodec.DecodeBatch(AISyncExecutionCodec.EncodeBatch(
                new(scope, 2, new[] { terminal }))).Records.Single());
            Reject(move with { Opcode = opcode });
        }
        return Task.CompletedTask;
    }

    internal static Task GuardsAsync()
    {
        var move = Move(); var b = move.Movement;
        foreach (var bad in new[] { default, b with { Version = 2 }, b with { Parts = 0 }, b with { Parts = 3 },
            b with { Gait = 4 }, b with { Navigation = 0 }, b with { Parts = 1 },
            b with { Before = b.Before with { Flags90 = 0x40 } }, b with { Before = b.Before with { Flags91 = 2 } },
            b with { After = b.After with { Retained = 2 } }, b with { After = b.After with { Style = 5 } },
            b with { Companion = b.Companion with { X = float.NaN } },
            b with { After = b.After with { X = 100001 } }, b with { Before = b.Before with { StopDistance = -2 } } })
            Reject(move with { Movement = bad });
        Reject(move with { Flags = AISyncTransactionFlags.HasGoal });
        Reject(move with { Movement = b with { PrimaryFlags = 1 } }); Reject(move with { NativeKind = 3 });
        Reject(move with { GoalX = -0F }); Reject(move with { NativeKind = 4 });
        var signedZero = b with { After = b.After with { Query0 = -0F } };
        AssertEx.False(AISyncMovementCodec.Encode(b).SequenceEqual(AISyncMovementCodec.Encode(signedZero)));
        var bytes = AISyncExecutionCodec.EncodeOperation(move);
        var changed = AISyncExecutionCodec.EncodeOperation(move with { Movement = signedZero });
        AssertEx.False(bytes.SequenceEqual(changed));
        var one = b with { Parts = 1, Companion = default, CompanionFlags = 0, CompanionRetainedAfter = 0 };
        AssertEx.Equal(one, AISyncMovementCodec.Decode(AISyncMovementCodec.Encode(one)));
        var scope = new AISyncExecutionScope(move.Session, 1, 2, 3);
        var old = AISyncExecutionCodec.EncodeBatch(new(scope, 1, new[] { move }));
        BinaryPrimitives.WriteUInt16LittleEndian(old.AsSpan(4), 1);
        AssertEx.Throws<ProtocolException>(() => AISyncExecutionCodec.DecodeBatch(old));
        return Task.CompletedTask;
    }
    internal static Task JournalAsync()
    {
        var begin=Move();var scope=new AISyncExecutionScope(begin.Session,1,2,3);
        var submit=begin with { Opcode=AISyncTransactionOpcode.Submit,GlobalSequence=2,OwnerSequence=2 };
        var packet=AISyncExecutionCodec.EncodeBatch(new(scope,1,new[]{begin,submit}));
        var journal=new AISyncExecutionJournal(scope);
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Append(packet));
        AssertEx.True(journal.ContainsOperation(1,submit));
        var changed=submit with { Movement=submit.Movement with { After=submit.Movement.After with { Query5=.75F } } };
        AssertEx.False(journal.ContainsOperation(1,changed));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Receive(new(scope,1,submit,AISyncExecutionStage.Waiting,AISyncExecutionResult.Ok)));
        AssertEx.Equal(AISyncExecutionJournalResult.RecoveryRequired,journal.Receive(new(scope,1,changed,AISyncExecutionStage.Admitted,AISyncExecutionResult.Ok)));
        return Task.CompletedTask;
    }
    internal static Task DirectivesAsync()
    {
        var basis=Move();var scope=new AISyncExecutionScope(basis.Session,1,2,3);
        foreach(var kind in new[]{4U,7U})foreach(var admission in new[]{0U,1U,2U})foreach(var retention in new[]{0U,1U,2U,3U})
        {
            var scalar=basis with { NativeKind=kind,Movement=default,NativeAdmission=admission,ScalarRetention=retention,Mode=2 };
            var bytes=AISyncExecutionCodec.EncodeBatch(new(scope,1,new[]{scalar}));
            AssertEx.Equal(scalar,AISyncExecutionCodec.DecodeBatch(bytes).Records.Single());
            var receipt=new AISyncExecutionReceipt(scope,1,scalar with { Opcode=AISyncTransactionOpcode.Submit },
                AISyncExecutionStage.Superseded,AISyncExecutionResult.Ok);
            AssertEx.Equal(receipt,AISyncExecutionCodec.DecodeReceipt(AISyncExecutionCodec.EncodeReceipt(receipt)));
        }
        Reject(basis with { NativeAdmission=3 });Reject(basis with { ScalarRetention=2 });
        var turn=basis with { NativeKind=4,Movement=default,Mode=2,NativeAdmission=1,ScalarRetention=2 };
        var submit=turn with { Opcode=AISyncTransactionOpcode.Submit,GlobalSequence=2,OwnerSequence=2 };
        var journal=new AISyncExecutionJournal(scope);var packet=AISyncExecutionCodec.EncodeBatch(new(scope,1,new[]{turn,submit}));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Append(packet));
        AssertEx.False(journal.ContainsOperation(1,submit with { NativeAdmission=2 }));
        AssertEx.False(journal.ContainsOperation(1,submit with { ScalarRetention=3 }));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Receive(new(scope,1,submit,AISyncExecutionStage.Waiting,AISyncExecutionResult.Ok)));
        AssertEx.Equal(AISyncExecutionJournalResult.Accepted,journal.Receive(new(scope,1,submit,AISyncExecutionStage.Superseded,AISyncExecutionResult.Ok)));
        return Task.CompletedTask;
    }

    private static void Reject(AISyncTransaction value) => AssertEx.Throws<ProtocolException>(
        () => AISyncExecutionCodec.ValidateExecutable(value));
    private static byte[] ReadFixture()
    {
        for (var d = new DirectoryInfo(Directory.GetCurrentDirectory()); d is not null; d = d.Parent)
        {
            var path = Path.Combine(d.FullName, "tests", "fixtures", "aisync-execution-move-v2.hex");
            if (File.Exists(path)) return Convert.FromHexString(string.Concat(File.ReadAllText(path).Where(c => !char.IsWhiteSpace(c))));
        }
        throw new FileNotFoundException("Movement fixture not found.");
    }
}
