#include "aimp/PlayerLifecycle.h"

#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/SignatureResolver.h"
#include "aimp/WorldPublicActions.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace aimp::player_lifecycle {
namespace {

enum class Decision { Suppress, NoSession, NativeMode, ContextUnavailable };

struct DeathEvent {
    std::uintptr_t character{};
    std::int32_t gameplayState{-1};
    std::int32_t gameplayMode{-1};
    std::uint16_t hp{};
    std::uint32_t healthState{};
    bool healthReadable{};
    Decision decision{Decision::ContextUnavailable};
};

std::atomic<bool> gInstalled{false};
std::atomic<bool> gEnabled{false};
std::atomic<std::uint64_t> gLocalDeathSequence{0};
std::atomic<std::uint64_t> gLocalLevelExitSequence{0};
// Diagnostics only: last observed level-exit decision, drained on the worker.
std::atomic<std::uint64_t> gLevelExitObserved{0}, gLevelExitAllowed{0}, gLevelExitDrained{0};
std::atomic<std::uint32_t> gLevelExitLastPrimitive{0};
std::atomic<std::int32_t> gLevelExitLastMode{-1};
std::atomic<std::uint64_t> gMissionCompleteObserved{0}, gMissionCompleteDrained{0};
const void* gGlobalRootAddress{};
// Published before HookManager resumes patched threads. Neither destination
// is cleared at logical shutdown: an in-flight naked detour still needs it.
void* gRetailTrampoline{};
const void* gRetailEpilogue{};
std::mutex gEventMutex;
std::array<DeathEvent, 8> gEvents{};
std::size_t gEventCount{};
std::uint64_t gDroppedEvents{};

void RecordEvent(const DeathEvent& event) noexcept {
    std::lock_guard lock(gEventMutex);
    if (gEventCount < gEvents.size()) {
        gEvents[gEventCount++] = event;
    } else {
        ++gDroppedEvents;
    }
}

// This is called by the interior x86 stub, not as a Character virtual.
// At 007B8AAD, EBP still holds the original ECX Character and both definition
// flags (+1C/+1F) already passed. The last cleanup store is at 007B8A95.
int __stdcall DecideLocalDeath(const std::uintptr_t victim) noexcept {
    if (!gEnabled.load(std::memory_order_acquire)) return 0;

    const auto root = crash_guard::SafeRead<std::uintptr_t>(gGlobalRootAddress);
    if (!root || *root == 0U) {
        RecordEvent({victim});
        return 0;
    }
    const auto context = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x08U));
    if (!context || *context == 0U) {
        RecordEvent({victim});
        return 0;
    }
    // Same THE_PLAYER resolution as FUN_0078BAF0 and PlayerSyncObserver;
    // no cached equipment, spawn result, appearance or NPC class heuristic.
    const auto count = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(*context + 0x84U));
    const auto players = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*context + 0x80U));
    if (!count || *count == 0U || !players || *players == 0U) {
        RecordEvent({victim});
        return 0;
    }
    const auto localPlayer = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*players));
    if (!localPlayer || *localPlayer == 0U) {
        RecordEvent({victim});
        return 0;
    }
    if (*localPlayer != victim) return 0;

    DeathEvent event{};
    event.character = victim;
    const auto hp = crash_guard::SafeRead<std::uint16_t>(
        reinterpret_cast<const void*>(victim + 0x33CU));
    const auto health = crash_guard::SafeRead<std::uint32_t>(
        reinterpret_cast<const void*>(victim + 0x32CU));
    event.healthReadable = hp.has_value() && health.has_value();
    if (event.healthReadable) {
        event.hp = *hp;
        event.healthState = *health;
    }

    const auto manager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x48U));
    if (!manager || *manager == 0U) {
        RecordEvent(event);
        return 0;
    }
    const auto state = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*manager + 0x08U));
    const auto mode = crash_guard::SafeRead<std::int32_t>(
        reinterpret_cast<const void*>(*manager + 0x14U));
    if (!state || !mode) {
        RecordEvent(event);
        return 0;
    }
    event.gameplayState = *state;
    event.gameplayMode = *mode;

    // SessionConfig alone is only launcher bootstrap. The existing native
    // authority is bound by a real Host/Client lobby and revoked on disconnect.
    // Native mode also excludes frontend/inactive gameplay exactly as retail.
    // WorldBusActive/AISync/PlayerSync toggles are deliberately irrelevant.
    if (!world_actions::SessionAuthorityBound()) {
        event.decision = Decision::NoSession;
    } else if (*mode == 0 || *mode == 3) {
        event.decision = Decision::NativeMode;
    } else {
        event.decision = Decision::Suppress;
    }
    RecordEvent(event);
    if (event.decision != Decision::Suppress) return 0;
    gLocalDeathSequence.fetch_add(1U, std::memory_order_acq_rel);
    return 1;
}

