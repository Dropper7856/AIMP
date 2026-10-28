#include "aimp/AISync.h"
#include "aimp/AISyncExecutionRuntime.h"
#include "aimp/CharacterActionRuntime.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterObservationHooks.h"
#include "aimp/CharacterIncarnation.h"
#include "aimp/CrashGuard.h"

#include "aimp/BuildProfile.h"
#include "aimp/CaptureCatalog.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/LifecycleCatalog.h"
#include "aimp/SignatureResolver.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/RuntimeFeatureFlags.h"

#include <atomic>

namespace aimp {
namespace {

using SpawnCallback = std::uint32_t(__cdecl*)(void*, const void*);

std::atomic<CaptureCatalog*> gCatalog{nullptr};
std::atomic<LifecycleCatalog*> gLifecycle{nullptr};
std::atomic<LevelGenerationTracker*> gGeneration{nullptr};
std::atomic<void*> gF0Trampoline{nullptr};
std::atomic<void*> gF130Trampoline{nullptr};
std::atomic<void*> gCharacterCreateTrampoline{nullptr};
std::atomic<void*> gCharacterDestroyTrampoline{nullptr};
std::atomic<void*> gCharacterFullDestroyTrampoline{nullptr};
std::atomic<void*> gCharacterFinalizeTrampoline{nullptr};
std::atomic<void*> gParticipantAppendTrampoline{nullptr};
std::atomic<void*> gChannelAllocateTrampoline{nullptr};
std::atomic<void*> gChannelReleaseTrampoline{nullptr};
std::atomic<void*> gContextAllocateTrampoline{nullptr};
std::atomic<void*> gContextReleaseTrampoline{nullptr};
std::atomic<std::uint32_t> gActiveDetours{0};

class ActiveDetour final {
public:
    ActiveDetour() noexcept {
        gActiveDetours.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveDetour() {
        gActiveDetours.fetch_sub(1, std::memory_order_acq_rel);
    }
};

std::uint32_t CaptureAndForward(
    const CharacterRequestKind kind,
    void* sink,
    const void* request,
    const std::size_t bytes,
    const std::uint32_t sourceRva,
    const std::atomic<void*>& trampoline) noexcept {
    const auto original = reinterpret_cast<SpawnCallback>(
        trampoline.load(std::memory_order_acquire));
    if (!runtime_features::Enabled(
            runtime_features::Feature::CharacterLifecycleCapture)) {
        return original == nullptr ? 0U : original(sink, request);
    }

    ActiveDetour active;
    auto* catalog = gCatalog.load(std::memory_order_acquire);
    const auto* generation = gGeneration.load(std::memory_order_acquire);
    if (catalog != nullptr && generation != nullptr) {
        catalog->CaptureFromHook(
            kind,
            sink,
            request,
            bytes,
            generation->CurrentGeneration(),
            sourceRva);
    }
    return original == nullptr ? 0U : original(sink, request);
}

std::uint32_t __cdecl ReceiveF0Detour(
    void* sink,
    const void* request) noexcept {
    return CaptureAndForward(
        CharacterRequestKind::SpawnF0,
        sink,
        request,
        0xF0,
        0x00014A70,
        gF0Trampoline);
}

std::uint32_t __cdecl Receive130Detour(
    void* sink,
    const void* request) noexcept {
    return CaptureAndForward(
        CharacterRequestKind::Spawn130,
        sink,
        request,
        0x130,
        0x00014C90,
        gF130Trampoline);
}

std::uint64_t Generation() noexcept {
    const auto* generation = gGeneration.load(std::memory_order_acquire);
    return generation == nullptr ? 0 : generation->CurrentGeneration();
}

void Lifecycle(
    const LifecycleEventKind kind,
    const std::uintptr_t owner,
    const std::uintptr_t first = 0,
    const std::uintptr_t second = 0,
    const std::uintptr_t result = 0) noexcept {
    if (!runtime_features::Enabled(
            runtime_features::Feature::CharacterLifecycleCapture)) {
        return;
    }
    auto* catalog = gLifecycle.load(std::memory_order_acquire);
    if (catalog != nullptr) {
        catalog->CaptureFromHook(
            kind,
            Generation(),
            owner,
            first,
            second,
            result);
    }
}

void* __fastcall CharacterCreateDetour(
    void* manager,
    void*,
    void* record,
    const void* request) noexcept {
    ActiveDetour active;
    Lifecycle(
        LifecycleEventKind::CharacterCreateBegin,
        reinterpret_cast<std::uintptr_t>(manager),
        reinterpret_cast<std::uintptr_t>(record),
        reinterpret_cast<std::uintptr_t>(request));
    const auto original = reinterpret_cast<
        void*(__thiscall*)(void*, void*, const void*)>(
            gCharacterCreateTrampoline.load(std::memory_order_acquire));
    void* result =
        original == nullptr ? nullptr : original(manager, record, request);
    if(result) {
        const auto runtime=crash_guard::SafeRead<std::uint32_t>(static_cast<const std::byte*>(result)+0x50U);
        character_incarnations::Ticket ticket{};
        if(runtime)static_cast<void>(character_incarnations::NativeRegistry().Created(
            Generation(),reinterpret_cast<std::uintptr_t>(result),*runtime,ticket));
    }
    Lifecycle(
        LifecycleEventKind::CharacterCreateEnd,
        reinterpret_cast<std::uintptr_t>(manager),
        reinterpret_cast<std::uintptr_t>(record),
        reinterpret_cast<std::uintptr_t>(request),
        reinterpret_cast<std::uintptr_t>(result));
    return result;
}

void __fastcall CharacterDestroyDetour(
    void* manager,
    void*,
    const std::uint32_t spawnKey) noexcept {
    ActiveDetour active;
    ai_sync::ForgetSpawnKey(spawnKey);
    character_action_runtime::ForgetSpawnKey(spawnKey);
    Lifecycle(
        LifecycleEventKind::CharacterDestroyBegin,
        reinterpret_cast<std::uintptr_t>(manager),
        spawnKey);
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, std::uint32_t)>(
            gCharacterDestroyTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(manager, spawnKey);
    }
    Lifecycle(
        LifecycleEventKind::CharacterDestroyEnd,
        reinterpret_cast<std::uintptr_t>(manager),
        spawnKey);
}

void __fastcall CharacterFullDestroyDetour(
    void* manager,
    void*,
    void* character) noexcept {
    ActiveDetour active;
    character_incarnations::Ticket ticket{};
    static_cast<void>(character_incarnations::NativeRegistry().Find(Generation(),reinterpret_cast<std::uintptr_t>(character),ticket));
    ai_sync::BeforeCharacterDestruction(reinterpret_cast<std::uintptr_t>(character));
    // Pre-destruction callbacks can reenter destruction or replace this address.
    // Only the exact lifetime retired by this invocation may produce its proof.
    const bool retired = ticket.serial != 0U &&
        character_incarnations::NativeRegistry().Retire(ticket) ==
            character_incarnations::Result::Ok;
    ai_sync::ForgetCharacter(reinterpret_cast<std::uintptr_t>(character));
    character_action_runtime::ForgetCharacter(reinterpret_cast<std::uintptr_t>(character));
    // The network movement pump caches raw Character addresses and writes yaw
    // and control every frame; it must never outlive the Character.
    character_direct_step_runtime::ForgetCharacter(reinterpret_cast<std::uintptr_t>(character));
    Lifecycle(
        LifecycleEventKind::CharacterFullDestroyBegin,
        reinterpret_cast<std::uintptr_t>(manager),
        reinterpret_cast<std::uintptr_t>(character));
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, void*)>(
            gCharacterFullDestroyTrampoline.load(
                std::memory_order_acquire));
    if (original != nullptr) {
        original(manager, character);
        if(retired&&ticket.generation<=UINT32_MAX)
            ai_sync::execution_runtime::AfterActorDestruction({ticket.character,ticket.runtimeId,
                static_cast<std::uint32_t>(ticket.generation),ticket.serial});
    }
    Lifecycle(
        LifecycleEventKind::CharacterFullDestroyEnd,
        reinterpret_cast<std::uintptr_t>(manager),
        reinterpret_cast<std::uintptr_t>(character));
}

void __fastcall ParticipantAppendDetour(
    void* wrapper,
    void*,
    const void* record) noexcept {
    ActiveDetour active;
    Lifecycle(
        LifecycleEventKind::ParticipantAppendBegin,
        reinterpret_cast<std::uintptr_t>(wrapper),
        reinterpret_cast<std::uintptr_t>(record));
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, const void*)>(
            gParticipantAppendTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(wrapper, record);
    }
    Lifecycle(
        LifecycleEventKind::ParticipantAppendEnd,
        reinterpret_cast<std::uintptr_t>(wrapper),
        reinterpret_cast<std::uintptr_t>(record));
}

