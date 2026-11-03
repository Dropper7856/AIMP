#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aimp::world_interactions {

inline constexpr std::uint64_t kObservationMultiplier = 0x9E3779B97F4A7C15ULL;
inline constexpr std::uint64_t kObservationInverse = 0xF1DE83E19937733DULL;
static_assert(kObservationMultiplier * kObservationInverse == 1U);

// IDs remain opaque on the wire. Recovering their process-local serial lets
// consecutive pending observations share one interval, rather than consuming
// one fixed hash slot each. No live observation is evicted or timed out.
constexpr std::uint64_t ObservationSerial(std::uint64_t id, std::uint64_t salt) noexcept {
    return (id ^ salt) * kObservationInverse;
}

template <std::size_t Capacity>
class LocalObservationLedger final {
public:
    [[nodiscard]] bool Contains(std::uint64_t serial) const noexcept {
        const auto index = Find(serial);
        return serial != 0U && index < size_ && ranges_[index].first <= serial;
    }

    [[nodiscard]] bool Remember(std::uint64_t serial) noexcept {
        if (serial == 0U) return false;
        const auto index = Find(serial);
        if (index < size_ && ranges_[index].first <= serial) return true;
        const bool left = index != 0U && ranges_[index - 1U].last != UINT64_MAX &&
            ranges_[index - 1U].last + 1U == serial;
        const bool right = index < size_ && serial != UINT64_MAX &&
            serial + 1U == ranges_[index].first;
        if (left && right) {
            ranges_[index - 1U].last = ranges_[index].last;
            Erase(index);
        } else if (left) {
            ranges_[index - 1U].last = serial;
        } else if (right) {
            ranges_[index].first = serial;
        } else {
            if (size_ == Capacity) return false;
            Insert(index, {serial, serial});
        }
        ++pending_;
        highWater_ = (std::max)(pending_, highWater_);
        return true;
    }

    [[nodiscard]] bool Consume(std::uint64_t serial) noexcept {
        const auto index = Find(serial);
        if (serial == 0U || index == size_ || ranges_[index].first > serial) return false;
        auto& range = ranges_[index];
        if (range.first == range.last) Erase(index);
        else if (serial == range.first) ++range.first;
        else if (serial == range.last) --range.last;
        else {
            // Retain the exact observation if fragmentation prevents a split.
            // Losing either neighboring live interval would permit double replay.
            if (size_ == Capacity) { ++splitRefused_; return false; }
            const auto last = range.last;
            range.last = serial - 1U;
            Insert(index + 1U, {serial + 1U, last});
        }
        --pending_;
        return true;
    }

    void Clear() noexcept { size_ = 0U; pending_ = highWater_ = splitRefused_ = 0U; }
    [[nodiscard]] std::uint64_t Pending() const noexcept { return pending_; }
    [[nodiscard]] std::uint64_t HighWater() const noexcept { return highWater_; }
    [[nodiscard]] std::uint64_t SplitRefused() const noexcept { return splitRefused_; }
    [[nodiscard]] std::size_t Ranges() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t OldestSerial() const noexcept { return size_ == 0U ? 0U : ranges_[0].first; }

private:
    struct Range { std::uint64_t first{}, last{}; };
    std::array<Range, Capacity> ranges_{};
    std::size_t size_{};
    std::uint64_t pending_{}, highWater_{}, splitRefused_{};
    [[nodiscard]] std::size_t Find(std::uint64_t serial) const noexcept {
        return static_cast<std::size_t>(std::lower_bound(ranges_.begin(), ranges_.begin() + size_, serial,
            [](const Range& range, std::uint64_t value) { return range.last < value; }) - ranges_.begin());
    }
    void Erase(std::size_t index) noexcept {
        for (auto i = index + 1U; i < size_; ++i) ranges_[i - 1U] = ranges_[i];
        --size_;
    }
    void Insert(std::size_t index, Range range) noexcept {
        for (auto i = size_; i > index; --i) ranges_[i] = ranges_[i - 1U];
        ranges_[index] = range;
        ++size_;
    }
};
} // namespace aimp::world_interactions