std::optional<std::int32_t> ReadGameplayMode() noexcept {
    const auto root = crash_guard::SafeRead<std::uintptr_t>(gGlobalRootAddress);
    if (!root || *root == 0U) return std::nullopt;
    const auto manager = crash_guard::SafeRead<std::uintptr_t>(
        reinterpret_cast<const void*>(*root + 0x48U));
    if (!manager || *manager == 0U) return std::nullopt;
    return crash_guard::SafeRead<std::int32_t>(reinterpret_cast<const void*>(*manager + 0x14U));
}

#if defined(_MSC_VER) && defined(_M_IX86)
__declspec(naked) void LocalDeathBoundaryDetour() {
    __asm {
        pushfd
        pushad
        // Preserve x87/MMX/XMM/MXCSR as well as all registers and EFLAGS.
        // ESI is callee-saved by DecideLocalDeath and holds the saved stack.
        mov esi, esp
        sub esp, 0210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push ebp
        call DecideLocalDeath
        test eax, eax
        jz retail

        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        jmp dword ptr [gRetailEpilogue]

    retail:
        fxrstor [esp]
        mov esp, esi
        popad
        popfd
        // Replay the complete displaced A1 imm32, then continue at 007B8AB2.
        jmp dword ptr [gRetailTrampoline]
    }
}
#endif

} // namespace

