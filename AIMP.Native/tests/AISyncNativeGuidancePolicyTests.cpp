#include "aimp/AISyncNativeGuidancePolicy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace aimp::ai_sync::movement::native_guidance;
namespace {
void Check(bool ok,const char* message) {
    if(!ok){std::cerr<<"Native guidance policy: "<<message<<'\n';std::exit(1);}
}
Snapshot Following() {
    Snapshot state{};state.status=2;state.phase=3;state.execution=2;
    state.goal={1,0,1};state.workerQuiet=true;state.routeUpdateQuiet=true;return state;
}
// These are the externally verified 006C7550/0067D0D0/00465E20/0044D320
// ordering rules, deliberately independent of the adapter implementation.
struct Retail {
    Snapshot state{Following()};float stop{-1.0F};bool tail{true};
    void Goal(const std::array<float,3>& target) {
        if(state.phase!=3||(state.flags90&0x40U)||(state.flags91&3U)==3U)return;
        state.pendingGoal=target;state.flags90|=0x40U;
    }
    void Stop(float distance) {
        if(stop!=distance){stop=distance;state.flags91|=0x40U;}
    }
    void Tick() {
        if(state.phase!=3)return;
        if(state.flags91&0x40U) {
            state.flags91&=static_cast<std::uint8_t>(~0x40U);state.phase=1;
        }
        if(state.phase==3&&(state.flags90&0x40U)) {
            state.flags90&=static_cast<std::uint8_t>(~0x40U);
            const auto dx=state.pendingGoal[0]-state.goal[0],dz=state.pendingGoal[2]-state.goal[2];
            if(tail&&dx*dx+dz*dz>=0.1F)state.goal=state.pendingGoal;
        }
    }
};
void NativeGoalMustBeConsumedBeforeStopChanges() {
    const std::array<float,3> target{3,0,1};
    Retail old;old.Goal(target);old.Stop(0.15F);old.Tick();
    Check(old.state.phase==1&&!SameGoal(old.state.goal,target),"dirty stop replans the old goal first");
    Check(ObserveGoal(old.state,target)==GoalResult::Pending,"old-goal replan is not target consumption");
    Retail current;current.Goal(target);
    Check((current.state.flags90&0x40U)&&SameGoal(current.state.pendingGoal,target),"goal deposition is visible");
    Check(ObserveGoal(current.state,target)==GoalResult::Pending,"deposition remains pending");
    current.Tick();
    Check(ObserveGoal(current.state,target)==GoalResult::Consumed,"actual current XYZ proves consumption");
    current.Stop(0.15F);current.Tick();
    Check(current.state.phase==1&&SameGoal(current.state.goal,target),"stop replan uses the consumed target");
}
void PendingClearIsNotSuccess() {
    Retail small;const std::array<float,3> nearby{1.1F,0,1};small.Goal(nearby);small.Tick();
    Check(!(small.state.flags90&0x40U)&&ObserveGoal(small.state,nearby)==GoalResult::Discarded,"native ignores a short edit");
    Check(!CanConsumeGoal(small.state.goal,nearby),"short changes are coalesced before a deposit");
    Retail vertical;const std::array<float,3> upstairs{1,4,1};vertical.Goal(upstairs);vertical.Tick();
    Check(ObserveGoal(vertical.state,upstairs)==GoalResult::Discarded,"vertical-only edit is not falsely acknowledged");
    Check(!CanConsumeGoal(vertical.state.goal,upstairs),"native threshold is horizontal");
    Check(!CoalescibleGoal(vertical.state.goal,upstairs),"another floor must not receive a coalesced stop");
    Check(CoalescibleGoal(small.state.goal,nearby),"a small three-dimensional edit may coalesce");
    Retail missingTail;missingTail.tail=false;const std::array<float,3> target{5,0,1};
    missingTail.Goal(target);missingTail.Tick();
    Check(ObserveGoal(missingTail.state,target)==GoalResult::Discarded,"missing tail can discard a large edit");
    auto planning=Following();planning.goal=target;planning.phase=1;
    Check(ObserveGoal(planning,target)==GoalResult::Waiting,"copied XYZ still waits for usable following phase");
    auto ready=Following();ready.goal=target;
    Check(ObserveGoal(ready,target)==GoalResult::Consumed,"finished native route accepts the deposited XYZ");
}
void SetterGatesArePreserved() {
    Retail retained;retained.state.flags91=2;
    Check(Editable(retained.state),"local arrival without authored stop remains editable");
    retained.state.flags91|=1U;retained.Goal({4,0,1});
    Check(!(retained.state.flags90&0x40U),"copying authored stop first makes retail reject a goal");
    Check(RetainedArrival(retained.state)&&Renewable(retained.state),"retained stop has a distinct safe renewal path");
    auto worker=Following();worker.workerQuiet=false;
    Check(!Editable(worker)&&!Renewable(worker),"worker state cannot be fabricated as quiescence");
    auto pending=Following();pending.flags90=0x40U;
    Check(!Editable(pending),"an unconsumed edit cannot be overwritten");
    auto dirty=Following();dirty.flags91=0x40U;
    Check(!Editable(dirty),"wait for native stop replan before another goal");
}
void ExhaustedThroughWaypointNeedsProvenNativeRenewal() {
    auto live=Following();live.flags91=2U;
    Check(Editable(live)&&!Renewable(live),"arrival bit with live or unknown tail still uses native edits");
    auto exhausted=live;exhausted.tailExhausted=true;
    Check(ExhaustedThroughArrival(exhausted)&&Renewable(exhausted)&&!Editable(exhausted),
        "proven empty tail plus local arrival uses renewal, not discarded edits");
    Check(!RetainedArrival(exhausted),"through arrival is distinct from authored stop handshake");
    Check(ObserveGoal(exhausted,{4,0,1})==GoalResult::Waiting,"empty tail is not acknowledged as consumption");
    auto blocked=exhausted;blocked.routeUpdateQuiet=false;
    Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"scheduled native route update must finish before tail renewal");
    blocked=exhausted;blocked.workerQuiet=false;
    Check(!Renewable(blocked)&&!Editable(blocked),"empty tail cannot bypass a live worker");
    blocked=exhausted;blocked.foreignBlock=true;
    Check(!Renewable(blocked)&&!Editable(blocked),"empty tail cannot replace another command's navigation");
    blocked=exhausted;blocked.flags90=0x40U;
    Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"pending setter must first finish natively");
    blocked=exhausted;blocked.flags91=0x42U;
    Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"dirty stop retains native replanning ownership");
    blocked=exhausted;blocked.flags91=0U;
    Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"empty tail alone does not invent arrival");
    blocked=exhausted;blocked.status=1U;
    Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"queued request is not treated as locally complete");
    for(const auto phase:{1U,4U,8U}) {
        blocked=exhausted;blocked.phase=phase;
        Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"planner and scheduler phases retain ownership");
    }
    for(const auto execution:{0U,1U,3U}) {
        blocked=exhausted;blocked.execution=execution;
        Check(!ExhaustedThroughArrival(blocked)&&!Renewable(blocked),"unknown/failure/completion execution needs its own handoff");
    }
}
void NativeSchedulerOwnsFailureAndCompletion() {
    for(const auto phase:{4U,8U}) {
        auto state=Following();state.phase=phase;state.execution=phase==8?3U:1U;
        Check(AwaitingScheduler(state)&&!Renewable(state),"phase outcome must first reach native scheduler");
        state.status=phase==8?3U:4U;
        Check(!AwaitingScheduler(state)&&Renewable(state),"retained terminal status can renew after handoff");
        state.workerQuiet=false;
        Check(!Renewable(state),"terminal status alone cannot release an active worker");
        state.workerQuiet=true;state.retainsBlock=true;
        Check(!Renewable(state),"terminal request waits for native block detachment");
        state.retainsBlock=false;state.foreignBlock=true;
        Check(!Renewable(state),"foreign block preserves another native controller");
    }
}
void RetainedReplacementKeepsCleanupIdentity() {
    auto state=Following();state.status=5;
    Check(!Renewable(state),"replacement status5 cannot abandon a retained native reference");
    state.retainedReferenceAbsent=true;
    Check(Renewable(state),"certified retained-list absence permits status5 renewal");
}
void NoPermanentContinuationCeiling() {
    std::uint64_t now=1000,last=0;std::uint32_t attempts=0;
    for(unsigned i=0;i<20;++i) {
        Check(RetryReady(now,last,attempts),"an authorized inert command retains a future retry");
        last=now;++attempts;
        Check(!RetryReady(now,last,attempts),"no retry flood within one frame");
        const auto delay=RetryDelay(attempts);
        Check(delay>=500&&delay<=4000,"backoff is finite and rate bounded");
        Check(!RetryReady(now+delay-1,last,attempts),"cooldown must fully elapse");now+=delay;
    }
    Check(RetryDelay(std::numeric_limits<std::uint32_t>::max())==4000,"attempt counter saturation remains resumable");
    Check(!RetryReady(999,1000,1),"clock reversal cannot reopen a retry");
}
void InitialSubmitNeedsUsableDistance() {
    const std::array<float,3> body{0,0,0};
    Check(!MeaningfulTarget(body,body,-1,-1),"synchronized startup does not construct a zero-length Move");
    Check(!MeaningfulTarget(body,{0.1F,0,0},-1,-1),"tiny carrier progress remains a retained wait");
    Check(MeaningfulTarget(body,{2,0,0},-1,-1),"a usable Host-authored lookahead can start");
    Check(!MeaningfulTarget(body,{1,0,0},2,2),"target inside native stop distance is not meaningful");
    Check(!MeaningfulTarget(body,{std::numeric_limits<float>::quiet_NaN(),0,0},-1,-1),"nonfinite goal never reaches a native factory");
}
}
int main() {
    NativeGoalMustBeConsumedBeforeStopChanges();PendingClearIsNotSuccess();SetterGatesArePreserved();
    ExhaustedThroughWaypointNeedsProvenNativeRenewal();
    NativeSchedulerOwnsFailureAndCompletion();RetainedReplacementKeepsCleanupIdentity();
    NoPermanentContinuationCeiling();InitialSubmitNeedsUsableDistance();
    std::cout<<"AISync native guidance ordering, consumption and renewal policy PASS\n";
}
