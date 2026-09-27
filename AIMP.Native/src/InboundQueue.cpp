#include "aimp/InboundQueue.h"

#include <utility>

namespace aimp {

InboundQueue::InboundQueue(const InboundQueueLimits limits) : limits_(limits) {}

bool InboundQueue::Enqueue(InboundItem item) {
    const auto itemBytes = item.payload.size();
    std::lock_guard lock(mutex_);
    if (itemBytes > limits_.maximumPayload) {
        ++rejectedOversize_;
        return false;
    }
    if (queue_.size() >= limits_.maximumItems ||
        itemBytes > limits_.maximumBytes ||
        bytes_ > limits_.maximumBytes - itemBytes) {
        ++rejectedCapacity_;
        return false;
    }

    bytes_ += itemBytes;
    queue_.push_back(std::move(item));
    ++accepted_;
    return true;
}

bool InboundQueue::TryPop(InboundItem& output) {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    output = std::move(queue_.front());
    bytes_ -= output.payload.size();
    queue_.pop_front();
    return true;
}

void InboundQueue::Clear() noexcept {
    std::lock_guard lock(mutex_);
    cleared_ += queue_.size();
    queue_.clear();
    bytes_ = 0;
}

InboundQueueStats InboundQueue::Stats() const noexcept {
    std::lock_guard lock(mutex_);
    return {
        queue_.size(),
        bytes_,
        accepted_,
        rejectedOversize_,
        rejectedCapacity_,
        cleared_};
}

} // namespace aimp
