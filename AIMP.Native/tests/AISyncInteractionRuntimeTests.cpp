#include "aimp/AISyncInteractionRuntime.h"
#include "aimp/AISync.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace aimp::ai_sync::interactions {
struct RuntimeTestAccess {
    static NativePairAdapter& Adapter(Runtime& runtime) noexcept { return runtime.adapter_; }
};
struct NativePairAdapterTestAccess {
    static void Prepare(NativePairAdapter& adapter, void* context,
        Check(*binding)(void*, const Command&, const Binding&) noexcept,
        bool(*definition)(void*, const Binding&, const Command&, Definition&) noexcept,
        bool(*registration)(void*, const Binding&, const Definition&, bool&) noexcept,
        bool(*transition)(void*, const Binding&, const Command&) noexcept,
        bool(*snapshot)(void*, const Binding&, PairState&) noexcept,
        bool(*definitionLive)(void*, const Binding&, const Definition&) noexcept) noexcept {
        adapter.primitivesContext_ = context; adapter.bindingPrimitive_ = binding;
        adapter.definitionPrimitive_ = definition; adapter.registerPrimitive_ = registration;
        adapter.transitionPrimitive_ = transition; adapter.snapshotPrimitive_ = snapshot;
        adapter.definitionLivePrimitive_ = definitionLive;
    }
};
}

// Standalone link only. All adapter primitives below are explicitly injected.
// An accidental retail resource lookup fails instead of pretending it worked.
// These definitions are excluded from the production library, and this test
// executable must not also link AISyncAnimation.cpp/AIMP.Core.
namespace aimp::ai_sync::animation {
bool ResolvePairDefinition(std::uintptr_t, std::uint32_t, std::uintptr_t,
    std::uint32_t, std::uint32_t, interactions::Definition&) noexcept { return false; }
bool InspectPair(std::uintptr_t, std::uint32_t, interactions::PairState&) noexcept { return false; }
}

