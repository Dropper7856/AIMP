// Included once inside AISync.cpp's private namespace, after the lease lifetime
// helpers. Exact-build x86 adapters; never called by a transport worker.
// Native contracts and static evidence: docs/AVS-NATIVE-ACTIONS-v416.md.
namespace pvp_actions {
#ifdef AIMP_ALIEN_ACTION_TEST
#define AIMP_ACTION_TRY try
#define AIMP_ACTION_CATCH catch(...)
#else
#define AIMP_ACTION_TRY __try
#define AIMP_ACTION_CATCH __except(EXCEPTION_EXECUTE_HANDLER)
#endif
template<class Fn> Fn Native(const std::uintptr_t rva) noexcept {
#ifdef AIMP_ALIEN_ACTION_TEST
    return reinterpret_cast<Fn>(ActionTestFunction(rva));
#else
    return reinterpret_cast<Fn>(gBase+rva);
#endif
}
namespace ap=aimp::alien_pvp;
namespace policy=aimp::alien_pvp::actions;
using Phase=ap::ActionPhase;
using Reason=ap::ActionReason;
using Action=ap::Action;
struct VentSlot final {
    std::array<std::uint32_t,4> node{};
    ap::vent_node::Methods entryMethods{};
    std::uintptr_t frame{},manager{};
    ap::Scope scope{};std::uintptr_t character{};
    bool used{},started{},quarantined{},cancelling{};
    std::uint32_t selectedIndex{};
};
// Node storage must outlive the native frame. On unprovable destruction it is
// quarantined, never overwritten while an old frame could still refer to it.
std::array<VentSlot,8> ventSlots{};
struct Active final {
    ap::Context context{};ap::Frame receipt{};
    std::uint64_t began{},lastStep{},submittedAt{};
    PairIdentity victim{};
    std::uintptr_t chooser{};
    VentSlot* vent{};
    bool used{},entered{},cancelled{},terminalSent{},hostEnded{},moveArrived{},entrySubmitted{},remoteVent{};
    bool presentationStarted{},ownerFinished{};
    std::uint64_t ventProgressAt{};float ventBestDistance{std::numeric_limits<float>::infinity()};
};
Active active{};
policy::EdgeGate edges{};
std::uint64_t edgeEpoch{};

bool Live(const ap::Context& c) noexcept {
    return EngineThread() && PvPLeaseLiveIdentity() &&
        c.base==gBase && c.character==gPvPLease.ticket.character &&
        c.scope.nativeGeneration==gPvPLease.nativeGeneration && c.scope.worldGeneration==gPvPLease.worldGeneration &&
        c.scope.incarnation==gPvPLease.ticket.serial && c.scope.ownerPeer==gPvPLease.ownerPeer &&
        c.scope.cookie==gPvPLease.sessionCookie && c.scope.entityA==gPvPLease.entityA && c.scope.entityB==gPvPLease.entityB;
}
#include "AISyncPvPDonorMovement.inl"
bool Body(const std::uintptr_t character,policy::Point& point) noexcept {
    return Read(character+0x70U,point.x) && Read(character+0x74U,point.y) &&
        Read(character+0x78U,point.z) && policy::Finite(point);
}
bool Cycle(const std::uintptr_t character,std::uint8_t& state,bool& backstage) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t hidden{},withdrawn{};
    if(!Read(character+0x36CU,cycle) || !cycle || !Read(cycle+4U,owner) || owner!=character ||
        !Read(cycle+0x1AU,state) || !Read(character+0x2DAU,hidden) || !Read(character+0x2DBU,withdrawn))return false;
    backstage=hidden!=0U || withdrawn!=0U;return true;
}
#ifndef AIMP_ALIEN_ACTION_TEST
struct VentTelemetry final {
    std::atomic<std::uint64_t> revision{},sequence{},elapsed{};
    std::atomic<unsigned> action{},phase{},cycle{},frameStatus{};
    std::atomic<bool> host{},backstage{},cancelling{};
    std::atomic<float> x{},y{},z{};
} ventTelemetry;
void TraceVent(unsigned frameStatus=0U) noexcept {
    if(!active.used || (active.receipt.action!=Action::EnterVent && active.receipt.action!=Action::ExitVent))return;
    policy::Point body{};std::uint8_t state{};bool backstage{};
    if(!Body(active.context.character,body)||!Cycle(active.context.character,state,backstage))return;
    ventTelemetry.sequence=active.receipt.hostSequence;ventTelemetry.elapsed=GetTickCount64()-active.began;
    ventTelemetry.action=static_cast<unsigned>(active.receipt.action);ventTelemetry.phase=static_cast<unsigned>(active.receipt.phase);
    ventTelemetry.cycle=state;ventTelemetry.frameStatus=frameStatus;ventTelemetry.host=active.context.host;
    ventTelemetry.backstage=backstage;ventTelemetry.cancelling=active.cancelled;
    ventTelemetry.x=body.x;ventTelemetry.y=body.y;ventTelemetry.z=body.z;
    ventTelemetry.revision.fetch_add(1U,std::memory_order_release);
}
void LogVent(Diagnostics& diagnostics) {
    static std::uint64_t lastRevision{},lastAt{};
    const auto revision=ventTelemetry.revision.load(std::memory_order_acquire),now=GetTickCount64();
    if(!revision || revision==lastRevision || now-lastAt<1000U)return;
    lastRevision=revision;lastAt=now;
    diagnostics.Log("info","alien_vent_progress","Engine-thread vent observations; frameResult 0 pending, 1 arrived, 2 refused.",
        {{"host",Diagnostics::Boolean(ventTelemetry.host.load()),true},
         {"hostSequence",Diagnostics::Unsigned(ventTelemetry.sequence.load()),true},
         {"elapsedMs",Diagnostics::Unsigned(ventTelemetry.elapsed.load()),true},
         {"action",Diagnostics::Unsigned(ventTelemetry.action.load()),true},
         {"phase",Diagnostics::Unsigned(ventTelemetry.phase.load()),true},
         {"cycle",Diagnostics::Unsigned(ventTelemetry.cycle.load()),true},
         {"frameResult",Diagnostics::Unsigned(ventTelemetry.frameStatus.load()),true},
         {"backstage",Diagnostics::Boolean(ventTelemetry.backstage.load()),true},
         {"cancelling",Diagnostics::Boolean(ventTelemetry.cancelling.load()),true},
         {"x",std::to_string(ventTelemetry.x.load()),true},
         {"y",std::to_string(ventTelemetry.y.load()),true},
         {"z",std::to_string(ventTelemetry.z.load()),true}});
}
#else
void TraceVent(unsigned=0U) noexcept {}
#endif
bool Ordinary(const std::uint8_t state) noexcept {return state==1U || state==55U || state==56U || state==59U;}
bool Protected(const std::uint8_t state) noexcept {return state==18U || state==75U || state==77U || state==78U || state==79U;}
bool Same(const ap::Context& c) noexcept {return active.used && c.character==active.context.character && c.scope==active.context.scope;}
bool Quarantined(const ap::Context& c) noexcept {
    for(const auto& slot:ventSlots)if(slot.quarantined && slot.character==c.character && slot.scope==c.scope)return true;
    return false;
}
// A remote-owner vent receipt owns journal ordering, but no local native
// movement resource. Its replica must remain available to measured poses.
bool Busy(const ap::Context& c) noexcept {return (Same(c) && !active.remoteVent) || Quarantined(c);}
void Reject(ap::Frame& r,Reason reason) noexcept {
    r.phase=Phase::Rejected;r.reason=reason;r.resolutionRevision=1U;
}
// Scalar wrappers isolate SEH from functions with C++ destructors.
bool ExitPoint(std::uintptr_t descriptor,policy::Point& out) noexcept {
    AIMP_ACTION_TRY {
        using Fn=void*(__thiscall*)(void*,void*);
        Native<Fn>(0x0021CCC0U)(reinterpret_cast<void*>(descriptor),&out);
        return policy::Finite(out);
    } AIMP_ACTION_CATCH {return false;}
}
bool PortalIndex(const ap::Context& c,const ap::Frame& plan,std::uint32_t& index) noexcept {
    std::uintptr_t withdraw{},manager{},entries{};std::uint32_t count{};bool found{};
    if(!Live(c) || !Read(c.character+0x34F0U,withdraw) || !withdraw ||
        !Read(withdraw+0x24U,manager) || !manager || !Read(manager+8U,entries) || !entries ||
        !Read(manager+12U,count) || count>4096U)return false;
    for(std::uint32_t i=0;i<count;++i) {
        std::uintptr_t descriptor{};policy::Point entrance{},exit{};
        if(!Read(entries+i*0x30U+0x28U,descriptor) || !descriptor || !Read(descriptor,entrance) || !ExitPoint(descriptor,exit))continue;
        constexpr float tolerance=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
        if(policy::DistanceSquared(entrance,{plan.x,plan.y,plan.z})>tolerance ||
            policy::DistanceSquared(exit,{plan.resolvedX,plan.resolvedY,plan.resolvedZ})>tolerance)continue;
        if(found)return false;
        index=i;found=true;
    }
    return found;
}
// Donor vent.cpp::OnVentPickLeave/OnVentPickExitLeave, scoped to this
// AIMP-owned node call. Other Aliens and ordinary Coop keep retail selection.
thread_local VentSlot* selectedPortal{};
void* ventEnterOriginal{};void* ventExitOriginal{};
std::uintptr_t PickPortal(std::uintptr_t manager,int reason,bool exit) noexcept {
    auto* slot=selectedPortal;
    if(!slot || slot->manager!=manager || !Live(active.context) || reason<0 || reason>1)return 0U;
    std::uintptr_t entries{},descriptor{};std::uint32_t count{};
    if(!Read(manager+8U,entries) || !entries || !Read(manager+12U,count) || slot->selectedIndex>=count ||
        !Read(entries+slot->selectedIndex*0x30U+0x28U,descriptor) || !descriptor)return 0U;
    const auto offset=(exit?0x48U:0x40U)+static_cast<unsigned>(reason)*4U;
    AIMP_ACTION_TRY {
        *reinterpret_cast<std::uint32_t*>(manager+offset)=slot->selectedIndex;
        return entries+slot->selectedIndex*0x30U;
    } AIMP_ACTION_CATCH {return 0U;}
}
std::uintptr_t __fastcall PickEnterDetour(void* manager,void*,int reason) noexcept {
    const auto selected=PickPortal(reinterpret_cast<std::uintptr_t>(manager),reason,false);
    if(selected)return selected;
    return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventEnterOriginal)(manager,reason);
}
std::uintptr_t __fastcall PickExitDetour(void* manager,void*,int reason) noexcept {
    const auto selected=PickPortal(reinterpret_cast<std::uintptr_t>(manager),reason,true);
    if(selected)return selected;
    return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventExitOriginal)(manager,reason);
}
std::uintptr_t RetailEntry(std::uintptr_t manager) noexcept {
    if(!ventEnterOriginal)return 0;
    AIMP_ACTION_TRY {
        return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,int)>(ventEnterOriginal)(reinterpret_cast<void*>(manager),0);
    } AIMP_ACTION_CATCH {return 0;}
}
bool SelectPortal(const ap::Context& c,const ap::Frame& request,ap::Frame& result) noexcept {
    std::uintptr_t withdraw{},manager{},entries{};std::uint32_t count{};
    if(!Read(c.character+0x34F0U,withdraw) || !withdraw || !Read(withdraw+0x24U,manager) || !manager ||
        !Read(manager+8U,entries) || !Read(manager+12U,count) || !entries || count==0U || count>4096U)return false;
    policy::Point body{};if(!Body(c.character,body))return false;
    policy::Vent best{};bool found{};
    float bestDistance=std::numeric_limits<float>::infinity();
    const bool enter=request.action==Action::EnterVent;
    for(std::uint32_t i=0U;i<count;++i) {
        std::uintptr_t descriptor{};policy::Vent vent{};vent.localIndex=i;
        if(!Read(entries+static_cast<std::uintptr_t>(i)*0x30U+0x28U,descriptor) || !descriptor ||
            !Read(descriptor,vent.entrance) || !policy::Finite(vent.entrance) || !ExitPoint(descriptor,vent.exit))continue;
        if(c.host) {
            if((request.flags&ap::VentSelected)!=0U) {
                constexpr float tolerance=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
                if(policy::DistanceSquared(vent.entrance,{request.x,request.y,request.z})>tolerance)continue;
                if(enter && !ap::vents::WithinReach({body.x,body.y,body.z},
                    {vent.entrance.x,vent.entrance.y,vent.entrance.z}))continue;
                if(found){result.reason=Reason::AmbiguousVent;return false;}
                best=vent;found=true;continue;
            }
            const auto p=enter?vent.entrance:vent.exit;const auto d=policy::DistanceSquared(body,p);
            if(enter) {
                if(!ap::vents::WithinReach({body.x,body.y,body.z},{p.x,p.y,p.z}))continue;
                if(!found || d<bestDistance){best=vent;found=true;bestDistance=d;}
            } else {
                // Donor vent.cpp::NearestUntried: nearest real opening to
                // the Alien, with a height penalty. Backstage is often above
                // the ceiling; a same-floor constraint prevents all exits.
                const auto score=std::hypot(body.x-vent.entrance.x,body.z-vent.entrance.z)+
                    std::abs(body.y-vent.entrance.y)*0.7F;
                if(std::isfinite(score) && (!found || score<bestDistance)) {best=vent;found=true;bestDistance=score;}
            }
        } else {
            const policy::Point entrance{request.x,request.y,request.z};
            const policy::Point exit{request.resolvedX,request.resolvedY,request.resolvedZ};
            constexpr float e=policy::kVentMatchTolerance*policy::kVentMatchTolerance;
            if(policy::DistanceSquared(entrance,vent.entrance)>e || policy::DistanceSquared(exit,vent.exit)>e)continue;
            if(found){result.reason=Reason::AmbiguousVent;return false;}
            best=vent;found=true;
        }
    }
    // Donor vent.cpp::PickGo keeps the retail path-aware choice when it is
    // almost as near as the closest opening. Forcing the nearest point can
    // choose the wrong side of a wall or an inaccessible approach.
    if(c.host && enter && found && (request.flags&ap::VentSelected)==0U) {
        const auto original=RetailEntry(manager);
        if(original>=entries && (original-entries)%0x30U==0U && (original-entries)/0x30U<count) {
            policy::Vent vent{};std::uintptr_t descriptor{};
            vent.localIndex=static_cast<std::uint32_t>((original-entries)/0x30U);
            if(Read(original+0x28U,descriptor) && descriptor && Read(descriptor,vent.entrance) &&
                policy::Finite(vent.entrance) && ExitPoint(descriptor,vent.exit) &&
                ap::vents::WithinReach({body.x,body.y,body.z},{vent.entrance.x,vent.entrance.y,vent.entrance.z}) &&
                std::sqrt(policy::DistanceSquared(body,vent.entrance))<=std::sqrt(bestDistance)+3.0F)best=vent;
        }
    }
    std::uintptr_t current{},table{};std::uint32_t size{};
    if(!found || !Read(withdraw+0x24U,current) || current!=manager || !Read(manager+8U,table) || table!=entries ||
        !Read(manager+12U,size) || size!=count)return false;
    result.x=best.entrance.x;result.y=best.entrance.y;result.z=best.entrance.z;
    result.resolvedX=best.exit.x;result.resolvedY=best.exit.y;result.resolvedZ=best.exit.z;
    return true;
}
VentSlot* FreeVentSlot() noexcept {
    for(auto& slot:ventSlots)if(!slot.used && !slot.quarantined)return &slot;
    return nullptr;
}
bool VentFrameLive(const ap::Context& c,const VentSlot& slot) noexcept {
    std::uintptr_t node{},tree{},vtable{},withdraw{},manager{};
    return Live(c) && slot.used && !slot.quarantined && slot.frame &&
        Read(slot.frame,vtable) && vtable==gBase+0x00DBCD38U &&
        Read(slot.frame+0x10U,node) && node==reinterpret_cast<std::uintptr_t>(slot.node.data()) &&
        Read(slot.frame+0x14U,tree) && tree==gPvPLease.tree &&
        Read(c.character+0x34F0U,withdraw) && withdraw && Read(withdraw+0x24U,manager) && manager==slot.manager;
}
bool StartVent(const ap::Context& c,ap::Frame& result,VentSlot& slot) noexcept {
    std::uintptr_t withdraw{};std::uint32_t allocated{};
    if(!Live(c) || !Read(c.character+0x34F0U,withdraw) || !withdraw ||
        !Read(withdraw+0x24U,slot.manager) || !slot.manager ||
        !Read(gBase+0x012B1C20U,allocated) || allocated>=0x6F6U || !PortalIndex(c,result,slot.selectedIndex))return false;
    slot.used=true;slot.scope=c.scope;slot.character=c.character;
    AIMP_ACTION_TRY {
        using Constructor=void*(__thiscall*)(void*);
        using Create=std::uintptr_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t);
        using Dispatch=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);
        const auto ctor=result.action==Action::EnterVent?0x0000F680U:0x0000FC50U;
        Native<Constructor>(ctor)(slot.node.data());
        if(result.action==Action::EnterVent) {
            if(!ap::vent_node::PilotEntryMethods(gBase,nullptr,
                [](void*,std::uintptr_t at,void* out,std::size_t n) noexcept {
                    return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),n);
                },slot.entryMethods)){slot={};return false;}
            slot.node[0]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(slot.entryMethods.data()));
        }
        slot.frame=Native<Create>(0x002F15C0U)(slot.node.data(),1U,gPvPLease.tree);
        if(!slot.frame){slot.used=false;return false;}
        // Dispatch invokes the real Start method: frame+3C flags and the
        // vent-manager lock are established by CATHODE, not our own writes.
        Native<Dispatch>(0x002D0260U)(slot.node.data(),c.character,slot.frame,0U,0U,0U);
        slot.started=true;
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool StepVent(const ap::Context& c,VentSlot& slot,const ap::Frame& plan,int& result) noexcept {
    if(!VentFrameLive(c,slot))return false;
    const auto previous=selectedPortal;selectedPortal=&slot;
    AIMP_ACTION_TRY {
        // Run the donor's actual GoBackstage/ComeFrontstage node Update,
        // including its portal getter, cancel polling and native frame status.
        using Update=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        Native<Update>(plan.action==Action::EnterVent?0x0000F5C0U:0x0000FB10U)(slot.node.data(),c.character,slot.frame);
        const auto status=*reinterpret_cast<const std::uint32_t*>(slot.frame+4U);
        result=status==9U?1:(status==8U?2:0);
        selectedPortal=previous;return true;
    } AIMP_ACTION_CATCH {selectedPortal=previous;slot.quarantined=true;return false;}
}
bool FinishVent(const ap::Context& c,VentSlot& slot,bool cancel,bool& done) noexcept {
    done=false;
    if(!VentFrameLive(c,slot))return false;
    AIMP_ACTION_TRY {
        using Two=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        using RequestCancel=void(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using PollCancel=std::uint32_t(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using Destroy=void*(__thiscall*)(void*,std::uint32_t);
        if(cancel && !slot.cancelling) {
            Native<RequestCancel>(0x0030A7C0U)(c.character,slot.frame);
            slot.cancelling=true;
        }
        // Retail node Update uses 0070A5E0 while frame status is 7. A
        // cancellation request is NOT proof that scheduler/worker Refs retired.
        // Keep both the pooled frame AND stable node storage until this passes.
        if(slot.cancelling && (Native<PollCancel>(0x0030A5E0U)(
                c.character,slot.frame)&0xFFU)==0U)return true;
        if(slot.started)Native<Two>(0x0000FBC0U)(slot.node.data(),c.character,slot.frame);
        Native<Destroy>(0x002FA8D0U)(reinterpret_cast<void*>(slot.frame),1U);
        slot={};done=true;return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
// Donor Path -> ActionMoveToMostRecentSensedPosition (006ED2B0).
// Only its immutable node layout is reproduced. The destination is the last
// authenticated Host pose, never a client-selected target or an AI blackboard.
struct RecoverySlot final {
    std::array<std::uint32_t,3> node{};
    ap::Context context{};std::uintptr_t frame{};
    policy::Point goal{};std::uint64_t selectedAt{},steppedAt{},began{};
    bool used{},cancelling{},quarantined{};
};
std::array<RecoverySlot,8> recoveries{};
std::uint64_t recoveryRetryAt{};
RecoverySlot* RecoveryFor(const ap::Context& c) noexcept {
    for(auto& slot:recoveries)if(slot.used && slot.context.character==c.character && slot.context.scope==c.scope)return &slot;
    return nullptr;
}
bool RecoveryLive(const ap::Context& c,const RecoverySlot& slot) noexcept {
    std::uintptr_t vtable{},node{},tree{};
    return Live(c) && slot.used && !slot.quarantined && slot.frame &&
        Read(slot.frame,vtable) && vtable==gBase+0x00DBCD38U &&
        Read(slot.frame+0x10U,node) && node==reinterpret_cast<std::uintptr_t>(slot.node.data()) &&
        Read(slot.frame+0x14U,tree) && tree==gPvPLease.tree;
}
bool FinishRecovery(const ap::Context& c,RecoverySlot& slot,bool cancel) noexcept {
    if(!RecoveryLive(c,slot)){slot.quarantined=true;return false;}
    AIMP_ACTION_TRY {
        using Two=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t);
        using PollCancel=std::uint32_t(__stdcall*)(std::uintptr_t,std::uintptr_t);
        using Destroy=void*(__thiscall*)(void*,std::uint32_t);
        if(cancel && !slot.cancelling) {
            Native<Two>(0x0030A7C0U)(slot.node.data(),c.character,slot.frame);slot.cancelling=true;
        }
        if(slot.cancelling && (Native<PollCancel>(0x0030A5E0U)(c.character,slot.frame)&0xFFU)==0U)return false;
        // This node's Shutdown is 00718810 -> 0070AF80, NOT the vent unlock.
        Native<Two>(0x00318810U)(slot.node.data(),c.character,slot.frame);
        Native<Destroy>(0x002FA8D0U)(reinterpret_cast<void*>(slot.frame),1U);
        slot={};return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool CancelRecovery(const ap::Context& c,bool living) noexcept {
    auto* slot=RecoveryFor(c);if(!slot)return true;
    if(!living || !Live(c)){slot->quarantined=true;return false;}
    return FinishRecovery(c,*slot,true);
}
bool StartRecovery(const ap::Context& c,const ap::Frame& goal,RecoverySlot& slot) noexcept {
    std::uint32_t allocated{};
    if(!Live(c) || c.host || !c.owned || !gPvPLease.owned || gPvPLease.releasing ||
        !Read(gBase+0x012B1C20U,allocated) || allocated>=0x6F6U ||
        !ap::action_abi::Matches(gBase,nullptr,[](void*,std::uintptr_t at,void* out,std::size_t n) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(at),n);
        }))return false;
    slot.used=true;slot.context=c;
    // 006ED2B0 allocates 12 bytes: vtable, shutdown speed, movement type.
    slot.node={static_cast<std::uint32_t>(gBase+0x00EB84D0U),0U,std::min(goal.gait,3U)};
    slot.goal={goal.x,goal.y,goal.z};slot.began=slot.selectedAt=GetTickCount64();
    AIMP_ACTION_TRY {
        using Create=std::uintptr_t(__thiscall*)(void*,std::uint32_t,std::uintptr_t);
        using Dispatch=void(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint32_t,std::uint32_t);
        slot.frame=Native<Create>(0x002F15C0U)(slot.node.data(),1U,gPvPLease.tree);
        if(!slot.frame){slot={};return false;}
        // Frame factory supplies ordinary movement flags=2. Unlike a vent
        // request, no flag is changed and no manager lock is manufactured.
        Native<Dispatch>(0x002D0260U)(slot.node.data(),c.character,slot.frame,0U,0U,0U);
        return true;
    } AIMP_ACTION_CATCH {slot.quarantined=true;return false;}
}
bool Recover(const ap::Context& c,const ap::Frame* goal) noexcept {
    auto* slot=RecoveryFor(c);
    if(!goal || c.host || !Live(c) || Same(c) || gPvPLease.releasing)
        return !CancelRecovery(c,Live(c));
    std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage) || backstage || !Ordinary(state))return !CancelRecovery(c,true);
    if(slot && (slot->quarantined || slot->cancelling))return !CancelRecovery(c,true);
    policy::Point body{};if(!Body(c.character,body))return slot!=nullptr;
    const policy::Point observed{goal->x,goal->y,goal->z};
    if(!policy::Finite(observed))return !CancelRecovery(c,true);
    const float planar=std::hypot(observed.x-body.x,observed.z-body.z);
    const float vertical=std::abs(observed.y-body.y);
    if(slot && planar<=1.25F && vertical<=0.5F)return !CancelRecovery(c,true);
    const auto now=GetTickCount64();
    if(!slot) {
        if((planar<=4.0F && vertical<=1.0F) || now<recoveryRetryAt)return false;
        for(auto& candidate:recoveries)if(!candidate.used && !candidate.quarantined){slot=&candidate;break;}
        if(!slot || !StartRecovery(c,*goal,*slot)){recoveryRetryAt=now+500U;return slot && slot->used;}
    }
    if(!RecoveryLive(c,*slot)){slot->quarantined=true;return true;}
    // Stable goals avoid replaying a new plan every render frame. Only the
    // authenticated observations may advance the recovery destination.
    if(now-slot->selectedAt>=250U && policy::DistanceSquared(observed,slot->goal)>=1.0F) {
        slot->goal=observed;slot->selectedAt=now;
    }
    if(slot->steppedAt && now-slot->steppedAt<33U)return true;
    slot->steppedAt=now;
    AIMP_ACTION_TRY {
        using Move=int(__thiscall*)(void*,std::uintptr_t,std::uintptr_t,const void*,std::uint32_t,float,float,std::uint32_t);
        const auto result=Native<Move>(0x0030A940U)(slot->node.data(),c.character,slot->frame,&slot->goal,0U,0.0F,0.5F,0U);
        if(result==1) {static_cast<void>(FinishRecovery(c,*slot,false));recoveryRetryAt=now+100U;}
        else if(result!=0 || now-slot->began>15000U) {
            static_cast<void>(FinishRecovery(c,*slot,true));recoveryRetryAt=now+500U;
        }
        // A failed path is retried with a new native request after retirement;
        // it is never converted into a teleport or a synthetic arrival.
        return true;
    } AIMP_ACTION_CATCH {slot->quarantined=true;return true;}
}
bool ResourcesPending() noexcept {
#ifndef AIMP_ALIEN_ACTION_TEST
    for(const auto& d:donorMovements)if(d.used && Live(d.context))return true;
#endif
    if(active.used)return true;
    for(const auto& slot:recoveries)if(slot.used && Live(slot.context))return true;
    return false;
}
// Refreshes the retail attack-availability cache for ONE explicit player
// target. It does not select an AI target, execute the BT or modify health.
// 006FBCF0 schedules geometric/native availability; 006D65C0 consumes its
// asynchronous result. Only then may +1C/+24 choose an admitted definition.
bool AttackAvailability(const ap::Context& c,const PairIdentity& victim,std::uintptr_t& chooser,
    bool& ready,std::uint32_t& selector) noexcept {
    ready=false;selector=0U;
    std::uintptr_t manager{},table{},context{},owner{};std::uint8_t count{};
    if(!Live(c) || !PairIdentityLive(victim) || !Read(c.character+0x34B4U,manager) || !manager ||
        !Read(manager,table) || table!=gBase+0x00ECC390U ||
        !Read(manager+4U,context) || !context || !Read(context+0x128U,owner) || owner!=c.character ||
        !Read(manager+0x800U,count) || count==0U || count>25U || (chooser && chooser!=manager))return false;
    chooser=manager;
    std::uint32_t pending{},a{},b{};
    if(!Read(manager+0x20U,pending) || !Read(manager+0x810U,a) || !Read(manager+0x814U,b))return false;
    AIMP_ACTION_TRY {
        using Poll=std::uint8_t(__thiscall*)(void*);
        using Evaluate=std::uint8_t(__thiscall*)(void*,std::uintptr_t);
        using Refresh=void(__thiscall*)(void*,std::uintptr_t);
        using Check=std::uint8_t(__thiscall*)(void*,std::uint32_t);
        using Choose=std::uint32_t(__thiscall*)(void*,std::uintptr_t,std::uint32_t,std::uint32_t);
        if(pending==1U) {
            if(!Native<Poll>(0x002D65C0U)(reinterpret_cast<void*>(manager)))return true;
            // This is the exact bookkeeping after 006D65C0 in 00710DE0,
            // NOT a forced successful ray/path state. No result bits are set.
            if(!Write(manager+0x20U,std::uint32_t{0U}))return false;
        }
        // Consume completed geometry BEFORE scheduling another query. Calling
        // 006FBCF0 on every poll would continually restart the asynchronous
        // check and prevent a stable preparation boundary.
        if(a==victim.entityA && b==victim.entityB) {
            Native<Refresh>(0x002F3CA0U)(reinterpret_cast<void*>(manager),victim.character);
        }
        if(a!=victim.entityA || b!=victim.entityB ||
            !Native<Check>(0x002D5BC0U)(reinterpret_cast<void*>(manager),5U)) {
            Native<Evaluate>(0x002FBCF0U)(reinterpret_cast<void*>(manager),victim.character);
            if(!Read(manager+0x810U,a) || !Read(manager+0x814U,b) || a!=victim.entityA || b!=victim.entityB)return false;
            // The next poll must consume a pending probe before choosing.
            return true;
        }
        const auto chosen=Native<Choose>(0x002D63D0U)(reinterpret_cast<void*>(manager),victim.character,1U,15U)&0xFFFFU;
        std::uint8_t proof{};
        if(chosen>=count || !Read(manager+0x64U+chosen*0x50U,proof) || (proof&3U)!=3U)return true;
        selector=chosen;ready=true;return true;
    } AIMP_ACTION_CATCH {return false;}
}
bool AttackQueryRetired(const std::uintptr_t chooser) noexcept {
    if(!chooser)return true;
    std::uint32_t pending{};if(!Read(chooser+0x20U,pending))return false;
    if(pending!=1U)return true;
    AIMP_ACTION_TRY {
        using Poll=std::uint8_t(__thiscall*)(void*);
        if(!Native<Poll>(0x002D65C0U)(reinterpret_cast<void*>(chooser)))return false;
        return Write(chooser+0x20U,std::uint32_t{0U});
    } AIMP_ACTION_CATCH {return false;}
}
bool SelectVictim(const ap::Context& c,const ap::Frame& command,PairIdentity& victim) noexcept {
    std::array<policy::Candidate,33> candidates{};std::size_t count{};
    policy::Point source{};if(!Body(c.character,source))return false;
    const auto append=[&](std::uint64_t peer) noexcept {
        if(!peer || peer==c.scope.ownerPeer || count==candidates.size())return;
        for(std::size_t i=0;i<count;++i)if(candidates[i].peer==peer)return;
        const auto character=ParticipantForPeer(peer);PairIdentity p{};policy::Point point{};
        if(!LivingCharacter(character) || !PublishedPairVictim(character,p) || !PairIdentityLive(p) || !Body(character,point))return;
        // This is only the candidate shortlist. Actual reachability is proved
        // by the explicit-target retail availability query before entry.
        candidates[count++]={peer,point,true,true,player_hiding::Hidden(character),true};
    };
    append(gCurrent.localPeer);
    for(std::uint32_t i=0;i<gCurrent.participantCount;++i)append(gCurrent.participants[i].peer);
    const auto peer=policy::SelectVictim(source,command.yaw,c.scope.ownerPeer,{candidates.data(),count});
    return peer && PublishedPairVictim(ParticipantForPeer(peer),victim) && PairIdentityLive(victim) && victim.peer==peer;
}
bool VictimStillInReach(const ap::Context& c,const PairIdentity& victim,const ap::Frame& command) noexcept {
    if(!PairIdentityLive(victim) || !LivingCharacter(victim.character) || ParticipantForPeer(victim.peer)!=victim.character)return false;
    policy::Point source{},target{};
    if(!Body(c.character,source) || !Body(victim.character,target))return false;
    const policy::Candidate candidate{victim.peer,target,true,true,false,true};
    return policy::SelectVictim(source,command.yaw,c.scope.ownerPeer,{&candidate,1U})==victim.peer;
}
bool EnterPair(const ap::Context& c,const PairIdentity& victim,const std::uint32_t selector) noexcept {
    transactions::Scope scope{};
    if(!Live(c) || !PvPLeaseScopeCurrent() || !c.host || !gPvPLease.owned || gPvPLease.releasing ||
        !host_producer::HostScope(scope) || !HostExecutionScopeQualified(scope,c.scope.nativeGeneration) ||
        !PairIdentityLive(victim) || PairReservedCharacter(c.character) || PairSourceBusy(c.character))return false;
    interactions::Definition definition{};
    if(!animation::ResolvePairDefinition(c.character,gPvPLease.runtimeId,victim.character,victim.runtimeId,selector,definition))return false;
    std::uintptr_t root{},manager{},cycle{};
    if(!Read(gBase+kInteractionManagerGlobalRva,root) || !root || !Read(root,manager) || !manager ||
        !Read(c.character+0x36CU,cycle) || !cycle)return false;
    // Go through the hooked retail entries: registration captures the typed
    // pair, preparation captures the real outcome, then clients remap victim
    // identities through the existing paired-animation transaction runtime.
    AIMP_ACTION_TRY {
        const auto accepted=Native<CharacterInteraction>(0x00254820U)(
            reinterpret_cast<void*>(manager+0x90U),c.scope.entityA,c.scope.entityB,victim.entityA,victim.entityB,
            definition.victimAttachment>0?std::uint64_t{1U}:std::uint64_t{0U},definition.victimAttachment);
        if((accepted&0xFFU)==0U)return false;
        Native<PairedTransitionA>(0x0025C4E0U)(reinterpret_cast<void*>(cycle),victim.character,
            static_cast<std::int32_t>(selector),0U);
        return true;
    } AIMP_ACTION_CATCH {return false;}
}
void Clear() noexcept {
    if(active.victim.character)player_hiding::FinishPvPSearch(active.victim.character);
    active={};
}
void Terminal(ap::Frame& result,Phase phase,Reason reason) noexcept {
    result=active.receipt;result.phase=phase;result.reason=reason;++result.resolutionRevision;
    active.receipt=result;active.terminalSent=true;
    TraceVent();
}
void Cancel(const ap::Context& c,bool living) noexcept {
    static_cast<void>(StopDonor(c));
    static_cast<void>(CancelRecovery(c,living));
    if(!Same(c))return;
    if(!living || !Live(c)) {
        if(active.vent)active.vent->quarantined=true;
        Clear();return;
    }
    active.cancelled=true;
    TraceVent();
    if(active.remoteVent && !active.vent){Clear();return;}
    if(active.victim.character)player_hiding::FinishPvPSearch(active.victim.character);
    if(active.vent) {
        bool done{};
        if(!FinishVent(c,*active.vent,true,done)) {active.vent->quarantined=true;Clear();return;}
        if(done)Clear();
        return;
    }
    if(active.moveArrived && (active.receipt.action==Action::EnterVent || active.receipt.action==Action::ExitVent)) {
        // The native node was already shut down and released. Stationary
        // backstage77 is not an outstanding traversal resource to retain.
        Clear();return;
    }
    // A registered pair belongs to the existing native retirement path. Never
    // force Idle/zero a cycle or mark a death complete on cancellation.
    if(!active.entrySubmitted && !active.entered && AttackQueryRetired(active.chooser))Clear();
}
bool InterruptVent(const ap::Context& c,ap::Frame& result) noexcept {
    if(!Same(c) || active.terminalSent ||
        (active.receipt.action!=Action::EnterVent && active.receipt.action!=Action::ExitVent))return false;
    Terminal(result,Phase::Cancelled,Reason::NativeBusy);
    Cancel(c,true);return true;
}
bool Begin(const ap::Context& c,const ap::Frame& command,ap::Frame& result) noexcept {
    result=command;result.resolutionRevision=1U;
    if(!Live(c) || !PvPLeaseScopeCurrent() || gPvPLease.releasing){Reject(result,Reason::Stale);return true;}
    if(Quarantined(c)){Reject(result,Reason::Fault);return true;}
    if(!ap::action_abi::Matches(gBase,nullptr,
        [](void*,std::uintptr_t address,void* out,std::size_t size) noexcept {
            return crash_guard::TryCopy(out,reinterpret_cast<const void*>(address),size);
        })) {Reject(result,Reason::NativeUnavailable);return true;}
    if(!c.host) {
        // Client never invokes the attack selector. The existing AISync native
        // pair transaction, with its real Host outcome, is its sole executor.
        if(command.kind!=ap::Kind::Resolution)return false;
        if(command.phase==Phase::Rejected || command.phase==Phase::Failed || command.phase==Phase::Cancelled) {
            if(Same(c) && active.receipt.hostSequence==command.hostSequence)Cancel(c,true);
            return true;
        }
        if(command.phase==Phase::Completed) {
            if(Same(c) && active.receipt.hostSequence==command.hostSequence)active.hostEnded=true;
            return true;
        }
        if(Same(c)) {
            if(active.receipt.hostSequence==command.hostSequence && command.action==Action::Attack) {
                active.receipt=command;return true;
            }
            result.phase=Phase::Failed;result.reason=Reason::NativeBusy;return true;
        }
    } else {
        if(edgeEpoch!=command.epoch) {if(active.used){Reject(result,Reason::Stale);return true;}
            edgeEpoch=command.epoch;edges.Reset(edgeEpoch);}
        if(!edges.Consume(command.epoch,command.hostSequence,GetTickCount64(),command.action==Action::Attack,active.used)) {
            Reject(result,active.used?Reason::NativeBusy:Reason::Cooldown);return true;
        }
    }
    std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage)){Reject(result,Reason::NativeUnavailable);return true;}
    if(command.action==Action::Attack) {
        if(c.host && (backstage || !Ordinary(state))){Reject(result,Reason::NativeBusy);return true;}
        PairIdentity victim{};
        if(c.host && !SelectVictim(c,command,victim)){Reject(result,Reason::NoTarget);return true;}
        if(!c.host && (!PublishedPairVictim(ParticipantForPeer(command.targetPeer),victim) ||
            !PairIdentityLive(victim) || victim.peer!=command.targetPeer || victim.entityA!=command.targetA ||
            victim.entityB!=command.targetB)) {Reject(result,Reason::NoTarget);return true;}
        active={};active.used=true;active.context=c;active.began=GetTickCount64();active.victim=victim;
        if(c.host) {
            policy::Point target{};
            if(!Body(victim.character,target)){Clear();Reject(result,Reason::Stale);return true;}
            result.targetPeer=victim.peer;result.targetA=victim.entityA;result.targetB=victim.entityB;
            result.x=target.x;result.y=target.y;result.z=target.z;result.selector=0U;
            result.phase=Phase::Preparing;result.reason=Reason::None;
        }
        active.receipt=result;return true;
    }
    if(command.action!=Action::EnterVent && command.action!=Action::ExitVent)return false;
    if((command.action==Action::EnterVent && backstage) || (command.action==Action::ExitVent && !backstage)) {
        Reject(result,Reason::WrongStage);return true;
    }
    if((!backstage && !Ordinary(state)) || state==18U || state==78U || state==79U) {
        Reject(result,Reason::NativeBusy);return true;
    }
    result.reason=Reason::NoVent;
    if(!SelectPortal(c,command,result)){Reject(result,result.reason);return true;}
    if(!c.localOwner) {
        // The owning player runs the donor traversal. Running another path
        // on the Host can circle/stall and cancel a successful owner entry.
        result.phase=Phase::Started;result.reason=Reason::None;
        active={};active.used=true;active.remoteVent=true;active.context=c;
        active.receipt=result;active.began=GetTickCount64();TraceVent();return true;
    }
    auto* slot=FreeVentSlot();
    if(!slot || !StartVent(c,result,*slot)){Reject(result,Reason::NativeUnavailable);return true;}
    result.phase=Phase::Started;result.reason=Reason::None;
    active={};active.used=true;active.context=c;active.receipt=result;active.vent=slot;active.began=GetTickCount64();
    active.ventProgressAt=active.began;
    TraceVent();
    return true;
}
bool Poll(const ap::Context& c,ap::Frame& result) noexcept {
    if(!Same(c))return false;
    if(!Live(c)){Cancel(c,false);return false;}
    const auto now=GetTickCount64();std::uint8_t state{};bool backstage{};
    if(!Cycle(c.character,state,backstage))return false;
    if(active.remoteVent) {
        if(active.vent) {
            int presentation{};
            if(!StepVent(c,*active.vent,active.receipt,presentation)) {
                active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
            }
            if(presentation!=0 || active.cancelled) {
                bool done{};
                if(!FinishVent(c,*active.vent,presentation==2 || active.cancelled,done)) {
                    active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
                }
                if(done)active.vent=nullptr;
            }
            if(!Cycle(c.character,state,backstage))return false;
        }
        if(active.cancelled){if(!active.vent)Clear();return false;}
        TraceVent();
        const bool expected=ap::spatial::VentStageReady(state,backstage,active.receipt.action==Action::EnterVent,true);
        if(expected && !active.vent && active.ownerFinished) {
            Terminal(result,Phase::Completed,Reason::None);Clear();return true;
        }
        if(now-active.began>(active.receipt.action==Action::EnterVent?12000U:27000U)) {
            Terminal(result,Phase::Failed,Reason::Timeout);Cancel(c,true);return true;
        }
        return false;
    }
    if(active.cancelled) {
        if(active.vent){Cancel(c,true);return false;}
        const auto* actor=Find(c.character);
        if(!Protected(state) && !PairSourceBusy(c.character) && !PairReservedCharacter(c.character) &&
            (!active.entrySubmitted || (actor && UnownedCleanupAllowed(*actor) && now-active.submittedAt>=500U)) &&
            AttackQueryRetired(active.chooser))Clear();
        return false;
    }
    if(active.receipt.action==Action::Attack) {
        if(!c.host) {
            if(!active.entered && state!=78U && active.victim.character)
                static_cast<void>(player_hiding::PreparePvPSearch(active.victim.character,c.scope.nativeGeneration));
            if(state==78U)active.entered=true;
            if((active.entered && !Protected(state)) || (active.hostEnded && !Protected(state)))Clear();
            else if(now-active.began>policy::kActionTimeoutMs && !Protected(state))Clear();
            return false;
        }
        if(active.receipt.phase==Phase::Preparing) {
            if(active.entrySubmitted) {
                if(state==78U) {
                    active.entered=true;result=active.receipt;result.phase=Phase::Started;result.reason=Reason::None;
                    ++result.resolutionRevision;active.receipt=result;return true;
                }
                if(now-active.submittedAt>1500U) {
                    Terminal(result,Phase::Failed,Reason::NativeRefused);active.cancelled=true;return true;
                }
                return false;
            }
            if(!VictimStillInReach(c,active.victim,active.receipt)) {
                Terminal(result,Phase::Rejected,Reason::NoTarget);active.cancelled=true;return true;
            }
            if(!player_hiding::PreparePvPSearch(active.victim.character,c.scope.nativeGeneration)) {
                Terminal(result,Phase::Rejected,Reason::NotReady);active.cancelled=true;return true;
            }
            bool ready{};std::uint32_t selector{};
            if(!AttackAvailability(c,active.victim,active.chooser,ready,selector)) {
                Terminal(result,Phase::Failed,Reason::NativeUnavailable);active.cancelled=true;return true;
            }
            if(ready && AttackQueryRetired(active.chooser)) {
                active.receipt.selector=selector;
                active.entrySubmitted=true;active.submittedAt=now;
                if(!EnterPair(c,active.victim,selector)) {
                    Terminal(result,Phase::Failed,Reason::NativeRefused);active.cancelled=true;return true;
                }
                // A successful registration/void transition call is not proof
                // of entry. Publish Started only after observing source cycle78.
                if(Cycle(c.character,state,backstage) && state==78U) {
                    active.entered=true;result=active.receipt;result.phase=Phase::Started;result.reason=Reason::None;
                    ++result.resolutionRevision;active.receipt=result;return true;
                }
                return false;
            }
            if(now-active.began>1000U){Terminal(result,Phase::Rejected,Reason::NativeRefused);active.cancelled=true;return true;}
            return false;
        }
        auto* actor=Find(c.character);
        if(actor && !actor->faulted && !actor->duplicate) {
            CaptureOutcome(*actor);CaptureInteractionCycle(*actor);Publish(*actor);
            // Completion follows the native pair's source-end evidence, not HP
            // zero or elapsed time. The victim death flow remains independent.
            if(!Protected(state) && (actor->desired.outcome.flags&OutcomeSourceEnded)!=0U) {
                Terminal(result,Phase::Completed,Reason::None);Clear();return true;
            }
        }
        if(now-active.began>policy::kActionTimeoutMs && !active.terminalSent) {
            Terminal(result,Phase::Failed,Reason::Timeout);active.cancelled=true;return true;
        }
        return false;
    }
    if(!active.vent && !active.moveArrived)return false;
    if(active.lastStep && now-active.lastStep<33U)return false;
    active.lastStep=now;
    int nativeResult=active.moveArrived?1:0;
    if(!active.moveArrived && !StepVent(c,*active.vent,active.receipt,nativeResult)) {
        active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
    }
    if(nativeResult==1)active.moveArrived=true;
    TraceVent(static_cast<unsigned>(nativeResult));
    // Retail shuts down a finished node immediately, releasing its request
    // and vent-manager lock. Waiting for the stage change while retaining
    // that lock can stall the transition. Stage confirmation remains separate.
    if(active.moveArrived && active.vent) {
        bool done{};
        if(!FinishVent(c,*active.vent,false,done) || !done) {
            active.vent->quarantined=true;Terminal(result,Phase::Failed,Reason::Fault);Clear();return true;
        }
        active.vent=nullptr;
        if(!Cycle(c.character,state,backstage))return false;
    }
    if(state==18U || state==75U || state==77U)active.entered=true;
    const bool expected=ap::spatial::VentStageReady(state,backstage,active.receipt.action==Action::EnterVent);
    if(nativeResult==1 && expected) {
        Terminal(result,Phase::Completed,Reason::None);
        Clear();return true;
    }
    // Reuse the donor's bounded traversal windows. A close entry that circles
    // without getting nearer is cancelled instead of capturing input for 30s.
    policy::Point body{};
    if(!active.moveArrived && Body(c.character,body)) {
        const policy::Point goal=active.receipt.action==Action::EnterVent?
            policy::Point{active.receipt.x,active.receipt.y,active.receipt.z}:
            policy::Point{active.receipt.resolvedX,active.receipt.resolvedY,active.receipt.resolvedZ};
        const float distance=std::sqrt(policy::DistanceSquared(body,goal));
        if(distance+0.25F<active.ventBestDistance) {active.ventBestDistance=distance;active.ventProgressAt=now;}
    }
    const auto traversalLimit=active.receipt.action==Action::EnterVent?10000U:25000U;
    const bool stalled=active.receipt.action==Action::EnterVent && !Protected(state) &&
        !active.moveArrived && now-active.ventProgressAt>5000U;
    if(nativeResult==2 || stalled || now-active.began>traversalLimit) {
        Terminal(result,Phase::Failed,nativeResult==2?Reason::NativeRefused:Reason::Timeout);
        Cancel(c,true);return true;
    }
    return false;
}
#ifndef AIMP_ALIEN_ACTION_TEST
bool ReplicatePose(const ap::Context& c,const ap::Frame& pose) noexcept {
    if((pose.flags&ap::OwnerMeasured)==0U)return false;
    if(Same(c) && active.remoteVent) {
        const bool entering=active.receipt.action==Action::EnterVent;
        if((pose.flags&ap::NativeBusy)==0U && ((pose.flags&ap::Backstage)!=0U)==entering)
            active.ownerFinished=true;
        if(entering)return ReplicatePvPPose(c,pose);
        if((pose.flags&ap::VentAnimating)!=0U) {
            if(active.presentationStarted || active.cancelled)return false;
            // Trigger the real exit node once the owner is in its exit passage.
            // Start at the approved opening, avoiding a second hidden path.
            if(!Live(c) || !StopDonor(c) || !CancelRecovery(c,true))return false;
            auto* slot=FreeVentSlot();if(!slot)return false;
            const float opening[]{active.receipt.x,active.receipt.y,active.receipt.z};
            if(!character_direct_step_runtime::ApplyAIRecoveryTransform(c.character,opening,pose.yaw))return false;
            if(!StartVent(c,active.receipt,*slot))return false;
            active.vent=slot;active.presentationStarted=true;return true;
        }
        if(active.vent)return false; // the native passage owns placement
    }
    if((pose.flags&ap::VentAnimating)!=0U)return false;
    return ReplicatePvPPose(c,pose);
}
#endif
void RetirePending() noexcept {
#ifndef AIMP_ALIEN_ACTION_TEST
    if(gPvPLease.releasing)for(auto& d:donorMovements)if(d.used && Live(d.context))
        static_cast<void>(StopDonor(d.context));
#endif
    for(auto& slot:recoveries)if(slot.used && slot.cancelling && !slot.quarantined)
        static_cast<void>(CancelRecovery(slot.context,Live(slot.context)));
    if(!active.used || !active.cancelled)return;
    ap::Frame ignored{};static_cast<void>(Poll(active.context,ignored));
}
const ap::ActionHooks hooks{&Begin,&Poll,&Busy,&Cancel,&Recover,&CancelRecovery
#ifndef AIMP_ALIEN_ACTION_TEST
    ,&DriveDonor,&ReplicatePose,&InterruptVent
#else
    ,nullptr,nullptr,&InterruptVent
#endif
};
#undef AIMP_ACTION_TRY
#undef AIMP_ACTION_CATCH
} // namespace pvp_actions
const alien_pvp::ActionHooks& PvPActionHooks() noexcept {return pvp_actions::hooks;}
void RetirePvPActions() noexcept {pvp_actions::RetirePending();}
bool InstallPvPDonorVentHooks(HookManager& hooks,std::string& error) {
    HookRequest locomotion;locomotion.name="alien_donor_responsive_locomotion";
    locomotion.target=reinterpret_cast<void*>(gBase+0x0004D4F0U);
    locomotion.detour=reinterpret_cast<void*>(&pvp_actions::PilotLocomotionDetour);
    locomotion.patchLength=5U;locomotion.relocationSafePrefix=true;
    locomotion.expected=MaskedBytes{{0x8B,0x11,0x8B,0x41,0x04},"xxxxx"};
    locomotion.trampolineOut=&pvp_actions::pilotLocomotionOriginal;
    if(!hooks.Install(locomotion,error))return false;
    for(const auto exit:{false,true}) {
        HookRequest request;request.name=exit?"alien_donor_pick_exit":"alien_donor_pick_entry";
        request.target=reinterpret_cast<void*>(gBase+(exit?0x00013310U:0x000132F0U));
        request.detour=exit?reinterpret_cast<void*>(&pvp_actions::PickExitDetour):reinterpret_cast<void*>(&pvp_actions::PickEnterDetour);
        request.patchLength=8U;request.relocationSafePrefix=true;
        request.expected=MaskedBytes{{0x8B,0x44,0x24,0x04,0x8B,0x44,0x81,static_cast<std::uint8_t>(exit?0x48:0x40)},"xxxxxxxx"};
        request.trampolineOut=exit?&pvp_actions::ventExitOriginal:&pvp_actions::ventEnterOriginal;
        if(!hooks.Install(request,error))return false;
    }
    return true;
}
