#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/AlienPvPVents.h"
#include <Windows.h>
#include "aimp/AlienPvPFreeCamera.h"
#include "aimp/AlienPvPSpatialPolicy.h"
#include "aimp/AlienPvPResponsiveStop.h"
#include <array>
#include <cstring>
#include <iostream>
#include <cstdlib>

namespace {
int genericCalls{},driveCalls{},stopCalls{},beginCalls{},placements{},recoveryCalls{};
int starts{},completions{},replicas{};
bool stopReady=true,nativeBusy=false,proxyVent=false;
aimp::alien_pvp::Frame lastIntent{};
void Check(bool pass,const char* message) {
    if(!pass){std::cerr<<message<<'\n';std::exit(1);}
}
bool Drive(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame* frame) noexcept {
    if(!frame){++stopCalls;return stopReady;}
    ++driveCalls;lastIntent=*frame;return true;
}
bool Begin(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame& frame,aimp::alien_pvp::Frame& out) noexcept {
    if(frame.phase==aimp::alien_pvp::ActionPhase::Completed){++completions;nativeBusy=false;out=frame;return true;}
    ++starts;
    ++beginCalls;out=frame;out.phase=aimp::alien_pvp::ActionPhase::Started;
    out.reason=aimp::alien_pvp::ActionReason::None;out.resolutionRevision=1U;
    nativeBusy=!proxyVent;return true;
}
bool Replicate(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame&) noexcept {++replicas;return true;}
bool Busy(const aimp::alien_pvp::Context&) noexcept {return nativeBusy;}
bool Recover(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame*) noexcept {++recoveryCalls;return true;}
void Cancel(const aimp::alien_pvp::Context& c,bool) noexcept {static_cast<void>(Drive(c,nullptr));nativeBusy=false;}
void Queue(aimp::alien_pvp::Frame frame) {
    std::array<std::uint8_t,aimp::alien_pvp::kWireBytes> bytes{};
    Check(aimp::alien_pvp::Encode(frame,bytes),"frame encoding failed");
    Check(aimp::alien_pvp::QueueCommand(std::as_bytes(std::span(bytes))),"command rejected");
}
}
// These stubs detect any accidental return to the generic NPC carrier. Retail
// native execution is tested separately in game, not simulated by these tests.
namespace aimp::character_direct_step_runtime {
bool ArmAIIntentReplication(std::uintptr_t,float,float,float,std::uint32_t,std::string&) noexcept {++genericCalls;return false;}
bool HoldAIIntentIdle(std::uintptr_t,std::uint32_t,std::string&) noexcept {++genericCalls;return false;}
bool ArmAIHybridReplication(std::uintptr_t,float,float,float,float,float,float,float,std::uint32_t,std::string&,std::uint64_t) noexcept {++genericCalls;return false;}
bool ReleaseAIIntentOwnership(std::uintptr_t) noexcept {return true;}
bool ApplyAIRecoveryTransform(std::uintptr_t character,const float* position,float yaw) noexcept {
    ++placements;std::memcpy(reinterpret_cast<void*>(character+0x70U),position,sizeof(float)*3U);
    std::memcpy(reinterpret_cast<void*>(character+0x174U),&yaw,sizeof(yaw));return true;
}
bool ApplyReplicatedYaw(std::uintptr_t,float,std::string&) noexcept {++genericCalls;return false;}
}
int main() {
    using namespace aimp::alien_pvp;
    ResponsiveStop responsiveStop;std::array<float,2> stoppedAt{};
    responsiveStop.Begin(1000,10,20);
    Check(responsiveStop.Target(1010,59,true,stoppedAt) && stoppedAt==std::array<float,2>{10,20},"stop hold lost release position");
    responsiveStop.Cancel();
    Check(!responsiveStop.Target(1020,55,true,stoppedAt),"new input remained pinned to a stopped position");
    responsiveStop.Begin(1000,10,20);
    Check(!responsiveStop.Target(3001,59,true,stoppedAt),"stop hold never expires");
    responsiveStop.Begin(1000,10,20);
    Check(!responsiveStop.Target(1100,18,false,stoppedAt) && !responsiveStop.Target(1200,1,true,stoppedAt),"stop hold survived a native passage");
    responsiveStop.Begin(1000,10,20);
    Check(responsiveStop.Target(1100,1,true,stoppedAt) && !responsiveStop.Target(1251,1,true,stoppedAt),"stop hold persisted after idle convergence");
    Check(spatial::VentStageReady(75,true,true),"stationary backstage75 never completes entry");
    Check(spatial::VentStageReady(77,false,false,true),"replica retained cycle77 blocks completed exit");
    Check(!spatial::VentStageReady(18,false,false,true),"active passage completes replica exit");
    Check(!spatial::VentStageReady(77,false,false),"owner exit completes from a stale backstage cycle");
    using aimp::alien_pvp_local::FlightDirection;
    const auto level=FlightDirection({0.0F,-0.99F,0.1F},0.0F,1.0F,0.0F,0.0F);
    Check(level[1]==0.0F && std::abs(level[2]-1.0F)<0.001F,"pitched camera makes W descend");
    const auto vertical=FlightDirection({0.0F,1.0F,0.0F},1.5707963F,1.0F,0.0F,0.0F);
    Check(vertical[0]>0.999F && vertical[1]==0.0F,"vertical look loses horizontal heading");
    const auto diagonal=FlightDirection({1.0F,0.0F,0.0F},0.0F,1.0F,1.0F,1.0F);
    Check(std::abs(std::hypot(std::hypot(diagonal[0],diagonal[2]),diagonal[1])-1.0F)<0.001F &&
        diagonal[1]>0.0F && diagonal[2]<0.0F,"flight diagonal is too fast or strafe is reversed");
    const auto idle=FlightDirection({0.0F,-1.0F,0.0F},0.0F,0.0F,0.0F,0.0F);
    Check(idle==std::array<float,3>{},"idle camera drifts");
    alignas(16) std::array<std::byte,0x4400> body{};
    alignas(16) std::array<std::byte,0x80> cycle{};
    const auto character=reinterpret_cast<std::uintptr_t>(body.data());
    const auto action=reinterpret_cast<std::uintptr_t>(cycle.data());
    std::memcpy(body.data()+0x36C,&action,sizeof(action));
    std::memcpy(cycle.data()+4,&character,sizeof(character));cycle[0x1A]=std::byte{1};
    ActionHooks hooks{&Begin,nullptr,&Busy,&Cancel,nullptr,nullptr,&Drive};
    Context context{{1,2,3,4,5,6,7},1,character,true,false,true,true,&hooks};
    Pump(context);
    Frame activate{};activate.scope=context.scope;activate.kind=Kind::Activate;activate.epoch=1;
    Queue(activate);Pump(context);
    Frame command{};command.scope=context.scope;command.kind=Kind::Commit;command.epoch=1;
    command.sequence=command.hostSequence=1;command.flags=Focused;command.forward=1;command.gait=3;
    Queue(command);Pump(context);
    Check(driveCalls==1 && lastIntent.gait==3 && MovingNow(),"sprint did not reach donor movement");
    Check(genericCalls==0,"generic carrier still drives the Alien");
    command.sequence=command.hostSequence=2;command.gait=0;command.strafe=-1;
    Queue(command);Pump(context);
    Check(lastIntent.gait==0 && lastIntent.strafe==-1,"slow/diagonal input was lost");
    stopReady=false;command.sequence=command.hostSequence=3;command.action=Action::EnterVent;
    Queue(command);Pump(context);
    Check(beginCalls==0,"vent started before movement retirement");
    stopReady=true;Sleep(330);Pump(context);
    Check(beginCalls==1,"deferred vent did not start after retirement");
    Check(VentAuthority().phase==ActionPhase::Started,"Host vent progress was not exposed to the selector");
    const auto duringAction=driveCalls;Pump(context);
    Check(driveCalls==duringAction,"movement replaced a native vent action");
    Frame stop=activate;stop.kind=Kind::Stop;Queue(stop);Pump(context);
    Check(!MovingNow() && stopCalls>0,"revocation left donor movement running");
    Check(genericCalls==0,"stop or action rearmed the generic carrier");
    Reset(true);
    context.host=false;context.localOwner=true;
    Pump(context);Queue(activate);
    LocalInput movement{1,0,0,0,3,Action::None,true};CaptureLocal(movement);
    const auto beforePlacement=driveCalls;Pump(context);
    Check(driveCalls==beforePlacement,"client predicted from an unaligned local spawn");
    Frame correction=activate;correction.kind=Kind::Correction;correction.sequence=1;correction.flags=PoseValid;
    correction.x=-32;correction.y=19;correction.z=-10;Queue(correction);Pump(context);
    Check(placements==1 && driveCalls>beforePlacement,"first Host pose did not align the client Alien");
    correction.sequence=2;correction.x=-31;Queue(correction);Pump(context);
    Check(placements==1,"ordinary pose correction teleported the Alien again");
    hooks.recover=&Recover;
    const auto immediateDrive=driveCalls;
    correction.sequence=3;correction.x=-300;Queue(correction);CaptureLocal(movement);Pump(context);
    Check(recoveryCalls==0 && driveCalls>immediateDrive,"a delayed Host pose stole the owner's fresh steering");
    hooks.recover=nullptr;
    auto selected=movement;selected.action=Action::ExitVent;selected.selectedVent=true;
    selected.ventX=10;selected.ventY=22;selected.ventZ=40;
    CaptureLocal(selected);CaptureLocal(movement);Pump(context);
    std::array<std::uint8_t,kWireBytes> bytes{};bool selectedObserved{};
    while(TryPopObservation(bytes)) {
        Frame frame{};Check(Decode(bytes,frame),"invalid observation");
        if(frame.kind==Kind::Input && frame.action==Action::ExitVent) {
            selectedObserved=(frame.flags&VentSelected)!=0U && frame.x==10 && frame.y==22 && frame.z==40;
            Frame ack{};ack.scope=context.scope;ack.epoch=frame.epoch;ack.kind=Kind::Acknowledge;ack.deliveryId=frame.deliveryId;Queue(ack);
        }
    }
    Check(selectedObserved,"selected vent coordinates were lost between input capture and publication");
    selected.action=Action::EnterVent;Sleep(20);CaptureLocal(selected);Pump(context);
    bool entrySelected{};
    while(TryPopObservation(bytes)) {
        Frame frame{};Check(Decode(bytes,frame),"invalid selected entry observation");
        if(frame.kind==Kind::Input && frame.action==Action::EnterVent)
            entrySelected=(frame.flags&VentSelected)!=0U && frame.x==10 && frame.y==22 && frame.z==40;
    }
    Check(entrySelected,"entry omitted the portal shown by the interaction prompt");
    Check(VentAuthority().phase==ActionPhase::Preparing,"queued travel was shown as completed");
    Check(vents::WithinReach({0,0,0},{0,3,0}),"overhead vent was not interactable");
    Check(!vents::WithinReach({0,0,0},{0,8,0}),"vent on another floor was interactable");
    Check(!vents::WithinReach({0,0,0},{8,0,0}),"distant vent was interactable");
    Reset(true);
    cycle[0x1A]=std::byte{77};body[0x2DA]=std::byte{1};
    LocalInput stationary{};stationary.focused=true;
    Pump(context);Queue(activate);CaptureLocal(stationary);Pump(context);
    const auto beforeBackstagePlacement=placements;
    correction.sequence=1;correction.flags=PoseValid;Queue(correction);Pump(context);
    Check(placements==beforeBackstagePlacement,"initial placement crossed native stages");
    correction.sequence=2;correction.flags=PoseValid|Backstage|NativeBusy;Queue(correction);Pump(context);
    Check(placements==beforeBackstagePlacement,"busy Host passage was used as initial placement");
    cycle[0x1A]=std::byte{75};correction.sequence=3;correction.flags=PoseValid|Backstage;
    Queue(correction);Pump(context);
    Check(placements==beforeBackstagePlacement,"initial placement interrupted local traversal");
    cycle[0x1A]=std::byte{77};correction.sequence=4;Queue(correction);Pump(context);
    Check(placements==beforeBackstagePlacement+1,"stationary backstage spawn never aligned");
    correction.sequence=5;Queue(correction);Pump(context);
    Check(placements==beforeBackstagePlacement+1,"backstage placement repeated");
    CaptureLocal(movement);Pump(context);
    Check(placements>beforeBackstagePlacement+1 && MovingNow(),"backstage movement remained gated after placement");
    Reset(true);
    // Reproduce diagnostics4696: Host Started and Completed arrive together,
    // before the Client's native settling interval has expired.
    context.host=false;context.localOwner=true;
    Pump(context);Queue(activate);Pump(context);
    command.sequence=command.hostSequence=1;command.action=Action::ExitVent;
    command.forward=command.strafe=0.0F;Queue(command);
    Frame resolution=command;resolution.kind=Kind::Resolution;
    resolution.phase=ActionPhase::Started;resolution.resolutionRevision=1;
    const int priorStarts=starts,priorCompletions=completions;
    Queue(resolution);resolution.phase=ActionPhase::Completed;resolution.resolutionRevision=2;Queue(resolution);
    Pump(context);
    Check(starts==priorStarts && completions==priorCompletions,"completion bypassed the local settle interval");
    Sleep(330);CaptureLocal(stationary);Pump(context);
    Check(starts==priorStarts+1 && completions==priorCompletions+1,"fast Host completion discarded the local exit start");
    // A stationary owner still publishes the action's order after entering a
    // vent; camera flight generates no physical Alien movement to advance it.
    Sleep(40);CaptureLocal(stationary);Pump(context);
    bool appliedStationaryAction{};
    while(TryPopObservation(bytes)) {
        Frame frame{};Check(Decode(bytes,frame),"invalid stationary observation");
        if(frame.kind==Kind::Pose && frame.hostSequence==command.hostSequence &&
            frame.acknowledgedInput==command.sequence)appliedStationaryAction=true;
    }
    Check(appliedStationaryAction,"stationary owner pose omitted its applied vent action");
    Check(!VentPending(),"local completed exit kept camera noclip pending Host acknowledgement");
    Reset(true);
    // Remote vent approval must leave replica pose application available.
    // It must not restart native steering while waiting for the owning player.
    context.host=true;context.localOwner=false;proxyVent=true;hooks.replicatePose=&Replicate;
    Pump(context);Queue(activate);Pump(context);
    command.sequence=command.hostSequence=1;command.action=Action::EnterVent;
    command.forward=1;Queue(command);Pump(context);Sleep(330);Pump(context);
    const auto priorDrive=driveCalls;
    correction.sequence=1;correction.flags=PoseValid|OwnerMeasured|Backstage;
    correction.hostSequence=correction.acknowledgedInput=1;Queue(correction);Pump(context);
    Check(replicas==1,"pending remote vent blocked measured owner stage replication");
    Check(driveCalls==priorDrive,"pending remote vent restarted the Host's own navigation");
    correction.sequence=2;correction.flags|=NativeBusy|VentAnimating;Queue(correction);Pump(context);
    Check(replicas==2,"authenticated exit animation was blocked by the busy-pose fence");
    correction.sequence=3;correction.flags&=~VentAnimating;Queue(correction);Pump(context);
    Check(replicas==2,"ordinary busy pose bypassed the animation-only fence");
    Reset(true);
    std::cout<<"Donor control, Host placement, selected vent transport and interaction reach passed.\n";
}
