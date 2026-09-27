#include "aimp/CathodePrimitiveExecutor.h"

#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldEntityRegistry.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <utility>

namespace aimp {

// Narrow bridge into the v12.3 owned shared-ref. It exists only in this
// translation unit so no general subsystem can accidentally cache the native
// reference outside the WorldEntityLease lifetime.
struct CathodePrimitiveLeaseAccess final {
    [[nodiscard]] static void* SharedRef(const WorldEntityLease& lease) noexcept {
        return lease.sharedRef_;
    }
};

namespace cathode_primitives {
namespace {

// STATIC CONFIRMED against the supplied retail AI.exe:
// FUN_009271C0 uses a 28-byte x86 stack contract (`ret 0x1C`). Retail
// call-sites include a trailing 32-bit argument that the function body does
// not read; it still MUST be present so callee cleanup preserves ESP.
// FUN_00921F70 builds the 12-byte CATHODE input target descriptor and retains
// the target shared-ref; FUN_009271C0 copies/schedules it through the CATHODE
// manager using the retail allocator/queue; FUN_004BF890 releases the temporary
// descriptor afterwards. FUN_0091D9F0 is also resolved as the downstream
// generic input dispatcher, but v12.5 deliberately does not call it directly.
using BuildInputTargetFn = void* (__thiscall*)(
    void* targetDescriptor,
    void** targetSharedRef,
    const std::uint32_t* inputShortGuid,
    double delaySeconds);
using QueueInputFn = void (__thiscall*)(
    void* manager,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* targetDescriptor,
    void* callbackRecord,
    double delaySeconds,
    std::uint32_t retailTailArgument);
using ReleaseInputTargetFn = void (__fastcall*)(void* targetDescriptor);
// WS3 v8: FUN_00926FF0 is STATIC CONFIRMED as the generic CATHODE callback
// constructor. It accepts arbitrary constructor flags and a byte payload which
// retail copies into its own MemoryAllocation<char>. FUN_0091E1E0/FUN_009252F0
// retain/release the constructor-owned WorkItem reference.
using BuildCallbackValueFn = void** (__thiscall*)(
    void* manager,
    void** destination,
    void** sourceSharedRef,
    std::uint32_t constructorFlags,
    std::size_t payloadSize,
    void* payloadData);
// FUN_00926D20 allocates one WorkItem from the retail pool and forwards to
// FUN_009265C0. The descriptor supplies input/delay/source while parent/payload
// are retained independently. This is the native primitive required to rebuild
// a retained ancestry rather than synthesizing links by writing WorkItem bytes.
using BuildRetainedWorkItemFn = void* (__thiscall*)(
    void* manager,
    void** sourceSharedRef,
    const std::uint32_t* inputShortGuid,
    void* inputDescriptor,
    const std::uint32_t* constructorFlags,
    const double* absoluteTime,
    void** parentValue,
    void** payloadSharedRef);
using EnterWorkItemCriticalSectionFn = void (__fastcall*)(void* ignoredMarker);
using LeaveWorkItemCriticalSectionFn = void (__cdecl*)();
using ExecuteWorkFn = std::uint8_t (__thiscall*)(void*, void**, void**, void**);
using AssignEntityRefFn = int* (__thiscall*)(void* destination, void** source);
using RetainCallbackValueFn = void (__fastcall*)(void* callbackValue);
using ReleaseCallbackValueFn = void (__fastcall*)(void* callbackValue);
using QueueTypedMessageFn = void (__thiscall*)(
    void* manager,
    const CathodeRuntimeKey* sourceKey,
    const CathodeRuntimeKey* targetKey,
    std::uint32_t messageType,
    std::size_t payloadSize,
    void* payloadData,
    double delaySeconds);

struct RetailInputTarget final {
    std::uint32_t inputShortGuid{};
    float delaySeconds{};
    void* targetSharedRef{};
};

static_assert(sizeof(void*) == 4, "CATHODE primitive ABI is x86-only.");
static_assert(sizeof(RetailInputTarget) == 12U, "Unexpected retail input-target layout.");

std::atomic<bool> gInitialized{false};
std::atomic<std::uint64_t> gVersion{0U};
std::atomic<std::uint64_t> gInitializationCount{0U};
std::atomic<std::uint64_t> gShutdownCount{0U};
std::atomic<std::uint64_t> gExecuteAttempts{0U};
std::atomic<std::uint64_t> gQueuedInputs{0U};
std::atomic<std::uint64_t> gInvalidPayloads{0U};
std::atomic<std::uint64_t> gUnsupportedOperations{0U};
std::atomic<std::uint64_t> gRuntimeUnavailable{0U};
std::atomic<std::uint64_t> gNativeFaults{0U};
std::atomic<std::uint64_t> gImplicitSourceFallbacks{0U};

const void* gManagerRootAddress{};
const void* gDispatchInputAddress{};
BuildInputTargetFn gBuildInputTarget{};
QueueInputFn gQueueInput{};
ReleaseInputTargetFn gReleaseInputTarget{};
BuildCallbackValueFn gBuildCallbackValue{};
BuildRetainedWorkItemFn gBuildRetainedWorkItem{};
EnterWorkItemCriticalSectionFn gEnterWorkItemCriticalSection{};
LeaveWorkItemCriticalSectionFn gLeaveWorkItemCriticalSection{};
RetainCallbackValueFn gRetainCallbackValue{};
ReleaseCallbackValueFn gReleaseCallbackValue{};
QueueTypedMessageFn gQueueTypedMessage{};
ExecuteWorkFn gExecuteWork{};
AssignEntityRefFn gAssignEntityRef{};

void TouchVersion() noexcept {
    gVersion.fetch_add(1U, std::memory_order_acq_rel);
}

bool InvokeReleaseSafely(
    const ReleaseInputTargetFn release,
    RetailInputTarget* descriptor) noexcept {
    if (release == nullptr || descriptor == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        release(descriptor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    release(descriptor);
    return true;
#endif
}

// Keep SEH isolated in a POD-only helper; MSVC rejects __try in functions that
// require C++ object unwinding (C2712). The temporary target descriptor owns
// one retail shared-ref after BuildInputTarget and is always released after the
// guarded call, including when QueueInput faults.
bool InvokeQueueSafely(
    void* manager,
    const BuildInputTargetFn build,
    const QueueInputFn queue,
    const ReleaseInputTargetFn release,
    void* targetSharedRef,
    void* sourceSharedRef,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const double relativeDelaySeconds) noexcept {
    if (manager == nullptr || build == nullptr || queue == nullptr || release == nullptr ||
        targetSharedRef == nullptr || sourceSharedRef == nullptr ||
        schedulerParameterShortGuid == 0U || targetInputShortGuid == 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0) {
        return false;
    }

    RetailInputTarget target{};
    void* targetShared = targetSharedRef;
    void* sourceShared = sourceSharedRef;
    bool invoked = false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        build(&target, &targetShared, &targetInputShortGuid, relativeDelaySeconds);
        queue(
            manager,
            &sourceShared,
            &schedulerParameterShortGuid,
            &target,
            nullptr,
            0.0,
            0U);
        invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        invoked = false;
    }
#else
    build(&target, &targetShared, &targetInputShortGuid, relativeDelaySeconds);
    queue(manager, &sourceShared, &schedulerParameterShortGuid, &target, nullptr, 0.0, 0U);
    invoked = true;
#endif

    // FUN_00921F70 increments the shared-ref stored at +8. FUN_009271C0 copies
    // that descriptor into a CATHODE-owned queued record, so the stack copy can
    // be released immediately, exactly like the retail call sites.
    if (target.targetSharedRef != nullptr && !InvokeReleaseSafely(release, &target)) {
        return false;
    }
    return invoked;
}

bool InvokeAssignEntityRefSafely(
    const AssignEntityRefFn assign,
    void** destination,
    void** source) noexcept {
    if (assign == nullptr || destination == nullptr || source == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        assign(destination, source);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    assign(destination, source);
    return true;
#endif
}

bool InvokeReleaseCallbackSafely(
    const ReleaseCallbackValueFn release,
    void* callbackValue) noexcept {
    if (release == nullptr || callbackValue == nullptr) return false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        release(callbackValue);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    release(callbackValue);
    return true;
#endif
}

bool InvokeQueueConstructedCallbackSafely(
    void* manager,
    const BuildInputTargetFn buildTarget,
    const QueueInputFn queue,
    const ReleaseInputTargetFn releaseTarget,
    const BuildCallbackValueFn buildCallback,
    const RetainCallbackValueFn retainCallback,
    const ReleaseCallbackValueFn releaseCallback,
    void* targetSharedRef,
    void* sourceSharedRef,
    void* callbackSharedRef,
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const std::uint32_t callbackConstructorFlags,
    const std::uint32_t callbackPayloadSize,
    const double relativeDelaySeconds) noexcept {
    if (manager == nullptr || buildTarget == nullptr || queue == nullptr ||
        releaseTarget == nullptr || buildCallback == nullptr || retainCallback == nullptr ||
        releaseCallback == nullptr || targetSharedRef == nullptr || sourceSharedRef == nullptr ||
        callbackSharedRef == nullptr || schedulerParameterShortGuid == 0U ||
        targetInputShortGuid == 0U ||
        callbackConstructorFlags != 0x12000000U || callbackPayloadSize != 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0) {
        return false;
    }

    RetailInputTarget target{};
    void* targetShared = targetSharedRef;
    void* sourceShared = sourceSharedRef;
    void* callbackShared = callbackSharedRef;
    void* callbackOwner = nullptr;
    bool queued = false;

#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        buildTarget(&target, &targetShared, &targetInputShortGuid, relativeDelaySeconds);
        buildCallback(
            manager,
            &callbackOwner,
            &callbackShared,
            callbackConstructorFlags,
            callbackPayloadSize,
            nullptr);
        if (callbackOwner != nullptr) {
            // Mirror retail ownership exactly: local owner + one queue retain.
            retainCallback(callbackOwner);
            queue(
                manager,
                &sourceShared,
                &schedulerParameterShortGuid,
                &target,
                callbackOwner,
                0.0,
                0U);
            queued = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        queued = false;
    }
#else
    buildTarget(&target, &targetShared, &targetInputShortGuid, relativeDelaySeconds);
    buildCallback(
        manager,
        &callbackOwner,
        &callbackShared,
        callbackConstructorFlags,
        callbackPayloadSize,
        nullptr);
    if (callbackOwner != nullptr) {
        retainCallback(callbackOwner);
        queue(manager, &sourceShared, &schedulerParameterShortGuid, &target, callbackOwner, 0.0, 0U);
        queued = true;
    }
#endif

    bool cleanupOk = true;
    if (target.targetSharedRef != nullptr && !InvokeReleaseSafely(releaseTarget, &target)) {
        cleanupOk = false;
    }
    if (callbackOwner != nullptr) {
        // Match the retail call site on the normal path: release only the
        // constructor/local owner after queueing.  On an SEH fault we cannot
        // prove whether FUN_009271C0 consumed the explicit retain before the
        // exception.  Prefer leaking that single exceptional-path retain over
        // a speculative second release that could double-free a scheduler-owned
        // callback.
        if (!InvokeReleaseCallbackSafely(releaseCallback, callbackOwner)) {
            cleanupOk = false;
        }
    }
    return queued && cleanupOk;
}

// Keep SEH in a POD-only helper (MSVC C2712). FUN_00926FF0 copies payload
// synchronously, so the caller's span only needs to remain valid for this call.
bool BuildNativeCallbackSafely(void* manager, void* shared,
    const std::uint32_t flags, const std::byte* payload, const std::size_t payloadSize,
    void*& value) noexcept {
    void* source = shared;
    void* bytes = payloadSize == 0U ? nullptr : const_cast<std::byte*>(payload);
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        gBuildCallbackValue(manager, &value, &source, flags, payloadSize, bytes);
        return value != nullptr;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}

struct RetailContextDescriptor final {
    std::uint32_t inputShortGuid{};
    std::uint32_t delayBits{};
    void* sourceSharedRef{};
};
static_assert(sizeof(RetailContextDescriptor) == 12U, "Unexpected retained-context descriptor layout.");

bool BuildRetainedContextSafely(void* manager, void* standaloneValue, void* retainedParent,
    const std::uint32_t inputShortGuid, const std::uint32_t descriptorDelayBits,
    void*& value) noexcept {
    if (manager == nullptr || standaloneValue == nullptr ||
        inputShortGuid == 0U || gBuildRetainedWorkItem == nullptr ||
        gEnterWorkItemCriticalSection == nullptr ||
        gLeaveWorkItemCriticalSection == nullptr ||
        gRetainCallbackValue == nullptr) return false;

    // STATIC CONFIRMED against the supplied retail AI.exe: every retail call
    // path into FUN_00926D20 is executed while lpCriticalSection_0174EF48 is
    // held, either directly or through FUN_0091E1B0/FUN_0091E1D0. 0.11.4.75
    // originally bypassed that contract and could corrupt the WorkItem pool,
    // producing a delayed crash after the reconstructed ROOT had already
    // completed. Reproduce the retail locking contract exactly here.
    bool entered = false;
    bool success = false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        gEnterWorkItemCriticalSection(nullptr);
        entered = true;

        const auto* words = static_cast<const std::uint32_t*>(standaloneValue);
        void* source = reinterpret_cast<void*>(words[2]);
        void* payloadShared = reinterpret_cast<void*>(words[4]);
        const auto flags = words[8];
        const auto absoluteTime = *reinterpret_cast<const double*>(
            reinterpret_cast<const std::byte*>(standaloneValue) + 0x18U);
        RetailContextDescriptor descriptor{};
        descriptor.inputShortGuid = inputShortGuid;
        descriptor.delayBits = descriptorDelayBits;
        descriptor.sourceSharedRef = source;
        void* parent = retainedParent;
        auto* built = gBuildRetainedWorkItem(manager, &source, &inputShortGuid,
            &descriptor, &flags, &absoluteTime, &parent, &payloadShared);
        if (built != nullptr) {
            gRetainCallbackValue(built); // constructor owner, released by NativeCallbackLease
            value = built;
            success = true;
        }
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        success = false;
        value = nullptr;
    }
#endif
    if (entered) gLeaveWorkItemCriticalSection();
    return success;
}

bool InvokeTypedMessageSafely(
    void* manager,
    const QueueTypedMessageFn queue,
    const CathodeRuntimeKey& sourceKey,
    const CathodeRuntimeKey& targetKey,
    const std::uint32_t messageType,
    const std::span<const std::byte> data,
    const double relativeDelaySeconds) noexcept {
    if (manager == nullptr || queue == nullptr || sourceKey.entityA == 0U ||
        targetKey.entityA == 0U || data.size() > kWorldPayloadMaximumBytes ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0) {
        return false;
    }
    void* bytes = data.empty() ? nullptr : const_cast<std::byte*>(data.data());
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
        queue(manager, &sourceKey, &targetKey, messageType, data.size(), bytes,
            relativeDelaySeconds);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    queue(manager, &sourceKey, &targetKey, messageType, data.size(), bytes,
        relativeDelaySeconds);
    return true;
#endif
}

[[nodiscard]] void* ReadManager() noexcept {
    if (gManagerRootAddress == nullptr) return nullptr;
    const auto manager = crash_guard::SafeRead<void*>(gManagerRootAddress);
    return manager.has_value() ? *manager : nullptr;
}

} // namespace

bool Initialize(const SignatureResolver& resolver, std::string& error) noexcept {
    error.clear();
    bool expected = false;
    if (!gInitialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }

    gManagerRootAddress = resolver.Resolve(SymbolId::CathodeEntityManagerRoot);
    gExecuteWork = reinterpret_cast<ExecuteWorkFn>(const_cast<void*>(resolver.Resolve(SymbolId::CathodeWorkItemExecute)));
    gAssignEntityRef = reinterpret_cast<AssignEntityRefFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeAssignEntityRef)));
    gDispatchInputAddress = resolver.Resolve(SymbolId::CathodeDispatchInput);
    gBuildInputTarget = reinterpret_cast<BuildInputTargetFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeBuildInputTarget)));
    gQueueInput = reinterpret_cast<QueueInputFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeQueueInput)));
    gQueueTypedMessage = reinterpret_cast<QueueTypedMessageFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeQueueTypedMessage)));
    gReleaseInputTarget = reinterpret_cast<ReleaseInputTargetFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeReleaseInputTarget)));
    // Fix3.20: the constructor/retain/release ABI is now both statically and
    // runtime constrained. Merely resolving these symbols does not enable the
    // historical 0x000C0603 replay path. Only the new exact Character family
    // calls them.
    gBuildCallbackValue = reinterpret_cast<BuildCallbackValueFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeBuildCallbackValue)));
    gBuildRetainedWorkItem = reinterpret_cast<BuildRetainedWorkItemFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeBuildRetainedWorkItem)));
    gEnterWorkItemCriticalSection = reinterpret_cast<EnterWorkItemCriticalSectionFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeEnterWorkItemCriticalSection)));
    gLeaveWorkItemCriticalSection = reinterpret_cast<LeaveWorkItemCriticalSectionFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeLeaveWorkItemCriticalSection)));
    gRetainCallbackValue = reinterpret_cast<RetainCallbackValueFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeRetainCallbackValue)));
    gReleaseCallbackValue = reinterpret_cast<ReleaseCallbackValueFn>(
        const_cast<void*>(resolver.Resolve(SymbolId::CathodeReleaseCallbackValue)));

    // Preserve baseline WorldSync availability even if one callback helper is
    // unavailable on an unsupported binary. The narrow Fix3.20 executor checks
    // all three pointers again and fails closed without affecting normal input/
    // typed-message replay.
    if (gManagerRootAddress == nullptr || gDispatchInputAddress == nullptr ||
        gBuildInputTarget == nullptr || gQueueInput == nullptr ||
        gQueueTypedMessage == nullptr || gReleaseInputTarget == nullptr) {
        gInitialized.store(false, std::memory_order_release);
        gManagerRootAddress = nullptr;
        gExecuteWork = nullptr;
        gAssignEntityRef = nullptr;
        gDispatchInputAddress = nullptr;
        gBuildInputTarget = nullptr;
        gQueueInput = nullptr;
        gQueueTypedMessage = nullptr;
        gReleaseInputTarget = nullptr;
        gBuildCallbackValue = nullptr;
        gBuildRetainedWorkItem = nullptr;
        gEnterWorkItemCriticalSection = nullptr;
        gLeaveWorkItemCriticalSection = nullptr;
        gRetainCallbackValue = nullptr;
        gReleaseCallbackValue = nullptr;
        error = "generic CATHODE input/message dispatcher/builder/scheduler/release symbols are unavailable";
        return false;
    }

    gInitializationCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return true;
}

