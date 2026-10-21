#include "aimp/MultiplayerPause.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldPublicActions.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

namespace aimp::multiplayer_pause {
namespace {

constexpr std::uintptr_t kGlobalRoot = 0x012F0C88U;
constexpr std::uintptr_t kPauseManager = 0x012F194CU;
constexpr std::uintptr_t kUiManager = 0x0134A78CU;
constexpr std::uintptr_t kHubType = 0x00EFF260U;
constexpr std::uintptr_t kRenderer = 0x015D0970U;
constexpr std::uintptr_t kRetailMaskAddress = 0x00EF3014U;
constexpr std::uint32_t kRetailMask = 0x000179FFU;
// FUN_0078F7E0 adds 0x2000 for the retail pause menu and 0x10000 for
// platform/challenge overlays. Other reasons, including loading, explicit
// scripted pauses, death screens and system dialogs, remain authoritative.
constexpr std::uint32_t kMenuReasons = 0x00012000U;
constexpr std::uint32_t kPauseGameReason = 0x401U;

std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
std::atomic<bool> gInstalled{};
std::atomic<bool> gEnabled{};
std::atomic<bool> gLoadingHold{};
std::mutex gLoadingMutex;
std::string gLoadingSession;
std::uint32_t gLoadingGeneration{};
bool gLoadingReleased{};
std::unordered_map<std::string, std::uint32_t> gReleasedLoadingGenerations;
std::atomic<std::uint64_t> gLoadingStartupPasses{}, gLoadingActorHolds{};
std::atomic<std::uint64_t> gLoadingReadinessCalls{}, gLoadingReadinessFaults{};
std::atomic<std::uint64_t> gLoadingReadinessFaultGeneration{};
std::atomic<std::uint64_t> gQueries{}, gFiltered{}, gSafetyPreserved{}, gReadFailures{};
std::atomic<std::uint32_t> gLastMenuFlags{}, gLastRemainingReasons{};
std::atomic<std::uint64_t> gPauseGameFiltered{}, gUiContextRejected{};
std::atomic<std::uint64_t> gRenderQueries{}, gRenderFiltered{};

template<typename T>
bool Read(const std::uintptr_t address, T& result) noexcept {
#if defined(_MSC_VER)
    // These scalar reads run in native frame/worker callbacks. Avoid a
    // VirtualQuery per field while still rejecting a disappearing context.
    __try {
        result = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return crash_guard::TryCopy(&result, reinterpret_cast<const void*>(address), sizeof(T));
#endif
}

bool ActiveMultiplayerWorld(const std::uintptr_t pauseManager) noexcept {
    if (!gEnabled.load(std::memory_order_acquire) ||
        !world_actions::SessionAuthorityBound() || gGenerations == nullptr)
        return false;
    const auto generation = gGenerations->CurrentGeneration();
    if (!gGenerations->IsStableGeneration(generation)) return false;
    const auto snapshot = gGenerations->Snapshot();
    if (snapshot.generation != generation || snapshot.stability != LevelStability::Stable)
        return false;
    std::uintptr_t currentPause{}, root{}, characterManager{}, gameplay{};
    std::uint32_t state{}, mode{};
    if (!Read(gBase + kPauseManager, currentPause) || currentPause != pauseManager ||
        !Read(gBase + kGlobalRoot, root) || root == 0U || root != snapshot.roots.globalRoot ||
        !Read(root + 0x08U, characterManager) ||
        characterManager != snapshot.roots.characterManager ||
        !Read(root + 0x48U, gameplay) || gameplay == 0U ||
        !Read(gameplay + 0x08U, state) || state != 4U ||
        !Read(gameplay + 0x14U, mode) || mode == 0U || mode == 3U)
        return false;
    // A transition revokes the generation token before changing roots. No
    // local-player health test: a dead Host still owns the world simulation.
    return gEnabled.load(std::memory_order_acquire) &&
        world_actions::SessionAuthorityBound() &&
        gGenerations->IsStableGeneration(generation);
}

bool NativeMenuOwnsPause() noexcept {
    // 00810420's exact movie index, without invoking UI code from a simulation
    // worker or keeping a movie pointer beyond this query. +1CE2 belongs to the
    // HUB movie, NOT to the smaller UI manager. Pause_Game/Unpause_Game own it.
    std::uintptr_t manager{}, movies{};
    std::uint32_t count{};
    if (!Read(gBase + kUiManager, manager) || manager == 0U ||
        !Read(manager + 0x38U, movies) || movies == 0U ||
        !Read(manager + 0x3CU, count) || count == 0U || count > 256U) return false;
    bool owned = false;
    for (std::uint32_t i = 0U; i < count && !owned; ++i) {
        std::uintptr_t movie{}, type{}, name{};
        std::uint8_t active{}, pauseOwned{};
        if (!Read(movies + i * sizeof(std::uintptr_t), movie) || movie == 0U || !Read(movie, type)) continue;
        if (type == gBase + kHubType && Read(movie + 0x1CE2U, pauseOwned) && pauseOwned != 0U)
            owned = true;
        else if (Read(movie + 0x0CU, active) && active != 0U &&
            Read(movie + 0x34U, name) && name != 0U) {
            constexpr char kPauseMenu[] = "pause_menu";
            owned = true;
            for (std::size_t j = 0U; j < sizeof(kPauseMenu); ++j) {
                char actual{};
                if (!Read(name + j, actual) || actual != kPauseMenu[j]) { owned = false; break; }
            }
        }
    }
    std::uintptr_t currentManager{}, currentMovies{};
    std::uint32_t currentCount{};
    if (!Read(gBase + kUiManager, currentManager) || currentManager != manager ||
        !Read(manager + 0x38U, currentMovies) || currentMovies != movies ||
        !Read(manager + 0x3CU, currentCount) || currentCount != count) {
        gUiContextRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    return owned;
}

std::uint32_t FilterMask(const std::uintptr_t pauseManager,
    const std::uint32_t flags, const std::uint32_t mask) noexcept {
    const bool pauseGame = (flags & kPauseGameReason) == kPauseGameReason;
    if ((flags & mask & (kMenuReasons | 1U)) == 0U || !ActiveMultiplayerWorld(pauseManager))
        return mask;
    auto ignored = kMenuReasons;
    if (pauseGame && ((flags & kMenuReasons) != 0U || NativeMenuOwnsPause())) {
        // 00807010 and the Escape path in 00790000 add the SAME 0x401 pair.
        // Ignore its blocking bit only with a certified active menu owner.
        ignored |= kPauseGameReason;
        gPauseGameFiltered.fetch_add(1U, std::memory_order_relaxed);
    }
    const auto filtered = mask & ~ignored;
    gLastMenuFlags.store(flags, std::memory_order_relaxed);
    gLastRemainingReasons.store(flags & filtered, std::memory_order_relaxed);
    gFiltered.fetch_add(1U, std::memory_order_relaxed);
    if ((flags & filtered) != 0U)
        gSafetyPreserved.fetch_add(1U, std::memory_order_relaxed);
    return filtered;
}

// The TEST shims pass the original instruction's register, rather than
// substituting a newly loaded global pointer across a generation change.
std::uint32_t __cdecl SimulationPauseBits(const std::uintptr_t pauseManager) noexcept {
    gQueries.fetch_add(1U, std::memory_order_relaxed);
    std::uint32_t flags{};
    if (pauseManager == 0U || !Read(pauseManager + 4U, flags)) {
        gReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return kRetailMask; // retain the paused branch when the context vanished
    }
    return flags & FilterMask(pauseManager, flags, kRetailMask);
}

bool LoadingWorldHasPlayer(const std::uintptr_t pauseManager) noexcept {
    if (!ActiveMultiplayerWorld(pauseManager)) return false;
    // Existing PlayerSyncObserver / PlayerLifecycle THE_PLAYER resolution. Do
    // not stop the updates responsible for creating and registering that player.
    std::uintptr_t root{}, context{}, players{}, player{}, equipment{}, presentation{};
    std::uint32_t count{};
    std::int32_t state{};
    float x{}, y{}, z{};
    if (!Read(gBase + kGlobalRoot, root) || root == 0U ||
        !Read(root + 0x08U, context) || context == 0U ||
        !Read(context + 0x84U, count) || count == 0U ||
        !Read(context + 0x80U, players) || players == 0U ||
        !Read(players, player) || player == 0U ||
        !Read(player + 0x1100U, equipment) || equipment == 0U ||
        !Read(equipment + 0x152CU, presentation) || presentation == 0U ||
        !Read(presentation + 0x04U, state) || state < 0 || state > 64 ||
        !Read(player + 0x70U, x) || !Read(player + 0x74U, y) || !Read(player + 0x78U, z))
        return false;
    std::uintptr_t currentRoot{};
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::abs(x) < 100000.0F && std::abs(y) < 100000.0F && std::abs(z) < 100000.0F &&
        Read(gBase + kGlobalRoot, currentRoot) && currentRoot == root &&
        ActiveMultiplayerWorld(pauseManager);
}

// Arm survives transitions, but actor hold starts only in an initialized world.
// Retail streaming/player creation must run to produce the readiness evidence.
std::uint32_t __cdecl LoadingSimulationPauseBits(const std::uintptr_t pauseManager) noexcept {
    if (gEnabled.load(std::memory_order_acquire) && gLoadingHold.load(std::memory_order_acquire)) {
        if (LoadingWorldHasPlayer(pauseManager)) {
            gLoadingActorHolds.fetch_add(1U, std::memory_order_relaxed);
            return kRetailMask;
        }
        gLoadingStartupPasses.fetch_add(1U, std::memory_order_relaxed);
    }
    return SimulationPauseBits(pauseManager);
}

std::uint32_t __cdecl SimulationPauseMask() noexcept {
    gQueries.fetch_add(1U, std::memory_order_relaxed);
    std::uint32_t mask{kRetailMask}, flags{};
    std::uintptr_t pauseManager{};
    if (!Read(gBase + kRetailMaskAddress, mask) ||
        !Read(gBase + kPauseManager, pauseManager) || pauseManager == 0U ||
        !Read(pauseManager + 4U, flags)) {
        gReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return mask;
    }
    return FilterMask(pauseManager, flags, mask);
}

std::uint32_t __cdecl SimulationRenderByte(const std::uintptr_t renderer) noexcept {
    std::uint8_t enabled{};
    if (!Read(renderer + 0x1B64U, enabled)) {
        gReadFailures.fetch_add(1U, std::memory_order_relaxed);
        return 0U;
    }
    // Keep the renderer's own state intact. Only its certified query sites
    // admit live world updates under a menu that no longer pauses simulation.
    if (enabled != 0U || renderer != gBase + kRenderer) return enabled;
    gRenderQueries.fetch_add(1U, std::memory_order_relaxed);
    std::uintptr_t pauseManager{};
    std::uint32_t flags{};
    if (!Read(gBase + kPauseManager, pauseManager) || pauseManager == 0U ||
        !Read(pauseManager + 4U, flags) || (flags & (kMenuReasons | 1U)) == 0U) return enabled;
    const auto mask = FilterMask(pauseManager, flags, kRetailMask);
    if (mask == kRetailMask || (flags & mask) != 0U) return enabled;
    gRenderFiltered.fetch_add(1U, std::memory_order_relaxed);
    return 1U;
}

#if defined(_MSC_VER) && defined(_M_IX86)

// CALL replaces one complete instruction. Its ordinary return lands on the
// padding NOPs, then the original instruction following that boundary. After
// PUSHFD/PUSHAD, EDI indexes saved ESI/+04, EDX/+14, ECX/+18, EAX/+1C,
// EFLAGS/+20. The original return address remains untouched at +24.
#define AIMP_PAUSE_SAVE_CONTEXT \
    __asm { pushfd } \
    __asm { pushad } \
    __asm { mov edi, esp } \
    __asm { sub esp, 0210h } \
    __asm { and esp, 0FFFFFFF0h } \
    __asm { fxsave [esp] } \
    __asm { cld }

#define AIMP_PAUSE_RESTORE_CONTEXT \
    __asm { fxrstor [esp] } \
    __asm { mov esp, edi } \
    __asm { popad } \
    __asm { popfd } \
    __asm { ret }

// Merge only the defined arithmetic flags of TEST. In particular preserve
// DF/IF and all other original flags despite CLD and the C++ helper call.
// AF is undefined for retail TEST and is conservatively left unchanged.
#define AIMP_PAUSE_TEST_FLAGS \
    __asm { test eax, eax } \
    __asm { pushfd } \
    __asm { pop eax } \
    __asm { and eax, 000008C5h } \
    __asm { mov edx, [edi + 020h] } \
    __asm { and edx, 0FFFFF73Ah } \
    __asm { or edx, eax } \
    __asm { mov [edi + 020h], edx }

__declspec(naked) void TestEaxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 01Ch]
        call SimulationPauseBits
        add esp, 4
    }
    AIMP_PAUSE_TEST_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void TestEcxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 018h]
        call SimulationPauseBits
        add esp, 4
    }
    AIMP_PAUSE_TEST_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void LoadingTestEaxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 01Ch]
        call LoadingSimulationPauseBits
        add esp, 4
    }
    AIMP_PAUSE_TEST_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void LoadingTestEcxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 018h]
        call LoadingSimulationPauseBits
        add esp, 4
    }
    AIMP_PAUSE_TEST_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void MaskEaxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        call SimulationPauseMask
        mov [edi + 01Ch], eax
    }
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void MaskEcxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        call SimulationPauseMask
        mov [edi + 018h], eax
    }
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void MaskEdxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        call SimulationPauseMask
        mov [edi + 014h], eax
    }
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void MaskEsiShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        call SimulationPauseMask
        mov [edi + 004h], eax
    }
    AIMP_PAUSE_RESTORE_CONTEXT
}

