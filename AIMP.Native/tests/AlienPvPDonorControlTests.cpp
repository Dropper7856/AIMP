#include "aimp/AlienPvPControlRuntime.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <cstdlib>

namespace {
int genericCalls{},driveCalls{},stopCalls{},beginCalls{};
bool stopReady=true,nativeBusy=false;
aimp::alien_pvp::Frame lastIntent{};
void Check(bool pass,const char* message) {
    if(!pass){std::cerr<<message<<'\n';std::exit(1);}
}
bool Drive(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame* frame) noexcept {
    if(!frame){++stopCalls;return stopReady;}
    ++driveCalls;lastIntent=*frame;return true;
}
bool Begin(const aimp::alien_pvp::Context&,const aimp::alien_pvp::Frame& frame,aimp::alien_pvp::Frame& out) noexcept {
    ++beginCalls;out=frame;out.phase=aimp::alien_pvp::ActionPhase::Started;
    out.reason=aimp::alien_pvp::ActionReason::None;out.resolutionRevision=1U;
    nativeBusy=true;return true;
}
bool Busy(const aimp::alien_pvp::Context&) noexcept {return nativeBusy;}
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
bool ApplyAIRecoveryTransform(std::uintptr_t,const float*,float) noexcept {return false;}
bool ApplyReplicatedYaw(std::uintptr_t,float,std::string&) noexcept {++genericCalls;return false;}
}
int main() {
    using namespace aimp::alien_pvp;
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
    stopReady=true;Pump(context);
    Check(beginCalls==1,"deferred vent did not start after retirement");
    const auto duringAction=driveCalls;Pump(context);
    Check(driveCalls==duringAction,"movement replaced a native vent action");
    Frame stop=activate;stop.kind=Kind::Stop;Queue(stop);Pump(context);
    Check(!MovingNow() && stopCalls>0,"revocation left donor movement running");
    Check(genericCalls==0,"stop or action rearmed the generic carrier");
    Reset(true);
    std::cout<<"Donor movement routing, gait, action retirement and revocation passed.\n";
}
