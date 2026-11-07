#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncExecutionCodec.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncRouteExecution.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <bit>
#include <new>

// 00d6: 0070A940 producer; 00683BF0/00683510 factories;
// 006777B0 initial goal; 006C7550 pending goal; 0067D0D0 stop.
// Only typed producer inputs are written. The constructor/scheduler own all
// refs, lists, callbacks, path state and locomotion. No pose correction here.
#include <atomic>
namespace aimp::ai_sync::execution {
namespace {
using namespace transactions;
std::atomic<std::uint64_t> gUpdatesAfterLocalEnd{},gHostPathSteers{},gFencedRestartsSkipped{};
std::atomic<std::uint64_t> gGuidanceDeferred{},gGuidanceSuspended{},gGuidanceFaults{},gGuidanceWithoutRoute{};
std::atomic<std::uint64_t> gGuidanceContinuations{},gGuidanceContinuationLimits{},gGuidanceExhaustedTailContinuations{};
std::atomic<std::uint64_t> gPacedSamples{},gPacingHoldSamples{},gPacingUnavailable{},gPacingGaitSamples{};
std::atomic<float> gPacingLead{};
std::atomic<std::uint64_t> gPhysicalTrailSamples{};
std::atomic<std::uint64_t> gGuidanceGoalDeposits{},gGuidanceGoalConsumptions{},gGuidanceGoalDiscards{};
std::atomic<std::uint64_t> gGuidanceStopChanges{},gGuidanceStartWaits{},gGuidanceRetainedWaits{},gGuidanceQueueWaits{};
HostProgressQuery gHostProgress{};
void* gHostProgressContext{}; // Installed before the engine starts this module.
std::atomic<MarkerResolve> gMarkerResolve{};
constexpr std::uintptr_t kScheduler=0x2CD0;
struct MarkerBusyScope {
    bool& busy;
    bool previous;
    explicit MarkerBusyScope(bool& value) noexcept : busy(value),previous(value) {busy=true;}
    ~MarkerBusyScope() {busy=previous;}
};
template<class T> bool Read(std::uintptr_t at,T& value) noexcept {
    __try { value=*reinterpret_cast<const T*>(at);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool SameFloat(float a,float b) noexcept {return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);}
bool SameGoal(const std::array<float,3>& a,const std::array<float,3>& b) noexcept {
    for(std::size_t i=0;i<3;++i)if(!SameFloat(a[i],b[i]))return false;
    return true;
}
bool RetainedReferenceAbsent(std::uintptr_t character,std::uintptr_t request) noexcept {
    // 00683700 may retain replaced status5 in this list. Unlike3/4/6,
    // 00683DB0 does not remove a nonactive5. An absent, well-formed ring is
    // the only local proof that dropping its Ref cannot lose that identity.
    std::uint32_t count{};std::uintptr_t head{},entry{};
    if(!Read(character+kScheduler+0x14U,count)||count>NativeRequestAdapter::kCapacity)return false;
    if(!count)return true;
    if(!Read(character+kScheduler+0x10U,head)||!head||!Read(head,entry))return false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t held{},next{};
        if(!entry||entry==head||!Read(entry+8U,held)||!Read(entry,next)||held==request)return false;
        entry=next;
    }
    return entry==head;
}
bool GuidanceParametersDiffer(const movement::Parameters& a,const movement::Parameters& b) noexcept {
    auto x=a,y=b;
    // Goal and stop variant have their own native setters. Local route/stop
    // lifecycle bits are not a reason to reapply a Host parameter snapshot.
    x.goal=y.goal={};x.stopDistance=y.stopDistance=0.0F;
    x.flags90&=0x3EU;y.flags90&=0x3EU;x.flags91&=0x11U;y.flags91&=0x11U;
    return !movement::Same(x,y);
}
bool MarkerGeometry(const movement::Bundle& a,const movement::Bundle& b) noexcept {
    auto x=a,y=b;
    x.before.retained=x.after.retained=y.before.retained=y.after.retained=0;
    return movement::Same(x,y);
}
bool MarkerKey(const native_intent::Payload& a,const native_intent::Payload& b) noexcept {
    for(std::size_t i=0;i<a.words.size();++i)
        if(i!=12&&i!=13&&a.words[i]!=b.words[i])return false;
    return true;
}
bool Gait(std::uintptr_t character,std::uint32_t requested) noexcept {
    // 00684900/00683E80 author these native inputs. Effective gait, velocity
    // and locomotion remain CATHODE-owned, with the resource pinned by Invoke.
    __try {
        const auto movement=*reinterpret_cast<const std::uintptr_t*>(character+0x42A8);
        if(!movement||requested>3)return false;
        auto& input=*reinterpret_cast<std::uint32_t*>(movement+0x194);
        if(input!=requested){input=requested;*reinterpret_cast<std::uint32_t*>(movement+0x190)=0;}
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
void SetMarkerResolver(MarkerResolve resolver) noexcept {gMarkerResolve.store(resolver,std::memory_order_release);}
void SetHostProgressQuery(HostProgressQuery query,void* context) noexcept {gHostProgress=query;gHostProgressContext=context;}
bool NativeRequestAdapter::MarkerRevalidate(const Lease& lease) noexcept {
    // Binding can be revoked after Submit or from a native callback. Keep the
    // complete operation, including revision and payload, for exact rechecks.
    if(!lease.markerRecord)return false;
    const Record record=*lease.markerRecord;
    const auto resolver=gMarkerResolve.load(std::memory_order_acquire);
    std::uint32_t marker{};
    std::uint32_t requestMarker{};
    return record.scope==lease.scope&&record.owner==lease.owner&&record.commandId==lease.command&&
        record.nativeKind==2&&record.parameterRevision==lease.revision&&record.intent==lease.markerPayload&&
        resolver&&resolver(record,marker)&&marker!=UINT32_MAX&&marker==lease.localMarker&&
        Read(lease.ref.object+0xB0U,requestMarker)&&requestMarker==lease.localMarker;
}
bool NativeRequestAdapter::MarkerFields(Lease& lease,bool after) noexcept {
    __try {
        if(!after)*reinterpret_cast<std::uint32_t*>(lease.ref.object+0xB0)=lease.localMarker;
        *reinterpret_cast<std::uint8_t*>(lease.ref.object+0xB4)=
            static_cast<std::uint8_t>(lease.markerPayload.words[after?13:12]);
        *reinterpret_cast<std::uint8_t*>(lease.ref.object+0x18)=
            static_cast<std::uint8_t>(after?lease.movement.after.retained:lease.movement.before.retained);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MarkerInsert(Lease& lease) noexcept {
    __try {
        const auto block=lease.ref.object+0x1CU;
        const auto& p=lease.movement.before;
        // Actual00440080 also authors the shadow destination. Never use the
        // Move request setter006777B0 or the context-reading00681810 here.
        if(markerGoal_) {
            if(!markerGoal_(movementCallsContext_,block,p.goal.data()))return false;
        } else reinterpret_cast<void(__thiscall*)(void*,const float*)>(base_+0x40080U)(
            reinterpret_cast<void*>(block),p.goal.data());
        if(!MarkerRevalidate(lease)||!NavigationRevalidate(lease)||lease.routeRejected)return false;
        *reinterpret_cast<std::uint32_t*>(block+0x50)=p.style;
        *reinterpret_cast<float*>(block+0x2C)=p.stoppingDistance;
        *reinterpret_cast<float*>(block+0x30)=p.stopDistance;
        for(std::size_t i=0;i<9;++i)*reinterpret_cast<float*>(block+0x58+i*4)=p.query[i];
        auto& f90=*reinterpret_cast<std::uint8_t*>(block+0x90);
        auto& f91=*reinterpret_cast<std::uint8_t*>(block+0x91);
        f90=static_cast<std::uint8_t>((f90&~0x3FU)|(p.flags90&0x3FU));
        f91=static_cast<std::uint8_t>((f91&~0x31U)|(p.flags91&0x31U));
        const auto navigation=lease.movementResource.object+0xC4U;
        if(markerInsertion_)return markerInsertion_(movementCallsContext_,navigation,block);
        reinterpret_cast<void(__thiscall*)(void*,void*)>(base_+0x4D190U)(
            reinterpret_cast<void*>(navigation),reinterpret_cast<void*>(block));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
MarkerInitialization NativeRequestAdapter::InitializeMarker(std::uintptr_t request,std::uintptr_t character,
    bool currentLifetime) noexcept {
    Lease* selected{};
    for(auto& lease:leases_)if(lease.used&&lease.ref.object==request) {selected=&lease;break;}
    if(!selected) {
        // An unknown request on an actor with retained marker ownership must
        // not escape into the local context solver.
        bool owned=false;
        for(auto& lease:leases_)if(lease.used&&lease.kind==2&&lease.actor.character==character) {
            lease.routeRejected=true;owned=true;
        }
        return owned?MarkerInitialization::Rejected:MarkerInitialization::Unowned;
    }
    auto& lease=*selected;
    const bool admitting=busy_&&invokingRecord_&&invokingRecord_->scope==lease.scope&&
        invokingRecord_->owner==lease.owner&&invokingRecord_->commandId==lease.command&&
        invokingRecord_->nativeKind==2&&invokingRecord_->opcode==Op::Submit;
    std::uint32_t marker{};
    if(lease.kind!=2||lease.actor.character!=character||!currentLifetime||retiring_||!base_||
        lease.routeRejected||lease.cancelled||lease.markerInitializing||lease.markerInitialized||
        (busy_&&!admitting)||(lease.unknown&&!admitting)||(!lease.admitted&&!admitting)||
        lease.movement.navigation!=movement::Navigation::HostRoute||!NavigationRevalidate(lease)||
        !Read(request+0xB0U,marker)||marker!=lease.localMarker) {
        lease.routeRejected=true;return MarkerInitialization::Rejected;
    }
    const MarkerBusyScope callbackGuard(busy_);
    if(!MarkerRevalidate(lease)||!NavigationRevalidate(lease)||lease.routeRejected) {
        lease.routeRejected=true;return MarkerInitialization::Rejected;
    }
    const character_direct_step_runtime::MovementResourceLease resources(character);
    if(!resources||!NavigationRevalidate(lease)) {lease.routeRejected=true;return MarkerInitialization::Rejected;}
    lease.markerInitializing=true;
    const bool applied=MarkerInsert(lease)&&MarkerRevalidate(lease)&&NavigationRevalidate(lease)&&!lease.routeRejected;
    lease.markerInitializing=false;
    if(!applied) {lease.routeRejected=true;lease.unknown=true;return MarkerInitialization::Rejected;}
    lease.markerInitialized=true;
    return MarkerInitialization::Applied;
}
NavigationOwnership NativeRequestAdapter::IdentifyNavigation(std::uintptr_t navigation,std::uintptr_t block,
    NavigationBinding& output,bool currentLifetime) noexcept {
    for (auto& lease : leases_) if (lease.used && (lease.kind == 1 || lease.kind == 2 || lease.kind == 3) && lease.ref.object &&
        block == lease.ref.object + (lease.kind == 1 ? 0x28U : 0x1CU)) {
        NavigationBinding value{{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision},
            lease.movement.navigation,lease.secondary ? 1U : 0U};
        if (lease.secondary) for (const auto& primary : leases_)
            if (primary.used && !primary.secondary && primary.scope == lease.scope && primary.owner == lease.owner &&
                primary.command == lease.command) value.policy = primary.movement.navigation;
        output = value;
        // A revoked scope still owns its known request. Correlate by retained
        // values and refuse before reading Character/M; never reclassify it as
        // unowned work that could dispatch a competing local solver.
        if (!currentLifetime || retiring_ || !base_) return NavigationOwnership::Rejected;
        const bool invoking=busy_&&invokingRecord_&&invokingRecord_->scope==lease.scope&&
            invokingRecord_->owner==lease.owner&&invokingRecord_->commandId==lease.command&&
            (invokingRecord_->opcode==Op::Submit||invokingRecord_->opcode==Op::Update);
        const bool admitting=invoking||lease.markerInitializing;
        if(invoking){value.command.revision=invokingRecord_->parameterRevision;output=value;}
        // The Host has already admitted a replacing native request. Retained
        // old B/Ref values still belong to their owner for later Cancel/Release,
        // but a delayed worker callback must never regain route authority.
        if (lease.preempted || lease.routeRejected || lease.cancelled || (lease.unknown&&!admitting))
            return NavigationOwnership::Rejected;
        if(busy_&&!admitting)return NavigationOwnership::Waiting;
        LocalActorBinding actor{};
        const auto resolver=invoking&&!steering_?resolve_:(lifecycleResolver_?lifecycleResolver_:resolve_);
        if (!resolver || !resolver(context_,lease.owner,actor) || !actor.exclusiveExecution ||
            !Live(lease,actor) || !lease.movementResource.object ||
            navigation != lease.movementResource.object + 0xC4U) return NavigationOwnership::Rejected;
        if(admitting)return NavigationOwnership::Admitting;
        if(!lease.admitted)return NavigationOwnership::Rejected;
        return NavigationOwnership::Ready;
    }
    // A new/unrecognized B must not turn an already owned strict N into
    // permission to run a local solver. Correlate retained values only; this
    // is a refusal identity, never authority to admit an unknown native block.
    for(const auto& lease:leases_)if(lease.used&&!lease.secondary&&lease.ref.object&&
        (lease.kind==1||lease.kind==2||lease.kind==3)&&movement::CarriesRoute(lease.movement.navigation)&&
        lease.movementResource.object&&navigation==lease.movementResource.object+0xC4U) {
        output={{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision},lease.movement.navigation,0};
        return NavigationOwnership::Rejected;
    }
    return NavigationOwnership::Unowned;
}
bool NativeRequestAdapter::NavigationRevalidate(const Lease& lease) noexcept {
    const auto resolver=lifecycleResolver_?lifecycleResolver_:resolve_;
    LocalActorBinding current{};std::uint32_t runtime{};std::uintptr_t scheduler{};
    return resolver&&resolver(context_,lease.owner,current)&&current==lease.actor&&current.exclusiveExecution&&
        Read(current.character+0x50U,runtime)&&runtime==current.runtimeId&&
        Read(current.character+kScheduler,scheduler)&&scheduler==current.character&&Live(lease,current);
}
bool NativeRequestAdapter::RejectNavigation(const NavigationBinding& binding) noexcept {
    if(busy_||retiring_||!base_||binding.policy!=movement::Navigation::HostRoute)return false;
    Lease* selected{};Lease* primary{};
    for(auto& lease:leases_)if(lease.used&&lease.scope==binding.command.scope&&lease.owner==binding.command.owner&&
        lease.command==binding.command.command&&lease.actor==binding.command.actor) {
        if(!lease.secondary)primary=&lease;
        const RetirementIdentity identity{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision};
        if(identity==binding.command&&binding.part==(lease.secondary?1U:0U))selected=&lease;
    }
    if(!selected||!primary||primary->movement.navigation!=binding.policy)return false;
    // Fence every retained part before Cancel can reenter the route hooks.
    for(auto& lease:leases_)if(lease.used&&lease.scope==binding.command.scope&&lease.owner==binding.command.owner&&
        lease.command==binding.command.command&&lease.actor==binding.command.actor)lease.routeRejected=true;
    busy_=true;
    bool complete=true;
    for(int pass=0;pass<2;++pass)for(auto& lease:leases_) {
        if(!lease.used||lease.scope!=binding.command.scope||lease.owner!=binding.command.owner||
            lease.command!=binding.command.command||lease.actor!=binding.command.actor||lease.secondary!=(pass==0))continue;
        if(lease.unknown||!NavigationRevalidate(lease)){complete=false;continue;}
        if(lease.cancelled)continue;
        const character_direct_step_runtime::MovementResourceLease resources(lease.actor.character);
        if(!resources||!NavigationRevalidate(lease)){complete=false;continue;}
        lease.unknown=true;
        if(!CancelRetiring(lease.ref.object)||!NavigationRevalidate(lease)){complete=false;continue;}
        lease.cancelled=true;lease.unknown=false;
    }
    busy_=false;return complete;
}
bool NativeRequestAdapter::MovementFactory(Lease& lease,std::uint32_t kind) noexcept {
    lease.refCertified=false;lease.releaseEntered=false;
    __try {
        if(movementFactory_) {
            if(!movementFactory_(movementCallsContext_,kind,lease.actor.character+kScheduler,
                reinterpret_cast<std::uintptr_t*>(&lease.ref)))return false;
        } else {
            const auto* control=scheduler_control::Find(kind);
            const auto factory=control?control->factoryRva:
                kind==1?0x283BF0U:kind==2?0x283530U:kind==3?0x283510U:kind==4?0x282450U:
                kind==5?0x282410U:kind==9?0x2823B0U:kind==7?0x2823F0U:kind==kPlacementKind?0x282430U:0U;
            if(!factory)return false;
            reinterpret_cast<Ref*(__thiscall*)(void*,Ref*)>(base_+factory)(
                reinterpret_cast<void*>(lease.actor.character+kScheduler),&lease.ref);
        }
        lease.refCertified=lease.ref.object!=0&&lease.ref.control!=0;
        return lease.refCertified;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementAdmit(Lease& lease,std::uint32_t admission,bool& accepted) noexcept {
    __try {
        // A local continuation only appends; it must never replace a Host
        // action which became runnable while the previous segment finished.
        if(steering_&&admission==0)admission=1;
        if(movementAdmission_)return movementAdmission_(movementCallsContext_,lease.ref.object,admission,accepted);
        const auto submit=admission==0?0x283CE0U:(admission==1?0x283C10U:0x283550U);
        accepted=reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(base_+submit)(
            reinterpret_cast<void*>(lease.ref.object))!=0;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementGoal(Lease& lease,const std::array<float,3>& goal,bool update) noexcept {
    __try {
        if(movementGoal_) {
            if(!movementGoal_(movementCallsContext_,lease.ref.object,goal.data(),update))return false;
        } else reinterpret_cast<void(__thiscall*)(void*,const float*)>(base_+(update?0x2C7550U:0x2777B0U))(
            reinterpret_cast<void*>(lease.ref.object),goal.data());
        if(!update) {
            std::array<float,3> actual{};
            if(!Read(lease.ref.object+0x28U,actual)||!SameGoal(actual,goal))return false;
            lease.executedGoal=actual;lease.executedGoalKnown=true;
        }
        // The update setter is void and can refuse silently. The steerer
        // verifies deposition, then later verifies actual native consumption.
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
movement::Parameters NativeRequestAdapter::ExecutionParameters(Lease& lease,const movement::Parameters& host) noexcept {
    if(lease.kind!=1||lease.movement.navigation!=movement::Navigation::HostGuided)return host;
    std::array<float,3> local{};
    if(!Read(lease.actor.character+0x70U,local)||!movement::FinitePoint(local))return host;
    const NavigationBinding binding{{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision},
        lease.movement.navigation,0U};
    const auto* graph=route_execution::Guidance(binding,host.goal);
    if(!graph)++gGuidanceWithoutRoute;
    movement::HostProgress progress{};
    const bool available=gHostProgress&&gHostProgress(gHostProgressContext,binding.command,progress)&&
        progress.gait<=3U&&movement::FinitePoint(progress.position)&&movement::FinitePoint(progress.velocity);
    lease.pacingSuspended=available&&progress.protectedMotion;
    lease.pacingGaitKnown=available&&!progress.protectedMotion;
    if(lease.pacingGaitKnown)lease.pacingGait=progress.gait;
    if(lease.pacingSuspended)return host;
    const auto selected=movement::SelectGuidedGoal(graph,local,host.goal,lease.guidance);
    float lead{};
    const auto goal=movement::LimitGuidedProgress(graph,local,available?&progress:nullptr,selected,lease.pacing,lead);
    lease.pacingGoalLimited=goal.paced;
    if(available&&progress.observed.valid)++gPhysicalTrailSamples;
    gPacingLead=lead;
    if(!available)++gPacingUnavailable;
    if(goal.paced)++gPacedSamples;
    if(goal.holding)++gPacingHoldSamples;
    auto value=host;value.goal=goal.position;
    // A rolling physical waypoint is a through-goal. Asking retail to brake at
    // every carrier sample amplifies latency and may never catch the Host.
    // A deliberate hold (or stationary Host) retains a real stopping radius.
    const bool through=goal.intermediate||(goal.paced&&!goal.holding&&available&&
        std::hypot(progress.velocity[0],progress.velocity[2])>0.2F);
    if(through) {
        value.stopDistance=value.stoppingDistance=-1.0F;
        // A rolling waypoint is not the Host's final stop. Native arrival
        // bit1 survives goal edits; copying Host bit0 onto it would latch3
        // and make 006C7550 refuse all subsequent goals.
        value.flags91&=~1U;
    } else if(goal.paced)value.stopDistance=value.stoppingDistance=0.15F;
    return value;
}
bool NativeRequestAdapter::MovementStop(Lease& lease,float distance,std::uint8_t variant) noexcept {
    __try {
        if(movementStop_) {
            if(!movementStop_(movementCallsContext_,lease.ref.object,distance,variant))return false;
        } else reinterpret_cast<void(__thiscall*)(void*,float,std::uint8_t)>(base_+0x27D0D0U)(
            reinterpret_cast<void*>(lease.ref.object),distance,variant);
        lease.executedStopDistance=distance;lease.executedStopVariant=variant;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementParameters(Lease& lease,const movement::Parameters& p,bool initial) noexcept {
    __try {
        const auto block=lease.ref.object+(lease.kind==1?0x28U:0x1CU);
        *reinterpret_cast<std::uint8_t*>(lease.ref.object+0x18)=static_cast<std::uint8_t>(p.retained);
        *reinterpret_cast<std::uint32_t*>(block+0x50)=p.style;
        *reinterpret_cast<float*>(block+0x2C)=p.stoppingDistance;
        if(initial) {
            *reinterpret_cast<float*>(block+0x30)=p.stopDistance;
            if(lease.kind==3)for(std::size_t i=0;i<3;++i)*reinterpret_cast<float*>(block+i*4)=p.goal[i];
        }
        for(std::size_t i=0;i<9;++i)*reinterpret_cast<float*>(block+0x58+i*4)=p.query[i];
        auto& f90=*reinterpret_cast<std::uint8_t*>(block+0x90);
        auto& f91=*reinterpret_cast<std::uint8_t*>(block+0x91);
        // 004659D0 consumes bit0 when establishing LOCAL route bounds/cursor.
        // 0070A940 authors it on construction, not on an existing goal edit.
        // A Host snapshot must not rearm or clear that Client lifecycle bit.
        const auto authored90=initial?0x3FU:0x3EU;
        f90=static_cast<std::uint8_t>((f90&~authored90)|(p.flags90&authored90));
        // The stop setter owns bit5 and dirty bit6 after construction.
        const auto mask=initial?0x31U:0x11U;
        f91=static_cast<std::uint8_t>((f91&~mask)|(p.flags91&mask));
        lease.executedStoppingDistance=p.stoppingDistance;
        if(initial) {lease.executedStopDistance=p.stopDistance;lease.executedStopVariant=(p.flags91>>5)&1U;}
        lease.executedArrivalKnown=true;
        lease.executedParameters=p;lease.executedParametersKnown=true;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementRevalidate(const Record& r,Lease& lease) noexcept {
    LocalActorBinding actor{};return Resolve(r,actor)&&Live(lease,actor);
}
namespace {
ExecutionOwnedQuery gExecutionOwned{};
}
void SetHostPoseQuery(HostPoseQuery) noexcept {}
void SetExecutionOwnedQuery(ExecutionOwnedQuery query) noexcept { gExecutionOwned = query; }
bool MovementExecutionOwned(std::uintptr_t character) noexcept {return gExecutionOwned&&gExecutionOwned(character);}
std::uint64_t SteersSuspended() noexcept { return 0; }
std::uint64_t HostProgressCatchups() noexcept { return 0; }
std::uint64_t HostStartHolds() noexcept { return 0; }
void NativeRequestAdapter::MovementUnknown(const Record& r) noexcept {
    for(auto& lease:leases_)if(lease.used&&lease.scope==r.scope&&lease.owner==r.owner&&lease.command==r.commandId)
        lease.unknown=true;
}
void NativeRequestAdapter::FenceSupersededMovements(const Record& current,const LocalActorBinding& actor) noexcept {
    // The retail scheduler's admission=0 performs the actual replacement.
    // Only AFTER it has returned accepted may old local route callbacks lose
    // authority. Do not Cancel or RefRelease here: the Host's exact terminal
    // records still own native cleanup, including any retained worker Ref.
    if(steering_||current.admission!=0 || PreserveNativeOperations(current.nativeKind))return;
    for(auto& lease:leases_)
        if(lease.used&&lease.scope==current.scope&&lease.owner==current.owner&&lease.actor==actor&&
            lease.command!=current.commandId&&!PreserveNativeOperations(lease.kind))
            lease.preempted=true;
}
Readiness NativeRequestAdapter::CheckMovement(const Record& r,const LocalActorBinding& actor) noexcept {
    auto* primary=Find(r);
    if(r.nativeKind==2&&(r.opcode==Op::Submit||r.opcode==Op::Update)) {
        std::uint32_t marker{};const auto resolver=gMarkerResolve.load(std::memory_order_acquire);
        auto after=r.movement.after;after.retained=r.movement.before.retained;
        if(r.movement.parts!=1||r.movement.navigation!=movement::Navigation::HostRoute||
            !movement::Same(after,r.movement.before)||!resolver||!resolver(r,marker)||marker==UINT32_MAX)
            return Readiness::Rejected;
        if(primary&&(!MarkerKey(r.intent,primary->markerPayload)||marker!=primary->localMarker||
            !MarkerGeometry(r.movement,primary->movement)))return Readiness::Rejected;
    }
    if(!primary) {
        if(r.opcode!=Op::Submit)return Readiness::Rejected;
        // Host-guided movement requires its path capture/value provider.
        if(r.movement.navigation==movement::Navigation::HostRoute&&
            !(movementRoutesAvailable_?movementRoutesAvailable_():route_runtime::Available())){waitReason_=5U;return Readiness::Waiting;}
        if(!SameGoal(r.movement.before.goal,r.movement.after.goal))return Readiness::Rejected;
        if(r.nativeKind==3) {
            auto after=r.movement.after;after.retained=r.movement.before.retained;
            if(!movement::Same(after,r.movement.before))return Readiness::Rejected;
        }
        // admission 0 replaces natively (00683700 replace=1), as on the Host.
        if(kCapacity-ActiveCount()<r.movement.parts)return Readiness::Rejected;
        if(r.nativeKind==1&&r.movement.navigation==movement::Navigation::HostGuided) {
            // A first carrier often equals the Client body. Retain the Host
            // Submit as Waiting instead of constructing a zero-length Move
            // which finishes before the first usable route/progress sample.
            Lease probe{};probe.scope=r.scope;probe.owner=r.owner;probe.command=r.commandId;
            probe.actor=actor;probe.kind=1;probe.revision=r.parameterRevision;probe.movement=r.movement;
            std::array<float,3> local{};
            const auto parameters=ExecutionParameters(probe,r.movement.after);
            if(!Read(actor.character+0x70U,local)||probe.pacingSuspended||probe.pacing.holding||
                !movement::native_guidance::MeaningfulTarget(local,parameters.goal,parameters.stopDistance,parameters.stoppingDistance)) {
                waitReason_=12U;++gGuidanceStartWaits;return Readiness::Waiting;
            }
        }

    } else {
        if(primary->preempted&&r.opcode==Op::Update)return Readiness::Rejected;
        if(primary->routeRejected&&r.opcode==Op::Update&&r.nativeKind==1&&r.movement.parts==1&&
            primary->cancelled&&!primary->unknown&&primary->kind==r.nativeKind&&
            primary->admission==r.admission&&Live(*primary,actor)&&r.parameterRevision>primary->revision&&
            r.movement.navigation==movement::Navigation::HostRoute) {
            const auto quiet=movementRouteQuiet_?movementRouteQuiet_(movementCallsContext_,r):route_execution::CommandRetired(r.scope,r.owner,r.commandId);
            if(!quiet){waitReason_=7U;return Readiness::Waiting;}
            return Resources(actor.character);
        }
        if(primary->kind!=r.nativeKind||primary->admission!=r.admission||primary->unknown||!Live(*primary,actor)||
            (primary->routeRejected&&(r.opcode==Op::Submit||r.opcode==Op::Update)))return Readiness::Rejected;
        for(const auto& lease:leases_)if(lease.used&&lease.scope==r.scope&&lease.owner==r.owner&&lease.command==r.commandId) {
            if(lease.unknown||!Live(lease,actor))return Readiness::Rejected;
            if(r.opcode==Op::Release&&!lease.cancelled) {
                std::uint32_t status{};if(!Read(lease.ref.object+0xC,status))return Readiness::Rejected;
                // Authoritative Release retires the whole command. If local
                // navigation is still executing, native Cancel removes it
                // before our Ref is dropped; do not run an obsolete goal until
                // a locally selected route happens to finish. 00683DB0 accepts
                // statuses0..6; retail producers use it for live replacement.
                if(status>6)return Readiness::Rejected;
            }
        }
        if(r.opcode==Op::Update) {
            std::uint32_t status{};
            if(!Read(primary->ref.object+0xC,status))return Readiness::Rejected;
            if(primary->cancelled||!primary->admitted||primary->revision==UINT32_MAX||
                r.parameterRevision<=primary->revision||r.movement.parts!=primary->movement.parts||
                r.movement.navigation!=primary->movement.navigation)
                return Readiness::Rejected;
            if(r.nativeKind==2)return status<=2?Resources(actor.character):Readiness::Rejected;
            if(r.nativeKind==1&&r.movement.navigation==movement::Navigation::HostGuided) {
                // An Update publishes desired values, even while retail is
                // queued, planning, ending or protected by an action. Only
                // the engine steerer may mutate/renew the native request.
                if(status>6U||!movement::Same(r.movement.companion,primary->movement.companion)||
                    r.movement.companionFlags!=primary->movement.companionFlags||
                    r.movement.companionRetainedAfter!=primary->movement.companionRetainedAfter||
                    r.movement.primaryFlags!=primary->movement.primaryFlags)return Readiness::Rejected;
                return Resources(actor.character);
            }
            // A strict route belongs to this native admission and its consumed
            // prefix. An Update cannot resurrect it with a fresh B and rewind
            // that prefix. Only a new Host command may obtain a new admission.
            if(status>=3&&status<=6)return r.movement.navigation==movement::Navigation::HostRoute?
                Readiness::Rejected:Resources(actor.character);
            if(status!=1&&status!=2)return Readiness::Rejected;
            // Companion mutations require their own native setter contract.
            if(!movement::Same(r.movement.companion,primary->movement.companion)||
                r.movement.companionFlags!=primary->movement.companionFlags||
                r.movement.companionRetainedAfter!=primary->movement.companionRetainedAfter||
                r.movement.primaryFlags!=primary->movement.primaryFlags)return Readiness::Rejected;
            if(r.nativeKind==3) {
                auto after=r.movement.after;after.retained=primary->movement.after.retained;
                if(!movement::Same(after,primary->movement.after))return Readiness::Rejected;
            }
            if(!SameGoal(r.movement.after.goal,primary->movement.after.goal)&&
                r.movement.navigation==movement::Navigation::LocalNative) {
                // A goal edit follows native request readiness. The Host path
                // for its exact parameter revision arrives in the same stream.
                std::uint32_t phase{};std::uint8_t flags{},stop{};
                if(!Read(primary->ref.object+0x68,phase)||!Read(primary->ref.object+0xB8,flags)||
                    !Read(primary->ref.object+0xB9,stop))return Readiness::Rejected;
                // 006C7550 accepts an edit only in following phase3 with no
                // unconsumed edit. Planning/pending is a native dependency,
                // never a retry-count deadline at which to cancel its worker.
                if(phase<3||(flags&0x40)){waitReason_=6U;return Readiness::Waiting;}
                // An already arrived/stopping simple Move cannot consume a
                // pending-goal edit. Reconstruct it through native Cancel,
                // RefRelease and factory/admission, without an arbitrary delay.
                const auto updatedStop=(stop&~1U)|(r.movement.after.flags91&1U);
                if((phase!=3||(updatedStop&3)==3)&&(r.nativeKind!=1||r.movement.parts!=1)){waitReason_=7U;return Readiness::Waiting;}
            }
        }
    }
    return Resources(actor.character);
}
bool NativeRequestAdapter::RestartMovement(Lease& lease,const Record& r,const LocalActorBinding& actor) noexcept {
    const bool guided=r.movement.navigation==movement::Navigation::HostGuided;
    if(guided) {
        movement::native_guidance::Snapshot native{};
        if(!steering_||!ReadGuidance(lease,native)||!movement::native_guidance::Renewable(native))return false;
    }
    lease.unknown=true;
    std::uint32_t status{};
    if(!Read(lease.ref.object+0xC,status))return false;
    // Terminal3/4/6 may still own a Ref in the native retained list. Cancel
    // removes that exact retained entry before our independent Ref is dropped.
    // Status5 reached here only with proven absence from that list.
    if((guided||status<=2)&&(!CancelRetiring(lease.ref.object)||!MovementRevalidate(r,lease)))return false;
    if(guided) {
        movement::native_guidance::Snapshot native{};
        if(!ReadGuidance(lease,native)||!native.workerQuiet||native.foreignBlock||native.retainsBlock)return false;
    }
    if(!ReleaseLeaseRef(lease))return false;
    lease.ref={};lease.admitted=lease.cancelled=false;
    lease.routeRejected=false;lease.guidanceGoalPending=false;
    lease.executedGoalKnown=lease.executedArrivalKnown=lease.executedParametersKnown=false;
    lease.revision=r.parameterRevision;lease.movement=r.movement;
    LocalActorBinding current{};Ref resource{};
    if(!Resolve(r,current)||current!=actor||!Read(actor.character+0x42A8U,resource)||
        resource.object!=lease.movementResource.object||resource.control!=lease.movementResource.control)return false;
    bool accepted{};
    const auto parameters=ExecutionParameters(lease,r.movement.after);
    if(!Gait(actor.character,lease.pacingGaitKnown?lease.pacingGait:r.movement.gait)||
        !MovementFactory(lease,1)||!MovementRevalidate(r,lease)||
        !MovementGoal(lease,parameters.goal,false)||!MovementRevalidate(r,lease)||
        !MovementParameters(lease,parameters,true)||
        !MovementAdmit(lease,r.admission,accepted)||!MovementRevalidate(r,lease)||!accepted)return false;
    lease.admitted=true;
    return MovementParameters(lease,parameters,false)&&
        MovementStop(lease,parameters.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1))&&
        MovementRevalidate(r,lease);
}
NativeOutcome NativeRequestAdapter::InvokeMovement(const Record& r,const LocalActorBinding& actor) noexcept {
    auto* primary=Find(r);
    if(r.opcode==Op::Submit) {
        if(primary)return NativeOutcome::Rejected;
        std::uint32_t marker{};
        if(r.nativeKind==2) {
            const auto resolver=gMarkerResolve.load(std::memory_order_acquire);
            if(!resolver||!resolver(r,marker)||marker==UINT32_MAX)return NativeOutcome::Rejected;
        }
        for(auto& lease:leases_)if(!lease.used){primary=&lease;break;}
        if(!primary)return NativeOutcome::Rejected;
        *primary={};primary->used=primary->unknown=true;primary->scope=r.scope;primary->owner=r.owner;
        primary->command=r.commandId;primary->actor=actor;primary->kind=r.nativeKind;primary->revision=r.parameterRevision;
        primary->admission=r.admission;
        primary->movement=r.movement;
        if(r.nativeKind==2) {
            primary->markerPayload=r.intent;primary->localMarker=marker;
            auto& retained=markerRecords_[static_cast<std::size_t>(primary-leases_.data())];
            retained.reset(new(std::nothrow) Record(r));primary->markerRecord=retained.get();
            if(!primary->markerRecord) {*primary={};return NativeOutcome::Rejected;}
        }
        if(!Read(actor.character+0x42A8,primary->movementResource)||!primary->movementResource.object||!primary->movementResource.control)
            return NativeOutcome::Unknown;
        bool accepted{};
        if(r.nativeKind==2) {
            if(!MovementFactory(*primary,2)||!MovementRevalidate(r,*primary)||!MarkerFields(*primary,false)||
                !MovementAdmit(*primary,r.admission,accepted)||!MovementRevalidate(r,*primary)||
                primary->routeRejected)return NativeOutcome::Unknown;
            // Admission may initialize now or leave status0 queued. Geometry
            // stays immutable in the lease until the native callback runs.
            if(!MarkerFields(*primary,true))return NativeOutcome::Unknown;
            primary->admitted=accepted;primary->unknown=false;
            if(accepted)FenceSupersededMovements(r,actor);
            return accepted?NativeOutcome::Admitted:NativeOutcome::Rejected;
        }
        // 0070A940 sets M+194 before constructing/admitting the Move. Native
        // synchronous callbacks must see the captured gait, not an old input.
        const auto before=ExecutionParameters(*primary,r.movement.before);
        const auto after=ExecutionParameters(*primary,r.movement.after);
        if(!Gait(actor.character,primary->pacingGaitKnown?primary->pacingGait:r.movement.gait)||
            !MovementFactory(*primary,r.nativeKind)||!MovementRevalidate(r,*primary)||
            (r.nativeKind==1&&(!MovementGoal(*primary,before.goal,false)||!MovementRevalidate(r,*primary)))||
            !MovementParameters(*primary,before,true))return NativeOutcome::Unknown;
        if(r.nativeKind==3) {
            __try {
                auto& flags=*reinterpret_cast<std::uint8_t*>(primary->ref.object+0xB8);
                flags=static_cast<std::uint8_t>((flags&~7U)|r.movement.primaryFlags);
            } __except(EXCEPTION_EXECUTE_HANDLER){return NativeOutcome::Unknown;}
        }
        if(!MovementAdmit(*primary,r.admission,accepted)||
            !MovementRevalidate(r,*primary))return NativeOutcome::Unknown;
        primary->admitted=accepted;
        if(!accepted){primary->unknown=false;return NativeOutcome::Rejected;}
        // Admission=0 has replaced previous scheduler requests NOW. Fence
        // their late route callbacks before subsequent native callbacks can
        // observe the new owner; their strong Refs are released by Host cleanup.
        FenceSupersededMovements(r,actor);
        if(!MovementParameters(*primary,after,false)||
            (r.nativeKind==1&&!MovementStop(*primary,after.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1)))||
            !MovementRevalidate(r,*primary))return NativeOutcome::Unknown;
        if(r.movement.parts==2) {
            Lease* companion{};for(auto& lease:leases_)if(!lease.used){companion=&lease;break;}
            if(!companion)return NativeOutcome::Unknown;
            *companion={};companion->used=companion->unknown=companion->secondary=true;
            companion->scope=r.scope;companion->owner=r.owner;companion->command=r.commandId;
            companion->actor=actor;companion->kind=3;companion->revision=r.parameterRevision;
            companion->admission=r.admission;
            companion->movementResource=primary->movementResource;
            if(!MovementFactory(*companion,3)||!MovementRevalidate(r,*companion)||
                !MovementParameters(*companion,r.movement.companion,true)){MovementUnknown(r);return NativeOutcome::Unknown;}
            __try {
                auto& flags=*reinterpret_cast<std::uint8_t*>(companion->ref.object+0xB8);
                flags=static_cast<std::uint8_t>((flags&~7U)|r.movement.companionFlags);
            } __except(EXCEPTION_EXECUTE_HANDLER){MovementUnknown(r);return NativeOutcome::Unknown;}
            if(!MovementAdmit(*companion,1,accepted)||!MovementRevalidate(r,*companion)||
                !MovementRevalidate(r,*primary)||!accepted){MovementUnknown(r);return NativeOutcome::Unknown;}
            companion->admitted=true;companion->unknown=false;
            __try{*reinterpret_cast<std::uint8_t*>(companion->ref.object+0x18)=static_cast<std::uint8_t>(r.movement.companionRetainedAfter);}
            __except(EXCEPTION_EXECUTE_HANDLER){MovementUnknown(r);return NativeOutcome::Unknown;}
        }
        primary->unknown=false;return NativeOutcome::Admitted;
    }
    if(!primary||primary->unknown)return NativeOutcome::Rejected;
    if(r.opcode==Op::Update) {
        if(r.nativeKind==1&&r.movement.navigation==movement::Navigation::HostGuided&&!steering_) {
            // No native mutation here, including after local completion.
            // Latest desired Host state is independent of the in-flight
            // native goal edit. Host terminal operations still retire it.
            for(auto& lease:leases_)if(lease.used&&lease.scope==r.scope&&lease.owner==r.owner&&lease.command==r.commandId)
                lease.revision=r.parameterRevision;
            primary->movement=r.movement;
            return NativeOutcome::Admitted;
        }
        if(primary->routeRejected) {
            const auto quiet=movementRouteQuiet_?movementRouteQuiet_(movementCallsContext_,r):route_execution::CommandRetired(r.scope,r.owner,r.commandId);
            if(r.nativeKind!=1||r.movement.parts!=1||!primary->cancelled||
                !quiet)return NativeOutcome::Rejected;
            // The Host fenced this command's routes: every recreated request
            // is refused at its first query, so each chase Update restarted
            // and re-cancelled the Move (stutter). Accept the Update; hybrid
            // recovery keeps following until the Host's next command.
            route::Key fenced{};fenced.scope=r.scope;fenced.owner=r.owner;fenced.commandId=r.commandId;
            route::Failure reason{};
            if(r.movement.navigation==movement::Navigation::HostRoute&&route_runtime::CommandBlocked(fenced,reason)) {
                primary->movement=r.movement;primary->revision=r.parameterRevision;
                ++gFencedRestartsSkipped;return NativeOutcome::Admitted;
            }
            if(!RestartMovement(*primary,r,actor)){MovementUnknown(r);return NativeOutcome::Unknown;}
            primary->movement=r.movement;primary->revision=r.parameterRevision;primary->unknown=false;
            ++gUpdatesAfterLocalEnd;return NativeOutcome::Admitted;
        }
        if(r.nativeKind==2) {
            // CheckMovement proved the exact same marker/HostRoute geometry.
            // Never rewrite B or reinsert a route for a retention/B4 revision.
            primary->unknown=true;
            primary->markerPayload=r.intent;*primary->markerRecord=r;
            primary->movement=r.movement;
            if(!MarkerFields(*primary,true)||!MovementRevalidate(r,*primary))return NativeOutcome::Unknown;
            primary->revision=r.parameterRevision;primary->unknown=false;
            return NativeOutcome::Admitted;
        }
        std::uint32_t status{},phase{};std::uint8_t flags{},stop{};
        const bool goalChanged=!SameGoal(r.movement.after.goal,primary->movement.after.goal);
        const bool ended=Read(primary->ref.object+0xC,status)&&status>=3&&status<=6;
        const bool following=!ended&&Read(primary->ref.object+0x68,phase)&&phase==3&&
            Read(primary->ref.object+0xB8,flags)&&!(flags&0x40)&&Read(primary->ref.object+0xB9,stop)&&
            (((stop&~1U)|(r.movement.after.flags91&1U))&3U)!=3U;
        // A retained Move can finish its native stop handshake without moving
        // request+C to a terminal scheduler status. Its setter refuses further
        // goals. Only the authority-checked steerer may renew that local
        // waypoint; wire Updates cannot bypass its quiescence/backoff policy.
        movement::native_guidance::Snapshot native{};
        const bool retainedArrival=steering_&&r.movement.navigation==movement::Navigation::HostGuided&&
            ReadGuidance(*primary,native)&&(movement::native_guidance::RetainedArrival(native)||
                movement::native_guidance::ExhaustedThroughArrival(native));
        if((ended||retainedArrival||(goalChanged&&!following&&r.movement.navigation==movement::Navigation::LocalNative))&&r.nativeKind==1&&r.movement.parts==1) {
            // The current Host Move still runs after the local request ended.
            // Recreate that same native request with the current parameters.
            if(!RestartMovement(*primary,r,actor)){MovementUnknown(r);return NativeOutcome::Unknown;}
            primary->movement=r.movement;primary->revision=r.parameterRevision;
            ++gUpdatesAfterLocalEnd;primary->unknown=false;return NativeOutcome::Admitted;
        }
        if(ended||retainedArrival) {
            // Recreate composite/control moves through the same certified
            // Cancel/Release and Submit paths. The current full bundle is the
            // new construction input; old before-update goals are not replayed.
            auto cleanup=r;cleanup.opcode=Op::Release;
            if(InvokeMovement(cleanup,actor)!=NativeOutcome::Admitted){MovementUnknown(r);return NativeOutcome::Unknown;}
            auto current=r;current.opcode=Op::Submit;current.movement.before=current.movement.after;
            const auto result=InvokeMovement(current,actor);
            if(result==NativeOutcome::Admitted)++gUpdatesAfterLocalEnd;
            return result;
        }
        primary->unknown=true;
        // Keep wire parameters exact in the lease/journal. Derived goals use
        // the current native revision and are edited later at a safe boundary.
        auto parameters=r.movement.after;
        if(r.movement.navigation==movement::Navigation::HostGuided) {
            primary->revision=r.parameterRevision;
            parameters=ExecutionParameters(*primary,r.movement.after);
        }
        if(!Gait(actor.character,primary->pacingGaitKnown?primary->pacingGait:r.movement.gait)||!MovementParameters(*primary,parameters,false)||
            (r.nativeKind==1&&!MovementStop(*primary,parameters.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1)))||
            !MovementRevalidate(r,*primary)||
            (!SameGoal(r.movement.after.goal,primary->movement.after.goal)&&
                r.movement.navigation==movement::Navigation::LocalNative&&
                (!MovementGoal(*primary,r.movement.after.goal,true)||!MovementRevalidate(r,*primary)))) {
            MovementUnknown(r);return NativeOutcome::Unknown;
        }
        // Both native requests represent one producer revision. A companion
        // queried later must not claim the previous parameter revision.
        for(auto& lease:leases_)if(lease.used&&lease.scope==r.scope&&lease.owner==r.owner&&lease.command==r.commandId)
            lease.revision=r.parameterRevision;
        primary->movement=r.movement;primary->unknown=false;
        return NativeOutcome::Admitted;
    }
    if(r.opcode!=Op::Cancel&&r.opcode!=Op::Stop&&r.opcode!=Op::Release)return NativeOutcome::Rejected;
    // Secondary first: never use the primary after releasing its last Ref.
    for(int pass=0;pass<2;++pass)for(auto& lease:leases_) {
        if(!lease.used||lease.scope!=r.scope||lease.owner!=r.owner||lease.command!=r.commandId||lease.secondary!=(pass==0))continue;
        lease.unknown=true;
        if(!MovementRevalidate(r,lease)||(!lease.cancelled&&(!CancelRetiring(lease.ref.object)||!MovementRevalidate(r,lease)))) {
            MovementUnknown(r);return NativeOutcome::Unknown;
        }
        lease.cancelled=true;
        if(r.opcode==Op::Release) {
            if(!ReleaseLeaseRef(lease)){MovementUnknown(r);return NativeOutcome::Unknown;}
            lease={};
        } else lease.unknown=false;
    }
    return NativeOutcome::Admitted;
}
std::uint64_t UpdatesAfterLocalEnd() noexcept { return gUpdatesAfterLocalEnd.load(std::memory_order_relaxed); }
std::uint64_t FencedRestartsSkipped() noexcept { return gFencedRestartsSkipped.load(std::memory_order_relaxed); }
bool NativeRequestAdapter::ReadGuidance(const Lease& lease,movement::native_guidance::Snapshot& out) noexcept {
    if(!lease.ref.object||!lease.movementResource.object)return false;
    const auto request=lease.ref.object,navigation=lease.movementResource.object+0xC4U;
    std::uintptr_t query{},control{},active{},previous{};
    if(!Read(request+0xCU,out.status)||!Read(request+0x68U,out.phase)||!Read(request+0x6CU,out.execution)||
        !Read(request+0xB8U,out.flags90)||!Read(request+0xB9U,out.flags91)||
        !Read(request+0x28U,out.goal)||!Read(request+0x34U,out.pendingGoal)||
        !Read(navigation+0x5CU,query)||!Read(navigation+0x60U,control)||
        !Read(navigation+0x40U,active)||!Read(navigation+0x44U,previous))return false;
    out.workerQuiet=!query&&!control;
    const auto block=request+0x28U;
    out.foreignBlock=(active&&active!=block)||(previous&&previous!=block);
    out.retainsBlock=active==block||previous==block;
    std::uintptr_t sentinel{},tail{};
    out.tailExhausted=Read(navigation,sentinel)&&sentinel&&Read(request+0x64U,tail)&&tail&&tail==sentinel;
    std::uint8_t update48{},update49{};
    out.routeUpdateQuiet=Read(navigation+0x48U,update48)&&Read(navigation+0x49U,update49)&&!update48&&!update49;
    if(out.status==5U)out.retainedReferenceAbsent=RetainedReferenceAbsent(lease.actor.character,request);
    return movement::FinitePoint(out.goal);
}
bool NativeRequestAdapter::GuidanceCanRestart(const Lease& current) noexcept {
    // Appending a continuation behind a future Host action would invert its
    // order. Retained terminal Refs do not block, but any live/unknown other
    // command and any foreign native queue member preserve the handoff.
    for(const auto& held:leases_)if(held.used&&held.actor==current.actor&&
        (held.scope!=current.scope||held.owner!=current.owner||held.command!=current.command)&&
        held.admitted&&!held.preempted&&!held.cancelled) {
        std::uint32_t status{};
        if(held.unknown||!Read(held.ref.object+0xCU,status)||status<3U||status>6U)return false;
    }
    std::uint32_t count{};std::uintptr_t head{},entry{};
    if(!Read(current.actor.character+kScheduler+8U,count)||count>kCapacity)return false;
    if(!count)return true;
    if(!Read(current.actor.character+kScheduler+4U,head)||!head||!Read(head,entry))return false;
    for(std::uint32_t i=0;i<count;++i) {
        if(!entry||entry==head)return false;
        std::uintptr_t request{},next{};
        if(!Read(entry+8U,request)||!Read(entry,next))return false;
        bool own{};
        for(const auto& held:leases_)if(held.used&&held.actor==current.actor&&held.scope==current.scope&&
            held.owner==current.owner&&held.command==current.command&&held.ref.object==request&&
            held.admitted&&!held.preempted&&!held.cancelled&&!held.unknown)own=true;
        if(!own)return false;
        entry=next;
    }
    return entry==head;
}
bool NativeRequestAdapter::ApplyGuidanceParameters(Lease& lease,const movement::Parameters& parameters,
    std::uint32_t gait,std::uint32_t revision) noexcept {
    // The caller has observed the actual goal and a native-safe phase. In
    // particular no stop-dirty bit can race an unconsumed pending goal.
    if(!Gait(lease.actor.character,gait)||!NavigationRevalidate(lease))return false;
    if(lease.pacingGaitKnown)++gPacingGaitSamples;
    if((!lease.executedParametersKnown||GuidanceParametersDiffer(lease.executedParameters,parameters))&&
        (!MovementParameters(lease,parameters,false)||!NavigationRevalidate(lease)))return false;
    const auto variant=static_cast<std::uint8_t>((parameters.flags91>>5)&1U);
    if(!lease.executedArrivalKnown||!SameFloat(lease.executedStopDistance,parameters.stopDistance)||
        lease.executedStopVariant!=variant) {
        if(!MovementStop(lease,parameters.stopDistance,variant)||!NavigationRevalidate(lease))return false;
        ++gGuidanceStopChanges;
    }
    lease.guidanceAppliedRevision=revision;
    return true;
}
void NativeRequestAdapter::SteerMovements(GuidanceAuthority authority,void* authorityContext) noexcept {
    using namespace movement::native_guidance;
    if(busy_||retiring_||!base_||!authority)return;
    for(auto& lease:leases_) {
        if(!lease.used||lease.secondary||lease.kind!=1||lease.movement.navigation!=movement::Navigation::HostGuided||
            !lease.admitted||lease.cancelled||lease.preempted||lease.unknown||lease.routeRejected)continue;
        const RetirementIdentity identity{lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision};
        if(!authority(authorityContext,identity)||MovementExecutionOwned(lease.actor.character)||!NavigationRevalidate(lease)) {++gGuidanceSuspended;continue;}
        std::uintptr_t head{},first{},request{};std::uint32_t count{};
        // A queued Move cannot author gait/stop over another native head.
        // Its OWN compound Stop may be head after a local end; renewal of
        // that same bundle uses certified secondary-first cleanup below.
        if(!Read(lease.actor.character+kScheduler+8U,count)||count>kCapacity||
            (count&&(!Read(lease.actor.character+kScheduler+4U,head)||!head||!Read(head,first)||
                !first||first==head||!Read(first+8U,request)))) {
            ++gGuidanceSuspended;++gGuidanceQueueWaits;continue;
        }
        const bool primaryHead=!count||request==lease.ref.object;
        bool ownHead=primaryHead;
        if(!ownHead)for(const auto& held:leases_)if(held.used&&held.secondary&&held.actor==lease.actor&&
            held.scope==lease.scope&&held.owner==lease.owner&&held.command==lease.command&&held.ref.object==request&&
            held.admitted&&!held.preempted&&!held.cancelled&&!held.unknown)ownHead=true;
        if(!ownHead){++gGuidanceSuspended;++gGuidanceQueueWaits;continue;}
        std::array<float,3> local{};
        if(!Read(lease.actor.character+0x70U,local)||!movement::FinitePoint(local)){++gGuidanceSuspended;continue;}
        if(!lease.guidanceProgressKnown) {lease.guidanceProgress=local;lease.guidanceProgressKnown=true;}
        const auto dx=local[0]-lease.guidanceProgress[0],dz=local[2]-lease.guidanceProgress[2];
        if(dx*dx+dz*dz>=0.0625F) {
            if(dx*dx+dz*dz<=16.0F&&std::abs(local[1]-lease.guidanceProgress[1])<=1.5F)lease.guidanceContinuations=0;
            lease.guidanceProgress=local;
        }
        const auto desired=ExecutionParameters(lease,lease.movement.after);
        if(lease.pacingSuspended){++gGuidanceSuspended;continue;}
        Snapshot native{};
        if(!ReadGuidance(lease,native)){++gGuidanceFaults;continue;}
        const auto now=movementClock_?movementClock_(movementCallsContext_):GetTickCount64();
        if(AwaitingScheduler(native)){++gGuidanceDeferred;continue;}
        if(SchedulerEnded(native)||RetainedArrival(native)||ExhaustedThroughArrival(native)) {
            if(native.status==5U&&!native.retainedReferenceAbsent){++gGuidanceRetainedWaits;continue;}
            if(Renewable(native))lease.guidanceGoalPending=false;
            if(!Renewable(native)||lease.pacing.holding||
                !MeaningfulTarget(local,desired.goal,desired.stopDistance,desired.stoppingDistance)){++gGuidanceDeferred;continue;}
            if(!GuidanceCanRestart(lease)){++gGuidanceQueueWaits;continue;}
            if(!RetryReady(now,lease.lastGuidanceContinuation,lease.guidanceContinuations)) {++gGuidanceContinuationLimits;continue;}
            const character_direct_step_runtime::MovementResourceLease resources(lease.actor.character);
            if(!resources||!NavigationRevalidate(lease)||!authority(authorityContext,identity)){++gGuidanceSuspended;continue;}
            Record frame{};frame.scope=lease.scope;frame.owner=lease.owner;frame.commandId=lease.command;
            frame.nativeKind=1;frame.parameterRevision=lease.revision;frame.admission=lease.admission;
            frame.movement=lease.movement;frame.opcode=Op::Update;
            const auto actor=lease.actor;
            const auto attempts=lease.guidanceContinuations==UINT32_MAX?UINT32_MAX:lease.guidanceContinuations+1U;
            busy_=steering_=true;invokingRecord_=&frame;
            const auto outcome=InvokeMovement(frame,actor);
            invokingRecord_=nullptr;steering_=busy_=false;
            if(auto* current=Find(frame)) {
                current->guidanceContinuations=attempts;current->lastGuidanceContinuation=current->lastGuidanceEdit=now;
                current->guidanceProgress=local;current->guidanceProgressKnown=true;current->guidanceGoalPending=false;
                if(outcome!=NativeOutcome::Admitted)current->unknown=true;
            }
            if(outcome==NativeOutcome::Admitted) {
                ++gGuidanceContinuations;
                if(ExhaustedThroughArrival(native))++gGuidanceExhaustedTailContinuations;
            } else ++gGuidanceFaults;
            continue;
        }
        if(!primaryHead){++gGuidanceSuspended;continue;}
        auto parameters=desired;
        auto gait=lease.pacingGaitKnown?lease.pacingGait:lease.movement.gait;
        auto revision=lease.revision;
        bool consumed{};
        if(lease.guidanceGoalPending) {
            const auto observation=ObserveGoal(native,lease.pendingGuidanceParameters.goal);
            if(observation==GoalResult::Waiting||observation==GoalResult::Pending) {++gGuidanceDeferred;continue;}
            lease.guidanceGoalPending=false;
            if(observation==GoalResult::Discarded) {
                // A void setter and a cleared pending bit are not evidence of
                // a route change. Preserve the desired goal for a later edit.
                ++gGuidanceGoalDiscards;lease.lastGuidanceEdit=now;continue;
            }
            consumed=true;++gGuidanceGoalConsumptions;
            // Finish the deposited frame before starting another goal. When
            // the latest target is identical/coalescible, use its newer intent.
            if(!CoalescibleGoal(native.goal,desired.goal)) {
                parameters=lease.pendingGuidanceParameters;
                gait=lease.pendingGuidanceGait;revision=lease.pendingGuidanceRevision;
            }
        }
        if(!Editable(native)){++gGuidanceDeferred;continue;}
        lease.executedGoal=native.goal;lease.executedGoalKnown=true;
        if(!consumed&&!CanConsumeGoal(native.goal,parameters.goal)&&!CoalescibleGoal(native.goal,parameters.goal)) {
            // A vertical-only edit cannot be consumed by 0044D320. Keep the
            // current native route intact; after its real end a factory can
            // accept the full three-dimensional Host target.
            ++gGuidanceDeferred;continue;
        }
        const bool changeGoal=!consumed&&CanConsumeGoal(native.goal,parameters.goal);
        const bool changeParameters=!lease.executedParametersKnown||GuidanceParametersDiffer(lease.executedParameters,parameters)||
            !lease.executedArrivalKnown||!SameFloat(lease.executedStopDistance,parameters.stopDistance)||
            lease.executedStopVariant!=((parameters.flags91>>5)&1U);
        std::uint32_t currentGait{};
        if(!Read(lease.movementResource.object+0x194U,currentGait)){++gGuidanceFaults;continue;}
        if(!consumed&&!changeGoal&&!changeParameters&&currentGait==gait)continue;
        if(!consumed&&now-lease.lastGuidanceEdit<150U){++gGuidanceDeferred;continue;}
        const character_direct_step_runtime::MovementResourceLease resources(lease.actor.character);
        if(!resources||!NavigationRevalidate(lease)||!authority(authorityContext,identity)){++gGuidanceSuspended;continue;}
        Record frame{};frame.scope=lease.scope;frame.owner=lease.owner;frame.commandId=lease.command;
        frame.nativeKind=1;frame.parameterRevision=lease.revision;frame.admission=lease.admission;
        frame.movement=lease.movement;frame.opcode=Op::Update;
        busy_=steering_=true;invokingRecord_=&frame;
        bool applied{};
        if(changeGoal) {
            applied=MovementGoal(lease,parameters.goal,true)&&NavigationRevalidate(lease);
            if(applied) {
                Snapshot deposited{};applied=ReadGuidance(lease,deposited);
                if(applied&&(deposited.flags90&0x40U)&&movement::native_guidance::SameGoal(deposited.pendingGoal,parameters.goal)) {
                    lease.pendingGuidanceParameters=parameters;lease.pendingGuidanceGait=gait;
                    lease.pendingGuidanceRevision=revision;lease.guidanceGoalPending=true;++gGuidanceGoalDeposits;
                } else if(applied)++gGuidanceDeferred; // Retail refused; retain intent, never claim it executed.
            }
        } else applied=ApplyGuidanceParameters(lease,parameters,gait,revision);
        invokingRecord_=nullptr;steering_=busy_=false;
        if(applied) {lease.lastGuidanceEdit=now;if(!changeGoal)++gHostPathSteers;}
        else {lease.unknown=true;++gGuidanceFaults;}
    }
}
GuidanceReport GuidanceDiagnostics() noexcept {
    return {gHostPathSteers.load(),gGuidanceDeferred.load(),gGuidanceSuspended.load(),gGuidanceFaults.load(),gGuidanceWithoutRoute.load(),
        gGuidanceContinuations.load(),gGuidanceContinuationLimits.load(),gPacedSamples.load(),gPacingHoldSamples.load(),
        gPacingUnavailable.load(),gPacingGaitSamples.load(),gPhysicalTrailSamples.load(),gPacingLead.load(),
        gGuidanceGoalDeposits.load(),gGuidanceGoalConsumptions.load(),gGuidanceGoalDiscards.load(),
        gGuidanceStopChanges.load(),gGuidanceStartWaits.load(),gGuidanceRetainedWaits.load(),gGuidanceQueueWaits.load(),
        gGuidanceExhaustedTailContinuations.load()};
}
std::uint64_t HostPathSteers() noexcept { return gHostPathSteers.load(std::memory_order_relaxed); }
} // namespace aimp::ai_sync::execution