// These sites originally compare one byte, so SF and parity come from AL.
// Unlike TEST, CMP also defines AF: merge it along with the other arithmetic
// flags, preserving DF/IF and all non-arithmetic caller state.
#define AIMP_PAUSE_CMP_FLAGS \
    __asm { cmp al, 0 } \
    __asm { pushfd } \
    __asm { pop eax } \
    __asm { and eax, 000008D5h } \
    __asm { mov edx, [edi + 020h] } \
    __asm { and edx, 0FFFFF72Ah } \
    __asm { or edx, eax } \
    __asm { mov [edi + 020h], edx }

__declspec(naked) void RenderEcxShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 018h]
        call SimulationRenderByte
        add esp, 4
    }
    AIMP_PAUSE_CMP_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

__declspec(naked) void RenderEsiShim() {
    AIMP_PAUSE_SAVE_CONTEXT
    __asm {
        push [edi + 004h]
        call SimulationRenderByte
        add esp, 4
    }
    AIMP_PAUSE_CMP_FLAGS
    AIMP_PAUSE_RESTORE_CONTEXT
}

#undef AIMP_PAUSE_CMP_FLAGS
#undef AIMP_PAUSE_SAVE_CONTEXT
#undef AIMP_PAUSE_RESTORE_CONTEXT
#undef AIMP_PAUSE_TEST_FLAGS

