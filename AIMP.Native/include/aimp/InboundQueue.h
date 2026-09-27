#pragma once

#include "aimp/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace aimp {

struct InboundItem {
    protocol::MessageType type{protocol::MessageType::InboundNetzFrame};
    std::uint16_t flags{};
    std::uint64_t sequence{};
    std::uint64_t levelGeneration{};
    std::vector<std::byte> payload;
};

struct InboundQueueLimits {
    std::size_t maximumItems{256};
    std::size_t maximumBytes{4U * 1024U * 1024U};
    std::size_t maximumPayload{kMaximumIpcPayload};
};

struct InboundQueueStats {
    std::size_t items{};
    std::size_t bytes{};
    std::uint64_t accepted{};
    std::uint64_t rejectedOversize{};
    std::uint64_t rejectedCapacity{};
    std::uint64_t cleared{};
};

class InboundQueue final {
public:
    explicit InboundQueue(InboundQueueLimits limits = {});

    bool Enqueue(InboundItem item);
    bool TryPop(InboundItem& output);
    void Clear() noexcept;
    [[nodiscard]] InboundQueueStats Stats() const noexcept;

private:
    InboundQueueLimits limits_;
    mutable std::mutex mutex_;
    std::deque<InboundItem> queue_;
    std::size_t bytes_{};
    std::uint64_t accepted_{};
    std::uint64_t rejectedOversize_{};
    std::uint64_t rejectedCapacity_{};
    std::uint64_t cleared_{};
};

} // namespace aimp
