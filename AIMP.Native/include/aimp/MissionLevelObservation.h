#pragma once

#include "aimp/CrashGuard.h"
#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace aimp::mission_level_observation {

// Selected level only: callers must separately fence loaded-level stability.
// Addresses refer to the researched x86 AI.exe at preferred base 0x400000.
inline std::string ReadSelectedLevelName(std::uintptr_t module) {
    constexpr std::uintptr_t preferredBase = 0x400000;
    constexpr std::uintptr_t runtimeRva = 0x016F0C88 - preferredBase;
    constexpr std::uintptr_t namesRva = 0x01BE1C4C - preferredBase;
    constexpr auto maxAddress = (std::numeric_limits<std::uint32_t>::max)();
    if (module == 0 || module > maxAddress - namesRva) return {};

    // Game pointers remain 32-bit, independent of the compiler's pointer width.
    const auto read = [](std::uintptr_t base, std::uintptr_t offset = 0)
        -> std::optional<std::uint32_t> {
        if (base == 0 || offset > maxAddress - (sizeof(std::uint32_t) - 1) ||
            base > maxAddress - (sizeof(std::uint32_t) - 1) - offset) return std::nullopt;
        return crash_guard::SafeRead<std::uint32_t>(
            reinterpret_cast<const void*>(base + offset));
    };

    // FUN_0078bbe0(*(DAT_016f0c88 + 0x3c)): ring cursor, record, index.
    const auto runtime = read(module, runtimeRva);
    if (!runtime) return {};
    const auto manager = read(*runtime, 0x3c);
    if (!manager) return {};
    const auto cursor = read(*manager, 0x44);
    if (!cursor || *cursor == maxAddress) return {};
    const auto record = read(*manager, (*cursor & 15u) * 4u);
    if (!record) return {};
    const auto index = read(*record, 4);

    // FUN_00a308e0 initializes 0xa0 records, stride 0x46; metadata starts
    // at 0x2bc0 == 0xa0 * 0x46. Negative indices also fail this unsigned bound.
    constexpr std::uint32_t levelCount = 0xa0;
    constexpr std::uintptr_t nameBytes = 0x46;
    if (!index || *index >= levelCount) return {};

    // FUN_00a2e810 returns *DAT_01be1c4c: global -> wrapper -> table.
    const auto wrapper = read(module, namesRva);
    if (!wrapper) return {};
    const auto names = read(*wrapper);
    if (!names || *names == 0) return {};
    const auto offset = *index * nameBytes;
    if (*names > maxAddress - offset - (nameBytes - 1)) return {};
    const auto bytes = crash_guard::SafeRead<std::array<char, nameBytes>>(
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(*names) + offset));
    if (!bytes) return {};
    for (std::size_t length = 0; length < bytes->size(); ++length) {
        if ((*bytes)[length] == '\0') return std::string(bytes->data(), length);
    }
    return {}; // Never publish a truncated name or scan into the next record.
}

} // namespace aimp::mission_level_observation
