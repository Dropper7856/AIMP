#pragma once
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::alien_pvp_local {
bool InstallCameraHooks(std::uintptr_t base,HookManager& hooks,std::string& error);

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
    bool possessionRequested{};
};

struct Readiness final {
    bool inputIsolated{}, cameraBound{}, technicalHidden{}, moving{};
};
[[nodiscard]] Readiness Snapshot() noexcept;
void Pump(const Context& context) noexcept;
// clearCurrentWorld is false during teardown/physical generation rollover:
// never dereference old Characters or resurrect a camera from the old world.
// Only the AISync lease-release path may prove a living native identity and
// permit a write to its movement slot. Ordinary teardown restores camera
// targets but must never touch an incarnation which may have been recycled.
void Reset(bool sameCameraWorld, bool certifiedLivingCharacter = false) noexcept;
[[nodiscard]] bool Drives(std::uintptr_t character, std::uint32_t nativeGeneration) noexcept;

} // namespace aimp::alien_pvp_local
