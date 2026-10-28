#pragma once
#include "aimp/AISync.h"
#include "aimp/AISyncExecutionCodec.h"
#include <algorithm>
#include <cstring>

namespace aimp::ai_sync::causal_state {
inline constexpr std::uint32_t kMagic=0x45534941U; // AISE
inline constexpr std::size_t kHeader=72,kBytes=kHeader+sizeof(State);
inline constexpr std::uint16_t kVersion=1;
template<class T> inline void Put(std::span<std::uint8_t> out,std::size_t at,T value) noexcept {
    for(std::size_t i=0;i<sizeof(T);++i)out[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
template<class T> inline T Get(std::span<const std::uint8_t> in,std::size_t at) noexcept {
    T value{};for(std::size_t i=0;i<sizeof(T);++i)value|=static_cast<T>(in[at+i])<<(8*i);return value;
}
inline bool Decode(std::span<const std::uint8_t> bytes,transactions::Scope& scope,
    std::uint64_t& serial,std::uint64_t& through,State& state) noexcept {
    if(bytes.size()!=kBytes||Get<std::uint32_t>(bytes,0)!=kMagic||Get<std::uint16_t>(bytes,4)!=kVersion||
        Get<std::uint16_t>(bytes,6)!=kHeader||Get<std::uint32_t>(bytes,8)!=kBytes||Get<std::uint32_t>(bytes,12))return false;
    std::copy_n(bytes.begin()+16,16,scope.session.begin());
    scope.authorityEpoch=Get<std::uint64_t>(bytes,32);scope.worldEpoch=Get<std::uint64_t>(bytes,40);
    scope.consumerEpoch=Get<std::uint64_t>(bytes,48);serial=Get<std::uint64_t>(bytes,56);through=Get<std::uint64_t>(bytes,64);
    if(!serial||!scope.authorityEpoch||!scope.worldEpoch||!scope.consumerEpoch||
        std::all_of(scope.session.begin(),scope.session.end(),[](auto b){return b==0;}))return false;
    std::memcpy(&state,bytes.data()+kHeader,sizeof(state));
    if(std::any_of(std::begin(state.interactionSession),std::end(state.interactionSession),[](auto value){return value!=0;})&&
        !std::equal(scope.session.begin(),scope.session.end(),std::begin(state.interactionSession)))return false;
    if(!action_correlation::Empty(state.executionAction)&&state.executionAction.source.session!=scope.session)return false;
    auto cookie=Get<std::uint64_t>(bytes,16);if(!cookie)cookie=1;
    return state.version==State{}.version&&state.sessionCookie==cookie&&state.worldGeneration==scope.worldEpoch;
}
inline bool Encode(std::span<std::uint8_t> bytes,const transactions::Scope& scope,std::uint64_t serial,
    std::uint64_t through,const State& state) noexcept {
    if(bytes.size()!=kBytes)return false;
    std::fill(bytes.begin(),bytes.end(),std::uint8_t{0});
    Put(bytes,0,kMagic);Put(bytes,4,kVersion);Put(bytes,6,static_cast<std::uint16_t>(kHeader));Put(bytes,8,static_cast<std::uint32_t>(kBytes));
    std::copy(scope.session.begin(),scope.session.end(),bytes.begin()+16);
    Put(bytes,32,scope.authorityEpoch);Put(bytes,40,scope.worldEpoch);Put(bytes,48,scope.consumerEpoch);
    Put(bytes,56,serial);Put(bytes,64,through);std::memcpy(bytes.data()+kHeader,&state,sizeof(state));
    transactions::Scope check{};std::uint64_t event{},prefix{};State value{};
    return Decode(bytes,check,event,prefix,value);
}
}
