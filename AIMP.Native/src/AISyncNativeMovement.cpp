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
    for(std::size_t i=0;i<3;++i)if(!SameFloat(a[i],b[i]))return false;return true;
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
        if (lease.routeRejected || lease.cancelled || (lease.unknown&&!admitting)) return NavigationOwnership::Rejected;
        if(busy_&&!admitting)return NavigationOwnership::Waiting;
        LocalActorBinding actor{};
        const auto resolver=invoking?resolve_:(lifecycleResolver_?lifecycleResolver_:resolve_);
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
        (lease.kind==1||lease.kind==2||lease.kind==3)&&lease.movement.navigation==movement::Navigation::HostRoute&&
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
        if(movementAdmission_)return movementAdmission_(movementCallsContext_,lease.ref.object,admission,accepted);
        const auto submit=admission==0?0x283CE0U:(admission==1?0x283C10U:0x283550U);
        accepted=reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(base_+submit)(
            reinterpret_cast<void*>(lease.ref.object))!=0;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementGoal(Lease& lease,const std::array<float,3>& goal,bool update) noexcept {
    __try {
        if(movementGoal_)return movementGoal_(movementCallsContext_,lease.ref.object,goal.data(),update);
        reinterpret_cast<void(__thiscall*)(void*,const float*)>(base_+(update?0x2C7550U:0x2777B0U))(
            reinterpret_cast<void*>(lease.ref.object),goal.data());return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool NativeRequestAdapter::MovementStop(Lease& lease,float distance,std::uint8_t variant) noexcept {
    __try {
        if(movementStop_)return movementStop_(movementCallsContext_,lease.ref.object,distance,variant);
        reinterpret_cast<void(__thiscall*)(void*,float,std::uint8_t)>(base_+0x27D0D0U)(
            reinterpret_cast<void*>(lease.ref.object),distance,variant);return true;
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
        f91=static_cast<std::uint8_t>((f91&~mask)|(p.flags91&mask));return true;
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

    } else {
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
                r.movement.navigation!=movement::Navigation::HostRoute) {
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
    lease.unknown=true;
    std::uint32_t status{};
    if(Read(lease.ref.object+0xC,status)&&status<=2&&(!CancelRetiring(lease.ref.object)||!MovementRevalidate(r,lease)))return false;
    if(!ReleaseLeaseRef(lease))return false;
    lease.ref={};lease.admitted=lease.cancelled=false;
    lease.routeRejected=false;
    lease.revision=r.parameterRevision;lease.movement=r.movement;
    LocalActorBinding current{};Ref resource{};
    if(!Resolve(r,current)||current!=actor||!Read(actor.character+0x42A8U,resource)||
        resource.object!=lease.movementResource.object||resource.control!=lease.movementResource.control)return false;
    bool accepted{};
    if(!Gait(actor.character,r.movement.gait)||
        !MovementFactory(lease,1)||!MovementRevalidate(r,lease)||
        !MovementGoal(lease,r.movement.after.goal,false)||!MovementRevalidate(r,lease)||
        !MovementParameters(lease,r.movement.after,true)||
        !MovementAdmit(lease,r.admission,accepted)||!MovementRevalidate(r,lease)||!accepted)return false;
    lease.admitted=true;
    return MovementParameters(lease,r.movement.after,false)&&
        MovementStop(lease,r.movement.after.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1))&&
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
            return accepted?NativeOutcome::Admitted:NativeOutcome::Rejected;
        }
        // 0070A940 sets M+194 before constructing/admitting the Move. Native
        // synchronous callbacks must see the captured gait, not an old input.
        if(!Gait(actor.character,r.movement.gait)||
            !MovementFactory(*primary,r.nativeKind)||!MovementRevalidate(r,*primary)||
            (r.nativeKind==1&&(!MovementGoal(*primary,r.movement.before.goal,false)||!MovementRevalidate(r,*primary)))||
            !MovementParameters(*primary,r.movement.before,true))return NativeOutcome::Unknown;
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
        if(!MovementParameters(*primary,r.movement.after,false)||
            (r.nativeKind==1&&!MovementStop(*primary,r.movement.after.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1)))||
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
        if((ended||(goalChanged&&!following&&r.movement.navigation!=movement::Navigation::HostRoute))&&r.nativeKind==1&&r.movement.parts==1) {
            // The current Host Move still runs after the local request ended.
            // Recreate that same native request with the current parameters.
            if(!RestartMovement(*primary,r,actor)){MovementUnknown(r);return NativeOutcome::Unknown;}
            primary->movement=r.movement;primary->revision=r.parameterRevision;
            ++gUpdatesAfterLocalEnd;primary->unknown=false;return NativeOutcome::Admitted;
        }
        if(ended) {
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
        if(!Gait(actor.character,r.movement.gait)||!MovementParameters(*primary,r.movement.after,false)||
            (r.nativeKind==1&&!MovementStop(*primary,r.movement.after.stopDistance,static_cast<std::uint8_t>((r.movement.after.flags91>>5)&1)))||
            !MovementRevalidate(r,*primary)||
            (!SameGoal(r.movement.after.goal,primary->movement.after.goal)&&
                r.movement.navigation!=movement::Navigation::HostRoute&&
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
void NativeRequestAdapter::SteerMovements() noexcept {
    // HostRoute is executed from imported native nodes. No waypoint targets,
    // speed correction, pose chasing or intermediate stop-distance overrides.
}
std::uint64_t HostPathSteers() noexcept { return gHostPathSteers.load(std::memory_order_relaxed); }
} // namespace aimp::ai_sync::execution