bool Initialized() noexcept {
    return gInitialized.load(std::memory_order_acquire);
}

RuntimeDiagnostics DiagnosticsSnapshot() noexcept {
    RuntimeDiagnostics result{};
    result.initialized = Initialized();
    result.version = gVersion.load(std::memory_order_acquire);
    result.initializationCount = gInitializationCount.load(std::memory_order_acquire);
    result.shutdownCount = gShutdownCount.load(std::memory_order_acquire);
    result.executeAttempts = gExecuteAttempts.load(std::memory_order_acquire);
    result.queuedInputs = gQueuedInputs.load(std::memory_order_acquire);
    result.invalidPayloads = gInvalidPayloads.load(std::memory_order_acquire);
    result.unsupportedOperations = gUnsupportedOperations.load(std::memory_order_acquire);
    result.runtimeUnavailable = gRuntimeUnavailable.load(std::memory_order_acquire);
    result.nativeFaults = gNativeFaults.load(std::memory_order_acquire);
    result.implicitSourceFallbacks = gImplicitSourceFallbacks.load(std::memory_order_acquire);
    return result;
}

ExecuteResult Execute(
    const std::uint32_t operation,
    const std::span<const std::byte> payload,
    const WorldEntityLease& target,
    const WorldEntityLease* source) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();

    if (operation != kOperationTriggerInput) {
        gUnsupportedOperations.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::UnsupportedOperation, false};
    }

    TriggerInputRequest request{};
    if (!DecodeTriggerInputPayload(payload, request)) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!Initialized() || !target || CathodePrimitiveLeaseAccess::SharedRef(target) == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }

    const WorldEntityLease* effectiveSource = source;
    if (effectiveSource == nullptr) {
        effectiveSource = &target;
        gImplicitSourceFallbacks.fetch_add(1U, std::memory_order_acq_rel);
    }
    if (!*effectiveSource || CathodePrimitiveLeaseAccess::SharedRef(*effectiveSource) == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }

    auto* manager = ReadManager();
    if (manager == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }

    if (!InvokeQueueSafely(
            manager,
            gBuildInputTarget,
            gQueueInput,
            gReleaseInputTarget,
            CathodePrimitiveLeaseAccess::SharedRef(target),
            CathodePrimitiveLeaseAccess::SharedRef(*effectiveSource),
            request.inputShortGuid,
            request.inputShortGuid,
            0.0)) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }

    gQueuedInputs.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return {ExecuteStatus::Queued, true};
}

