#include "aimp/AISyncInteraction.h"
#include "aimp/AISyncAnimation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace aimp::ai_sync::interactions {
namespace {
constexpr std::uintptr_t kSourceType = 0x00EAE5D0U, kVictimType = 0x00EAE690U;
template<class T> bool Read(const Memory& memory, const std::uintptr_t address, T& value) noexcept {
    return memory.read != nullptr && address >= 0x10000U &&
        address <= 0xFFFFFFFFULL - sizeof(T) && memory.read(memory.context, address, &value, sizeof(T));
}
bool Pointer(const Memory& memory, const std::uintptr_t address, std::uintptr_t& value) noexcept {
    std::uint32_t word{};
    if (!Read(memory, address, word)) return false;
    value = word;
    return true;
}
bool Graph(const Memory& memory, const std::uintptr_t cycle, std::uintptr_t& graph) noexcept {
    std::uintptr_t action{};
    return Pointer(memory, cycle + 0x14U, action) &&
        (action == 0U || Pointer(memory, action + 0x50U, graph));
}
bool ScopeValid(const transactions::Scope& scope) noexcept {
    return std::any_of(scope.session.begin(), scope.session.end(), [](auto value) { return value != 0U; }) &&
        scope.authorityEpoch != 0U && scope.worldEpoch != 0U && scope.consumerEpoch != 0U;
}
bool ParticipantValid(const Participant& value) noexcept {
    return value.birth != 0U && (value.peer != 0U ||
        ((value.entityA != 0U || value.entityB != 0U) && value.actorClass != 0U));
}
bool OutcomeValid(const Outcome& outcome) noexcept {
    if (outcome.revision == 0U || outcome.flags > 63U || (outcome.flags & 1U) == 0U ||
        outcome.definition > 0xFFFFU || (outcome.kind != 0U && outcome.kind != 1U && outcome.kind != 3U) || outcome.hpBefore == 0U ||
        outcome.hpBefore > 0xFFFFU || outcome.hpPrepared > outcome.hpBefore || outcome.hpReaction > 0xFFFFU ||
        outcome.healthReaction > 4U || !std::isfinite(outcome.sourceYaw) || std::abs(outcome.sourceYaw) > 100000.0F ||
        ((outcome.hpReaction == 0U) != (outcome.healthReaction == 4U)) ||
        (((outcome.flags & 16U) != 0U) != (outcome.hpReaction == 0U)) ||
        ((outcome.flags & 32U) != 0U && (outcome.flags & 2U) == 0U)) return false;
    for (const auto offset : outcome.targetOffset)
        if (!std::isfinite(offset) || std::abs(offset) > 100000.0F) return false;
    return true;
}
std::uint32_t RequiredEvidence(const Outcome& outcome) noexcept {
    return PreparationApplied | ReactionApplied |
        ((outcome.kind == 0U && (outcome.flags & 48U) == 48U) ? NativeDeathApplied : 0U);
}
void RefreshOutcome(Receipt& receipt) noexcept {
    receipt.outcomeApplied = (receipt.observedOutcomeEvidence & receipt.requiredOutcomeEvidence) == receipt.requiredOutcomeEvidence;
    if (receipt.outcomeApplied) receipt.appliedOutcomeRevision = receipt.outcomeRevision;
    if (!receipt.outcomeApplied && receipt.phase == Phase::Completed) receipt.phase = Phase::Running;
}
bool CommandValid(const Command& command) noexcept {
    if (!ScopeValid(command.key.scope) || command.key.command == 0U ||
        !ParticipantValid(command.source) || command.source.peer != 0U || !ParticipantValid(command.victim) ||
        command.source == command.victim || command.pairRevision == 0U || command.transitionRevision == 0U ||
        command.sourceDefinition < 0 || command.sourceDefinition > 0xFFFF ||
        // 0065BF60/B constructs cycle42. The retained inspector/lifetime
        // contract is 4E/4F; reject before registration rather than enter B
        // and discover that mismatch after irreversible native effects.
        command.transition != Transition::A ||
        command.firstFlag > 0xFFU || command.secondFlag > 0xFFU ||
        (command.transition == Transition::A && command.secondFlag != 0U)) return false;
    return command.outcomeRequired ? OutcomeValid(command.outcome) : command.outcome == Outcome{};
}
bool SameActor(const Participant& left, const Participant& right) noexcept {
    if (left.peer != 0U || right.peer != 0U) return left.peer != 0U && left.peer == right.peer && left.birth == right.birth;
    return left.entityA == right.entityA && left.entityB == right.entityB && left.birth == right.birth;
}
struct Busy final { bool& value; explicit Busy(bool& flag) noexcept : value(flag) { value = true; } ~Busy() { value = false; } };
struct ReceiptPublication final {
    Receipt& destination;
    Receipt value{};
    ~ReceiptPublication() { destination = value; }
};
}

