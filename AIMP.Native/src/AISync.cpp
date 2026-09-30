#include "aimp/AISync.h"
#include "aimp/BuildProfile.h"
#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <type_traits>

namespace aimp::ai_sync {
namespace {
static_assert(sizeof(void*) == 4U);
static_assert(std::is_trivially_copyable_v<State>);
constexpr std::size_t kActors = 16U, kRing = 128U;
constexpr ULONGLONG kSendPeriod = 100ULL, kRefreshPeriod = 500ULL, kTimeout = 1500ULL;
constexpr std::uint32_t kExecutionFlags = 0x00070101U;
using ManagerUpdate = void(__thiscall*)(void*, std::uint32_t, float);
using Coordinator = void(__thiscall*)(void*, std::uint32_t);
using Movement = std::uint32_t(__thiscall*)(void*, void*, void*, const float*,
    std::uint32_t, float, float, std::uint32_t);
using Factory = void*(__thiscall*)(void*, std::uint32_t, void*);
using Cleanup = void(__stdcall*)(void*, void*); // binary 0070B134: RET 8
using Destroy = void*(__thiscall*)(void*, std::uint32_t);

// This is the complete 12-byte canonical definition consumed by the adapter:
// FUN_006ED2B0 allocates 0xC; FUN_006C9090 parses MovementSpeedType 0..3.
// No decision/tick callback on this definition is called on a Client.
struct MovementDefinition { std::uintptr_t vtable{}; std::uint32_t priority{}, speed{}; };
struct Actor {
    std::uintptr_t character{}, tree{}, hostExecutionState{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, characterClass{}, birth{};
    State desired{}, previous{};
    MovementDefinition definition{};
    void* executionState{};
    ULONGLONG lastSend{}, lastReceive{}, lastActionProbe{};
    std::uint32_t completedRevision{}, remoteBirth{}, remoteRevision{};
    bool blocked{}, duplicate{}, faulted{};
};
std::array<Actor, kActors> gActors{}; // engine thread only
std::array<std::atomic<std::uintptr_t>, kActors> gOwned{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedGeneration{};
std::array<State, kRing> gOutgoing{}; // single engine-thread producer, IPC consumer
std::atomic<std::uint32_t> gWrite{}, gRead{};
std::mutex gMailboxMutex;
Configuration gRequested{};
bool gConfigurationPending{};
std::array<State, kActors> gIncoming{};
std::array<bool, kActors> gIncomingUsed{};
std::array<ULONGLONG, kActors> gIncomingAt{};
std::array<ULONGLONG, kActors> gNextIdentityProbe{};
Configuration gCurrent{}; // engine thread only
std::atomic<Role> gRole{Role::Retail};
std::atomic<bool> gInstalled{}, gStopping{};
std::atomic<void*> gUpdateOriginal{}, gMovementOriginal{}, gCoordinatorOriginal{};
std::atomic<void*> gPrepassOriginal{};
void* gPrepassTrampoline{};
void* gUpdateTrampoline{}, *gMovementTrampoline{}, *gCoordinatorTrampoline{};
void* gBranchTrampoline{}, *gBranchExit{};
std::atomic<void*> gBranchPublished{};
std::atomic<std::uint32_t> gShutdownEngineThread{};
std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
GameThreadDispatcher* gDispatcher{};
std::uint64_t gGeneration{};
std::uint32_t gNextBirth{1U};
thread_local Actor* gUpdating{};
std::atomic<std::uint64_t> gObserved{}, gApplied{}, gDropped{}, gRefused{}, gSuspended{}, gFaults{};
std::atomic<std::uint64_t> gCaptured{}, gStarted{}, gCompleted{}, gMissing{}, gOrphaned{};
std::atomic<std::uint64_t> gManagerUpdates{}, gAdmitted{}, gIdentityRejected{}, gTreeUnavailable{}, gOwnerMismatch{};
std::atomic<std::uint64_t> gMovementCalls{}, gUnsupportedMovement{}, gRemoteQueued{}, gOutsideFrameMovement{};
std::atomic<std::uint64_t> gMovementPublished{}, gInactiveCaptured{};
std::atomic<std::uint64_t> gPolicyCaptured{}, gPolicyApplied{}, gPolicyRejected{};
std::atomic<std::uint32_t> gActiveNativeGeneration{}, gActiveWorldGeneration{}, gLastMovementDefinitionRva{};
std::atomic<std::uint32_t> gLastEntityA{}, gLastEntityB{}, gLastCharacterClass{};

// Called only on a certified engine callback, with owner lifetime supplied by
// that callback. SEH avoids VirtualQuery on the per-frame path; no heap scan.
template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U || address > 0xFFFF0000U) return false;
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool EngineThread() noexcept {
    if (gStopping.load(std::memory_order_acquire))
        return gShutdownEngineThread.load(std::memory_order_acquire) == GetCurrentThreadId();
    return gDispatcher != nullptr && gDispatcher->IsCertifiedEngineThread(GetCurrentThreadId());
}
bool Client() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return role == Role::Client || role == Role::FrozenClient;
}
bool Stable() noexcept {
    return gGenerations != nullptr && gGenerations->IsStableGeneration(gCurrent.nativeGeneration);
}
bool Valid(const State& s) noexcept {
    if (s.version != 2U || s.nativeGeneration == 0U || s.sessionCookie == 0U ||
        s.worldGeneration == 0U || (s.entityA == 0U && s.entityB == 0U) ||
        s.birth == 0U || s.revision == 0U || s.kind > 1U || s.movementSpeedType > 3U ||
        s.locomotionStyle > 8U || (s.flags & ~kExecutionFlags) != 0U) return false;
    for (const auto coordinate : s.destination)
        if (!std::isfinite(coordinate) || std::abs(coordinate) > 100000.0F) return false;
    return std::isfinite(s.stoppingDistance) && s.stoppingDistance >= 0.0F &&
        s.stoppingDistance <= 100.0F && std::isfinite(s.tolerance) &&
        s.tolerance >= 0.0F && s.tolerance <= 100.0F;
}
bool Eligible(const std::uintptr_t c, std::uint32_t& type) noexcept {
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwned[i].load(std::memory_order_acquire) == c && gGenerations != nullptr &&
            gOwnedGeneration[i].load(std::memory_order_relaxed) == gGenerations->CurrentGeneration()) return false;
    std::uintptr_t characterTemplate{};
    if (!Read(c + 0x64U, characterTemplate) ||
        !Read(characterTemplate + 4U, type)) return false;
    // +1F is an execution gate, not proof that the NPC owns an initialized
    // manager/tree. The retail ctor creates a tree independently of that gate;
    // Admit validates its actual Character owner instead.
    // 2/7 are player families in FUN_00733850, never proxy them here.
    return type == 1U || type == 3U || type == 4U || type == 6U || type == 9U;
}
bool SupportedMovement(const std::uintptr_t table) noexcept {
    if (table < gBase) return false;
    const auto rva = table - gBase;
    return rva == 0x00EB84D0U || rva == 0x00EB8910U || rva == 0x00EBC048U ||
        rva == 0x00EB5D80U || rva == 0x00EBC8B8U;
}
bool MovementRequest(const std::uintptr_t state, const std::uintptr_t character,
    std::uintptr_t& request) noexcept {
    // FUN_0040C850 copies the shared-ref at state+2C/+30. On the certified
    // engine callback its direct object pointer is kept alive by the state.
    // FUN_0070A940 admits only request type 1 (+10); FUN_00683620 sets +14
    // to Character+2CD0, the independent Character execution context.
    std::uint32_t type{};
    std::uintptr_t owner{};
    std::uint8_t active{};
    return Read(state + 0x2CU, request) && request != 0U &&
        Read(request + 0x10U, type) && type == 1U &&
        Read(request + 0x14U, owner) && owner == character + 0x2CD0U &&
        Read(request + 0x18U, active) && active != 0U;
}
bool CapturePolicy(const std::uintptr_t state, const std::uintptr_t character, State& value) noexcept {
    std::uintptr_t request{};
    std::uint8_t flagsB8{}, flagsB9{};
    if (!MovementRequest(state, character, request) || !Read(request + 0xB8U, flagsB8) ||
        !Read(request + 0xB9U, flagsB9)) return false;
    // Copy the actual outputs of retail virtuals +404/+408 after the Host
    // helper returns. NEVER invoke MOVE_TO_TARGET's senses callback on a Client.
    value.flags |= (static_cast<std::uint32_t>(flagsB8 & 0x18U) << 13U) |
        (static_cast<std::uint32_t>(flagsB9 & 0x10U) << 14U);
    gPolicyCaptured.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool ApplyPolicy(Actor& actor) noexcept {
    std::uintptr_t request{};
    if (!MovementRequest(reinterpret_cast<std::uintptr_t>(actor.executionState), actor.character, request))
        return false;
    __try {
        // The canonical helper has submitted a native request. Character's
        // execution pass runs later in the same frame. Change ONLY the three
        // certified execution bits; keep its activity and all other flags.
        auto& flagsB8 = *reinterpret_cast<std::uint8_t*>(request + 0xB8U);
        auto& flagsB9 = *reinterpret_cast<std::uint8_t*>(request + 0xB9U);
        flagsB8 = static_cast<std::uint8_t>((flagsB8 & ~0x18U) | ((actor.desired.flags >> 13U) & 0x18U));
        flagsB9 = static_cast<std::uint8_t>((flagsB9 & ~0x10U) | ((actor.desired.flags >> 14U) & 0x10U));
        gPolicyApplied.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
bool SuspendExistingTree(Actor& actor) noexcept {
    __try {
        // Retail cleanup, not memset of AI state. Order: selection -> pending
        // -> active; the promotion helpers then swap EMPTY vector roles.
        for (const auto offset : {0x44U, 0x48U, 0x4CU}) {
            const auto header = *reinterpret_cast<const std::uintptr_t*>(actor.tree + offset);
            const auto begin = *reinterpret_cast<const std::uintptr_t*>(header);
            const auto end = *reinterpret_cast<const std::uintptr_t*>(header + 4U);
            if (end < begin || (end - begin) % 4U != 0U || (end - begin) / 4U > 64U) return false;
        }
        using Clear = void(__thiscall*)(void*);
        reinterpret_cast<Clear>(gBase + 0x002FE080U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE5C0U)(reinterpret_cast<void*>(actor.tree));
        reinterpret_cast<Clear>(gBase + 0x002FE690U)(reinterpret_cast<void*>(actor.tree));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
Actor* Find(const std::uintptr_t c) noexcept {
    for (auto& actor : gActors) if (actor.character == c) return &actor;
    return nullptr;
}
bool Release(Actor& actor) noexcept;
Actor* Admit(const std::uintptr_t c) noexcept {
    if (auto* actor = Find(c); actor != nullptr) {
        std::uint32_t id{};
        std::uintptr_t tree{};
        if (!Read(c + 0x50U, id) || !Read(c + 0x34D8U, tree)) return nullptr;
        if (id == actor->runtimeId) {
            if (tree != actor->tree) {
                static_cast<void>(Release(*actor));
                actor->tree = tree; actor->completedRevision = 0U;
                actor->hostExecutionState = 0U;
                actor->desired.kind = 0U;
                std::uintptr_t owner{};
                actor->faulted = tree == 0U || !Read(tree + 0xCU, owner) || owner != c;
                if (!actor->faulted && Client()) actor->faulted = !SuspendExistingTree(*actor);
                if (actor->faulted) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
            }
            return actor;
        }
        // Address reuse without a destruction notification: do not touch a
        // request belonging to the old Character. Keep this visible in diagnostics.
        if (actor->executionState != nullptr) gOrphaned.fetch_add(1U, std::memory_order_relaxed);
        *actor = {};
    }
    std::uint32_t type{}, id{}, a{}, b{};
    std::uintptr_t tree{};
    if (!Eligible(c, type)) return nullptr;
    if (!Read(c + 0x50U, id) || id == 0U ||
        !Read(c + 0x54U, a) || !Read(c + 0x58U, b) || (a == 0U && b == 0U)) {
        gIdentityRejected.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    if (!Read(c + 0x34D8U, tree) || tree == 0U) {
        gTreeUnavailable.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    std::uintptr_t owner{};
    if (!Read(tree + 0xCU, owner) || owner != c) {
        gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed); return nullptr;
    }
    for (auto& actor : gActors) {
        if (actor.character != 0U) continue;
        actor = {};
        actor.character = c; actor.tree = tree; actor.runtimeId = id;
        actor.entityA = a; actor.entityB = b; actor.characterClass = type;
        actor.birth = gNextBirth++;
        gAdmitted.fetch_add(1U, std::memory_order_relaxed);
        gLastEntityA.store(a, std::memory_order_relaxed);
        gLastEntityB.store(b, std::memory_order_relaxed);
        gLastCharacterClass.store(type, std::memory_order_relaxed);
        actor.desired.nativeGeneration = gCurrent.nativeGeneration;
        actor.desired.sessionCookie = gCurrent.sessionCookie;
        actor.desired.worldGeneration = gCurrent.worldGeneration;
        actor.desired.entityA = a; actor.desired.entityB = b;
        actor.desired.characterClass = type; actor.desired.birth = actor.birth;
        actor.desired.revision = 1U;
        actor.definition.vtable = gBase + 0x00EB84D0U;
        if (Client()) actor.faulted = !SuspendExistingTree(actor);
        for (auto& other : gActors) {
            if (&other != &actor && other.character != 0U &&
                other.entityA == a && other.entityB == b) {
                actor.duplicate = true; other.duplicate = true;
                gRefused.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        return &actor;
    }
    gRefused.fetch_add(1U, std::memory_order_relaxed);
    return nullptr;
}
bool CharacterReady(const Actor& actor) noexcept {
    std::uint8_t alive{};
    if (!Read(actor.character + 0x2FAU, alive) || alive == 0U) return false;
    __try {
        // Exactly the admission guard in FUN_007342A0; prevents proxy requests
        // during death/disabled-character/script exclusivity states.
        return reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + 0x0038B380U)(
            reinterpret_cast<void*>(actor.character + 0x280U)) == 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Release(Actor& actor) noexcept {
    if (actor.executionState == nullptr) return true;
    void* state = actor.executionState;
    actor.executionState = nullptr; // never retry a partially completed native cleanup
    __try {
        reinterpret_cast<Cleanup>(gBase + 0x0030AF80U)(reinterpret_cast<void*>(actor.character), state);
        const auto table = *reinterpret_cast<std::uintptr_t*>(state);
        reinterpret_cast<Destroy>(*reinterpret_cast<std::uintptr_t*>(table))(state, 1U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        actor.faulted = true;
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
bool RunMovement(Actor& actor) noexcept {
    const auto& s = actor.desired;
    __try {
        // Retail applies speed and style when it creates a request, not in
        // its destination-update path. Recreate only when these inputs change.
        if (actor.executionState != nullptr &&
            (actor.definition.speed != s.movementSpeedType ||
             *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(actor.executionState) + 0x3CU)
                 != s.locomotionStyle)) {
            if (!Release(actor)) return false;
        }
        if (actor.executionState == nullptr) {
            actor.definition.speed = s.movementSpeedType;
            actor.executionState = reinterpret_cast<Factory>(gBase + 0x002F15C0U)(
                &actor.definition, 1U, reinterpret_cast<void*>(actor.tree));
            if (actor.executionState == nullptr) return false;
            gStarted.fetch_add(1U, std::memory_order_relaxed);
            *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(actor.executionState) + 0x3CU) =
                s.locomotionStyle;
        }
        // Execute the native movement request, NEVER the leaf's target/sense tick.
        const auto result = reinterpret_cast<Movement>(gMovementOriginal.load(std::memory_order_acquire))(
            &actor.definition, reinterpret_cast<void*>(actor.character), actor.executionState,
            s.destination, s.flags & 1U, s.stoppingDistance, s.tolerance, (s.flags >> 8U) & 1U);
        if (result == 0U && !ApplyPolicy(actor)) {
            actor.faulted = true;
            gPolicyRejected.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (result != 0U) {
            gCompleted.fetch_add(1U, std::memory_order_relaxed);
            actor.completedRevision = s.revision;
            static_cast<void>(Release(actor));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        actor.faulted = true;
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}
bool SameAction(const State& a, const State& b) noexcept {
    if (a.kind != b.kind) return false;
    if (a.kind == 0U) return true;
    if (a.movementSpeedType != b.movementSpeedType || a.locomotionStyle != b.locomotionStyle ||
        a.flags != b.flags || a.stoppingDistance != b.stoppingDistance || a.tolerance != b.tolerance)
        return false;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (std::abs(a.destination[i] - b.destination[i]) > 0.05F) return false;
    return true;
}
bool HostMovementActive(const Actor& actor) noexcept {
    if (actor.hostExecutionState == 0U) return false;
    // Establish retail ownership BEFORE reading the captured state. An action
    // can finish/free its state after the movement helper returns. Never retain
    // a raw state reference as a lifetime guarantee. Scan only at publication
    // time (at most 10 Hz), with the same bounded active-vector contract as BT.
    std::uintptr_t header{}, begin{}, end{};
    if (!Read(actor.tree + 0x44U, header) || !Read(header, begin) || !Read(header + 4U, end) ||
        end < begin || (end - begin) % 4U != 0U || (end - begin) / 4U > 64U) return false;
    for (auto entry = begin; entry < end; entry += 4U) {
        std::uintptr_t state{};
        if (!Read(entry, state)) return false;
        if (state != actor.hostExecutionState) continue;
        std::uint32_t status{};
        std::uintptr_t ownerTree{}, node{}, table{}, request{};
        return Read(state + 4U, status) && (status == 2U || status == 6U) &&
            Read(state + 0x14U, ownerTree) && ownerTree == actor.tree &&
            Read(state + 0x10U, node) && Read(node, table) &&
            SupportedMovement(table) && MovementRequest(state, actor.character, request);
    }
    return false;
}
void Publish(Actor& actor) noexcept {
    const auto now = GetTickCount64();
    if (now - actor.lastSend < kSendPeriod) return;
    if (actor.desired.kind == 1U && now - actor.lastActionProbe >= kSendPeriod) {
        actor.lastActionProbe = now;
        if (!HostMovementActive(actor)) {
            actor.desired.kind = 0U;
            gInactiveCaptured.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    const bool changed = !SameAction(actor.desired, actor.previous);
    if (!changed && now - actor.lastSend < kRefreshPeriod) return;
    auto state = actor.desired;
    if (changed) ++state.revision;
    const auto write = gWrite.load(std::memory_order_relaxed);
    if (write - gRead.load(std::memory_order_acquire) >= kRing) {
        gDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    actor.desired.revision = state.revision;
    actor.previous = state; actor.lastSend = now;
    gOutgoing[write % kRing] = state;
    gWrite.store(write + 1U, std::memory_order_release);
    gObserved.fetch_add(1U, std::memory_order_relaxed);
    if (state.kind == 1U) gMovementPublished.fetch_add(1U, std::memory_order_relaxed);
}

// Interior hook at 0073433E. The original preamble 007342A0..0073433D still
// runs, including +4234/+4238 updates and Character housekeeping. EDI is on
// the retail stack at this point. Both paths restore all registers/flags.
// Client skips ONLY 0073433E..007344D3 (senses/targeting/BT/roles/AI adapters),
// then takes the retail POP ESI / RET 8 epilogue at 007344D4.
int __cdecl BypassBranch() noexcept {
    return gUpdating != nullptr && gUpdating->blocked ? 1 : 0;
}
__declspec(naked) void BranchDetour() {
    __asm {
        pushfd
        pushad
        call BypassBranch
        test eax, eax
        jnz bypass
        popad
        popfd
        jmp dword ptr [gBranchTrampoline]
    bypass:
        popad
        popfd
        pop edi
        jmp dword ptr [gBranchExit]
    }
}
void __fastcall UpdateDetour(void* manager, void*, std::uint32_t previousTime, float delta) noexcept {
    const auto original = reinterpret_cast<ManagerUpdate>(gUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (!OwnsAI() || !EngineThread()) { original(manager, previousTime, delta); return; }
    if (!Client() && !Stable()) { original(manager, previousTime, delta); return; }
    gManagerUpdates.fetch_add(1U, std::memory_order_relaxed);
    std::uintptr_t c{};
    if (!Read(reinterpret_cast<std::uintptr_t>(manager) + 0x128U, c) || c == 0U) {
        gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
        original(manager, previousTime, delta); return;
    }
    Actor* actor = Admit(c);
    // A capacity/key refusal must NOT silently re-enable an eligible Client brain.
    Actor refused{};
    std::uint32_t type{};
    if (actor == nullptr && Client() && Eligible(c, type)) {
        refused.character = c; refused.blocked = true; actor = &refused;
    }
    auto* previous = gUpdating;
    gUpdating = actor;
    if (actor != nullptr) {
        actor->blocked = Client();
        if (actor->blocked) gSuspended.fetch_add(1U, std::memory_order_relaxed);
    }
    original(manager, previousTime, delta);
    gUpdating = previous;
    if (actor == nullptr || actor == &refused || actor->character != c) return;
    if (actor->duplicate) { static_cast<void>(Release(*actor)); return; }
    if (gRole.load(std::memory_order_acquire) == Role::Host) {
        Publish(*actor);
    } else if (Client()) {
        const auto now = GetTickCount64();
        if (!Stable() || gRole.load(std::memory_order_acquire) == Role::FrozenClient ||
            actor->faulted || actor->desired.kind == 0U || now - actor->lastReceive > kTimeout ||
            !CharacterReady(*actor)) {
            static_cast<void>(Release(*actor));
        } else if (actor->completedRevision != actor->desired.revision) {
            if (!RunMovement(*actor)) static_cast<void>(Release(*actor));
        }
    }
}
void __fastcall PrepassDetour(void* context, void*) noexcept {
    std::uintptr_t manager{}, owner{};
    std::uint32_t type{};
    // FUN_00733850 assigns (Character+34AC)->+4 to the embedded AI manager.
    // FUN_00712020 invokes task arbitration BEFORE the normal manager frame.
    if (Client() && EngineThread() && Read(reinterpret_cast<std::uintptr_t>(context) + 4U, manager) &&
        Read(manager + 0x128U, owner) && Eligible(owner, type)) return;
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gPrepassOriginal.load());
    if (original != nullptr) original(context);
}
void __fastcall CoordinatorDetour(void* tree, void*, std::uint32_t execute) noexcept {
    const auto original = reinterpret_cast<Coordinator>(gCoordinatorOriginal.load(std::memory_order_acquire));
    // CATHODE forced-target/relance also call this outside the ordinary AI frame.
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (Client() && EngineThread() && Read(reinterpret_cast<std::uintptr_t>(tree) + 0xCU, owner) &&
        Eligible(owner, type)) return;
    if (original != nullptr) original(tree, execute);
}
std::uint32_t __fastcall MovementDetour(void* node, void*, void* c, void* state,
    const float* destination, std::uint32_t rangeEnabled, float stoppingDistance,
    float tolerance, std::uint32_t flag) noexcept {
    const auto original = reinterpret_cast<Movement>(gMovementOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 2U;
    const auto result = original(node, c, state, destination, rangeEnabled, stoppingDistance, tolerance, flag);
    if (gRole.load(std::memory_order_acquire) != Role::Host || !EngineThread() || !Stable()) return result;
    auto* actor = gUpdating;
    if (actor == nullptr || actor->character != reinterpret_cast<std::uintptr_t>(c))
        actor = Admit(reinterpret_cast<std::uintptr_t>(c));
    if (actor == nullptr || actor->duplicate || actor->faulted) return result;
    gMovementCalls.fetch_add(1U, std::memory_order_relaxed);
    std::uintptr_t table{};
    if (!Read(reinterpret_cast<std::uintptr_t>(node), table)) return result;
    const auto rva = table >= gBase ? static_cast<std::uint32_t>(table - gBase) : 0U;
    gLastMovementDefinitionRva.store(rva, std::memory_order_relaxed);
    // Admit only audited helper contracts. MOVE_TO_TARGET and BACKSTAGE_SWEEP
    // differ in execution policy, now sampled after native resolution. Their
    // +40C/+410 callbacks are both false, so no extra focal payload is omitted.
    // BREAKOUT/vent transitions retain different contracts and stay passive.
    if (!SupportedMovement(table)) {
        actor->hostExecutionState = 0U; actor->desired.kind = 0U;
        gUnsupportedMovement.fetch_add(1U, std::memory_order_relaxed); return result;
    }
    std::uintptr_t ownerTree{};
    if (!Read(reinterpret_cast<std::uintptr_t>(state) + 0x14U, ownerTree) || ownerTree != actor->tree) {
        gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed); return result;
    }
    auto value = actor->desired;
    if (!Read(reinterpret_cast<std::uintptr_t>(node) + 8U, value.movementSpeedType) ||
        !Read(reinterpret_cast<std::uintptr_t>(state) + 0x3CU, value.locomotionStyle)) return result;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!Read(reinterpret_cast<std::uintptr_t>(destination) + i * sizeof(float), value.destination[i]))
            return result;
    value.kind = result == 0U ? 1U : 0U;
    value.stoppingDistance = stoppingDistance; value.tolerance = tolerance;
    value.flags = (rangeEnabled & 1U) | ((flag & 1U) << 8U);
    if (result == 0U && !CapturePolicy(reinterpret_cast<std::uintptr_t>(state),
            actor->character, value)) {
        actor->hostExecutionState = 0U; actor->desired.kind = 0U;
        gPolicyRejected.fetch_add(1U, std::memory_order_relaxed); return result;
    }
    if (Valid(value)) {
        actor->desired = value;
        actor->hostExecutionState = result == 0U ? reinterpret_cast<std::uintptr_t>(state) : 0U;
        if (actor != gUpdating) gOutsideFrameMovement.fetch_add(1U, std::memory_order_relaxed);
        gCaptured.fetch_add(1U, std::memory_order_relaxed);
    }
    else gRefused.fetch_add(1U, std::memory_order_relaxed);
    return result;
}

bool Hook(HookManager& hooks, const char* name, std::uintptr_t rva, void* detour,
    std::initializer_list<unsigned int> prefix, void** trampoline,
    std::atomic<void*>* published, std::string& error,
    const std::uint32_t relocatedAbsoluteRva = 0U) {
    MaskedBytes bytes;
    for (const auto byte : prefix) bytes.bytes.push_back(static_cast<std::uint8_t>(byte));
    if (relocatedAbsoluteRva != 0U) {
        // Retail HIGHLOW relocations at RVA 00312023 / 0030A943 adjust the
        // absolute SEH-handler operand in PUSH -1; PUSH handler. Match the
        // actual loaded address, without wildcarding the certified prefix.
        if (bytes.bytes.size() != 7U || bytes.bytes[0] != 0x6AU ||
            bytes.bytes[1] != 0xFFU || bytes.bytes[2] != 0x68U) {
            error = std::string(name) + ": unexpected relocated prefix shape";
            return false;
        }
        const auto absolute = static_cast<std::uint32_t>(gBase) + relocatedAbsoluteRva;
        std::memcpy(bytes.bytes.data() + 3U, &absolute, sizeof(absolute));
    }
    bytes.mask.assign(bytes.bytes.size(), 'x');
    HookRequest request;
    request.name = name; request.target = reinterpret_cast<void*>(gBase + rva);
    request.detour = detour; request.patchLength = bytes.bytes.size();
    request.expected = bytes; request.relocationSafePrefix = true;
    request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
    return hooks.Install(request, error);
}
} // namespace

bool Install(const std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    GameThreadDispatcher& dispatcher, std::string& error) {
    if (base < 0x10000U || !hooks.MutationAllowed()) {
        error = "AISync V1 requires the certified retail x86 executable and its resolved loaded module base.";
        return false;
    }
    gBase = base; gGenerations = &generations; gDispatcher = &dispatcher;
    gBranchExit = reinterpret_cast<void*>(base + 0x003344D4U);
    const auto first = hooks.InstalledCount();
    // Complete instructions; no relative CALL/Jcc in a prefix. The two PUSH
    // handler operands below are certified PE HIGHLOW relocations; their loaded
    // absolute values remain valid when copied into the trampoline.
    const bool installed =
        Hook(hooks, "aisync_brain_branch", 0x0033433EU, reinterpret_cast<void*>(&BranchDetour),
            {0x8B,0x8E,0xF4,0x00,0x00,0x00,0x5F}, &gBranchTrampoline, &gBranchPublished, error) &&
        Hook(hooks, "aisync_manager_update", 0x003342A0U, reinterpret_cast<void*>(&UpdateDetour),
            {0x56,0x8B,0xF1,0x8B,0x8E,0x28,0x01,0x00,0x00}, &gUpdateTrampoline, &gUpdateOriginal, error) &&
        Hook(hooks, "aisync_coordinator", 0x0032B880U, reinterpret_cast<void*>(&CoordinatorDetour),
            {0x83,0xEC,0x08,0x53,0x55}, &gCoordinatorTrampoline, &gCoordinatorOriginal, error) &&
        Hook(hooks, "aisync_task_arbitration", 0x00312020U, reinterpret_cast<void*>(&PrepassDetour),
            {0x6A,0xFF,0x68,0xE8,0xC2,0x12,0x01}, &gPrepassTrampoline, &gPrepassOriginal, error,
            0x00D2C2E8U) &&
        Hook(hooks, "aisync_resolved_movement", 0x0030A940U, reinterpret_cast<void*>(&MovementDetour),
            {0x6A,0xFF,0x68,0x98,0x97,0x12,0x01}, &gMovementTrampoline, &gMovementOriginal, error,
            0x00D29798U);
    if (!installed) {
        std::string rollback;
        static_cast<void>(hooks.UninstallFrom(first, rollback));
        if (!rollback.empty()) error += "; rollback: " + rollback;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
}
bool Configure(const std::span<const std::byte> payload) noexcept {
    Configuration value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (value.version != 1U || static_cast<std::uint32_t>(value.role) > 3U ||
        (value.role != Role::Retail && value.sessionCookie == 0U) ||
        (value.role != Role::Retail && value.role != Role::FrozenClient &&
            (value.nativeGeneration == 0U || value.worldGeneration == 0U))) return false;
    std::lock_guard lock(gMailboxMutex);
    gRequested = value; gConfigurationPending = true;
    gIncomingUsed.fill(false);
    return true;
}
bool QueueRemote(const std::span<const std::byte> payload) noexcept {
    State value{};
    if (payload.size() != sizeof(value) || !Installed()) return false;
    std::memcpy(&value, payload.data(), sizeof(value));
    if (!Valid(value)) return false;
    std::lock_guard lock(gMailboxMutex);
    const auto& configuration = gRequested;
    if (configuration.role != Role::Client || value.sessionCookie != configuration.sessionCookie ||
        value.worldGeneration != configuration.worldGeneration) {
        gDropped.fetch_add(1U, std::memory_order_relaxed); return true;
    }
    gRemoteQueued.fetch_add(1U, std::memory_order_relaxed);
    std::size_t free = kActors;
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) { if (free == kActors) free = i; continue; }
        const auto& old = gIncoming[i];
        if (old.entityA != value.entityA || old.entityB != value.entityB) continue;
        if (value.birth < old.birth || (value.birth == old.birth && value.revision < old.revision)) return true;
        gIncoming[i] = value; gIncomingAt[i] = GetTickCount64(); return true;
    }
    if (free == kActors) { gDropped.fetch_add(1U, std::memory_order_relaxed); return true; }
    gIncoming[free] = value; gIncomingUsed[free] = true; gIncomingAt[free] = GetTickCount64();
    gNextIdentityProbe[free] = 0ULL;
    return true;
}
bool TryPopObserved(State& state) noexcept {
    const auto read = gRead.load(std::memory_order_relaxed);
    if (read == gWrite.load(std::memory_order_acquire)) return false;
    state = gOutgoing[read % kRing];
    gRead.store(read + 1U, std::memory_order_release);
    return true;
}
void PumpOnEngineThread() noexcept {
    if (!Installed() || !EngineThread()) return;
    std::unique_lock lock(gMailboxMutex, std::try_to_lock);
    if (!lock.owns_lock()) return; // never wait for IPC on the engine thread
    if (gConfigurationPending) {
        if (gRequested.role == Role::Retail || gRequested.sessionCookie != gCurrent.sessionCookie ||
            gRequested.worldGeneration != gCurrent.worldGeneration) {
            for (auto& actor : gActors) {
                if (actor.character != 0U && gGeneration == gGenerations->CurrentGeneration())
                    static_cast<void>(Release(actor));
                else if (actor.executionState != nullptr) gOrphaned.fetch_add(1U, std::memory_order_relaxed);
                actor = {};
            }
            // Keep births monotonic across live OFF/ON toggles. A Client may
            // retain an older Host birth while only the Host is being isolated.
        }
        gCurrent = gRequested; gConfigurationPending = false;
        gActiveNativeGeneration.store(gCurrent.nativeGeneration, std::memory_order_relaxed);
        gActiveWorldGeneration.store(gCurrent.worldGeneration, std::memory_order_relaxed);
        gRole.store(gCurrent.role, std::memory_order_release);
    }
    const auto generation = gGenerations->CurrentGeneration();
    if (generation != gGeneration) {
        // Normal destruction hooks release states BEFORE Character/BT destruction.
        // Never dereference an old-generation Character on an exceptional missed hook.
        for (auto& actor : gActors) {
            if (actor.executionState != nullptr) gOrphaned.fetch_add(1U, std::memory_order_relaxed);
            actor = {};
        }
        gGeneration = generation;
        gIncomingUsed.fill(false);
    }
    if (!Stable() || !Client()) return;
    const auto now = GetTickCount64();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (!gIncomingUsed[i]) continue;
        if (now - gIncomingAt[i] > kTimeout) {
            gIncomingUsed[i] = false; gMissing.fetch_add(1U, std::memory_order_relaxed); continue;
        }
        const auto value = gIncoming[i];
        if (value.sessionCookie != gCurrent.sessionCookie || value.worldGeneration != gCurrent.worldGeneration) {
            gIncomingUsed[i] = false; continue;
        }
        if (now < gNextIdentityProbe[i]) continue;
        Actor* found = nullptr;
        for (auto& actor : gActors)
            if (actor.character != 0U && !actor.duplicate && actor.entityA == value.entityA &&
                actor.entityB == value.entityB && actor.characterClass == value.characterClass) found = &actor;
        // Spawn can legitimately follow an incoming snapshot. Retain one latest
        // snapshot per identity; no spawn fallback or matching by list index.
        if (found == nullptr) {
            gMissing.fetch_add(1U, std::memory_order_relaxed);
            gNextIdentityProbe[i] = now + kSendPeriod;
            continue;
        }
        auto& actor = *found;
        gIncomingUsed[i] = false;
        if (value.birth < actor.remoteBirth ||
            (value.birth == actor.remoteBirth && value.revision < actor.remoteRevision)) continue;
        if (value.birth != actor.remoteBirth || value.movementSpeedType != actor.desired.movementSpeedType ||
            value.locomotionStyle != actor.desired.locomotionStyle || value.kind == 0U)
            static_cast<void>(Release(actor));
        if (value.birth != actor.remoteBirth) actor.completedRevision = 0U;
        actor.remoteBirth = value.birth; actor.remoteRevision = value.revision;
        actor.desired = value; actor.lastReceive = now;
        gApplied.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ObserveOwnedCharacter(const std::uintptr_t c) noexcept {
    if (c == 0U) return;
    const auto generation = gGenerations == nullptr ? 0U : gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        auto& owned = gOwned[i];
        auto value = owned.load(std::memory_order_relaxed);
        if (value == c || ((value == 0U || gOwnedGeneration[i].load() != generation) &&
                owned.compare_exchange_strong(value, c))) {
            gOwnedGeneration[i].store(generation,
                std::memory_order_release);
            return;
        }
    }
}
void ForgetCharacter(const std::uintptr_t c) noexcept {
    for (auto& owned : gOwned) if (owned.load(std::memory_order_relaxed) == c) owned.store(0U);
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) { static_cast<void>(Release(*actor)); *actor = {}; }
}
void ForgetSpawnKey(const std::uint32_t id) noexcept {
    if (!EngineThread()) return;
    for (auto& actor : gActors)
        if (actor.character != 0U && actor.runtimeId == id) ForgetCharacter(actor.character);
}
void TransportLost() noexcept {
    std::lock_guard lock(gMailboxMutex);
    if (gRequested.role == Role::Client || Client()) gRequested.role = Role::FrozenClient;
    else gRequested.role = Role::Retail;
    gConfigurationPending = true; gIncomingUsed.fill(false);
}
void BeginLogicalShutdown() noexcept {
    if (gDispatcher != nullptr) gShutdownEngineThread.store(gDispatcher->EngineThreadId());
    gStopping.store(true, std::memory_order_release);
    TransportLost();
    gRole.store(Client() ? Role::FrozenClient : Role::Retail, std::memory_order_release);
}
bool OwnsAI() noexcept {
    const auto role = gRole.load(std::memory_order_acquire);
    return Installed() && role != Role::Retail;
}
bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
Counters DiagnosticsSnapshot() noexcept {
    Counters value{gObserved.load(), gApplied.load(), gDropped.load(), gRefused.load(), gSuspended.load(), gFaults.load(),
        gCaptured.load(), gStarted.load(), gCompleted.load(), gMissing.load(), gOrphaned.load()};
    value.managerUpdates = gManagerUpdates.load(); value.admitted = gAdmitted.load();
    value.identityRejected = gIdentityRejected.load(); value.treeUnavailable = gTreeUnavailable.load();
    value.ownerMismatch = gOwnerMismatch.load(); value.movementCalls = gMovementCalls.load();
    value.unsupportedMovement = gUnsupportedMovement.load(); value.remoteQueued = gRemoteQueued.load();
    value.outsideFrameMovement = gOutsideFrameMovement.load();
    value.movementPublished = gMovementPublished.load(); value.inactiveCaptured = gInactiveCaptured.load();
    value.policyCaptured = gPolicyCaptured.load(); value.policyApplied = gPolicyApplied.load();
    value.policyRejected = gPolicyRejected.load();
    value.role = static_cast<std::uint32_t>(gRole.load());
    value.nativeGeneration = gActiveNativeGeneration.load(); value.worldGeneration = gActiveWorldGeneration.load();
    value.lastMovementDefinitionRva = gLastMovementDefinitionRva.load();
    value.lastEntityA = gLastEntityA.load(); value.lastEntityB = gLastEntityB.load();
    value.lastCharacterClass = gLastCharacterClass.load();
    value.stable = gGenerations != nullptr && gGenerations->IsStableGeneration(value.nativeGeneration);
    return value;
}
void LogDiagnostics(Diagnostics& diagnostics) {
    const auto ai = DiagnosticsSnapshot();
    diagnostics.Log("info", "aisync_v1_snapshot", "AISync V1 cumulative counters; sampled outside engine callbacks.",
        {{"installed", Diagnostics::Boolean(Installed()), true},
         {"ownsAI", Diagnostics::Boolean(OwnsAI()), true},
         {"role", Diagnostics::Unsigned(ai.role), true},
         {"nativeGeneration", Diagnostics::Unsigned(ai.nativeGeneration), true},
         {"worldGeneration", Diagnostics::Unsigned(ai.worldGeneration), true},
         {"stable", Diagnostics::Boolean(ai.stable), true},
         {"managerUpdates", Diagnostics::Unsigned(ai.managerUpdates), true},
         {"admitted", Diagnostics::Unsigned(ai.admitted), true},
         {"identityRejected", Diagnostics::Unsigned(ai.identityRejected), true},
         {"treeUnavailable", Diagnostics::Unsigned(ai.treeUnavailable), true},
         {"ownerMismatch", Diagnostics::Unsigned(ai.ownerMismatch), true},
         {"movementCalls", Diagnostics::Unsigned(ai.movementCalls), true},
         {"unsupportedMovement", Diagnostics::Unsigned(ai.unsupportedMovement), true},
         {"outsideFrameMovement", Diagnostics::Unsigned(ai.outsideFrameMovement), true},
         {"movementPublished", Diagnostics::Unsigned(ai.movementPublished), true},
         {"inactiveCaptured", Diagnostics::Unsigned(ai.inactiveCaptured), true},
         {"policyCaptured", Diagnostics::Unsigned(ai.policyCaptured), true},
         {"policyApplied", Diagnostics::Unsigned(ai.policyApplied), true},
         {"policyRejected", Diagnostics::Unsigned(ai.policyRejected), true},
         {"lastMovementDefinitionRva", Diagnostics::Unsigned(ai.lastMovementDefinitionRva), true},
         {"lastEntityA", Diagnostics::Unsigned(ai.lastEntityA), true},
         {"lastEntityB", Diagnostics::Unsigned(ai.lastEntityB), true},
         {"lastCharacterClass", Diagnostics::Unsigned(ai.lastCharacterClass), true},
         {"remoteQueued", Diagnostics::Unsigned(ai.remoteQueued), true},
         {"observed", Diagnostics::Unsigned(ai.observed), true},
         {"applied", Diagnostics::Unsigned(ai.applied), true},
         {"dropped", Diagnostics::Unsigned(ai.dropped), true},
         {"refused", Diagnostics::Unsigned(ai.refused), true},
         {"suspended", Diagnostics::Unsigned(ai.suspended), true},
         {"faults", Diagnostics::Unsigned(ai.faults), true},
         {"capturedMovement", Diagnostics::Unsigned(ai.capturedMovement), true},
         {"startedMovement", Diagnostics::Unsigned(ai.startedMovement), true},
         {"completedMovement", Diagnostics::Unsigned(ai.completedMovement), true},
         {"missingIdentity", Diagnostics::Unsigned(ai.missingIdentity), true},
         {"orphanedStates", Diagnostics::Unsigned(ai.orphanedStates), true}});
}
} // namespace aimp::ai_sync
