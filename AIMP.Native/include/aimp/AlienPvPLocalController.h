#pragma once
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }

namespace aimp::alien_pvp_local {
bool InstallCameraHooks(std::uintptr_t base,HookManager& hooks,std::string& error);
// PlayerLifecycle spectate: third-person orbit around a living Character,
// driven by the mouse (wheel = distance), applied after the retail
// FollowBehavior update of exactly this behavior. target 0 turns it off.
// Engine thread.
void SetSpectateOrbit(std::uintptr_t base,std::uintptr_t followBehavior,std::uintptr_t target) noexcept;
struct SpectateOrbitStats {std::uint64_t frames{},collisions{},noWorld{};};
[[nodiscard]] SpectateOrbitStats SpectateOrbitSnapshot() noexcept;

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
    // Living survivors' current positions on this PC (Host-certified
    // participants other than the Alien owner). Used only to rank vent exits.
    std::uint32_t survivorCount{};
    float survivors[4][3]{};
};

struct Readiness final {
    bool inputIsolated{}, cameraBound{}, technicalHidden{}, moving{};
};
[[nodiscard]] Readiness Snapshot() noexcept;
struct VentInterface {bool open{},backstage{};int nearby{-1},selected{-1};bool freeCamera{};
    // Vent-exit picker (replaces free flight while backstage): candidate exits
    // near survivors, the selected position in that list and the list size.
    int pickerSlot{-1},pickerCount{};
    // A living survivor is inside the Host's attack shortlist (reach, height,
    // aim cone or contact) from the pilot's view: "Left click to kill".
    bool attackReady{};};
[[nodiscard]] VentInterface VentSnapshot() noexcept;
// Exit-camera aim (AvS 2026-11-25 round 3: grey view into the duct), safe to
// read from the diagnostics thread. mode 1 physics rays, 2 toward survivor (no
// world sampled), 3 kept mouse view; open = free distance seen from the exit
// (m, -1 unknown); aims = exits aimed so far; vent = last aimed exit index.
// Round 4: mode 4 = boxed in at the exit, probed 1.2 m above the floor below;
// out = metres the eye sits out from the exit, lift = eye height over the exit.
// Round 5: floorDrop = room floor found below the exit (m, 0 = floor-level
// exit), ventHidden = frames a vent in reach was hidden behind geometry (E not
// offered). mode 5 = eye pulled back to the verified-open probe point.
struct VentCameraStats {std::uint32_t aims{},mode{},worlds{};float open{};int vent{-1};float out{},lift{},floorDrop{};
    std::uint64_t ventHidden{};};
[[nodiscard]] VentCameraStats VentCameraSnapshot() noexcept;
bool GoToVent(std::uint32_t index) noexcept;
void Pump(const Context& context) noexcept;
// clearCurrentWorld is false during teardown/physical generation rollover:
// never dereference old Characters or resurrect a camera from the old world.
// Only the AISync lease-release path may prove a living native identity and
// permit a write to its movement slot. Ordinary teardown restores camera
// targets but must never touch an incarnation which may have been recycled.
void Reset(bool sameCameraWorld, bool certifiedLivingCharacter = false) noexcept;
[[nodiscard]] bool Drives(std::uintptr_t character, std::uint32_t nativeGeneration) noexcept;
// The Alien whose camera the local pilot currently drives (0 otherwise),
// published by Pump for the audio thread's listener redirect.
[[nodiscard]] std::uintptr_t ListenerCharacter() noexcept;

} // namespace aimp::alien_pvp_local
