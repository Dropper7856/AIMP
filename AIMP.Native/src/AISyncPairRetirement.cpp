#include "aimp/AISyncPairRetirement.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"

#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>

namespace aimp::ai_sync::pair_retirement {
namespace {
using namespace interactions;
constexpr std::size_t kCapacity = NativePairAdapter::kCapacity;
constexpr std::uintptr_t kSourceType = 0xEAE5D0U, kVictimType = 0xEAE690U;
constexpr std::uintptr_t kActionType = 0xEB2D50U;
struct Side {
    std::uintptr_t cycle{}, action{}, graph{};
    std::uint8_t serial{};
    bool anchored{}, revoked{}, cleaning{}, cleaned{}, disposing{}, disposed{}, unknown{};
};
struct Entry {
    Key key{};
    Binding binding{};
    std::uint64_t serial{}, generation{};
    std::array<Side, 2> sides{};
    bool used{}, releasedRead{};
};
struct Missed {
    std::uintptr_t object{};
    std::uint8_t serial{};
    bool action{}, used{};
};
struct Token {
    std::size_t slot{}, side{};
    std::uint64_t generation{};
    bool valid{}, disposal{};
};
std::mutex gMutex;
std::array<Entry, kCapacity> gEntries{};
// A bounded negative ledger. Overflow fences future tracking rather than
// forgetting an unbound cleanup and later treating its address as new evidence.
std::array<Missed, 256> gMissed{};
std::array<LocalParticipant, kCapacity * 2> gRevoked{};
std::size_t gMissedCount{}, gRevokedCount{};
std::uint64_t gGeneration{};
bool gCoverageLost{}, gInstalled{}, gInstallAttempted{};
std::uintptr_t gBase{};
Qualifier gQualifier{};
transactions::Scope gScope{};
bool gScoped{};
std::uint32_t gNativeWorld{};
std::atomic<void*> gSourceOriginal{}, gVictimOriginal{}, gActionOriginal{};
void* gSourceTrampoline{};
void* gVictimTrampoline{};
void* gActionTrampoline{};

template<class T> bool Read(std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && address <= 0xFFFFFFFFULL - sizeof(T) &&
        crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(T));
}
bool Pointer(std::uintptr_t at, std::uintptr_t& value) noexcept {
    std::uint32_t word{};
    if (!Read(at, word)) return false;
    value = word; return true;
}
const LocalParticipant& Actor(const Entry& entry, std::size_t side) noexcept {
    return side == 0 ? entry.binding.source : entry.binding.victim;
}
bool Complete(const Entry& entry) noexcept {
    for (const auto& side : entry.sides)
        if (side.unknown || !side.cleaned || !side.disposed) return false;
    return true;
}
Entry* Find(const Key& key, std::uint64_t serial) noexcept {
    for (auto& entry : gEntries) if (entry.used && entry.key == key && entry.serial == serial) return &entry;
    return nullptr;
}
bool Revoked(const LocalParticipant& actor) noexcept {
    for (std::size_t i = 0; i < gRevokedCount; ++i) if (gRevoked[i] == actor) return true;
    return false;
}
void Remember(std::uintptr_t object, std::uint8_t serial, bool action) noexcept {
    for (std::size_t i = 0; i < gMissedCount; ++i)
        if (gMissed[i].object == object && gMissed[i].serial == serial && gMissed[i].action == action) return;
    if (gMissedCount == gMissed.size()) { gCoverageLost = true; return; }
    gMissed[gMissedCount++] = {object, serial, action, true};
}
bool MissedBefore(std::uintptr_t object, std::uint8_t serial, bool action) noexcept {
    for (std::size_t i = 0; i < gMissedCount; ++i)
        if (gMissed[i].used && gMissed[i].object == object && gMissed[i].action == action &&
            (action || gMissed[i].serial == serial)) return true;
    return false;
}
// Called only while Track's caller holds native lifetime, or at the actual
// native cleanup entry where this cycle and its action are still owned.
bool CaptureAction(Side& side, std::uintptr_t suppliedGraph) noexcept {
    std::uintptr_t action{}, graph{}, type{};
    if (!Pointer(side.cycle + 0x14U, action)) return false;
    if (action == 0) return (side.action == 0 && suppliedGraph == 0) ||
        (side.disposed && side.graph != 0 && suppliedGraph == side.graph);
    if ((side.action != 0 && side.action != action) || MissedBefore(action, 0, true) ||
        !Pointer(action, type) || type != gBase + kActionType || !Pointer(action + 0x50U, graph) ||
        (suppliedGraph != 0 && graph != suppliedGraph) || (side.graph != 0 && graph != side.graph)) return false;
    side.action = action;
    if (graph != 0) side.graph = graph;
    return true;
}
bool Anchor(Entry& entry, std::size_t index, const PairState& state) noexcept {
    auto& side = entry.sides[index];
    if (side.cleaned || side.revoked) return !side.unknown;
    // InspectNativePair can report the victim's unrelated current cycle before
    // the native 4F link is acquired. It is not this reservation's anchor.
    if (index == 1 && !state.linked) return !side.unknown;
    const auto cycle = index == 0 ? state.sourceCycle : state.victimCycle;
    const auto serial = index == 0 ? state.sourceSerial : state.victimSerial;
    const auto graph = index == 0 ? state.sourceGraph : state.victimGraph;
    if (cycle == 0) return !side.anchored && graph == 0;
    if (side.cleaning || side.disposing) return true;
    std::uintptr_t owner{}, type{};
    std::uint8_t currentSerial{}, kind{};
    if ((side.anchored && (side.cycle != cycle || side.serial != serial)) ||
        MissedBefore(cycle, serial, false) || !Pointer(cycle, type) ||
        type != gBase + (index == 0 ? kSourceType : kVictimType) ||
        !Pointer(cycle + 4U, owner) || owner != Actor(entry, index).character ||
        !Read(cycle + 0x19U, currentSerial) || currentSerial != serial ||
        !Read(cycle + 0x1AU, kind) || kind != (index == 0 ? 0x4EU : 0x4FU)) return false;
    side.cycle = cycle; side.serial = serial; side.anchored = true;
    if (!CaptureAction(side, graph)) return false;
    // Recheck persistent-cycle reuse after the dependent reads.
    return Read(cycle + 0x19U, currentSerial) && currentSerial == serial &&
        Pointer(cycle + 4U, owner) && owner == Actor(entry, index).character;
}
Token BeginCleanup(std::uintptr_t cycle, std::size_t index) noexcept {
    const std::lock_guard lock(gMutex);
    Token token{};
    std::uint8_t serial{}, kind{};
    std::uintptr_t owner{}, type{};
    const bool readable = Pointer(cycle, type) && Pointer(cycle + 4U, owner) &&
        Read(cycle + 0x19U, serial) && Read(cycle + 0x1AU, kind);
    for (std::size_t i = 0; i < gEntries.size(); ++i) {
        auto& entry = gEntries[i]; auto& side = entry.sides[index];
        if (!entry.used || !side.anchored || side.cycle != cycle || side.cleaned) continue;
        if (!readable || type != gBase + (index == 0 ? kSourceType : kVictimType) ||
            kind != (index == 0 ? 0x4EU : 0x4FU) || owner != Actor(entry, index).character ||
            serial != side.serial || side.cleaning || side.unknown || token.valid) {
            side.unknown = true;
            if (token.valid) { gEntries[token.slot].sides[index].unknown = true; token.valid = false; }
            continue;
        }
        // A late action may be learned before invalidation. After invalidation
        // only an already captured action/graph is accepted.
        if ((side.revoked && (side.action == 0 || side.graph == 0)) ||
            !CaptureAction(side, side.graph) || side.action == 0 || side.graph == 0) {
            side.unknown = true; continue;
        }
        side.cleaning = true;
        token = {i, index, entry.generation, true, false};
    }
    if (!token.valid && readable) Remember(cycle, serial, false);
    return token;
}
Token BeginDisposal(std::uintptr_t action) noexcept {
    const std::lock_guard lock(gMutex);
    Token token{};
    std::uintptr_t graph{};
    const bool readable = Pointer(action + 0x50U, graph);
    for (std::size_t i = 0; i < gEntries.size(); ++i) for (std::size_t index = 0; index < 2; ++index) {
        auto& entry = gEntries[i]; auto& side = entry.sides[index];
        if (!entry.used || side.action != action || side.disposed) continue;
        if (!readable || graph == 0 || side.graph != graph || side.disposing || side.unknown || token.valid) {
            side.unknown = true;
            if (token.valid) { gEntries[token.slot].sides[token.side].unknown = true; token.valid = false; }
            continue;
        }
        side.disposing = true;
        token = {i, index, entry.generation, true, true};
    }
    // Unrelated engine action destruction is not lost coverage for this
    // adapter. A tracked pair can become Ready only from its own entry token.
    // In particular, tracking halfway through an unobserved destructor cannot
    // synthesize the missing disposal-return token.
    return token;
}
void Finish(const Token& token) noexcept {
    if (!token.valid) return;
    const std::lock_guard lock(gMutex);
    auto& entry = gEntries[token.slot];
    if (!entry.used || entry.generation != token.generation) return;
    auto& side = entry.sides[token.side];
    if (token.disposal) { side.disposing = false; side.disposed = true; }
    else { side.cleaning = false; side.cleaned = true; }
    // No native reads and no external callbacks after the native return.
}
#if defined(_MSC_VER) && defined(_M_IX86)
using Cleanup = void(__thiscall*)(void*);
void Invoke(std::atomic<void*>& original, void* self) {
    const auto function = reinterpret_cast<Cleanup>(original.load(std::memory_order_acquire));
    if (!function) std::terminate(); // HookManager publishes before resuming threads.
    function(self);
}
void __fastcall SourceDetour(void* self, void*) {
    const auto token = BeginCleanup(reinterpret_cast<std::uintptr_t>(self), 0);
    Invoke(gSourceOriginal, self); Finish(token);
}
void __fastcall VictimDetour(void* self, void*) {
    const auto token = BeginCleanup(reinterpret_cast<std::uintptr_t>(self), 1);
    Invoke(gVictimOriginal, self); Finish(token);
}
void __fastcall ActionDetour(void* self, void*) {
    const auto token = BeginDisposal(reinterpret_cast<std::uintptr_t>(self));
    Invoke(gActionOriginal, self); Finish(token);
}
#endif
} // namespace

