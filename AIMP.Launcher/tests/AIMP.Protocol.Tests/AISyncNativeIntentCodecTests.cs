using System.Buffers.Binary;
using AIMP.Protocol;

namespace AIMP.Protocol.Tests;

internal static class AISyncNativeIntentCodecTests
{
    internal static Task RunAllAsync()
    {
        PayloadDomain(); EnvelopesAndReceipts(); return Task.CompletedTask;
    }
    private static void Check(bool ok) { if (!ok) throw new InvalidOperationException("Native intent codec assertion failed."); }
    private static void Reject(Action action)
    {
        try { action(); } catch (ProtocolException) { return; }
        throw new InvalidOperationException("Invalid native intent accepted.");
    }
    private static uint F(float value) => BitConverter.SingleToUInt32Bits(value);
    private static uint[] Words(uint id)
    {
        var w = new uint[36]; w[0] = id;
        if (id is 8 or 10) { w[1] = 1; w[2] = 0xFEDCBA98; }
        if (id is 9 or 12) { w[1] = 0x11; w[2] = 0x89ABCDEF; w[3] = 0xFEDCBA98; }
        if (id == 13) { w[1] = 1; w[2] = F(-0F); }
        if (id is >= 15 and <= 17) { w[1] = 2; w[4] = 0x11223344; w[6] = 0x55667788; }
        if (id == 19) { w[1] = 4; w[8] = F(-1.5F); w[11] = F(1F); }
        if (id == 20)
        {
            w[21] = 0x11223344; w[22] = 0x55667788; w[23] = w[24] = w[25] = uint.MaxValue;
            w[26] = F(64); w[27] = F(60); w[29] = uint.MaxValue; w[30] = F(-1);
            w[31] = F(60); w[32] = F(-100000); w[33] = 0x0302BE7F;
        }
        return w;
    }
    private static AISyncTransaction Record() => new()
    {
        Session = new Guid("00000001-0000-0000-0000-000000000000"), AuthorityEpoch = 2, WorldEpoch = 3, ConsumerEpoch = 4,
        EntityA = 1, EntityB = 2, Birth = 1, CharacterClass = 1, OwnerEpoch = 1,
        Capability = AISyncTransactionCapability.Movement, CommandId = 1, GlobalSequence = 1, OwnerSequence = 1,
        ParameterRevision = 1, Opcode = AISyncTransactionOpcode.Begin, NativeKind = AISyncNativeIntentCodec.Kind,
        NativeAdmission = 1, NativeIntent = new(Words(9))
    };
    private static AISyncExecutionScope Scope(AISyncTransaction r) => new(r.Session, r.AuthorityEpoch, r.WorldEpoch, r.ConsumerEpoch);
    private static void PayloadDomain()
    {
        Check(AISyncNativeIntentCodec.Decode(new byte[144]) is null);
        for (uint id = 1; id <= 20; ++id)
        {
            if (id is 11 or 14 or 18) { Reject(() => _ = new AISyncNativeIntent(Words(id))); continue; }
            var words = Words(id); var p = new AISyncNativeIntent(words);
            var copy = new AISyncNativeIntent(words);
            Check(p == copy && p.Equals(copy) && p.GetHashCode() == copy.GetHashCode());
            words[0] = 0; Check(p.ContractId == id);
            var bytes = AISyncNativeIntentCodec.Encode(p);
            Check(bytes.Length == 144 && p == AISyncNativeIntentCodec.Decode(bytes));
            for (var n = 0; n < bytes.Length; ++n) { var length = n; Reject(() => AISyncNativeIntentCodec.Decode(bytes.AsSpan(0, length))); }
            words = Words(id); words[35] = 1; Reject(() => _ = new AISyncNativeIntent(words));
        }
        var little = AISyncNativeIntentCodec.Encode(new(Words(9)));
        Check(little.AsSpan(8, 4).SequenceEqual(new byte[] { 0xEF, 0xCD, 0xAB, 0x89 }));
        foreach (var first in new uint[] { 0, 1, uint.MaxValue }) foreach (var second in new uint[] { 0, 1, uint.MaxValue })
        {
            var words = Words(12); words[2] = first; words[3] = second;
            var selectors = new AISyncNativeIntent(words);
            Check(AISyncNativeIntentCodec.Decode(AISyncNativeIntentCodec.Encode(selectors)) == selectors);
            words[1] = 0x22; Reject(() => _ = new AISyncNativeIntent(words));
        }
        foreach (var id in new uint[] { 15, 16, 17 })
        {
            var words = Words(id); words[4] = 0; _ = new AISyncNativeIntent(words);
            words[5] = 1; Reject(() => _ = new AISyncNativeIntent(words));
            words = Words(id); words[1] = 5; Reject(() => _ = new AISyncNativeIntent(words));
            words[4] = words[6] = 0;
            if (id == 16) _ = new AISyncNativeIntent(words);
            else Reject(() => _ = new AISyncNativeIntent(words));
            words[1] = 2; Reject(() => _ = new AISyncNativeIntent(words));
        }
        foreach (var id in new uint[] { 13, 19 }) foreach (var bits in new uint[] { 0x7F800000, 0xFF800000, 0x7FC00000 })
        {
            var words = Words(id); words[id == 13 ? 2 : 8] = bits; Reject(() => _ = new AISyncNativeIntent(words));
        }
        var referenced = Words(20);
        referenced[12] = 0x11223344; referenced[13] = 0x55667788;
        referenced[18] = 0x99AABBCC; referenced[19] = 0xDDEEFF00; referenced[33] = 0x000D2880;
        referenced[1] = 9;
        var scripted = new AISyncNativeIntent(referenced);
        AssertEx.Equal(scripted, AISyncNativeIntentCodec.Decode(AISyncNativeIntentCodec.Encode(scripted))!);
        for (var at = 12; at < 20; at += 2)
        {
            var pair = Words(20); pair[at] = 0x10203040; pair[at + 1] = 0x50607080;
            var identity = new AISyncNativeIntent(pair);
            AssertEx.Equal(identity, AISyncNativeIntentCodec.Decode(AISyncNativeIntentCodec.Encode(identity))!);
            pair[at] = 0; Reject(() => _ = new AISyncNativeIntent(pair));
        }
        foreach (var index in new[] { 1, 2, 4, 6, 8, 20, 34, 35 })
        {
            var words = Words(20); words[index] = 1; Reject(() => _ = new AISyncNativeIntent(words));
        }
        foreach (var index in new[] { 21, 22 }) { var words = Words(20); words[index] = 0; Reject(() => _ = new AISyncNativeIntent(words)); }
        foreach (var index in new[] { 23, 24, 25 }) { var words = Words(20); words[index] = 0xFFFFFFFE; Reject(() => _ = new AISyncNativeIntent(words)); }
        foreach (var index in new[] { 26, 27, 28, 30, 31, 32 }) { var words = Words(20); words[index] = 0x7FC00000; Reject(() => _ = new AISyncNativeIntent(words)); }
        var timed = Words(20); timed[28] = 0x3F000000; _ = new AISyncNativeIntent(timed);
        var flags = Words(20); flags[33] |= 0x80000000; Reject(() => _ = new AISyncNativeIntent(flags));
    }
    private static void EnvelopesAndReceipts()
    {
        var r = Record(); var scope = Scope(r);
        var scalar = r with { NativeKind = 7, NativeAdmission = 0, NativeIntent = null };
        var batch = new AISyncExecutionBatch(scope, 5, new[] { r, scalar });
        var bytes = AISyncExecutionCodec.EncodeBatch(batch);
        Check(bytes[4] == 3 && bytes.Length == 72 + 2 * AISyncExecutionCodec.NativeIntentRecordBytes);
        var emptyAt = 72 + AISyncExecutionCodec.NativeIntentRecordBytes + AISyncExecutionCodec.ExtendedRecordBytes;
        Check(bytes.AsSpan(emptyAt, 144).ToArray().All(b => b == 0));
        Check(AISyncExecutionCodec.DecodeBatch(bytes).Records.SequenceEqual(batch.Records));
        var invalid = bytes.ToArray(); invalid[emptyAt] = 1; Reject(() => AISyncExecutionCodec.DecodeBatch(invalid));
        for (var n = 0; n < bytes.Length; ++n) { var length = n; Reject(() => AISyncExecutionCodec.DecodeBatch(bytes.AsSpan(0, length))); }
        var receipt = new AISyncExecutionReceipt(scope, 5, r, AISyncExecutionStage.Waiting, AISyncExecutionResult.Ok);
        bytes = AISyncExecutionCodec.EncodeReceipt(receipt);
        Check(bytes[4] == 3 && bytes.Length == AISyncExecutionCodec.MaximumReceiptBytes);
        Check(AISyncExecutionCodec.DecodeReceipt(bytes) == receipt);
        var canonical = AISyncExecutionCodec.EncodeOperation(r);
        bytes[64 + AISyncExecutionCodec.ExtendedRecordBytes + 8] ^= 1;
        var changed = AISyncExecutionCodec.DecodeReceipt(bytes).Operation;
        Check(changed.NativeIntent != r.NativeIntent && !canonical.SequenceEqual(AISyncExecutionCodec.EncodeOperation(changed)));
        foreach (var op in new[] { AISyncTransactionOpcode.Update, AISyncTransactionOpcode.Cancel, AISyncTransactionOpcode.Stop })
            Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { Opcode = op }));
        foreach (var op in new[] { AISyncTransactionOpcode.Complete, AISyncTransactionOpcode.Release })
        {
            Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { Opcode = op }));
            var terminal = r with { Opcode = op, NativeIntent = null };
            AISyncExecutionCodec.ValidateExecutable(terminal);
            Check(AISyncExecutionCodec.EncodeBatch(new(scope, 5, new[] { terminal }))[4] == 3);
        }
        Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { NativeIntent = null }));
        Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { NativeAdmission = 0 }));
        Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { ScalarRetention = 2 }));
        Reject(() => AISyncExecutionCodec.ValidateExecutable(r with { Aux = -0F }));
        Reject(() => AISyncExecutionCodec.ValidateExecutable(scalar with { NativeIntent = r.NativeIntent }));
        bytes = AISyncExecutionCodec.EncodeBatch(new(scope, 5, new[] { scalar }));
        Check(bytes[4] == 1 && AISyncExecutionCodec.DecodeBatch(bytes).Records[0] == scalar);
        scalar = scalar with { NativeAdmission = 1 };
        bytes = AISyncExecutionCodec.EncodeBatch(new(scope, 5, new[] { scalar }));
        Check(bytes[4] == 2 && AISyncExecutionCodec.DecodeBatch(bytes).Records[0] == scalar);
        bytes = AISyncExecutionCodec.EncodeReceipt(receipt with { Operation = scalar });
        Check(bytes[4] == 2 && AISyncExecutionCodec.DecodeReceipt(bytes).Operation == scalar);
        var control = new AISyncExecutionControl(scope, AISyncExecutionControlKind.Offer, 128);
        Check(AISyncExecutionControlCodec.Decode(AISyncExecutionControlCodec.Encode(control)) == control
            && AISyncExecutionControlCodec.ProfileForKind(AISyncNativeIntentCodec.Kind) == 128);
        Check(BinaryPrimitives.ReadUInt32LittleEndian(AISyncNativeIntentCodec.Encode(r.NativeIntent)) == 9);
    }
}
