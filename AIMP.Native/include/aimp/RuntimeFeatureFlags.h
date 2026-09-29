#pragma once

#include <cstdint>
#include <string_view>

namespace aimp::runtime_features {

enum class Feature : std::uint32_t {
    PlayerSyncObserver = 1U << 0U,
    RemoteMovementRuntime = 1U << 1U,
    WorldSyncHooks = 1U << 2U,
    CharacterLifecycleCapture = 1U << 3U,
    EngineThreadEvidence = 1U << 4U,
    NativeDiagnostics = 1U << 5U,
    LocomotionFlightRecorder = 1U << 6U,
    GameThreadPumps = 1U << 7U,
};

[[nodiscard]] bool Enabled(Feature feature) noexcept;
void SetEnabled(Feature feature, bool enabled) noexcept;
[[nodiscard]] bool SetByName(std::string_view name, bool enabled) noexcept;
[[nodiscard]] bool EnabledByName(std::string_view name) noexcept;
[[nodiscard]] std::uint32_t Mask() noexcept;

} // namespace aimp::runtime_features
