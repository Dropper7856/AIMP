#include "aimp/WorldReplicationCodec.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace aimp::world_replication_codec {
namespace {

constexpr std::uint32_t kMagic = 0x31525741U; // ASCII AWR1
constexpr std::uint8_t kIntentType = 1U;
constexpr std::uint8_t kMutationType = 2U;
constexpr std::uint8_t kResultType = 3U;
constexpr std::size_t kPrefixBytes = 8U;
constexpr std::size_t kIntentBaseBytes = 76U;
constexpr std::size_t kMutationBaseBytes = 96U;
constexpr std::size_t kResultBytes = 20U;

void AppendU16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void AppendU32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void AppendU64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

bool ReadU16(
    const std::span<const std::byte> input,
    std::size_t& offset,
    std::uint16_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1U])) << 8U);
    offset += 2U;
    return true;
}

bool ReadU32(
    const std::span<const std::byte> input,
    std::size_t& offset,
    std::uint32_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < 4U) return false;
    value = 0U;
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + index])) << (index * 8U);
    }
    offset += 4U;
    return true;
}

bool ReadU64(
    const std::span<const std::byte> input,
    std::size_t& offset,
    std::uint64_t& value) noexcept {
    if (offset > input.size() || input.size() - offset < 8U) return false;
    value = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(input[offset + index])) << (index * 8U);
    }
    offset += 8U;
    return true;
}

void AppendPrefix(std::vector<std::byte>& output, const std::uint8_t type) {
    AppendU32(output, kMagic);
    AppendU16(output, kWorldReplicationSchemaVersion);
    output.push_back(static_cast<std::byte>(type));
    output.push_back(std::byte{0});
}

bool ReadPrefix(
    const std::span<const std::byte> input,
    const std::uint8_t expectedType,
    std::size_t& offset) noexcept {
    if (input.size() < kPrefixBytes) return false;
    std::uint32_t magic = 0U;
    std::uint16_t schema = 0U;
    offset = 0U;
    if (!ReadU32(input, offset, magic) || !ReadU16(input, offset, schema)) return false;
    if (magic != kMagic || schema != kWorldReplicationSchemaVersion ||
        std::to_integer<std::uint8_t>(input[offset]) != expectedType ||
        std::to_integer<std::uint8_t>(input[offset + 1U]) != 0U) return false;
    offset += 2U;
    return true;
}

void AppendEntity(std::vector<std::byte>& output, const WorldEntityId& value) {
    AppendU64(output, value.high);
    AppendU64(output, value.low);
}

bool ReadEntity(
    const std::span<const std::byte> input,
    std::size_t& offset,
    WorldEntityId& value) noexcept {
    return ReadU64(input, offset, value.high) && ReadU64(input, offset, value.low);
}

void AppendManifest(
    std::vector<std::byte>& output,
    const WorldManifestIdentity& value) {
    AppendU32(output, value.formatVersion);
    AppendU32(output, value.compilerVersion);
    AppendEntity(output, value.mapContentId);
}

bool ReadManifest(
    const std::span<const std::byte> input,
    std::size_t& offset,
    WorldManifestIdentity& value) noexcept {
    return ReadU32(input, offset, value.formatVersion) &&
        ReadU32(input, offset, value.compilerVersion) &&
        ReadEntity(input, offset, value.mapContentId);
}

bool ValidKind(const WorldMutationKind kind, const bool allowIntent) noexcept {
    switch (kind) {
    case WorldMutationKind::Intent: return allowIntent;
    case WorldMutationKind::State:
    case WorldMutationKind::Event:
    case WorldMutationKind::Timeline: return true;
    default: return false;
    }
}

bool ValidFlags(const std::uint8_t flags) noexcept {
    constexpr auto kKnown = static_cast<std::uint8_t>(
        WorldMutationFlagReliable | WorldMutationFlagReplaceable |
        WorldMutationFlagReplayable | WorldMutationFlagSyntheticProbe);
    return (flags & static_cast<std::uint8_t>(~kKnown)) == 0U;
}

