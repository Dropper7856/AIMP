#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace aimp::ai_sync::movement {
// Native Move(1) contains a navigation block at +28, companion(3) at +1C.
// These are typed producer inputs, never a copy of either native structure.
// +94/+98 are FLOAT outputs of producer virtual+410 (0070AD0A..32), not
// producerState/Character pointers. +80..A0 maps block+58..78 geometry/query
// scalars. Native list links, refs, cursors and execution status are excluded.
struct Parameters {
    std::uint32_t retained{}, style{};
    std::array<float,3> goal{};
    float stoppingDistance{}, stopDistance{}; // Native -1 sentinel is allowed.
    std::array<float,9> query{};
    std::uint32_t flags90{}, flags91{}; // Producer-owned bits only.
    bool operator==(const Parameters&) const noexcept = default;
};
enum class Navigation : std::uint32_t { LocalNative = 1, HostRoute = 2 };
struct Bundle {
    std::uint32_t version{}, parts{}, gait{};
    Navigation navigation{};
    Parameters before{}, after{}, companion{};
    // Companion-specific request+B8 bits0..2 are OUTSIDE its navigation block.
    // 0070AE47..53 writes them before Append; 0070AE69 sets retained afterwards.
    std::uint32_t companionFlags{}, companionRetainedAfter{};
    std::uint32_t primaryFlags{}; // Standalone native Stop(3), request+B8 bits0..2.
    bool operator==(const Bundle&) const noexcept = default;
};
inline constexpr std::size_t kParametersBytes = 72, kBundleBytes = 244;
using CanonicalBundle = std::array<std::uint8_t,kBundleBytes>;
[[nodiscard]] bool Empty(const Bundle&) noexcept;
[[nodiscard]] bool Validate(const Bundle&) noexcept;
[[nodiscard]] bool Encode(const Bundle&, CanonicalBundle&) noexcept;
[[nodiscard]] bool Decode(std::span<const std::uint8_t>, Bundle&) noexcept;
[[nodiscard]] bool Same(const Bundle&, const Bundle&) noexcept;
[[nodiscard]] bool Same(const Parameters&, const Parameters&) noexcept;
} // namespace aimp::ai_sync::movement
