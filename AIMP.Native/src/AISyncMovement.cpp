#include "aimp/AISyncMovement.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace aimp::ai_sync::movement {
namespace {
template<class T> void Put(std::span<std::uint8_t> out, std::size_t at, T value) noexcept {
    for (std::size_t i=0;i<sizeof(T);++i) out[at+i]=static_cast<std::uint8_t>(value>>(i*8));
}
std::uint32_t Get(std::span<const std::uint8_t> in, std::size_t at) noexcept {
    std::uint32_t v{}; for(std::size_t i=0;i<4;++i)v|=static_cast<std::uint32_t>(in[at+i])<<(i*8); return v;
}
bool Finite(float f) noexcept { return std::isfinite(f) && std::abs(f)<=100000.0F; }
bool Zero(float f) noexcept { return std::bit_cast<std::uint32_t>(f)==0; }
bool Empty(const Parameters& p) noexcept {
    return !p.retained && !p.style && !p.flags90 && !p.flags91 && Zero(p.stoppingDistance) && Zero(p.stopDistance) &&
        std::all_of(p.goal.begin(),p.goal.end(),Zero) && std::all_of(p.query.begin(),p.query.end(),Zero);
}
bool Valid(const Parameters& p) noexcept {
    // Query words are copied bit-exact. 004541D0 leaves block+6C/+70
    // uninitialized when 0070A940's vfunc+410 declines: the Host executes those
    // exact bits, so the Client must receive them unchanged, never rejected.
    return p.retained<=1 && p.style<=4 && !(p.flags90&~0x3FU) && !(p.flags91&~0x31U) &&
        Finite(p.stoppingDistance) && p.stoppingDistance>=-1 && Finite(p.stopDistance) && p.stopDistance>=-1 &&
        std::all_of(p.goal.begin(),p.goal.end(),Finite);
}
void Write(std::span<std::uint8_t> out, std::size_t at, const Parameters& p) noexcept {
    Put(out,at,p.retained); Put(out,at+4,p.style);
    for(std::size_t i=0;i<3;++i)Put(out,at+8+i*4,std::bit_cast<std::uint32_t>(p.goal[i]));
    Put(out,at+20,std::bit_cast<std::uint32_t>(p.stoppingDistance)); Put(out,at+24,std::bit_cast<std::uint32_t>(p.stopDistance));
    for(std::size_t i=0;i<9;++i)Put(out,at+28+i*4,std::bit_cast<std::uint32_t>(p.query[i]));
    Put(out,at+64,p.flags90); Put(out,at+68,p.flags91);
}
void Read(std::span<const std::uint8_t> in, std::size_t at, Parameters& p) noexcept {
    p.retained=Get(in,at);p.style=Get(in,at+4);
    for(std::size_t i=0;i<3;++i)p.goal[i]=std::bit_cast<float>(Get(in,at+8+i*4));
    p.stoppingDistance=std::bit_cast<float>(Get(in,at+20));p.stopDistance=std::bit_cast<float>(Get(in,at+24));
    for(std::size_t i=0;i<9;++i)p.query[i]=std::bit_cast<float>(Get(in,at+28+i*4));
    p.flags90=Get(in,at+64);p.flags91=Get(in,at+68);
}
}
bool Empty(const Bundle& b) noexcept {
    return !b.version && !b.parts && !b.gait && static_cast<std::uint32_t>(b.navigation)==0 &&
        Empty(b.before) && Empty(b.after) && Empty(b.companion) && !b.companionFlags && !b.companionRetainedAfter && !b.primaryFlags;
}
bool Validate(const Bundle& b) noexcept {
    if(Empty(b))return true;
    return b.version==1 && b.parts>=1 && b.parts<=2 && b.gait<=3 && b.primaryFlags<=7 &&
        (b.navigation==Navigation::LocalNative || CarriesRoute(b.navigation)) &&
        Valid(b.before) && Valid(b.after) && (b.parts==2 ?
            Valid(b.companion) && b.companionFlags<=7 && b.companionRetainedAfter<=1 :
            Empty(b.companion) && !b.companionFlags && !b.companionRetainedAfter);
}
bool Encode(const Bundle& b, CanonicalBundle& out) noexcept {
    if(!Validate(b))return false;
    CanonicalBundle bytes{};Put(bytes,0,b.version);Put(bytes,4,b.parts);Put(bytes,8,b.gait);Put(bytes,12,static_cast<std::uint32_t>(b.navigation));
    Write(bytes,16,b.before);Write(bytes,88,b.after);Write(bytes,160,b.companion);
    Put(bytes,232,b.companionFlags);Put(bytes,236,b.companionRetainedAfter);Put(bytes,240,b.primaryFlags);out=bytes;return true;
}
bool Decode(std::span<const std::uint8_t> in, Bundle& out) noexcept {
    if(in.size()!=kBundleBytes)return false;
    Bundle b{};b.version=Get(in,0);b.parts=Get(in,4);b.gait=Get(in,8);b.navigation=static_cast<Navigation>(Get(in,12));
    Read(in,16,b.before);Read(in,88,b.after);Read(in,160,b.companion);
    b.companionFlags=Get(in,232);b.companionRetainedAfter=Get(in,236);
    b.primaryFlags=Get(in,240);
    if(!Validate(b))return false;out=b;return true;
}
bool Same(const Bundle& a, const Bundle& b) noexcept {
    CanonicalBundle first{},second{};return Encode(a,first) && Encode(b,second) && first==second;
}
bool Same(const Parameters& a,const Parameters& b) noexcept {
    std::array<std::uint8_t,kParametersBytes> first{},second{};Write(first,0,a);Write(second,0,b);return first==second;
}
} // namespace aimp::ai_sync::movement