void __fastcall CharacterFinalizeDetour(
    void* sink,
    void*,
    const void* request) noexcept {
    ActiveDetour active;
    Lifecycle(
        LifecycleEventKind::CharacterFinalizeBegin,
        reinterpret_cast<std::uintptr_t>(sink),
        reinterpret_cast<std::uintptr_t>(request));
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, const void*)>(
            gCharacterFinalizeTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(sink, request);
    }
    Lifecycle(
        LifecycleEventKind::CharacterFinalizeEnd,
        reinterpret_cast<std::uintptr_t>(sink),
        reinterpret_cast<std::uintptr_t>(request));
}

void* __fastcall ChannelAllocateDetour(
    void* netz,
    void*,
    const bool enabled) noexcept {
    ActiveDetour active;
    const auto original = reinterpret_cast<
        void*(__thiscall*)(void*, bool)>(
            gChannelAllocateTrampoline.load(std::memory_order_acquire));
    void* result =
        original == nullptr ? nullptr : original(netz, enabled);
    Lifecycle(
        LifecycleEventKind::ChannelAllocated,
        reinterpret_cast<std::uintptr_t>(netz),
        enabled ? 1U : 0U,
        0,
        reinterpret_cast<std::uintptr_t>(result));
    return result;
}

void __fastcall ChannelReleaseDetour(
    void* netz,
    void*,
    void* channel) noexcept {
    ActiveDetour active;
    Lifecycle(
        LifecycleEventKind::ChannelReleased,
        reinterpret_cast<std::uintptr_t>(netz),
        reinterpret_cast<std::uintptr_t>(channel));
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, void*)>(
            gChannelReleaseTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(netz, channel);
    }
}

