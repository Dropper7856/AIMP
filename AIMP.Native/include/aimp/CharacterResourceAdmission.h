#pragma once

#include "aimp/NativeResourceLease.h"

namespace aimp::character_direct_step_runtime {

// 006618C0 obtains this exact reference through 00402080, then reads +10..28.
// Hold on the engine thread across a remote native movement/stop transition.
class MovementResourceLease final {
public:
    explicit MovementResourceLease(const std::uintptr_t character) noexcept
        : resource_(character == 0U ? 0U : character + 0x42A8U, 0x2CU) {}
    explicit operator bool() const noexcept { return static_cast<bool>(resource_); }
private:
    native_resource::StrongLease resource_;
};

// Admission hint only. Callers executing a callback-capable native operation
// must hold MovementResourceLease for that operation instead of caching true.
inline bool HasLiveMovementResources(const std::uintptr_t character) noexcept {
    const MovementResourceLease resources(character);
    return static_cast<bool>(resources);
}

} // namespace aimp::character_direct_step_runtime
