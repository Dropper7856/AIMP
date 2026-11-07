#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/AlienPvPControlPolicy.h"
#include "aimp/AlienPvPDelivery.h"
#include "aimp/AlienPvPSpatialPolicy.h"
#include "aimp/AlienPvPMovementMath.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CrashGuard.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace aimp::alien_pvp {
namespace {
std::mutex gQueueMutex;
Mailbox<64> gIncoming,gOutgoing;
std::atomic<std::uint64_t> gReceived{},gRejected{},gExpired{},gDropped{},gInputs{},gCommits{},gPoses{},gCorrections{},gUnsupported{};
struct State final {
    Context context{};InputWindow window{};PredictionHistory history{};
    DeliveryInbox inbox{};DeliveryOutbox<> outbox{};bool deliveryFault{};std::uint64_t deliveryGeneration{};
    LocalInput local{};std::uint64_t capturedAt{};
    std::array<LocalInput,8> actions{};std::size_t actionRead{},actionCount{};
    std::uint64_t inputSequence{},poseSequence{},lastInputSent{},lastStatus{},lastPose{},lastTick{},lastAckAt{},lastRoar{};
    std::uint64_t appliedInput{},appliedHost{},correctionAt{};
    Frame correction{},lastObserved{};
    bool moving{},hadPose{},ownsSlot{},expired{},recovering{},hasDeferredAction{},aligned{};
    Frame deferredAction{};std::uint64_t deferredActionAt{};
    float correctionX{},correctionZ{};bool predictionCorrection{};
};
State g{}; // certified engine thread only
// A possession can restart on the SAME native Character/scope. A distinct
// delivery generation prevents old IDs/ACKs from aliasing the restarted stream.
// Advertised in Status.sequence; all reliable IDs use generation:counter32.
std::uint64_t gDeliveryGeneration{};

template<typename T> bool Read(std::uintptr_t at,T& value) noexcept {
    return at && crash_guard::TryCopy(&value,reinterpret_cast<const void*>(at),sizeof(T));
}
bool NativeStage(std::uintptr_t character,std::uint8_t& state,bool& backstage) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t a{},b{};std::uint32_t dead{};
    if(!Read(character+0x2F90U,dead) || dead ||
        !Read(character+0x36CU,cycle) || !cycle || !Read(cycle+4U,owner) || owner!=character ||
        !Read(cycle+0x1AU,state) || !Read(character+0x2DAU,a) || !Read(character+0x2DBU,b))return false;
    backstage=spatial::Backstage(a,b);return true;
}
bool OrdinaryCycle(std::uintptr_t character) noexcept {
    std::uintptr_t cycle{},owner{};std::uint8_t state{};std::uint32_t dead{};
    return Read(character+0x2F90U,dead) && dead==0U &&
        Read(character+0x36CU,cycle) && cycle && Read(cycle+4U,owner) && owner==character &&
        Read(cycle+0x1AU,state) && (state==1U || state==55U || state==56U || state==57U || state==59U);
}
bool ReadPose(Frame& f) noexcept {
    if(!Read(g.context.character+0x70U,f.x) || !Read(g.context.character+0x74U,f.y) ||
        !Read(g.context.character+0x78U,f.z) || !Read(g.context.character+0x174U,f.yaw) ||
        !std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z) || !std::isfinite(f.yaw) ||
        std::abs(f.x)>100000.0F || std::abs(f.y)>100000.0F || std::abs(f.z)>100000.0F)return false;
    // Retail yaw is an angle, not necessarily already in the wire interval.
    // Normalize only the copied observation; never rewrite the Character here.
    f.yaw=WrapYaw(f.yaw);
    return true;
}
void Publish(Frame frame,std::uint64_t now) noexcept {
    frame.scope=g.context.scope;frame.sampledAtMs=now;
    if(!Valid(frame)){gRejected.fetch_add(1U,std::memory_order_relaxed);return;}
    if(Reliable(frame)) {
        if(!g.outbox.Push(frame,now)){g.deliveryFault=true;gDropped.fetch_add(1U,std::memory_order_relaxed);}
        return;
    }
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(!lock.owns_lock() || !gOutgoing.Push({frame,now}))gDropped.fetch_add(1U,std::memory_order_relaxed);
}
void FlushReliable(std::uint64_t now) noexcept {
    if(g.outbox.Expired(now))g.deliveryFault=true;
    Frame frame{};if(!g.outbox.Due(now,frame))return;
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(lock.owns_lock())static_cast<void>(gOutgoing.Push({frame,now}));
    // Retained even if the bounded output queue or lock was unavailable.
}
std::uint32_t Flags(std::uint64_t now) noexcept {
    return Bound|(g.context.localOwner?LocalOwner:0U)|(g.context.host?Host:0U)|
        (g.context.pilotReady&&!g.deliveryFault?PilotReady:0U)|(g.local.focused?Focused:0U)|
        (g.moving?Moving:0U)|(g.window.ActiveAt(now)?Active:0U)|(g.expired?Expired:0U);
}
// Source: the donor's ActAlienVoice/GameTick: NPC audio component +11A0,
// thiscall 00784230(type). No behavior tree, random auto-voice, or global hook.
// The caller already holds AIMP's exact-build, engine-thread incarnation proof.
bool Roar() noexcept {
#if defined(_MSC_VER) && !defined(AIMP_ALIEN_PVP_TEST)
    __try {
        reinterpret_cast<void(__thiscall*)(void*,int)>(g.context.base+0x00384230U)(
            reinterpret_cast<void*>(g.context.character+0x11A0U),1);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool ActionBusy() noexcept {
    return g.context.actions && g.context.actions->busy && g.context.actions->busy(g.context);
}
void ReleaseCarrier() noexcept {
    if(g.ownsSlot && character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character))g.ownsSlot=false;
    g.moving=false;
}
void PublishAction(Frame result,std::uint64_t now) noexcept {
    result.scope=g.context.scope;result.flags=Flags(now);
    result.kind=g.context.host && result.action!=Action::Roar?Kind::Resolution:Kind::ActionResult;
    if(result.phase==ActionPhase::Started || result.phase==ActionPhase::Completed)result.flags|=ActionApplied;
    if(result.reason==ActionReason::NativeUnavailable) {
        result.flags|=UnsupportedAction;gUnsupported.fetch_add(1U,std::memory_order_relaxed);
    }
    if(result.kind==Kind::Resolution)g.window.AcknowledgeLocalResolution(result,now);
    Publish(result,now);
}
bool CancelRecovery() noexcept {
    const bool retired=!g.context.actions || !g.context.actions->cancelRecovery ||
        g.context.actions->cancelRecovery(g.context,true);
    g.recovering=!retired;return retired;
}
bool StopDonorMovement() noexcept {
    return !g.context.actions || !g.context.actions->drive || g.context.actions->drive(g.context,nullptr);
}
void ApplyAction(const Frame& command,std::uint64_t now) noexcept {
    if(command.action==Action::None || (command.flags&Focused)==0U)return;
    Frame result=command;result.resolutionRevision=1U;
    if(command.action==Action::Roar) {
        result.phase=ActionPhase::Rejected;result.reason=ActionReason::NativeBusy;
        if((!g.lastRoar || now-g.lastRoar>=2000U) && !ActionBusy() && OrdinaryCycle(g.context.character) && Roar()) {
            result.phase=ActionPhase::Completed;result.reason=ActionReason::None;g.lastRoar=now;
        }
        PublishAction(result,now);return;
    }
    // Non-hosts never resolve targets or portals from their own local view.
    // Attack execution is carried by the existing typed AISync pair stream;
    // vent execution waits for the Host-selected real portal resolution.
    if(!g.context.host)return;
    if(!StopDonorMovement() || !CancelRecovery()) {
        if(!g.hasDeferredAction) {g.deferredAction=command;g.deferredActionAt=now;g.hasDeferredAction=true;}
        else if(g.deferredAction.hostSequence!=command.hostSequence) {
            result.phase=ActionPhase::Rejected;result.reason=ActionReason::NativeBusy;PublishAction(result,now);
        }
        return;
    }
    if(!ActionBusy())ReleaseCarrier();
    if(!g.context.actions || !g.context.actions->begin || !g.context.actions->begin(g.context,command,result)) {
        result=command;result.resolutionRevision=1U;result.phase=ActionPhase::Rejected;
        result.reason=ActionReason::NativeUnavailable;
    }
    PublishAction(result,now);
}
void ApplyResolution(const Frame& plan,std::uint64_t now) noexcept {
    if(g.context.host)return;
    if((!StopDonorMovement() || !CancelRecovery()) && (plan.phase==ActionPhase::Preparing || plan.phase==ActionPhase::Started)) {
        if(!g.hasDeferredAction || g.deferredAction.hostSequence==plan.hostSequence) {
            if(!g.hasDeferredAction)g.deferredActionAt=now;
            g.deferredAction=plan;g.hasDeferredAction=true;
        }
        return;
    }
    if(g.hasDeferredAction && g.deferredAction.hostSequence==plan.hostSequence)g.hasDeferredAction=false;
    if(plan.phase==ActionPhase::Started && !ActionBusy())ReleaseCarrier();
    Frame receipt=plan;
    if(g.context.actions && g.context.actions->begin && g.context.actions->begin(g.context,plan,receipt))
        PublishAction(receipt,now);
}
void PollAction(std::uint64_t now) noexcept {
    if(g.hasDeferredAction) {
        if(!g.window.ActiveAt(now) || !Fresh(now,g.deferredActionAt,1500U)) {
            Frame failed=g.deferredAction;failed.phase=ActionPhase::Rejected;failed.reason=ActionReason::NativeBusy;
            failed.resolutionRevision=std::max(std::uint64_t{1},failed.resolutionRevision+1U);
            g.hasDeferredAction=false;PublishAction(failed,now);
        } else if(StopDonorMovement() && CancelRecovery()) {
            const auto pending=g.deferredAction;g.hasDeferredAction=false;
            if(pending.kind==Kind::Resolution)ApplyResolution(pending,now);else ApplyAction(pending,now);
        }
    }
    if(!g.context.actions || !g.context.actions->poll)return;
    if(g.window.Granted() && !g.window.ActiveAt(now) && g.context.actions->cancel)
        g.context.actions->cancel(g.context,true);
    Frame result{};
    if(g.context.actions->poll(g.context,result))PublishAction(result,now);
}
void Drain(std::uint64_t now) noexcept {
    std::array<TimedFrame,32> pending{};std::size_t count{};
    {
        std::unique_lock lock(gQueueMutex,std::try_to_lock);
        if(!lock.owns_lock())return;
        while(count<pending.size() && gIncoming.Pop(pending[count]))++count;
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& value=pending[i];const auto previousEpoch=g.window.Epoch();
        const auto& f=value.frame;
        if(f.scope!=g.context.scope){gRejected.fetch_add(1U,std::memory_order_relaxed);continue;}
        if(f.kind==Kind::Acknowledge){static_cast<void>(g.outbox.Acknowledge(f));continue;}
        if(f.deliveryId) {
            const auto verdict=g.inbox.Inspect(f.deliveryId);
            if(verdict==DeliveryInbox::Verdict::Gap)continue;
            if(verdict==DeliveryInbox::Verdict::Duplicate){Publish(Acknowledgement(f),now);continue;}
            // Complete delivery even if gameplay validation rejects a stale
            // epoch. Acknowledging receipt must never certify native execution.
            static_cast<void>(g.inbox.Commit(f.deliveryId));
            Publish(Acknowledgement(f),now);
        }
        if(f.deliveryId && f.sampledAtMs && f.kind!=Kind::Stop &&
            !Fresh(now,f.sampledAtMs,f.kind==Kind::Commit?500U:kDeliveryDeadlineMs)) {
            g.deliveryFault=true;gRejected.fetch_add(1U,std::memory_order_relaxed);continue;
        }
        if(!g.window.Accept(value.frame,value.at,now)) {
            gRejected.fetch_add(1U,std::memory_order_relaxed);continue;
        }
        if(f.kind==Kind::Stop){g.deliveryFault=false;g.hasDeferredAction=false;}
        if(f.kind==Kind::Stop || (f.kind==Kind::Activate && previousEpoch!=f.epoch)) {
            if(g.context.actions && g.context.actions->cancel)g.context.actions->cancel(g.context,true);
        }
        if(f.kind==Kind::Activate && previousEpoch!=f.epoch) {
            g.inputSequence=g.poseSequence=g.appliedInput=g.appliedHost=0U;
            g.history.Clear();g.lastInputSent=0U;g.lastAckAt=now;g.predictionCorrection=false;
            g.correction={};g.correctionAt=0U;g.lastObserved={};g.hadPose=false;g.lastPose=0U;
            g.actionRead=g.actionCount=0U;g.local.action=Action::None;g.hasDeferredAction=false;
        } else if(f.kind==Kind::Commit) {
            gCommits.fetch_add(1U,std::memory_order_relaxed);g.lastAckAt=value.at;
            ApplyAction(f,now);
        } else if(f.kind==Kind::Resolution) {
            ApplyResolution(f,now);
        } else if(f.kind==Kind::Correction && !g.context.host) {
            if(!g.aligned && (f.flags&(PoseValid|Backstage|NativeBusy))==PoseValid &&
                !ActionBusy() && OrdinaryCycle(g.context.character)) {
                // AIMP fences ordinary spawn/lifecycle replay for the possessed
                // Alien. Apply the Host's first authenticated placement once,
                // before predicting any movement from a different local spawn.
                const float position[]{f.x,f.y,f.z};
                g.aligned=character_direct_step_runtime::ApplyAIRecoveryTransform(g.context.character,position,f.yaw);
                if(g.aligned){g.history.Clear();g.hadPose=false;g.predictionCorrection=false;}
            }
            g.correction=f;g.correctionAt=value.at;g.predictionCorrection=false;
            if(g.context.localOwner)
                g.predictionCorrection=g.history.Error(f.acknowledgedInput,f.x,f.y,f.z,g.correctionX,g.correctionZ);
        }
    }
}
void StopMovement() noexcept {
    if(!g.ownsSlot){g.moving=false;return;}
    if(OrdinaryCycle(g.context.character)) {
        std::string error;
        static_cast<void>(character_direct_step_runtime::HoldAIIntentIdle(g.context.character,1U,error));
    } else {
        // A cutscene, vent or paired capture owns its own native lifetime.
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character));
        g.ownsSlot=false;
    }
    g.moving=false;
}
void ApplyMovement(std::uint64_t now,float dt) noexcept {
    if(!g.context.host && !g.aligned){static_cast<void>(StopDonorMovement());ReleaseCarrier();return;}
    const bool active=g.window.ActiveAt(now) && !g.deliveryFault;
    const bool fresh=g.context.localOwner ?
        active && g.context.pilotReady && Fresh(now,g.capturedAt,150U) &&
            Fresh(now,g.lastAckAt,kAuthorityTimeoutMs) : g.window.InputFresh(now);
    if(ActionBusy() || g.hasDeferredAction) {static_cast<void>(StopDonorMovement());static_cast<void>(CancelRecovery());ReleaseCarrier();return;}
    std::uint8_t cycle{};bool backstage{};
    const bool stageKnown=NativeStage(g.context.character,cycle,backstage);
    if(fresh && stageKnown && spatial::ManualBackstage(cycle,backstage,false)) {
        if(!StopDonorMovement()){ReleaseCarrier();return;}
        if(!CancelRecovery()){ReleaseCarrier();return;}
        ReleaseCarrier();
        const auto& committed=g.window.Input();
        const LocalInput input=g.context.localOwner?g.local:LocalInput{committed.forward,committed.strafe,
            committed.yaw,committed.pitch,committed.gait,Action::None,(committed.flags&Focused)!=0U};
        Frame body{};if(!ReadPose(body))return;
        float dx{},dz{};
        const bool moved=input.focused && spatial::BackstageStep(input.forward,input.strafe,input.yaw,input.gait,dt,dx,dz);
        bool corrected{};float cx{},cz{};
        if(!g.context.host && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&(PoseValid|Backstage))==(PoseValid|Backstage) &&
            (g.correction.flags&NativeBusy)==0U && std::abs(g.correction.y-body.y)<=1.0F) {
            if(g.context.localOwner && g.predictionCorrection) {
                corrected=spatial::BoundedStep(g.correctionX,g.correctionZ,4.0F*dt,cx,cz);
                g.correctionX-=cx;g.correctionZ-=cz;
            } else if(!g.context.localOwner)
                corrected=spatial::BoundedStep(g.correction.x-body.x,g.correction.z-body.z,8.0F*dt,cx,cz);
        }
        const float pos[]{body.x+dx+cx,body.y,body.z+dz+cz};
        if((moved || corrected) && character_direct_step_runtime::ApplyAIRecoveryTransform(
            g.context.character,pos,ApproachYaw(body.yaw,input.yaw,dt))) {
            g.moving=moved || corrected;g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
            if(corrected)gCorrections.fetch_add(1U,std::memory_order_relaxed);
        }
        g.expired=false;return;
    }
    if(!fresh || !stageKnown || backstage || !OrdinaryCycle(g.context.character)) {
        static_cast<void>(StopDonorMovement());
        const bool expired=g.window.Granted() && !fresh;
        if(expired&&!g.expired)gExpired.fetch_add(1U,std::memory_order_relaxed);
        g.expired=expired;static_cast<void>(CancelRecovery());StopMovement();return;
    }
    g.expired=false;
    const auto& committed=g.window.Input();
    LocalInput input=g.context.localOwner?g.local:LocalInput{committed.forward,committed.strafe,
        committed.yaw,committed.pitch,committed.gait,Action::None,(committed.flags&Focused)!=0U};
    if(!input.focused){input.forward=input.strafe=0.0F;}
    Frame body{};
    if(!ReadPose(body)){StopMovement();return;}
    if(g.context.actions && g.context.actions->recover) {
        const bool ready=!g.context.host && input.focused && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&PoseValid)!=0U && (g.correction.flags&(NativeBusy|Backstage))==0U;
        const bool substantial=ready && (std::hypot(g.correction.x-body.x,g.correction.z-body.z)>4.0F ||
            std::abs(g.correction.y-body.y)>1.0F);
        if(substantial || g.recovering) {
            if(!StopDonorMovement()){ReleaseCarrier();return;}
            ReleaseCarrier();
            g.recovering=g.context.actions->recover(g.context,ready?&g.correction:nullptr);
            if(g.recovering){g.moving=true;return;}
        }
    }
    alien_pvp_movement::Plan plan{};
    const bool moving=alien_pvp_movement::TryPlan(input.forward,input.strafe,input.yaw,input.gait,plan);
    if(g.context.actions && g.context.actions->drive) {
        ReleaseCarrier();
        Frame intent{};intent.forward=input.forward;intent.strafe=input.strafe;
        intent.yaw=input.yaw;intent.gait=input.gait;
        // Feed bounded observer correction through the same native direction
        // request. The local owner keeps its immediate control and animation.
        if(!g.context.host && !g.context.localOwner && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
            (g.correction.flags&(PoseValid|NativeBusy|Backstage))==PoseValid) {
            const float dx=g.correction.x-body.x,dz=g.correction.z-body.z;
            if(std::hypot(dx,dz)>0.25F) {
                const float vx=(moving?plan.worldX:0.0F)+dx*0.5F;
                const float vz=(moving?plan.worldZ:0.0F)+dz*0.5F;
                intent.yaw=std::atan2(vx,vz);intent.forward=1.0F;intent.strafe=0.0F;
            }
        }
        g.moving=g.context.actions->drive(g.context,input.focused?&intent:nullptr) &&
            (intent.forward!=0.0F || intent.strafe!=0.0F) && input.focused;
        if(g.moving){g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();}
        return;
    }
    std::string error;bool applied{};
    // Progressive correction uses the already-existing no-snap native carrier,
    // exclusively in PvP. No Coop follower policy or transform writer changes.
    bool correction=false;float targetX=body.x,targetZ=body.z;
    if(!g.context.host && g.correctionAt && Fresh(now,g.correctionAt,250U) &&
        (g.correction.flags&PoseValid)!=0U && (g.correction.flags&(NativeBusy|Backstage))==0U && input.focused) {
        if(g.context.localOwner && g.predictionCorrection) {
            float dx{},dz{};
            correction=spatial::BoundedStep(g.correctionX,g.correctionZ,0.5F,dx,dz);
            targetX+=dx;targetZ+=dz;
            // Consume only displacement observed since the previous sample,
            // not a requested correction that the locomotor may ignore.
            if(g.predictionCorrection && g.hadPose) {
                const float advancedX=body.x-g.lastObserved.x,advancedZ=body.z-g.lastObserved.z;
                const float d=std::hypot(g.correctionX,g.correctionZ);
                if(d>0.05F) {
                    const float progress=std::clamp((advancedX*g.correctionX+advancedZ*g.correctionZ)/d,0.0F,d);
                    g.correctionX-=g.correctionX/d*progress;g.correctionZ-=g.correctionZ/d*progress;
                }
            }
        } else if(!g.context.localOwner && std::abs(g.correction.y-body.y)<=1.0F) {
            const auto age=std::min(0.1F,static_cast<float>(now-g.correctionAt)/1000.0F);
            float dx{},dz{};
            correction=spatial::BoundedStep(g.correction.x+g.correction.velocityX*age-body.x,
                g.correction.z+g.correction.velocityZ*age-body.z,0.5F,dx,dz);
            targetX+=dx;targetZ+=dz;
        }
    }
    if(correction) {
        // Raw local intent remains the decision source on the owner. On an
        // observer the Host's measured physical velocity drives presentation.
        const float vx=g.context.localOwner?(moving?plan.worldX*plan.intensity:0.0F):g.correction.velocityX;
        const float vz=g.context.localOwner?(moving?plan.worldZ*plan.intensity:0.0F):g.correction.velocityZ;
        applied=character_direct_step_runtime::ArmAIHybridReplication(g.context.character,vx,vz,
            moving?plan.intensity:0.0F,targetX,body.y,targetZ,input.yaw,input.gait,error,now+kInputTimeoutMs);
        if(applied)gCorrections.fetch_add(1U,std::memory_order_relaxed);
    } else {
        applied=moving?character_direct_step_runtime::ArmAIIntentReplication(g.context.character,
            plan.worldX,plan.worldZ,plan.intensity,plan.gait,error):
            character_direct_step_runtime::HoldAIIntentIdle(g.context.character,input.gait,error);
        if(applied)
            static_cast<void>(character_direct_step_runtime::ApplyReplicatedYaw(g.context.character,
                ApproachYaw(body.yaw,input.yaw,dt),error));
    }
    if(applied) {
        if(!g.context.host){g.lastObserved=body;g.hadPose=true;}
        g.ownsSlot=true;g.moving=moving;
        g.appliedInput=g.window.LastInput();g.appliedHost=g.window.LastHost();
    } else {StopMovement();}
}
void PublishInput(std::uint64_t now) noexcept {
    if(!g.context.localOwner || !g.context.pilotReady || g.deliveryFault || !g.window.Granted() || !g.window.Epoch())return;
    // Do not let a newer steering sequence overtake an unacknowledged click.
    if(g.outbox.Has(Kind::Input))return;
    if(g.lastInputSent && now-g.lastInputSent<50U && !g.actionCount)return;
    if(g.inputSequence==UINT64_MAX)return;
    Frame frame{};frame.kind=Kind::Input;frame.epoch=g.window.Epoch();frame.sequence=++g.inputSequence;
    frame.flags=Flags(now);frame.gait=g.local.gait;frame.forward=g.local.forward;frame.strafe=g.local.strafe;
    frame.yaw=g.local.yaw;frame.pitch=g.local.pitch;
    if(!Fresh(now,g.capturedAt,150U) || !g.local.focused) {
        frame.forward=frame.strafe=0.0F;frame.flags&=~Focused;
        g.actionRead=g.actionCount=0U;
    }
    LocalInput queued{};
    if(g.actionCount) {
        queued=g.actions[g.actionRead];frame.action=queued.action;
        g.actionRead=(g.actionRead+1U)%g.actions.size();--g.actionCount;
    }
    Frame body{};
    if(ReadPose(body)) {
        frame.x=body.x;frame.y=body.y;frame.z=body.z;frame.flags|=PoseValid;
        g.history.Record(frame.sequence,body.x,body.y,body.z);
    }
    if(queued.selectedVent && queued.action==Action::ExitVent) {
        frame.flags|=VentSelected;frame.x=queued.ventX;frame.y=queued.ventY;frame.z=queued.ventZ;
    }
    Publish(frame,now);gInputs.fetch_add(1U,std::memory_order_relaxed);g.lastInputSent=now;
}
} // namespace

