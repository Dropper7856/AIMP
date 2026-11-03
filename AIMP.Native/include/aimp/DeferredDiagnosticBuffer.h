#pragma once
#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

namespace aimp {
// Diagnostic records only. Gameplay admission/ordering never depends on this
// buffer. Producers copy fixed records; formatting and file I/O belong to Drain.
template<class Record, std::size_t Capacity>
class DeferredDiagnosticBuffer final {
public:
    bool Push(const Record& record) noexcept {
        std::lock_guard lock(mutex_);
        if (count_ == Capacity) { ++dropped_; return false; }
        records_[(head_ + count_) % Capacity] = record;
        ++count_;
        return true;
    }
    std::vector<Record> Drain() {
        std::vector<Record> result;
        result.reserve(Capacity);
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count_; ++i)
            result.push_back(records_[(head_ + i) % Capacity]);
        head_ = (head_ + count_) % Capacity;
        count_ = 0;
        return result;
    }
    std::size_t TakeDropped() noexcept {
        std::lock_guard lock(mutex_);
        const auto result = dropped_;
        dropped_ = 0;
        return result;
    }
private:
    std::mutex mutex_;
    std::array<Record, Capacity> records_{};
    std::size_t head_{}, count_{}, dropped_{};
};
}