ExecuteResult ExecuteInputEdge(
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const float relativeDelaySeconds,
    const WorldEntityLease& target,
    const WorldEntityLease& source) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (schedulerParameterShortGuid == 0U || targetInputShortGuid == 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0F ||
        relativeDelaySeconds > 600.0F) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!Initialized() || !target || !source ||
        CathodePrimitiveLeaseAccess::SharedRef(target) == nullptr ||
        CathodePrimitiveLeaseAccess::SharedRef(source) == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    auto* manager = ReadManager();
    if (manager == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (!InvokeQueueSafely(
            manager,
            gBuildInputTarget,
            gQueueInput,
            gReleaseInputTarget,
            CathodePrimitiveLeaseAccess::SharedRef(target),
            CathodePrimitiveLeaseAccess::SharedRef(source),
            schedulerParameterShortGuid,
            targetInputShortGuid,
            static_cast<double>(relativeDelaySeconds))) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gQueuedInputs.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return {ExecuteStatus::Queued, true};
}

ExecuteResult ExecuteInputEdgeSharedRefs(
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const float relativeDelaySeconds,
    void* targetSharedRef,
    void* sourceSharedRef) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (schedulerParameterShortGuid == 0U || targetInputShortGuid == 0U ||
        targetSharedRef == nullptr || sourceSharedRef == nullptr ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0F ||
        relativeDelaySeconds > 600.0F) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!Initialized()) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    auto* manager = ReadManager();
    if (manager == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (!InvokeQueueSafely(
            manager,
            gBuildInputTarget,
            gQueueInput,
            gReleaseInputTarget,
            targetSharedRef,
            sourceSharedRef,
            schedulerParameterShortGuid,
            targetInputShortGuid,
            static_cast<double>(relativeDelaySeconds))) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gQueuedInputs.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return {ExecuteStatus::Queued, true};
}

