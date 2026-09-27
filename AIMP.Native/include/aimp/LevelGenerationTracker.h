#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace aimp {

enum class LevelStability : std::uint8_t {
    Unknown,
    Transition,
    Stable,
};

struct EngineRootObservation {
    std::uintptr_t globalRoot{};
    std::uintptr_t netz{};
    std::uintptr_t characterManager{};
    std::uintptr_t participantRegistry{};
    std::uintptr_t replicationContextPool{};

    bool operator==(const EngineRootObservation&) const = default;
};

struct LevelGenerationSnapshot {
    std::uint64_t generation{};
    LevelStability stability{LevelStability::Unknown};
    std::uint32_t stablePolls{};
    EngineRootObservation roots{};
};

class LevelGenerationTracker final {
public:
    static constexpr std::uint32_t kStablePollsRequired = 3;

    LevelGenerationSnapshot Observe(
        const EngineRootObservation& roots) noexcept;
    LevelGenerationSnapshot BeginTransition() noexcept;
    [[nodiscard]] LevelGenerationSnapshot Snapshot() const noexcept;
    [[nodiscard]] std::uint64_t CurrentGeneration() const noexcept;
    // Zero is never certified. A transition revokes this token BEFORE the
    // next generation is published, so hot readers need no snapshot mutex.
    [[nodiscard]] bool IsStableGeneration(std::uint64_t generation) const noexcept;

private:
    mutable std::mutex mutex_;
    EngineRootObservation roots_{};
    std::uint64_t generation_{};
    std::uint32_t stablePolls_{};
    LevelStability stability_{LevelStability::Unknown};
    std::atomic<std::uint64_t> currentGeneration_{0};
    std::atomic<std::uint64_t> stableGeneration_{0};
};

} // namespace aimp
