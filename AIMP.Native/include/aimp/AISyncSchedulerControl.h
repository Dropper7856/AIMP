#pragma once

#include <array>
#include <cstdint>

namespace aimp::ai_sync::scheduler_control {
// 00d6 AI.exe. These requests contain only the shared scheduler header.
// Their kind selects a native scheduler mode; it does not select an animation.
// Kind8 shares their vtable but has a spatial payload, so it is NOT admitted
// by vtable alone. Kind10 owns a different component/callback transaction.
struct Contract {
    std::uint32_t kind, factoryRva;
};
inline constexpr std::uint32_t kProfile = 256U;
inline constexpr std::uint32_t kTypeRva = 0x00EB199CU;
inline constexpr std::uint32_t kStatusRva = 0x00248BB0U; // RET 0C, no side effects.
inline constexpr std::array<Contract, 2> kContracts{{
    {6U, 0x002823D0U},
    {11U, 0x00282390U},
}};
[[nodiscard]] constexpr const Contract* Find(std::uint32_t kind) noexcept {
    for (const auto& contract : kContracts) if (contract.kind == kind) return &contract;
    return nullptr;
}
[[nodiscard]] constexpr bool Supported(std::uint32_t kind) noexcept { return Find(kind) != nullptr; }
} // namespace aimp::ai_sync::scheduler_control
