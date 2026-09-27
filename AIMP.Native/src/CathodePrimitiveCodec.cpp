#include "aimp/CathodePrimitiveExecutor.h"

namespace aimp::cathode_primitives {
namespace {

void StoreU32(std::byte* destination, const std::uint32_t value) noexcept {
    destination[0] = static_cast<std::byte>(value & 0xFFU);
    destination[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    destination[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    destination[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void StoreU64(std::byte* destination, const std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        destination[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t LoadU32(const std::byte* source) noexcept {
    return std::to_integer<std::uint32_t>(source[0]) |
        (std::to_integer<std::uint32_t>(source[1]) << 8U) |
        (std::to_integer<std::uint32_t>(source[2]) << 16U) |
        (std::to_integer<std::uint32_t>(source[3]) << 24U);
}

[[nodiscard]] std::uint64_t LoadU64(const std::byte* source) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(source[index])) << (index * 8U);
    }
    return value;
}

} // namespace

bool EncodeTriggerInputPayload(
    const TriggerInputRequest& request,
    WorldPayload& output) noexcept {
    if (request.inputShortGuid == 0U ||
        (request.hasSourceEntity && !request.sourceEntity.Valid()) ||
        (!request.hasSourceEntity && request.sourceEntity.Valid())) {
        return false;
    }

    output = {};
    output.size = static_cast<std::uint16_t>(kTriggerInputPayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(PrimitiveKind::TriggerInput);
    output.bytes[2] = static_cast<std::byte>(request.hasSourceEntity
        ? PrimitivePayloadFlagSourceEntity
        : PrimitivePayloadFlagNone);
    output.bytes[3] = std::byte{0};
    StoreU32(output.bytes.data() + 4U, request.inputShortGuid);
    StoreU64(output.bytes.data() + 8U, request.sourceEntity.high);
    StoreU64(output.bytes.data() + 16U, request.sourceEntity.low);
    return true;
}

bool DecodeTriggerInputPayload(
    const std::span<const std::byte> payload,
    TriggerInputRequest& output) noexcept {
    output = {};
    if (payload.size() != kTriggerInputPayloadBytes ||
        std::to_integer<std::uint8_t>(payload[0]) != kPayloadVersion ||
        std::to_integer<std::uint8_t>(payload[1]) !=
            static_cast<std::uint8_t>(PrimitiveKind::TriggerInput) ||
        payload[3] != std::byte{0}) {
        return false;
    }

    const auto flags = std::to_integer<std::uint8_t>(payload[2]);
    if ((flags & static_cast<std::uint8_t>(~PrimitivePayloadFlagSourceEntity)) != 0U) {
        return false;
    }

    output.inputShortGuid = LoadU32(payload.data() + 4U);
    output.hasSourceEntity = (flags & PrimitivePayloadFlagSourceEntity) != 0U;
    output.sourceEntity.high = LoadU64(payload.data() + 8U);
    output.sourceEntity.low = LoadU64(payload.data() + 16U);
    if (output.inputShortGuid == 0U) return false;
    if (output.hasSourceEntity) return output.sourceEntity.Valid();
    return !output.sourceEntity.Valid();
}

} // namespace aimp::cathode_primitives
