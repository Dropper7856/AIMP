#include "aimp/HidingDoorPresentation.h"

#include "aimp/BuildProfile.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CrashGuard.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldEntityRegistry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>

namespace aimp::hiding_door_presentation {
namespace {
// CathodeLib FunctionType values; validate package metadata as well as the
// live native class, so an arbitrary same-scope GUID is never enough.
constexpr std::uint32_t kPlayEnvironmentAnimationType = 1553761290U;
constexpr std::uint32_t kUiIconType = 1449778502U;
constexpr std::uint32_t kGlobalEventMonitorType = 1544937131U;
constexpr std::size_t kMaximumOwners = 64U;
struct Pending final {
    std::uintptr_t owner{};
    player_hiding_presentation::State state{};
    std::uint32_t generation{};
    std::uint64_t receivedAt{};
    bool consumed{};
};
std::mutex gMutex;
std::array<Pending, kMaximumOwners> gPending{};
std::optional<LocalSnapshot> gLocal;
DiagnosticsSnapshot gDiagnostics{};
std::atomic<WorldEntityRegistry*> gRegistry{nullptr};
std::atomic<const void*> gAnimationVtable{nullptr};
std::uint64_t gNextTransition{};
thread_local bool gReplaying{};

std::uint64_t Now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint32_t Age(const std::uint64_t then, const std::uint64_t now,
    const std::uint32_t initial = 0U) noexcept {
    const auto elapsed = now >= then ? now - then : 0U;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        elapsed + initial, std::numeric_limits<std::uint32_t>::max()));
}
bool AnimationClass(const void* object) noexcept {
    const auto expected = gAnimationVtable.load(std::memory_order_acquire);
    if (object == nullptr || expected == nullptr) return false;
    const auto live = crash_guard::SafeRead<const void*>(object);
    return live.has_value() && *live == expected;
}
std::optional<WorldEntityId> TypedEntity(WorldEntityRegistry& registry,
    const CathodeRuntimeKey key, const std::uint32_t primitive) noexcept {
    const auto id = registry.FindWorldEntityId(key);
    if (!id.has_value()) return std::nullopt;
    const auto type = registry.FindWorldEntityPrimitiveType(*id);
    if (!type.has_value() || *type != primitive) return std::nullopt;
    return id;
}
struct InputEdge final {
    const void* target{};
    const void* source{};
    std::uint32_t parameter{}, input{};
    double delay{};
};
std::optional<InputEdge> ReadInputEdge(void** sourceSharedRef, const std::uint32_t* scheduler,
    const void* targetDescriptor, const double delay) noexcept {
    if (targetDescriptor == nullptr || sourceSharedRef == nullptr || scheduler == nullptr)
        return std::nullopt;
    const auto* bytes = static_cast<const std::byte*>(targetDescriptor);
    const auto input = crash_guard::SafeRead<std::uint32_t>(bytes);
    if (!input || *input != kApplyStart) return std::nullopt;
    const auto parameter = crash_guard::SafeRead<std::uint32_t>(scheduler);
    const auto targetDelay = crash_guard::SafeRead<float>(bytes + 4U);
    const auto targetShared = crash_guard::SafeRead<const std::byte*>(bytes + 8U);
    const auto sourceShared = crash_guard::SafeRead<const std::byte*>(sourceSharedRef);
    if (!parameter || !targetDelay || !targetShared || !*targetShared || !sourceShared || !*sourceShared)
        return std::nullopt;
    // Retail shared-ref execution object at +0x0C, identical to the existing
    // WorldInteractionAdapters::ExecutionObjectFromShared helper.
    const auto target = crash_guard::SafeRead<const void*>(*targetShared + 0x0CU);
    const auto source = crash_guard::SafeRead<const void*>(*sourceShared + 0x0CU);
    if (!target || !source) return std::nullopt;
    return InputEdge{*target, *source, *parameter, *input, static_cast<double>(*targetDelay) + delay};
}
std::optional<LocalSnapshot> DescribeInputEdge(const InputEdge& edge) noexcept {
    if (edge.input != kApplyStart || !std::isfinite(edge.delay) || edge.delay != 0.0 ||
        !AnimationClass(edge.target)) return std::nullopt;
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return std::nullopt;
    CathodeRuntimeKey targetKey{}, sourceKey{};
    if (!registry->TryExtractRuntimeKey(edge.target, targetKey) ||
        !registry->TryExtractRuntimeKey(edge.source, sourceKey) || targetKey.entityB == 0U ||
        targetKey.entityB != sourceKey.entityB) return std::nullopt;
    const auto identity = RecognizeInputEdge(targetKey.entityA, targetKey.entityB,
        sourceKey.entityA, sourceKey.entityB, edge.parameter, edge.input);
    const auto kind = identity.kind;
    if (kind == 0U || !TypedEntity(*registry, targetKey, kPlayEnvironmentAnimationType) ||
        !TypedEntity(*registry, sourceKey, kGlobalEventMonitorType) ||
        !TypedEntity(*registry, {Selection(kind).icon, targetKey.entityB}, kUiIconType)) return std::nullopt;
    const auto context = registry->ReplicationContext();
    if (!context.manifestLoaded || context.levelGeneration == 0U) return std::nullopt;
    LocalSnapshot snapshot{};
    snapshot.state.phase = identity.phase;
    snapshot.state.kind = kind;
    snapshot.state.entityShortGuid = Selection(kind).icon;
    snapshot.state.hierarchyHash = targetKey.entityB;
    snapshot.state.animate = true;
    snapshot.levelGeneration = context.levelGeneration;
    return snapshot;
}
enum class ApplyResult { Queued, Unresolved, Rejected, NativeFault };
ApplyResult Apply(WorldEntityRegistry& registry,
    const player_hiding_presentation::State& state) noexcept {
    const auto selection = Selection(state.kind);
    const auto animation = state.phase == 1U ? selection.enter : selection.exit;
    const auto targetId = TypedEntity(registry, {animation, state.hierarchyHash},
        kPlayEnvironmentAnimationType);
    const auto iconId = TypedEntity(registry, {selection.icon, state.hierarchyHash}, kUiIconType);
    const auto sourceId = TypedEntity(registry, {selection.monitor, state.hierarchyHash},
        kGlobalEventMonitorType);
    if (!targetId || !iconId || !sourceId) return ApplyResult::Unresolved;
    // Require the mutation endpoint to remain in the manifest's World domain.
    if (!registry.FindRuntimeWorldEntityId({animation, state.hierarchyHash}))
        return ApplyResult::Rejected;
    auto target = registry.Resolve(*targetId);
    // A GlobalEventMonitor may be classified outside World. Resolve its exact
    // retained execution ref as source only; never execute any of its inputs.
    bool sourceFault{};
    auto source = registry.ResolveRuntimeReference({selection.monitor, state.hierarchyHash}, sourceFault);
    if (sourceFault || target.status == WorldRegistryResolveStatus::NativeFault)
        return ApplyResult::NativeFault;
    if (!target.lease || !source) return ApplyResult::Unresolved;
    if (!AnimationClass(target.lease.Get())) return ApplyResult::Rejected;

    struct ReplayScope final {
        bool previous{gReplaying};
        ReplayScope() noexcept { gReplaying = true; }
        ~ReplayScope() { gReplaying = previous; }
    } scope;
    const auto result = cathode_primitives::ExecuteInputEdge(
        state.phase == 1U ? kEntryEvent : kExitEvent, kApplyStart, 0.0F, target.lease, source);
    if (result.queued && result.status == cathode_primitives::ExecuteStatus::Queued)
        return ApplyResult::Queued;
    if (result.status == cathode_primitives::ExecuteStatus::NativeFault)
        return ApplyResult::NativeFault;
    if (result.status == cathode_primitives::ExecuteStatus::RuntimeUnavailable)
        return ApplyResult::Unresolved;
    return ApplyResult::Rejected;
}
} // namespace

