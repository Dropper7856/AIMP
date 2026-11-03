#include "aimp/PairedTransformGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstddef>

namespace aimp::paired_transform_guard {
namespace {
// 006618C0 copies a paired partner's transform (+0x10..+0x2B) into the
// animation state (+0x44..+0x5F). It locks the partner through a shared
// pointer obtained from *(self+4)->vtable[0x1C] and FUN_00402080, and never
// tests the result. A Client crashed there (EAX=0, read 0x10) while an
// android's kill graph still referenced a remote player proxy whose pair had
// just been released. A partner that no longer exists keeps the previous
// transform for this update instead of faulting; live partners run retail.
using Update = void(__thiscall*)(void* self);
using GetShared = void*(__thiscall*)(void* holder, void* output);
using RawOf = void**(__thiscall*)(void* shared);

struct SharedRef final {
    void* object{};
    long* control{};
};

constexpr std::uintptr_t kUpdateRva = 0x002618C0U;
constexpr std::uintptr_t kRawOfRva = 0x00002080U;

std::uintptr_t gBase{};
void* gTrampoline{};
std::atomic<void*> gOriginal{};
std::atomic<bool> gInstalled{};
std::atomic<std::uint64_t> gMissingPartners{}, gReportedMissingPartners{};

// Same control-block release as the retail epilogue: [1] uses, [2] weak.
void Release(long* control) noexcept {
    if (control == nullptr || _InterlockedDecrement(control + 1) != 0) return;
    auto** table = *reinterpret_cast<void***>(control);
    reinterpret_cast<void(__thiscall*)(void*)>(table[0])(control);
    if (_InterlockedDecrement(control + 2) == 0)
        reinterpret_cast<void(__thiscall*)(void*)>(table[1])(control);
}

bool PartnerLive(void* self) noexcept {
    auto* holder = *reinterpret_cast<void**>(static_cast<std::byte*>(self) + 4U);
    if (holder == nullptr) return false;
    SharedRef local{};
    auto** table = *reinterpret_cast<void***>(holder);
    auto* shared = reinterpret_cast<GetShared>(table[0x1CU / sizeof(void*)])(holder, &local);
    auto** raw = reinterpret_cast<RawOf>(gBase + kRawOfRva)(shared);
    const bool live = raw != nullptr && *raw != nullptr;
    Release(local.control);
    return live;
}

void __fastcall UpdateDetour(void* self, void*) noexcept {
    const auto original = reinterpret_cast<Update>(gOriginal.load(std::memory_order_acquire));
    if (self != nullptr && PartnerLive(self)) {
        original(self);
        return;
    }
    gMissingPartners.fetch_add(1U, std::memory_order_relaxed);
}
} // namespace

bool Install(const std::uintptr_t base, HookManager& hooks, std::string& error) {
    if (Installed()) return true;
    if (base == 0U || !hooks.MutationAllowed()) {
        error = "Paired transform guard requires the certified executable";
        return false;
    }
    gBase = base;
    HookRequest request;
    request.name = "PairedTransformGuard.PartnerTransform";
    request.target = reinterpret_cast<void*>(base + kUpdateRva);
    request.detour = reinterpret_cast<void*>(&UpdateDetour);
    // SUB ESP,8; PUSH ESI; MOV ESI,ECX: position-independent.
    request.expected.bytes = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1};
    request.expected.mask = "xxxxxx";
    request.patchLength = request.expected.bytes.size();
    request.relocationSafePrefix = true;
    request.trampolineOut = &gTrampoline;
    request.publishedTrampolineOut = &gOriginal;
    if (!hooks.Install(request, error)) return false;
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

void LogDiagnostics(Diagnostics& diagnostics) {
    const auto missing = gMissingPartners.load(std::memory_order_relaxed);
    if (missing == gReportedMissingPartners.exchange(missing, std::memory_order_relaxed)) return;
    diagnostics.Log("warning", "paired_transform_guard",
        "A paired animation update referenced a partner that no longer exists; the previous transform was kept.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"missingPartners", Diagnostics::Unsigned(missing), true}});
}
} // namespace aimp::paired_transform_guard
