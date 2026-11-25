#pragma once
#include "aimp/AlienPvPWire.h"
#include "aimp/AlienPvPActionRuntime.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace aimp::alien_pvp {
struct Context final {
    Scope scope{};
    std::uintptr_t base{},character{};
    bool host{},localOwner{},owned{},pilotReady{};
    const ActionHooks* actions{};
};
struct LocalInput final {
    float forward{},strafe{},yaw{},pitch{};
    std::uint32_t gait{1U};
    Action action{Action::None};
    bool focused{};
    bool selectedVent{};
    float ventX{},ventY{},ventZ{};
};
// Engine-thread calls. No camera/input/character pointer crosses IPC.
void CaptureLocal(const LocalInput& input) noexcept;
void Pump(const Context& context) noexcept;
void Reset(bool certifiedLivingCharacter=false) noexcept;
bool Owns(std::uintptr_t character,std::uint32_t generation) noexcept;
bool MovingNow() noexcept;
Frame VentAuthority() noexcept;
bool VentPending() noexcept;
bool ControlActive() noexcept;
// Worker-thread calls. Queues hold copied frames, never native resources.
bool QueueCommand(std::span<const std::byte> bytes) noexcept;
bool TryPopObservation(std::array<std::uint8_t,kWireBytes>& bytes) noexcept;
struct Counters final {
    std::uint64_t received{},rejected{},expired{},dropped{},inputs{},commits{},poses{},corrections{},unsupportedActions{};
};
Counters Snapshot() noexcept;
// Engine thread: the last owner pose (Correction) this observer received and
// its age. False on the owner or before any correction. Debug only.
bool LastCorrection(Frame& frame,std::uint64_t& ageMs) noexcept;
// Debug point for "the Alien is hard to control / stops in place": frames in
// which the local pilot held a movement key, how many drove the Alien and why
// the others did not (session totals, any thread).
struct MovementStats final {
    std::uint64_t heldFrames{},drivenFrames{},blockedAction{},blockedVent{},blockedStale{},blockedStage{},driveRefused{};
    std::uint64_t chaseDriven{}; // AvS round 5: frames the auto-chase drove the Alien
};
MovementStats MovementSnapshot() noexcept;
} // namespace aimp::alien_pvp