bool Initialize(const SignatureResolver& resolver, WorldEntityRegistry& registry,
    std::string& error) noexcept {
    error.clear();
    const auto allocatorVtable = resolver.Resolve(SymbolId::CathodePlayEnvironmentAnimationVtable);
    if (allocatorVtable == nullptr || !resolver.ExactBuildValidated()) {
        error = "Hiding door presentation: certified environment animation class unavailable";
        return false;
    }
    ForgetAll();
    // Existing profile symbol VA 011C8AC0 is the allocator/shared-holder
    // class, NOT the execution object. Factory 004CC880 dereferences shared
    // +0x0C and calls 004BF290, which installs execution vptr VA 011F1628.
    // Use their observed image-relative displacement only on the exact
    // validated retail image; never classify a live object as its allocator.
    constexpr std::ptrdiff_t kExecutionVtableFromAllocator = 0x011F1628 - 0x011C8AC0;
    gAnimationVtable.store(static_cast<const std::byte*>(allocatorVtable) +
        kExecutionVtableFromAllocator, std::memory_order_release);
    gRegistry.store(&registry, std::memory_order_release);
    return true;
}

bool Publish(const std::uintptr_t ownerToken, const player_hiding_presentation::State& state,
    std::string& error) noexcept {
    error.clear();
    if (ownerToken == 0U || !player_hiding_presentation::Valid(state) ||
        (state.phase != 0U && !ValidSelection(state))) {
        error = "Hiding door presentation requires an exact high/low UI icon runtime key";
        std::lock_guard lock(gMutex); ++gDiagnostics.rejected;
        return false;
    }
    if (state.phase == 0U) { Forget(ownerToken); return true; }
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) { error = "Hiding door presentation is not initialized"; return false; }
    const auto context = registry->ReplicationContext();
    if (!context.manifestLoaded || context.levelGeneration == 0U) {
        error = "Hiding door presentation requires an active manifest generation";
        return false;
    }
    std::lock_guard lock(gMutex);
    Pending* available{};
    for (auto& pending : gPending) {
        if (pending.owner == 0U && available == nullptr) available = &pending;
        if (pending.owner != ownerToken) continue;
        if (!IsNewTransition(pending.generation, pending.state.transition,
                context.levelGeneration, state.transition)) {
            ++gDiagnostics.duplicate;
            return true;
        }
        available = &pending;
        break;
    }
    if (available == nullptr) { error = "Hiding door presentation owner capacity exceeded"; return false; }
    *available = {ownerToken, state, context.levelGeneration, Now(), false};
    return true;
}