ExecuteResult ExecuteInputEdgeWithConstructedCallback(
    const std::uint32_t schedulerParameterShortGuid,
    const std::uint32_t targetInputShortGuid,
    const std::uint32_t callbackConstructorFlags,
    const std::uint32_t callbackPayloadSize,
    const float relativeDelaySeconds,
    const WorldEntityLease& target,
    const WorldEntityLease& source,
    const WorldEntityLease& callback) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (schedulerParameterShortGuid == 0U || targetInputShortGuid == 0U ||
        callbackConstructorFlags != 0x12000000U || callbackPayloadSize != 0U ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0F ||
        relativeDelaySeconds > 600.0F) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!Initialized() || !target || !source || !callback ||
        CathodePrimitiveLeaseAccess::SharedRef(target) == nullptr ||
        CathodePrimitiveLeaseAccess::SharedRef(source) == nullptr ||
        CathodePrimitiveLeaseAccess::SharedRef(callback) == nullptr ||
        gBuildCallbackValue == nullptr || gRetainCallbackValue == nullptr ||
        gReleaseCallbackValue == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    const auto callbackVtable = crash_guard::SafeRead<void*>(callback.Get());
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!callbackVtable.has_value() || *callbackVtable == nullptr || moduleBase == 0U ||
        reinterpret_cast<std::uintptr_t>(*callbackVtable) < moduleBase ||
        reinterpret_cast<std::uintptr_t>(*callbackVtable) - moduleBase != 0x00DD08D8U) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    auto* manager = ReadManager();
    if (manager == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (!InvokeQueueConstructedCallbackSafely(
            manager,
            gBuildInputTarget,
            gQueueInput,
            gReleaseInputTarget,
            gBuildCallbackValue,
            gRetainCallbackValue,
            gReleaseCallbackValue,
            CathodePrimitiveLeaseAccess::SharedRef(target),
            CathodePrimitiveLeaseAccess::SharedRef(source),
            CathodePrimitiveLeaseAccess::SharedRef(callback),
            schedulerParameterShortGuid,
            targetInputShortGuid,
            callbackConstructorFlags,
            callbackPayloadSize,
            static_cast<double>(relativeDelaySeconds))) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gQueuedInputs.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return {ExecuteStatus::Queued, true};
}