void* __fastcall ContextAllocateDetour(
    void* pool,
    void*,
    void* channel,
    void* character) noexcept {
    ActiveDetour active;
    const auto original = reinterpret_cast<
        void*(__thiscall*)(void*, void*, void*)>(
            gContextAllocateTrampoline.load(std::memory_order_acquire));
    void* result = original == nullptr
        ? nullptr
        : original(pool, channel, character);
    Lifecycle(
        LifecycleEventKind::ReplicationContextAllocated,
        reinterpret_cast<std::uintptr_t>(pool),
        reinterpret_cast<std::uintptr_t>(channel),
        reinterpret_cast<std::uintptr_t>(character),
        reinterpret_cast<std::uintptr_t>(result));
    return result;
}

void __fastcall ContextReleaseDetour(
    void* pool,
    void*,
    void* context) noexcept {
    ActiveDetour active;
    Lifecycle(
        LifecycleEventKind::ReplicationContextReleased,
        reinterpret_cast<std::uintptr_t>(pool),
        reinterpret_cast<std::uintptr_t>(context));
    const auto original = reinterpret_cast<
        void(__thiscall*)(void*, void*)>(
            gContextReleaseTrampoline.load(std::memory_order_acquire));
    if (original != nullptr) {
        original(pool, context);
    }
}

} // namespace

CharacterObservationHooks::CharacterObservationHooks(
    CaptureCatalog& catalog,
    LifecycleCatalog& lifecycle,
    LevelGenerationTracker& generation,
    SpawnCharacterTracker* spawnTracker) noexcept
    : catalog_(&catalog),
      lifecycle_(&lifecycle),
      generation_(&generation),
      spawnTracker_(spawnTracker) {}

