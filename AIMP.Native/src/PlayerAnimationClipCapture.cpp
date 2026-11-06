#include "aimp/PlayerAnimationClipCapture.h"
#include "aimp/HookManager.h"
#include "aimp/Diagnostics.h"
#include "aimp/PlayerCharacters.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>

namespace aimp::player_animation_clip_capture {
namespace {
struct ClipResult { std::uintptr_t resource; std::int32_t metadata; };
using Resolve = ClipResult*(__thiscall*)(void*, ClipResult*, const void*, const void*);
std::atomic<void*> gSingle{}, gMultiple{};
void* gSingleStorage{}; void* gMultipleStorage{};
struct Row {
    std::uint32_t name{}, context{}, path{};
    std::int32_t metadata{};
    bool multiple{}, readable{};
    std::uint64_t count{}, lastTick{};
};
std::mutex gMutex;
// Tree rebuilds resolve dozens of facial/hand poses after locomotion leaves.
// Keep a separate, bounded window so these cannot evict crouched requests.
constexpr std::size_t kCrouchedRows = 128U;
constexpr std::size_t kOtherRows = 64U;
std::array<Row, kCrouchedRows + kOtherRows> gRows{};
std::uint64_t gReplaced{};
template<class T> bool Read(std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U) return false;
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Record(void* database, const void* name, const void* contexts,
    const ClipResult* output, bool multiple) noexcept {
    std::uint32_t identity{};
    if (!Read(reinterpret_cast<std::uintptr_t>(database) + 0x10U, identity) || !player_characters::IsRepresentation(identity)) return;
    Row entry{}; entry.multiple = multiple;
    if (!Read(reinterpret_cast<std::uintptr_t>(name) + 8U, entry.name)) return;
    const auto contextAddress = reinterpret_cast<std::uintptr_t>(contexts);
    if (multiple) {
        std::uint32_t references{};
        if (Read(contextAddress + 0x38U, references) && references != 0U)
            Read(contextAddress + 0x10U, entry.context);
    } else Read(contextAddress + 8U, entry.context);
    ClipResult result{};
    if (Read(reinterpret_cast<std::uintptr_t>(output), result)) {
        entry.metadata = result.metadata;
        entry.readable = Read(result.resource + 0x10U, entry.path);
    }
    entry.lastTick = GetTickCount64();
    std::lock_guard lock(gMutex);
    const bool crouched = entry.context == 4114047242U;
    const std::size_t begin = crouched ? 0U : kCrouchedRows;
    const std::size_t end = crouched ? kCrouchedRows : gRows.size();
    std::size_t selected = begin;
    for (std::size_t i = begin; i < end; ++i) {
        auto& row = gRows[i];
        if (row.count == 0U || (row.name == entry.name && row.context == entry.context &&
            row.path == entry.path && row.metadata == entry.metadata &&
            row.multiple == entry.multiple && row.readable == entry.readable)) { selected = i; break; }
        if (row.lastTick < gRows[selected].lastTick) selected = i;
    }
    auto& row = gRows[selected];
    if (row.count == 0U || row.name != entry.name || row.context != entry.context ||
        row.path != entry.path || row.metadata != entry.metadata || row.multiple != entry.multiple ||
        row.readable != entry.readable) {
        if (row.count != 0U) ++gReplaced;
        row = entry;
    }
    ++row.count; row.lastTick = entry.lastTick;
}
ClipResult* __fastcall Single(void* self, void*, ClipResult* output, const void* name, const void* context) {
    const auto original = reinterpret_cast<Resolve>(gSingle.load(std::memory_order_acquire));
    auto* result = original(self, output, name, context);
    Record(self, name, context, output, false);
    return result;
}
ClipResult* __fastcall Multiple(void* self, void*, ClipResult* output, const void* name, const void* contexts) {
    const auto original = reinterpret_cast<Resolve>(gMultiple.load(std::memory_order_acquire));
    auto* result = original(self, output, name, contexts);
    Record(self, name, contexts, output, true);
    return result;
}
}
bool Install(std::uintptr_t moduleBase, HookManager& hooks, std::string& error) {
    if (!hooks.ExactBuildValidated()) { error = "Animation probes require the certified retail image."; return false; }
    const auto checkpoint = hooks.InstalledCount();
    // Disassembled from the supplied x86 image: push ebp; mov ebp,esp;
    // sub esp,10h/24h. Six complete instructions bytes, no relocation operand.
    // Both functions return their output pointer in EAX and pop 3 stack args.
    for (const bool multiple : {false, true}) {
        HookRequest request{};
        request.name = multiple ? "remote_animation_clip_context_stack" : "remote_animation_clip_explicit_context";
        request.target = reinterpret_cast<void*>(moduleBase + (multiple ? 0x636160U : 0x635FF0U));
        request.detour = multiple ? reinterpret_cast<void*>(&Multiple) : reinterpret_cast<void*>(&Single);
        request.patchLength = 6U;
        request.expected = MaskedBytes{{0x55U, 0x8BU, 0xECU, 0x83U, 0xECU,
            static_cast<std::uint8_t>(multiple ? 0x24U : 0x10U)}, "xxxxxx"};
        request.relocationSafePrefix = true;
        request.trampolineOut = multiple ? &gMultipleStorage : &gSingleStorage;
        request.publishedTrampolineOut = multiple ? &gMultiple : &gSingle;
        if (!hooks.Install(request, error)) {
            std::string rollback;
            hooks.UninstallFrom(checkpoint, rollback);
            return false;
        }
    }
    return true;
}
void Clear() noexcept { std::lock_guard lock(gMutex); gRows = {}; gReplaced = 0U; }
void Export(Diagnostics& diagnostics) {
    decltype(gRows) rows{}; std::uint64_t replaced{};
    { std::lock_guard lock(gMutex); rows = gRows; replaced = gReplaced; }
    diagnostics.Log("info", "remote_animation_clip_summary", "Actual retail clip-resolver returns for the crew representation databases; no rendering claim.",
        {{"replacedRows", Diagnostics::Unsigned(replaced), true}});
    for (const auto& row : rows) {
        if (row.count == 0U) continue;
        diagnostics.Log("info", "remote_animation_clip_resolution", "Passive retail clip-resource resolution; aggregated across crew representation databases.",
            {{"name", Diagnostics::Unsigned(row.name), true},
             {"context", Diagnostics::Unsigned(row.context), true},
             {"path", Diagnostics::Unsigned(row.path), true},
             {"metadata", std::to_string(row.metadata), true},
             {"multipleContexts", Diagnostics::Boolean(row.multiple), true},
             {"resourceReadable", Diagnostics::Boolean(row.readable), true},
             {"count", Diagnostics::Unsigned(row.count), true},
             {"lastTick", Diagnostics::Unsigned(row.lastTick), true}});
    }
}
}
