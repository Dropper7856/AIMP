using System.Buffers.Binary;

namespace AIMP.Protocol.Tests;

internal static class AISyncCodecTests
{
    internal static Task AuthorityStateRoundTripAsync()
    {
        var session = Guid.NewGuid();
        var native = ValidState(session);
        AIMP.Protocol.AISyncCodec.ValidateNative(native);
        AssertEx.True(AIMP.Protocol.AISyncCodec.BelongsTo(native, session, 7));
        var network = AIMP.Protocol.AISyncCodec.EncodeNetwork(session, native);
        var decoded = AIMP.Protocol.AISyncCodec.DecodeNetwork(network, session);
        AssertEx.SequenceEqual(native, decoded.ToArray());
        ValidateRejectedStates(native);
        return Task.CompletedTask;
    }

    private static void ValidateRejectedStates(byte[] valid)
    {
        var movingWithoutPose = valid.ToArray();
        U32(movingWithoutPose, 40, 2U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(movingWithoutPose));
        var actionWithoutIdentity = valid.ToArray();
        U32(actionWithoutIdentity, 72, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(actionWithoutIdentity));
        var interactionWithoutIdentity = valid.ToArray();
        U32(interactionWithoutIdentity, 92, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(interactionWithoutIdentity));
        var playerFamily = valid.ToArray();
        U32(playerFamily, 28, 2U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(playerFamily));
    }

    private static byte[] ValidState(Guid session)
    {
        var bytes = new byte[AIMP.Protocol.AISyncCodec.NativeStateBytes];
        U32(bytes, 0, AIMP.Protocol.AISyncCodec.StateVersion);
        U32(bytes, 4, 1U);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(8),
            AIMP.Protocol.AISyncCodec.SessionCookie(session));
        U32(bytes, 16, 7U); U32(bytes, 20, 11U); U32(bytes, 28, 1U);
        U32(bytes, 32, 1U); U32(bytes, 36, 1U);
        U32(bytes, 40, 13U); U32(bytes, 68, 1U);
        U32(bytes, 72, 0x00EB5D80U); U32(bytes, 76, 42U);
        U32(bytes, 80, 1U); U32(bytes, 84, 2U); U32(bytes, 88, 3U);
        U32(bytes, 92, 1U); U32(bytes, 96, 77U); U32(bytes, 104, 7U);
        U32(bytes, 108, 1U); U32(bytes, 124, uint.MaxValue);
        return bytes;
    }

    private static void U32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);
}
