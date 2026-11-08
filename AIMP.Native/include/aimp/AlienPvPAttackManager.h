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
    bool (*write)(void*,std::uintptr_t,const void*,std::size_t) noexcept{};
    template<class T> bool Read(std::uintptr_t address,T& value) const noexcept {
        return read && read(context,address,&value,sizeof(value));
    }
    template<class T> bool Write(std::uintptr_t address,const T& value) const noexcept {
        return write && write(context,address,&value,sizeof(value));
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
// 006F3A30 and ActionMeleeAttack 00717D90 both admit a target only through
// 004128E0: the selected record [[[manager+4]+C8]+10] must hold the victim at
// +42C and its birth (*(victim+50), FUN_00783B00) at +430. A piloted Alien
// has no retail target, so the query always failed (452 NativeRefused). This
// is the donor's control.cpp::SetTargetOf on the record the query reads.
struct HeldTarget final {std::uintptr_t record{},previousTarget{};std::uint32_t previousBirth{};std::uintptr_t victim{};};
// Restores only a record still carrying our victim; a retail retarget wins.
inline void Release(const Memory& memory,HeldTarget& held) noexcept {
    std::uintptr_t target{};
    if(held.record && held.previousTarget!=held.victim && memory.Read(held.record+0x42CU,target) && target==held.victim) {
        static_cast<void>(memory.Write(held.record+0x42CU,held.previousTarget));
        static_cast<void>(memory.Write(held.record+0x430U,held.previousBirth));
    }
    held={};
}
enum class HoldResult : std::uint8_t {Held,NoSelection,NoRecord,Fault};
inline HoldResult Hold(const Memory& memory,const Manager& manager,std::uintptr_t victim,HeldTarget& held) noexcept {
    std::uintptr_t context{},selection{},record{},target{};std::uint32_t birth{},victimBirth{};
    if(!manager.address || !victim || !memory.Read(manager.address+4U,context) || !context ||
        !memory.Read(context+0xC8U,selection) || !selection)return HoldResult::NoSelection;
    if(!memory.Read(selection+0x10U,record) || !record)return HoldResult::NoRecord;
    if(!memory.Read(record+0x42CU,target) || !memory.Read(record+0x430U,birth) ||
        !memory.Read(victim+0x50U,victimBirth))return HoldResult::Fault;
    if(target==victim && birth==victimBirth) {
        if(!held.record)held={record,target,birth,victim};
        return HoldResult::Held;
    }
    // Retail may promote another record between polls; keep the first
    // original only for the record we actually changed.
    if(held.record!=record) {
        if(held.record)Release(memory,held);
        held={record,target,birth,victim};
    }
    if(!memory.Write(record+0x430U,victimBirth) || !memory.Write(record+0x42CU,victim))return HoldResult::Fault;
    return HoldResult::Held;
}
} // namespace aimp::alien_pvp::attack
