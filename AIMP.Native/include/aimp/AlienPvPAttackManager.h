#pragma once
#include <cstdint>
#include <cstddef>

namespace aimp::alien_pvp::attack {
// Retail AlienAttackManager (012CC330). The human/android manager at
// 012CC390 has a different definition table and asynchronous query layout.
inline constexpr std::uintptr_t kTableRva=0x00ECC330U;
inline constexpr std::uintptr_t kEvaluateRva=0x002F3A30U, kChooseRva=0x00310C60U;
struct Memory final {
    std::uintptr_t base{};void* context{};
    bool (*read)(void*,std::uintptr_t,void*,std::size_t) noexcept{};
    template<class T> bool Read(std::uintptr_t address,T& value) const noexcept {
        return read && read(context,address,&value,sizeof(value));
    }
};
struct Manager final {std::uintptr_t address{},definitions{};std::uint32_t count{};};
inline bool Resolve(const Memory& memory,std::uintptr_t character,std::uintptr_t animated,Manager& out) noexcept {
    out={};std::uintptr_t manager{},table{},context{},owner{},list{},definitions{};std::uint32_t count{};
    if(!memory.base || !character || !animated || !memory.Read(animated+0x34B4U,manager) || !manager ||
        !memory.Read(manager,table) || table!=memory.base+kTableRva ||
        !memory.Read(manager+4U,context) || !context || !memory.Read(context+0x128U,owner) || owner!=character ||
        !memory.Read(manager+0x1C0U,list) || !list || !memory.Read(list+4U,definitions) || !definitions ||
        !memory.Read(list+8U,count) || !count || count>25U)return false;
    out={manager,definitions,count};return true;
}
inline bool Selected(const Memory& memory,const Manager& manager,std::uint32_t selector) noexcept {
    if(!manager.address || selector>=manager.count)return false;
    std::uint8_t proof{};std::uint32_t index{};
    return memory.Read(manager.address+0x20U+selector*0x10U,proof) && (proof&7U)==7U &&
        memory.Read(manager.definitions+selector*0x84U+4U,index) && index==selector;
}
} // namespace aimp::alien_pvp::attack