struct PredicateSite final {
    std::uintptr_t rva;
    bool ecx;
    bool holdActors{true};
};

// Exact AI.exe SHA-256 is a prerequisite of HookManager::MutationAllowed.
// These are simulation boundaries, including callbacks omitted as LAB_... by
// the C export. No camera, UI event, input-focus or pause-manager mutation.
// Keep the native physics producer/consumer pipeline running under the loading
// overlay. Holding Havok while animation/initialization queues transform commands
// overflows FUN_007EC250's bounded queue (Host crash 230, capacity 10000).
// AI decisions and action execution remain held until the session release.
constexpr std::array<PredicateSite, 21U> kPredicates{{
    {0x003750E7U, false, false}, // AI_CHARACTER_LOCO_PRE_HAVOK
    {0x00375167U, false, false}, // HAVOK_UPDATE_DYNAMIC_OBJECTS
    {0x0037519CU, false, false}, // HAVOK_UPDATE_WORLD
    {0x003751C2U, true, false},  // HAVOK_UPDATE_WORLD second predicate
    {0x003751E5U, false}, // DYNAMIC_EFFECTS_UPDATE paused argument
    {0x00375233U, true, false},  // COVER_PER_FRAME_UPDATE
    {0x00375265U, false, false}, // PATHFINDING_PATH_QUEUE_UPDATE
    {0x003752A5U, false}, // EFFECT_MANAGER_UPDATE
    {0x003834DCU, true, false},  // HAVOK_PROCESS_RAYCASTS
    {0x003835ADU, false, false}, // ZONE_MANAGER
    {0x0039F787U, false}, // AI_PROCESS_ACTIVITIES
    {0x003B418BU, false}, // AI_START_THINK
    {0x003C9F38U, false, false}, // CHARACTER_BUILD_START_PRIMARY_ANIMATION
    {0x003C9F78U, false, false}, // CHARACTER_BUILD_SECONDARY_ANIMATION
    {0x003C9FE0U, false, false}, // CHARACTER_IK_AND_RAGDOLL
    {0x003CD667U, false}, // AI_CHARACTER_DISPLAY_ACTIONS
    {0x003CD6ACU, true, false},  // HAVOK_PRE_CHARACTER_UPDATE
    {0x003CD6FCU, true, false},  // HAVOK_POST_CHARACTER_UPDATE
    {0x003CD770U, false, false}, // MAIN_THREAD_CHARACTER_PROCESSING
    {0x003D4F90U, false}, // AI_COMBAT_MANAGER_MAIN
    {0x003D6347U, false}, // AI_CHARACTER_LOGIC_ACTIONS
}};