bool SamePreparation(const Outcome& left, const Outcome& right) noexcept {
    return left.definition == right.definition && left.kind == right.kind &&
        left.hpBefore == right.hpBefore && left.hpPrepared == right.hpPrepared &&
        left.sourceYaw == right.sourceYaw && left.targetOffset == right.targetOffset &&
        (left.flags & 1U) == (right.flags & 1U);
}

bool InspectNativePair(const Memory& memory, const std::uintptr_t source,
    const std::uint32_t runtimeId, PairState& output) noexcept {
    output = {};
    PairState state{};
    std::uintptr_t type{}, owner{}, linkedSource{}, linkedCycle{};
    std::uint32_t cachedVictim{}, cachedSource{}, currentSource{};
    std::uint8_t cycleKind{}, prepared{}, finished{}, acquired{}, ended{};
    if (memory.imageBase < 0x10000U || memory.imageBase > 0xFFFFFFFFULL - kVictimType ||
        runtimeId == 0U || !Read(memory, source + 0x50U, state.sourceRuntimeId) || state.sourceRuntimeId != runtimeId ||
        !Pointer(memory, source + 0x36CU, state.sourceCycle) || state.sourceCycle == 0U ||
        !Pointer(memory, state.sourceCycle, type) || type != memory.imageBase + kSourceType ||
        !Pointer(memory, state.sourceCycle + 4U, owner) || owner != source ||
        !Read(memory, state.sourceCycle + 0x1AU, cycleKind) || cycleKind != 0x4EU ||
        !Read(memory, state.sourceCycle + 0x19U, state.sourceSerial) ||
        !Read(memory, state.sourceCycle + 8U, state.sourceFlags) ||
        !Read(memory, state.sourceCycle + 0xE0U, state.stage) ||
        !Read(memory, state.sourceCycle + 0xE7U, prepared) ||
        !Read(memory, state.sourceCycle + 0xE6U, finished) ||
        !Pointer(memory, state.sourceCycle + 0x4CU, state.definition) ||
        !Pointer(memory, state.sourceCycle + 0x44U, state.victim) ||
        !Graph(memory, state.sourceCycle, state.sourceGraph)) return false;
    state.source = source;
    state.sourcePrepared = prepared != 0U;
    state.sourceFinished = finished != 0U;
    // Stage 6 is also a failed initializer. Preserve that fact without calling
    // it successful completion or inventing a victim for an empty +44.
    if (state.victim != 0U) {
        if (!Read(memory, state.sourceCycle + 0x48U, cachedVictim) || cachedVictim == 0U ||
            !Read(memory, state.victim + 0x50U, state.victimRuntimeId) || state.victimRuntimeId != cachedVictim ||
            !Pointer(memory, state.victim + 0x36CU, state.victimCycle)) return false;
        if (state.victimCycle != 0U && Pointer(memory, state.victimCycle, type) && type == memory.imageBase + kVictimType &&
            Pointer(memory, state.victimCycle + 4U, owner) && owner == state.victim &&
            Read(memory, state.victimCycle + 0x1AU, cycleKind) && cycleKind == 0x4FU &&
            Read(memory, state.victimCycle + 0x19U, state.victimSerial) &&
            Read(memory, state.victimCycle + 8U, state.victimFlags) &&
            Pointer(memory, state.victimCycle + 0x68U, linkedSource) && linkedSource == source &&
            Pointer(memory, state.victimCycle + 0xB0U, linkedCycle) && linkedCycle == state.sourceCycle &&
            Read(memory, state.victimCycle + 0x6CU, cachedSource) && cachedSource == runtimeId &&
            Read(memory, state.victimCycle + 0xD9U, acquired) &&
            Read(memory, state.victimCycle + 0xD8U, ended) && Graph(memory, state.victimCycle, state.victimGraph)) {
            state.linked = true;
            state.victimAcquired = acquired != 0U;
            state.victimEnded = ended != 0U;
        }
    }
    // Recheck both ends after reading the graph, including persistent cycle
    // reuse. This is a local inspection, not a pin across a world transition.
    std::uintptr_t currentCycle{};
    std::uint8_t serial{};
    if (!Read(memory, source + 0x50U, currentSource) || currentSource != runtimeId ||
        !Pointer(memory, source + 0x36CU, currentCycle) || currentCycle != state.sourceCycle ||
        !Read(memory, state.sourceCycle + 0x19U, serial) || serial != state.sourceSerial) return false;
    if (state.linked && (!Read(memory, state.victim + 0x50U, cachedVictim) || cachedVictim != state.victimRuntimeId ||
        !Pointer(memory, state.victim + 0x36CU, currentCycle) || currentCycle != state.victimCycle ||
        !Read(memory, state.victimCycle + 0x19U, serial) || serial != state.victimSerial)) return false;
    output = state;
    return true;
}

