using AIMP.Protocol;

namespace AIMP.Launcher;

// WM09 source-controlled primitive semantics. Entries are keyed by stable
// FunctionType ShortGuid, never by level entity/GUID. The catalog is
// deliberately small and conservative: absence means SemanticUnknown and can
// never authorize native takeover by itself.
//
// A type is catalogued only when the supplied retail AI.exe/AI.exe.c plus the
// existing static reverse-engineering in this source tree proves the relevant
// native behavior. PrimitiveName text is never used to make an admission
// decision.
internal static class WorldReplicationSemanticCatalog
{
    internal enum Effect : byte
    {
        Unknown = 0,
        None,
        InternalOnly,
        Shared
    }

    internal enum Routing : byte
    {
        Unknown = 0,
        Direct,
        Forwarding,
        FanOut,
        Decision,
        Query,
        Scheduler
    }

    internal readonly record struct Semantics(
        Effect Effect,
        WorldReplicationSemanticClass SemanticClass,
        WorldReplicationContractKind ContractKind,
        Routing Routing,
        bool ReplayHistorySensitive,
        bool NativeHandlerCertified,
        bool EffectBeforeOutput,
        bool TakeoverCertified,
        bool ExactInputCertified);

    private static readonly Semantics RoutingOnly = new(
        Effect.None, WorldReplicationSemanticClass.RoutingOnly,
        WorldReplicationContractKind.Unknown,
        Routing.Forwarding, false, true, false, true, false);

    // Exact retail LogicSwitch input semantics. AI.exe/AI.exe.c proves that
    // the generic CATHODE set_true/set_false inputs dispatch through vtable
    // +0x39C/+0x3A0 to FUN_005B4D20/FUN_005B4D60. Those handlers are
    // state-conditioned but convergent: set_true requests the final true state
    // and set_false requests the final false state. They emit the transition
    // edge only when the old state differs, then commit the requested state via
    // vtable+0x44/FUN_005B4DD0 and the normal CATHODE state registry path.
    //
    // The complete operation is therefore the exact input, not the later
    // EmittedOutput and not a direct byte write. Only these two certified
    // inputs are takeover-safe; other LogicSwitch inputs remain Unknown.
    internal const uint LogicSwitchPrimitiveType = 0x0E423FCCU;
    internal const uint LogicSwitchSetTrueInputShortGuid = 0x5EB0E648U;
    internal const uint LogicSwitchSetFalseInputShortGuid = 0xD2B3CC2BU;

    private static readonly Semantics LogicSwitchConvergentStateInput = new(
        Effect.Shared, WorldReplicationSemanticClass.SharedWorldMutation,
        WorldReplicationContractKind.StateTransition,
        Routing.Direct, false, true, false, true, true);

    // Exact supplied AI.exe (SHA-256 00d6...8364) / AI.exe.c proves the
    // PLAY_ENVIRONMENT_ANIMATION generated class constructor at FUN_0047AE20
    // installs vtable RVA 0x00DC8AC0; the current strict BuildProfile retains
    // that constructor-proven vtable. The retained v12.7 native implementation
    // additionally certifies parameterless edges into this exact runtime class
    // as executable animated-world consequences replayable through retail
    // CATHODE rather than transform writes. This is a family-level certificate,
    // not a level/name heuristic. Exact output timing is deliberately not used.
    private static readonly Semantics PlayEnvironmentAnimation = new(
        Effect.Shared, WorldReplicationSemanticClass.SharedWorldMutation,
        WorldReplicationContractKind.Event,
        Routing.Direct, false, true, false, true, false);

    // Stable FunctionType ShortGuids, not level entity IDs. These values occur
    // as FunctionEntity primitive types in the supplied COMMANDS graph.
    // Everything not listed remains Unknown even when its human-readable name
    // sounds like a timer, trigger, physics operation, door or gameplay sink.
    private static readonly IReadOnlyDictionary<(uint PrimitiveType, uint InputShortGuid), Semantics>
        ByExactInput = new Dictionary<(uint PrimitiveType, uint InputShortGuid), Semantics>
        {
            [(LogicSwitchPrimitiveType, LogicSwitchSetTrueInputShortGuid)] = LogicSwitchConvergentStateInput,
            [(LogicSwitchPrimitiveType, LogicSwitchSetFalseInputShortGuid)] = LogicSwitchConvergentStateInput,
        };

    private static readonly IReadOnlyDictionary<uint, Semantics> ByPrimitiveType =
        new Dictionary<uint, Semantics>
        {
            [0x5C9C840AU] = PlayEnvironmentAnimation,
        };

    internal static Semantics Classify(WorldEntityDescriptor descriptor, uint inputShortGuid)
    {
        // Composite public ports, aliases and proxies are structural graph
        // carriers. Their manifest objects do not themselves apply gameplay.
        if (descriptor.Kind == WorldEntityKind.CompositeInstance)
            return new Semantics(Effect.None, WorldReplicationSemanticClass.CompositeRouting,
                WorldReplicationContractKind.Unknown,
                Routing.Forwarding, false, true, false, true, false);
        if (descriptor.Kind is WorldEntityKind.Proxy or WorldEntityKind.Alias)
            return RoutingOnly;

        if (descriptor.Kind != WorldEntityKind.Function || descriptor.PrimitiveType == 0U)
            return Unknown();

        if (ByExactInput.TryGetValue((descriptor.PrimitiveType, inputShortGuid), out var exact))
            return exact;

        return ByPrimitiveType.TryGetValue(descriptor.PrimitiveType, out var value)
            ? value
            : Unknown();
    }

    internal static Semantics Unknown() => new(
        Effect.Unknown,
        WorldReplicationSemanticClass.Unknown,
        WorldReplicationContractKind.Unknown,
        Routing.Unknown,
        false,
        false,
        false,
        false,
        false);
}
