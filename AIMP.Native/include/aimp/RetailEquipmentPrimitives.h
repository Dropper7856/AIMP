#pragma once

#include <cstdint>

namespace aimp {

// AI.exe FUN_00654E60 / RVA 0x00254E60. Static decompilation of the
// retail NPC equipment path shows this exact nine-argument contract. The
// command reconstructs the transient equipment action and executes its vfunc,
// so callers must use this boundary instead of writing Character equipment
// fields or attaching models manually.
using NpcEquipmentCommandFn = void(__thiscall*)(
    void* controller,
    std::uint8_t changed,
    std::uint32_t currentSlot,
    std::uint32_t transitionControlA,
    std::uint8_t direction,
    std::uint32_t targetSlot,
    std::uint32_t transitionControlB,
    std::uint8_t flag7,
    std::uint8_t flag8);

} // namespace aimp
