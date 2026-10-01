#include "aimp/Protocol.h"

#include <algorithm>
#include <limits>

namespace aimp::protocol {
namespace {

void PutU16(std::byte* output, const std::uint16_t value) noexcept {
    output[0] = static_cast<std::byte>(value & 0xFFU);
    output[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void PutU32(std::byte* output, const std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void PutU64(std::byte* output, const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint16_t GetU16(const std::byte* input) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(input[0]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[1])) << 8U));
}

std::uint32_t GetU32(const std::byte* input) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[index]))
            << (index * 8U);
    }
    return value;
}

std::uint64_t GetU64(const std::byte* input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[index]))
            << (index * 8U);
    }
    return value;
}

} // namespace

bool IsKnownMessageType(const std::uint16_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::BuildInfo:
    case MessageType::SessionConfig:
    case MessageType::NetzReady:
    case MessageType::LevelChanged:
    case MessageType::PeerJoin:
    case MessageType::PeerLeave:
    case MessageType::PeerBind:
    case MessageType::OutboundNetzFrame:
    case MessageType::InboundNetzFrame:
    case MessageType::ResyncRequest:
    case MessageType::Error:
    case MessageType::Heartbeat:
    case MessageType::Shutdown:
    case MessageType::WorldIntent:
    case MessageType::WorldMutation:
    case MessageType::WorldMutationResult:
    case MessageType::WorldIntentRejected:
    case MessageType::WorldSyncLifecycle:
    case MessageType::WorldIntentAbandoned:
    case MessageType::PlayerSyncState:
    case MessageType::PlayerSyncAction:
    case MessageType::AISyncConfigure:
    case MessageType::AISyncState:
    case MessageType::TestModeConfigure:
    case MessageType::CapabilityQuery:
    case MessageType::CapabilityState:
    case MessageType::CaptureState:
    case MessageType::DummyCreate:
    case MessageType::DummyCreateResult:
    case MessageType::DummyRemove:
    case MessageType::DummyRemoveResult:
    case MessageType::DummyMove:
    case MessageType::DummyMoveResult:
    case MessageType::DummyBindingState:
    case MessageType::DiagnosticSnapshotRequest:
    case MessageType::DiagnosticSnapshotResult:
    case MessageType::ExperimentalError:
    case MessageType::LevelState:
    case MessageType::GameCommandExecute:
    case MessageType::GameCommandResult:
        return true;
    }
    return false;
}

bool EncodeFrame(
    const MessageType type,
    const std::uint16_t flags,
    const std::uint64_t sequence,
    const std::span<const std::byte> payload,
    std::vector<std::byte>& output) {
    if (!IsKnownMessageType(static_cast<std::uint16_t>(type)) ||
        flags != 0 ||
        payload.size() > kMaximumIpcPayload ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    output.resize(kWireHeaderBytes + payload.size());
    auto* cursor = output.data();
    PutU32(cursor, kMagic);
    PutU16(cursor + 4, kProtocolVersion);
    PutU16(cursor + 6, static_cast<std::uint16_t>(type));
    PutU16(cursor + 8, flags);
    PutU32(cursor + 10, static_cast<std::uint32_t>(payload.size()));
    PutU64(cursor + 14, sequence);
    std::copy(payload.begin(), payload.end(), cursor + kWireHeaderBytes);
    return true;
}

DecodeResult DecodeHeader(
    const std::span<const std::byte> input,
    MessageType& type,
    std::uint16_t& flags,
    std::uint32_t& payloadLength,
    std::uint64_t& sequence) {
    if (input.size() < kWireHeaderBytes) {
        return DecodeResult::NeedMoreData;
    }
    if (GetU32(input.data()) != kMagic) {
        return DecodeResult::InvalidMagic;
    }
    if (GetU16(input.data() + 4) != kProtocolVersion) {
        return DecodeResult::UnsupportedVersion;
    }

    const auto rawType = GetU16(input.data() + 6);
    if (!IsKnownMessageType(rawType)) {
        return DecodeResult::InvalidType;
    }

    flags = GetU16(input.data() + 8);
    if (flags != 0) {
        return DecodeResult::InvalidFlags;
    }
    payloadLength = GetU32(input.data() + 10);
    sequence = GetU64(input.data() + 14);
    type = static_cast<MessageType>(rawType);
    if (payloadLength > kMaximumIpcPayload) {
        return DecodeResult::PayloadTooLarge;
    }
    return DecodeResult::Complete;
}

DecodeResult DecodeFrame(
    const std::span<const std::byte> input,
    Frame& output,
    std::size_t& consumed) {
    consumed = 0;
    MessageType type{};
    std::uint16_t flags = 0;
    std::uint32_t payloadLength = 0;
    std::uint64_t sequence = 0;
    const auto headerResult =
        DecodeHeader(input, type, flags, payloadLength, sequence);
    if (headerResult != DecodeResult::Complete) {
        return headerResult;
    }

    const auto total = kWireHeaderBytes + static_cast<std::size_t>(payloadLength);
    if (input.size() < total) {
        return DecodeResult::NeedMoreData;
    }

    output.type = type;
    output.flags = flags;
    output.sequence = sequence;
    output.payload.assign(input.begin() + kWireHeaderBytes, input.begin() + total);
    consumed = total;
    return DecodeResult::Complete;
}

} // namespace aimp::protocol
