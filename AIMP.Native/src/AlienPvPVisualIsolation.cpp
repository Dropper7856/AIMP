#include "aimp/AlienPvPVisualIsolation.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aimp::alien_pvp_visual {
namespace {

constexpr std::uintptr_t kModelInstRva=0x00298B00U;
constexpr std::uintptr_t kModelInstAtRva=0x00298BD0U;
constexpr std::uintptr_t kSubFlagsRva=0x005B01A0U;
constexpr std::size_t kMaxParts=96U;
constexpr std::size_t kNameCapacity=64U;

struct Part final {
    std::array<char,kNameCapacity> name{};
    std::uint8_t original{};
    bool used{};
};
struct ModelState final {
    std::uintptr_t character{};
    std::uint32_t generation{};
    std::array<Part,kMaxParts> parts{};
    std::size_t count{};
};
ModelState gTechnical{},gHead{};

template<typename T>
bool Read(std::uintptr_t at,T& output) noexcept {
    return at!=0U && crash_guard::TryCopy(&output,reinterpret_cast<const void*>(at),sizeof(T));
}
bool WriteByte(std::uintptr_t at,std::uint8_t value) noexcept {
    if (!at) return false;
#if defined(_MSC_VER)
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(at)=value;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return false;
#endif
}
bool SubName(std::uintptr_t sub,std::array<char,kNameCapacity>& name) noexcept {
    std::uintptr_t table{},object{},address{};
    if (!Read(sub+0x38U,table) || !table ||
        !Read(table,object) || !object ||
        !Read(object+8U,address) || !address) return false;
    for (std::size_t i=0U;i<name.size();++i) {
        if (!Read(address+i,name[i])) return false;
        if (name[i]=='\0') return i>0U;
    }
    return false;
}
std::uintptr_t SubFlags(std::uintptr_t module,std::uintptr_t sub) noexcept {
#if defined(_MSC_VER)
    __try {
        return reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(
            module+kSubFlagsRva)(reinterpret_cast<void*>(sub));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0U; }
#else
    return 0U;
#endif
}
std::uintptr_t Model(std::uintptr_t module,std::uintptr_t anim,std::uint32_t index,
    bool many) noexcept {
#if defined(_MSC_VER)
    __try {
        if (many)
            return reinterpret_cast<std::uintptr_t(__thiscall*)(void*,unsigned)>(
                module+kModelInstAtRva)(reinterpret_cast<void*>(anim),index);
        return reinterpret_cast<std::uintptr_t(__thiscall*)(void*)>(
            module+kModelInstRva)(reinterpret_cast<void*>(anim));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0U; }
#else
    return 0U;
#endif
}
bool MatchHead(const char* name) noexcept {
    return std::strstr(name,"SKULL")!=nullptr ||
        std::strstr(name,"SHELL")!=nullptr ||
        std::strcmp(name,"crusher")==0;
}
template<typename Callback>
bool Visit(std::uintptr_t character,Callback&& consume) noexcept {
    if (!character) return false;
    std::uintptr_t module=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) return false;
    const auto anim=character+0x3F0U;
    std::uint8_t multi{};
    std::uint32_t models{};
    if (!Read(anim+0x3CDU,multi) || !Read(anim+0x3D0U,models)) return false;
    if (!multi) models=1U;
    else if (!models || models>16U) return false;
    std::uint32_t visited{};
    for (std::uint32_t m=0U;m<models;++m) {
        const auto model=Model(module,anim,m,multi!=0U);
        if (!model) return false;
        std::uintptr_t countObject{},subList{};
        std::uint32_t parts{};
        if (!Read(model+0xC8U,countObject) || !countObject ||
            !Read(countObject+0x54U,parts) || parts>256U ||
            !Read(model+0x10CU,subList) || (!subList && parts)) return false;
        for (std::uint32_t i=0U;i<parts;++i) {
            std::uintptr_t sub{};
            if (!Read(subList+i*sizeof(std::uintptr_t),sub)) return false;
            if (!sub) continue;
            std::array<char,kNameCapacity> name{};
            if (!SubName(sub,name)) return false;
            const auto flags=SubFlags(module,sub);
            if (!flags || !consume(name,flags)) return false;
            ++visited;
        }
    }
    return visited!=0U;
}
Part* Find(ModelState& state,const std::array<char,kNameCapacity>& name) noexcept {
    for (std::size_t i=0U;i<state.count;++i)
        if (std::strcmp(state.parts[i].name.data(),name.data())==0)
            return &state.parts[i];
    return nullptr;
}
bool Hide(ModelState& state,std::uintptr_t character,std::uint32_t generation,
    bool onlyHead) noexcept {
    if (!character || !generation) return false;
    if (state.character!=character || state.generation!=generation) {
        state={};state.character=character;state.generation=generation;
    }
    std::size_t selected{};
    const bool success=Visit(character,[&](const std::array<char,kNameCapacity>& name,
        std::uintptr_t flags) noexcept {
        if (onlyHead && !MatchHead(name.data())) return true;
        std::uint8_t value{};
        if (!Read(flags+7U,value)) return false;
        auto* existing=Find(state,name);
        if (!existing) {
            if (state.count==state.parts.size()) return false;
            auto& entry=state.parts[state.count++];
            entry.name=name;entry.original=value;entry.used=true;
        }
        ++selected;
        return value==0U || WriteByte(flags+7U,0U);
    });
    return success && selected!=0U;
}
void Restore(ModelState& state,std::uintptr_t character,std::uint32_t generation,
    bool provenLive) noexcept {
    if (provenLive && state.count && character && generation &&
        state.character==character && state.generation==generation) {
        static_cast<void>(Visit(character,[&](const std::array<char,kNameCapacity>& name,
            std::uintptr_t flags) noexcept {
            const auto* existing=Find(state,name);
            return !existing || WriteByte(flags+7U,existing->original);
        }));
    }
    // The retail engine can reuse a flags address for a different NPC. Never
    // retain a native pointer or apply saved values in a future generation.
    state={};
}
} // namespace

bool HideTechnical(std::uintptr_t player,std::uint32_t generation) noexcept {
    return Hide(gTechnical,player,generation,false);
}
bool HideAlienHead(std::uintptr_t alien,std::uint32_t generation) noexcept {
    return Hide(gHead,alien,generation,true);
}
void RestoreTechnical(std::uintptr_t player,std::uint32_t generation,bool provenLive) noexcept {
    Restore(gTechnical,player,generation,provenLive);
}
void RestoreAlienHead(std::uintptr_t alien,std::uint32_t generation,bool provenLive) noexcept {
    Restore(gHead,alien,generation,provenLive);
}
void ForgetAll() noexcept {gTechnical={};gHead={};}
} // namespace aimp::alien_pvp_visual