void CaptureLocal(const LocalInput& input) noexcept {
    g.local=input;g.capturedAt=GetTickCount64();
    if(input.action!=Action::None && g.window.Granted()) {
        if(g.actionCount<g.actions.size()) {
            g.actions[(g.actionRead+g.actionCount)%g.actions.size()]=input;++g.actionCount;
        } else gDropped.fetch_add(1U,std::memory_order_relaxed);
    }
    g.local.action=Action::None;
}
void Reset(bool certifiedLivingCharacter) noexcept {
    if(g.context.actions && g.context.actions->cancel)g.context.actions->cancel(g.context,certifiedLivingCharacter);
    if(certifiedLivingCharacter && g.context.character && g.ownsSlot)
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(g.context.character));
    g={};
    // The engine thread never blocks behind the IPC worker. Stamped old-scope
    // frames which cannot be removed here will fail the next identity check.
    std::unique_lock lock(gQueueMutex,std::try_to_lock);
    if(lock.owns_lock()){gIncoming.Clear();gOutgoing.Clear();}
}
bool Owns(std::uintptr_t character,std::uint32_t generation) noexcept {
    return character && g.context.owned && g.context.character==character &&
        g.context.scope.nativeGeneration==generation;
}
bool MovingNow() noexcept { return g.moving; }
void Pump(const Context& context) noexcept {
    if(!context.owned || !context.base || !context.character || !Valid(context.scope)) {
        if(g.context.character)Reset(false);
        return;
    }
    if(g.context.character!=context.character || g.context.scope!=context.scope) {
        Reset(false);
        if(gDeliveryGeneration==UINT32_MAX)return;
        g.context=context;g.window.Reset(context.scope);
        g.deliveryGeneration=++gDeliveryGeneration;
        g.inbox.Clear(g.deliveryGeneration<<32U);g.outbox.Clear(g.deliveryGeneration<<32U);
    }
    g.context=context;
    const auto now=GetTickCount64();
    const auto dt=g.lastTick && now>=g.lastTick?
        std::clamp(static_cast<float>(now-g.lastTick)/1000.0F,0.001F,0.05F):0.016F;
    g.lastTick=now;
    Drain(now);PollAction(now);PublishInput(now);FlushReliable(now);ApplyMovement(now,dt);
    if(!g.lastStatus || now-g.lastStatus>=250U) {
        Frame status{};status.kind=Kind::Status;status.sequence=g.deliveryGeneration;status.epoch=g.window.Epoch();status.flags=Flags(now);
        std::uint8_t state{};bool backstage{};
        const bool known=NativeStage(context.character,state,backstage);
        if(known && backstage)status.flags|=Backstage;
        if(ActionBusy() || !known || (!OrdinaryCycle(context.character) && !spatial::ManualBackstage(state,backstage,false)))status.flags|=NativeBusy;
        if(ReadPose(status))status.flags|=PoseValid;
        Publish(status,now);g.lastStatus=now;
    }
    if(context.host && g.window.Granted() && g.window.Epoch() &&
        (!g.lastPose || now-g.lastPose>=50U) && g.poseSequence!=UINT64_MAX) {
        Frame pose{};pose.kind=Kind::Pose;pose.epoch=g.window.Epoch();pose.sequence=++g.poseSequence;
        pose.flags=Flags(now);pose.hostSequence=g.appliedHost;pose.acknowledgedInput=g.appliedInput;
        pose.gait=g.window.Input().gait;
        std::uint8_t state{};bool backstage{};
        const bool known=NativeStage(context.character,state,backstage);
        if(known && backstage)pose.flags|=Backstage;
        if(ActionBusy() || !known || (!OrdinaryCycle(context.character) && !spatial::ManualBackstage(state,backstage,false)))pose.flags|=NativeBusy;
        if(ReadPose(pose)) {
            pose.flags|=PoseValid;
            if(g.hadPose && g.lastPose && now>g.lastPose && now-g.lastPose<=200U) {
                const auto elapsed=static_cast<float>(now-g.lastPose)/1000.0F;
                pose.velocityX=(pose.x-g.lastObserved.x)/elapsed;pose.velocityZ=(pose.z-g.lastObserved.z)/elapsed;
                if(std::hypot(pose.velocityX,pose.velocityZ)>30.0F)pose.velocityX=pose.velocityZ=0.0F;
            }
            g.hadPose=true;g.lastObserved=pose;Publish(pose,now);gPoses.fetch_add(1U,std::memory_order_relaxed);
        } else g.hadPose=false;
        g.lastPose=now;
    }
}
bool QueueCommand(std::span<const std::byte> bytes) noexcept {
    Frame frame{};
    if(!Decode({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},frame) ||
        (frame.kind!=Kind::Activate && frame.kind!=Kind::Commit && frame.kind!=Kind::Correction && frame.kind!=Kind::Stop && frame.kind!=Kind::Resolution && frame.kind!=Kind::Acknowledge)) {
        gRejected.fetch_add(1U,std::memory_order_relaxed);return false;
    }
    const auto now=GetTickCount64();std::lock_guard lock(gQueueMutex);
    if(!gIncoming.Push({frame,now})){gDropped.fetch_add(1U,std::memory_order_relaxed);return false;}
    gReceived.fetch_add(1U,std::memory_order_relaxed);return true;
}
bool TryPopObservation(std::array<std::uint8_t,kWireBytes>& bytes) noexcept {
    std::lock_guard lock(gQueueMutex);TimedFrame value{};
    while(gOutgoing.Pop(value))if(Encode(value.frame,bytes))return true;
    return false;
}
Counters Snapshot() noexcept {
    return {gReceived.load(),gRejected.load(),gExpired.load(),gDropped.load(),gInputs.load(),gCommits.load(),
        gPoses.load(),gCorrections.load(),gUnsupported.load()};
}
} // namespace aimp::alien_pvp
