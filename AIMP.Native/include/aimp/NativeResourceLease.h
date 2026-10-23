#pragma once

#include "aimp/CrashGuard.h"
#include <Windows.h>
#include <cstdint>
#include <limits>

namespace aimp::native_resource {

// Retail shared-reference ABI (00402080/004460A0): {object, control},
// control+4 strong count, control+8 weak count, virtual destroy/delete at +0/+4.
// Engine thread only: the caller must already own the containing Character's
// lifetime. This pins the resource across synchronous engine callbacks; it is
// deliberately neither an object-lifetime cache nor a Character lease.
class StrongLease final {
public:
    explicit StrongLease(const std::uintptr_t reference, const std::size_t bytes) noexcept {
        Pair before{}, after{};
        if (!crash_guard::TryCopy(&before, reinterpret_cast<const void*>(reference), sizeof(before)) ||
            before.object == 0U || before.control == 0U ||
            !crash_guard::IsReadable(reinterpret_cast<const void*>(before.object), bytes) ||
            !crash_guard::IsWritable(reinterpret_cast<void*>(before.control + 4U), sizeof(LONG))) return;
        auto* count = reinterpret_cast<volatile LONG*>(before.control + 4U);
        LONG value = InterlockedCompareExchange(count, 0, 0);
        // Never resurrect an expired reference or overflow the native counter.
        while (value > 0 && value < (std::numeric_limits<LONG>::max)()) {
            const LONG previous = InterlockedCompareExchange(count, value + 1, value);
            if (previous == value) {
                pair_ = before;
                if (!crash_guard::TryCopy(&after, reinterpret_cast<const void*>(reference), sizeof(after)) ||
                    after.object != before.object || after.control != before.control) Reset();
                return;
            }
            value = previous;
        }
    }
    ~StrongLease() { Reset(); }
    StrongLease(const StrongLease&) = delete;
    StrongLease& operator=(const StrongLease&) = delete;
    explicit operator bool() const noexcept { return pair_.object != 0U; }
    std::uintptr_t Object() const noexcept { return pair_.object; }
private:
    struct Pair { std::uintptr_t object{}, control{}; } pair_;
    void Reset() noexcept {
        if (pair_.control == 0U) return;
        const auto control = pair_.control;
        pair_ = {};
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(control + 4U)) != 0) return;
        const auto table = *reinterpret_cast<const std::uintptr_t*>(control);
        reinterpret_cast<void(__thiscall*)(void*)>(
            *reinterpret_cast<const std::uintptr_t*>(table))(reinterpret_cast<void*>(control));
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(control + 8U)) == 0)
            reinterpret_cast<void(__thiscall*)(void*)>(
                *reinterpret_cast<const std::uintptr_t*>(
                    *reinterpret_cast<const std::uintptr_t*>(control) + 4U))(reinterpret_cast<void*>(control));
    }
};

} // namespace aimp::native_resource