bool Install(const BuildProfile& profile, const SignatureResolver& resolver,
    HookManager& hooks, const bool exactExecutableHash, std::string& error) {
    if (Installed()) return true;
    if (!exactExecutableHash || !resolver.ExactBuildValidated()) {
        error = "PlayerLifecycle requires the SHA-256-certified executable and fixed-RVA profile.";
        return false;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    static_assert(sizeof(void*) == 4U, "PlayerLifecycle requires the x86 ABI.");
    const auto* boundary = profile.Find(SymbolId::CharacterDeathSoloGameOver);
    const auto* epilogue = resolver.Resolve(SymbolId::CharacterDeathSoloGameOverEpilogue);
    const auto* target = resolver.Resolve(SymbolId::CharacterDeathSoloGameOver);
    const auto* root = resolver.Resolve(SymbolId::GlobalStateRoot);
    if (boundary == nullptr || target == nullptr || epilogue == nullptr || root == nullptr ||
        boundary->approvedPatchLength != 5U || !boundary->trampolineRelocationSafe) {
        error = "PlayerLifecycle death boundary, epilogue or global-root signature is unavailable.";
        return false;
    }
    // A1's absolute operand is masked for PE rebasing, but its loaded value
    // must still point to the exact GlobalStateRoot resolved by the profile.
    const auto operand = crash_guard::SafeRead<std::uintptr_t>(
        static_cast<const std::byte*>(target) + 1U);
    if (!operand || *operand != reinterpret_cast<std::uintptr_t>(root)) {
        error = "PlayerLifecycle displaced MOV does not reference GlobalStateRoot.";
        return false;
    }
    gGlobalRootAddress = root;
    gRetailEpilogue = epilogue;
    gEnabled.store(true, std::memory_order_release);
    HookRequest request{};
    request.name = "PlayerLifecycle.LocalDeathSoloGameOver";
    request.target = const_cast<void*>(target);
    request.detour = reinterpret_cast<void*>(&LocalDeathBoundaryDetour);
    request.patchLength = boundary->approvedPatchLength;
    request.expected = boundary->expected;
    request.relocationSafePrefix = boundary->trampolineRelocationSafe;
    request.trampolineOut = &gRetailTrampoline;
    if (!hooks.Install(request, error)) {
        gEnabled.store(false, std::memory_order_release);
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
#else
    (void)profile;
    (void)hooks;
    error = "PlayerLifecycle requires MSVC x86 interior-hook support.";
    return false;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

std::uint64_t LocalDeathSequence() noexcept {
    return gLocalDeathSequence.load(std::memory_order_acquire);
}

// Same session/mode contract as the death boundary: only an AIMP gameplay
// session suppresses the retail transition; solo play and frontend keep it.
bool DecideLevelExit(const std::uint32_t primitive) noexcept {
    if (!gEnabled.load(std::memory_order_acquire) || !LevelExitPrimitive(primitive)) return false;
    const auto mode = ReadGameplayMode();
    gLevelExitObserved.fetch_add(1U, std::memory_order_relaxed);
    gLevelExitLastPrimitive.store(primitive, std::memory_order_relaxed);
    gLevelExitLastMode.store(mode.value_or(-1), std::memory_order_relaxed);
    if (!world_actions::SessionAuthorityBound() || !mode || *mode == 0 || *mode == 3) {
        gLevelExitAllowed.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gLocalLevelExitSequence.fetch_add(1U, std::memory_order_acq_rel);
    return true;
}

std::uint64_t LocalLevelExitSequence() noexcept {
    return gLocalLevelExitSequence.load(std::memory_order_acquire);
}

void ObserveMissionComplete() noexcept {
    if (!gEnabled.load(std::memory_order_acquire) || !world_actions::SessionAuthorityBound()) return;
    const auto mode = ReadGameplayMode();
    if (!mode || *mode == 0 || *mode == 3) return;
    gMissionCompleteObserved.fetch_add(1U, std::memory_order_relaxed);
    gLocalLevelExitSequence.fetch_add(1U, std::memory_order_acq_rel);
}

void Shutdown() noexcept {
    gEnabled.store(false, std::memory_order_release);
}

void DrainDiagnostics(Diagnostics& diagnostics) {
    std::array<DeathEvent, 8> events{};
    std::size_t count{};
    std::uint64_t dropped{};
    {
        std::lock_guard lock(gEventMutex);
        count = gEventCount;
        events = gEvents;
        dropped = gDroppedEvents;
        gEventCount = 0U;
        gDroppedEvents = 0U;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& event = events[index];
        if (event.decision == Decision::ContextUnavailable) {
            diagnostics.Log("error", "playerlifecycle_context_unavailable",
                "[PlayerLifecycle] death boundary context unavailable; retail path preserved.",
                {{"character", Diagnostics::Pointer(reinterpret_cast<const void*>(event.character))}});
            continue;
        }
        diagnostics.Log("info", "playerlifecycle_local_death_boundary",
            "[PlayerLifecycle] local death boundary reached",
            {{"character", Diagnostics::Pointer(reinterpret_cast<const void*>(event.character))},
             {"healthReadable", Diagnostics::Boolean(event.healthReadable), true},
             {"hp", Diagnostics::Unsigned(event.hp), true},
             {"healthState", Diagnostics::Unsigned(event.healthState), true},
             {"gameplayStateBefore", std::to_string(event.gameplayState), true},
             {"gameplayMode", std::to_string(event.gameplayMode), true}});
        diagnostics.Log("info",
            event.decision == Decision::Suppress
                ? "playerlifecycle_solo_gameover_suppressed" : "playerlifecycle_retail_gameover_allowed",
            event.decision == Decision::Suppress
                ? "[PlayerLifecycle] solo GameOver suppressed"
                : (event.decision == Decision::NoSession
                    ? "[PlayerLifecycle] retail GameOver allowed: no active AIMP session"
                    : "[PlayerLifecycle] retail path allowed: native gameplay mode inactive"));
    }
    const auto observed = gLevelExitObserved.load(std::memory_order_relaxed);
    if (observed != gLevelExitDrained.exchange(observed, std::memory_order_relaxed)) {
        diagnostics.Log("info", "playerlifecycle_level_exit",
            "[PlayerLifecycle] retail level exit (GoToFrontend/SwitchLevel) observed",
            {{"observed", Diagnostics::Unsigned(observed), true},
             {"suppressed", Diagnostics::Unsigned(LocalLevelExitSequence()), true},
             {"retailAllowed", Diagnostics::Unsigned(gLevelExitAllowed.load(std::memory_order_relaxed)), true},
             {"lastPrimitive", Diagnostics::Unsigned(gLevelExitLastPrimitive.load(std::memory_order_relaxed)), true},
             {"lastGameplayMode", std::to_string(gLevelExitLastMode.load(std::memory_order_relaxed)), true}});
    }
    const auto completed = gMissionCompleteObserved.load(std::memory_order_relaxed);
    if (completed != gMissionCompleteDrained.exchange(completed, std::memory_order_relaxed)) {
        diagnostics.Log("info", "playerlifecycle_mission_complete",
            "[PlayerLifecycle] local terminal HUD trigger executed; completion reported before Flash dispatch.",
            {{"observed", Diagnostics::Unsigned(completed), true},
             {"levelExitSequence", Diagnostics::Unsigned(LocalLevelExitSequence()), true}});
    }
    if (dropped != 0U) {
        diagnostics.Log("warning", "playerlifecycle_diagnostics_overflow",
            "[PlayerLifecycle] death diagnostics queue overflow; hook decisions were unaffected.",
            {{"dropped", Diagnostics::Unsigned(dropped), true}});
    }
}

} // namespace aimp::player_lifecycle