NativeCallbackLease::~NativeCallbackLease() { static_cast<void>(Reset()); }

NativeCallbackLease::NativeCallbackLease(NativeCallbackLease&& other) noexcept
    : value_(other.value_), release_(other.release_) {
    other.value_ = nullptr;
    other.release_ = nullptr;
}

NativeCallbackLease& NativeCallbackLease::operator=(NativeCallbackLease&& other) noexcept {
    if (this == &other) return *this;
    static_cast<void>(Reset());
    value_ = other.value_;
    release_ = other.release_;
    other.value_ = nullptr;
    other.release_ = nullptr;
    return *this;
}

bool NativeCallbackLease::Reset() noexcept {
    auto* value = value_;
    const auto release = reinterpret_cast<ReleaseCallbackValueFn>(release_);
    value_ = nullptr;
    release_ = nullptr;
    if (value == nullptr) return true;
    if (release == nullptr || gEnterWorkItemCriticalSection == nullptr ||
        gLeaveWorkItemCriticalSection == nullptr) return false;

    // FUN_009252F0 may recycle the WorkItem back into the same retail pool
    // touched by FUN_00926D20. Retail destruction paths hold the CATHODE
    // WorkItem critical section while performing this release.
    bool entered = false;
    bool released = false;
#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        gEnterWorkItemCriticalSection(nullptr);
        entered = true;
        release(value);
        released = true;
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        released = false;
    }
