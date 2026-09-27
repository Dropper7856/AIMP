#include "aimp/NativeTransportAdapter.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"

namespace aimp {

NativeTransportAdapter::NativeTransportAdapter(
    Diagnostics& diagnostics) noexcept
    : diagnostics_(&diagnostics) {}

void NativeTransportAdapter::ReservedSlot00() noexcept {
    reservedSlotCalls_.fetch_add(1, std::memory_order_relaxed);
}

void NativeTransportAdapter::SendOpaqueFrame(
    const std::uint32_t logicalChannelId,
    const void* payload,
    const std::uint32_t payloadBytes,
    const std::int32_t streamFlag) noexcept {
    if (!TryAcquireSRWLockExclusive(&lock_)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Observation observation{};
    observation.logicalChannelId = logicalChannelId;
    observation.payloadBytes = payloadBytes;
    observation.streamFlag = streamFlag;
    observation.threadId = GetCurrentThreadId();
    if (payload != nullptr &&
        payloadBytes != 0 &&
        payloadBytes <= scratch_.size()) {
        observation.copied = crash_guard::TryCopy(
            scratch_.data(),
            payload,
            payloadBytes);
    }

    if (count_ == kSlots) {
        head_ = (head_ + 1) % kSlots;
        --count_;
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    observations_[(head_ + count_) % kSlots] = observation;
    ++count_;
    ReleaseSRWLockExclusive(&lock_);
}

void NativeTransportAdapter::DrainDiagnostics() {
    std::array<Observation, kSlots> local{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&lock_);
    count = count_;
    for (std::size_t index = 0; index < count; ++index) {
        local[index] = observations_[(head_ + index) % kSlots];
    }
    head_ = 0;
    count_ = 0;
    ReleaseSRWLockExclusive(&lock_);

    for (std::size_t index = 0; index < count; ++index) {
        const auto& observation = local[index];
        diagnostics_->Log(
            observation.copied ? "info" : "warning",
            "transport_observation",
            "Logger-only adapter observed an opaque call; no frame was interpreted or transmitted.",
            {{"logicalChannelId",
              Diagnostics::Unsigned(observation.logicalChannelId),
              true},
             {"payloadBytes",
              Diagnostics::Unsigned(observation.payloadBytes),
              true},
             {"streamFlag",
              std::to_string(observation.streamFlag),
              true},
             {"originThreadId",
              Diagnostics::Unsigned(observation.threadId),
              true},
             {"boundedCopySucceeded",
              Diagnostics::Boolean(observation.copied),
              true}});
    }

    const auto dropped = dropped_.exchange(0, std::memory_order_acq_rel);
    const auto reservedCalls =
        reservedSlotCalls_.exchange(0, std::memory_order_acq_rel);
    if (dropped != 0 || reservedCalls != 0) {
        diagnostics_->Log(
            "warning",
            "transport_observation_counters",
            "Transport adapter counters; the adapter is never attached by Phase 1 bootstrap.",
            {{"dropped", Diagnostics::Unsigned(dropped), true},
             {"reservedSlotCalls",
              Diagnostics::Unsigned(reservedCalls),
              true}});
    }
}

NativeTransportController::NativeTransportController(
    NativeTransportAdapter& adapter) noexcept
    : adapter_(&adapter) {}

bool NativeTransportController::Attach(
    const std::uintptr_t netzAddress,
    const TransportAttachGate& gate,
    std::string& error) {
    if (!gate.explicitOptIn ||
        !gate.exactBuildValidated ||
        !gate.signatureValidated ||
        !gate.gameThreadValidated ||
        !gate.netzLifetimeValidated ||
        gate.expectedNetzVtable == 0) {
        error = "all explicit transport research gates must be true";
        return false;
    }
    if (netzAddress == 0 ||
        adapter_ == nullptr ||
        attachedNetz_.load(std::memory_order_acquire) != 0) {
        error = "invalid NetZ address or adapter already attached";
        return false;
    }
    const auto vtable = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(netzAddress));
    if (!vtable.has_value() || *vtable != gate.expectedNetzVtable) {
        error = "NetZ vtable no longer matches the validated profile";
        return false;
    }

    auto* field = reinterpret_cast<void* volatile*>(netzAddress + 0x69C);
    if (!crash_guard::IsWritable(
            reinterpret_cast<void*>(netzAddress + 0x69C),
            sizeof(void*))) {
        error = "NetZ transport field is not writable";
        return false;
    }
    void* previous = InterlockedCompareExchangePointer(
        field,
        adapter_,
        nullptr);
    if (previous != nullptr) {
        error = "NetZ transport field already contains an unknown object";
        return false;
    }
    attachedNetz_.store(netzAddress, std::memory_order_release);
    return true;
}

bool NativeTransportController::Detach(
    const TransportAttachGate& gate,
    std::string& error) {
    if (!gate.explicitOptIn ||
        !gate.exactBuildValidated ||
        !gate.signatureValidated ||
        !gate.gameThreadValidated ||
        !gate.netzLifetimeValidated ||
        gate.expectedNetzVtable == 0) {
        error = "detach requires the same validated game-thread/lifetime gates";
        return false;
    }

    const auto netzAddress =
        attachedNetz_.load(std::memory_order_acquire);
    if (netzAddress == 0) {
        return true;
    }
    const auto vtable = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(netzAddress));
    if (!vtable.has_value() || *vtable != gate.expectedNetzVtable) {
        error = "NetZ vtable changed; detach refused to avoid a stale write";
        return false;
    }
    auto* field = reinterpret_cast<void* volatile*>(netzAddress + 0x69C);
    if (!crash_guard::IsWritable(
            reinterpret_cast<void*>(netzAddress + 0x69C),
            sizeof(void*))) {
        error = "NetZ transport field is no longer writable";
        return false;
    }
    void* previous = InterlockedCompareExchangePointer(
        field,
        nullptr,
        adapter_);
    if (previous != adapter_) {
        error = "transport field changed externally; no overwrite performed";
        return false;
    }
    attachedNetz_.store(0, std::memory_order_release);
    return true;
}

bool NativeTransportController::IsAttached() const noexcept {
    return attachedNetz_.load(std::memory_order_acquire) != 0;
}

} // namespace aimp
