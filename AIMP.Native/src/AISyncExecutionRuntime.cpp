#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/AISyncProfiling.h"
#include "aimp/AISyncExecutionSession.h"
#include "aimp/AISyncRouteRuntime.h"
#include "aimp/AISyncRouteExecution.h"
#include "aimp/AISyncCausalState.h"
#include "aimp/CharacterIncarnation.h"
#include <memory>
#include <mutex>
#include <algorithm>

namespace aimp::ai_sync::execution_runtime {
namespace {
execution::ExecutionMailbox gMailbox;
std::unique_ptr<execution::ExecutionSession> gSession;
std::unique_ptr<execution::OwnershipCoordinator> gOwnership;
std::atomic<bool> gAvailable{};
std::atomic<bool> gRetireRequested{};
struct NavigationRejection { execution::NavigationBinding binding{}; bool used{}; };
std::array<NavigationRejection,execution::NativeRequestAdapter::kCapacity> gNavigationRejections{};
bool gRejectingNavigation{};
QualifyScope gQualify{};
QualifyScope gRetirementQualify{};
execution::OwnershipAdapter gOwnershipHooks{};
ReservationQuery gReserved{};
ContextReady gContextReady{};
PlacementReady gPlacementReady{};
bool gCommandAdmission{}; // Bootstrap policy, not a wire flag.
transactions::Scope gScope{}; // Engine thread only.
std::uint32_t gNativeGeneration{}; // Engine thread only.
std::size_t gRetirementOwnerCursor{};
std::atomic<std::uint64_t> gRetirementStatus{};
std::atomic<std::uint64_t> gHistoryFloor{},gCheckpoints{},gRetainedRecords{};
std::array<std::atomic<std::uint64_t>,13> gWaits{};
std::mutex gDestroyedMutex;
std::array<execution::ActorLifetime,character_incarnations::Registry::kCapacity> gDestroyed{};
std::size_t gDestroyedCount{};
std::atomic<bool> gDestructionCoverageLost{};
void PublishWaits() noexcept {
    const auto w=gSession->Waits();
    const std::uint64_t values[]{w.waiting,w.oldestKind,w.oldestOpcode,w.oldestAdmission,w.oldestReason,w.oldestSeq,w.oldestAgeMs,
        w.maxKind|(static_cast<std::uint64_t>(w.maxOpcode)<<32),w.maxReason,w.maxSeq,w.maxAgeMs,w.transitionPlacements,w.lifecycleEvents};
    for(std::size_t i=0;i<gWaits.size();++i)gWaits[i].store(values[i],std::memory_order_relaxed);
}
void PublishRetirementStatus() noexcept {
    const auto progress = gSession->RetirementState();
    std::uint64_t fenced = 0;
    for (std::size_t i = 0; i < gOwnership->Count(); ++i) {
        execution::OwnershipToken token{}; execution::OwnershipPhase phase{};
        if (gOwnership->Read(i,token,phase) && phase == execution::OwnershipPhase::Fenced) ++fenced;
    }
    const auto packed = static_cast<std::uint64_t>(progress.remaining) |
        (static_cast<std::uint64_t>(progress.unknown) << 8) | (fenced << 16) |
        (static_cast<std::uint64_t>(gSession->Retired()) << 24) |
        (static_cast<std::uint64_t>(gSession->RetirementComplete()) << 25) |
        (static_cast<std::uint64_t>(ContextRetirementComplete()) << 26);
    gRetirementStatus.store(packed,std::memory_order_release);
}
bool Permit(void*, const transactions::Record& record) noexcept {
    if (!gQualify(record.scope, gNativeGeneration) || !gMailbox.Current(record.scope, gNativeGeneration)) return false;
    execution::OwnershipToken token{}; execution::OwnershipPhase phase{};
    if (!gOwnership->Find(record.owner, token, phase)) return true; // Unowned Submit stays Waiting.
    if (phase == execution::OwnershipPhase::Fenced || phase == execution::OwnershipPhase::Restoring) return false;
    if (phase == execution::OwnershipPhase::Retiring || phase == execution::OwnershipPhase::Released)
        return record.opcode == transactions::Op::Cancel || record.opcode == transactions::Op::Stop ||
            record.opcode == transactions::Op::Complete || record.opcode == transactions::Op::Release;
    return true;
}
bool Resolve(void*, const transactions::OwnerKey& owner, execution::LocalActorBinding& actor) noexcept {
    execution::ActorLifetime local{};
    if (!gOwnership->ResolveOwned(gScope, owner, local, true)) return false;
    actor = {local.character, local.runtimeId, true,local.creationSerial}; return true;
}
bool ResolveLifecycle(void*,const transactions::OwnerKey& owner,execution::LocalActorBinding& actor) noexcept {
    execution::ActorLifetime lifetime{};
    // A native Character event is not a scheduler takeover. Requiring the
    // locomotion drain here would deadlock the very backstage/placement event
    // that makes that drain possible. The embedding resolver still proves the
    // current Host identity, local creation serial, generation and Client role.
    if(!gQualify(gScope,gNativeGeneration)||!gMailbox.Bound(gScope,gNativeGeneration)||!gOwnershipHooks.resolve||
        gOwnershipHooks.resolve(gOwnershipHooks.context,gScope,owner,lifetime)!=execution::OwnershipCheck::Ready)return false;
    // Host events follow its spawn baseline in the stream. Applying one before
    // the Client confirmed that baseline let the baseline snap undo it.
    if(gPlacementReady&&!gPlacementReady(lifetime.character))return false;
    actor={lifetime.character,lifetime.runtimeId,true,lifetime.creationSerial};return true;
}
execution::Readiness ReservedDependency(void*,const transactions::Record& record) noexcept {
    if(gReserved&&gReserved(record.scope,record.owner,record.globalSeq)&&record.opcode!=transactions::Op::Cancel&&
        record.opcode!=transactions::Op::Stop&&record.opcode!=transactions::Op::Complete&&record.opcode!=transactions::Op::Release)
        return execution::Readiness::Waiting;
    return execution::Readiness::Ready;
}
execution::OwnershipCheck ResolveLocal(void*, const transactions::Scope& scope, const transactions::OwnerKey& owner,
    execution::ActorLifetime& actor) noexcept {
    const auto qualify = gSession->Retired() ? gRetirementQualify : gQualify;
    if (!qualify || !qualify(scope,gNativeGeneration)) return execution::OwnershipCheck::Waiting;
    return gOwnershipHooks.resolve ? gOwnershipHooks.resolve(gOwnershipHooks.context, scope, owner, actor) : execution::OwnershipCheck::Reject;
}
execution::OwnershipCheck DrainLocal(void*, const execution::OwnershipToken& token) noexcept {
    return gOwnershipHooks.drain ? gOwnershipHooks.drain(gOwnershipHooks.context, token) : execution::OwnershipCheck::Unknown;
}
execution::OwnershipCheck PrepareLocal(void*, const transactions::Scope& scope, const transactions::OwnerKey& owner,
    const execution::ActorLifetime& actor) noexcept {
    return gOwnershipHooks.prepare ? gOwnershipHooks.prepare(gOwnershipHooks.context,scope,owner,actor) : execution::OwnershipCheck::Ready;
}
execution::OwnershipCheck NativeReleased(void*, const execution::OwnershipToken& token) noexcept {
    if (!gSession->OwnerReleased(token.owner)) return execution::OwnershipCheck::Waiting;
    return gOwnershipHooks.nativeReleased ? gOwnershipHooks.nativeReleased(gOwnershipHooks.context, token) : execution::OwnershipCheck::Unknown;
}
execution::OwnershipCheck RestoreLocal(void*, const execution::OwnershipToken& token) noexcept {
    return gOwnershipHooks.restore ? gOwnershipHooks.restore(gOwnershipHooks.context, token) : execution::OwnershipCheck::Unknown;
}
execution::OwnershipCheck DestroyedReleased(void*,const execution::OwnershipToken& token) noexcept {
    // The owner cannot be restored after its original destructor returned.
    // Independent native request and route pins must nevertheless be retired.
    if(gDestructionCoverageLost)return execution::OwnershipCheck::Unknown;
    return gSession->OwnerReleased(token.owner)&&route_runtime::RetirementComplete(token.scope)?
        execution::OwnershipCheck::Ready:execution::OwnershipCheck::Waiting;
}
void DrainDestroyed() noexcept {
    decltype(gDestroyed) facts{};
    std::size_t count{};
    {const std::lock_guard lock(gDestroyedMutex);count=gDestroyedCount;
        std::copy_n(gDestroyed.begin(),count,facts.begin());gDestroyedCount=0;}
    for(std::size_t i=0;i<count;++i) {
        if(facts[i].nativeGeneration!=gNativeGeneration)continue;
        gSession->ConfirmActorDestroyed({facts[i].character,facts[i].runtimeId,true,facts[i].creationSerial});
        gOwnership->ConfirmDestroyed(facts[i]);route_execution::ConfirmActorDestroyed(facts[i]);
    }
    gOwnership->ReleaseDestroyed();
}
bool MutableOwnership(bool admission) noexcept {
    if (!gAvailable.load(std::memory_order_acquire)) return false;
    const auto qualify = !admission && gSession->Retired() ? gRetirementQualify : gQualify;
    return qualify && qualify(gScope, gNativeGeneration) &&
        !gSession->OperationInProgress() && !gOwnership->InTransition() && (!admission ||
            (!gRetireRequested.load(std::memory_order_acquire) && gMailbox.Bound(gScope, gNativeGeneration)));
}
transactions::Result Bind(void*, const transactions::Scope& scope, std::uint32_t generation) noexcept {
    if (!gQualify(scope, generation)) return transactions::Result::WrongScope;
    // World revocation happens before the legacy context-retirement barrier.
    // Doing it here was too late: that barrier prevented this Bind entirely.
    if(gContextReady&&!gContextReady(scope))return transactions::Result::OwnerBusy;
    if (((scope != gScope || generation != gNativeGeneration) && !route_runtime::RetirementComplete(gScope)) ||
        gOwnership->CanConfigure(scope, generation) != execution::OwnershipResult::Ready) return transactions::Result::OwnerBusy;
    // The preceding release barriers qualify this transition. Consume an
    // inherited OFF/old-scope request BEFORE configuring the fresh scope.
    // In particular PumpRetirements cannot consume startup OFF while the
    // native generation is still zero. Leaving that flag set retires the
    // newly Ready session on its first idle frame, before command sequence1.
    // A later concurrent Retire still sets the flag and invalidates the
    // mailbox revision; its Ready cannot escape that revision fence.
    const bool inheritedRetirement=gRetireRequested.exchange(false,std::memory_order_acq_rel);
    const auto result = gSession->Configure(scope,gSession->Retired() ? transactions::ExecutionBarrier::ExternalQuiescence :
        transactions::ExecutionBarrier::RequireReleased);
    if(result!=transactions::Result::Ok&&inheritedRetirement)gRetireRequested.store(true,std::memory_order_release);
    if (result == transactions::Result::Ok) {
        if (gOwnership->Configure(scope, generation) != execution::OwnershipResult::Ready) return transactions::Result::OwnerBusy;
        BeginExecutionCarrierScope(scope);
        gNativeGeneration = generation; gScope = scope; gRetirementOwnerCursor = 0; PublishRetirementStatus();
        gHistoryFloor.store(0);gCheckpoints.store(0);gRetainedRecords.store(gSession->RetainedRecords());
    }
    return result;
}
void AdmitWaitingOwner(const transactions::Record& record) noexcept {
    if (!gCommandAdmission || execution::ProfileForKind(record.nativeKind)==execution::kProfileLifecycle || record.nativeKind==native_intent::kKind || record.opcode != transactions::Op::Submit || !gSession->IsWaiting(record) ||
        !MutableOwnership(true)) return;
    execution::OwnershipToken token{}; execution::OwnershipPhase phase{};
    if (!gOwnership->Find(record.owner,token,phase)) static_cast<void>(RequestOwnership(record.owner));
    else if (phase == execution::OwnershipPhase::Draining) static_cast<void>(RetryOwnership(record.owner));
}
bool Execute(void*, std::span<const std::uint8_t> bytes, const transactions::Scope& scope,
    std::span<execution::Receipt> receipts, std::size_t& count) noexcept {
    if (!gMailbox.Current(scope, gNativeGeneration) || !gQualify(scope, gNativeGeneration)) return false;
    execution::Batch batch{};
    if (!execution::DecodeBatch(bytes,scope,batch)) return false;
    std::array<bool,execution::kMaxRecords> wasWaiting{};
    for (std::size_t i = 0; i < batch.count; ++i) wasWaiting[i] = gSession->IsWaiting(batch.records[i]);
    const auto result = gSession->Handle(bytes, scope, true, receipts, count);
    // Only a logically accepted, retained Submit may request the local drain.
    // Duplicates never progress a waiting drain; only an exact Retry does.
    // The callback still proves the actual local lifetime and quiescence.
    for (std::size_t i = 0; i < count; ++i)
        if (!wasWaiting[i] && receipts[i].stage == execution::Stage::Waiting) AdmitWaitingOwner(batch.records[i]);
    gRetainedRecords.store(gSession->RetainedRecords(),std::memory_order_relaxed);
    PublishRetirementStatus(); return result;
}
bool Retry(void*, const transactions::Record& record, std::uint64_t batch,
    const transactions::Scope& scope, execution::Receipt& receipt) noexcept {
    if (!gMailbox.Current(scope, gNativeGeneration) || !gQualify(scope, gNativeGeneration)) return false;
    AdmitWaitingOwner(record);
    const auto result = gSession->Retry(record, batch, scope, true, receipt);
    PublishRetirementStatus(); return result;
}
transactions::Result Checkpoint(void*,const transactions::Scope& scope,std::uint64_t through) noexcept {
    if(!gMailbox.Current(scope,gNativeGeneration)||!gQualify(scope,gNativeGeneration))return transactions::Result::WrongScope;
    const auto result=gSession->Checkpoint(scope,through);
    if(result==transactions::Result::Ok) {
        if(gHistoryFloor.exchange(through,std::memory_order_relaxed)!=through)gCheckpoints.fetch_add(1,std::memory_order_relaxed);
        gRetainedRecords.store(gSession->RetainedRecords(),std::memory_order_relaxed);
    }
    return result;
}
bool Route(void*,std::span<const std::uint8_t> bytes,const transactions::Scope& current) noexcept {
    if(!gMailbox.Current(current,gNativeGeneration)||!gQualify(current,gNativeGeneration)||!route_runtime::QueueClient(bytes))return false;
    route_runtime::Pump(); // Install value points before the next causal work item.
    return true;
}
bool Carrier(void*,std::span<const std::uint8_t> bytes,const transactions::Scope& current,std::uint64_t arrivalTick) noexcept {
    transactions::Scope scope{};std::uint64_t serial{},through{};State state{};
    if(!gMailbox.Current(current,gNativeGeneration)||!gQualify(current,gNativeGeneration)||
        !causal_state::Decode(bytes,scope,serial,through,state)||scope!=current||through!=gSession->AcceptedThrough())return false;
    return ApplyExecutionCarrier(state,through,arrivalTick);
}
execution::Readiness PermitRetirement(void*, const execution::RetirementIdentity& identity) noexcept {
    if (!gSession->Retired() || identity.scope != gScope || !gRetirementQualify ||
        !gRetirementQualify(identity.scope,gNativeGeneration)) return execution::Readiness::Waiting;
    execution::ActorLifetime actor{};
    const auto result = gOwnership->ResolveBinding(identity.scope,identity.owner,actor,true);
    if (result == execution::OwnershipResult::Waiting) return execution::Readiness::Waiting;
    if (result != execution::OwnershipResult::Ready) return execution::Readiness::Rejected;
    return actor.character == identity.actor.character && actor.runtimeId == identity.actor.runtimeId &&
          actor.creationSerial==identity.actor.creationSerial&&
        actor.nativeGeneration == gNativeGeneration ? execution::Readiness::Ready : execution::Readiness::Rejected;
}
}
bool Initialize(std::uintptr_t base, bool exact, QualifyScope qualify, execution::OwnershipAdapter ownership,
    QualifyScope retirementQualify, bool commandAdmission, ReservationQuery reserved, ContextReady contextReady,
    PlacementReady placementReady) noexcept {
    if (gSession != nullptr || qualify == nullptr) return false;
    // Partial local adapters must never begin a drain that cannot be retired.
    const bool anyHook = ownership.resolve || ownership.drain || ownership.nativeReleased || ownership.restore || ownership.prepare;
    if (commandAdmission && !anyHook) return false;
    if (anyHook && (!ownership.resolve || !ownership.drain || !ownership.nativeReleased || !ownership.restore || !retirementQualify)) return false;
    try {
        gSession = std::make_unique<execution::ExecutionSession>();
        gOwnership = std::make_unique<execution::OwnershipCoordinator>(execution::OwnershipAdapter{
            nullptr, &ResolveLocal, &DrainLocal, &NativeReleased, &RestoreLocal, &PrepareLocal,&DestroyedReleased});
    } catch (...) { gSession.reset(); gOwnership.reset(); return false; }
    if (!gSession->Initialize(base, exact, &Permit, &Resolve, nullptr,&ReservedDependency,&ResolveLifecycle)) { gSession.reset(); gOwnership.reset(); return false; }
    gQualify = qualify; gRetirementQualify = retirementQualify; gOwnershipHooks = ownership;
    gCommandAdmission = commandAdmission;
    gReserved=reserved;gContextReady=contextReady;gPlacementReady=placementReady;
    gAvailable.store(true, std::memory_order_release); return true;
}
bool QueueBinding(std::span<const std::uint8_t> bytes, std::uint32_t generation) noexcept {
    if (!gAvailable.load(std::memory_order_acquire)) return false;
    execution::Control control{};
    if(!execution::DecodeControl(bytes,control))return false;
    if(control.kind==execution::ControlKind::Offer&&(control.value&8U)!=0&&!route_runtime::Available())return false;
    return gMailbox.QueueBinding(control, generation);
}
bool QueueBatch(std::span<const std::uint8_t> bytes, bool retry) noexcept {
    return gAvailable.load(std::memory_order_acquire) && gMailbox.QueueBatch(bytes, retry);
}
bool QueueRoute(std::span<const std::uint8_t> bytes) noexcept {
    return gAvailable.load(std::memory_order_acquire)&&gMailbox.QueueRoute(bytes);
}
bool QueueCarrier(std::span<const std::uint8_t> bytes) noexcept {
    return gAvailable.load(std::memory_order_acquire)&&gMailbox.QueueCarrier(bytes);
}
execution::OwnershipResult RequestOwnership(const transactions::OwnerKey& input) noexcept {
    const auto owner = input;
    if (!MutableOwnership(true)) return execution::OwnershipResult::Busy;
    const auto fenced = gSession->FenceOwner(owner);
    if (fenced != transactions::Result::Ok)
        return fenced == transactions::Result::StaleOwner ? execution::OwnershipResult::StaleOwner : execution::OwnershipResult::Busy;
    const auto result = gOwnership->Request(owner);
    if (!MutableOwnership(true)) { static_cast<void>(gOwnership->Retire(owner)); return execution::OwnershipResult::Busy; }
    return result;
}
execution::OwnershipResult RetryOwnership(const transactions::OwnerKey& owner) noexcept {
    if (!MutableOwnership(true)) return execution::OwnershipResult::Busy;
    const auto result = gOwnership->Retry(owner);
    if (!MutableOwnership(true)) { static_cast<void>(gOwnership->Retire(owner)); return execution::OwnershipResult::Busy; }
    return result;
}
execution::OwnershipResult RetireOwnership(const transactions::OwnerKey& owner) noexcept {
    return MutableOwnership(false) ? gOwnership->Retire(owner) : execution::OwnershipResult::Busy;
}
execution::OwnershipResult ReleaseOwnership(const transactions::OwnerKey& owner) noexcept {
    return MutableOwnership(false) ? gOwnership->Release(owner) : execution::OwnershipResult::Busy;
}
bool AllowsLegacy(const execution::ActorLifetime& actor) noexcept {
    // Initialize publishes the coordinator after hooks can already be present.
    // Never race its unique_ptr construction/reset before that publication.
    return !gAvailable.load(std::memory_order_acquire) || gOwnership->AllowsLegacy(actor); // Engine thread only.
}
void InvalidateActor(const execution::ActorLifetime& actor) noexcept {
    // Engine thread, including destruction callbacks. Close permission and
    // notify the Host without waiting for its next command or a timeout.
    if (gAvailable.load(std::memory_order_acquire) && gOwnership->InvalidateActor(actor)) {
        static_cast<void>(gMailbox.Fence(execution::Recovery::NativeUnavailable));
        gRetireRequested.store(true, std::memory_order_release);
        PublishRetirementStatus();
    }
}
void BeforeActorDestruction(const execution::ActorLifetime& actor) noexcept {
    if(!gAvailable.load(std::memory_order_acquire)||actor.nativeGeneration!=gNativeGeneration||
        !gRetirementQualify||!gRetirementQualify(gScope,gNativeGeneration)||
        gSession->OperationInProgress()||gOwnership->InTransition())return;
    bool owned{};
    for(std::size_t i=0;i<gOwnership->Count();++i) {
        execution::OwnershipToken token{};execution::OwnershipPhase phase{};
        if(gOwnership->Read(i,token,phase)&&token.actor==actor&&phase!=execution::OwnershipPhase::Released)owned=true;
    }
    if(!owned)return;
    static_cast<void>(gMailbox.Fence(gScope,execution::Recovery::NativeUnavailable));
    gRetireRequested.store(true,std::memory_order_release);
    if(!gSession->BeginRetirement())return;
    gOwnership->RetireAll();
    static_cast<void>(gSession->DrainActorRetirement({actor.character,actor.runtimeId,true,actor.creationSerial},&PermitRetirement,nullptr));
    // ForgetCharacter then revokes this lifetime. Native Ref release alone
    // does not reopen legacy writers or assert that a route/graph was freed.
    PublishRetirementStatus();
}
void Retire() noexcept {
    gMailbox.Retire(); gRetireRequested.store(true, std::memory_order_release);
}
bool RevokePreviousWorld(const std::uint32_t current) noexcept {
    if (current == 0U) return false;
    if (!gAvailable.load(std::memory_order_acquire) || gNativeGeneration == 0U ||
        current == gNativeGeneration) return true;
    if (current < gNativeGeneration || gSession->OperationInProgress() || gOwnership->InTransition()) return false;
    gMailbox.RetireGeneration(gNativeGeneration);
    gRetireRequested.store(true, std::memory_order_release);
    gOwnership->ForgetGeneration(gNativeGeneration);
    // No session/route cleanup success is inferred here. In particular the
    // existing reference-retirement barriers are still required before Bind.
    return true;
}
void AfterActorDestruction(const execution::ActorLifetime& actor) noexcept {
    if(!actor.character||!actor.runtimeId||!actor.nativeGeneration||!actor.creationSerial)return;
    const std::lock_guard lock(gDestroyedMutex);
    for(std::size_t i=0;i<gDestroyedCount;++i)if(gDestroyed[i]==actor)return;
    if(gDestroyedCount==gDestroyed.size()){gDestructionCoverageLost=true;return;}
    gDestroyed[gDestroyedCount++]=actor;
}
bool Available() noexcept { return gAvailable.load(std::memory_order_acquire); }
execution::MailboxFailure LastMailboxFailure() noexcept {return gMailbox.LastFailure();}
bool CurrentScope(transactions::Scope& output) noexcept {
    if(!Available()||!gQualify||!gQualify(gScope,gNativeGeneration)||gSession->Retired()||!gMailbox.Bound(gScope,gNativeGeneration))return false;
    output=gScope;return true;
}
bool IsOwnedPlacement(std::uintptr_t scheduler) noexcept {
    transactions::Scope scope{};execution::RetirementIdentity identity{};
    return CurrentScope(scope)&&!gRetireRequested.load(std::memory_order_acquire)&&
        gSession->IsOwnedPlacement(scheduler,&identity)&&identity.scope==scope;
}
std::size_t ActorRequests(std::uintptr_t c) noexcept {return Available()?gSession->ActorRequests(c):0U;}
bool OrdinaryMotionAvailable(const execution::ActorLifetime& actor) noexcept {
    transactions::Scope scope{};
    if(!CurrentScope(scope)||gRetireRequested.load(std::memory_order_acquire)||
        gSession->OperationInProgress()||gOwnership->InTransition()||
        actor.nativeGeneration!=gNativeGeneration||!actor.creationSerial||
        gSession->ActorRequests(actor.character)!=0U)return false;
    for(std::size_t i=0;i<gOwnership->Count();++i) {
        execution::OwnershipToken token{};execution::OwnershipPhase phase{};
        if(!gOwnership->Read(i,token,phase))return false;
        if(token.actor.character!=actor.character)continue;
        if(token.actor!=actor||token.scope!=scope)return false;
        if(phase!=execution::OwnershipPhase::Owned&&phase!=execution::OwnershipPhase::Released)return false;
    }
    return true;
}
bool RecoveryMotionAvailable(const execution::ActorLifetime& actor) noexcept {
    transactions::Scope scope{};
    if(!CurrentScope(scope)||gRetireRequested.load(std::memory_order_acquire)||
        gSession->OperationInProgress()||gOwnership->InTransition()||
        actor.nativeGeneration!=gNativeGeneration||!actor.creationSerial)return false;
    bool granted{};
    for(std::size_t i=0;i<gOwnership->Count();++i) {
        execution::OwnershipToken token{};execution::OwnershipPhase phase{};
        if(!gOwnership->Read(i,token,phase))return false;
        if(token.actor.character!=actor.character)continue;
        if(token.actor!=actor||token.scope!=scope||phase!=execution::OwnershipPhase::Owned)return false;
        granted=true;
    }
    return granted;
}
action_correlation::Readiness ActionReadiness(const action_correlation::Reference& reference,
    const transactions::OwnerKey& owner,const execution::ActorLifetime& actor,std::size_t* blockingRequests,bool admittedHistory) noexcept {
    using Ready=action_correlation::Readiness;
    transactions::Scope scope{};
    if(!CurrentScope(scope)||gSession->OperationInProgress()||gOwnership->InTransition())return Ready::Waiting;
    const auto logical=gSession->ActionReadiness(reference,owner,admittedHistory);
    if(logical!=Ready::Ready)return logical;
    execution::OwnershipToken token{};execution::OwnershipPhase phase{};
    // A retained Submit can be waiting for its first local ownership grant.
    // Absence is a pending bootstrap, not evidence that its owner was retired.
    if(!gOwnership->Find(owner,token,phase)) {
        for(std::size_t i=0;i<gOwnership->Count();++i)if(gOwnership->Read(i,token,phase)&&
            token.owner.entityA==owner.entityA&&token.owner.entityB==owner.entityB&&token.owner.capability==owner.capability)
            return Ready::Superseded;
        return Ready::Waiting;
    }
    execution::ActorLifetime local{};
    const auto owned=gOwnership->ResolveBinding(scope,owner,local,false);
    if(owned==execution::OwnershipResult::StaleOwner||owned==execution::OwnershipResult::WrongScope)return Ready::Superseded;
    if(owned==execution::OwnershipResult::Unknown)return Ready::Rejected;
    if(owned!=execution::OwnershipResult::Ready)return Ready::Waiting;
    if(local.character!=actor.character||local.runtimeId!=actor.runtimeId||local.nativeGeneration!=actor.nativeGeneration||
        !actor.creationSerial||local.creationSerial!=actor.creationSerial)return Ready::Rejected;
    // Qualification may run embedding callbacks; revalidate the binding before
    // granting this one start. This neither enters nor retries the kind5 order.
    transactions::Scope after{};
    if(!CurrentScope(after)||after!=scope)return Ready::Waiting;
    if(blockingRequests)*blockingRequests=gSession->ActionBlockingRequests(reference,owner,actor.character);
    return Ready::Ready;
}
execution::NavigationOwnership IdentifyNavigation(std::uintptr_t n,std::uintptr_t b,execution::NavigationBinding& output) noexcept {
    if (!gAvailable.load(std::memory_order_acquire)) return execution::NavigationOwnership::Unowned;
    const bool current=gQualify&&gQualify(gScope,gNativeGeneration)&&gMailbox.Matches(gScope);
    return gSession->IdentifyNavigation(n,b,output,current);
}
execution::MarkerInitialization InitializeMarker(std::uintptr_t request,std::uintptr_t character) noexcept {
    if(!gAvailable.load(std::memory_order_acquire))return execution::MarkerInitialization::Unowned;
    const bool current=gQualify&&gQualify(gScope,gNativeGeneration)&&gMailbox.Matches(gScope)&&
        !gRetireRequested.load(std::memory_order_acquire);
    return gSession->InitializeMarker(request,character,current);
}
bool Matches(const transactions::Scope& scope) noexcept {
    return gAvailable.load(std::memory_order_acquire) && gMailbox.Matches(scope);
}
void Fail(const transactions::Scope& scope,execution::Recovery reason) noexcept {
    if (gAvailable.load(std::memory_order_acquire) && gMailbox.Fence(scope,reason))
        gRetireRequested.store(true,std::memory_order_release);
}
bool RejectNavigation(const execution::NavigationBinding& binding) noexcept {
    if(!gAvailable.load(std::memory_order_acquire)||!gQualify||
        !gQualify(binding.command.scope,gNativeGeneration)||binding.command.scope!=gScope||
        !gMailbox.Bound(gScope,gNativeGeneration)||binding.policy!=movement::Navigation::HostRoute)return false;
    NavigationRejection* empty{};
    for(auto& item:gNavigationRejections) {
        if(item.used&&item.binding.command.scope!=gScope)item={};
        if(item.used&&item.binding.command==binding.command)return true;
        if(!item.used&&!empty)empty=&item;
    }
    if(!empty)return false;
    *empty={binding,true};return true;
}
void DrainNavigationRejections() noexcept {
    if(gRejectingNavigation||gSession->OperationInProgress()||gOwnership->InTransition())return;
    gRejectingNavigation=true;
    for(auto& item:gNavigationRejections)if(item.used) {
        if(item.binding.command.scope!=gScope){item={};continue;}
        if(!gQualify||!gQualify(gScope,gNativeGeneration)||!gMailbox.Bound(gScope,gNativeGeneration))continue;
        // Remove before entering native callbacks. A nested failure may queue
        // its own copied identity; it cannot mutate this borrowed call token.
        const auto binding=item.binding;item={};
        static_cast<void>(gSession->RejectNavigation(binding));
    }
    gRejectingNavigation=false;
}
void Pump(bool includeRetirements) noexcept {
    const profiling::Timer timing(profiling::Section::CommandPump);
    if (gAvailable.load(std::memory_order_acquire)) {
        if (gSession->OperationInProgress() || gOwnership->InTransition()) return;
        if (includeRetirements) PumpRetirements();
        DrainNavigationRejections();
        gMailbox.Pump({nullptr, &Bind, &Execute, &Retry, &Checkpoint,&Carrier,&Route});
        DrainNavigationRejections();
        if (!gSession->OperationInProgress() && !gOwnership->InTransition()) { gSession->Steer(); PublishWaits(); }
    }
}
void PumpRetirements() noexcept {
    if (!gAvailable.load(std::memory_order_acquire) || gNativeGeneration == 0 ||
        gSession->OperationInProgress() || gOwnership->InTransition()) return;
    // These are return tokens, not native cleanup. Drain even after the old
    // world qualifier has closed; never dereference its dead Characters.
    DrainDestroyed();
    if (!gRetireRequested.load(std::memory_order_acquire) && gMailbox.BindingActive() && !gSession->Retired()) return;
    if (!gSession->BeginRetirement()) return;
    gRetireRequested.store(false,std::memory_order_release); gOwnership->RetireAll();
    // Destruction-confirmed leases need no live world/Character access. Their
    // independent Ref can drain even after the old qualification has closed;
    // PermitRetirement still gates all remaining live native operations.
    static_cast<void>(gSession->DrainRetirement(&PermitRetirement,nullptr));
    gOwnership->ReleaseDestroyed();
    if (!gRetirementQualify || !gRetirementQualify(gScope,gNativeGeneration)) { PublishRetirementStatus(); return; }
    for (std::size_t scan = 0, attempts = 0; scan < gOwnership->Count() && attempts < 4; ++scan) {
        if (!gRetirementQualify(gScope,gNativeGeneration)) { PublishRetirementStatus(); return; }
        if (gRetirementOwnerCursor >= gOwnership->Count()) gRetirementOwnerCursor = 0;
        execution::OwnershipToken token{}; execution::OwnershipPhase phase{};
        if (!gOwnership->Read(gRetirementOwnerCursor++,token,phase)) return;
        if (phase != execution::OwnershipPhase::Retiring && phase != execution::OwnershipPhase::Restoring) continue;
        ++attempts; static_cast<void>(gOwnership->Release(token.owner));
    }
    PublishRetirementStatus();
}
std::atomic<std::uint32_t> gContextBlocker{};
std::uint32_t ContextRetirementBlocker() noexcept {return gContextBlocker.load(std::memory_order_relaxed);}
bool ContextRetirementComplete() noexcept {
    if (!gAvailable.load(std::memory_order_acquire) || gNativeGeneration == 0) {gContextBlocker=0;return true;}
    const auto blocked=[](std::uint32_t code) noexcept {gContextBlocker.store(code,std::memory_order_relaxed);return false;};
    if (gSession->OperationInProgress()) return blocked(1U);
    if (gOwnership->InTransition()) return blocked(2U);
    if (!gSession->RetirementComplete()) return blocked(3U);
    if (!route_runtime::RetirementComplete(gScope)) return blocked(4U);
    for (std::size_t i = 0; i < gOwnership->Count(); ++i) {
        execution::OwnershipToken token{}; execution::OwnershipPhase phase{};
        if (!gOwnership->Read(i,token,phase) || phase != execution::OwnershipPhase::Released) return blocked(5U);
    }
    gContextBlocker=0;return true;
}
RetirementDiagnostics RetirementSnapshot() noexcept {
    const auto packed = gRetirementStatus.load(std::memory_order_acquire);
    return {static_cast<std::uint32_t>(packed & 255U),static_cast<std::uint32_t>((packed >> 8) & 255U),
        static_cast<std::uint32_t>((packed >> 16) & 255U),(packed & (1ULL << 24)) != 0,
        (packed & (1ULL << 25)) != 0,(packed & (1ULL << 26)) != 0};
}
HistoryDiagnostics HistorySnapshot() noexcept {
    return {gHistoryFloor.load(std::memory_order_relaxed),gCheckpoints.load(std::memory_order_relaxed),
        gRetainedRecords.load(std::memory_order_relaxed)};
}
bool Peek(execution::MailboxPacket& packet) noexcept { return gMailbox.Peek(packet); }
execution::WaitReport WaitSnapshot() noexcept {
    std::uint64_t v[13]{};for(std::size_t i=0;i<gWaits.size();++i)v[i]=gWaits[i].load(std::memory_order_relaxed);
    execution::WaitReport w{};
    w.waiting=static_cast<std::uint32_t>(v[0]);w.oldestKind=static_cast<std::uint32_t>(v[1]);w.oldestOpcode=static_cast<std::uint32_t>(v[2]);
    w.oldestAdmission=static_cast<std::uint32_t>(v[3]);w.oldestReason=static_cast<std::uint32_t>(v[4]);w.oldestSeq=v[5];w.oldestAgeMs=v[6];
    w.maxKind=static_cast<std::uint32_t>(v[7]);w.maxOpcode=static_cast<std::uint32_t>(v[7]>>32);w.maxReason=static_cast<std::uint32_t>(v[8]);
    w.maxSeq=v[9];w.maxAgeMs=v[10];w.transitionPlacements=v[11];w.lifecycleEvents=v[12];return w;
}
bool Commit(std::uint64_t token) noexcept { return gMailbox.Commit(token); }
} // namespace aimp::ai_sync::execution_runtime