bool Install(std::uintptr_t base, HookManager& hooks, Qualifier qualifier, std::string& error) {
    if (gInstallAttempted || !qualifier || base < 0x10000U || base > 0xFFFFFFFFULL - 0xD27680U ||
        !hooks.ExactBuildValidated()) { error = "Pair retirement requires one exact-build installation and a qualifier"; return false; }
#if defined(_MSC_VER) && defined(_M_IX86)
    gInstallAttempted = true; gBase = base; gQualifier = qualifier;
    const auto before = hooks.InstalledCount();
    const auto install = [&](const char* name, std::uintptr_t rva, std::uintptr_t handlerRva,
        void* detour, void** trampoline, std::atomic<void*>& original) {
        HookRequest request{}; request.name = name; request.target = reinterpret_cast<void*>(base + rva);
        request.detour = detour; request.patchLength = 7; request.relocationSafePrefix = true;
        request.expected.bytes = {0x6A, 0xFF, 0x68, 0, 0, 0, 0}; request.expected.mask = "xxxxxxx";
        const auto handler = static_cast<std::uint32_t>(base + handlerRva);
        std::memcpy(request.expected.bytes.data() + 3, &handler, sizeof(handler));
        request.trampolineOut = trampoline; request.publishedTrampolineOut = &original;
        return hooks.Install(request, error);
    };
    if (!install("AISync pair source cleanup", 0x25B190U, 0xD26561U,
            reinterpret_cast<void*>(&SourceDetour), &gSourceTrampoline, gSourceOriginal) ||
        !install("AISync pair victim cleanup", 0x25B630U, 0xD265D1U,
            reinterpret_cast<void*>(&VictimDetour), &gVictimTrampoline, gVictimOriginal) ||
        !install("AISync pair action disposal", 0x2A5850U, 0xD27680U,
            reinterpret_cast<void*>(&ActionDetour), &gActionTrampoline, gActionOriginal)) {
        std::string rollback;
        if (!hooks.UninstallFrom(before, rollback)) error += "; pair retirement rollback: " + rollback;
        return false;
    }
    gInstalled = true; return true;
#else
    static_cast<void>(hooks); error = "Pair retirement hooks require MSVC x86"; return false;
#endif
}
Check Track(const Key& key, std::uint64_t serial, const Binding& binding, const PairState& state) noexcept {
    // Qualifier is deliberately outside the lock and never invoked by hooks.
    if (!gInstalled || !serial || !binding.source.creationSerial || !binding.victim.creationSerial ||
        !gQualifier || !gQualifier(key, binding)) return Check::Unknown;
    const std::lock_guard lock(gMutex);
    if (gNativeWorld != 0U && binding.nativeGeneration != gNativeWorld)
        return Check::Unknown;
    if (!gScoped || key.scope != gScope) {
        // Scope retirement may discard only obligations whose exact two native
        // return pairs were delivered. Generation never resets, so an old
        // return token cannot publish into a new scope's reused entry slot.
        for (const auto& previous : gEntries)
            if (previous.used && (!previous.releasedRead || !Complete(previous))) return Check::Unknown;
        // Preserve a missed cleanup of the very anchors being introduced,
        // including one that ran synchronously inside admission before Track.
        const bool missedSource = state.sourceCycle != 0 && MissedBefore(state.sourceCycle, state.sourceSerial, false);
        const bool missedVictim = state.linked && state.victimCycle != 0 &&
            MissedBefore(state.victimCycle, state.victimSerial, false);
        const bool revokedSource = Revoked(binding.source), revokedVictim = Revoked(binding.victim);
        gEntries = {}; gRevoked = {}; gRevokedCount = 0;
        gMissed = {}; gMissedCount = 0; gCoverageLost = false;
        if (missedSource) Remember(state.sourceCycle, state.sourceSerial, false);
        if (missedVictim) Remember(state.victimCycle, state.victimSerial, false);
        if (revokedSource) gRevoked[gRevokedCount++] = binding.source;
        if (revokedVictim) gRevoked[gRevokedCount++] = binding.victim;
        gScope = key.scope; gScoped = true;
    }
    auto* entry = Find(key, serial);
    if (!entry) {
        if (gCoverageLost || Revoked(binding.source) || Revoked(binding.victim) ||
            gGeneration == std::numeric_limits<std::uint64_t>::max()) return Check::Unknown;
        // Keep completed receipts until CheckReleased has delivered their proof
        // to the engine-thread owner. An unrelated Track must not evict a
        // worker-completed receipt before its reservation can be released.
        for (auto& candidate : gEntries)
            if (!candidate.used || (candidate.releasedRead && Complete(candidate))) { entry = &candidate; break; }
        if (!entry) return Check::Unknown;
        *entry = {}; entry->used = true; entry->key = key; entry->serial = serial;
        entry->binding = binding; entry->generation = ++gGeneration;
    }
    if (entry->binding != binding) { for (auto& side : entry->sides) side.unknown = true; return Check::Unknown; }
    if ((state.source != 0 && state.source != binding.source.character) ||
        (state.victim != 0 && state.victim != binding.victim.character) ||
        (state.sourceRuntimeId != 0 && state.sourceRuntimeId != binding.source.runtimeId) ||
        (state.victimRuntimeId != 0 && state.victimRuntimeId != binding.victim.runtimeId)) {
        for (auto& side : entry->sides) side.unknown = true; return Check::Unknown;
    }
    for (std::size_t index = 0; index < 2; ++index)
        if (!Anchor(*entry, index, state)) entry->sides[index].unknown = true;
    for (auto& other : gEntries) if (other.used && &other != entry && !Complete(other))
        for (auto& side : entry->sides) for (auto& previous : other.sides)
            if (side.anchored && previous.anchored && !previous.cleaned &&
                (side.cycle == previous.cycle || (side.action != 0 && side.action == previous.action))) {
                side.unknown = true; previous.unknown = true;
            }
    for (const auto& side : entry->sides) if (side.unknown) return Check::Unknown;
    return Complete(*entry) ? Check::Ready : Check::Waiting;
}
bool ResetForWorld(const std::uint32_t current) noexcept {
    const std::lock_guard lock(gMutex);
    if (current == 0U || current < gNativeWorld) return false;
    if (current == gNativeWorld) return true;
    for (const auto& entry : gEntries)
        if (entry.used && (entry.binding.nativeGeneration == 0U || entry.binding.nativeGeneration >= current))
            return false;
    // Token.generation never resets: a destructor returning after this point
    // cannot complete an entry created in the new world at the same address.
    gEntries = {}; gMissed = {}; gRevoked = {};
    gMissedCount = gRevokedCount = 0U; gCoverageLost = false;
    gScope = {}; gScoped = false; gNativeWorld = current;
    return true;
}
Check CheckCycleRetired(const Key& key, std::uint64_t serial, bool victim) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* entry = Find(key, serial);
    if (!gInstalled || !entry) return Check::Unknown;
    const auto& side = entry->sides[victim ? 1U : 0U];
    if (side.unknown) return Check::Unknown;
    return side.cleaned ? Check::Ready : Check::Waiting;
}
Check ReleaseWithDeadVictim(const Key& key, std::uint64_t serial) noexcept {
    const std::lock_guard lock(gMutex);
    auto* entry = Find(key, serial);
    if (!gInstalled || !entry) return Check::Unknown;
    const auto& source = entry->sides[0];
    if (source.unknown || entry->sides[1].unknown) return Check::Unknown;
    if (!source.cleaned || !source.disposed) return Check::Waiting;
    // The corpse keeps its own native cycle. Release the source reservation,
    // but retain the victim's exact cleanup/action return tokens until its
    // later Character destruction. Erasing this entry made that legitimate
    // teardown look like a missed cleanup, poisoning a subsequent world's
    // reused cycle addresses in the negative ledger.
    entry->releasedRead = true;
    return Check::Ready;
}
Check CheckReleased(const Key& key, std::uint64_t serial) noexcept {
    const std::lock_guard lock(gMutex);
    auto* entry = Find(key, serial);
    if (!gInstalled || !entry) return Check::Unknown;
    for (const auto& side : entry->sides) if (side.unknown) return Check::Unknown;
    if (!Complete(*entry)) return Check::Waiting;
    entry->releasedRead = true;
    return Check::Ready;
}
void Invalidate(const LocalParticipant& actor) noexcept {
    const std::lock_guard lock(gMutex);
    if (!Revoked(actor)) {
        if (gRevokedCount == gRevoked.size()) gCoverageLost = true;
        else gRevoked[gRevokedCount++] = actor;
    }
    for (auto& entry : gEntries) if (entry.used)
        for (std::size_t index = 0; index < 2; ++index)
            if (Actor(entry, index) == actor) entry.sides[index].revoked = true;
}
} // namespace aimp::ai_sync::pair_retirement
