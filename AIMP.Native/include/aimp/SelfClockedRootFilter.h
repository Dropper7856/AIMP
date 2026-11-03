#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::world_interactions {

// A CATHODE output that fires many times per second (light flicker, oscillating
// comparators, per-frame polling) is a self-clocked local loop: every peer runs
// it from its own timers, so replicating each edge only floods the World bus
// and fights the receiver's own loop. Player and AI interactions never sustain
// this rate. The key returns to normal replication after a quiet period.
class SelfClockedRootFilter final {
public:
    static constexpr std::uint32_t kBurstLimit = 8U;
    static constexpr std::uint64_t kWindowMs = 1000U;
    static constexpr std::uint64_t kQuietReleaseMs = 5000U;

    enum class Decision : std::uint8_t { Replicate, BecameLocal, Local };

    [[nodiscard]] Decision Observe(const std::uint64_t key, const std::uint64_t nowMs) noexcept {
        if (key == 0U) return Decision::Replicate;
        Slot* selected = nullptr;
        for (std::size_t probe = 0U; probe < kProbeLimit; ++probe) {
            auto& slot = slots_[(key + probe) % kCapacity];
            if (slot.key == key) { selected = &slot; break; }
            if (selected == nullptr && (slot.key == 0U || nowMs - slot.lastMs > kQuietReleaseMs))
                selected = &slot;
        }
        // A full neighbourhood cannot prove a loop: keep the retail replication.
        if (selected == nullptr) return Decision::Replicate;
        auto& slot = *selected;
        if (slot.key != key) slot = Slot{key, nowMs, nowMs, 0U, false};
        if (slot.local) {
            const bool quiet = nowMs - slot.lastMs > kQuietReleaseMs;
            slot.lastMs = nowMs;
            if (!quiet) return Decision::Local;
            slot = Slot{key, nowMs, nowMs, 0U, false};
        }
        slot.lastMs = nowMs;
        if (nowMs - slot.windowStartMs >= kWindowMs) {
            slot.windowStartMs = nowMs;
            slot.count = 0U;
        }
        if (++slot.count <= kBurstLimit) return Decision::Replicate;
        slot.local = true;
        return Decision::BecameLocal;
    }

    void Clear() noexcept { slots_ = {}; }

private:
    struct Slot {
        std::uint64_t key{};
        std::uint64_t windowStartMs{};
        std::uint64_t lastMs{};
        std::uint32_t count{};
        bool local{};
    };
    static constexpr std::size_t kCapacity = 1024U;
    static constexpr std::size_t kProbeLimit = 8U;
    std::array<Slot, kCapacity> slots_{};
};

} // namespace aimp::world_interactions
