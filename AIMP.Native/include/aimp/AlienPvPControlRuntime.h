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
// Worker-thread calls. Queues hold copied frames, never native resources.
bool QueueCommand(std::span<const std::byte> bytes) noexcept;
bool TryPopObservation(std::array<std::uint8_t,kWireBytes>& bytes) noexcept;
struct Counters final {
    std::uint64_t received{},rejected{},expired{},dropped{},inputs{},commits{},poses{},corrections{},unsupportedActions{};
};
Counters Snapshot() noexcept;
} // namespace aimp::alien_pvp
