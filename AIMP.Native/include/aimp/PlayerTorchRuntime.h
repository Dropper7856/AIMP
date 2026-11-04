#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace aimp::player_torch {
// Native compact 3x4 rows: right/up/forward columns and world translation.
// Invalid samples leave output untouched.
bool BuildYawTransform(float yaw, const std::array<float, 3>& origin,
    std::array<float, 12>& output) noexcept;
bool ValidDirection(const std::array<float, 3>& direction) noexcept;
bool DecodeDirection(std::string_view text, std::array<float, 3>& direction) noexcept;
bool BuildAimTransform(const std::array<float, 3>& direction, const std::array<float, 3>& origin,
    std::array<float, 12>& output) noexcept;
void Initialize(std::uintptr_t moduleBase) noexcept;
// IPC only publishes state. Native objects are accessed by the engine pump.
bool Publish(std::uintptr_t character, bool enabled, std::string& error,
    const std::array<float, 3>* direction = nullptr) noexcept;
void PumpOnEngineThread(std::uintptr_t character) noexcept;
void PumpAllOnEngineThread() noexcept;
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;
struct DiagnosticsSnapshot final {
    std::uint64_t applications{}, failures{}, creations{};
    bool desired{}, emitted{};
    // Submission is not a render/illumination readback.
    bool transformSubmitted{}, socketOrigin{};
    bool hasDirection{};
    const char* stage{"pending"};
    // Renderer activation readback, not proof that pixels reached the screen.
    std::uintptr_t renderer{}, lastOnRenderer{};
    std::uint64_t onFrames{}, renderOnFrames{};
    const char* lastOnStage{"none"};
};
DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;
}
