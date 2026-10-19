#include "aimp/MultiplayerPause.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/WorldPublicActions.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include <Windows.h>

namespace aimp::multiplayer_pause {
namespace {

constexpr std::uintptr_t kGlobalRoot = 0x012F0C88U;
constexpr std::uintptr_t kPauseManager = 0x012F194CU;
constexpr std::uintptr_t kRetailMaskAddress = 0x00EF3014U;
constexpr std::uint32_t kRetailMask = 0x000179FFU;
// FUN_0078F7E0 adds 0x2000 for the retail pause menu and 0x10000 for
// platform/challenge overlays. Other reasons, including loading, explicit
// scripted pauses, death screens and system dialogs, remain authoritative.
constexpr std::uint32_t kMenuReasons = 0x00012000U;

std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
std::atomic<bool> gInstalled{};
std::atomic<bool> gEnabled{};
std::atomic<std::uint64_t> gQueries{}, gFiltered{}, gSafetyPreserved{}, gReadFailures{};
std::atomic<std::uint32_t> gLastMenuFlags{}, gLastRemainingReasons{};

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

std::uint32_t FilterMask(const std::uintptr_t pauseManager,
    const std::uint32_t flags, const std::uint32_t mask) noexcept {
    if ((flags & mask & kMenuReasons) == 0U || !ActiveMultiplayerWorld(pauseManager))
        return mask;
    const auto filtered = mask & ~kMenuReasons;
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

#undef AIMP_PAUSE_SAVE_CONTEXT
#undef AIMP_PAUSE_RESTORE_CONTEXT
#undef AIMP_PAUSE_TEST_FLAGS

struct PredicateSite final {
    std::uintptr_t rva;
    bool ecx;
};

// Exact AI.exe SHA-256 is a prerequisite of HookManager::MutationAllowed.
// These are simulation boundaries, including callbacks omitted as LAB_... by
// the C export. No camera, UI event, input-focus or pause-manager mutation.
constexpr std::array<PredicateSite, 21U> kPredicates{{
    {0x003750E7U, false}, // AI_CHARACTER_LOCO_PRE_HAVOK
    {0x00375167U, false}, // HAVOK_UPDATE_DYNAMIC_OBJECTS
    {0x0037519CU, false}, // HAVOK_UPDATE_WORLD
    {0x003751C2U, true},  // HAVOK_UPDATE_WORLD second predicate
    {0x003751E5U, false}, // DYNAMIC_EFFECTS_UPDATE paused argument
    {0x00375233U, true},  // COVER_PER_FRAME_UPDATE
    {0x00375265U, false}, // PATHFINDING_PATH_QUEUE_UPDATE
    {0x003752A5U, false}, // EFFECT_MANAGER_UPDATE
    {0x003834DCU, true},  // HAVOK_PROCESS_RAYCASTS
    {0x003835ADU, false}, // ZONE_MANAGER
    {0x0039F787U, false}, // AI_PROCESS_ACTIVITIES
    {0x003B418BU, false}, // AI_START_THINK
    {0x003C9F38U, false}, // CHARACTER_BUILD_START_PRIMARY_ANIMATION
    {0x003C9F78U, false}, // CHARACTER_BUILD_SECONDARY_ANIMATION
    {0x003C9FE0U, false}, // CHARACTER_IK_AND_RAGDOLL
    {0x003CD667U, false}, // AI_CHARACTER_DISPLAY_ACTIONS
    {0x003CD6ACU, true},  // HAVOK_PRE_CHARACTER_UPDATE
    {0x003CD6FCU, true},  // HAVOK_POST_CHARACTER_UPDATE
    {0x003CD770U, false}, // MAIN_THREAD_CHARACTER_PROCESSING
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
    patches.reserve(kPredicates.size() + 6U);
    for (const auto& site : kPredicates) {
        patches.push_back(CallPatch(site.rva,
            Bytes({0xF7, static_cast<unsigned char>(site.ecx ? 0x41 : 0x40),
                0x04, 0xFF, 0x79, 0x01, 0x00}),
            site.ecx ? &TestEcxShim : &TestEaxShim));
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

    // Publish immutable shim data before the one transactional install resumes
    // process threads. A rejected batch leaves all 27 retail instructions intact.
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

void Shutdown() noexcept {
    gEnabled.store(false, std::memory_order_release);
}

void LogDiagnostics(Diagnostics& diagnostics) {
    diagnostics.Log("info", "multiplayer_simulation_pause",
        "Certified simulation queries ignore retail menu/overlay pauses in an active multiplayer world; native UI, input focus and other pause reasons remain intact.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"enabled", Diagnostics::Boolean(gEnabled.load()), true},
         {"sites", "27", true},
         {"menuReasonMask", Diagnostics::Unsigned(kMenuReasons), true},
         {"queries", Diagnostics::Unsigned(gQueries.load()), true},
         {"filtered", Diagnostics::Unsigned(gFiltered.load()), true},
         {"safetyReasonsPreserved", Diagnostics::Unsigned(gSafetyPreserved.load()), true},
         {"lastMenuFlags", Diagnostics::Unsigned(gLastMenuFlags.load()), true},
         {"lastRemainingReasons", Diagnostics::Unsigned(gLastRemainingReasons.load()), true},
         {"contextReadFailures", Diagnostics::Unsigned(gReadFailures.load()), true}});
}

} // namespace aimp::multiplayer_pause
