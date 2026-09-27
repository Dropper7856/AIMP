#pragma once

#include "aimp/WorldReplicationTypes.h"

#include <cstddef>
#include <span>
#include <vector>

namespace aimp::world_replication_codec {

[[nodiscard]] bool EncodeIntent(
    const WorldIntent& value,
    std::vector<std::byte>& output) noexcept;
[[nodiscard]] bool DecodeIntent(
    std::span<const std::byte> payload,
    WorldIntent& output) noexcept;

[[nodiscard]] bool EncodeMutation(
    const WorldMutation& value,
    std::vector<std::byte>& output) noexcept;
[[nodiscard]] bool DecodeMutation(
    std::span<const std::byte> payload,
    WorldMutation& output) noexcept;

[[nodiscard]] bool EncodeResult(
    const WorldMutationResult& value,
    std::vector<std::byte>& output) noexcept;
[[nodiscard]] bool DecodeResult(
    std::span<const std::byte> payload,
    WorldMutationResult& output) noexcept;

} // namespace aimp::world_replication_codec
