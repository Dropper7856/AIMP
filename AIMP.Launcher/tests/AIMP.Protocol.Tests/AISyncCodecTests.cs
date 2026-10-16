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
        ValidateNativeConsequences(session);
        ValidateCombatAndScripted(session);
        ValidateNpcPairedOutcome(session);
        ValidateIndependentEventHistory(session);
        return Task.CompletedTask;
    }


    private static void ValidateIndependentEventHistory(Guid session)
    {
        var history = new AIMP.Launcher.AISyncEventHistory();
        var state = ValidState(session);
        AssertEx.True(!history.Observe(state));
        U32(state, 552, 1U); U32(state, 556, 5U); U32(state, 560, 1U); U32(state, 564, 3U);
        state.AsSpan(552, 36).CopyTo(state.AsSpan(596)); U32(state, 592, 1U); U32(state, 632, 10U);
        U32(state, 40, U32(state, 40) | AIMP.Protocol.AISyncCodec.ShotEvent);
        U32(state, 588, 3U);
        AIMP.Protocol.AISyncCodec.ValidateNative(state);
        AssertEx.True(history.Observe(state));
        AssertEx.True(!history.Observe(state));
        U32(state, 36, 100U); // a motion revision cannot replay or acknowledge a hit
        U32(state, 40, U32(state, 40) | AIMP.Protocol.AISyncCodec.ConsequenceEvent);
        U32(state, 356, 1U);
        U32(state, 316, 1U); U32(state, 320, 1U); U32(state, 324, 50U); U32(state, 328, 1U);
        U32(state, 360, 1U); U32(state, 364, 50U); U32(state, 396, 100U); U32(state, 400, 1U);
        state.AsSpan(316, 16).CopyTo(state.AsSpan(404));
        AIMP.Protocol.AISyncCodec.ValidateNative(state);
        AssertEx.True(history.Observe(state));
        AssertEx.True(!history.Observe(state));
        U32(state, 40, U32(state, 40) | AIMP.Protocol.AISyncCodec.NavigationEvent);
        U32(state, 256, 4U);
        U32(state, 260, 1U); U32(state, 284, 1U); U32(state, 288, 7U);
        AIMP.Protocol.AISyncCodec.ValidateNative(state);
        AssertEx.True(history.Observe(state));
        AssertEx.True(!history.Observe(state));
        U32(state, 40, U32(state, 40) | AIMP.Protocol.AISyncCodec.ParticipantConsequenceEvent);
        U32(state, 480, 2U); U32(state, 192, 1U);
        BinaryPrimitives.WriteUInt64LittleEndian(state.AsSpan(472), 5678UL);
        state.AsSpan(356, 64).CopyTo(state.AsSpan(480)); U32(state, 480, 2U);
        U32(state, 112, U32(state, 112) | 16U); U32(state, 196, 1U);
        U32(state, 208, 100U); U32(state, 212, 90U); U32(state, 216, 90U); U32(state, 220, 1U);
        AIMP.Protocol.AISyncCodec.ValidateNative(state);
        AssertEx.True(history.Observe(state)); // both new channels are remembered
        AssertEx.True(!history.Observe(state));
        var old = state.ToArray();
        U32(state, 32, 2U); U32(state, 588, 1U);
        AssertEx.True(history.Observe(state)); // a new NPC birth resets the sequences
        U32(old, 588, 999U);
        AssertEx.True(!history.Observe(old)); // late data cannot cross that birth
        history.Clear(); AssertEx.True(history.Observe(old));
    }

    private static void ValidateNpcPairedOutcome(Guid session)
    {
        var npc = ValidState(session);
        U32(npc, 112, (U32(npc, 112) & ~2U) | 16U);
        npc.AsSpan(172, 8).Clear();
        U32(npc, 544, 7U); U32(npc, 548, 2U);
        U32(npc, 192, 1U); U32(npc, 196, 1U);
        U32(npc, 208, 100U); U32(npc, 212, 90U); U32(npc, 216, 90U); U32(npc, 220, 1U);
        AIMP.Protocol.AISyncCodec.ValidateNative(npc);
        foreach (var offset in new[] { 544, 548 })
        {
            var invalid = npc.ToArray(); U32(invalid, offset, 0U);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(invalid));
        }
        var ambiguous = npc.ToArray();
        BinaryPrimitives.WriteUInt64LittleEndian(ambiguous.AsSpan(172), 1234UL);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(ambiguous));
    }

    private static void ValidateCombatAndScripted(Guid session)
    {
        var holstered = ValidState(session); U32(holstered, 552, 4U);
        AIMP.Protocol.AISyncCodec.ValidateNative(holstered);
        var inactiveShot = holstered.ToArray(); U32(inactiveShot, 596, 4U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(inactiveShot));
        var shot = ValidState(session);
        U32(shot, 552, 2U); U32(shot, 556, 7U); U32(shot, 560, 1U); U32(shot, 564, 3U);
        F32(shot, 568, 5F); F32(shot, 580, 0.1F);
        U32(shot, 588, 9U); U32(shot, 592, 1U);
        shot.AsSpan(552, 36).CopyTo(shot.AsSpan(596)); U32(shot, 632, 10U);
        U32(shot, 40, U32(shot, 40) | AIMP.Protocol.AISyncCodec.ShotEvent);
        AIMP.Protocol.AISyncCodec.ValidateNative(shot);
        AssertEx.Equal((1U, 9U), AIMP.Protocol.AISyncCodec.ShotKey(shot));
        var history = shot.ToArray(); U32(history, 40, U32(history, 40) & ~AIMP.Protocol.AISyncCodec.ShotEvent);
        AssertEx.Equal((1U, 0U), AIMP.Protocol.AISyncCodec.ShotKey(history));
        foreach (var (offset, value) in new (int, uint)[] { (632, 0), (560, 11), (600, 3), (592, 2) })
        {
            var invalid = shot.ToArray(); U32(invalid, offset, value);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(invalid));
        }
        var stance = ValidState(session);
        U32(stance, 256, 3); U32(stance, 260, 1); U32(stance, 280, 2); U32(stance, 284, 5); U32(stance, 288, 7);
        U32(stance, 40, U32(stance, 40) | AIMP.Protocol.AISyncCodec.NavigationEvent);
        AIMP.Protocol.AISyncCodec.ValidateNative(stance);
        AssertEx.Equal((1U, 3U), AIMP.Protocol.AISyncCodec.NavigationKey(stance));
        U32(stance, 288, 1); // a normal goal cannot masquerade as a short event
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(stance));
        var scripted = ValidState(session);
        U32(scripted, 40, U32(scripted, 40) | 32U);
        U32(scripted, 240, 4); U32(scripted, 244, 0x00EADB58); U32(scripted, 248, 0x12); U32(scripted, 252, 3);
        U32(scripted, 420, 1); U32(scripted, 424, 71); U32(scripted, 428, 91);
        U32(scripted, 432, uint.MaxValue); U32(scripted, 436, uint.MaxValue); F32(scripted, 444, 1F);
        AIMP.Protocol.AISyncCodec.ValidateNative(scripted);
        var badScripted = scripted.ToArray(); U32(badScripted, 428, 0);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(badScripted));
        badScripted = scripted.ToArray(); U32(badScripted, 468, 0x80000000);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(badScripted));
    }

    private static void ValidateNativeConsequences(Guid session)
    {
        var state = ValidState(session);
        U32(state, 316, 7); U32(state, 320, 1); U32(state, 324, 0); U32(state, 328, 4);
        BinaryPrimitives.WriteUInt64LittleEndian(state.AsSpan(332), 5678UL);
        U32(state, 356, 3); U32(state, 360, 1); U32(state, 364, 50);
        U32(state, 396, 50); U32(state, 400, 1);
        state.AsSpan(316, 16).CopyTo(state.AsSpan(404));
        U32(state, 40, U32(state, 40) | AIMP.Protocol.AISyncCodec.ConsequenceEvent);
        AIMP.Protocol.AISyncCodec.ValidateNative(state);
        var network = AIMP.Protocol.AISyncCodec.EncodeNetwork(session, state);
        AssertEx.SequenceEqual(state, AIMP.Protocol.AISyncCodec.DecodeNetwork(network, session).ToArray());
        AssertEx.Equal((1U, 3U), AIMP.Protocol.AISyncCodec.DamageKey(state));
        var commit = state.ToArray(); U32(commit, 360, 3); U32(commit, 364, 0);
        AIMP.Protocol.AISyncCodec.ValidateNative(commit);
        U32(commit, 364, 1); // a direct setter cannot pretend to be an inferred hit
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(commit));
        var history = state.ToArray();
        U32(history, 40, U32(history, 40) & ~AIMP.Protocol.AISyncCodec.ConsequenceEvent);
        AssertEx.Equal((1U, 0U), AIMP.Protocol.AISyncCodec.DamageKey(history));
        // A remote-player result uses a peer, never a process-local Character.
        var playerHit = ValidState(session);
        BinaryPrimitives.WriteUInt64LittleEndian(playerHit.AsSpan(472), 5678UL);
        state.AsSpan(356, 64).CopyTo(playerHit.AsSpan(480));
        U32(playerHit, 40, U32(playerHit, 40) | AIMP.Protocol.AISyncCodec.ParticipantConsequenceEvent);
        AIMP.Protocol.AISyncCodec.ValidateNative(playerHit);
        AssertEx.Equal((1U, 3U), AIMP.Protocol.AISyncCodec.ParticipantDamageKey(playerHit));
        foreach (var (offset, value) in new (int, uint)[]
        {
            (320, 0), (328, 1), (324, 65536), (360, 0), (372, 256), (376, 256),
            (396, 0), (404, 8), (408, 0), (340, 1)
        })
        {
            var invalid = state.ToArray(); U32(invalid, offset, value);
            AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(invalid));
        }
        var missingPlayer = playerHit.ToArray(); missingPlayer.AsSpan(472, 8).Clear();
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(() => AIMP.Protocol.AISyncCodec.ValidateNative(missingPlayer));
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
        F32(goal, 276, 1F); U32(goal, 280, 2U); U32(goal, 284, 7U); U32(goal, 288, 1U);
        var network = AIMP.Protocol.AISyncCodec.EncodeNetwork(session, goal);
        AssertEx.Equal(AIMP.Protocol.AISyncCodec.NetworkStateBytes, network.Length);
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
        U32(impact, 0, AIMP.Protocol.AISyncCodec.ImpactVersion);
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

        // v1 sent a process-local runtime ID at offset 44. Its bytes cannot
        // silently be interpreted as an authoritative NPC incarnation.
        var legacyImpact = impact.ToArray();
        U32(legacyImpact, 0, 1U);
        AssertEx.Throws<AIMP.Protocol.ProtocolException>(
            () => AIMP.Protocol.AISyncCodec.ValidateNativeImpact(legacyImpact));

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

    internal static byte[] ValidState(Guid session)
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