bool OwnsSource(const PairState& state) noexcept {
    return state.source != 0U && state.sourceCycle != 0U && state.sourcePrepared && !state.sourceFinished &&
        state.sourceGraph != 0U && state.stage < 6U && (state.sourceFlags & 4U) == 0U &&
        state.linked && state.victimAcquired && (state.victimFlags & 4U) == 0U;
}

bool NativePairAdapter::Initialize(const std::uintptr_t imageBase, const bool exactBuildValidated,
    Entrypoints entries, const Permission permission, void* context) noexcept {
    if (base_ != 0U || count_ != 0U || !exactBuildValidated || imageBase < 0x10000U ||
        imageBase > 0xFFFFFFFFULL - 0x00EAE690U || permission == nullptr) return false;
    base_ = imageBase;
    if (entries.pair == 0U) entries.pair = imageBase + 0x00254820U;
    if (entries.transitionA == 0U) entries.transitionA = imageBase + 0x0025C4E0U;
    if (entries.transitionB == 0U) entries.transitionB = imageBase + 0x0025BF60U;
    entries_ = entries; permission_ = permission; context_ = context;
    return true;
}
Result NativePairAdapter::Configure(const transactions::Scope& scope) noexcept {
    if (busy_) return Result::Reentrant;
    if (base_ == 0U || !ScopeValid(scope)) return Result::Rejected;
    if (configured_ && scope == scope_) return Result::Duplicate;
    for (std::size_t i = 0; i < count_; ++i) if (!leases_[i].receipt.resourcesReleased) return Result::Waiting;
    scope_ = scope; configured_ = true; count_ = 0U;
    return Result::Admitted;
}
bool NativePairAdapter::ResetForWorld(const std::uint32_t current) noexcept {
    if (busy_ || current == 0U) return false;
    for (std::size_t i = 0; i < count_; ++i)
        if (leases_[i].binding.nativeGeneration == 0U || leases_[i].binding.nativeGeneration >= current)
            return false;
    ++revocation_;
    leases_ = {}; count_ = 0U; scope_ = {}; configured_ = false;
    return true;
}
NativePairAdapter::Lease* NativePairAdapter::FindLease(const Key& key) noexcept {
    for (std::size_t i = 0; i < count_; ++i) if (leases_[i].command.key == key) return &leases_[i];
    return nullptr;
}
Check NativePairAdapter::CheckBinding(const Command& command, const Binding& binding) noexcept {
    if (binding.source.character == 0U || binding.victim.character == 0U ||
        binding.source.character == binding.victim.character || binding.source.runtimeId == 0U ||
        binding.victim.runtimeId == 0U || !binding.exclusiveExecution ||
        (command.outcomeRequired && !binding.outcomeBound) || binding.localPairFlagsHigh > 1U) { RefusalSite()=211U; return Check::Rejected; }
    if(command.victim.peer!=0U&&(binding.victim.perspective==Perspective::Npc||
        binding.victim.creationSerial==0U||binding.victim.participantIncarnation!=command.victim.birth)){RefusalSite()=212U;return Check::Rejected;}
    const auto permission = permission_(context_, command, binding);
    if (permission != Check::Ready) return permission;
    if (bindingPrimitive_ != nullptr) return bindingPrimitive_(primitivesContext_, command, binding);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        const auto live = [](const LocalParticipant& actor) {
            return *reinterpret_cast<const std::uint32_t*>(actor.character + 0x50U) == actor.runtimeId;
        };
        if (live(binding.source) && live(binding.victim)) return Check::Ready;
        RefusalSite()=213U; return Check::Rejected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return Check::Unknown; }
