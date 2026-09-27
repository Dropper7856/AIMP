#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace aimp::crash_guard {

bool IsReadable(const void* address, std::size_t bytes) noexcept;
bool IsWritable(void* address, std::size_t bytes) noexcept;
bool TryCopy(void* destination, const void* source, std::size_t bytes) noexcept;
bool TryWrite(void* destination, const void* source, std::size_t bytes) noexcept;
bool InstallUnhandledExceptionRecorder() noexcept;
void UninstallUnhandledExceptionRecorder() noexcept;

bool IsInRange(
    const void* address,
    std::size_t bytes,
    const void* rangeBase,
    std::size_t rangeBytes) noexcept;

template <typename T>
std::optional<T> SafeRead(const void* address) noexcept {
    T value{};
    if (!TryCopy(&value, address, sizeof(T))) {
        return std::nullopt;
    }
    return value;
}

} // namespace aimp::crash_guard