void PumpAllOnEngineThread() noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return;
    const auto context = registry->ReplicationContext();
    const auto now = Now();
    for (std::size_t index = 0; index < gPending.size(); ++index) {
        Pending work{};
        {
            std::lock_guard lock(gMutex);
            auto& pending = gPending[index];
            if (pending.owner == 0U || pending.consumed) continue;
            if (!context.manifestLoaded || pending.generation != context.levelGeneration) {
                pending = {}; ++gDiagnostics.generationDiscarded; continue;
            }
            if (!pending.state.animate || pending.state.phase == 2U) {
                pending.consumed = true; ++gDiagnostics.baseline; continue;
            }
            if (!ReplayAgeAllowed(pending.state.elapsedMilliseconds,
                    now >= pending.receivedAt ? now - pending.receivedAt : 0U)) {
                pending.consumed = true; ++gDiagnostics.stale; continue;
            }
            work = pending;
            // Do not hold the mailbox mutex during retail calls or their
            // observer callbacks. Mark before queueing to prevent reentrance.
            pending.consumed = true;
        }
        const auto result = Apply(*registry, work.state);
        std::lock_guard lock(gMutex);
        auto& current = gPending[index];
        switch (result) {
        case ApplyResult::Queued:
            ++gDiagnostics.queued;
            gDiagnostics.lastTargetShortGuid = work.state.phase == 1U ?
                Selection(work.state.kind).enter : Selection(work.state.kind).exit;
            gDiagnostics.lastHierarchyHash = work.state.hierarchyHash;
            break;
        case ApplyResult::Unresolved:
            ++gDiagnostics.unresolved;
            // Streaming can resolve on a later engine pump, within the age limit.
            if (current.owner == work.owner && current.generation == work.generation &&
                current.state.transition == work.state.transition) current.consumed = false;
            break;
        case ApplyResult::Rejected: ++gDiagnostics.rejected; break;
        case ApplyResult::NativeFault: ++gDiagnostics.nativeFaults; break;
        }
    }
}

bool ObserveLocalInputEdge(const void* target, const void* source,
    const std::uint32_t schedulerParameter, const std::uint32_t input,
    const double delay) noexcept {
    if (gReplaying) return false;
    auto description = DescribeInputEdge({target, source, schedulerParameter, input, delay});
    if (!description) return false;
    auto snapshot = *description;
    snapshot.observedAtMilliseconds = Now();
    std::lock_guard lock(gMutex);
    snapshot.state.transition = ++gNextTransition;
    gLocal = snapshot;
    ++gDiagnostics.captured;
    return true;
}

bool ObserveAcceptedQueueInput(void** sourceSharedRef, const std::uint32_t* scheduler,
    const void* targetDescriptor, const double delay) noexcept {
    if (gReplaying) return false;
    const auto edge = ReadInputEdge(sourceSharedRef, scheduler, targetDescriptor, delay);
    return edge && ObserveLocalInputEdge(edge->target, edge->source, edge->parameter, edge->input, edge->delay);
}
bool IsHidingDoorInput(void** sourceSharedRef, const std::uint32_t* scheduler,
    const void* targetDescriptor, const double delay) noexcept {
    const auto edge = ReadInputEdge(sourceSharedRef, scheduler, targetDescriptor, delay);
    return edge && DescribeInputEdge(*edge).has_value();
}

std::optional<LocalSnapshot> ReadLocalSnapshot() noexcept {
    auto* registry = gRegistry.load(std::memory_order_acquire);
    if (registry == nullptr) return std::nullopt;
    const auto context = registry->ReplicationContext();
    std::lock_guard lock(gMutex);
    if (!gLocal || !context.manifestLoaded || gLocal->levelGeneration != context.levelGeneration)
        return std::nullopt;
    auto copy = *gLocal;
    copy.state.elapsedMilliseconds = std::min(Age(copy.observedAtMilliseconds, Now()), 60000U);
    return copy;
}
void Forget(const std::uintptr_t owner) noexcept {
    std::lock_guard lock(gMutex);
    for (auto& pending : gPending) if (pending.owner == owner) pending = {};
}
void ForgetAll() noexcept {
    std::lock_guard lock(gMutex);
    gPending = {}; gLocal.reset();
}
void ResetForGeneration() noexcept { ForgetAll(); }
void BeginLogicalShutdown() noexcept {
    gRegistry.store(nullptr, std::memory_order_release);
    gAnimationVtable.store(nullptr, std::memory_order_release);
    ForgetAll();
}
DiagnosticsSnapshot Diagnostics() noexcept {
    std::lock_guard lock(gMutex);
    return gDiagnostics;
}
bool RemoteReplayActive() noexcept { return gReplaying; }
} // namespace aimp::hiding_door_presentation
