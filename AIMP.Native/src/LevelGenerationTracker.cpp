#include "aimp/LevelGenerationTracker.h"

#include <algorithm>
#include <limits>

namespace aimp {
namespace {

bool Coherent(const EngineRootObservation& roots) noexcept {
    return roots.globalRoot != 0 &&
        roots.netz != 0 &&
        roots.characterManager != 0 &&
        roots.participantRegistry != 0 &&
        roots.replicationContextPool != 0;
}

} // namespace

LevelGenerationSnapshot LevelGenerationTracker::Observe(
    const EngineRootObservation& roots) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (roots != roots_) {
            stableGeneration_.store(0U, std::memory_order_release);
            roots_ = roots;
            stablePolls_ = 1;
            ++generation_;
            currentGeneration_.store(generation_, std::memory_order_release);
            stability_ = LevelStability::Transition;
        } else if (stablePolls_ <
                   (std::numeric_limits<std::uint32_t>::max)()) {
            ++stablePolls_;
        }
        if (Coherent(roots_) &&
            stablePolls_ >= kStablePollsRequired) {
            stability_ = LevelStability::Stable;
        } else if (generation_ == 0) {
            stability_ = LevelStability::Unknown;
        } else {
            stability_ = LevelStability::Transition;
        }
        stableGeneration_.store(
            stability_ == LevelStability::Stable ? generation_ : 0U,
            std::memory_order_release);
        return {generation_, stability_, stablePolls_, roots_};
    } catch (...) {
        return Snapshot();
    }
}

LevelGenerationSnapshot LevelGenerationTracker::BeginTransition() noexcept {
    try {
        std::lock_guard lock(mutex_);
        stableGeneration_.store(0U, std::memory_order_release);
        roots_ = {};
        stablePolls_ = 0;
        ++generation_;
        currentGeneration_.store(generation_, std::memory_order_release);
        stability_ = LevelStability::Transition;
        return {generation_, stability_, stablePolls_, roots_};
    } catch (...) {
        return Snapshot();
    }
}

LevelGenerationSnapshot LevelGenerationTracker::Snapshot() const noexcept {
    try {
        std::lock_guard lock(mutex_);
        return {generation_, stability_, stablePolls_, roots_};
    } catch (...) {
        return {};
    }
}

std::uint64_t LevelGenerationTracker::CurrentGeneration() const noexcept {
    return currentGeneration_.load(std::memory_order_acquire);
}

bool LevelGenerationTracker::IsStableGeneration(const std::uint64_t generation) const noexcept {
    return generation != 0U &&
        stableGeneration_.load(std::memory_order_acquire) == generation &&
        currentGeneration_.load(std::memory_order_acquire) == generation &&
        stableGeneration_.load(std::memory_order_acquire) == generation;
}

} // namespace aimp
