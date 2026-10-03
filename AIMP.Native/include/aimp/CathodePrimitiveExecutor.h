#pragma once

#include "aimp/Common.h"
#include "aimp/WorldReplicationTypes.h"

#include <cstdint>
#include <span>
#include <string>

namespace aimp {

class SignatureResolver;
class WorldEntityLease;
struct CathodeRuntimeKey;

namespace cathode_primitives {

inline constexpr std::uint32_t kOperationTriggerInput = 0x000C0501U;
inline constexpr std::uint8_t kPayloadVersion = 1U;
inline constexpr std::size_t kTriggerInputPayloadBytes = 24U;

enum class PrimitiveKind : std::uint8_t {
    TriggerInput = 1,
};

enum PrimitivePayloadFlags : std::uint8_t {
    PrimitivePayloadFlagNone = 0U,
    PrimitivePayloadFlagSourceEntity = 1U << 0U,
};

struct TriggerInputRequest final {
    std::uint32_t inputShortGuid{};
    bool hasSourceEntity{};
    WorldEntityId sourceEntity{};
};

enum class ExecuteStatus : std::uint8_t {
    Queued = 0,
    UnsupportedOperation,
    InvalidPayload,
    RuntimeUnavailable,
    NativeFault,
};

struct ExecuteResult final {
    ExecuteStatus status{ExecuteStatus::RuntimeUnavailable};
    bool queued{};
};

struct RuntimeDiagnostics final {
    bool initialized{};
    std::uint64_t version{};
    std::uint64_t initializationCount{};
    std::uint64_t shutdownCount{};
    std::uint64_t executeAttempts{};
    std::uint64_t queuedInputs{};
    std::uint64_t invalidPayloads{};
    std::uint64_t unsupportedOperations{};
    std::uint64_t runtimeUnavailable{};
    std::uint64_t nativeFaults{};
    std::uint64_t implicitSourceFallbacks{};
    std::uint64_t typedCharacterEventsEncoded{}, typedCharacterEventsRebuilt{}, typedCharacterEventsRejected{};
    std::uint32_t lastTypedCharacterEventKind{};
};

[[nodiscard]] constexpr ModuleReport Report() noexcept {
    return {
        "WorldSync2.CathodePrimitives",
        ModuleStatus::Implemented,
        "v12.8 executes two evidence-backed generic CATHODE primitives through retail APIs: input edges via FUN_009271C0 and typed message/blob queueing via FUN_00930640. No object-specific gameplay adapter is required."};
}

bool Initialize(const SignatureResolver& resolver, std::string& error) noexcept;
[[nodiscard]] bool Initialized() noexcept;
[[nodiscard]] RuntimeDiagnostics DiagnosticsSnapshot() noexcept;

[[nodiscard]] bool EncodeTriggerInputPayload(
    const TriggerInputRequest& request,
    WorldPayload& output) noexcept;
[[nodiscard]] bool DecodeTriggerInputPayload(
    std::span<const std::byte> payload,
    TriggerInputRequest& output) noexcept;

[[nodiscard]] ExecuteResult Execute(
    std::uint32_t operation,
    std::span<const std::byte> payload,
    const WorldEntityLease& target,
    const WorldEntityLease* source = nullptr) noexcept;

// Internal evidence-backed path shared with v12.6 adapters. It still uses the
// exact retail builder/scheduler and requires an owned source + target lease.
[[nodiscard]] ExecuteResult ExecuteInputEdge(
    std::uint32_t schedulerParameterShortGuid,
    std::uint32_t targetInputShortGuid,
    float relativeDelaySeconds,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept;

// Narrow dynamic-object variant used when AIMP already owns retained retail
// shared-refs that do not belong to the static WorldEntityRegistry (for
// example, a freshly created NPC equipment Composite). It uses the same
// FUN_00921F70 -> FUN_009271C0 retail path and never caches either pointer.
[[nodiscard]] ExecuteResult ExecuteInputEdgeSharedRefs(
    std::uint32_t schedulerParameterShortGuid,
    std::uint32_t targetInputShortGuid,
    float relativeDelaySeconds,
    void* targetSharedRef,
    void* sourceSharedRef) noexcept;

// Fix3.20 narrow constructed-callback helper. Retail constructs the callback
// work item with FUN_00926FF0 from a separately resolved callback execution
// shared-ref, retains it for the scheduler, queues it through FUN_009271C0
// using the edge source shared-ref, then releases only the constructor owner.
[[nodiscard]] ExecuteResult ExecuteInputEdgeWithConstructedCallback(
    std::uint32_t schedulerParameterShortGuid,
    std::uint32_t targetInputShortGuid,
    std::uint32_t callbackConstructorFlags,
    std::uint32_t callbackPayloadSize,
    float relativeDelaySeconds,
    const WorldEntityLease& target,
    const WorldEntityLease& source,
    const WorldEntityLease& callback) noexcept;

// WS3 v9: constructor-owned lease for one reconstructed retained WorkItem frame. FUN_00926FF0 is
// generic: it retains any CATHODE source shared-ref and copies arbitrary callback
// payload bytes. Public input borrows this holder; native descendants retain it.
class NativeCallbackLease final {
public:
    NativeCallbackLease() noexcept = default;
    ~NativeCallbackLease();
    NativeCallbackLease(const NativeCallbackLease&) = delete;
    NativeCallbackLease& operator=(const NativeCallbackLease&) = delete;
    NativeCallbackLease(NativeCallbackLease&& other) noexcept;
    NativeCallbackLease& operator=(NativeCallbackLease&& other) noexcept;
    [[nodiscard]] void* Get() const noexcept { return value_; }
    [[nodiscard]] bool Reset() noexcept;
private:
    friend ExecuteStatus CreateNativeCallback(const WorldEntityLease&, std::uint32_t,
        std::span<const std::byte>, NativeCallbackLease&) noexcept;
    friend ExecuteStatus CreateNativeContextFrame(const WorldEntityLease&, std::uint32_t,
        std::uint32_t, std::uint32_t, std::span<const std::byte>,
        const NativeCallbackLease*, NativeCallbackLease&) noexcept;
    void* value_{};
    void* release_{};
};
[[nodiscard]] ExecuteStatus CreateNativeCallback(const WorldEntityLease& context,
    std::uint32_t constructorFlags, std::span<const std::byte> payload,
    NativeCallbackLease& output) noexcept;
// WS3 retained ScheduledInput replay. Capture occurs at retail consumer
// FUN_00926680 after FUN_00926A60 selected the holder. Replay therefore keeps
// 00926680 as the boundary, requires retail's same-holder shape, installs the
// reconstructed WorkItem time, preserves manager trigger_object (+0x5C), and
// fails closed for the non-portable scheduler debug latch (+0x10A).
[[nodiscard]] bool ExecuteRetainedInput(const WorldEntityLease& owner,
    const WorldEntityLease& target, const NativeCallbackLease& work) noexcept;
[[nodiscard]] ExecuteStatus CreateNativeContextFrame(const WorldEntityLease& context,
    std::uint32_t inputShortGuid, std::uint32_t constructorFlags,
    std::uint32_t descriptorDelayBits, std::span<const std::byte> payload,
    const NativeCallbackLease* retainedParent, NativeCallbackLease& output) noexcept;

// Message 6 carries a polymorphic Character event value, not an opaque blob.
// Replace its certified local vptr with a wire tag on a COPY of native data.
// ExecuteTypedMessage rebuilds that vptr locally before retail owns the copy.
// Unsupported nonempty message-6 layouts remain local to their origin.
[[nodiscard]] bool PrepareTypedMessageForNetwork(
    std::uint32_t messageType, std::span<std::byte> copiedData) noexcept;

[[nodiscard]] ExecuteResult ExecuteTypedMessage(
    std::uint32_t messageType,
    std::span<const std::byte> data,
    double relativeDelaySeconds,
    const CathodeRuntimeKey& targetKey,
    const CathodeRuntimeKey& sourceKey) noexcept;

void BeginLogicalShutdown() noexcept;

} // namespace cathode_primitives
} // namespace aimp
