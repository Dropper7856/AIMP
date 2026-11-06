#pragma once
#include <cstdint>

namespace aimp::alien_pvp_local {

// All entry points run on the certified engine thread. The local controller
// never accepts a network pointer, never changes the AI lease owner and never
// drives another retail Character.
struct Context {
    std::uintptr_t moduleBase{};
    std::uintptr_t alien{};
    std::uintptr_t technicalPlayer{};
    std::uint32_t nativeGeneration{};
    std::uint64_t ownerPeer{}, localPeer{};
    bool exclusiveLease{};
};

void Pump(const Context& context) noexcept;
// clearCurrentWorld is false during teardown/physical generation rollover:
// never dereference old Characters or resurrect a camera from the old world.
void Reset(bool clearCurrentWorld) noexcept;
[[nodiscard]] bool Drives(std::uintptr_t character, std::uint32_t nativeGeneration) noexcept;

} // namespace aimp::alien_pvp_local
