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
        ValidateExecutionReplayCarrier(session);
        ValidateNavigationCarrier(session);
        ValidateNpcFamilies(session);
        ValidateImpactRoundTrip(session);
        return Task.CompletedTask;
    }


    private static void ValidateExecutionReplayCarrier(Guid session)
    {
        var execution = ValidState(session);
        U32(execution, 40, U32(execution, 40) | (1U << 5));
        U32(execution, 240, 1U);
        U32(execution, 244, 0x00EABE00U);
        U32(execution, 248, 0x37U);
        U32(execution, 252, 3U); // transition + certified
        AIMP.Protocol.AISyncCodec.ValidateNative(execution);

        var missingIdentity = ValidState(session);
        U32(missingIdentity, 40, U32(missingIdentity, 40) | (1U << 5));
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(missingIdentity));

        var flagsWithoutAuthority = ValidState(session);
        U32(flagsWithoutAuthority, 252, 1U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(flagsWithoutAuthority));

        var certifiedWithoutTransition = execution.ToArray();
        U32(certifiedWithoutTransition, 252, 2U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(certifiedWithoutTransition));
    }

    private static void ValidateNavigationCarrier(Guid session)
    {
        var goal = ValidState(session);
        U32(goal, 256, 9U); U32(goal, 260, 1U);
        F32(goal, 264, 2F); F32(goal, 268, 3F); F32(goal, 272, -4F);
        F32(goal, 276, 1F); U32(goal, 280, 2U); U32(goal, 284, 7U);
        var network = AIMP.Protocol.AISyncCodec.EncodeNetwork(session, goal);
        AssertEx.Equal(304, network.Length);
        AssertEx.SequenceEqual(goal, AIMP.Protocol.AISyncCodec.DecodeNetwork(network, session).ToArray());
        // A moving target can update its goal within the same native request.
        U32(goal, 256, 10U); F32(goal, 264, 5F);
        AIMP.Protocol.AISyncCodec.ValidateNative(goal);
        foreach (var (offset, value) in new (int, uint)[]
        {
            (256, 0U), (284, 0U), (260, 2U), (280, 4U)
        })
        {
            var invalid = goal.ToArray(); U32(invalid, offset, value);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(invalid));
        }
        foreach (var (offset, value) in new (int, float)[]
        {
            (264, float.NaN), (268, float.PositiveInfinity), (272, 100001F),
            (276, -2F), (276, 1001F), (276, float.NaN)
        })
        {
            var invalid = goal.ToArray(); F32(invalid, offset, value);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(invalid));
        }
        var legacy = goal.AsSpan(0, 256).ToArray(); U32(legacy, 0, 10U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(legacy));
    }

    private static void ValidateImpactRoundTrip(Guid session)
    {
        var impact = new byte[AIMP.Protocol.AISyncCodec.NativeImpactBytes];
        U32(impact, 0, 1U);
        U32(impact, 4, 9U);
        BinaryPrimitives.WriteUInt64LittleEndian(impact.AsSpan(8),
            AIMP.Protocol.AISyncCodec.SessionCookie(session));
        U32(impact, 16, 7U);
        U32(impact, 20, 41U);
        BinaryPrimitives.WriteUInt64LittleEndian(impact.AsSpan(24), 1234UL);
        U32(impact, 32, 11U);
        U32(impact, 36, 22U);
        U32(impact, 40, 1U);
        U32(impact, 44, 3U);
        U32(impact, 48, 25U);
        U32(impact, 52, 0x16U);
        U32(impact, 56, 1U);
        AIMP.Protocol.AISyncCodec.ValidateNativeImpact(impact);
        AssertEx.True(AIMP.Protocol.AISyncCodec.ImpactBelongsTo(impact, session, 7));
        AssertEx.Equal(1234UL, AIMP.Protocol.AISyncCodec.ImpactSourcePeer(impact));
        AssertEx.Equal(41U, AIMP.Protocol.AISyncCodec.ImpactSequence(impact));

        var network = AIMP.Protocol.AISyncCodec.EncodeImpactNetwork(session, impact);
        var decoded = AIMP.Protocol.AISyncCodec.DecodeImpactNetwork(network, session);
        foreach (var family in new uint[] { 2, 7 })
        {
            var androidImpact = impact.ToArray(); U32(androidImpact, 40, family);
            var androidNetwork = AIMP.Protocol.AISyncCodec.EncodeImpactNetwork(session, androidImpact);
            AssertEx.SequenceEqual(androidImpact,
                AIMP.Protocol.AISyncCodec.DecodeImpactNetwork(androidNetwork, session).ToArray());
        }
        AssertEx.SequenceEqual(impact, decoded.ToArray());

        var badDamage = impact.ToArray();
        U32(badDamage, 48, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNativeImpact(badDamage));
    }

    private static void ValidateNpcFamilies(Guid session)
    {
        foreach (var family in new uint[] { 1, 2, 3, 4, 6, 7, 9 })
        {
            var state = ValidState(session); U32(state, 28, family);
            var network = AIMP.Protocol.AISyncCodec.EncodeNetwork(session, state);
            AssertEx.SequenceEqual(state, AIMP.Protocol.AISyncCodec.DecodeNetwork(network, session).ToArray());
        }
        foreach (var family in new uint[] { 0, 5, 8, 10, uint.MaxValue })
        {
            var state = ValidState(session); U32(state, 28, family);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(state));
        }
    }

    private static void ValidateRejectedStates(byte[] valid)
    {
        var movingWithoutPose = valid.ToArray();
        U32(movingWithoutPose, 40, 2U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(movingWithoutPose));

        var movingWithoutIntentRevision = valid.ToArray();
        U32(movingWithoutIntentRevision, 40, 3U);
        U32(movingWithoutIntentRevision, 76, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(movingWithoutIntentRevision));

        var invalidIntentIntensity = valid.ToArray();
        F32(invalidIntentIntensity, 72, 65F);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(invalidIntentIntensity));

        var actionWithoutIdentity = valid.ToArray();
        U32(actionWithoutIdentity, 80, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(actionWithoutIdentity));

        var interactionWithoutIdentity = valid.ToArray();
        U32(interactionWithoutIdentity, 100, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(interactionWithoutIdentity));

        var playerFamily = valid.ToArray();
        U32(playerFamily, 28, 0U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(playerFamily));

        var oldVersion = valid.ToArray();
        U32(oldVersion, 0, 8U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNative(oldVersion));
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

        // Pose + Moving + Action + Interaction + RoleTask.
        U32(bytes, 40, 31U);
        F32(bytes, 60, 1.5F); F32(bytes, 64, -0.5F);
        U32(bytes, 68, 1U); F32(bytes, 72, 1.25F); U32(bytes, 76, 2U);

        U32(bytes, 80, 0x00EB5D80U); U32(bytes, 84, 42U);
        U32(bytes, 88, 1U); U32(bytes, 92, 2U); U32(bytes, 96, 3U);

        U32(bytes, 100, 1U); U32(bytes, 104, 77U); U32(bytes, 112, 7U);
        U32(bytes, 116, 1U); U32(bytes, 132, uint.MaxValue);
        U32(bytes, 148, 1U); U32(bytes, 152, 0x00EF2000U);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(172), 1234UL);
        U32(bytes, 184, 1U); U32(bytes, 188, 1U);
        return bytes;
    }

    private static uint U32(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset));

    private static void U32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(offset), value);

    private static void F32(byte[] bytes, int offset, float value) =>
        U32(bytes, offset, (uint)BitConverter.SingleToInt32Bits(value));
}
