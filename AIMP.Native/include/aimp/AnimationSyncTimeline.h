#pragma once
#include "aimp/AnimationSync.h"
#include <algorithm>
#include <cmath>

namespace aimp::animation_sync::timeline {
void Initialize(std::uintptr_t base) noexcept;
// Engine-thread only. Rejects graph types outside the verified scripted chain.
bool Read(std::uintptr_t graph, float& phase, float& duration) noexcept;
bool Seek(std::uintptr_t graph, float phase) noexcept;

// Native clocks are absolute clip seconds; start/end restrict the repeat range.
// loops is the current cycle+A8 remaining count; -1 is indefinite, 0 completed.
inline bool Project(const State& state, std::uint64_t ageMs,
    float& phase, std::int32_t& remainingLoops) noexcept {
    if (!Valid(state) || !Active(state) || state.loops == 0) return false;
    const double start = std::max(0.0, static_cast<double>(state.startFrame) / 30.0);
    const double end = state.endFrame > 0
        ? std::min(static_cast<double>(state.durationSeconds), static_cast<double>(state.endFrame) / 30.0)
        : state.durationSeconds;
    const double length = end - start;
    if (!(length > 0.0)) return false;
    const double current = std::max(start, static_cast<double>(state.phaseSeconds));
    const double projected = current + static_cast<double>(ageMs) * 0.001 * state.speed;
    const double completed = std::floor(std::max(0.0, projected - start) / length);
    if (state.loops > 0 && completed >= state.loops) return false;
    remainingLoops = state.loops < 0 ? -1 : state.loops - static_cast<std::int32_t>(completed);
    phase = static_cast<float>(start + std::fmod(std::max(0.0, projected - start), length));
    return std::isfinite(phase);
}
}
