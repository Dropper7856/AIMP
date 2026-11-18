#include "aimp/LevelIntegrityGuard.h"
#include "aimp/HookManager.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <Windows.h>

namespace aimp::level_integrity_guard {
namespace {
std::atomic<bool> gInstalled{};
std::atomic<bool> gAlreadyBypassed{};

// CALL FUN_00774CA0 (file fingerprint; cdecl, caller pops 0Ch, then TEST AL).
// 007DBFA4: data\ENV\<level>\RENDERABLE\models_level.bin against 34 entries.
// 009E8BA5: owned DLC content against 8 entries.
// A failed check calls FUN_00A14770, which sets the quit flag the main loop
// polls: the game exits with code 0 during level load.
struct Site {
    std::uintptr_t rva;
    std::array<std::uint8_t, 5U> retail;
    std::array<std::uint8_t, 5U> openCage;
};
constexpr std::array<Site, 2U> kSites{{
    {0x003DBFA4U, {0xE8, 0xF7, 0x8C, 0xF9, 0xFF}, {0xE8, 0x67, 0x28, 0x25, 0x00}},
    {0x005E8BA5U, {0xE8, 0xF6, 0xC0, 0xD8, 0xFF}, {0xE8, 0x66, 0x5C, 0x04, 0x00}},
}};
// MOV AL,1 + NOPs: "fingerprint matched". The caller's ADD ESP,0C still pops
// the three pushed arguments.
constexpr std::array<std::uint8_t, 5U> kPass{0xB0, 0x01, 0x90, 0x90, 0x90};

bool SiteEquals(const std::uintptr_t base, const Site& site, const std::array<std::uint8_t, 5U>& bytes) {
    return std::memcmp(reinterpret_cast<const void*>(base + site.rva), bytes.data(), bytes.size()) == 0;
}

// OpenCAGE "current-gen script fixes" (CathodeLib DisableCurrentGenOptimisations):
// at 008D7BD5 the retail exe calls FUN_00630590, which drops entities whose
// requires_script_for_current_gen is false; OpenCAGE retargets the CALL to a
// MOV AL,1;RET stub. Levels edited with OpenCAGE (the developer test levels)
// rely on it, and the dev PC's exe has it on disk while retail does not.
constexpr Site kCurrentGenSite{0x004D7BD5U, {0xE8, 0xB6, 0x89, 0xD5, 0xFF}, {0xE8, 0x16, 0xF5, 0x5C, 0x00}};
std::atomic<bool> gCurrentGenScripts{};

void InstallCurrentGenScripts(const std::uintptr_t base, HookManager& hooks) {
    if (SiteEquals(base, kCurrentGenSite, kCurrentGenSite.openCage)) {
        gCurrentGenScripts.store(true, std::memory_order_release);
        return;
    }
    std::array<BytePatchRequest, 1U> patch{};
    patch[0].target = reinterpret_cast<void*>(base + kCurrentGenSite.rva);
    for (const auto value : kCurrentGenSite.retail) patch[0].expected.push_back(static_cast<std::byte>(value));
    for (const auto value : kPass) patch[0].replacement.push_back(static_cast<std::byte>(value));
    // Optional: a refusal leaves only edited levels' extra scripts off.
    std::string ignored;
    if (hooks.InstallPatchBatch("LevelIntegrityGuard.CurrentGenScripts", patch, ignored))
        gCurrentGenScripts.store(true, std::memory_order_release);
}
}

bool CurrentGenScriptsEnabled() noexcept { return gCurrentGenScripts.load(std::memory_order_acquire); }

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "Level integrity bypass requires the certified executable";
        return false;
    }
    if (SiteEquals(base, kSites[0], kSites[0].openCage) && SiteEquals(base, kSites[1], kSites[1].openCage)) {
        gAlreadyBypassed.store(true, std::memory_order_release);
        InstallCurrentGenScripts(base, hooks);
        gInstalled.store(true, std::memory_order_release);
        return true;
    }
    std::array<BytePatchRequest, kSites.size()> patches{};
    for (std::size_t i = 0U; i < kSites.size(); ++i) {
        patches[i].target = reinterpret_cast<void*>(base + kSites[i].rva);
        for (const auto value : kSites[i].retail) patches[i].expected.push_back(static_cast<std::byte>(value));
        for (const auto value : kPass) patches[i].replacement.push_back(static_cast<std::byte>(value));
    }
    if (!hooks.InstallPatchBatch("LevelIntegrityGuard.FingerprintBypass", patches, error)) return false;
    InstallCurrentGenScripts(base, hooks);
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
bool AlreadyBypassed() noexcept { return gAlreadyBypassed.load(std::memory_order_acquire); }
}

