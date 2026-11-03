#pragma once
#include <cstddef>
#include <deque>
#include <vector>

namespace aimp {
// Complete wire frames may be concatenated on the pipe's byte stream.
inline std::size_t TakeIpcWriteBatch(std::deque<std::vector<std::byte>>& frames,
    std::size_t& queuedBytes, std::vector<std::byte>& bytes,
    const std::size_t maximumFrames, const std::size_t maximumBytes) {
    bytes.clear();
    std::size_t count{};
    while (count < maximumFrames && !frames.empty()) {
        const auto& frame = frames.front();
        if (frame.size() > maximumBytes - bytes.size()) break;
        bytes.insert(bytes.end(), frame.begin(), frame.end());
        queuedBytes -= frame.size();
        frames.pop_front();
        ++count;
    }
    return count;
}
}