bool CharacterObservationHooks::InstallExactProfile(
    SignatureResolver& resolver,
    HookManager& hooks,
    std::string& error) {
    if (installed_ ||
        !resolver.ExactBuildValidated() ||
        catalog_ == nullptr ||
        lifecycle_ == nullptr ||
        generation_ == nullptr) {
        error =
            "Character request hooks require one exact profile and live process-lifetime capture services";
        return false;
    }
    const auto& profile = Retail2015Profile();
    const auto* f0 = profile.Find(SymbolId::ReceiveSpawnRequestF0);
    const auto* f130 = profile.Find(SymbolId::ReceiveSpawnRequest130);
    auto* f0Target = resolver.ResolveMutable(SymbolId::ReceiveSpawnRequestF0);
    auto* f130Target =
        resolver.ResolveMutable(SymbolId::ReceiveSpawnRequest130);
    if (f0 == nullptr ||
        f130 == nullptr ||
        f0Target == nullptr ||
        f130Target == nullptr) {
        error = "Exact Character callback signatures did not resolve";
        return false;
    }

    const auto hookBaseline = hooks.InstalledCount();
    gCatalog.store(catalog_, std::memory_order_release);
    gLifecycle.store(lifecycle_, std::memory_order_release);
    gGeneration.store(generation_, std::memory_order_release);
    const bool createTrackedExternally =
        spawnTracker_ != nullptr && spawnTracker_->HookInstalled();
    if (createTrackedExternally) {
        spawnTracker_->AttachLifecycleCatalog(*lifecycle_);
    }
    const auto rollback = [&](const std::string_view cause) {
        std::string uninstallError;
        hooks.UninstallFrom(hookBaseline, uninstallError);
        BeginLogicalShutdown();
        error = std::string(cause);
        if (!uninstallError.empty()) {
            error += "; rollback: ";
            error += uninstallError;
        }
        return false;
    };
    HookRequest f0Request{};
    f0Request.name = "observe_spawn_request_f0";
    f0Request.target = f0Target;
    f0Request.detour = reinterpret_cast<void*>(&ReceiveF0Detour);
    f0Request.patchLength = f0->approvedPatchLength;
    f0Request.expected = f0->expected;
    f0Request.relocationSafePrefix = f0->trampolineRelocationSafe;
    f0Request.trampolineOut = &f0Trampoline_;
    f0Request.publishedTrampolineOut = &gF0Trampoline;
    if (!hooks.Install(f0Request, error)) {
        BeginLogicalShutdown();
        return false;
    }
    processLifetimeRequired_ = true;
    gF0Trampoline.store(f0Trampoline_, std::memory_order_release);

    HookRequest f130Request{};
    f130Request.name = "observe_spawn_request_130";
    f130Request.target = f130Target;
    f130Request.detour = reinterpret_cast<void*>(&Receive130Detour);
    f130Request.patchLength = f130->approvedPatchLength;
    f130Request.expected = f130->expected;
    f130Request.relocationSafePrefix = f130->trampolineRelocationSafe;
    f130Request.trampolineOut = &f130Trampoline_;
    f130Request.publishedTrampolineOut = &gF130Trampoline;
    if (!hooks.Install(f130Request, error)) {
        return rollback(error);
    }
    gF130Trampoline.store(f130Trampoline_, std::memory_order_release);

    const auto install = [&](
        const SymbolId id,
        const char* name,
        void* detour,
        void** trampoline,
        std::atomic<void*>& published) {
        const auto* spec = profile.Find(id);
        auto* target = resolver.ResolveMutable(id);
        if (spec == nullptr || target == nullptr) {
            error = std::string(name) + " exact signature did not resolve";
            return false;
        }
        HookRequest request{};
        request.name = name;
        request.target = target;
        request.detour = detour;
        request.patchLength = spec->approvedPatchLength;
        request.expected = spec->expected;
        request.relocationSafePrefix = spec->trampolineRelocationSafe;
        request.trampolineOut = trampoline;
        request.publishedTrampolineOut = &published;
        if (!hooks.Install(request, error)) {
            return false;
        }
        processLifetimeRequired_ = true;
        published.store(*trampoline, std::memory_order_release);
        return true;
    };
    const bool createHookAvailable = createTrackedExternally || install(
        SymbolId::CharacterCreate,
        "observe_character_create",
        reinterpret_cast<void*>(&CharacterCreateDetour),
        &characterCreateTrampoline_,
        gCharacterCreateTrampoline);
    if (!createHookAvailable ||
        !install(
            SymbolId::CharacterDestroy,
            "observe_character_destroy",
            reinterpret_cast<void*>(&CharacterDestroyDetour),
            &characterDestroyTrampoline_,
            gCharacterDestroyTrampoline) ||
        !install(
            SymbolId::CharacterDestroyFull,
            "observe_character_full_destroy",
            reinterpret_cast<void*>(&CharacterFullDestroyDetour),
            &characterFullDestroyTrampoline_,
            gCharacterFullDestroyTrampoline) ||
        !install(
            SymbolId::CharacterOrchestrator,
            "observe_character_finalize",
            reinterpret_cast<void*>(&CharacterFinalizeDetour),
            &characterFinalizeTrampoline_,
            gCharacterFinalizeTrampoline) ||
        !install(
            SymbolId::ParticipantAppend,
            "observe_participant_append",
            reinterpret_cast<void*>(&ParticipantAppendDetour),
            &participantAppendTrampoline_,
            gParticipantAppendTrampoline) ||
        !install(
            SymbolId::AllocateChannel,
            "observe_channel_allocate",
            reinterpret_cast<void*>(&ChannelAllocateDetour),
            &channelAllocateTrampoline_,
            gChannelAllocateTrampoline) ||
        !install(
            SymbolId::ReleaseChannel,
            "observe_channel_release",
            reinterpret_cast<void*>(&ChannelReleaseDetour),
            &channelReleaseTrampoline_,
            gChannelReleaseTrampoline) ||
        !install(
            SymbolId::ReplicationContextAllocate,
            "observe_context_allocate",
            reinterpret_cast<void*>(&ContextAllocateDetour),
            &contextAllocateTrampoline_,
            gContextAllocateTrampoline) ||
        !install(
            SymbolId::ReplicationContextRelease,
            "observe_context_release",
            reinterpret_cast<void*>(&ContextReleaseDetour),
            &contextReleaseTrampoline_,
            gContextReleaseTrampoline)) {
        return rollback(error);
    }
    installed_ = true;
    lifetimeHooksInstalled_ = true;
    return true;
}

