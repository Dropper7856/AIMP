#include "aimp/RuntimeFeatureFlags.h"

#include <atomic>

namespace aimp::runtime_features {
namespace {

constexpr std::uint32_t kAllFeatures =
    static_cast<std::uint32_t>(Feature::PlayerSyncObserver) |
    static_cast<std::uint32_t>(Feature::RemoteMovementRuntime) |
    static_cast<std::uint32_t>(Feature::WorldSyncHooks) |
    static_cast<std::uint32_t>(Feature::CharacterLifecycleCapture) |
    static_cast<std::uint32_t>(Feature::EngineThreadEvidence) |
    static_cast<std::uint32_t>(Feature::NativeDiagnostics) |
    static_cast<std::uint32_t>(Feature::LocomotionFlightRecorder) |
    static_cast<std::uint32_t>(Feature::GameThreadPumps);

std::atomic<std::uint32_t> gFeatureMask{kAllFeatures};

[[nodiscard]] bool TryResolve(
    const std::string_view name,
    Feature& feature) noexcept {
    if (name == "player-sync-observer") {
        feature = Feature::PlayerSyncObserver;
    } else if (name == "remote-movement-runtime") {
        feature = Feature::RemoteMovementRuntime;
    } else if (name == "worldsync-hooks") {
        feature = Feature::WorldSyncHooks;
    } else if (name == "character-lifecycle") {
        feature = Feature::CharacterLifecycleCapture;
    } else if (name == "engine-evidence") {
        feature = Feature::EngineThreadEvidence;
    } else if (name == "native-diagnostics") {
        feature = Feature::NativeDiagnostics;
    } else if (name == "locomotion-flight") {
        feature = Feature::LocomotionFlightRecorder;
    } else if (name == "game-thread-pumps") {
        feature = Feature::GameThreadPumps;
    } else {
        return false;
    }
    return true;
}

} // namespace

bool Enabled(const Feature feature) noexcept {
    return (gFeatureMask.load(std::memory_order_acquire) &
        static_cast<std::uint32_t>(feature)) != 0U;
}

void SetEnabled(const Feature feature, const bool enabled) noexcept {
    const auto bit = static_cast<std::uint32_t>(feature);
    if (enabled) {
        gFeatureMask.fetch_or(bit, std::memory_order_acq_rel);
    } else {
        gFeatureMask.fetch_and(~bit, std::memory_order_acq_rel);
    }
}

bool SetByName(const std::string_view name, const bool enabled) noexcept {
    Feature feature{};
    if (!TryResolve(name, feature)) return false;
    SetEnabled(feature, enabled);
    return true;
}

bool EnabledByName(const std::string_view name) noexcept {
    Feature feature{};
    return TryResolve(name, feature) && Enabled(feature);
}

std::uint32_t Mask() noexcept {
    return gFeatureMask.load(std::memory_order_acquire);
}

} // namespace aimp::runtime_features