#endif
    if (entered) gLeaveWorkItemCriticalSection();
    return released;
}

bool ExecuteRetainedInput(const WorldEntityLease& owner, const WorldEntityLease& target,
    const NativeCallbackLease& work) noexcept {
    void* manager = ReadManager();
    void* ownerRef = CathodePrimitiveLeaseAccess::SharedRef(owner);
    void* targetRef = CathodePrimitiveLeaseAccess::SharedRef(target);
    void* item = work.Get();
    if (!Initialized() || !manager || !ownerRef || !targetRef || !item || !gExecuteWork ||
        !gAssignEntityRef || !gEnterWorkItemCriticalSection || !gLeaveWorkItemCriticalSection)
        return false;

    // STATIC CONFIRMED against the supplied retail AI.exe: every direct call
    // from FUN_00926A60 into FUN_00926680 passes the SAME holder for param_1
    // and param_2. A ScheduledInput ROOT is captured after that holder
    // selection, so replay one certified consumer invocation rather than
    // re-running the scheduler selector/fan-out. Fail closed if the wire/root
    // preparation ever supplies a shape that retail does not use here.
    if (ownerRef != targetRef) return false;

    bool entered = false;
    bool executed = false;
    bool timeSaved = false;
    bool triggerSaved = false;
    std::uint64_t previousTime{};
    void* previousTrigger{};
    void* emptyTrigger{};
    auto* managerTime = reinterpret_cast<std::uint64_t*>(
        static_cast<std::byte*>(manager) + 0x90U);
    auto** managerTrigger = reinterpret_cast<void**>(
        static_cast<std::byte*>(manager) + 0x5CU);
    auto* schedulerDebugLatch = reinterpret_cast<std::uint8_t*>(
        static_cast<std::byte*>(manager) + 0x10AU);

#if defined(_MSC_VER) && defined(_M_IX86)
    __try {
#endif
        gEnterWorkItemCriticalSection(nullptr);
        entered = true;

        // FUN_0092CE20 saves/restores manager+0x90 around scheduler draining,
        // while FUN_00926A60 installs WorkItem+0x18 immediately before the
        // consumer call. Reproduce only that already-selected consumer
        // context; do not re-run scheduler admission or fan-out.
        previousTime = *managerTime;
        timeSaved = true;

        // manager+0x10A is not gameplay context. Retail raises it only around
        // a special scheduler assertion path; 00926680 clears it and executes
        // an INT3 only when a debugger is attached. The capture side refuses
        // that Host shape, and replay independently fails closed if the local
        // process is currently inside it. Never synthesize or consume the
        // receiver's local assertion latch for a remote ROOT.
        if (*schedulerDebugLatch != 0U) {
            executed = false;
        } else {
            // FUN_00926680 assigns manager+0x5C (retail save name:
            // "trigger_object") from FUN_009263A0(current WorkItem). It clears
            // that holder on normal return instead of restoring a pre-existing
            // value. Remote replay may be nested inside local CATHODE execution,
            // so retain the previous holder with retail shared-ref assignment and
            // restore it after the consumer returns or faults.
            if (!InvokeAssignEntityRefSafely(
                    gAssignEntityRef, &previousTrigger, managerTrigger)) {
                executed = false;
            } else {
                triggerSaved = true;
                *managerTime = *reinterpret_cast<const std::uint64_t*>(
                    static_cast<const std::byte*>(item) + 0x18U);

                // Replay the exact post-selection retail boundary. The original
                // scheduler always passes one holder address in BOTH argument
                // positions, including its special fan-out path.
                static_cast<void>(gExecuteWork(manager, &targetRef, &targetRef, &item));
                executed = true;
            }
        }
#if defined(_MSC_VER) && defined(_M_IX86)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        executed = false;
    }
#endif

    // Restore temporary scheduler context even after a guarded native fault.
    // The local snapshot itself is an owned retail shared-ref and must also be
    // released after manager+0x5C has retained the restored value.
    if (triggerSaved) {
        const bool restored = InvokeAssignEntityRefSafely(
            gAssignEntityRef, managerTrigger, &previousTrigger);
        const bool releasedSnapshot = InvokeAssignEntityRefSafely(
            gAssignEntityRef, &previousTrigger, &emptyTrigger);
        if (!restored || !releasedSnapshot) executed = false;
    }
    if (timeSaved) *managerTime = previousTime;
    if (entered) gLeaveWorkItemCriticalSection();
    return executed;
}

