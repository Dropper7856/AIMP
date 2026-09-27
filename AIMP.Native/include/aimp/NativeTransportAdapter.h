#pragma once

#include "aimp/Common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <Windows.h>

namespace aimp {

class Diagnostics;

struct TransportAttachGate {
    bool explicitOptIn{};
    bool exactBuildValidated{};
    bool signatureValidated{};
    bool gameThreadValidated{};
    bool netzLifetimeValidated{};
    std::uintptr_t expectedNetzVtable{};
};

class NativeTransportAdapter {
public:
    explicit NativeTransportAdapter(Diagnostics& diagnostics) noexcept;
    virtual void ReservedSlot00() noexcept;
    virtual void SendOpaqueFrame(
        std::uint32_t logicalChannelId,
        const void* payload,
        std::uint32_t payloadBytes,
        std::int32_t streamFlag) noexcept;

    void DrainDiagnostics();

    [[nodiscard]] static constexpr ModuleStatus Status() noexcept {
        return ModuleStatus::PartiallyImplemented;
    }

private:
    struct Observation {
        std::uint32_t logicalChannelId{};
        std::uint32_t payloadBytes{};
        std::int32_t streamFlag{};
        std::uint32_t threadId{};
        bool copied{};
    };

    static constexpr std::size_t kSlots = 32;
    Diagnostics* diagnostics_{};
    SRWLOCK lock_{SRWLOCK_INIT};
    std::array<Observation, kSlots> observations_{};
    std::size_t head_{};
    std::size_t count_{};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> reservedSlotCalls_{0};
    std::array<std::byte, kMaximumTransportObservationPayload> scratch_{};
};

class NativeTransportController final {
public:
    explicit NativeTransportController(NativeTransportAdapter& adapter) noexcept;

    bool Attach(
        std::uintptr_t netzAddress,
        const TransportAttachGate& gate,
        std::string& error);
    bool Detach(
        const TransportAttachGate& gate,
        std::string& error);

    [[nodiscard]] bool IsAttached() const noexcept;

private:
    NativeTransportAdapter* adapter_{};
    std::atomic<std::uintptr_t> attachedNetz_{0};
};

} // namespace aimp