namespace aimp::level_integrity_guard {
namespace {
// FUN_007e3b60 parses MAIN.PKG <game_data><levels>: for each <level id path>
// it calls FUN_0077b340 (lookup by id) on the level registry
// *(*(0x016F0C88)+0x3C) and, when absent, FUN_0079a6b0(id, "STD_LEVEL", path)
// (both __thiscall; the registry copies the strings). load_level only accepts
// registered ids, and editing MAIN.PKG makes retail quit at boot, so the
// developer test levels are registered here, in memory, the same way.
constexpr std::uintptr_t kRegistryRootRva = 0x012F0C88U;
constexpr std::uintptr_t kLookupRva = 0x0037B340U;
constexpr std::uintptr_t kRegisterRva = 0x0039A6B0U;
constexpr std::uintptr_t kStdLevelRva = 0x00EF4C4CU;   // "STD_LEVEL"
constexpr int kRegistryCapacity = 160;                 // slots 0x50..0x2D0
using LookupFn = void*(__thiscall*)(void*, const char*);
using RegisterFn = void(__thiscall*)(void*, const char*, const char*, const char*);

int RegisterLevelGuarded(const std::uintptr_t base, const char* id, const char* path) noexcept {
    __try {
        const auto root = *reinterpret_cast<const std::uintptr_t*>(base + kRegistryRootRva);
        if (root == 0U) return -1;
        auto* registry = *reinterpret_cast<void* const*>(root + 0x3CU);
        if (registry == nullptr) return -2;
        const auto count = *reinterpret_cast<const int*>(reinterpret_cast<std::uintptr_t>(registry) + 0x2D0U);
        if (std::strncmp(reinterpret_cast<const char*>(base + kStdLevelRva), "STD_LEVEL", 10) != 0) return -5;
        if (reinterpret_cast<LookupFn>(base + kLookupRva)(registry, id) != nullptr) return 0;
        if (count < 0 || count >= kRegistryCapacity - 1) return -3;
        reinterpret_cast<RegisterFn>(base + kRegisterRva)(
            registry, id, reinterpret_cast<const char*>(base + kStdLevelRva), path);
        return reinterpret_cast<LookupFn>(base + kLookupRva)(registry, id) != nullptr ? 1 : -6;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -4;
    }
}
}

std::vector<TestLevelRegistration> RegisterTestLevels(const std::uintptr_t base) noexcept {
    std::vector<TestLevelRegistration> results;
    if (base == 0U) return results;
    char exe[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (length == 0U || length >= MAX_PATH) return results;
    std::string game(exe, length);
    game.resize(game.find_last_of("\\/") + 1U);
    for (const char* name : {"WORLDSYNC_TEST_LEVEL", "AISYNC_TEST_LEVEL"}) {
        const auto commands = game + "DATA\\ENV\\PRODUCTION\\" + name + "\\WORLD\\COMMANDS.PAK";
        if (GetFileAttributesA(commands.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const auto id = std::string("Production\\") + name;
        const auto path = std::string("DATA\\ENV\\Production\\") + name;
        results.push_back({name, RegisterLevelGuarded(base, id.c_str(), path.c_str())});
    }
    return results;
}
}