ExecuteStatus CreateNativeCallback(const WorldEntityLease& context,
    const std::uint32_t constructorFlags, const std::span<const std::byte> payload,
    NativeCallbackLease& output) noexcept {
    if (output.Get() != nullptr || payload.size() > kWorldPayloadMaximumBytes)
        return ExecuteStatus::InvalidPayload;
    if (!Initialized() || !context || gBuildCallbackValue == nullptr ||
        gReleaseCallbackValue == nullptr) return ExecuteStatus::RuntimeUnavailable;
    void* shared = CathodePrimitiveLeaseAccess::SharedRef(context);
    auto* manager = ReadManager();
    if (manager == nullptr || shared == nullptr) return ExecuteStatus::RuntimeUnavailable;
    output.release_ = reinterpret_cast<void*>(gReleaseCallbackValue);
    if (!BuildNativeCallbackSafely(manager, shared, constructorFlags,
            payload.data(), payload.size(), output.value_))
        return ExecuteStatus::NativeFault;
    // A constructed native parent is ready; nothing has been scheduled yet.
    return ExecuteStatus::Queued;
}

ExecuteStatus CreateNativeContextFrame(const WorldEntityLease& context,
    const std::uint32_t inputShortGuid, const std::uint32_t constructorFlags,
    const std::uint32_t descriptorDelayBits, const std::span<const std::byte> payload,
    const NativeCallbackLease* retainedParent, NativeCallbackLease& output) noexcept {
    if (output.Get() != nullptr || inputShortGuid == 0U ||
        payload.size() > kWorldPayloadMaximumBytes)
        return ExecuteStatus::InvalidPayload;
    if (!Initialized() || !context || gBuildCallbackValue == nullptr ||
        gReleaseCallbackValue == nullptr)
        return ExecuteStatus::RuntimeUnavailable;

    // The retail callback wrapper FUN_00926FF0 is the proven constructor for a
    // standalone canonical "callback" WorkItem. ROOT v8 used this exact path
    // successfully. Do not clone such a frame through FUN_00926D20: the clone
    // is unnecessary when there is no retained parent and can perturb retail
    // descriptor/lifetime state that later CATHODE variable resolution relies on.
    // Reserve the raw retained-WorkItem constructor for frames that genuinely
    // need a parent or a non-canonical input descriptor.
    constexpr std::uint32_t kCanonicalCallbackInputShortGuid = 0x6575CA9BU;
    const bool hasRetainedParent = retainedParent != nullptr && retainedParent->Get() != nullptr;
    if (!hasRetainedParent &&
        inputShortGuid == kCanonicalCallbackInputShortGuid &&
        descriptorDelayBits == 0U) {
        return CreateNativeCallback(context, constructorFlags, payload, output);
    }

    if (gBuildRetainedWorkItem == nullptr ||
        gEnterWorkItemCriticalSection == nullptr ||
        gLeaveWorkItemCriticalSection == nullptr ||
        gRetainCallbackValue == nullptr)
        return ExecuteStatus::RuntimeUnavailable;

    // For an actual retained/non-callback frame, use the proven generic callback
    // constructor solely to allocate/copy the payload and retain the source
    // shared-ref through retail code. Then clone that descriptor through
    // FUN_00926D20 while supplying the real parent.
    NativeCallbackLease standalone;
    const auto standaloneStatus = CreateNativeCallback(
        context, constructorFlags, payload, standalone);
    if (standaloneStatus != ExecuteStatus::Queued) return standaloneStatus;

    auto* manager = ReadManager();
    if (manager == nullptr) return ExecuteStatus::RuntimeUnavailable;
    void* built{};
    void* retainedParentValue = retainedParent != nullptr ? retainedParent->Get() : nullptr;
    // The retail retained-WorkItem constructor also accepts a null parent. This
    // is required for the outermost captured ancestry frame when its input is
    // not the canonical callback ShortGuid.
    if (!BuildRetainedContextSafely(manager, standalone.Get(), retainedParentValue,
            inputShortGuid, descriptorDelayBits, built)) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return ExecuteStatus::NativeFault;
    }
    output.release_ = reinterpret_cast<void*>(gReleaseCallbackValue);
    output.value_ = built;
    return ExecuteStatus::Queued;
}