using namespace aimp::ai_sync;
using namespace interactions;
namespace {
void CheckValue(const bool value, const int line) {
    if (!value) { std::cerr << "interaction runtime invariant at " << line << '\n'; std::abort(); }
}
#define CHECK(value) CheckValue((value), __LINE__)
transactions::Scope Scope() {
    transactions::Scope scope{}; scope.session[0] = 99U;
    scope.authorityEpoch = 1U; scope.worldEpoch = 2U; scope.consumerEpoch = 3U;
    return scope;
}
State Carrier() {
    State state{}; state.sessionCookie = 99U; state.worldGeneration = 2U;
    state.interactionSession[0]=99U;state.interactionAuthority=1U;state.interactionCommand=1U;
    state.entityA = 1U; state.entityB = 2U; state.birth = 10U; state.characterClass = 3U;
    state.revision = state.interactionRevision = state.interactionPairRevision = 1U;
    state.interactionTransitionRevision = 2U; state.interactionTransition = 1U;
    state.interactionTransitionVariant = 2;
    state.stateFlags = InteractionValid;
    state.interactionFlags = InteractionActorIsSource | InteractionPairValid |
        InteractionCycleActive | InteractionNativeOutcomeRequired;
    state.interactionTargetA = 3U; state.interactionTargetB = 4U;
    state.interactionTargetBirth = 11U; state.interactionTargetClass = 3U;
    state.outcome.revision = 1U; state.outcome.flags = OutcomePrepared | OutcomeVictimAdmitted;
    state.outcome.hpBefore = state.outcome.hpPrepared = state.outcome.hpReaction = 100U;
    return state;
}
Outcome EvidenceOutcome(const State& state) {
    const auto& o = state.outcome;
    return {o.revision,o.flags,o.definition,o.kind,o.hpBefore,o.hpPrepared,o.hpReaction,o.healthReaction,
        o.sourceYaw,{o.targetOffset[0],o.targetOffset[1],o.targetOffset[2]}};
}
struct Bytes {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    template<class T> void Put(const std::uintptr_t address, const T value) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0U; i < sizeof(T); ++i) bytes[address + i] = data[i];
    }
    static bool Read(void* context, const std::uintptr_t address, void* output, const std::size_t size) noexcept {
        const auto& memory = *static_cast<Bytes*>(context);
        auto* target = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0U; i < size; ++i) {
            const auto found = memory.bytes.find(address + i);
            if (found == memory.bytes.end()) return false;
            target[i] = found->second;
        }
        return true;
    }
};
struct Harness {
    Runtime runtime;
    State state{Carrier()};
    Binding binding{{0x100000U, 15U, 1U, 2U, Perspective::Npc},
        {0x200000U, 16U, 3U, 4U, Perspective::Npc}, 0U, true, true};
    PairState native{};
    Bytes bytes;
    unsigned registrations{}, transitions{}, proofs{};
    bool registerOk{true}, snapshotOk{true}, definitionReady{true}, invalidateOnEntry{};
    Check releaseProof{Check::Waiting};
    const Receipt* published{};
    Harness() {
        native.source = binding.source.character; native.victim = binding.victim.character;
        native.sourceRuntimeId = binding.source.runtimeId; native.victimRuntimeId = binding.victim.runtimeId;
        native.sourceCycle = 0x110000U; native.victimCycle = 0x210000U;
        native.sourceGraph = 0x130000U; native.victimGraph = 0x230000U;
        native.definition = 0x140000U;
        native.sourceSerial = 4U; native.victimSerial = 7U;
        native.sourcePrepared = native.linked = native.victimAcquired = true;
        native.stage = 1U;
        bytes.Put(binding.source.character + 0x50U, binding.source.runtimeId);
        bytes.Put(binding.source.character + 0x36CU, static_cast<std::uint32_t>(native.sourceCycle));
        bytes.Put(native.sourceCycle, 0x400000U + 0xEAE5D0U);
        bytes.Put(native.sourceCycle + 4U, static_cast<std::uint32_t>(binding.source.character));
        bytes.Put(native.sourceCycle + 0x1AU, std::uint8_t{0x4E});
        bytes.Put(native.sourceCycle + 0x19U, native.sourceSerial);
        bytes.Put(native.sourceCycle + 8U, std::uint8_t{});
        bytes.Put(native.sourceCycle + 0x14U, 0x120000U);
        bytes.Put(0x120000U + 0x50U, static_cast<std::uint32_t>(native.sourceGraph));
        CHECK(runtime.Initialize(0x400000U, true, {}, &Permit, this));
        auto& adapter = RuntimeTestAccess::Adapter(runtime);
        NativePairAdapterTestAccess::Prepare(adapter, this, &Permit, &DefinitionCall,
            &Register, &TransitionCall, &Snapshot, &DefinitionLive);
        CHECK(runtime.Configure(Scope(), 99U, 2U) == Result::Admitted);
    }
    static Check Permit(void* context, const Command& command, const Binding&) noexcept {
        const auto& h=*static_cast<Harness*>(context);
        return h.published&&(!h.published->pairEntered||h.published->key!=command.key)?Check::Waiting:Check::Ready;
    }
    static bool DefinitionCall(void* context, const Binding&, const Command& command, Definition& output) noexcept {
        output = {0x180000U, 0x140000U, static_cast<std::uint32_t>(command.sourceDefinition), 0U, 4};
        return static_cast<Harness*>(context)->definitionReady;
    }
    static bool DefinitionLive(void*, const Binding&, const Definition&) noexcept { return true; }
    static bool Register(void* context, const Binding&, const Definition&, bool& accepted) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.registrations; accepted = true;
        CHECK(h.runtime.Configure(Scope(), 99U, 2U) == Result::Reentrant);
        if (h.invalidateOnEntry) h.runtime.Invalidate(h.binding.source);
        return h.registerOk;
    }
    static bool TransitionCall(void* context, const Binding&, const Command&) noexcept {
        ++static_cast<Harness*>(context)->transitions; return true;
    }
    static bool Snapshot(void* context, const Binding&, PairState& output) noexcept {
        const auto& h = *static_cast<Harness*>(context); output = h.native; return h.snapshotOk;
    }
    static Check Proof(void* context, const Command&, const Binding&, const Receipt&) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.proofs; return h.releaseProof;
    }
    Memory MemoryView() noexcept { return {0x400000U, &bytes, &Bytes::Read}; }
    bool Retains() noexcept { return runtime.RetainSource(binding.source.character, native.sourceCycle, MemoryView()); }
    Result Submit(Receipt& receipt) noexcept { return runtime.Submit(state, binding, receipt); }
};
void CarrierAndAdmission() {
    Harness h; Receipt receipt{};
    h.state.interactionCommand=1234567890123ULL;
    auto state = h.state; ++state.sessionCookie;
    CHECK(h.runtime.Submit(state, h.binding, receipt) == Result::WrongScope);
    state=h.state;state.interactionSession[8]=1U;
    CHECK(h.runtime.Submit(state,h.binding,receipt)==Result::WrongScope);
    state=h.state;++state.interactionAuthority;
    CHECK(h.runtime.Submit(state,h.binding,receipt)==Result::WrongScope);
    state=h.state;state.interactionCommand=0U;
    CHECK(h.runtime.Submit(state,h.binding,receipt)==Result::WrongScope);
    state = h.state; state.interactionTargetPeer = 22U;
    state.interactionTargetBirth = state.interactionTargetClass = 0U;
    state.interactionFlags |= InteractionTargetIsParticipant;
    CHECK(h.runtime.Submit(state, h.binding, receipt) == Result::Rejected);
    CHECK(h.registrations == 0U);
    state=h.state;state.interactionTransition=2U;
    CHECK(h.runtime.Submit(state,h.binding,receipt)==Result::Rejected&&h.registrations==0U&&h.transitions==0U);
    h.definitionReady = false;
    CHECK(h.Submit(receipt) == Result::Waiting); CHECK(!receipt.pairEntered);
    h.definitionReady = true;
    CHECK(h.Submit(receipt) == Result::Admitted); CHECK(h.registrations == 1U && h.transitions == 1U);
    CHECK(receipt.key.command==h.state.interactionCommand); // Host identity, never a local synthesized ID.
    auto conflict=h.state;++conflict.interactionCommand;
    CHECK(h.runtime.Submit(conflict,h.binding,receipt)==Result::Conflict);
    CHECK(!h.runtime.Find(conflict,receipt));
    conflict=h.state;++conflict.interactionPairRevision;
    CHECK(h.runtime.Submit(conflict,h.binding,receipt)==Result::Conflict);
    CHECK(h.registrations==1U&&h.transitions==1U);
    CHECK(h.runtime.Find(h.state,receipt));
    CHECK(!receipt.outcomeApplied && !receipt.resourcesReleased);
    const auto key = receipt.key;
    ++h.state.revision; ++h.state.interactionRevision; ++h.state.outcome.revision;
    h.state.outcome.flags |= OutcomeVictimEnded;
    h.state.position[0] = 90000.0F;
    CHECK(h.Submit(receipt) == Result::Duplicate); CHECK(receipt.key == key);
    CHECK(h.registrations == 1U && h.transitions == 1U);
    ++h.state.interactionTransitionVariant;
    CHECK(h.Submit(receipt) == Result::Conflict); CHECK(h.registrations == 1U);
    CHECK(h.runtime.Configure(Scope(), 100U, 2U) == Result::Conflict);
    auto scope = Scope(); ++scope.consumerEpoch;
    CHECK(h.runtime.Configure(scope, 99U, 2U) == Result::WrongScope);
    ++scope.authorityEpoch;CHECK(h.runtime.Configure(scope,99U,2U)==Result::Waiting);
}
void PartialEntryAndRevocation() {
    Harness h; Receipt receipt{}; h.registerOk = false;
    CHECK(h.Submit(receipt) == Result::Unknown); CHECK(receipt.pairEntered && !receipt.transitionEntered);
    CHECK(h.Submit(receipt) == Result::Unknown); CHECK(h.registrations == 1U && h.transitions == 0U);
    CHECK(!h.Retains());
    Harness revoked; revoked.invalidateOnEntry = true;
    CHECK(revoked.Submit(receipt) == Result::Unknown);
    CHECK(revoked.Submit(receipt) == Result::Unknown); CHECK(revoked.registrations == 1U);
    CHECK(!revoked.Retains());
}
void SourceLifetimeAndRelease() {
    Harness h; Receipt receipt{}; CHECK(h.Submit(receipt) == Result::Admitted);
    const auto key = receipt.key; const auto serial = receipt.serial;
    CHECK(h.Retains());
    h.bytes.bytes.erase(h.native.sourceCycle + 0x14U);
    CHECK(CheckSourceRetention(h.MemoryView(), h.binding.source, h.native, h.native.sourceCycle) == Check::Unknown);
    CHECK(h.Retains()); // Read failure cannot discharge known source ownership.
    h.bytes.Put(h.native.sourceCycle + 0x14U, 0x120000U);
    h.native.sourceFinished = h.native.victimEnded = true; h.native.stage = 6U;
    h.bytes.Put(h.native.sourceCycle + 0xE6U, std::uint8_t{1});
    h.bytes.Put(h.native.sourceCycle + 0xE0U, 6U);
    CHECK(h.runtime.Inspect(key, receipt) == Result::Pending);
    CHECK(h.Retains()); // Native finish request accepted, graph still running.
    CHECK(h.runtime.OutcomeApplied(key, serial + 1U, EvidenceOutcome(h.state), PreparationApplied | ReactionApplied) == Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key, serial, EvidenceOutcome(h.state), PreparationApplied | ReactionApplied) == Result::Admitted);
    CHECK(h.runtime.Inspect(key, receipt) == Result::Completed);
    CHECK(h.Retains()); // Logical completion is not graph/resource release.
    h.native.victim = 0U; h.native.linked = false; h.native.victimAcquired = false;
    CHECK(h.runtime.Inspect(key, receipt) == Result::Unknown);
    CHECK(h.Retains()); // Cached exact source ownership survives victim teardown.
    CHECK(h.runtime.Release(key, &Harness::Proof, &h, receipt) == Result::Waiting);
    CHECK(!receipt.resourcesReleased);
    h.bytes.Put(h.native.sourceCycle + 8U, std::uint8_t{6});
    CHECK(!h.Retains()); // End_Action requests termination and permits cleanup.
    h.releaseProof = Check::Ready;
    CHECK(h.runtime.Release(key, &Harness::Proof, &h, receipt) == Result::Released);
    CHECK(receipt.resourcesReleased && !h.Retains());
    CHECK(h.runtime.Release(key, &Harness::Proof, &h, receipt) == Result::Duplicate);
    CHECK(h.proofs == 2U);
}
void ExactCallbacksAndReuse() {
    Harness h; Receipt receipt{}; CHECK(h.Submit(receipt) == Result::Admitted);
    const auto key = receipt.key; const auto serial = receipt.serial;
    CHECK(h.runtime.Observe(key, serial + 1U, h.binding.source, h.native.sourceCycle,
        h.native.sourceSerial, FinishCause::NativeInterruption, receipt) == Result::Rejected);
    CHECK(h.Retains());
    h.bytes.Put(h.native.sourceCycle + 0x19U, std::uint8_t{5}); CHECK(!h.Retains());
    h.bytes.Put(h.native.sourceCycle + 0x19U, std::uint8_t{4}); CHECK(h.Retains());
    h.bytes.Put(0x120000U + 0x50U, 0x130004U); CHECK(!h.Retains());
    h.bytes.Put(0x120000U + 0x50U, 0x130000U); CHECK(h.Retains());
    CHECK(h.runtime.Observe(key, serial, h.binding.victim, h.native.victimCycle,
        h.native.victimSerial, FinishCause::NativeDestruction, receipt) == Result::Pending);
    CHECK(receipt.phase == Phase::Interrupted && !receipt.victimCompleted && h.Retains());
    h.runtime.Invalidate(h.binding.source);CHECK(!h.Retains());
    Harness ended; CHECK(ended.Submit(receipt) == Result::Admitted);
    CHECK(ended.runtime.Observe(receipt.key, receipt.serial, ended.binding.source, ended.native.sourceCycle,
        ended.native.sourceSerial, FinishCause::NativeGraphEnd, receipt) == Result::Pending);
    CHECK(ended.Retains()); // End_Action can be declined by the native handler.
    ended.native.sourceFlags=6U;
    ended.bytes.Put(ended.native.sourceCycle+8U,std::uint8_t{6});
    CHECK(ended.runtime.Observe(receipt.key, receipt.serial, ended.binding.source, ended.native.sourceCycle,
        ended.native.sourceSerial, FinishCause::NativeGraphEnd, receipt) == Result::Pending);
    CHECK(!ended.Retains() && !receipt.sourceCompleted);
    Harness victimRevoked;CHECK(victimRevoked.Submit(receipt)==Result::Admitted);
    victimRevoked.runtime.Invalidate(victimRevoked.binding.victim);
    CHECK(victimRevoked.Retains()); // Losing the victim cannot erase the admitted source outro.
    victimRevoked.bytes.Put(victimRevoked.native.sourceCycle+8U,std::uint8_t{6});
    CHECK(!victimRevoked.Retains());
}
void AcceptedEndExitBoundary() {
    Harness h;Receipt receipt{};CHECK(h.Submit(receipt)==Result::Admitted);
    const auto test=[&]() {return CheckSourceExit(h.MemoryView(),h.binding.source,h.native,h.native.sourceCycle);};
    CHECK(test()==Check::Rejected); // E6/stage6/HP alone never authorizes exit.
    h.native.sourceFinished=h.native.victimEnded=true;h.native.stage=6;
    CHECK(test()==Check::Rejected);
    h.bytes.Put(h.native.sourceCycle+8U,std::uint8_t{6});CHECK(test()==Check::Ready);
    CHECK(!h.Retains()&&!receipt.resourcesReleased);
    h.bytes.Put(h.native.sourceCycle+0x19U,std::uint8_t{5});CHECK(test()==Check::Rejected);
    h.bytes.Put(h.native.sourceCycle+0x19U,h.native.sourceSerial);
    h.bytes.Put(0x120000U+0x50U,0x130004U);CHECK(test()==Check::Rejected);
    h.bytes.Put(0x120000U+0x50U,static_cast<std::uint32_t>(h.native.sourceGraph));
    h.bytes.bytes.erase(h.native.sourceCycle+8U);CHECK(test()==Check::Unknown);
}
void BoundedCommands() {
    Harness h; h.definitionReady = false; Receipt receipt{};
    for (std::size_t i = 0U; i < Runtime::kCapacity; ++i) {
        h.state.interactionPairRevision = static_cast<std::uint32_t>(i + 1U);
        h.state.interactionCommand=i+1U;
        CHECK(h.Submit(receipt) == Result::Waiting);
    }
    ++h.state.interactionPairRevision;
    ++h.state.interactionCommand;
    CHECK(h.Submit(receipt) == Result::Capacity); CHECK(h.registrations == 0U);
}
void OutcomeProgressAndEvidence() {
    Harness h; Receipt receipt{}; CHECK(h.Submit(receipt) == Result::Admitted);
    const auto key = receipt.key; const auto serial = receipt.serial;
    const auto initial = EvidenceOutcome(h.state);
    CHECK(receipt.outcomeRevision == 1U && receipt.appliedOutcomeRevision == 0U);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,ReactionApplied) == Result::Admitted);
    CHECK(h.runtime.Inspect(key,receipt) == Result::Pending && !receipt.outcomeApplied);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,PreparationApplied) == Result::Admitted);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,PreparationApplied) == Result::Duplicate);
    h.native.sourceFinished = h.native.victimEnded = true;
    CHECK(h.runtime.Inspect(key,receipt) == Result::Completed && receipt.appliedOutcomeRevision == 1U);

    ++h.state.revision; ++h.state.outcome.revision;
    h.state.outcome.flags |= OutcomeSourceEnded | OutcomeVictimEnded;
    CHECK(h.Submit(receipt) == Result::Duplicate);
    CHECK(receipt.outcomeApplied && receipt.appliedOutcomeRevision == 2U);
    // The late kill is a new native obligation even though admission is final.
    ++h.state.revision; ++h.state.outcome.revision;
    h.state.outcome.flags |= OutcomeTerminal | OutcomeNativeDeath;
    h.state.outcome.hpReaction = 0U; h.state.outcome.healthReaction = 4U;
    CHECK(h.Submit(receipt) == Result::Duplicate);
    CHECK(!receipt.outcomeApplied && receipt.phase == Phase::Running && receipt.outcomeRevision == 3U);
    CHECK(receipt.appliedOutcomeRevision == 2U && receipt.observedOutcomeEvidence == PreparationApplied);
    CHECK(receipt.requiredOutcomeEvidence == (PreparationApplied | ReactionApplied | NativeDeathApplied));
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,PreparationApplied | ReactionApplied) == Result::Rejected);
    auto latest = EvidenceOutcome(h.state); auto forged = latest; forged.hpPrepared = 1U;
    CHECK(h.runtime.OutcomeApplied(key,serial,forged,ReactionApplied | NativeDeathApplied) == Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key,serial,latest,0U) == Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key,serial,latest,8U) == Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key,serial,latest,ReactionApplied) == Result::Admitted);
    CHECK(h.runtime.Inspect(key,receipt) == Result::Pending && !receipt.outcomeApplied);
    CHECK(h.runtime.Release(key,&Harness::Proof,&h,receipt) == Result::Waiting);
    CHECK(h.proofs == 0U); // A previously complete pair reopened no release path.
    CHECK(h.runtime.OutcomeApplied(key,serial,latest,NativeDeathApplied) == Result::Admitted);
    CHECK(h.runtime.Inspect(key,receipt) == Result::Completed && receipt.appliedOutcomeRevision == 3U);
    ++h.state.outcome.revision; ++h.state.revision;
    CHECK(h.Submit(receipt) == Result::Duplicate && receipt.appliedOutcomeRevision == 4U);
    CHECK(h.registrations == 1U && h.transitions == 1U);
    const auto good = h.state;
    --h.state.outcome.revision;
    CHECK(h.Submit(receipt) == Result::Conflict);
    h.state = good; ++h.state.outcome.revision; h.state.outcome.flags &= ~OutcomeSourceEnded;
    CHECK(h.Submit(receipt) == Result::Conflict);
    h.state = good; ++h.state.outcome.revision; --h.state.outcome.hpPrepared;
    CHECK(h.Submit(receipt) == Result::Conflict);
    h.state = good; h.state.outcome.hpReaction = 1U; h.state.outcome.healthReaction = 1U;
    CHECK(h.Submit(receipt) == Result::Rejected); // Contradictory terminal result.
    CHECK(h.registrations == 1U && h.transitions == 1U);
}
void PlayerLifeBindingAndDelayedPreparation() {
    Harness h;Receipt receipt{};
    h.state.interactionTargetPeer=22U;h.state.interactionParticipantIncarnation=8U;
    h.state.interactionTargetA=h.state.interactionTargetB=0U;
    h.state.interactionTargetBirth=h.state.interactionTargetClass=0U;
    h.state.interactionFlags|=InteractionTargetIsParticipant;
    h.binding.victim.perspective=Perspective::RemotePlayer;
    h.binding.victim.creationSerial=1234U;h.binding.victim.participantIncarnation=7U;
    CHECK(h.Submit(receipt)==Result::Rejected&&h.registrations==0U);
    h.binding.victim.participantIncarnation=8U;
    CHECK(h.Submit(receipt)==Result::Admitted&&h.registrations==1U);
    const auto initial=EvidenceOutcome(h.state);const auto key=receipt.key;const auto serial=receipt.serial;
    ++h.state.outcome.revision;++h.state.revision;
    h.state.outcome.hpReaction=20U;
    CHECK(h.Submit(receipt)==Result::Duplicate&&h.registrations==1U);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,PreparationApplied)==Result::Admitted);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,ReactionApplied)==Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key,serial,EvidenceOutcome(h.state),ReactionApplied)==Result::Admitted);
    auto recycled=h.binding;++recycled.victim.creationSerial;
    CHECK(h.runtime.Submit(h.state,recycled,receipt)==Result::Conflict&&h.registrations==1U);
}
void KillToNextWorldSequence() {
    Harness h;Receipt receipt{};
    h.state.interactionTargetPeer=22U;h.state.interactionParticipantIncarnation=8U;
    h.state.interactionTargetA=h.state.interactionTargetB=0U;
    h.state.interactionTargetBirth=h.state.interactionTargetClass=0U;
    h.state.interactionFlags|=InteractionTargetIsParticipant;
    h.binding.victim.perspective=Perspective::RemotePlayer;
    h.binding.victim.creationSerial=1234U;h.binding.victim.participantIncarnation=8U;
    CHECK(h.Submit(receipt)==Result::Admitted);
    const auto key=receipt.key;const auto serial=receipt.serial;const auto initial=EvidenceOutcome(h.state);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,PreparationApplied|ReactionApplied)==Result::Admitted);
    // A source finish request and victim end are not disposal of the source outro.
    h.native.sourceFinished=h.native.victimEnded=true;h.native.stage=6U;
    h.bytes.Put(h.native.sourceCycle+0xE6U,std::uint8_t{1});
    CHECK(h.runtime.Observe(key,serial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeSourceFinish,receipt)==Result::Completed);
    CHECK(h.Retains());
    auto next=Scope();++next.worldEpoch;++next.consumerEpoch;
    CHECK(h.runtime.Configure(next,99U,3U)==Result::Waiting);
    // A late fatal result adds a real native obligation, never a second admission.
    ++h.state.revision;++h.state.outcome.revision;
    h.state.outcome.flags|=OutcomeTerminal|OutcomeNativeDeath|OutcomeSourceEnded|OutcomeVictimEnded;
    h.state.outcome.hpReaction=0U;h.state.outcome.healthReaction=4U;
    CHECK(h.Submit(receipt)==Result::Duplicate&&!receipt.outcomeApplied);
    CHECK(h.registrations==1U&&h.transitions==1U&&h.Retains());
    CHECK(h.runtime.Release(key,&Harness::Proof,&h,receipt)==Result::Waiting&&h.proofs==0U);
    CHECK(h.runtime.OutcomeApplied(key,serial,initial,NativeDeathApplied)==Result::Rejected);
    CHECK(h.runtime.OutcomeApplied(key,serial,EvidenceOutcome(h.state),ReactionApplied|NativeDeathApplied)==Result::Admitted);
    CHECK(h.runtime.Inspect(key,receipt)==Result::Completed&&h.Retains());
    h.bytes.Put(h.native.sourceCycle+8U,std::uint8_t{6});
    h.native.sourceFlags=6U;
    CHECK(h.runtime.Observe(key,serial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeGraphEnd,receipt)==Result::Pending);
    CHECK(receipt.phase==Phase::Interrupted&&receipt.outcomeApplied);
    CHECK(!h.Retains()&&!receipt.resourcesReleased); // Native flags6 permits cleanup, not fabricated success.
    CHECK(h.runtime.Release(key,&Harness::Proof,&h,receipt)==Result::Waiting&&h.proofs==1U);
    h.releaseProof=Check::Ready;
    CHECK(h.runtime.Release(key,&Harness::Proof,&h,receipt)==Result::Released);
    CHECK(h.runtime.Configure(next,99U,3U)==Result::Admitted);
    CHECK(h.Submit(receipt)==Result::WrongScope);
    CHECK(h.runtime.Observe(key,serial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeDestruction,receipt)==Result::WrongScope);
    CHECK(h.runtime.OutcomeApplied(key,serial,EvidenceOutcome(h.state),NativeDeathApplied)==Result::WrongScope);
    CHECK(h.registrations==1U&&h.transitions==1U);
}
void SourceGraphInitializesAfterAdmission() {
    Harness h;Receipt receipt{};
    h.native.sourcePrepared=h.native.linked=h.native.victimAcquired=false;
    h.native.sourceGraph=0U;
    CHECK(h.Submit(receipt)==Result::Pending);
    CHECK(h.Retains()); // Idle cannot replace the admitted pre-init cycle.
    h.bytes.Put(h.native.sourceCycle+8U,std::uint8_t{6});CHECK(!h.Retains());
    h.bytes.Put(h.native.sourceCycle+8U,std::uint8_t{});
    h.bytes.Put(h.native.sourceCycle+0xE7U,std::uint8_t{1});
    CHECK(h.Retains()); // Native initialization can precede the next pump.
    h.bytes.Put(h.native.sourceCycle+8U,std::uint8_t{6});
    CHECK(!h.Retains());
}
void CallbackPublication() {
    Harness h;Receipt receipt{};CHECK(h.Submit(receipt)==Result::Admitted);
    h.published=&receipt;
    CHECK(h.runtime.Inspect(receipt.key,receipt)==Result::Pending);
    CHECK(h.runtime.Observe(receipt.key,receipt.serial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeSourceFinish,receipt)==Result::Pending);
    CHECK(h.Submit(receipt)==Result::Duplicate);
    CHECK(receipt.pairEntered&&receipt.phase!=Phase::Fenced);
    h.native.sourceFlags=6U;
    CHECK(h.runtime.Observe(receipt.key,receipt.serial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeGraphEnd,receipt)==Result::Pending);
    h.releaseProof=Check::Ready;
    CHECK(h.runtime.Release(receipt.key,&Harness::Proof,&h,receipt)==Result::Released);
    CHECK(h.registrations==1U&&h.transitions==1U);
}
void KillReservationCannotCrossWorlds() {
    Harness h; Receipt receipt{};
    h.binding.nativeGeneration=1U;
    CHECK(h.Submit(receipt)==Result::Admitted);
    const auto oldKey=receipt.key;const auto oldSerial=receipt.serial;
    auto next=Scope();++next.worldEpoch;++next.consumerEpoch;
    CHECK(h.runtime.Configure(next,99U,3U)==Result::Waiting);
    CHECK(!h.runtime.ResetForWorld(1U)); // Same live world is not a cleanup proof.
    CHECK(h.runtime.ResetForWorld(3U));
    CHECK(h.proofs==0U); // Revocation never invoked or manufactured release.
    CHECK(h.runtime.Configure(next,99U,3U)==Result::Admitted);
    h.state.worldGeneration=3U;
    h.binding.nativeGeneration=3U;
    ++h.binding.source.creationSerial;++h.binding.victim.creationSerial;
    CHECK(h.Submit(receipt)==Result::Admitted && receipt.serial>oldSerial);
    CHECK(h.runtime.Observe(oldKey,oldSerial,h.binding.source,h.native.sourceCycle,h.native.sourceSerial,
        FinishCause::NativeGraphEnd,receipt)==Result::WrongScope);
    CHECK(h.registrations==2U&&h.transitions==2U);
}
}
int main() {
    CarrierAndAdmission(); PartialEntryAndRevocation(); SourceLifetimeAndRelease();
    ExactCallbacksAndReuse(); AcceptedEndExitBoundary();BoundedCommands(); OutcomeProgressAndEvidence();
    PlayerLifeBindingAndDelayedPreparation();KillToNextWorldSequence();SourceGraphInitializesAfterAdmission();CallbackPublication();KillReservationCannotCrossWorlds();
    std::cout << "State/native adapter bridge and modeled source lifetime passed. No game execution or native release proof claimed.\n";
}