std::vector<std::byte> Bytes(const std::initializer_list<unsigned char> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

std::vector<std::byte> MaskLoadBytes(const std::initializer_list<unsigned char> opcode) {
    auto result = Bytes(opcode);
    // The preferred-base bytes contain VA 012F3014. The loader relocates that
    // operand when AI.exe is rebased; certify against the actual module VA.
    const auto operand = static_cast<std::uint32_t>(gBase + kRetailMaskAddress);
    for (unsigned shift = 0U; shift != 32U; shift += 8U)
        result.push_back(static_cast<std::byte>((operand >> shift) & 0xFFU));
    return result;
}

BytePatchRequest CallPatch(const std::uintptr_t rva,
    std::vector<std::byte> expected, void (*shim)()) {
    BytePatchRequest patch{};
    const auto address = gBase + rva;
    patch.target = reinterpret_cast<void*>(address);
    patch.expected = std::move(expected);
    patch.replacement.assign(patch.expected.size(), std::byte{0x90});
    patch.replacement[0] = std::byte{0xE8};
    // x86 rel32 arithmetic wraps modulo 2^32, covering the whole address space.
    const auto relative = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(shim) - (address + 5U));
    std::memcpy(patch.replacement.data() + 1U, &relative, sizeof(relative));
    return patch;
}

