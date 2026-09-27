#include "aimp/WorldInteractionAdapters.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace aimp::world_interactions {
namespace {

void StoreU16(std::byte* destination, const std::uint16_t value) noexcept {
    destination[0] = static_cast<std::byte>(value & 0xFFU);
    destination[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

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

[[nodiscard]] std::uint16_t LoadU16(const std::byte* source) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(source[0]) |
        (std::to_integer<std::uint16_t>(source[1]) << 8U));
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

[[nodiscard]] bool ValidCategory(const SourceCategory category) noexcept {
    // v12.8 active wire is intentionally object-agnostic. Historical numeric
    // categories remain declared only so old source can be read, but they are
    // not accepted in a v12.8 session.
    return category == SourceCategory::GenericWorld;
}

[[nodiscard]] bool ValidDelayDouble(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 600.0;
}

[[nodiscard]] bool ValidDelay(const float value) noexcept {
    // Retail graph delays are relative. Keep the network representation bounded
    // so a corrupt observation cannot enqueue an effectively immortal action.
    return std::isfinite(value) && value >= 0.0F && value <= 600.0F;
}

} // namespace

bool EncodeObservedInputEdgePayload(
    const ObservedInputEdge& value,
    WorldPayload& output) noexcept {
    if (!ValidCategory(value.sourceCategory) ||
        value.schedulerParameterShortGuid == 0U || value.targetInputShortGuid == 0U ||
        value.observationId == 0U || !value.sourceEntity.Valid() ||
        !ValidDelay(value.relativeDelaySeconds)) {
        return false;
    }

    output = {};
    output.size = static_cast<std::uint16_t>(kObservedInputEdgePayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kObservedInputEdgeKind);
    output.bytes[2] = static_cast<std::byte>(value.sourceCategory);
    output.bytes[3] = std::byte{0};
    StoreU32(output.bytes.data() + 4U, value.schedulerParameterShortGuid);
    StoreU32(output.bytes.data() + 8U, value.targetInputShortGuid);
    StoreU32(output.bytes.data() + 12U, 0U);
    StoreU64(output.bytes.data() + 16U, value.observationId);
    StoreU64(output.bytes.data() + 24U, value.sourceEntity.high);
    StoreU64(output.bytes.data() + 32U, value.sourceEntity.low);
    StoreU32(
        output.bytes.data() + 40U,
        std::bit_cast<std::uint32_t>(value.relativeDelaySeconds));
    StoreU32(output.bytes.data() + 44U, 0U);
    return true;
}

bool DecodeObservedInputEdgePayload(
    const std::span<const std::byte> payload,
    ObservedInputEdge& output) noexcept {
    output = {};
    if (payload.size() != kObservedInputEdgePayloadBytes ||
        std::to_integer<std::uint8_t>(payload[0]) != kPayloadVersion ||
        std::to_integer<std::uint8_t>(payload[1]) != kObservedInputEdgeKind ||
        payload[3] != std::byte{0} || LoadU32(payload.data() + 12U) != 0U ||
        LoadU32(payload.data() + 44U) != 0U) {
        return false;
    }

    output.sourceCategory = static_cast<SourceCategory>(
        std::to_integer<std::uint8_t>(payload[2]));
    output.schedulerParameterShortGuid = LoadU32(payload.data() + 4U);
    output.targetInputShortGuid = LoadU32(payload.data() + 8U);
    output.observationId = LoadU64(payload.data() + 16U);
    output.sourceEntity.high = LoadU64(payload.data() + 24U);
    output.sourceEntity.low = LoadU64(payload.data() + 32U);
    output.relativeDelaySeconds = std::bit_cast<float>(LoadU32(payload.data() + 40U));
    return ValidCategory(output.sourceCategory) &&
        output.schedulerParameterShortGuid != 0U && output.targetInputShortGuid != 0U &&
        output.observationId != 0U && output.sourceEntity.Valid() &&
        ValidDelay(output.relativeDelaySeconds);
}



bool EncodeObservedCallbackInputEdgePayload(
    const ObservedCallbackInputEdge& value,
    WorldPayload& output) noexcept {
    if (!ValidCategory(value.sourceCategory) ||
        value.schedulerParameterShortGuid == 0U || value.targetInputShortGuid == 0U ||
        value.callbackConstructorFlags != kCallbackReferenceConstructorFlags ||
        value.callbackPayloadSize != 0U ||
        value.callbackFamilyCallerRva != kCallbackReferenceFamilyCallerRva ||
        value.observationId == 0U || !value.sourceEntity.Valid() ||
        !ValidDelay(value.relativeDelaySeconds)) {
        return false;
    }

    output = {};
    output.size = static_cast<std::uint16_t>(kObservedCallbackInputEdgePayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kObservedCallbackInputEdgeKind);
    output.bytes[2] = static_cast<std::byte>(value.sourceCategory);
    output.bytes[3] = std::byte{0};
    StoreU32(output.bytes.data() + 4U, value.schedulerParameterShortGuid);
    StoreU32(output.bytes.data() + 8U, value.targetInputShortGuid);
    StoreU32(output.bytes.data() + 12U, value.callbackConstructorFlags);
    StoreU32(output.bytes.data() + 16U, value.callbackPayloadSize);
    StoreU32(output.bytes.data() + 20U, value.callbackFamilyCallerRva);
    StoreU64(output.bytes.data() + 24U, value.observationId);
    StoreU64(output.bytes.data() + 32U, value.sourceEntity.high);
    StoreU64(output.bytes.data() + 40U, value.sourceEntity.low);
    StoreU32(output.bytes.data() + 48U,
        std::bit_cast<std::uint32_t>(value.relativeDelaySeconds));
    StoreU32(output.bytes.data() + 52U, 0U);
    return true;
}

bool DecodeObservedCallbackInputEdgePayload(
    const std::span<const std::byte> payload,
    ObservedCallbackInputEdge& output) noexcept {
    output = {};
    if (payload.size() != kObservedCallbackInputEdgePayloadBytes ||
        std::to_integer<std::uint8_t>(payload[0]) != kPayloadVersion ||
        std::to_integer<std::uint8_t>(payload[1]) != kObservedCallbackInputEdgeKind ||
        payload[3] != std::byte{0} || LoadU32(payload.data() + 52U) != 0U) {
        return false;
    }

    output.sourceCategory = static_cast<SourceCategory>(
        std::to_integer<std::uint8_t>(payload[2]));
    output.schedulerParameterShortGuid = LoadU32(payload.data() + 4U);
    output.targetInputShortGuid = LoadU32(payload.data() + 8U);
    output.callbackConstructorFlags = LoadU32(payload.data() + 12U);
    output.callbackPayloadSize = LoadU32(payload.data() + 16U);
    output.callbackFamilyCallerRva = LoadU32(payload.data() + 20U);
    output.observationId = LoadU64(payload.data() + 24U);
    output.sourceEntity.high = LoadU64(payload.data() + 32U);
    output.sourceEntity.low = LoadU64(payload.data() + 40U);
    output.relativeDelaySeconds = std::bit_cast<float>(LoadU32(payload.data() + 48U));
    return ValidCategory(output.sourceCategory) &&
        output.schedulerParameterShortGuid != 0U && output.targetInputShortGuid != 0U &&
        output.callbackConstructorFlags == kCallbackReferenceConstructorFlags &&
        output.callbackPayloadSize == 0U &&
        output.callbackFamilyCallerRva == kCallbackReferenceFamilyCallerRva &&
        output.observationId != 0U && output.sourceEntity.Valid() &&
        ValidDelay(output.relativeDelaySeconds);
}

bool EncodeObservedCharacterCallbackInputEdgePayload(
    const ObservedCharacterCallbackInputEdge& value,
    WorldPayload& output) noexcept {
    if (value.sourceCategory != SourceCategory::GenericWorld ||
        value.schedulerParameterShortGuid == 0U || value.targetInputShortGuid == 0U ||
        value.callbackInputShortGuid != kCharacterCallbackInputShortGuid ||
        value.callbackConstructorFlags != kCharacterCallbackConstructorFlags ||
        value.callbackExecutionVtableRva != kCharacterCallbackExecutionVtableRva ||
        value.callbackFamilyCallerRva != kCharacterCallbackFamilyCallerRva ||
        value.observationId == 0U || !value.sourceEntity.Valid() ||
        !value.callbackEntity.Valid() || !ValidDelay(value.relativeDelaySeconds)) {
        return false;
    }
    output = {};
    output.size = static_cast<std::uint16_t>(kObservedCharacterCallbackInputEdgePayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kObservedCharacterCallbackInputEdgeKind);
    output.bytes[2] = static_cast<std::byte>(value.sourceCategory);
    output.bytes[3] = std::byte{0};
    StoreU32(output.bytes.data() + 4U, value.schedulerParameterShortGuid);
    StoreU32(output.bytes.data() + 8U, value.targetInputShortGuid);
    StoreU32(output.bytes.data() + 12U, value.callbackInputShortGuid);
    StoreU32(output.bytes.data() + 16U, value.callbackConstructorFlags);
    StoreU32(output.bytes.data() + 20U, value.callbackExecutionVtableRva);
    StoreU32(output.bytes.data() + 24U, value.callbackFamilyCallerRva);
    StoreU32(output.bytes.data() + 28U, 0U);
    StoreU64(output.bytes.data() + 32U, value.observationId);
    StoreU64(output.bytes.data() + 40U, value.sourceEntity.high);
    StoreU64(output.bytes.data() + 48U, value.sourceEntity.low);
    StoreU64(output.bytes.data() + 56U, value.callbackEntity.high);
    StoreU64(output.bytes.data() + 64U, value.callbackEntity.low);
    StoreU32(output.bytes.data() + 72U,
        std::bit_cast<std::uint32_t>(value.relativeDelaySeconds));
    StoreU32(output.bytes.data() + 76U, 0U);
    return true;
}

bool DecodeObservedCharacterCallbackInputEdgePayload(
    const std::span<const std::byte> payload,
    ObservedCharacterCallbackInputEdge& output) noexcept {
    output = {};
    if (payload.size() != kObservedCharacterCallbackInputEdgePayloadBytes ||
        std::to_integer<std::uint8_t>(payload[0]) != kPayloadVersion ||
        std::to_integer<std::uint8_t>(payload[1]) != kObservedCharacterCallbackInputEdgeKind ||
        payload[3] != std::byte{0} || LoadU32(payload.data() + 28U) != 0U ||
        LoadU32(payload.data() + 76U) != 0U) {
        return false;
    }
    output.sourceCategory = static_cast<SourceCategory>(
        std::to_integer<std::uint8_t>(payload[2]));
    output.schedulerParameterShortGuid = LoadU32(payload.data() + 4U);
    output.targetInputShortGuid = LoadU32(payload.data() + 8U);
    output.callbackInputShortGuid = LoadU32(payload.data() + 12U);
    output.callbackConstructorFlags = LoadU32(payload.data() + 16U);
    output.callbackExecutionVtableRva = LoadU32(payload.data() + 20U);
    output.callbackFamilyCallerRva = LoadU32(payload.data() + 24U);
    output.observationId = LoadU64(payload.data() + 32U);
    output.sourceEntity.high = LoadU64(payload.data() + 40U);
    output.sourceEntity.low = LoadU64(payload.data() + 48U);
    output.callbackEntity.high = LoadU64(payload.data() + 56U);
    output.callbackEntity.low = LoadU64(payload.data() + 64U);
    output.relativeDelaySeconds = std::bit_cast<float>(LoadU32(payload.data() + 72U));
    return output.sourceCategory == SourceCategory::GenericWorld &&
        output.schedulerParameterShortGuid != 0U && output.targetInputShortGuid != 0U &&
        output.callbackInputShortGuid == kCharacterCallbackInputShortGuid &&
        output.callbackConstructorFlags == kCharacterCallbackConstructorFlags &&
        output.callbackExecutionVtableRva == kCharacterCallbackExecutionVtableRva &&
        output.callbackFamilyCallerRva == kCharacterCallbackFamilyCallerRva &&
        output.observationId != 0U && output.sourceEntity.Valid() &&
        output.callbackEntity.Valid() && ValidDelay(output.relativeDelaySeconds);
}

bool EncodeObservedLogicSwitchStatePayload(
    const ObservedLogicSwitchState& value,
    WorldPayload& output) noexcept {
    if (value.observationId == 0U || value.contextRuntimeKey.entityA == 0U) return false;
    output = {};
    output.size = static_cast<std::uint16_t>(kObservedLogicSwitchStatePayloadBytes);
    output.bytes[0] = static_cast<std::byte>(kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kObservedLogicSwitchStateKind);
    output.bytes[2] = value.enabled ? std::byte{1} : std::byte{0};
    output.bytes[3] = std::byte{0};
    StoreU64(output.bytes.data() + 4U, value.observationId);
    StoreU32(output.bytes.data() + 12U, value.contextRuntimeKey.entityA);
    StoreU32(output.bytes.data() + 16U, value.contextRuntimeKey.entityB);
    StoreU32(output.bytes.data() + 20U, 0U);
    return true;
}

bool DecodeObservedLogicSwitchStatePayload(
    const std::span<const std::byte> payload,
    ObservedLogicSwitchState& output) noexcept {
    output = {};
    if (payload.size() != kObservedLogicSwitchStatePayloadBytes ||
        std::to_integer<std::uint8_t>(payload[0]) != kPayloadVersion ||
        std::to_integer<std::uint8_t>(payload[1]) != kObservedLogicSwitchStateKind ||
        std::to_integer<std::uint8_t>(payload[2]) > 1U ||
        payload[3] != std::byte{0} || LoadU32(payload.data() + 20U) != 0U) {
        return false;
    }
    output.enabled = std::to_integer<std::uint8_t>(payload[2]) != 0U;
    output.observationId = LoadU64(payload.data() + 4U);
    output.contextRuntimeKey.entityA = LoadU32(payload.data() + 12U);
    output.contextRuntimeKey.entityB = LoadU32(payload.data() + 16U);
    return output.observationId != 0U && output.contextRuntimeKey.entityA != 0U;
}

bool ValidPublicRootContext(const CompositePublicInputEvent& value) noexcept {
    if (value.rootKind != PublicRootKind::CompositeInput &&
        value.rootKind != PublicRootKind::EmittedOutput &&
        value.rootKind != PublicRootKind::ScheduledInput &&
        value.rootKind != PublicRootKind::SchedulerEdge &&
        value.rootKind != PublicRootKind::TypedMessage &&
        value.rootKind != PublicRootKind::CanonicalCompositeInput &&
        value.rootKind != PublicRootKind::DoorLockState)
        return false;
    const bool typedMessage = value.rootKind == PublicRootKind::TypedMessage;
    const bool canonicalComposite = value.rootKind == PublicRootKind::CanonicalCompositeInput;
    const bool doorLockState = value.rootKind == PublicRootKind::DoorLockState;
    if (value.rootKind != PublicRootKind::EmittedOutput ? value.interfaceSelector != 0U
        : (value.interfaceSelector != 1U && value.interfaceSelector != 2U)) return false;
    if (value.rootCount == 0U || value.rootCount > kMaximumTransactionRoots || value.rootOrdinal >= value.rootCount)
        return false;
    if (value.rootKind == PublicRootKind::SchedulerEdge) {
        if (value.schedulerParameterShortGuid == 0U ||
            value.contextKind != PublicContextKind::Autonomous || value.contextFrameCount != 0U ||
            value.contextPayloadSize != 0U || (value.flags & kPublicRootDetachedCallback) != 0U)
            return false;
    } else if (value.schedulerParameterShortGuid != 0U) {
        return false;
    }
    if (value.rootKind == PublicRootKind::ScheduledInput &&
        (value.contextKind != PublicContextKind::NativeContextChain || value.contextFrameCount == 0U ||
         value.contextFrames[0].inputShortGuid != value.publicInputShortGuid || value.relativeDelaySeconds != 0.0))
        return false;
    if (canonicalComposite &&
        (value.contextKind != PublicContextKind::Autonomous || value.contextFrameCount != 0U ||
         value.contextPayloadSize != 0U || value.relativeDelaySeconds != 0.0 ||
         value.flags != 0U))
        return false;
    if (doorLockState &&
        (value.contextKind != PublicContextKind::Autonomous || value.contextFrameCount != 0U ||
         value.contextPayloadSize != 0U || value.relativeDelaySeconds != 0.0 ||
         value.flags != 0U))
        return false;
    const bool actorEmpty = value.actorEntity.high == 0U && value.actorEntity.low == 0U;
    if (value.actorKind == PublicActorKind::WorldEntity) {
        if (!value.actorEntity.Valid()) return false;
    } else if ((value.actorKind != PublicActorKind::Authority &&
                value.actorKind != PublicActorKind::SourcePlayer) || !actorEmpty) return false;

    if (value.contextFrameCount > kCompositePublicRootMaximumContextDepth ||
        value.contextPayloadSize > kCompositePublicRootMaximumContextPayloadBytes)
        return false;

    // ROOT v12 certifies one bounded opaque payload class: the deep-copied
    // World->World FUN_00930640 typed-message blob. It is data, not a native
    // callback/context frame, and is remapped through stable endpoint identity.
    if (typedMessage) {
        return value.contextKind == PublicContextKind::Autonomous &&
            value.contextFrameCount == 0U &&
            (value.flags & kPublicRootDetachedCallback) == 0U;
    }

    if (value.contextPayloadSize != 0U) return false;
    if (value.contextKind == PublicContextKind::Autonomous)
        return value.contextFrameCount == 0U &&
            (value.flags & kPublicRootDetachedCallback) == 0U;
    if (value.contextKind != PublicContextKind::NativeContextChain ||
        value.contextFrameCount == 0U ||
        (value.flags & ~world_actions::kPublicRootFlagAwaitAuthority) != kPublicRootDetachedCallback)
        return false;

    std::size_t expectedOffset = 0U;
    for (std::size_t i = 0; i < value.contextFrameCount; ++i) {
        const auto& frame = value.contextFrames[i];
        const auto descriptorDelay = std::bit_cast<float>(frame.descriptorDelayBits);
        if (!std::isfinite(descriptorDelay) || descriptorDelay < 0.0F ||
            frame.runtimeKey.entityA == 0U || frame.inputShortGuid == 0U ||
            frame.invocation == 0U || frame.payloadOffset != expectedOffset ||
            static_cast<std::size_t>(frame.payloadOffset) + frame.payloadSize > value.contextPayloadSize)
            return false;
        expectedOffset += frame.payloadSize;
    }
    return expectedOffset == value.contextPayloadSize;
}

bool EncodeCompositePublicInputPayload(
    const CompositePublicInputEvent& value,
    WorldPayload& output) noexcept {
    if (value.observationId == 0U ||
        (value.publicInputShortGuid == 0U &&
            (!value.retailRoot || value.rootKind != PublicRootKind::TypedMessage)) ||
        (value.retailRoot
            ? (!ValidPublicRootContext(value) || !value.targetEntity.Valid() || !ValidDelayDouble(value.relativeDelaySeconds) ||
               (value.flags & ~kPublicRootKnownFlags) != 0U ||
               (value.flags & 3U) == 3U)
            : (value.targetEntity.Valid() || value.relativeDelaySeconds != 0.0 || value.flags != 0U))) {
        return false;
    }

    output = {};
    const auto descriptorBytes = static_cast<std::size_t>(value.contextFrameCount) *
        kCompositePublicRootContextFrameBytes;
    const auto rootSize = kCompositePublicRootHeaderBytes + descriptorBytes + value.contextPayloadSize;
    if (value.retailRoot && rootSize > kWorldPayloadMaximumBytes) return false;
    output.size = static_cast<std::uint16_t>(value.retailRoot
        ? rootSize : kCompositePublicInputPayloadBytes);
    output.bytes[0] = static_cast<std::byte>(value.retailRoot
        ? kCompositePublicRootPayloadVersion : kPayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kCompositePublicInputKind);
    output.bytes[2] = value.retailRoot ? static_cast<std::byte>(value.rootKind) : std::byte{0};
    output.bytes[3] = static_cast<std::byte>(value.flags);
    StoreU32(output.bytes.data() + 4U, value.publicInputShortGuid);
    StoreU64(output.bytes.data() + 8U, value.observationId);
    if (value.retailRoot) {
        StoreU64(output.bytes.data() + 16U, value.targetEntity.high);
        StoreU64(output.bytes.data() + 24U, value.targetEntity.low);
        StoreU64(output.bytes.data() + 32U,
            std::bit_cast<std::uint64_t>(value.relativeDelaySeconds));
        output.bytes[40] = static_cast<std::byte>(value.contextKind);
        output.bytes[41] = static_cast<std::byte>(value.actorKind);
        output.bytes[42] = static_cast<std::byte>(value.contextFrameCount);
        output.bytes[43] = static_cast<std::byte>(value.interfaceSelector);
        StoreU16(output.bytes.data() + 44U, value.contextPayloadSize);
        StoreU16(output.bytes.data() + 46U, static_cast<std::uint16_t>(descriptorBytes));
        StoreU64(output.bytes.data() + 48U, value.actorEntity.high);
        StoreU64(output.bytes.data() + 56U, value.actorEntity.low);
        if (value.causalSerial == 0U || value.segmentSerial == 0U) return false;
        StoreU64(output.bytes.data() + 64U, value.causalSerial);
        StoreU64(output.bytes.data() + 72U, value.segmentSerial);
        StoreU16(output.bytes.data() + 80U, value.rootOrdinal);
        StoreU16(output.bytes.data() + 82U, value.rootCount);
        StoreU32(output.bytes.data() + 84U, value.schedulerParameterShortGuid);
        StoreU64(output.bytes.data() + 88U, value.authorityTransaction);

        auto* descriptor = output.bytes.data() + kCompositePublicRootHeaderBytes;
        for (std::size_t i = 0; i < value.contextFrameCount; ++i) {
            const auto& frame = value.contextFrames[i];
            auto* bytes = descriptor + i * kCompositePublicRootContextFrameBytes;
            StoreU32(bytes + 0U, frame.runtimeKey.entityA);
            StoreU32(bytes + 4U, frame.runtimeKey.entityB);
            StoreU32(bytes + 8U, frame.constructorFlags);
            StoreU32(bytes + 12U, frame.inputShortGuid);
            StoreU64(bytes + 16U, frame.invocation);
            StoreU16(bytes + 24U, frame.payloadOffset);
            StoreU16(bytes + 26U, frame.payloadSize);
            StoreU32(bytes + 28U, frame.descriptorDelayBits);
        }
        if (value.contextPayloadSize != 0U) {
            const auto payloadOffset = kCompositePublicRootHeaderBytes + descriptorBytes;
            std::copy_n(value.contextPayload.begin(), value.contextPayloadSize,
                output.bytes.begin() + payloadOffset);
        }
    }
    return true;
}

bool DecodeCompositePublicInputPayload(
    const std::span<const std::byte> payload,
    CompositePublicInputEvent& output) noexcept {
    output = {};
    if (payload.size() < kCompositePublicInputPayloadBytes ||
        std::to_integer<std::uint8_t>(payload[1]) != kCompositePublicInputKind) {
        return false;
    }
    output.publicInputShortGuid = LoadU32(payload.data() + 4U);
    output.observationId = LoadU64(payload.data() + 8U);
    const auto version = std::to_integer<std::uint8_t>(payload[0]);
    if (version == kCompositePublicRootPayloadVersion ||
        version == kCompositePublicRootPreviousPayloadVersion) {
        if (payload.size() < kCompositePublicRootHeaderBytes || payload.size() > kWorldPayloadMaximumBytes)
            return false;
        output.targetEntity = {LoadU64(payload.data() + 16U), LoadU64(payload.data() + 24U)};
        output.relativeDelaySeconds = std::bit_cast<double>(LoadU64(payload.data() + 32U));
        output.flags = std::to_integer<std::uint8_t>(payload[3]);
        output.contextKind = static_cast<PublicContextKind>(payload[40]);
        output.actorKind = static_cast<PublicActorKind>(payload[41]);
        output.contextFrameCount = std::to_integer<std::uint8_t>(payload[42]);
        output.interfaceSelector = std::to_integer<std::uint8_t>(payload[43]);
        output.contextPayloadSize = LoadU16(payload.data() + 44U);
        const auto descriptorBytes = LoadU16(payload.data() + 46U);
        output.actorEntity = {LoadU64(payload.data() + 48U), LoadU64(payload.data() + 56U)};
        output.rootKind = static_cast<PublicRootKind>(payload[2]);
        const bool typedMessage = output.rootKind == PublicRootKind::TypedMessage;
        const bool canonicalComposite = output.rootKind == PublicRootKind::CanonicalCompositeInput;
        const bool doorLockState = output.rootKind == PublicRootKind::DoorLockState;
        if (((canonicalComposite || doorLockState) && version != kCompositePublicRootPayloadVersion) ||
            (!typedMessage && output.contextPayloadSize != 0U) ||
            output.contextPayloadSize > kCompositePublicRootMaximumContextPayloadBytes) return false;
        output.causalSerial = LoadU64(payload.data() + 64U);
        output.segmentSerial = LoadU64(payload.data() + 72U);
        output.rootOrdinal = LoadU16(payload.data() + 80U);
        output.rootCount = LoadU16(payload.data() + 82U);
        output.schedulerParameterShortGuid = LoadU32(payload.data() + 84U);
        output.authorityTransaction = LoadU64(payload.data() + 88U);
        if (output.causalSerial == 0U || output.segmentSerial == 0U ||
            output.contextFrameCount > kCompositePublicRootMaximumContextDepth ||
            descriptorBytes != static_cast<std::size_t>(output.contextFrameCount) * kCompositePublicRootContextFrameBytes)
            return false;
        const auto expectedSize = kCompositePublicRootHeaderBytes + descriptorBytes + output.contextPayloadSize;
        if (payload.size() != expectedSize || expectedSize > kWorldPayloadMaximumBytes)
            return false;

        const auto* descriptor = payload.data() + kCompositePublicRootHeaderBytes;
        for (std::size_t i = 0; i < output.contextFrameCount; ++i) {
            auto& frame = output.contextFrames[i];
            const auto* bytes = descriptor + i * kCompositePublicRootContextFrameBytes;
            frame.runtimeKey.entityA = LoadU32(bytes + 0U);
            frame.runtimeKey.entityB = LoadU32(bytes + 4U);
            frame.constructorFlags = LoadU32(bytes + 8U);
            frame.inputShortGuid = LoadU32(bytes + 12U);
            frame.invocation = LoadU64(bytes + 16U);
            frame.payloadOffset = LoadU16(bytes + 24U);
            frame.payloadSize = LoadU16(bytes + 26U);
            frame.descriptorDelayBits = LoadU32(bytes + 28U);
        }
        if (output.contextPayloadSize != 0U) {
            const auto payloadOffset = kCompositePublicRootHeaderBytes + descriptorBytes;
            std::copy_n(payload.begin() + payloadOffset,
                output.contextPayloadSize, output.contextPayload.begin());
        }
        if (!ValidPublicRootContext(output) || !output.targetEntity.Valid() ||
            !ValidDelayDouble(output.relativeDelaySeconds) ||
            (output.flags & ~kPublicRootKnownFlags) != 0U ||
            (output.flags & 3U) == 3U) return false;
        output.retailRoot = true;
    } else if (version != kPayloadVersion || payload.size() != kCompositePublicInputPayloadBytes ||
               payload[2] != std::byte{0} || payload[3] != std::byte{0}) return false;
    output.publicInputShortGuid = LoadU32(payload.data() + 4U);
    output.observationId = LoadU64(payload.data() + 8U);
    return output.observationId != 0U &&
        (output.publicInputShortGuid != 0U ||
         (output.retailRoot && output.rootKind == PublicRootKind::TypedMessage));
}

bool PrepareObservedTypedMessagePayload(
    const ObservedTypedMessage& value,
    const std::size_t dataLength,
    WorldPayload& output) noexcept {
    if (value.observationId == 0U ||
        value.sourceRuntimeKey.entityA == 0U || value.targetRuntimeKey.entityA == 0U ||
        !ValidDelayDouble(value.relativeDelaySeconds) ||
        dataLength > kObservedTypedMessageMaximumDataBytes) {
        return false;
    }
    const auto headerBytes = value.stableWorldIdentityIncluded
        ? kObservedTypedMessageIdentityHeaderBytes
        : kObservedTypedMessageHeaderBytes;
    if (value.stableWorldIdentityIncluded &&
        (!value.sourceWorldEntity.Valid() || !value.targetWorldEntity.Valid())) {
        return false;
    }
    const auto total = headerBytes + dataLength;
    if (total > kWorldPayloadMaximumBytes || total > 0xFFFFU) return false;

    output = {};
    output.size = static_cast<std::uint16_t>(total);
    output.bytes[0] = static_cast<std::byte>(value.stableWorldIdentityIncluded
        ? kObservedTypedMessageIdentityPayloadVersion
        : kObservedTypedMessagePayloadVersion);
    output.bytes[1] = static_cast<std::byte>(kObservedTypedMessageKind);
    output.bytes[2] = std::byte{0};
    output.bytes[3] = std::byte{0};
    StoreU32(output.bytes.data() + 4U, value.messageType);
    StoreU64(output.bytes.data() + 8U, value.observationId);
    StoreU32(output.bytes.data() + 16U, value.sourceRuntimeKey.entityA);
    StoreU32(output.bytes.data() + 20U, value.sourceRuntimeKey.entityB);
    StoreU32(output.bytes.data() + 24U, value.targetRuntimeKey.entityA);
    StoreU32(output.bytes.data() + 28U, value.targetRuntimeKey.entityB);
    StoreU64(
        output.bytes.data() + 32U,
        std::bit_cast<std::uint64_t>(value.relativeDelaySeconds));
    StoreU16(output.bytes.data() + 40U, static_cast<std::uint16_t>(dataLength));
    StoreU16(output.bytes.data() + 42U, 0U);
    StoreU32(output.bytes.data() + 44U, 0U);
    if (value.stableWorldIdentityIncluded) {
        StoreU64(output.bytes.data() + 48U, value.sourceWorldEntity.high);
        StoreU64(output.bytes.data() + 56U, value.sourceWorldEntity.low);
        StoreU64(output.bytes.data() + 64U, value.targetWorldEntity.high);
        StoreU64(output.bytes.data() + 72U, value.targetWorldEntity.low);
    }
    return true;
}

bool EncodeObservedTypedMessagePayload(
    const ObservedTypedMessage& value,
    const std::span<const std::byte> data,
    WorldPayload& output) noexcept {
    if (!PrepareObservedTypedMessagePayload(value, data.size(), output)) return false;
    const auto headerBytes = value.stableWorldIdentityIncluded
        ? kObservedTypedMessageIdentityHeaderBytes
        : kObservedTypedMessageHeaderBytes;
    std::copy(data.begin(), data.end(), output.bytes.begin() + headerBytes);
    return true;
}

bool DecodeObservedTypedMessagePayload(
    const std::span<const std::byte> payload,
    ObservedTypedMessage& output,
    std::span<const std::byte>& data) noexcept {
    output = {};
    data = {};
    if (payload.size() < kObservedTypedMessageHeaderBytes ||
        payload.size() > kWorldPayloadMaximumBytes ||
        std::to_integer<std::uint8_t>(payload[1]) != kObservedTypedMessageKind ||
        payload[2] != std::byte{0} || payload[3] != std::byte{0} ||
        LoadU16(payload.data() + 42U) != 0U || LoadU32(payload.data() + 44U) != 0U) {
        return false;
    }
    const auto version = std::to_integer<std::uint8_t>(payload[0]);
    std::size_t headerBytes = 0U;
    if (version == kObservedTypedMessageIdentityPayloadVersion) {
        headerBytes = kObservedTypedMessageIdentityHeaderBytes;
        if (payload.size() < headerBytes) return false;
        output.sourceWorldEntity.high = LoadU64(payload.data() + 48U);
        output.sourceWorldEntity.low = LoadU64(payload.data() + 56U);
        output.targetWorldEntity.high = LoadU64(payload.data() + 64U);
        output.targetWorldEntity.low = LoadU64(payload.data() + 72U);
        if (!output.sourceWorldEntity.Valid() || !output.targetWorldEntity.Valid()) return false;
        output.stableWorldIdentityIncluded = true;
    } else if (version == kObservedTypedMessagePayloadVersion) {
        // Normal Fix3.13-compatible v2 traffic remains unchanged and carries no
        // stable-ID extension.
        headerBytes = kObservedTypedMessageHeaderBytes;
    } else {
        return false;
    }
    const auto dataLength = static_cast<std::size_t>(LoadU16(payload.data() + 40U));
    if (dataLength > kObservedTypedMessageMaximumDataBytes ||
        payload.size() != headerBytes + dataLength) {
        return false;
    }
    output.messageType = LoadU32(payload.data() + 4U);
    output.observationId = LoadU64(payload.data() + 8U);
    output.sourceRuntimeKey.entityA = LoadU32(payload.data() + 16U);
    output.sourceRuntimeKey.entityB = LoadU32(payload.data() + 20U);
    output.targetRuntimeKey.entityA = LoadU32(payload.data() + 24U);
    output.targetRuntimeKey.entityB = LoadU32(payload.data() + 28U);
    output.relativeDelaySeconds = std::bit_cast<double>(LoadU64(payload.data() + 32U));
    if (output.observationId == 0U || output.sourceRuntimeKey.entityA == 0U ||
        output.targetRuntimeKey.entityA == 0U || !ValidDelayDouble(output.relativeDelaySeconds)) {
        return false;
    }
    data = payload.subspan(headerBytes, dataLength);
    return true;
}

} // namespace aimp::world_interactions
