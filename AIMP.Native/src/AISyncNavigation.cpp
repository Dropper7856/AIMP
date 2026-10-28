#include "aimp/AISyncNavigation.h"
#include "aimp/CrashGuard.h"
namespace aimp::ai_sync::navigation {
namespace {
std::uintptr_t gBase{};
template<class T> bool Read(std::uintptr_t at,T& out) noexcept {
    return at>=0x10000U&&crash_guard::TryCopy(&out,reinterpret_cast<const void*>(at),sizeof(out));
}
}
bool Initialize(std::uintptr_t base,std::string& error) {
    if(base<0x10000U){error="Invalid AISync image base";return false;}
    gBase=base;return true;
}
HeadObservation InspectHead(std::uintptr_t character) noexcept {
    HeadObservation result{};std::uintptr_t owner{},sentinel{},node{},request{},table{};
    const auto scheduler=character+0x2CD0U;
    if(!Read(scheduler,owner)||owner!=character||!Read(scheduler+8U,result.count)||result.count>64U)return result;
    if(result.count==0U){result.readable=true;return result;}
    if(!Read(scheduler+4U,sentinel)||!sentinel||!Read(sentinel,node)||node==sentinel||
        !Read(node+8U,request)||!request||!Read(request,table)||table<gBase||
        !Read(request+0x10U,result.kind)||!Read(request+0xCU,result.status))return result;
    result.typeRva=static_cast<std::uint32_t>(table-gBase);result.readable=true;return result;
}
Counters Snapshot() noexcept {return {};}
}