bool CharacterObservationHooks::InstallActionLifetimeHooks(
    SignatureResolver& resolver, HookManager& hooks, std::string& error) {
    if (lifetimeHooksInstalled_) return true; // reuse shared hooks, never hook twice
    if (!resolver.ExactBuildValidated() || generation_ == nullptr) {
        error = "Character action lifecycle requires the certified profile";
        return false;
    }
    const auto baseline = hooks.InstalledCount();
    // These shared detours need the world identity even without RE capture.
    // Publish it before HookManager makes either detour reachable.
    auto* previousGeneration = gGeneration.exchange(
        generation_, std::memory_order_acq_rel);
    const auto install = [&](SymbolId id, const char* name, void* detour,
        void** trampoline, std::atomic<void*>& published) {
        const auto* spec = Retail2015Profile().Find(id);
        auto* target = resolver.ResolveMutable(id);
        if (spec == nullptr || target == nullptr) {
            error = std::string(name) + " did not resolve";
            return false;
        }
        HookRequest request{};
        request.name = name;
        request.target = target;
        request.detour = detour;
        request.patchLength = spec->approvedPatchLength;
        request.expected = spec->expected;
        request.relocationSafePrefix = spec->trampolineRelocationSafe;
        request.trampolineOut = trampoline;
        request.publishedTrampolineOut = &published;
        if (!hooks.Install(request, error)) return false;
        processLifetimeRequired_ = true;
        return true;
    };
    if (!install(SymbolId::CharacterDestroy, "character_action_lifetime_destroy",
            reinterpret_cast<void*>(&CharacterDestroyDetour),
            &characterDestroyTrampoline_, gCharacterDestroyTrampoline) ||
        !install(SymbolId::CharacterDestroyFull, "character_action_lifetime_full_destroy",
            reinterpret_cast<void*>(&CharacterFullDestroyDetour),
            &characterFullDestroyTrampoline_, gCharacterFullDestroyTrampoline)) {
        std::string rollback;
        if (hooks.UninstallFrom(baseline, rollback)) {
            gGeneration.store(previousGeneration, std::memory_order_release);
        }
        // Failed rollback leaves callable detours; keep their generation service
        // and the existing process-lifetime retention flag and trampolines.
        if (!rollback.empty()) error += "; rollback: " + rollback;
        return false;
    }
    // The functional-only pair does not attach catalogs or create RE traces.
    lifetimeHooksInstalled_ = true;
    return true;
}

bool CharacterObservationHooks::LifetimeHooksInstalled() const noexcept {
    return lifetimeHooksInstalled_;
}

bool CharacterObservationHooks::Installed() const noexcept {
    return installed_;
}

void CharacterObservationHooks::BeginLogicalShutdown() noexcept {
    if (spawnTracker_ != nullptr) {
        spawnTracker_->DetachLifecycleCatalog();
    }
    // Do not clear the trampolines: a thread may already have followed the
    // target JMP. New detours become forward-only and no longer dereference
    // capture services. Bootstrap retains those services for process lifetime
    // when any observation hook was installed.
    gCatalog.store(nullptr, std::memory_order_release);
    gLifecycle.store(nullptr, std::memory_order_release);
    gGeneration.store(nullptr, std::memory_order_release);
}

std::uint32_t CharacterObservationHooks::ActiveDetours() noexcept {
    return gActiveDetours.load(std::memory_order_acquire);
}

bool CharacterObservationHooks::CaptureServicesAttached() noexcept {
    return gCatalog.load(std::memory_order_acquire) != nullptr &&
        gLifecycle.load(std::memory_order_acquire) != nullptr &&
        gGeneration.load(std::memory_order_acquire) != nullptr;
}

bool CharacterObservationHooks::ProcessLifetimeRequired() const noexcept {
    return processLifetimeRequired_;
}

} // namespace aimp