bool ValidIntent(const WorldIntent& value) noexcept {
    return value.manifest.Valid() && value.entity.Valid() &&
        value.levelGeneration != 0U && value.clientSequence != 0U &&
        value.operation != 0U && value.payload.Valid() &&
        ValidKind(value.requestedKind, false);
}

bool ValidMutation(const WorldMutation& value) noexcept {
    if (!value.manifest.Valid() || !value.entity.Valid() ||
        value.levelGeneration == 0U || value.sequence == 0U ||
        value.timelineTick == 0U || value.operation == 0U ||
        value.sourcePeer == 0U ||
        !value.payload.Valid() || !ValidKind(value.kind, false) ||
        !ValidFlags(value.flags) ||
        (value.flags & WorldMutationFlagReliable) == 0U) return false;
    const auto replayFlags = static_cast<std::uint8_t>(
        WorldMutationFlagReplaceable | WorldMutationFlagReplayable);
    if (value.kind == WorldMutationKind::Event) {
        return value.revision == 0U && (value.flags & replayFlags) == 0U;
    }
    return value.revision != 0U && (value.flags & replayFlags) == replayFlags;
}

bool AppendPayload(std::vector<std::byte>& output, const WorldPayload& value) {
    if (!value.Valid()) return false;
    AppendU16(output, value.size);
    output.insert(output.end(), value.bytes.begin(), value.bytes.begin() + value.size);
    return true;
}

bool ReadPayload(
    const std::span<const std::byte> input,
    std::size_t& offset,
    WorldPayload& value,
    const std::optional<std::uint16_t> knownLength = std::nullopt) noexcept {
    std::uint16_t length = 0U;
    if (knownLength.has_value()) {
        length = *knownLength;
    } else if (!ReadU16(input, offset, length)) {
        return false;
    }
    if (length > kWorldPayloadMaximumBytes || offset > input.size() ||
        input.size() - offset < length) return false;
    value = {};
    value.size = length;
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), length, value.bytes.begin());
    offset += length;
    return true;
}

} // namespace

