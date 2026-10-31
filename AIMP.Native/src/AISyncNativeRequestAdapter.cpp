#include "aimp/AISyncNativeRequestAdapter.h"
#include "aimp/AISyncExecutionCodec.h"
#include "aimp/CharacterResourceAdmission.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>

namespace aimp::ai_sync::execution {
namespace {
std::atomic<IntentCheck> gIntentCheck{};
std::atomic<IntentInvoke> gIntentInvoke{};
using namespace transactions;
constexpr std::uintptr_t kScheduler = 0x2CD0;
constexpr std::uintptr_t kTurnFactory = 0x00282450, kStanceFactory = 0x002823F0, kActionFactory = 0x00282410;
constexpr std::uintptr_t kTurnType = 0x00EB19CC, kStanceType = 0x00EB19BC, kActionType = 0x00EB198C;
// Kind5 overrides the generic status method 006777E0 with 006799F0.
constexpr std::uintptr_t kActionStatus = 0x002799F0;
constexpr std::uintptr_t kDirectedFactory = 0x002823B0, kDirectedType = 0x00EB19AC, kDirectedStatus = 0x002815A0;
constexpr std::uintptr_t kPlacementFactory = 0x00282430, kPlacementType = 0x00EB199C;
constexpr std::uintptr_t kSubmit = 0x00283CE0, kCancel = 0x00283DB0, kRelease = 0x0000D770;
struct BusyScope {
    bool& flag;
    explicit BusyScope(bool& value) noexcept : flag(value) { flag = true; }
    ~BusyScope() { flag = false; }
};
template<class T> bool Read(std::uintptr_t at, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(at); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Match(std::uintptr_t at, std::initializer_list<unsigned char> bytes) noexcept {
    for (auto expected : bytes) { unsigned char actual{}; if (!Read(at++, actual) || actual != expected) return false; }
    return true;
}
bool ControlImage(std::uintptr_t base,std::uint32_t kind) noexcept {
    const auto* contract=scheduler_control::Find(kind);
    std::uintptr_t eligibility{},status{};
    return contract&&Match(base+contract->factoryRva,{0x51,0x56,0x8B,0x74,0x24,0x0C,0x56})&&
        Match(base+contract->factoryRva+0x18,{0xC2,0x04,0x00})&&
        Read(base+scheduler_control::kTypeRva+4U,eligibility)&&eligibility==base+0xAA70F0U&&
        Read(base+scheduler_control::kTypeRva+8U,status)&&status==base+scheduler_control::kStatusRva&&
        Match(status,{0xC2,0x0C,0x00});
}
bool PlacementImage(std::uintptr_t base) noexcept {
    std::uintptr_t eligibility{},status{},tick{};
    return Match(base+kPlacementFactory,{0x51,0x56,0x8B,0x74,0x24,0x0C,0x56})&&
        Match(base+kPlacementFactory+0x18,{0xC2,0x04,0x00})&&
        Read(base+kPlacementType+4,eligibility)&&eligibility==base+0xAA70F0U&&Match(eligibility,{0xB0,0x01,0xC3})&&
        Read(base+kPlacementType+8,status)&&status==base+0x248BB0U&&Match(status,{0xC2,0x0C,0x00})&&
        Read(base+kPlacementType+12,tick)&&tick==base+0x74D3F0U&&Match(tick,{0xC2,0x04,0x00});
}
std::uintptr_t Type(std::uint32_t kind) noexcept {
    if (scheduler_control::Supported(kind)) return scheduler_control::kTypeRva;
    switch(kind) { case 1: return 0xEB1A70; case 2: return 0xEB1A60; case 3: return 0xEB1A50;
        case 4: return kTurnType; case 7: return kStanceType; case 5: return kActionType; case 8: return kPlacementType; case 9: return kDirectedType; default: return 0; }
}
LifecycleTrace gLifecycleTrace{};
std::atomic<CommandTrace> gCommandTrace{};
std::atomic<ConsumedAction> gConsumedAction{};
std::atomic<std::uint64_t> gConsumedActions{};
struct ConsumedKey { transactions::Scope scope{}; transactions::OwnerKey owner{}; std::uint64_t command{}; bool used{}; };
std::array<ConsumedKey,16> gConsumedKeys{}; // Engine thread only.
std::size_t gConsumedNext{};
ConsumedKey* FindConsumed(const transactions::Record& r) noexcept {
    for(auto& key:gConsumedKeys)if(key.used&&key.scope==r.scope&&key.owner==r.owner&&key.command==r.commandId)return &key;
    return nullptr;
}
// Submit consumed by an entered interaction, or a later operation of it.
bool Consumed(const transactions::Record& r) noexcept {
    if(r.nativeKind!=kActionKind)return false;
    if(FindConsumed(r))return r.opcode!=transactions::Op::Submit;
    const auto query=gConsumedAction.load(std::memory_order_acquire);
    return r.opcode==transactions::Op::Submit&&query&&query(r);
}
void TraceCommand(const Record& record,std::uintptr_t character,NativeOutcome outcome) noexcept {
    if(const auto trace=gCommandTrace.load(std::memory_order_acquire))trace(record,character,static_cast<std::uint32_t>(outcome));
}
void TraceLifecycle(std::uint32_t stage,const Record& r,std::uintptr_t character,const float* before,std::uint32_t result) noexcept {
    if(!gLifecycleTrace)return;
    LifecycleTraceSample sample{};
    sample.stage=stage;sample.kind=r.nativeKind;sample.mode=r.mode;sample.result=result;sample.seq=r.globalSeq;
    sample.hasGoal=(r.flags&HasGoal)!=0;sample.x=r.goalX;sample.y=r.goalY;sample.z=r.goalZ;sample.yaw=r.aux;
    if(before){sample.beforeX=before[0];sample.beforeY=before[1];sample.beforeZ=before[2];}
    if(character){Read(character+0x70,sample.afterX);Read(character+0x74,sample.afterY);Read(character+0x78,sample.afterZ);}
    gLifecycleTrace(sample);
}
bool WriteParameters(std::uintptr_t request, const Record& r) noexcept {
    __try {
        // The allocation for6/11 ends after the base request. Writing Turn
        // fields here would corrupt adjacent storage rather than author data.
        if (scheduler_control::Supported(r.nativeKind)) return true;
        if (r.nativeKind == kPlacementKind) {
            *reinterpret_cast<float*>(request + 0x1C) = r.goalX;
            *reinterpret_cast<float*>(request + 0x20) = r.goalY;
            *reinterpret_cast<float*>(request + 0x24) = r.goalZ;
            *reinterpret_cast<float*>(request + 0x28) = r.aux;
            *reinterpret_cast<std::uint8_t*>(request + 0x2C) = static_cast<std::uint8_t>(r.mode);
        } else if (r.nativeKind == 7) *reinterpret_cast<std::uint32_t*>(request + 0x1C) = r.mode;
        else if (r.nativeKind == kDirectedKind) {
            // Same value writes as 00686860; +34 rebases at the LOCAL Character.
            *reinterpret_cast<float*>(request + 0x1C) = r.aux;
            *reinterpret_cast<float*>(request + 0x20) = r.distance;
            *reinterpret_cast<float*>(request + 0x24) = r.stopDistance;
            *reinterpret_cast<std::uint8_t*>(request + 0x34) = 1U;
        } else if (r.nativeKind == kActionKind) {
            // Same fields the retail producers write before 00683CE0.
            *reinterpret_cast<std::uint32_t*>(request + 0x1C) = r.mode;
            *reinterpret_cast<std::uint8_t*>(request + 0x20) = static_cast<std::uint8_t>(r.gait & 1U);
            *reinterpret_cast<std::uint8_t*>(request + 0x21) = static_cast<std::uint8_t>((r.gait >> 1U) & 1U);
            *reinterpret_cast<std::uint8_t*>(request + 0x22) = static_cast<std::uint8_t>((r.gait >> 2U) & 1U);
        } else {
            const bool point = (r.flags & HasGoal) != 0;
            *reinterpret_cast<std::uint32_t*>(request + 0x30) = point ? 0U : 1U;
            if (point) {
                *reinterpret_cast<float*>(request + 0x1C) = r.goalX;
                *reinterpret_cast<float*>(request + 0x20) = r.goalY;
                *reinterpret_cast<float*>(request + 0x24) = r.goalZ;
            } else *reinterpret_cast<float*>(request + 0x28) = r.aux;
            *reinterpret_cast<float*>(request + 0x2C) = r.distance;
            *reinterpret_cast<std::uint32_t*>(request + 0x34) = r.mode & 1U;
            *reinterpret_cast<std::uint8_t*>(request + 0x38) = static_cast<std::uint8_t>((r.mode >> 1U) & 1U);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool NativeRequestAdapter::Initialize(std::uintptr_t base, bool exact, Permission permission,
    ResolveActor resolve, void* context, DependencyGate dependencyGate, ResolveActor lifecycleResolver) noexcept {
    if (busy_ || retiring_ || ActiveCount() != 0 || !exact || base == 0 || base > UINTPTR_MAX - 0x00EC0000U ||
        permission == nullptr || resolve == nullptr) return false;
    base_ = base;
    if (!CheckImage()) { base_ = 0; return false; }
    permission_ = permission; resolve_ = resolve; lifecycleResolver_=lifecycleResolver; context_ = context; dependencyGate_=dependencyGate; return true;
}
bool NativeRequestAdapter::CheckImage() const noexcept {
    std::uintptr_t turn{}, stance{}, action{}, directed{}, markerEligibility{}, markerStatus{};
    // Factories and status methods corroborated by the existing navigation
    // adapter. Submit/Cancel may already be detoured by mandatory AISync hooks;
    // the bootstrap's exact-build proof is therefore also required.
    return Match(base_ + kTurnFactory, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) &&
        Match(base_ + kTurnFactory + 0x18, {0xC2,0x04,0x00}) &&
        Match(base_ + kStanceFactory, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) &&
        Match(base_ + kStanceFactory + 0x18, {0xC2,0x04,0x00}) &&
        Read(base_ + kTurnType + 8, turn) && turn == base_ + 0x002777E0 &&
        Read(base_ + kStanceType + 8, stance) && stance == base_ + 0x002777E0 &&
        Match(base_ + kActionFactory, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) &&
        Match(base_ + kActionFactory + 0x18, {0xC2,0x04,0x00}) &&
        Read(base_ + kActionType + 8, action) && action == base_ + kActionStatus &&
        Match(base_ + kDirectedFactory, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) &&
        Match(base_ + kDirectedFactory + 0x18, {0xC2,0x04,0x00}) &&
        Read(base_ + kDirectedType + 8, directed) && directed == base_ + kDirectedStatus &&
        // 00d6 saved00683530/00677800 and complete native00682560. The
        // initializer itself is parent-detoured; the callback stays native.
        Match(base_+0x283530U,{0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) &&
        Match(base_+0x283548U,{0xC2,0x04,0x00}) &&
        Read(base_+0xEB1A64U,markerEligibility) && markerEligibility==base_+0x277800U &&
        Match(markerEligibility,{0x33,0xC0,0x83,0xB9,0xB0,0x00,0x00,0x00,0xFF,0x0F,0x95,0xC0,0xC3}) &&
        Read(base_+0xEB1A68U,markerStatus) && markerStatus==base_+0x282560U &&
        Match(markerStatus+0x49U,{0xE8,0x62,0xF2,0xFF,0xFF}) &&
        Match(base_ + kRelease, {0x56,0x8B,0x71,0x04});
}
std::size_t NativeRequestAdapter::ActiveCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(leases_.begin(), leases_.end(), [](const auto& l) { return l.used; }));
}
std::size_t NativeRequestAdapter::ActiveCount(const OwnerKey& owner) const noexcept {
    return static_cast<std::size_t>(std::count_if(leases_.begin(), leases_.end(), [&](const auto& lease) {
        return lease.used && lease.owner.entityA == owner.entityA && lease.owner.entityB == owner.entityB &&
            lease.owner.capability == owner.capability;
    }));
}
NativeRequestAdapter::Lease* NativeRequestAdapter::Find(const Record& r) noexcept {
    for (auto& lease : leases_) if (lease.used && !lease.secondary && lease.scope == r.scope && lease.owner == r.owner && lease.command == r.commandId)
        return &lease;
    return nullptr;
}
std::size_t NativeRequestAdapter::ActorRequests(std::uintptr_t character,const Record* action) const noexcept {
    return static_cast<std::size_t>(std::count_if(leases_.begin(),leases_.end(),[&](const auto& lease) {
        if(!lease.used||lease.actor.character!=character)return false;
        // A retained strong Ref is not an active scheduler request. Native
        // terminal status (3..6) or our completed Cancel permits the next
        // interaction; the Ref still stays here until its exact Release.
        // Unknown/unreadable state must continue to block admission.
        if(!lease.unknown) {
            std::uint32_t status{};
            if(lease.cancelled||(lease.admitted&&Read(lease.ref.object+0xC,status)&&status>=3U&&status<=6U))return false;
        }
        return !(action&&action->nativeKind==kActionKind&&lease.kind==kActionKind&&!lease.unknown&&lease.admitted&&!lease.cancelled&&
            lease.scope==action->scope&&lease.owner==action->owner&&lease.command==action->commandId&&lease.revision==action->parameterRevision);
    }));
}
Readiness NativeRequestAdapter::Resources(std::uintptr_t character) noexcept {
    if(character_direct_step_runtime::HasLiveMovementResources(character))return Readiness::Ready;
    waitReason_=4U;return Readiness::Waiting;
}
bool NativeRequestAdapter::Resolve(const Record& r, LocalActorBinding& actor) noexcept {
    const bool lifecycle=ProfileForKind(r.nativeKind)==kProfileLifecycle||r.nativeKind==native_intent::kKind;
    const auto resolver=lifecycle&&lifecycleResolver_?lifecycleResolver_:resolve_;
    if (base_ == 0 || permission_ == nullptr || resolve_ == nullptr || !permission_(context_, r) ||
        !resolver(context_, r.owner, actor) || !actor.exclusiveExecution || actor.character == 0 || actor.runtimeId == 0) return false;
    std::uint32_t id{}; std::uintptr_t schedulerOwner{};
    return Read(actor.character + 0x50, id) && id == actor.runtimeId &&
        Read(actor.character + kScheduler, schedulerOwner) && schedulerOwner == actor.character;
}
bool NativeRequestAdapter::Live(const Lease& lease, const LocalActorBinding& actor) const noexcept {
    std::uintptr_t scheduler{}, type{};std::uint32_t kind{};Ref resource{};
    return lease.actor.character == actor.character && lease.actor.runtimeId == actor.runtimeId &&
        lease.actor.creationSerial==actor.creationSerial&&
        lease.ref.object != 0 && lease.ref.control != 0 &&
        Read(lease.ref.object + 0x14, scheduler) && scheduler == actor.character + kScheduler &&
        Read(lease.ref.object, type) && type == base_ + Type(lease.kind) &&
        ((lease.kind!=kPlacementKind&&lease.kind!=2)||(Read(lease.ref.object+0x10,kind)&&kind==lease.kind))&&
        ((lease.kind!=1&&lease.kind!=2&&lease.kind!=3)||(Read(actor.character+0x42A8,resource)&&resource.object!=0&&resource.control!=0&&
            resource.object==lease.movementResource.object&&resource.control==lease.movementResource.control));
}
bool NativeRequestAdapter::IsOwnedPlacement(std::uintptr_t scheduler,RetirementIdentity* identity) const noexcept {
    if(!base_||retiring_||!PlacementDedupReady()||!resolve_)return false;
    std::uintptr_t character{},head{},first{},request{},control{};std::uint32_t count{},status{},runtime{},strong{};
    if(!Read(scheduler,character)||!character||scheduler!=character+kScheduler||
        !Read(character+0x50,runtime)||!runtime||!Read(scheduler+4,head)||!head||
        !Read(scheduler+8,count)||!count||!Read(head,first)||!first||first==head||
        !Read(first+8,request)||!Read(first+12,control)||!control||
        !Read(request+0xC,status)||(status!=1U&&status!=2U))return false;
    for(const auto& lease:leases_)if(lease.used&&!lease.secondary&&lease.kind==kPlacementKind&&lease.admitted&&
        !lease.unknown&&!lease.cancelled&&lease.ref.object==request&&lease.ref.control==control&&
        lease.actor.character==character&&lease.actor.runtimeId==runtime) {
        LocalActorBinding actor{};
        if(!resolve_(context_,lease.owner,actor)||!actor.exclusiveExecution||!Live(lease,actor)||
            !Read(control+4,strong)||!strong)return false;
        if(identity)*identity={lease.scope,lease.owner,lease.command,lease.actor,lease.kind,lease.revision};
        return true;
    }
    return false;
}
void SetLifecycleTrace(LifecycleTrace trace) noexcept { gLifecycleTrace = trace; }
void SetNativeIntentAdapter(IntentCheck check,IntentInvoke invoke) noexcept {
    gIntentInvoke.store(invoke,std::memory_order_release);gIntentCheck.store(check,std::memory_order_release);
}
void SetCommandTrace(CommandTrace trace) noexcept { gCommandTrace.store(trace,std::memory_order_release); }
void SetConsumedAction(ConsumedAction query) noexcept { gConsumedAction.store(query,std::memory_order_release); }
std::uint64_t ConsumedActions() noexcept { return gConsumedActions.load(std::memory_order_relaxed); }
Readiness NativeRequestAdapter::Check(const Record& r) noexcept {
    if (!busy_ && !retiring_ && ProfileForKind(r.nativeKind)==kProfileLifecycle && !ValidateExecutable(r))
        TraceLifecycle(10U,r,0,nullptr,0U);
    if (busy_ || retiring_ || !ValidateExecutable(r)) return Readiness::Rejected;
    // Schema/profile support never grants local safety for duplicated native
    // lifecycle children. Cleanup of an existing lease remains possible if
    // the integration proof is withdrawn while a session is retiring.
    if(r.nativeKind==kPlacementKind&&(!PlacementDedupReady()||!PlacementImage(base_))&&
        r.opcode!=Op::Cancel&&r.opcode!=Op::Stop&&r.opcode!=Op::Release)return Readiness::Rejected;
    const BusyScope callbackGuard(busy_);
    const bool cleanup=r.opcode==Op::Cancel||r.opcode==Op::Stop||r.opcode==Op::Release;
    // A route can fail asynchronously after its Submit was acknowledged. If
    // native cancellation then has an unknown outcome, another command of the
    // SAME owner must not enter a second executor. Other actors remain usable.
    if(!cleanup)for(const auto& lease:leases_)if(lease.used&&lease.unknown&&
        lease.scope==r.scope&&lease.owner==r.owner)return Readiness::Rejected;
    if(Consumed(r))return Readiness::Ready;
    if(dependencyGate_) {
        const auto gate=dependencyGate_(context_,r);
        if(gate==Readiness::Waiting)waitReason_=2U;
        if(gate!=Readiness::Ready)return gate;
    }
    if (Find(r) == nullptr && (r.opcode == Op::Cancel || r.opcode == Op::Stop || r.opcode == Op::Release))
        return base_ != 0 && permission_ != nullptr && permission_(context_, r) ? Readiness::Ready : Readiness::Rejected;
    LocalActorBinding actor{};
    if (!Resolve(r, actor)) { waitReason_=3U; return Readiness::Waiting; }
    if(scheduler_control::Supported(r.nativeKind)&&!ControlImage(base_,r.nativeKind))return Readiness::Rejected;
    if(r.nativeKind==native_intent::kKind) {
        const auto check=gIntentCheck.load(std::memory_order_acquire);
        const auto result=check?check(r,actor):Readiness::Rejected;
        if(result==Readiness::Waiting)waitReason_=9U;
        return r.opcode==Op::Submit?result:Readiness::Rejected;
    }
    if(ProfileForKind(r.nativeKind)==kProfileLifecycle)
        return r.opcode==Op::Submit?Readiness::Ready:Readiness::Rejected;
    if ((r.nativeKind == 1 || r.nativeKind == 2 || r.nativeKind == 3)) return CheckMovement(r,actor);
    if (auto* lease = Find(r)) {
        if (lease->unknown || lease->kind != r.nativeKind || lease->admission!=r.admission || !Live(*lease, actor)) return Readiness::Rejected;
        if (r.opcode == Op::Release && !lease->cancelled) {
            std::uint32_t status{};
            if (!Read(lease->ref.object + 0xC, status)) return Readiness::Rejected;
            if (status > 6) return Readiness::Rejected;
        }
        return Resources(actor.character);
    }
    if (r.opcode != Op::Submit) return Readiness::Rejected;
    // admission 0 (00683CE0) itself cancels/removes the queued requests via
    // 00683700(replace=1), exactly as on the Host. Do not wait for the Host's
    // later Release of each old command: that stalled every following order.
    if (ActiveCount() >= kCapacity) return Readiness::Rejected;
    return Resources(actor.character);
}
NativeOutcome NativeRequestAdapter::Invoke(const Record& input) noexcept {
    const Record r=input;
    if (Check(r) != Readiness::Ready) {
        TraceCommand(r,0U,NativeOutcome::Rejected);
        return NativeOutcome::Rejected;
    }
    busy_ = true;
    if(Consumed(r)) {
        // Already executed by the entered interaction: acknowledge, no call.
        if(r.opcode==Op::Submit) {
            auto& key=gConsumedKeys[gConsumedNext++%gConsumedKeys.size()];
            key={r.scope,r.owner,r.commandId,true};gConsumedActions.fetch_add(1U,std::memory_order_relaxed);
        } else if(r.opcode==Op::Release) {
            if(auto* key=FindConsumed(r))key->used=false;
        }
        busy_=false;TraceCommand(r,0U,NativeOutcome::Admitted);return NativeOutcome::Admitted;
    }
    if (Find(r) == nullptr && (r.opcode == Op::Cancel || r.opcode == Op::Stop || r.opcode == Op::Release)) {
        const auto permitted = permission_(context_, r);
        busy_ = false;
        TraceCommand(r,0U,permitted?NativeOutcome::Admitted:NativeOutcome::Rejected);
        return permitted ? NativeOutcome::Admitted : NativeOutcome::Rejected;
    }
    LocalActorBinding actor{};
    if (!Resolve(r, actor)) {
        busy_ = false;
        TraceCommand(r,0U,NativeOutcome::Rejected);
        return NativeOutcome::Rejected;
    }
    NativeOutcome result = NativeOutcome::Rejected;
    if(r.nativeKind==native_intent::kKind) {
        const auto invoke=gIntentInvoke.load(std::memory_order_acquire);
        const character_direct_step_runtime::MovementResourceLease resources(actor.character);
        if(resources&&invoke)result=invoke(r,actor);
        else result=NativeOutcome::Unknown; // Readiness was checked before entry.
        busy_=false;TraceCommand(r,actor.character,result);return result;
    }
    if(ProfileForKind(r.nativeKind)==kProfileLifecycle) {
        // Character-level native calls do not depend on a live locomotion M.
        // In particular a backstage exit must run before movement can resume.
        float before[3]{};
        Read(actor.character+0x70,before[0]);Read(actor.character+0x74,before[1]);Read(actor.character+0x78,before[2]);
        invokingRecord_=&r;result=InvokeLifecycle(r,actor);invokingRecord_=nullptr;
        if(result==NativeOutcome::Admitted)++lifecycleEvents_;
        TraceLifecycle(12U,r,actor.character,before,static_cast<std::uint32_t>(result));
        busy_=false;
        TraceCommand(r,actor.character,result);
        return result;
    }
    {
        const character_direct_step_runtime::MovementResourceLease resources(actor.character);
        invokingRecord_=&r;
        if (resources && permission_(context_, r)) result = InvokePinned(r, actor);
        invokingRecord_=nullptr;
    }
    busy_ = false;
    TraceCommand(r,actor.character,result);
    return result;
}
NativeOutcome NativeRequestAdapter::InvokePinned(const Record& r, const LocalActorBinding& actor) noexcept {
    if(ProfileForKind(r.nativeKind)==kProfileLifecycle)
        return InvokeLifecycle(r,actor);
    if ((r.nativeKind == 1 || r.nativeKind == 2 || r.nativeKind == 3)) return InvokeMovement(r,actor);
    auto* lease = Find(r);
    __try {
        if (r.opcode == Op::Submit) {
            if (lease != nullptr) return NativeOutcome::Rejected;
            for (auto& candidate : leases_) if (!candidate.used) { lease = &candidate; break; }
            if (lease == nullptr) return NativeOutcome::Rejected;
            *lease = {}; lease->used = true; lease->scope = r.scope; lease->owner = r.owner;
            lease->command = r.commandId; lease->actor = actor; lease->kind = r.nativeKind; lease->revision = r.parameterRevision;
            lease->admission=r.admission;
            // Keep this tombstone before allocation: a partial native failure
            // must not make the same Submit look safe to retry.
            lease->unknown = true;
            if(!MovementFactory(*lease,r.nativeKind))return NativeOutcome::Unknown;
            LocalActorBinding afterFactory{};
            if (!Resolve(r, afterFactory) || !Live(*lease, afterFactory) || !WriteParameters(lease->ref.object, r))
                return NativeOutcome::Unknown;
            *reinterpret_cast<std::uint8_t*>(lease->ref.object + 0x18) = static_cast<std::uint8_t>(r.scalarRetention&1);
            bool accepted{};
            if(!MovementAdmit(*lease,r.admission,accepted))return NativeOutcome::Unknown;
            LocalActorBinding after{};
            if (!Resolve(r, after) || !Live(*lease, after)) return NativeOutcome::Unknown;
            *reinterpret_cast<std::uint8_t*>(lease->ref.object+0x18)=static_cast<std::uint8_t>((r.scalarRetention>>1)&1);
            lease->admitted = accepted != 0; lease->unknown = false;
            return accepted != 0 ? NativeOutcome::Admitted : NativeOutcome::Rejected;
        }
        if (lease == nullptr || lease->unknown || !Live(*lease, actor)) return NativeOutcome::Rejected;
        if (r.opcode == Op::Update) {
            std::uint32_t status{};
            // Intermediate Host revisions may be skipped (current intent only).
            if (!lease->admitted || lease->cancelled || lease->revision == UINT32_MAX ||
                r.parameterRevision <= lease->revision || !Read(lease->ref.object + 0xC, status)) return NativeOutcome::Rejected;
            if (status >= 3 && status <= 6) {
                lease->unknown = true;
                if(!ReleaseLeaseRef(*lease))return NativeOutcome::Unknown;
                lease->ref={};lease->admitted=lease->cancelled=false;
                // Recreate the SAME current Host command through its factory;
                // no historical parameters or synthetic destination is used.
                if(!MovementFactory(*lease,r.nativeKind))return NativeOutcome::Unknown;
                LocalActorBinding current{};
                if(!Resolve(r,current)||!Live(*lease,current)||!WriteParameters(lease->ref.object,r))return NativeOutcome::Unknown;
                *reinterpret_cast<std::uint8_t*>(lease->ref.object+0x18)=static_cast<std::uint8_t>(r.scalarRetention&1);
                bool accepted{};
                if(!MovementAdmit(*lease,r.admission,accepted)||!Resolve(r,current)||!Live(*lease,current))return NativeOutcome::Unknown;
                *reinterpret_cast<std::uint8_t*>(lease->ref.object+0x18)=static_cast<std::uint8_t>((r.scalarRetention>>1)&1);
                lease->admitted=accepted;lease->revision=r.parameterRevision;lease->unknown=false;
                return accepted?NativeOutcome::Admitted:NativeOutcome::Rejected;
            }
            if (status != 1 && status != 2) return NativeOutcome::Rejected;
            if (!WriteParameters(lease->ref.object, r)) { lease->unknown = true; return NativeOutcome::Unknown; }
            *reinterpret_cast<std::uint8_t*>(lease->ref.object+0x18)=static_cast<std::uint8_t>((r.scalarRetention>>1)&1);
            lease->revision = r.parameterRevision; return NativeOutcome::Admitted;
        }
        if (r.opcode == Op::Cancel || r.opcode == Op::Stop) {
            if (!lease->cancelled) {
                lease->unknown = true;
                if(!CancelRetiring(lease->ref.object))return NativeOutcome::Unknown;
                LocalActorBinding after{};
                if (!Resolve(r, after) || !Live(*lease, after)) return NativeOutcome::Unknown;
                lease->unknown = false; lease->cancelled = true;
            }
            return NativeOutcome::Admitted;
        }
        if (r.opcode == Op::Release) {
            std::uint32_t status{};
            if (!Read(lease->ref.object + 0xC, status) || status>6)
                return NativeOutcome::Rejected;
            // Even a terminal retained request needs native Cancel to remove
            // scheduler retention before releasing our independent strong Ref.
            lease->unknown = true;
            if (!lease->cancelled)
                if(!CancelRetiring(lease->ref.object))return NativeOutcome::Unknown;
            LocalActorBinding after{};
            if (!Resolve(r, after) || !Live(*lease, after)) return NativeOutcome::Unknown;
            if(!ReleaseLeaseRef(*lease))return NativeOutcome::Unknown;
            *lease = {}; return NativeOutcome::Admitted;
        }
        return NativeOutcome::Rejected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (lease != nullptr) lease->unknown = true;
        return NativeOutcome::Unknown;
    }
}
NativeOutcome NativeRequestAdapter::InvokeLifecycle(const Record& r,const LocalActorBinding& actor) noexcept {
    __try {
        if(r.opcode!=Op::Submit)return NativeOutcome::Rejected;
        if(r.nativeKind==kRelocateKind||r.nativeKind==kForcedRelocateKind||r.nativeKind==kDirectRelocateKind||r.nativeKind==kDirectForcedRelocateKind) {
            // 007C6330 -> 007C2710 -> 00401410 copies FOUR floats. The
            // fourth lane is part of the native position carrier, not yaw.
            alignas(16) const float position[]{r.goalX,r.goalY,r.goalZ,0.0F};
            if(r.nativeKind==kRelocateKind||r.nativeKind==kForcedRelocateKind)
                reinterpret_cast<void(__thiscall*)(void*,const float*,float,std::uint32_t)>(base_+(r.nativeKind==kRelocateKind?0x3C6330U:0x3C63B0U))(
                    reinterpret_cast<void*>(actor.character),position,r.aux,r.mode);
            else
                reinterpret_cast<void(__thiscall*)(void*,const float*,float,std::uint32_t,std::uint32_t)>(base_+0x3C2710U)(
                    reinterpret_cast<void*>(actor.character),position,r.aux,r.mode,r.nativeKind==kDirectForcedRelocateKind?1U:0U);
        } else if(r.nativeKind==kBackstageKind) {
            // Pose observations on this event never synthesize a placement.
            // Explicit relocation has its own independently ordered kind.
            reinterpret_cast<void(__thiscall*)(void*,std::uint8_t)>(base_+0x38B140U)(
                reinterpret_cast<void*>(actor.character),static_cast<std::uint8_t>(r.mode));
        } else {
            std::uintptr_t table{},getter{};
            if(!Read(actor.character,table)||!Read(table+0x1C,getter)||getter<base_+0x1000U||getter>=base_+0xD9E13BU)
                return NativeOutcome::Unknown;
            const auto animated=reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(getter)(reinterpret_cast<void*>(actor.character));
            LocalActorBinding current{};
            if(!animated||!Resolve(r,current)||current.character!=actor.character||current.runtimeId!=actor.runtimeId||
                current.creationSerial!=actor.creationSerial)return NativeOutcome::Unknown;
            reinterpret_cast<void(__thiscall*)(void*)>(base_+0x3C0220U)(reinterpret_cast<void*>(animated));
        }
        LocalActorBinding after{};
        return Resolve(r,after)&&after.character==actor.character&&after.runtimeId==actor.runtimeId&&
            after.creationSerial==actor.creationSerial?NativeOutcome::Admitted:NativeOutcome::Unknown;
    } __except(EXCEPTION_EXECUTE_HANDLER){return NativeOutcome::Unknown;}
}
NativeRequestStatus NativeRequestAdapter::Inspect(const Record& r) noexcept {
    NativeRequestStatus result{}; LocalActorBinding actor{};
    if (busy_ || retiring_ || !ValidateExecutable(r)) return result;
    const BusyScope callbackGuard(busy_);
    if (!Resolve(r, actor)) return result;
    const auto* lease = Find(r);
    if (lease == nullptr) return result;
    result.found = true; result.admitted = lease->admitted;
    result.cancelled = lease->cancelled; result.unknown = lease->unknown;
    if (!lease->unknown && Live(*lease, actor)) static_cast<void>(Read(lease->ref.object + 0xC, result.status));
    return result;
}
} // namespace aimp::ai_sync::execution
