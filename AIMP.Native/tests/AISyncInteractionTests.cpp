#include "aimp/AISyncInteraction.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace aimp::ai_sync::interactions {
struct NativePairAdapterTestAccess {
    static void Prepare(NativePairAdapter& adapter, void* context,
        Check(*binding)(void*,const Command&,const Binding&) noexcept,
        bool(*definition)(void*,const Binding&,const Command&,Definition&) noexcept,
        bool(*registration)(void*,const Binding&,const Definition&,bool&) noexcept,
        bool(*transition)(void*,const Binding&,const Command&) noexcept,
        bool(*snapshot)(void*,const Binding&,PairState&) noexcept,
        bool(*definitionLive)(void*,const Binding&,const Definition&) noexcept) {
        adapter.primitivesContext_ = context; adapter.bindingPrimitive_ = binding;
        adapter.definitionPrimitive_ = definition; adapter.registerPrimitive_ = registration;
        adapter.transitionPrimitive_ = transition; adapter.snapshotPrimitive_ = snapshot;
        adapter.definitionLivePrimitive_ = definitionLive;
    }
};
}
using namespace aimp::ai_sync;
using namespace interactions;
namespace {
void CheckValue(const bool condition, const int line) { if (!condition) { std::cerr << "interaction invariant at " << line << '\n'; std::abort(); } }
#define CHECK(value) CheckValue((value), __LINE__)
transactions::Scope Scope() {
    transactions::Scope scope{}; scope.session[0] = 7U;
    scope.authorityEpoch = 1U; scope.worldEpoch = 2U; scope.consumerEpoch = 3U; return scope;
}
Command Value() {
    Command command{}; command.key = {Scope(),11U}; command.source = {0U,1U,2U,1U,3U};
    command.victim = {19U,0U,0U,2U,0U}; command.pairRevision = 3U; command.transitionRevision = 4U;
    command.sourceDefinition = 2; command.outcomeRequired = true;
    command.outcome.revision = 1U; command.outcome.flags = 3U;
    command.outcome.hpBefore = command.outcome.hpPrepared = command.outcome.hpReaction = 100U;
    return command;
}
Binding Local() {
    Binding binding{}; binding.source = {0x100000U,15U,1U,2U,Perspective::Npc};
    binding.victim = {0x200000U,16U,3U,4U,Perspective::RemotePlayer,10U,2U};
    binding.exclusiveExecution = binding.outcomeBound = true; return binding;
}
struct Harness {
    NativePairAdapter adapter;
    Command command{Value()}; Binding binding{Local()}; PairState state{};
    Check permission{Check::Ready}, live{Check::Ready}, release{Check::Ready};
    bool preparedVariant{}, definition{true}, stableDefinition{true}, registration{true}, accepted{true}, transition{true}, snapshot{true};
    bool revokeRegister{}, revokeTransition{}, revokeRelease{}, revokeDefinition{}, reenter{};
    bool mutateInput{};
    const Receipt* published{};
    bool waitAfterRegister{},waitAfterTransition{};
    unsigned registers{}, transitions{}, snapshots{}, releases{};
    Harness() {
        state.source = binding.source.character; state.victim = binding.victim.character;
        state.sourceRuntimeId = binding.source.runtimeId; state.victimRuntimeId = binding.victim.runtimeId;
        state.sourceCycle = 0x110000U; state.victimCycle = 0x210000U;
        state.sourceSerial = 4U; state.victimSerial = 7U;
        state.sourceGraph = 0x130000U; state.victimGraph = 0x230000U;
        state.definition = 0x140000U; state.stage = 1U;
        state.sourcePrepared = state.victimAcquired = state.linked = true;
        CHECK(adapter.Initialize(0x400000U,true,{},&Permit,this));
        NativePairAdapterTestAccess::Prepare(adapter,this,&Live,&DefinitionCall,&Register,&TransitionCall,&Snapshot,&DefinitionLive);
        CHECK(adapter.Configure(Scope()) == Result::Admitted);
    }
    static Check Permit(void* context, const Command&, const Binding&) noexcept {
        const auto& h=*static_cast<Harness*>(context);
        if(h.published&&(!h.published->pairEntered||h.published->key!=h.command.key))return Check::Waiting;
        return h.permission;
    }
    static Check Live(void* context, const Command&, const Binding&) noexcept { return static_cast<Harness*>(context)->live; }
    static bool DefinitionCall(void* context, const Binding&, const Command& command, Definition& output) noexcept {
        auto& h = *static_cast<Harness*>(context);
        if (h.revokeDefinition) h.adapter.Invalidate(h.binding.source);
        if (h.mutateInput) {h.command.sourceDefinition=99;h.binding.victim.character+=0x1000U;}
        output = {0x180000U,(h.preparedVariant&&static_cast<std::uint32_t>(command.sourceDefinition)==h.command.outcome.definition)?0x150000U:0x140000U,static_cast<std::uint32_t>(command.sourceDefinition),0U,4}; return h.definition;
    }
    static bool DefinitionLive(void* context, const Binding&, const Definition&) noexcept { return static_cast<Harness*>(context)->stableDefinition; }
    static bool Register(void* context, const Binding& binding, const Definition&, bool& accepted) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.registers; accepted = h.accepted;
        if (h.reenter) { Receipt ignored{}; CHECK(h.adapter.Admit(h.command,h.binding,ignored) == Result::Reentrant); }
        if (h.revokeRegister) h.adapter.Invalidate(binding.victim);
        if(h.waitAfterRegister)h.permission=Check::Waiting;
        return h.registration;
    }
    static bool TransitionCall(void* context, const Binding& binding, const Command&) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.transitions;
        if (h.revokeTransition) h.adapter.Invalidate(binding.source);
        if(h.waitAfterTransition)h.permission=Check::Waiting;
        return h.transition;
    }
    static bool Snapshot(void* context, const Binding&, PairState& output) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.snapshots; output = h.state; return h.snapshot;
    }
    static Check Release(void* context, const Command&, const Binding& binding, const Receipt&) noexcept {
        auto& h = *static_cast<Harness*>(context); ++h.releases;
        if (h.revokeRelease) h.adapter.Invalidate(binding.source);
        return h.release;
    }
    Result Admit(Receipt& output) { return adapter.Admit(command,binding,output); }
};
void AdmissionAndCompletion() {
    Harness borrowed;borrowed.mutateInput=true;const auto originalCommand=borrowed.command;const auto originalBinding=borrowed.binding;Receipt frozen{};
    CHECK(borrowed.Admit(frozen)==Result::Admitted&&borrowed.command.sourceDefinition==99);
    CHECK(borrowed.adapter.Admit(originalCommand,originalBinding,frozen)==Result::Duplicate);
    Harness definitionMismatch;definitionMismatch.state.definition+=4U;
    CHECK(definitionMismatch.Admit(frozen)==Result::Unknown&&definitionMismatch.registers==1U&&definitionMismatch.transitions==1U);
    CHECK(definitionMismatch.Admit(frozen)==Result::Duplicate&&definitionMismatch.registers==1U);
    Harness unsupported;unsupported.command.transition=Transition::B;Receipt rejected{};
    CHECK(unsupported.Admit(rejected)==Result::Rejected&&unsupported.registers==0&&unsupported.transitions==0);
    CHECK(!unsupported.adapter.Find(unsupported.command.key,rejected));
    Harness h; Receipt receipt{};
    CHECK(h.Admit(receipt) == Result::Admitted); CHECK(h.registers == 1U && h.transitions == 1U);
    CHECK(receipt.phase == Phase::Running && !receipt.outcomeApplied);
    CHECK(h.Admit(receipt) == Result::Duplicate); CHECK(h.registers == 1U && h.transitions == 1U);
    auto changed = h.command; changed.firstFlag = 1U;
    CHECK(h.adapter.Admit(changed,h.binding,receipt) == Result::Conflict);
    CHECK(h.adapter.Find(h.command.key,receipt)); const auto serial = receipt.serial;
    CHECK(h.adapter.Release(h.command.key,&Harness::Release,&h,receipt) == Result::Waiting);
    h.state.victimEnded = true;
    CHECK(h.adapter.Observe(h.command.key,serial,h.binding.victim,h.state.victimCycle,h.state.victimSerial,
        FinishCause::NativeVictimFinish,receipt) == Result::Pending);
    CHECK(receipt.victimCompleted && !receipt.sourceCompleted);
    h.state.sourceFinished = true;
    CHECK(h.adapter.Inspect(h.command.key,receipt) == Result::Pending); // Host consequence still outstanding.
    CHECK(h.adapter.OutcomeApplied(h.command.key,serial,h.command.outcome,PreparationApplied | ReactionApplied) == Result::Admitted);
    CHECK(h.adapter.Inspect(h.command.key,receipt) == Result::Completed);
    CHECK(!receipt.resourcesReleased);
    CHECK(h.adapter.Release(h.command.key,&Harness::Release,&h,receipt) == Result::Released);
    CHECK(h.adapter.Release(h.command.key,&Harness::Release,&h,receipt) == Result::Duplicate);
    CHECK(h.releases == 1U);
    CHECK(h.adapter.Observe(h.command.key,serial,h.binding.victim,h.state.victimCycle,h.state.victimSerial,
        FinishCause::NativeVictimFinish,receipt) == Result::Rejected);
}
void CallbackPublication() {
    Harness h;Receipt receipt{};CHECK(h.Admit(receipt)==Result::Admitted);
    h.published=&receipt;
    // These are the actual callback-capable public operations, with the same
    // receipt/key alias used by the engine integration. A callback must never
    // observe an empty admission while a result is being assembled.
    CHECK(h.adapter.Inspect(receipt.key,receipt)==Result::Pending);
    CHECK(h.adapter.Observe(receipt.key,receipt.serial,h.binding.source,h.state.sourceCycle,h.state.sourceSerial,
        FinishCause::NativeSourceFinish,receipt)==Result::Pending);
    CHECK(receipt.pairEntered&&receipt.phase!=Phase::Fenced);
    h.state.sourceFlags=6U;
    CHECK(h.adapter.Observe(receipt.key,receipt.serial,h.binding.source,h.state.sourceCycle,h.state.sourceSerial,
        FinishCause::NativeGraphEnd,receipt)==Result::Pending);
    CHECK(receipt.phase==Phase::Interrupted);
    CHECK(h.adapter.Release(receipt.key,&Harness::Release,&h,receipt)==Result::Released);
    CHECK(h.registers==1U&&h.transitions==1U&&h.releases==1U);
}
void AdmissionWaits() {
    for(const bool beforeTransition:{true,false}) {
        Harness h;Receipt receipt{};
        h.waitAfterRegister=beforeTransition;h.waitAfterTransition=!beforeTransition;
        CHECK(h.Admit(receipt)==Result::Pending&&receipt.pairEntered&&receipt.phase==Phase::Registered);
        CHECK(h.registers==1U&&h.transitions==(beforeTransition?0U:1U));
        // Contention remains Pending. Repeated pumps cannot redo registration
        // or an already entered transition, and cannot synthesize completion.
        for(unsigned i=0;i<3U;++i)CHECK(h.adapter.Inspect(receipt.key,receipt)==Result::Pending);
        h.waitAfterRegister=h.waitAfterTransition=false;h.permission=Check::Ready;
        const auto resumed=h.adapter.Inspect(receipt.key,receipt);
        CHECK((resumed==Result::Admitted||resumed==Result::Pending)&&receipt.phase==Phase::Running);
        CHECK(h.registers==1U&&h.transitions==1U&&!receipt.resourcesReleased);
        h.permission=Check::Waiting;
        CHECK(h.adapter.Inspect(receipt.key,receipt)==Result::Pending&&receipt.phase==Phase::Running);
        h.permission=Check::Rejected;
        CHECK(h.adapter.Inspect(receipt.key,receipt)==Result::Unknown&&receipt.phase==Phase::Fenced);
    }
}
void FencesAndIdentity() {
    for (unsigned test = 0U; test < 5U; ++test) {
        Harness h; Receipt receipt{};
        if (test == 0U) h.registration = false;
        if (test == 1U) h.transition = false;
        if (test == 2U) h.snapshot = false;
        if (test == 3U) h.revokeRegister = true;
        if (test == 4U) h.revokeTransition = true;
        CHECK(h.Admit(receipt) == Result::Unknown); CHECK(receipt.phase == Phase::Fenced);
        CHECK(h.Admit(receipt) == Result::Duplicate); CHECK(h.registers == 1U && h.transitions <= 1U);
        auto next = h.command; ++next.key.command;
        CHECK(h.adapter.Admit(next,h.binding,receipt) == Result::Waiting);
        auto newScope = Scope(); ++newScope.worldEpoch;
        CHECK(h.adapter.Configure(newScope) == Result::Waiting);
        CHECK(h.adapter.Release(h.command.key,&Harness::Release,&h,receipt) == Result::Released);
        CHECK(h.adapter.Configure(newScope) == Result::Admitted);
    }
    Harness h; Receipt receipt{}; h.reenter = true; CHECK(h.Admit(receipt) == Result::Admitted);
    const auto serial = receipt.serial;
    CHECK(h.adapter.Observe(h.command.key,serial+1U,h.binding.source,h.state.sourceCycle,h.state.sourceSerial,
        FinishCause::NativeSourceFinish,receipt) == Result::Rejected);
    CHECK(h.adapter.Observe(h.command.key,serial,h.binding.source,h.state.sourceCycle,h.state.sourceSerial+1U,
        FinishCause::NativeSourceFinish,receipt) == Result::Rejected);
    CHECK(h.adapter.Observe(h.command.key,serial,h.binding.victim,h.state.victimCycle,h.state.victimSerial,
        FinishCause::NativeDestruction,receipt) == Result::Pending);
    CHECK(receipt.phase == Phase::Interrupted && !receipt.victimCompleted);
    h.revokeRelease = true;
    CHECK(h.adapter.Release(h.command.key,&Harness::Release,&h,receipt) == Result::Unknown);
    CHECK(!receipt.resourcesReleased);
}
void PrerequisitesAndReuse() {
    Harness h; Receipt receipt{};
    h.permission = Check::Waiting; CHECK(h.Admit(receipt) == Result::Waiting); CHECK(h.registers == 0U);
    h.permission = Check::Ready; h.definition = false;
    CHECK(h.Admit(receipt) == Result::Waiting); CHECK(h.registers == 0U);
    h.definition = true; h.stableDefinition = false;
    CHECK(h.Admit(receipt) == Result::Waiting); CHECK(h.registers == 0U);
    h.stableDefinition = true; h.binding.outcomeBound = false;
    CHECK(h.Admit(receipt) == Result::Rejected); CHECK(h.registers == 0U);
    h.binding.outcomeBound = true; h.command.victim.birth = 0U;
    CHECK(h.Admit(receipt) == Result::Rejected); h.command.victim.birth = 2U;
    CHECK(h.Admit(receipt) == Result::Admitted);
    ++h.state.sourceSerial; CHECK(h.adapter.Inspect(h.command.key,receipt) == Result::Unknown);
    CHECK(receipt.phase == Phase::Fenced);
}
void PreEntryRevocation() {
    Harness h; Receipt receipt{}; h.revokeDefinition = true;
    CHECK(h.Admit(receipt) == Result::Unknown && h.registers == 0 && h.transitions == 0 && !receipt.pairEntered);
}
struct NativeMemory {
    std::map<std::uintptr_t,std::uint8_t> bytes;
    template<class T> void Put(std::uintptr_t address, const T value) {
        const auto* source = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0U; i < sizeof(T); ++i) bytes[address+i] = source[i];
    }
    static bool Read(void* context, std::uintptr_t address, void* output, std::size_t size) noexcept {
        auto& memory = *static_cast<NativeMemory*>(context); auto* target = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0U; i < size; ++i) { const auto found = memory.bytes.find(address+i); if (found == memory.bytes.end()) return false; target[i] = found->second; }
        return true;
    }
};
void PreparedDefinitionTransition() {
    Harness h;h.preparedVariant=true;Receipt receipt{};
    CHECK(h.Admit(receipt)==Result::Admitted);
    h.state.definition=0x150000U;
    CHECK(h.adapter.Inspect(h.command.key,receipt)==Result::Pending);
    CHECK(h.registers==1U&&h.transitions==1U&&receipt.native.definition==0x150000U);
    h.state.definition=0x140000U;
    CHECK(h.adapter.Inspect(h.command.key,receipt)==Result::Unknown&&receipt.failureReason==16U);
    Harness unrelated;unrelated.preparedVariant=true;
    CHECK(unrelated.Admit(receipt)==Result::Admitted);unrelated.state.definition=0x160000U;
    CHECK(unrelated.adapter.Inspect(unrelated.command.key,receipt)==Result::Unknown&&receipt.failureReason==16U);
    Harness incarnation;incarnation.preparedVariant=true;
    CHECK(incarnation.Admit(receipt)==Result::Admitted);incarnation.state.definition=0x150000U;++incarnation.state.sourceSerial;
    CHECK(incarnation.adapter.Inspect(incarnation.command.key,receipt)==Result::Unknown&&(receipt.failureReason&8U)!=0U);
}
void NativeLayoutAndOwnership() {
    NativeMemory m;
    constexpr std::uint32_t base=0x400000U,source=0x100000U,victim=0x200000U,sc=0x110000U,vc=0x210000U,sa=0x120000U,va=0x220000U;
    m.Put(source+0x50U,15U); m.Put(victim+0x50U,16U); m.Put(source+0x36CU,sc); m.Put(victim+0x36CU,vc);
    m.Put(sc,base+0xEAE5D0U); m.Put(sc+4U,source); m.Put(sc+0x1AU,std::uint8_t{0x4E}); m.Put(sc+0x19U,std::uint8_t{4});
    m.Put(sc+8U,std::uint8_t{}); m.Put(sc+0xE0U,1U); m.Put(sc+0xE7U,std::uint8_t{1}); m.Put(sc+0xE6U,std::uint8_t{});
    m.Put(sc+0x4CU,0x140000U); m.Put(sc+0x44U,victim); m.Put(sc+0x48U,16U); m.Put(sc+0x14U,sa); m.Put(sa+0x50U,0x130000U);
    m.Put(vc,base+0xEAE690U); m.Put(vc+4U,victim); m.Put(vc+0x1AU,std::uint8_t{0x4F}); m.Put(vc+0x19U,std::uint8_t{7});
    m.Put(vc+8U,std::uint8_t{}); m.Put(vc+0x68U,source); m.Put(vc+0xB0U,sc); m.Put(vc+0x6CU,15U);
    m.Put(vc+0xD9U,std::uint8_t{1}); m.Put(vc+0xD8U,std::uint8_t{}); m.Put(vc+0x14U,va); m.Put(va+0x50U,0x230000U);
    const Memory memory{base,&m,&NativeMemory::Read}; PairState state{};
    CHECK(InspectNativePair(memory,source,15U,state)); CHECK(OwnsSource(state));
    m.Put(vc+0xD8U,std::uint8_t{1});
    CHECK(InspectNativePair(memory,source,15U,state)); CHECK(OwnsSource(state)); // Victim ended is not source completion.
    m.Put(sc+0xE6U,std::uint8_t{1}); CHECK(InspectNativePair(memory,source,15U,state)); CHECK(!OwnsSource(state));
    m.Put(sc+0xE6U,std::uint8_t{}); m.Put(victim+0x50U,17U);
    CHECK(!InspectNativePair(memory,source,15U,state)); CHECK(state.source == 0U);
    m.Put(victim+0x50U,16U); m.Put(vc+0xB0U,sc+4U);
    CHECK(InspectNativePair(memory,source,15U,state)); CHECK(!state.linked && !OwnsSource(state));
}
}
int main() {
    CHECK(DecodePairPayloadHigh(0U)==0U); CHECK(DecodePairPayloadHigh(1U)==1U);
    CHECK(DecodePairPayloadHigh(0x12345600U)==0U); CHECK(DecodePairPayloadHigh(0x12345601U)==1U);
    CHECK(DecodePairPayloadHigh(0x12345602U)==2U); CHECK(DecodePairPayloadHigh(0x123456FFU)==255U);
    for(const auto raw:{0x12345600U,0x12345601U,0x12345602U,0x123456FFU}) {
        Harness h; Receipt receipt{};h.binding.localPairFlagsHigh=DecodePairPayloadHigh(raw);
        const auto admitted=h.Admit(receipt);
        if((raw&0xFFU)<=1U) {CHECK(admitted==Result::Admitted);CHECK(h.registers==1U&&h.transitions==1U);}
        else {CHECK(admitted==Result::Rejected);CHECK(h.registers==0U&&h.transitions==0U);}
    }
    AdmissionAndCompletion(); CallbackPublication(); AdmissionWaits(); FencesAndIdentity(); PrerequisitesAndReuse(); PreEntryRevocation(); PreparedDefinitionTransition(); NativeLayoutAndOwnership();
    std::cout << "AISync native pair adapter and 00d6 layout invariants passed; runtime rendering/audio/physics UNPROVEN.\n";
}