bool EncodeIntent(const WorldIntent& value, std::vector<std::byte>& output) noexcept {
    try {
        output.clear();
        if (!ValidIntent(value)) return false;
        output.reserve(kIntentBaseBytes + value.payload.size);
        AppendPrefix(output, kIntentType);
        AppendManifest(output, value.manifest);
        AppendEntity(output, value.entity);
        AppendU32(output, value.levelGeneration);
        AppendU64(output, value.sourcePeer);
        AppendU64(output, value.clientSequence);
        output.push_back(static_cast<std::byte>(value.requestedKind));
        output.push_back(std::byte{0});
        AppendU32(output, value.operation);
        if (!AppendPayload(output, value.payload)) return false;
        return output.size() == kIntentBaseBytes + value.payload.size;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool DecodeIntent(const std::span<const std::byte> payload, WorldIntent& output) noexcept {
    output = {};
    std::size_t offset = 0U;
    if (!ReadPrefix(payload, kIntentType, offset) ||
        !ReadManifest(payload, offset, output.manifest) ||
        !ReadEntity(payload, offset, output.entity) ||
        !ReadU32(payload, offset, output.levelGeneration) ||
        !ReadU64(payload, offset, output.sourcePeer) ||
        !ReadU64(payload, offset, output.clientSequence) ||
        offset > payload.size() || payload.size() - offset < 2U) return false;
    output.requestedKind = static_cast<WorldMutationKind>(
        std::to_integer<std::uint8_t>(payload[offset++]));
    if (std::to_integer<std::uint8_t>(payload[offset++]) != 0U ||
        !ReadU32(payload, offset, output.operation) ||
        !ReadPayload(payload, offset, output.payload) ||
        offset != payload.size()) return false;
    return ValidIntent(output);
}

bool EncodeMutation(const WorldMutation& value, std::vector<std::byte>& output) noexcept {
    try {
        output.clear();
        if (!ValidMutation(value)) return false;
        output.reserve(kMutationBaseBytes + value.payload.size);
        AppendPrefix(output, kMutationType);
        AppendManifest(output, value.manifest);
        AppendEntity(output, value.entity);
        AppendU32(output, value.levelGeneration);
        output.push_back(static_cast<std::byte>(value.kind));
        output.push_back(static_cast<std::byte>(value.flags));
        AppendU16(output, 0U);
        AppendU64(output, value.sequence);
        AppendU64(output, value.revision);
        AppendU64(output, value.timelineTick);
        AppendU64(output, value.sourcePeer);
        AppendU32(output, value.operation);
        AppendU16(output, value.payload.size);
        AppendU16(output, 0U);
        output.insert(output.end(), value.payload.bytes.begin(),
            value.payload.bytes.begin() + value.payload.size);
        return output.size() == kMutationBaseBytes + value.payload.size;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool DecodeMutation(const std::span<const std::byte> payload, WorldMutation& output) noexcept {
    output = {};
    std::size_t offset = 0U;
    std::uint16_t reserved = 0U;
    std::uint16_t length = 0U;
    if (!ReadPrefix(payload, kMutationType, offset) ||
        !ReadManifest(payload, offset, output.manifest) ||
        !ReadEntity(payload, offset, output.entity) ||
        !ReadU32(payload, offset, output.levelGeneration) ||
        offset > payload.size() || payload.size() - offset < 2U) return false;
    output.kind = static_cast<WorldMutationKind>(
        std::to_integer<std::uint8_t>(payload[offset++]));
    output.flags = std::to_integer<std::uint8_t>(payload[offset++]);
    if (!ReadU16(payload, offset, reserved) || reserved != 0U ||
        !ReadU64(payload, offset, output.sequence) ||
        !ReadU64(payload, offset, output.revision) ||
        !ReadU64(payload, offset, output.timelineTick) ||
        !ReadU64(payload, offset, output.sourcePeer) ||
        !ReadU32(payload, offset, output.operation) ||
        !ReadU16(payload, offset, length) ||
        !ReadU16(payload, offset, reserved) || reserved != 0U ||
        !ReadPayload(payload, offset, output.payload, length) ||
        offset != payload.size()) return false;
    return ValidMutation(output);
}

bool EncodeResult(const WorldMutationResult& value, std::vector<std::byte>& output) noexcept {
    try {
        output.clear();
        if (value.sequence == 0U || (value.dispatched && !value.entityResolved) ||
            value.code > WorldMutationResultCode::NativeFault) return false;
        output.reserve(kResultBytes);
        AppendPrefix(output, kResultType);
        AppendU64(output, value.sequence);
        output.push_back(static_cast<std::byte>(value.code));
        output.push_back(static_cast<std::byte>(value.entityResolved ? 1U : 0U));
        output.push_back(static_cast<std::byte>(value.dispatched ? 1U : 0U));
        output.push_back(std::byte{0});
        return output.size() == kResultBytes;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool DecodeResult(const std::span<const std::byte> payload, WorldMutationResult& output) noexcept {
    output = {};
    std::size_t offset = 0U;
    if (!ReadPrefix(payload, kResultType, offset) ||
        !ReadU64(payload, offset, output.sequence) ||
        offset > payload.size() || payload.size() - offset != 4U) return false;
    const auto code = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto resolved = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto dispatched = std::to_integer<std::uint8_t>(payload[offset++]);
    const auto reserved = std::to_integer<std::uint8_t>(payload[offset++]);
    if (code > static_cast<std::uint8_t>(WorldMutationResultCode::NativeFault) ||
        resolved > 1U || dispatched > 1U || reserved != 0U) return false;
    output.code = static_cast<WorldMutationResultCode>(code);
    output.entityResolved = resolved != 0U;
    output.dispatched = dispatched != 0U;
    return output.sequence != 0U && (!output.dispatched || output.entityResolved);
}

} // namespace aimp::world_replication_codec