#else
    return Check::Rejected;
#endif
}
bool NativePairAdapter::ResolveDefinition(const Binding& binding, const Command& command, Definition& output) noexcept {
    if (definitionPrimitive_ != nullptr) return definitionPrimitive_(primitivesContext_, binding, command, output);
    return animation::ResolvePairDefinition(binding.source.character, binding.source.runtimeId,
        binding.victim.character, binding.victim.runtimeId, static_cast<std::uint32_t>(command.sourceDefinition), output);
}
bool NativePairAdapter::DefinitionLive(const Binding& binding, const Definition& definition) noexcept {
    if (definitionLivePrimitive_ != nullptr) return definitionLivePrimitive_(primitivesContext_, binding, definition);
    Definition current{};
    return animation::ResolvePairDefinition(binding.source.character, binding.source.runtimeId,
        binding.victim.character, binding.victim.runtimeId, definition.selector, current) && current == definition;
}
bool NativePairAdapter::Register(const Binding& binding, const Definition& definition, bool& accepted) noexcept {
    accepted = false;
    if (registerPrimitive_ != nullptr) return registerPrimitive_(primitivesContext_, binding, definition, accepted);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        const auto root = *reinterpret_cast<const std::uintptr_t*>(base_ + 0x012F0C88U);
        if (root == 0U) return false;
        const auto manager = *reinterpret_cast<const std::uintptr_t*>(root);
        if (manager == 0U) return false;
        using Pair = std::uint32_t(__thiscall*)(void*, std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t, std::uint64_t, std::int32_t);
        const auto payload = static_cast<std::uint64_t>(definition.victimAttachment > 0 ? 1U : 0U) |
            (static_cast<std::uint64_t>(binding.localPairFlagsHigh) << 32U);
        accepted = (reinterpret_cast<Pair>(entries_.pair)(reinterpret_cast<void*>(manager + 0x90U),
            binding.source.entityA, binding.source.entityB, binding.victim.entityA, binding.victim.entityB,
            payload, definition.victimAttachment) & 0xFFU) != 0U;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool NativePairAdapter::TransitionNative(const Binding& binding, const Command& command) noexcept {
    if (transitionPrimitive_ != nullptr) return transitionPrimitive_(primitivesContext_, binding, command);
#if defined(_WIN32) && defined(_M_IX86)
    __try {
        const auto cycle = *reinterpret_cast<const std::uintptr_t*>(binding.source.character + 0x36CU);
        if (cycle == 0U || *reinterpret_cast<const std::uintptr_t*>(cycle + 4U) != binding.source.character) return false;
        if (command.transition == Transition::A) {
            using TransitionA = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t);
            reinterpret_cast<TransitionA>(entries_.transitionA)(reinterpret_cast<void*>(cycle),
                binding.victim.character, command.sourceDefinition, command.firstFlag);
        } else {
            using TransitionB = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t, std::uint32_t);
            reinterpret_cast<TransitionB>(entries_.transitionB)(reinterpret_cast<void*>(cycle),
                binding.victim.character, command.sourceDefinition, command.firstFlag, command.secondFlag);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool NativePairAdapter::SnapshotNative(const Binding& binding, PairState& state) noexcept {
    if (snapshotPrimitive_ != nullptr) return snapshotPrimitive_(primitivesContext_, binding, state);
    return animation::InspectPair(binding.source.character, binding.source.runtimeId, state);
}
std::uint32_t& RefusalSite() noexcept { static std::uint32_t site{}; return site; }
Result NativePairAdapter::Fence(Lease& lease, Receipt& output,const std::uint32_t site) noexcept {
    if(!lease.receipt.failureSite)lease.receipt.failureSite=site;
    lease.receipt.phase = Phase::Fenced; output = lease.receipt; return Result::Unknown;
}
Result NativePairAdapter::Admit(const Command& inputCommand, const Binding& inputBinding, Receipt& published) noexcept {
    const auto command=inputCommand;
    const auto binding=inputBinding;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    if (!configured_ || command.key.scope != scope_) return Result::WrongScope;
    if (!CommandValid(command)) { RefusalSite()=201U; return Result::Rejected; }
    if (auto* previous = FindLease(command.key); previous != nullptr) {
        output = previous->receipt;
        if(previous->command!=command||previous->binding!=binding)return Result::Conflict;
        if(previous->receipt.phase==Phase::Registered&&
            (!previous->receipt.transitionEntered||!previous->captured)) {
            Busy guard(busy_);
            return AdvanceAdmission(*previous,output);
        }
        return Result::Duplicate;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& previous = leases_[i];
        if (previous.receipt.resourcesReleased) continue;
        if (SameActor(previous.command.source, command.source) || SameActor(previous.command.source, command.victim) ||
            SameActor(previous.command.victim, command.source) || SameActor(previous.command.victim, command.victim) ||
            previous.binding.source.character == binding.source.character || previous.binding.source.character == binding.victim.character ||
            previous.binding.victim.character == binding.source.character || previous.binding.victim.character == binding.victim.character)
            return Result::Waiting;
    }
    if (count_ == kCapacity || serial_ == std::numeric_limits<std::uint64_t>::max()) return Result::Capacity;
    Busy guard(busy_);
    const auto revision = revocation_;
    const auto checked = CheckBinding(command, binding);
    if (revision != revocation_) return Result::Unknown;
    if (checked != Check::Ready) return checked == Check::Waiting ? Result::Waiting :
        checked == Check::Unknown ? Result::Unknown : Result::Rejected;
    Definition definition{};
    if (!ResolveDefinition(binding, command, definition)) return Result::Waiting;
    if (revision != revocation_) return Result::Unknown;
    if (CheckBinding(command, binding) != Check::Ready || !DefinitionLive(binding, definition)) return Result::Waiting;
    if (revision != revocation_) return Result::Unknown;
    // 00662670 replaces cycle+4C at 00662837 with the prepared definition.
    // Resolve ONLY the selector carried by the authoritative outcome, before
    // irreversible registration; it is distinct from the initial source layout.
    Definition prepared=definition;
    if(command.outcomeRequired&&(command.outcome.flags&2U)!=0U) {
        auto preparation=command;preparation.sourceDefinition=static_cast<std::int32_t>(command.outcome.definition);
        if(!ResolveDefinition(binding,preparation,prepared))return Result::Waiting;
        if(revision!=revocation_)return Result::Unknown;
        if(prepared.manager!=definition.manager||prepared.kind!=command.outcome.kind||
            !DefinitionLive(binding,prepared)||CheckBinding(command,binding)!=Check::Ready)return Result::Waiting;
        if(revision!=revocation_)return Result::Unknown;
    }
    auto& lease = leases_[count_++]; lease = {};
    lease.command = command; lease.binding = binding; lease.definition = definition;lease.preparedDefinition=prepared;
    lease.outcome = command.outcome;
    lease.receipt.key = command.key; lease.receipt.serial = ++serial_;
    lease.receipt.outcomeRevision = command.outcome.revision;
    lease.receipt.requiredOutcomeEvidence = command.outcomeRequired ? RequiredEvidence(command.outcome) : 0U;
    lease.receipt.outcomeApplied = !command.outcomeRequired;
    lease.receipt.pairEntered = true; // Before any native side effect.
    bool accepted{};
    if (!Register(binding, definition, accepted) || lease.revoked) return Fence(lease, output);
    if (!accepted) { RefusalSite()=204U; lease.receipt.phase = Phase::Interrupted; output = lease.receipt; return Result::Rejected; }
    lease.receipt.phase = Phase::Registered;
    return AdvanceAdmission(lease,output);
}
bool NativePairAdapter::DefinitionMatches(Lease& lease,const PairState& state) noexcept {
    if(state.definition==lease.preparedDefinition.object&&DefinitionLive(lease.binding,lease.preparedDefinition)&&!lease.revoked) {
        lease.preparedSelected=true;return true;
    }
    // One-way native preparation. Never allow an arbitrary replacement or a
    // rollback to the source layout after the prepared definition was observed.
    return !lease.preparedSelected&&state.definition==lease.definition.object&&
        DefinitionLive(lease.binding,lease.definition)&&!lease.revoked;
}
Result NativePairAdapter::AdvanceAdmission(Lease& lease,Receipt& output) noexcept {
    // Registration and transition are separate irreversible operations. Waiting
    // retains the registered pair; it cannot register or transition it twice.
    if(lease.revoked)return Fence(lease,output);
    if(!lease.receipt.transitionEntered) {
        const auto permission=CheckBinding(lease.command,lease.binding);
        if(permission==Check::Waiting){output=lease.receipt;return Result::Pending;}
        if(permission!=Check::Ready||!DefinitionLive(lease.binding,lease.definition)||lease.revoked)
            return Fence(lease,output);
        lease.receipt.transitionEntered=true;
        if(!TransitionNative(lease.binding,lease.command)||lease.revoked)return Fence(lease,output);
    }
    if(!lease.captured) {
        // Copy the exact cycle produced by the call before consulting another
        // callback-capable permission check. This is observation, not another
        // native admission. Failed reads/mismatched incarnations still fence.
        PairState native{};
        if(!SnapshotNative(lease.binding,native)||lease.revoked||
            native.source!=lease.binding.source.character||native.sourceRuntimeId!=lease.binding.source.runtimeId||
            !DefinitionMatches(lease,native)||
            (native.victim!=0U&&(native.victim!=lease.binding.victim.character||native.victimRuntimeId!=lease.binding.victim.runtimeId)))
            return Fence(lease,output);
        lease.receipt.native=native;lease.captured=true;
    }
    const auto permission=CheckBinding(lease.command,lease.binding);
    if(permission==Check::Waiting){output=lease.receipt;return Result::Pending;}
    if(permission!=Check::Ready||lease.revoked)return Fence(lease,output);
    const auto& native=lease.receipt.native;
    if(native.linked&&native.sourcePrepared&&native.victimAcquired)lease.receipt.phase=Phase::Running;
    output = lease.receipt;
    return lease.receipt.phase == Phase::Running ? Result::Admitted : Result::Pending;
}
Result NativePairAdapter::Inspect(const Key& inputKey, Receipt& published) noexcept {
    const auto key=inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    auto* lease = FindLease(key);
    if (lease == nullptr) return key.scope == scope_ ? Result::Rejected : Result::WrongScope;
    output = lease->receipt;
    if (output.resourcesReleased) return Result::Released;
    if (output.phase == Phase::Fenced) return Result::Unknown;
    if (output.phase == Phase::Interrupted) return Result::Pending;
    Busy guard(busy_);
    if(!output.transitionEntered||!lease->captured)return AdvanceAdmission(*lease,output);
    const auto permission=CheckBinding(lease->command,lease->binding);
    if(permission==Check::Waiting)return Result::Pending;
    if(permission!=Check::Ready)return Fence(*lease,output);
    PairState current{};
    std::uint32_t failure{};
    if(!SnapshotNative(lease->binding,current))failure|=1U;
    else {
        if(current.source!=lease->binding.source.character||current.sourceRuntimeId!=lease->binding.source.runtimeId)failure|=2U;
        if(current.sourceCycle!=output.native.sourceCycle)failure|=4U;
        if(current.sourceSerial!=output.native.sourceSerial)failure|=8U;
        if(!DefinitionMatches(*lease,current))failure|=16U;
        if(current.victim!=0U&&(current.victim!=lease->binding.victim.character||current.victimRuntimeId!=lease->binding.victim.runtimeId))failure|=32U;
        if(output.native.linked&&(!current.linked||current.victimCycle!=output.native.victimCycle||current.victimSerial!=output.native.victimSerial))failure|=64U;
    }
    if(lease->revoked)failure|=128U;
    if(failure){lease->receipt.failureReason=failure;return Fence(*lease,output);}
    lease->receipt.native = current;
    if ((current.sourceFlags & 4U) != 0U || (current.linked && (current.victimFlags & 4U) != 0U)) {
        lease->receipt.phase = Phase::Interrupted; output = lease->receipt; return Result::Pending;
    }
    if (current.sourceFinished) { lease->receipt.sourceCompleted = true; lease->receipt.sourceCause = FinishCause::NativeSourceFinish; }
    if (current.victimEnded) { lease->receipt.victimCompleted = true; lease->receipt.victimCause = FinishCause::NativeVictimFinish; }
    if (lease->receipt.sourceCompleted && lease->receipt.victimCompleted && lease->receipt.outcomeApplied)
        lease->receipt.phase = Phase::Completed;
    else if (current.linked && current.sourcePrepared && current.victimAcquired) lease->receipt.phase = Phase::Running;
    output = lease->receipt;
    return output.phase == Phase::Completed ? Result::Completed : Result::Pending;
}
Result NativePairAdapter::Observe(const Key& inputKey, const std::uint64_t serial, const LocalParticipant& participant,
    const std::uintptr_t cycle, const std::uint8_t cycleSerial, const FinishCause cause, Receipt& published) noexcept {
    const auto key=inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    auto* lease = FindLease(key);
    if (lease == nullptr) return Result::Rejected;
    output = lease->receipt;
    if (output.serial != serial || output.resourcesReleased || cause == FinishCause::None) return Result::Rejected;
    const bool source = participant == lease->binding.source && cycle == output.native.sourceCycle && cycleSerial == output.native.sourceSerial;
    const bool victim = participant == lease->binding.victim && cycle == output.native.victimCycle && cycleSerial == output.native.victimSerial;
    if ((!source && !victim) || (source && cause == FinishCause::NativeVictimFinish) ||
        (victim && cause == FinishCause::NativeSourceFinish)) return Result::Rejected;
    if (cause == FinishCause::NativeDestruction || cause == FinishCause::NativeInterruption) {
        lease->receipt.phase = Phase::Interrupted;
        if (source) lease->receipt.sourceCause = cause; else lease->receipt.victimCause = cause;
    } else {
        // NativeSourceFinish may decline at 0077A180/+3C; and victim finish
        // may be teardown. Reinspect exact native facts before recording it.
        const auto result = Inspect(key, output);
        if (result == Result::Unknown || result == Result::Rejected) return result;
    }
    output = lease->receipt;
    return output.phase == Phase::Completed ? Result::Completed : Result::Pending;
}
Result NativePairAdapter::AdvanceOutcome(const Key& key, const std::uint64_t serial, const Outcome& outcome) noexcept {
    if (busy_) return Result::Reentrant;
    auto* lease = FindLease(key);
    if (lease == nullptr || lease->receipt.serial != serial || lease->receipt.resourcesReleased) return Result::Rejected;
    if (lease->revoked || lease->receipt.phase == Phase::Fenced) return Result::Unknown;
    if (!lease->command.outcomeRequired || !OutcomeValid(outcome)) return Result::Rejected;
    const auto& previous = lease->outcome;
    if (outcome.revision == previous.revision) return outcome == previous ? Result::Duplicate : Result::Conflict;
    if (outcome.revision < previous.revision || !SamePreparation(previous, outcome) ||
        (previous.flags & ~outcome.flags) != 0U) return Result::Conflict;
    auto& receipt = lease->receipt;
    // Immutable preparation and an actually executed native death survive
    // metadata progress. A changed reaction requires a new exact application.
    if (outcome.hpReaction != previous.hpReaction || outcome.healthReaction != previous.healthReaction ||
        ((outcome.flags ^ previous.flags) & 2U) != 0U) receipt.observedOutcomeEvidence &= ~ReactionApplied;
    lease->outcome = outcome; receipt.outcomeRevision = outcome.revision;
    receipt.requiredOutcomeEvidence = RequiredEvidence(outcome);
    RefreshOutcome(receipt);
    return Result::Admitted;
}
Result NativePairAdapter::OutcomeApplied(const Key& key, const std::uint64_t serial,
    const Outcome& outcome, const std::uint32_t evidence) noexcept {
    if (busy_) return Result::Reentrant;
    auto* lease = FindLease(key);
    if (lease == nullptr || lease->receipt.serial != serial || lease->receipt.resourcesReleased) return Result::Rejected;
    if (lease->revoked || lease->receipt.phase == Phase::Fenced) return Result::Unknown;
    auto& receipt = lease->receipt;
    const bool retainedPreparation=evidence==PreparationApplied&&outcome.revision<lease->outcome.revision&&
        SamePreparation(outcome,lease->outcome);
    if (!lease->command.outcomeRequired || (outcome != lease->outcome&&!retainedPreparation) || evidence == 0U ||
        (evidence & ~receipt.requiredOutcomeEvidence) != 0U) return Result::Rejected;
    const auto previous = receipt.observedOutcomeEvidence;
    receipt.observedOutcomeEvidence |= evidence; RefreshOutcome(receipt);
    return previous == receipt.observedOutcomeEvidence ? Result::Duplicate : Result::Admitted;
}
Result NativePairAdapter::Release(const Key& inputKey, ReleaseProof proof, void* context, Receipt& published) noexcept {
    const auto key=inputKey;
    ReceiptPublication publication{published};
    auto& output=publication.value;
    if (busy_) return Result::Reentrant;
    auto* lease = FindLease(key);
    if (lease == nullptr) return Result::Rejected;
    output = lease->receipt;
    if (output.resourcesReleased) return Result::Duplicate;
    if (proof == nullptr || (output.phase != Phase::Completed && output.phase != Phase::Interrupted && output.phase != Phase::Fenced)) return Result::Waiting;
    Busy guard(busy_);
    const auto revision = revocation_;
    const auto released = proof(context, lease->command, lease->binding, lease->receipt);
    if (revision != revocation_) return Fence(*lease, output);
    if (released == Check::Unknown) return Fence(*lease, output);
    if (released != Check::Ready) return released == Check::Waiting ? Result::Waiting : Result::Rejected;
    lease->receipt.resourcesReleased = true; lease->receipt.phase = Phase::Released;
    output = lease->receipt;
    return Result::Released;
}
void NativePairAdapter::Invalidate(const LocalParticipant& actor) noexcept {
    // Also fence an in-progress admission before its lease is installed.
    ++revocation_;
    for (std::size_t i = 0; i < count_; ++i) {
        auto& lease = leases_[i];
        if (!lease.receipt.resourcesReleased && (lease.binding.source == actor || lease.binding.victim == actor)) {
            lease.revoked = true; lease.receipt.phase = Phase::Fenced;
        }
    }
}
bool NativePairAdapter::Find(const Key& key, Receipt& output) const noexcept {
    output = {};
    for (std::size_t i = 0; i < count_; ++i) if (leases_[i].command.key == key) { output = leases_[i].receipt; return true; }
    return false;
}
} // namespace aimp::ai_sync::interactions