#endif
} // namespace

bool Install(const std::uintptr_t moduleBase, HookManager& hooks,
    LevelGenerationTracker& generations, std::string& error) {
    if (Installed()) return true;
    if (moduleBase == 0U || !hooks.MutationAllowed()) {
        error = "Multiplayer pause requires the SHA-256-certified executable and hook opt-in.";
        return false;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    static_assert(sizeof(void*) == 4U, "Multiplayer pause requires the retail x86 ABI.");
    std::uint32_t nativeMask{};
    if (!Read(moduleBase + kRetailMaskAddress, nativeMask) || nativeMask != kRetailMask) {
        error = "Multiplayer pause retail simulation mask mismatch.";
        return false;
    }
    gBase = moduleBase;
    gGenerations = &generations;
    std::vector<BytePatchRequest> patches;
    patches.reserve(kPredicates.size() + 6U + 15U);
    for (const auto& site : kPredicates) {
        patches.push_back(CallPatch(site.rva,
            Bytes({0xF7, static_cast<unsigned char>(site.ecx ? 0x41 : 0x40),
                0x04, 0xFF, 0x79, 0x01, 0x00}),
            site.holdActors
                ? (site.ecx ? &LoadingTestEcxShim : &LoadingTestEaxShim)
                : (site.ecx ? &TestEcxShim : &TestEaxShim)));
    }
    // The following complete MOVs load the mask for short TEST instructions.
    // Replacing only the MOV keeps all following flags/register consumers and
    // branch destinations intact. ESI at 00790129 feeds precisely two TESTs,
    // 0079012E/00790162, before its next assignment at 007903C4.
    patches.push_back(CallPatch(0x0005C52DU, MaskLoadBytes({0x8B,0x0D}), &MaskEcxShim));
    patches.push_back(CallPatch(0x0027A523U, MaskLoadBytes({0x8B,0x15}), &MaskEdxShim));
    patches.push_back(CallPatch(0x002B27D5U, MaskLoadBytes({0x8B,0x0D}), &MaskEcxShim));
    patches.push_back(CallPatch(0x002B44A7U, MaskLoadBytes({0x8B,0x15}), &MaskEdxShim));
    patches.push_back(CallPatch(0x003DDBBFU, MaskLoadBytes({0xA1}), &MaskEaxShim));
    patches.push_back(CallPatch(0x00390129U, Bytes({0xBE,0xFF,0x79,0x01,0x00}), &MaskEsiShim));

    // 009F22A0 disables the world's render-update queues while a menu is up.
    // Unpausing physics alone would still leave a frozen scene behind the UI.
    // Preserve the native field and change all 15 certified consumer queries.
    for (std::uintptr_t i = 0U; i < 14U; ++i)
        patches.push_back(CallPatch(0x00600CD0U + i * 0x30U,
            Bytes({0x80,0xB9,0x64,0x1B,0x00,0x00,0x00}), &RenderEcxShim));
    patches.push_back(CallPatch(0x006048D3U,
        Bytes({0x80,0xBE,0x64,0x1B,0x00,0x00,0x00}), &RenderEsiShim));

    // Publish immutable shim data before the one transactional install resumes
    // process threads. A rejected batch leaves all 42 retail instructions intact.
    gEnabled.store(true, std::memory_order_release);
    if (!hooks.InstallPatchBatch("multiplayer_simulation_pause_queries", patches, error)) {
        gEnabled.store(false, std::memory_order_release);
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
#else
    (void)generations;
    error = "Multiplayer pause requires the MSVC x86 instruction shim.";
    return false;
#endif
}

bool Installed() noexcept {
    return gInstalled.load(std::memory_order_acquire);
}

bool SetLoadingHold(const bool active, const std::string& sessionId, const std::uint32_t generation) {
    if (!Installed() || sessionId.empty() || generation == 0U) return false;
    std::lock_guard lock(gLoadingMutex);
    if (active) {
        // Repeated arm is idempotent; an obsolete request cannot replace a hold.
        const auto released = gReleasedLoadingGenerations.find(sessionId);
        if ((released != gReleasedLoadingGenerations.end() && released->second >= generation) ||
            (gLoadingHold.load(std::memory_order_acquire) && gLoadingSession != sessionId) ||
            (gLoadingSession == sessionId && (gLoadingGeneration > generation ||
                (gLoadingGeneration == generation && gLoadingReleased))))
            return false;
        gLoadingSession = sessionId;
        gLoadingGeneration = generation;
        gLoadingReleased = false;
        gLoadingHold.store(true, std::memory_order_release);
        return true;
    }
    if (gLoadingSession != sessionId || gLoadingGeneration != generation) return false;
    gReleasedLoadingGenerations[sessionId] = generation;
    gLoadingReleased = true;
    gLoadingHold.store(false, std::memory_order_release);
    return true;
}

void ClearLoadingHold() noexcept {
    std::lock_guard lock(gLoadingMutex);
    gLoadingHold.store(false, std::memory_order_release);
    gLoadingSession.clear();
    gLoadingGeneration = 0U;
    gLoadingReleased = false;
    gReleasedLoadingGenerations.clear();
}

bool LoadingHeld() noexcept { return gLoadingHold.load(std::memory_order_acquire); }

void PumpLoadingCharacterReadiness(void* readinessProducer) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    if (!LoadingHeld() || readinessProducer == nullptr || gGenerations == nullptr) return;
    const auto generation = gGenerations->CurrentGeneration();
    if (!gGenerations->IsStableGeneration(generation) ||
        gLoadingReadinessFaultGeneration.load(std::memory_order_acquire) == generation) return;
    std::uintptr_t pause{}, root{}, manager{};
    std::uint32_t count{};
    if (!Read(gBase + kPauseManager, pause) || !LoadingWorldHasPlayer(pause) ||
        !Read(gBase + kGlobalRoot, root) || root == 0U ||
        !Read(root + 0x08U, manager) || manager == 0U ||
        !Read(manager + 0x4370U, count) || count > 36U) return;
    // FUN_007CC080 first visits this fixed queue, calling FUN_007A76E0 for
    // each (spawnKey, recordIndex). Its remaining actor updates stay paused.
    // 36 records of 0x1D0 bytes fill [manager+0x230, manager+0x4370).
    // Never synthesize a Ready result or edit the native queue/record state.
    using Producer = void(__thiscall*)(void*, std::uint32_t, void*);
    for (std::uint32_t i = 0U; i < count; ++i) {
        std::uint32_t key{}, index{};
        std::uintptr_t currentRoot{}, currentManager{};
        if (!LoadingHeld() || !gGenerations->IsStableGeneration(generation) ||
            !Read(gBase + kGlobalRoot, currentRoot) || currentRoot != root ||
            !Read(root + 0x08U, currentManager) || currentManager != manager ||
            !Read(manager + 0x4374U + i * 8U, key) ||
            !Read(manager + 0x4378U + i * 8U, index) || index >= 36U) return;
        __try {
            reinterpret_cast<Producer>(readinessProducer)(reinterpret_cast<void*>(manager), key,
                reinterpret_cast<void*>(manager + 0x230U + index * 0x1D0U));
            gLoadingReadinessCalls.fetch_add(1U, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gLoadingReadinessFaults.fetch_add(1U, std::memory_order_relaxed);
            gLoadingReadinessFaultGeneration.store(generation, std::memory_order_release);
            return; // quarantine this generation; do not retry uncertain native state
        }
    }
#else
    (void)readinessProducer;
#endif
}

void Shutdown() noexcept {
    ClearLoadingHold();
    gEnabled.store(false, std::memory_order_release);
}

void LogDiagnostics(Diagnostics& diagnostics) {
    diagnostics.Log("info", "multiplayer_simulation_pause",
        "Certified simulation queries ignore retail menu/overlay pauses in an active multiplayer world; native UI, input focus and other pause reasons remain intact.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"enabled", Diagnostics::Boolean(gEnabled.load()), true},
         {"sites", "42", true},
         {"loadingHold", Diagnostics::Boolean(LoadingHeld()), true},
         {"loadingStartupPasses", Diagnostics::Unsigned(gLoadingStartupPasses.load()), true},
         {"loadingActorHolds", Diagnostics::Unsigned(gLoadingActorHolds.load()), true},
         {"loadingReadinessCalls", Diagnostics::Unsigned(gLoadingReadinessCalls.load()), true},
         {"loadingReadinessFaults", Diagnostics::Unsigned(gLoadingReadinessFaults.load()), true},
         {"menuReasonMask", Diagnostics::Unsigned(kMenuReasons), true},
         {"pauseGameReason", Diagnostics::Unsigned(kPauseGameReason), true},
         {"queries", Diagnostics::Unsigned(gQueries.load()), true},
         {"filtered", Diagnostics::Unsigned(gFiltered.load()), true},
         {"pauseGameFiltered", Diagnostics::Unsigned(gPauseGameFiltered.load()), true},
         {"renderQueries", Diagnostics::Unsigned(gRenderQueries.load()), true},
         {"renderFiltered", Diagnostics::Unsigned(gRenderFiltered.load()), true},
         {"uiContextRejected", Diagnostics::Unsigned(gUiContextRejected.load()), true},
         {"safetyReasonsPreserved", Diagnostics::Unsigned(gSafetyPreserved.load()), true},
         {"lastMenuFlags", Diagnostics::Unsigned(gLastMenuFlags.load()), true},
         {"lastRemainingReasons", Diagnostics::Unsigned(gLastRemainingReasons.load()), true},
         {"contextReadFailures", Diagnostics::Unsigned(gReadFailures.load()), true}});
}

} // namespace aimp::multiplayer_pause
