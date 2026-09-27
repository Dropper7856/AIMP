#include "aimp/WorldAnimatedAdapters.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace aimp::world_animated {
namespace {

template <typename T>
void WriteLittle(std::byte* destination, const T value) noexcept {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    const auto raw = static_cast<U>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        destination[index] = static_cast<std::byte>((raw >> (index * 8U)) & 0xFFU);
    }
}

template <typename T>
[[nodiscard]] T ReadLittle(const std::byte* source) noexcept {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    U value{};
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<U>(std::to_integer<unsigned int>(source[index])) << (index * 8U);
    }
    return static_cast<T>(value);
}

[[nodiscard]] bool ValidDoorAction(const DoorAction action) noexcept {
    switch (action) {
    case DoorAction::Open:
    case DoorAction::Close:
    case DoorAction::Lock:
    case DoorAction::Unlock:
    case DoorAction::ForceOpen:
    case DoorAction::ForceClose:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool ValidAnimatedCategory(const AnimatedTargetCategory category) noexcept {
    return category == AnimatedTargetCategory::PlayEnvironmentAnimation ||
        category == AnimatedTargetCategory::AnimatedModelAttachmentNode;
}

} // namespace

bool EncodeDoorActionPayload(
    const DoorActionPayload& value,
    WorldPayload& output) noexcept {
    output = {};
    if (!ValidDoorAction(value.action) || value.observationId == 0U) return false;
    output.size = static_cast<std::uint8_t>(kDoorActionPayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(value.action);
    WriteLittle<std::uint64_t>(output.bytes.data() + 8U, value.observationId);
    return true;
}

bool DecodeDoorActionPayload(
    const std::span<const std::byte> payload,
    DoorActionPayload& output) noexcept {
    output = {};
    if (payload.size() != kDoorActionPayloadBytes ||
        payload[0] != static_cast<std::byte>(kPayloadVersion)) {
        return false;
    }
    for (std::size_t index = 2U; index < 8U; ++index) {
        if (payload[index] != std::byte{0}) return false;
    }
    output.action = static_cast<DoorAction>(std::to_integer<std::uint8_t>(payload[1]));
    output.observationId = ReadLittle<std::uint64_t>(payload.data() + 8U);
    return ValidDoorAction(output.action) && output.observationId != 0U;
}

bool EncodeAnimatedInputEdgePayload(
    const AnimatedInputEdge& value,
    WorldPayload& output) noexcept {
    output = {};
    if (!ValidAnimatedCategory(value.targetCategory) ||
        value.schedulerParameterShortGuid == 0U || value.targetInputShortGuid == 0U ||
        value.observationId == 0U || !value.sourceEntity.Valid() ||
        !std::isfinite(value.relativeDelaySeconds) || value.relativeDelaySeconds < 0.0F ||
        value.relativeDelaySeconds > 600.0F) {
        return false;
    }
    output.size = static_cast<std::uint8_t>(kAnimatedInputEdgePayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(1U);
    output.bytes[2] = static_cast<std::byte>(value.targetCategory);
    WriteLittle<std::uint32_t>(output.bytes.data() + 4U, value.schedulerParameterShortGuid);
    WriteLittle<std::uint32_t>(output.bytes.data() + 8U, value.targetInputShortGuid);
    WriteLittle<std::uint64_t>(output.bytes.data() + 16U, value.observationId);
    WriteLittle<std::uint64_t>(output.bytes.data() + 24U, value.sourceEntity.high);
    WriteLittle<std::uint64_t>(output.bytes.data() + 32U, value.sourceEntity.low);
    WriteLittle<std::uint32_t>(
        output.bytes.data() + 40U,
        std::bit_cast<std::uint32_t>(value.relativeDelaySeconds));
    return true;
}

bool DecodeAnimatedInputEdgePayload(
    const std::span<const std::byte> payload,
    AnimatedInputEdge& output) noexcept {
    output = {};
    if (payload.size() != kAnimatedInputEdgePayloadBytes ||
        payload[0] != static_cast<std::byte>(kPayloadVersion) ||
        payload[1] != static_cast<std::byte>(1U) ||
        payload[3] != std::byte{0} ||
        ReadLittle<std::uint32_t>(payload.data() + 12U) != 0U ||
        ReadLittle<std::uint32_t>(payload.data() + 44U) != 0U) {
        return false;
    }
    output.targetCategory = static_cast<AnimatedTargetCategory>(
        std::to_integer<std::uint8_t>(payload[2]));
    output.schedulerParameterShortGuid = ReadLittle<std::uint32_t>(payload.data() + 4U);
    output.targetInputShortGuid = ReadLittle<std::uint32_t>(payload.data() + 8U);
    output.observationId = ReadLittle<std::uint64_t>(payload.data() + 16U);
    output.sourceEntity.high = ReadLittle<std::uint64_t>(payload.data() + 24U);
    output.sourceEntity.low = ReadLittle<std::uint64_t>(payload.data() + 32U);
    output.relativeDelaySeconds = std::bit_cast<float>(
        ReadLittle<std::uint32_t>(payload.data() + 40U));
    return ValidAnimatedCategory(output.targetCategory) &&
        output.schedulerParameterShortGuid != 0U && output.targetInputShortGuid != 0U &&
        output.observationId != 0U && output.sourceEntity.Valid() &&
        std::isfinite(output.relativeDelaySeconds) && output.relativeDelaySeconds >= 0.0F &&
        output.relativeDelaySeconds <= 600.0F;
}

} // namespace aimp::world_animated
