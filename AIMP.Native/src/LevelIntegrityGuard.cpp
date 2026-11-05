#include "aimp/LevelIntegrityGuard.h"
#include "aimp/HookManager.h"

#include <array>
#include <atomic>
#include <cstring>

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
}

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "Level integrity bypass requires the certified executable";
        return false;
    }
    if (SiteEquals(base, kSites[0], kSites[0].openCage) && SiteEquals(base, kSites[1], kSites[1].openCage)) {
        gAlreadyBypassed.store(true, std::memory_order_release);
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
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
bool AlreadyBypassed() noexcept { return gAlreadyBypassed.load(std::memory_order_acquire); }
}
