#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace aimp::player_torch {
// Native compact 3x4 rows: right/up/forward columns and world translation.
// Until aim pitch is replicated, the beam follows actor yaw on the XZ plane.
// Invalid samples leave output untouched.
bool BuildYawTransform(float yaw, const std::array<float, 3>& origin,
    std::array<float, 12>& output) noexcept;
void Initialize(std::uintptr_t moduleBase) noexcept;
// IPC only publishes state. Native objects are accessed by the engine pump.
bool Publish(std::uintptr_t character, bool enabled, std::string& error) noexcept;
void PumpOnEngineThread(std::uintptr_t character) noexcept;
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;
struct DiagnosticsSnapshot final {
    std::uint64_t applications{}, failures{}, creations{};
    bool desired{}, emitted{};
    // Submission is not a render/illumination readback.
    bool transformSubmitted{}, socketOrigin{};
};
DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;
}