ExecuteResult ExecuteTypedMessage(
    const std::uint32_t messageType,
    const std::span<const std::byte> data,
    const double relativeDelaySeconds,
    const CathodeRuntimeKey& targetKey,
    const CathodeRuntimeKey& sourceKey) noexcept {
    gExecuteAttempts.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    if (data.size() > kWorldPayloadMaximumBytes ||
        !std::isfinite(relativeDelaySeconds) || relativeDelaySeconds < 0.0 ||
        relativeDelaySeconds > 600.0 || targetKey.entityA == 0U || sourceKey.entityA == 0U) {
        gInvalidPayloads.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::InvalidPayload, false};
    }
    if (!Initialized() || gQueueTypedMessage == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    auto* manager = ReadManager();
    if (manager == nullptr) {
        gRuntimeUnavailable.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::RuntimeUnavailable, false};
    }
    if (!InvokeTypedMessageSafely(
            manager, gQueueTypedMessage, sourceKey, targetKey, messageType, data,
            relativeDelaySeconds)) {
        gNativeFaults.fetch_add(1U, std::memory_order_acq_rel);
        return {ExecuteStatus::NativeFault, false};
    }
    gQueuedInputs.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
    return {ExecuteStatus::Queued, true};
}

void BeginLogicalShutdown() noexcept {
    if (!gInitialized.exchange(false, std::memory_order_acq_rel)) return;
    gManagerRootAddress = nullptr;
    gExecuteWork = nullptr;
    gAssignEntityRef = nullptr;
    gDispatchInputAddress = nullptr;
    gBuildInputTarget = nullptr;
    gQueueInput = nullptr;
    gQueueTypedMessage = nullptr;
    gReleaseInputTarget = nullptr;
    gBuildCallbackValue = nullptr;
    gBuildRetainedWorkItem = nullptr;
    gEnterWorkItemCriticalSection = nullptr;
    gLeaveWorkItemCriticalSection = nullptr;
    gRetainCallbackValue = nullptr;
    gReleaseCallbackValue = nullptr;
    gShutdownCount.fetch_add(1U, std::memory_order_acq_rel);
    TouchVersion();
}

} // namespace cathode_primitives
} // namespace aimp
