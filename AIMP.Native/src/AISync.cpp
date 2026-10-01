#include "aimp/AISync.h"
#include "aimp/BuildProfile.h"
#include "aimp/CharacterDirectStepRuntime.h"
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
constexpr std::size_t kActors = 32U, kRing = 256U;
constexpr ULONGLONG kSendPeriod = 50ULL, kRefreshPeriod = 250ULL, kTimeout = 1500ULL;
constexpr ULONGLONG kManagerDormancyPeriod = 500ULL;
constexpr ULONGLONG kPairedTransitionMaximumHold = 3000ULL;
constexpr std::uint32_t kStateFlags = PoseValid | Moving | ActionValid | InteractionValid | RoleTaskValid;
constexpr std::uint32_t kInteractionFlags = InteractionActorIsSource |
    InteractionTargetIsParticipant | InteractionPairValid;
constexpr std::uint32_t kLifecycleFlags = LifecycleSpatialDiscontinuity | LifecycleTraversalAction |
    LifecycleWithdrawn | LifecycleWithdrawState2 | LifecycleHostDormant;
constexpr float kLifecycleDiscontinuityDistance = 3.5F;
constexpr float kLifecycleVerticalDiscontinuity = 2.5F;
constexpr std::uintptr_t kPositionOffset = 0x70U, kYawOffset = 0x174U;
constexpr std::uintptr_t kInteractionManagerGlobalRva = 0x012F0C88U;
constexpr auto kCharacterRegistry = character_direct_step_runtime::NativeCharacterRegistry();
constexpr float kMotionThreshold = 0.02F, kMaximumVelocity = 20.0F;
using ManagerUpdate = void(__thiscall*)(void*, std::uint32_t, float);
using Coordinator = void(__thiscall*)(void*, std::uint32_t);
using CharacterInteraction = std::uint32_t(__thiscall*)(void*, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, std::uint64_t, std::int32_t);
using PairedTransitionA = void(__thiscall*)(void*, std::uintptr_t, std::int32_t, std::uint32_t);
using PairedTransitionB = void(__thiscall*)(void*, std::uintptr_t, std::int32_t,
    std::uint32_t, std::uint32_t);
using CharacterImpact = void(__thiscall*)(void*, int*);
struct Actor {
    std::uintptr_t character{}, tree{}, lastRoleTask{};
    std::uint32_t runtimeId{}, entityA{}, entityB{}, characterClass{}, birth{};
    State desired{}, previous{};
    ULONGLONG lastSend{}, lastReceive{}, lastManagerUpdate{};
    ULONGLONG lastPoseSample{}, lastProxyRefresh{}, lastInteractionAttempt{}, lastInteractionCapture{};
    ULONGLONG interactionHoldUntil{};
    std::uint32_t completedRevision{}, completedInteractionRevision{};
    std::uint32_t completedActionRevision{}, completedRoleTaskRevision{}, completedLifecycleRevision{};
    std::uint32_t interactionActionRevision{}, interactionActionPathHash{};
    std::uint32_t remoteBirth{}, remoteRevision{};
    float lastPosition[3]{};
    bool blocked{}, duplicate{}, faulted{}, hasLastPosition{}, interactionOwnsCycle{};
    bool traversalAction{}, hostDormant{}, withdrawKnown{};
    std::uint32_t withdrawState{};
};
std::array<Actor, kActors> gActors{}; // engine thread only
// NPC-backed Characters created by AIMP represent remote human players. Their
// addresses are known only after CharacterCreate returns, while their runtime
// spawn keys are known before construction and close the re-entrant admission
// window inside the retail constructor.
std::array<std::atomic<std::uintptr_t>, kActors> gOwned{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedGeneration{};
std::array<std::atomic<std::uint32_t>, kActors> gOwnedSpawnKeys{};
std::array<std::atomic<std::uint64_t>, kActors> gOwnedSpawnKeyGeneration{};
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
std::atomic<void*> gUpdateOriginal{}, gCoordinatorOriginal{}, gInteractionOriginal{};
std::atomic<void*> gPairedTransitionAOriginal{}, gPairedTransitionBOriginal{};
std::atomic<void*> gCharacterImpactOriginal{}, gCharacterImpactSecondaryOriginal{};
std::atomic<void*> gPrepassOriginal{};
void* gPrepassTrampoline{};
void* gUpdateTrampoline{}, *gCoordinatorTrampoline{}, *gInteractionTrampoline{};
void* gPairedTransitionATrampoline{}, *gPairedTransitionBTrampoline{};
void* gCharacterImpactTrampoline{}, *gCharacterImpactSecondaryTrampoline{};
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
std::atomic<std::uint64_t> gMissing{};
std::atomic<std::uint64_t> gManagerUpdates{}, gAdmitted{}, gIdentityRejected{}, gTreeUnavailable{}, gOwnerMismatch{};
std::atomic<std::uint64_t> gRemoteQueued{};
std::atomic<std::uint64_t> gPoseCaptured{}, gPoseApplied{}, gPoseRejected{};
std::atomic<std::uint64_t> gActionCaptured{}, gActionTransitions{};
std::atomic<std::uint64_t> gRoleTaskCaptured{}, gRoleTaskTransitions{};
std::atomic<std::uint64_t> gLifecycleCaptured{}, gLifecycleApplied{}, gLifecycleSnaps{}, gExecutionBarriers{};
std::atomic<std::uint64_t> gWithdrawSamples{}, gWithdrawTransitions{}, gWithdrawApplied{};
std::atomic<std::uint64_t> gInteractionCaptured{}, gInteractionApplied{}, gInteractionRejected{};
std::atomic<std::uint64_t> gInteractionRemapped{};
std::atomic<std::uint64_t> gPairedTransitionCaptured{}, gPairedTransitionApplied{};
std::atomic<std::uint64_t> gPlayerRepresentationsRegistered{};
std::atomic<std::uint64_t> gProvisionalPlayerActorsRetired{};
std::atomic<std::uint32_t> gActiveNativeGeneration{}, gActiveWorldGeneration{};
std::atomic<std::uint32_t> gLastEntityA{}, gLastEntityB{}, gLastCharacterClass{};
std::atomic<std::uint32_t> gLastActionTypeRva{}, gLastActionPathHash{}, gLastActionStatus{};
std::atomic<std::uint32_t> gLastRoleTaskTypeRva{}, gLastRoleTaskKeyA{}, gLastRoleTaskKeyB{};
std::atomic<std::uint32_t> gLastLifecycleFlags{};
std::atomic<std::uint32_t> gLastWithdrawState{};
std::atomic<std::uint32_t> gLastInteractionTargetA{}, gLastInteractionTargetB{};
std::atomic<std::uint32_t> gLastInteractionTransition{};

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
bool FiniteCoordinate(const float value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 100000.0F;
}
// Certified retail NPC Character families used by AISync. Keep this boundary
// fail-closed: class 0 is used by AIMP's dynamically created player visual in
// the observed PlayerSync path, while 2/7 are retail player families. Unknown
// Character classes must never become AI authority merely because they own a
// behaviour manager/tree.
bool AICharacterClass(const std::uint32_t type) noexcept {
    switch (type) {
    case 1U:
    case 3U:
    case 4U:
    case 6U:
    case 9U:
        return true;
    default:
        return false;
    }
}
bool Valid(const State& s) noexcept {
    if (s.version != 5U || s.nativeGeneration == 0U || s.sessionCookie == 0U ||
        s.worldGeneration == 0U || (s.entityA == 0U && s.entityB == 0U) ||
        s.birth == 0U || s.revision == 0U || !AICharacterClass(s.characterClass) ||
        (s.stateFlags & ~kStateFlags) != 0U || s.moveType > 3U ||
        s.actionStatus > 10U || s.actionDepth > 64U ||
        (s.interactionFlags & ~kInteractionFlags) != 0U ||
        s.interactionTransition > 2U || (s.interactionTransitionFlags & ~0xFFFFU) != 0U ||
        s.interactionVariant < -1 || s.interactionVariant > 0xFFFF ||
        (s.lifecycleFlags & ~kLifecycleFlags) != 0U) return false;
    for (const auto coordinate : s.position)
        if (!FiniteCoordinate(coordinate)) return false;
    for (const auto coordinate : s.interactionTargetPosition)
        if (!FiniteCoordinate(coordinate)) return false;
    if (!std::isfinite(s.yaw) || std::abs(s.yaw) > 3.3F ||
        !std::isfinite(s.velocityX) || !std::isfinite(s.velocityZ) ||
        std::abs(s.velocityX) > kMaximumVelocity || std::abs(s.velocityZ) > kMaximumVelocity)
        return false;
    if ((s.stateFlags & PoseValid) == 0U && (s.stateFlags & Moving) != 0U) return false;
    if ((s.stateFlags & ActionValid) != 0U &&
        (s.actionTypeRva == 0U || s.actionDepth == 0U || s.actionRevision == 0U)) return false;
    if ((s.stateFlags & InteractionValid) != 0U &&
        (s.interactionRevision == 0U ||
         (s.interactionTargetA == 0U && s.interactionTargetB == 0U) ||
         ((s.interactionFlags & InteractionPairValid) == 0U &&
             s.interactionTransition == 0U))) return false;
    if ((s.stateFlags & InteractionValid) == 0U && s.interactionTransition != 0U) return false;
    if ((s.stateFlags & RoleTaskValid) != 0U &&
        (s.roleTaskRevision == 0U || s.roleTaskTypeRva == 0U)) return false;
    if (s.lifecycleFlags != 0U && s.lifecycleRevision == 0U) return false;
    return true;
}
bool RegisteredPlayerRepresentation(const std::uintptr_t character) noexcept {
    if (character == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwned[i].load(std::memory_order_acquire) == character &&
            gOwnedGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool RegisteredPlayerSpawnKey(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return false;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i)
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return true;
    return false;
}
bool PlayerRepresentation(const std::uintptr_t character) noexcept {
    if (RegisteredPlayerRepresentation(character)) return true;
    std::uint32_t runtimeId{};
    return Read(character + 0x50U, runtimeId) && RegisteredPlayerSpawnKey(runtimeId);
}
bool Eligible(const std::uintptr_t c, std::uint32_t& type) noexcept {
    // PlayerSync owns every dynamically created remote-player Character. Test
    // the explicit ownership registry first, then require a certified NPC class
    // as an independent fail-closed boundary in case a spawn reservation is
    // cleared/reordered during retail lifecycle callbacks.
    if (PlayerRepresentation(c)) return false;
    std::uintptr_t characterTemplate{};
    if (!Read(c + 0x64U, characterTemplate) ||
        !Read(characterTemplate + 4U, type)) return false;
    // +1F is an execution gate, not proof that the NPC owns an initialized
    // manager/tree. Admit still validates the actual tree and Character owner.
    // AISync deliberately owns only the retail NPC families certified by the
    // reverse-engineering audit; unknown/player classes stay with their native
    // subsystem instead of being guessed into AI authority.
    return AICharacterClass(type);
}
bool CharacterIdentity(const std::uintptr_t character, std::uint32_t& entityA,
    std::uint32_t& entityB, std::uint32_t& type) noexcept {
    std::uintptr_t characterTemplate{};
    return character != 0U && Read(character + 0x54U, entityA) &&
        Read(character + 0x58U, entityB) && (entityA != 0U || entityB != 0U) &&
        Read(character + 0x64U, characterTemplate) &&
        Read(characterTemplate + 4U, type);
}
bool ParticipantCharacter(const std::uintptr_t character) noexcept {
    std::uint32_t entityA{}, entityB{}, type{};
    return CharacterIdentity(character, entityA, entityB, type) &&
        (type == 2U || type == 7U || RegisteredPlayerRepresentation(character));
}
template<class Visitor>
bool ForEachCharacter(Visitor&& visitor) noexcept {
    if (gBase == 0U) return false;
    const auto table = gBase + kCharacterRegistry.table;
    for (std::size_t index = 0U; index < kCharacterRegistry.capacity; ++index) {
        const auto entry = table + index * kCharacterRegistry.stride;
        std::uintptr_t character{};
        std::uint8_t active{};
        if (!Read(entry + kCharacterRegistry.character, character) ||
            !Read(entry + kCharacterRegistry.active, active)) return false;
        if (active != 0U && character != 0U && !visitor(character)) return true;
    }
    return true;
}
std::uintptr_t FindCharacterByIdentity(const std::uint32_t entityA,
    const std::uint32_t entityB) noexcept {
    std::uintptr_t found{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t character) noexcept {
        std::uint32_t candidateA{}, candidateB{}, type{};
        if (CharacterIdentity(character, candidateA, candidateB, type) &&
            candidateA == entityA && candidateB == entityB) {
            found = character;
            return false;
        }
        return true;
    }));
    return found;
}
std::uintptr_t FindParticipantNear(const float* position,
    const std::uintptr_t excluded) noexcept {
    std::uintptr_t found{};
    float bestDistanceSquared = 4.0F;
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t character) noexcept {
        if (character == excluded || !ParticipantCharacter(character)) return true;
        float x{}, y{}, z{};
        if (!Read(character + kPositionOffset, x) ||
            !Read(character + kPositionOffset + 4U, y) ||
            !Read(character + kPositionOffset + 8U, z) ||
            !FiniteCoordinate(x) || !FiniteCoordinate(y) || !FiniteCoordinate(z)) return true;
        const auto dx = x - position[0];
        const auto dy = y - position[1];
        const auto dz = z - position[2];
        const auto distanceSquared = dx * dx + dy * dy + dz * dz;
        if (std::isfinite(distanceSquared) && distanceSquared < bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            found = character;
        }
        return true;
    }));
    return found;
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

std::uint32_t HashActionWord(std::uint32_t hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= 16777619U;
    }
    return hash;
}

bool TraversalActionType(const std::uint32_t typeRva) noexcept {
    // Certified Alien traversal/backstage leaf vtables from the current retail build.
    // These are only lifecycle hints: the network never serializes Alien-only state.
    switch (typeRva) {
    case 0x00DBC498U: // MOVE_TO_BACKSTAGE_VIA_VENT_CLOSEST_TO_ALIEN
    case 0x00DBC8E0U: // MOVE_TO_FRONTSTAGE_VIA_FLANKED_VENT_CLOSEST_TO_PLAYER
    case 0x00EBC8B8U: // BACKSTAGE_AREA_SWEEP
    case 0x00ECA338U: // BACKSTAGE_ALIEN_RESPONSE
        return true;
    default:
        return false;
    }
}

std::uint32_t SemanticLifecycleFlags(const Actor& actor) noexcept {
    std::uint32_t flags{};
    if (actor.hostDormant) flags |= LifecycleHostDormant;
    if (actor.withdrawKnown && actor.withdrawState != 0U) {
        flags |= LifecycleWithdrawn;
        if (actor.withdrawState == 2U) flags |= LifecycleWithdrawState2;
    }
    return flags;
}

void RefreshLifecycleFlags(Actor& actor, const std::uint32_t eventFlags) noexcept {
    actor.desired.lifecycleFlags = (eventFlags &
        (LifecycleSpatialDiscontinuity | LifecycleTraversalAction)) |
        SemanticLifecycleFlags(actor);
}

void MarkLifecycle(Actor& actor, const std::uint32_t flags) noexcept {
    ++actor.desired.lifecycleRevision;
    if (actor.desired.lifecycleRevision == 0U) actor.desired.lifecycleRevision = 1U;
    RefreshLifecycleFlags(actor, flags);
    gLifecycleCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastLifecycleFlags.store(actor.desired.lifecycleFlags, std::memory_order_relaxed);
}

bool ResolveAlienWithdrawContext(const std::uintptr_t character,
    std::uintptr_t& context, std::uint32_t& state) noexcept {
    context = 0U;
    state = 0U;
    std::uintptr_t alienData{};
    if (character == 0U || !Read(character + 0x34F0U, alienData) || alienData == 0U ||
        !Read(alienData + 0x18U, context) || context == 0U || !Read(context, state) || state > 2U)
        return false;
    return true;
}

void CaptureWithdrawState(Actor& actor) noexcept {
    std::uintptr_t context{};
    std::uint32_t state{};
    if (!ResolveAlienWithdrawContext(actor.character, context, state)) return;
    gWithdrawSamples.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(state, std::memory_order_relaxed);
    if (!actor.withdrawKnown) {
        actor.withdrawKnown = true;
        actor.withdrawState = state;
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    if (actor.withdrawState == state) {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
        return;
    }
    actor.withdrawState = state;
    gWithdrawTransitions.fetch_add(1U, std::memory_order_relaxed);
    MarkLifecycle(actor, 0U);
}

std::uint32_t DesiredWithdrawState(const State& state) noexcept {
    if ((state.lifecycleFlags & LifecycleWithdrawState2) != 0U) return 2U;
    if ((state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U) return 1U;
    return 0U;
}

bool ApplyAlienWithdrawEntry(const std::uintptr_t context,
    const std::uint32_t desiredState) noexcept {
    __try {
        using Reset = void(__fastcall*)(void*);
        using Set = void(__thiscall*)(void*, int);
        const auto reset = reinterpret_cast<Reset>(gBase + 0x003349D0U); // FUN_007349D0
        const auto set = reinterpret_cast<Set>(gBase + 0x0032FC20U);     // FUN_0072FC20
        if (desiredState == 0U) {
            set(reinterpret_cast<void*>(context), 0);
            reset(reinterpret_cast<void*>(context));
        } else {
            reset(reinterpret_cast<void*>(context));
            set(reinterpret_cast<void*>(context), static_cast<int>(desiredState));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyAuthoritativeWithdraw(Actor& actor, const State& state) noexcept {
    std::uintptr_t context{};
    std::uint32_t current{};
    if (!ResolveAlienWithdrawContext(actor.character, context, current)) return true;
    const auto desired = DesiredWithdrawState(state);
    if (current == desired) return true;
    if (!ApplyAlienWithdrawEntry(context, desired)) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    gWithdrawApplied.fetch_add(1U, std::memory_order_relaxed);
    gLastWithdrawState.store(desired, std::memory_order_relaxed);
    return true;
}

bool ProxyWithdrawn(const State& state) noexcept {
    return (state.lifecycleFlags & (LifecycleWithdrawn | LifecycleHostDormant)) != 0U;
}

bool ResolveRoleTaskExecutor(const std::uintptr_t character,
    std::uintptr_t& roleManager, std::uintptr_t& executor) noexcept {
    roleManager = 0U;
    executor = 0U;
    // Retail role/task ownership is not embedded directly at Character+424.
    // Character+34DC owns the role manager, whose +424 slot points at the
    // active role-task executor. This is the exact chain used by the retail
    // role/task queries around FUN_006E87B0 and FUN_0076F080.
    return character != 0U && Read(character + 0x34DCU, roleManager) && roleManager != 0U &&
        Read(roleManager + 0x424U, executor) && executor != 0U;
}

bool ClearRoleTaskExecutor(Actor& actor) noexcept {
    std::uintptr_t roleManager{}, executor{};
    if (!ResolveRoleTaskExecutor(actor.character, roleManager, executor)) return true;
    std::uintptr_t currentTask{};
    if (!Read(executor + 8U, currentTask) || currentTask == 0U) return true;
    __try {
        // FUN_00761A90 is the retail role-task cleanup. It releases the admitted
        // task plus its movement requests instead of zeroing opaque state.
        reinterpret_cast<void(__fastcall*)(int*)>(gBase + 0x00361A90U)(
            reinterpret_cast<int*>(executor));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool QuiesceProxyExecution(Actor& actor) noexcept {
    if (actor.character == 0U) return false;
    static_cast<void>(character_direct_step_runtime::Stop(actor.character));
    const bool tree = actor.tree == 0U || SuspendExistingTree(actor);
    const bool task = ClearRoleTaskExecutor(actor);
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    actor.interactionOwnsCycle = false;
    actor.interactionHoldUntil = 0U;
    gExecutionBarriers.fetch_add(1U, std::memory_order_relaxed);
    return tree && task;
}

bool TeleportCharacterEntry(const std::uintptr_t entry, const float* position) noexcept {
    // Keep SEH in a trivial helper. MSVC rejects __try in callers that require
    // C++ object unwinding (SnapAuthoritativePose owns std::array/std::string).
    __try {
        using Teleport = void(__thiscall*)(void*, const float*);
        reinterpret_cast<Teleport>(gBase +
            character_direct_step_runtime::NativeMovementRvas().teleportCharacter)(
                reinterpret_cast<void*>(entry), position);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
}

bool SnapAuthoritativePose(Actor& actor, const State& state) noexcept {
    if ((state.stateFlags & PoseValid) == 0U || actor.character == 0U) return false;
    const auto table = gBase + kCharacterRegistry.table;
    std::uintptr_t entry{};
    for (std::size_t index = 0U; index < kCharacterRegistry.capacity; ++index) {
        const auto candidate = table + index * kCharacterRegistry.stride;
        std::uintptr_t character{};
        std::uint8_t active{};
        if (!Read(candidate + kCharacterRegistry.character, character) ||
            !Read(candidate + kCharacterRegistry.active, active)) return false;
        if (active != 0U && character == actor.character) { entry = candidate; break; }
    }
    if (entry == 0U) return false;
    alignas(16) const std::array<float, 4> position{
        state.position[0], state.position[1], state.position[2], 0.0F};
    if (!TeleportCharacterEntry(entry, position.data())) return false;
    std::string error;
    if (!character_direct_step_runtime::ApplyReplicatedYaw(actor.character, state.yaw, error))
        return false;
    actor.completedRevision = 0U;
    actor.lastProxyRefresh = 0U;
    gLifecycleSnaps.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void CaptureRoleTask(Actor& actor) noexcept {
    std::uint32_t typeRva{}, keyA{}, keyB{};
    std::uintptr_t roleManager{}, executor{}, task{}, table{}, descriptor{};
    bool valid = ResolveRoleTaskExecutor(actor.character, roleManager, executor) &&
        Read(executor + 8U, task) && task != 0U && Read(task, table) &&
        table >= gBase && table - gBase <= 0xFFFFFFFFULL;
    if (valid) {
        typeRva = static_cast<std::uint32_t>(table - gBase);
        if (Read(task + 0x18U, descriptor) && descriptor != 0U) {
            static_cast<void>(Read(descriptor + 4U, keyA));
            static_cast<void>(Read(descriptor + 8U, keyB));
        }
    }
    const bool wasValid = (actor.desired.stateFlags & RoleTaskValid) != 0U;
    // Revision follows admission/replacement of the actual task object, not
    // mutable task context fields. Pointer identity is kept Host-local only.
    const bool changed = valid != wasValid || task != actor.lastRoleTask;
    if (changed) {
        ++actor.desired.roleTaskRevision;
        if (actor.desired.roleTaskRevision == 0U) actor.desired.roleTaskRevision = 1U;
        gRoleTaskTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    actor.lastRoleTask = valid ? task : 0U;
    actor.desired.roleTaskTypeRva = valid ? typeRva : 0U;
    actor.desired.roleTaskKeyA = valid ? keyA : 0U;
    actor.desired.roleTaskKeyB = valid ? keyB : 0U;
    if (valid) {
        actor.desired.stateFlags |= RoleTaskValid;
        gRoleTaskCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastRoleTaskTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastRoleTaskKeyA.store(keyA, std::memory_order_relaxed);
        gLastRoleTaskKeyB.store(keyB, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~RoleTaskValid;
    }
}

void CaptureActiveAction(Actor& actor) noexcept {
    std::uint32_t typeRva{}, pathHash{}, status{}, depth{};
    std::uintptr_t header{}, begin{}, end{};
    if (Read(actor.tree + 0x44U, header) && Read(header, begin) && Read(header + 4U, end) &&
        end >= begin && (end - begin) % 4U == 0U && (end - begin) / 4U <= 64U) {
        depth = static_cast<std::uint32_t>((end - begin) / 4U);
        pathHash = 2166136261U;
        for (auto entry = begin; entry < end && depth != 0U; entry += 4U) {
            std::uintptr_t state{}, node{}, table{};
            std::uint8_t selectedChild{};
            if (!Read(entry, state) || !Read(state + 0x10U, node) || !Read(node, table) ||
                table < gBase || table - gBase > 0xFFFFFFFFULL ||
                !Read(state + 0x1CU, selectedChild)) {
                depth = 0U; break;
            }
            const auto rva = static_cast<std::uint32_t>(table - gBase);
            pathHash = HashActionWord(pathHash, rva);
            pathHash = HashActionWord(pathHash, selectedChild);
            if (entry + 4U == end) {
                typeRva = rva;
                if (!Read(state + 4U, status)) depth = 0U;
            }
        }
    }
    const bool valid = depth != 0U && typeRva != 0U && status <= 10U;
    const bool wasValid = (actor.desired.stateFlags & ActionValid) != 0U;
    const bool changed = valid != wasValid || actor.desired.actionTypeRva != typeRva ||
        actor.desired.actionPathHash != pathHash || actor.desired.actionStatus != status ||
        actor.desired.actionDepth != depth;
    if (changed) {
        ++actor.desired.actionRevision;
        if (actor.desired.actionRevision == 0U) actor.desired.actionRevision = 1U;
        gActionTransitions.fetch_add(1U, std::memory_order_relaxed);
    }
    const bool traversal = valid && TraversalActionType(typeRva);
    if (traversal != actor.traversalAction) {
        actor.traversalAction = traversal;
        MarkLifecycle(actor, traversal ? LifecycleTraversalAction : 0U);
    }
    actor.desired.actionTypeRva = valid ? typeRva : 0U;
    actor.desired.actionPathHash = valid ? pathHash : 0U;
    actor.desired.actionStatus = valid ? status : 0U;
    actor.desired.actionDepth = valid ? depth : 0U;
    if (valid) {
        actor.desired.stateFlags |= ActionValid;
        gActionCaptured.fetch_add(1U, std::memory_order_relaxed);
        gLastActionTypeRva.store(typeRva, std::memory_order_relaxed);
        gLastActionPathHash.store(pathHash, std::memory_order_relaxed);
        gLastActionStatus.store(status, std::memory_order_relaxed);
    } else {
        actor.desired.stateFlags &= ~ActionValid;
    }
}

std::uint32_t MoveTypeForSpeed(const float speed) noexcept {
    if (speed < 0.65F) return 0U;
    if (speed < 1.8F) return 1U;
    if (speed < 3.2F) return 2U;
    return 3U;
}

void CapturePose(Actor& actor, const ULONGLONG now) noexcept {
    float position[3]{}, yaw{};
    if (!Read(actor.character + kPositionOffset, position[0]) ||
        !Read(actor.character + kPositionOffset + 4U, position[1]) ||
        !Read(actor.character + kPositionOffset + 8U, position[2]) ||
        !Read(actor.character + kYawOffset, yaw) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2]) || !std::isfinite(yaw) || std::abs(yaw) > 3.3F) {
        actor.desired.stateFlags &= ~(PoseValid | Moving);
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    float velocityX{}, velocityZ{};
    if (actor.hasLastPosition && now > actor.lastPoseSample) {
        const auto elapsed = now - actor.lastPoseSample;
        const auto dx = position[0] - actor.lastPosition[0];
        const auto dy = position[1] - actor.lastPosition[1];
        const auto dz = position[2] - actor.lastPosition[2];
        const auto distance = std::hypot(dx, dz);
        const auto spatialDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (elapsed <= 500ULL && std::isfinite(spatialDistance) &&
            (spatialDistance >= kLifecycleDiscontinuityDistance ||
             std::abs(dy) >= kLifecycleVerticalDiscontinuity)) {
            MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
                (actor.traversalAction ? LifecycleTraversalAction : 0U));
        }
        if (elapsed >= 5ULL && elapsed <= 250ULL && std::isfinite(distance) && distance < 2.0F) {
            const auto scale = 1000.0F / static_cast<float>(elapsed);
            velocityX = dx * scale; velocityZ = dz * scale;
            if (!std::isfinite(velocityX) || !std::isfinite(velocityZ) ||
                std::abs(velocityX) > kMaximumVelocity || std::abs(velocityZ) > kMaximumVelocity) {
                velocityX = 0.0F; velocityZ = 0.0F;
            }
        }
    }
    for (std::size_t i = 0U; i < 3U; ++i) {
        actor.lastPosition[i] = position[i]; actor.desired.position[i] = position[i];
    }
    actor.hasLastPosition = true; actor.lastPoseSample = now;
    actor.desired.yaw = yaw;
    actor.desired.velocityX = velocityX; actor.desired.velocityZ = velocityZ;
    const auto speed = std::hypot(velocityX, velocityZ);
    actor.desired.moveType = MoveTypeForSpeed(speed);
    actor.desired.stateFlags |= PoseValid;
    if (std::isfinite(speed) && speed >= kMotionThreshold)
        actor.desired.stateFlags |= Moving;
    else
        actor.desired.stateFlags &= ~Moving;
    gPoseCaptured.fetch_add(1U, std::memory_order_relaxed);
}

Actor* Find(const std::uintptr_t c) noexcept {
    for (auto& actor : gActors) if (actor.character == c) return &actor;
    return nullptr;
}
bool Release(Actor& actor) noexcept;
Actor* Admit(const std::uintptr_t c) noexcept {
    // CharacterCreate may run a manager update before its detour can publish
    // the resulting address. The pre-reserved spawn key rejects that update;
    // the address check also retires an actor admitted by an older build/path.
    if (PlayerRepresentation(c)) {
        if (auto* actor = Find(c); actor != nullptr) {
            // CharacterDirectStep belongs to PlayerSync for this object. Do not
            // call Release(), which would stop the remote-player controller.
            *actor = {};
            gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
        }
        return nullptr;
    }
    if (auto* actor = Find(c); actor != nullptr) {
        std::uint32_t id{};
        std::uintptr_t tree{};
        if (!Read(c + 0x50U, id) || !Read(c + 0x34D8U, tree)) return nullptr;
        if (id == actor->runtimeId) {
            if (tree != actor->tree) {
                static_cast<void>(Release(*actor));
                actor->tree = tree; actor->completedRevision = 0U;
                std::uintptr_t owner{};
                actor->faulted = tree == 0U || !Read(tree + 0xCU, owner) || owner != c;
                if (!actor->faulted && Client()) actor->faulted = !SuspendExistingTree(*actor);
                if (actor->faulted) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
            }
            return actor;
        }
        // Address reuse without a destruction notification: do not touch a
        // request belonging to the old Character. Keep this visible in diagnostics.
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
Actor* FindActorByIdentity(const std::uint32_t entityA, const std::uint32_t entityB) noexcept {
    for (auto& actor : gActors)
        if (actor.character != 0U && !actor.duplicate &&
            actor.entityA == entityA && actor.entityB == entityB) return &actor;
    return nullptr;
}
void CaptureInteraction(const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    if ((sourceA == 0U && sourceB == 0U) || (targetA == 0U && targetB == 0U) ||
        variant < -1 || variant > 0xFFFF) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    // AISync authors only interactions initiated by an admitted authoritative
    // NPC. Player-originated impacts keep their existing Player/World channels.
    Actor* actor = FindActorByIdentity(sourceA, sourceB);
    if (actor == nullptr) {
        const auto source = FindCharacterByIdentity(sourceA, sourceB);
        if (source != 0U) actor = Admit(source);
    }
    if (actor == nullptr || actor->faulted || actor->duplicate) return;
    const auto mappedTargetA = targetA;
    const auto mappedTargetB = targetB;
    const auto target = FindCharacterByIdentity(mappedTargetA, mappedTargetB);
    if (target == 0U || target == actor->character) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    float position[3]{};
    if (!Read(target + kPositionOffset, position[0]) ||
        !Read(target + kPositionOffset + 4U, position[1]) ||
        !Read(target + kPositionOffset + 8U, position[2]) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2])) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto& state = actor->desired;
    ++state.interactionRevision;
    if (state.interactionRevision == 0U) state.interactionRevision = 1U;
    state.interactionTargetA = mappedTargetA;
    state.interactionTargetB = mappedTargetB;
    state.interactionFlags = InteractionActorIsSource | InteractionPairValid;
    if (ParticipantCharacter(target)) state.interactionFlags |= InteractionTargetIsParticipant;
    state.interactionTransition = 0U;
    state.interactionTransitionFlags = 0U;
    state.interactionPayloadLow = static_cast<std::uint32_t>(payload);
    state.interactionPayloadHigh = static_cast<std::uint32_t>(payload >> 32U);
    state.interactionVariant = variant;
    for (std::size_t i = 0U; i < 3U; ++i) state.interactionTargetPosition[i] = position[i];
    state.stateFlags |= InteractionValid;
    actor->lastInteractionCapture = GetTickCount64();
    gInteractionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTargetA.store(mappedTargetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(mappedTargetB, std::memory_order_relaxed);
}
Actor* FindActorByActionManager(const std::uintptr_t manager) noexcept {
    if (manager == 0U) return nullptr;
    for (auto& actor : gActors) {
        std::uintptr_t candidate{};
        if (actor.character != 0U && Read(actor.character + 0x36CU, candidate) &&
            candidate == manager) return &actor;
    }
    std::uintptr_t character{};
    static_cast<void>(ForEachCharacter([&](const std::uintptr_t candidate) noexcept {
        std::uintptr_t actionManager{};
        if (Read(candidate + 0x36CU, actionManager) && actionManager == manager) {
            character = candidate;
            return false;
        }
        return true;
    }));
    return character == 0U ? nullptr : Admit(character);
}
void CapturePairedTransition(const std::uintptr_t manager, const std::uintptr_t target,
    const std::int32_t variant, const std::uint32_t transition,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    if (variant < -1 || variant > 0xFFFF || firstFlag > 0xFFU || secondFlag > 0xFFU ||
        transition == 0U || transition > 2U) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    auto* actor = FindActorByActionManager(manager);
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (actor == nullptr || actor->faulted || actor->duplicate || target == actor->character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) return;
    float position[3]{};
    if (!Read(target + kPositionOffset, position[0]) ||
        !Read(target + kPositionOffset + 4U, position[1]) ||
        !Read(target + kPositionOffset + 8U, position[2]) ||
        !FiniteCoordinate(position[0]) || !FiniteCoordinate(position[1]) ||
        !FiniteCoordinate(position[2])) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    const auto now = GetTickCount64();
    auto& state = actor->desired;
    const bool augmentsPair = (state.stateFlags & InteractionValid) != 0U &&
        (state.interactionFlags & InteractionPairValid) != 0U &&
        state.interactionTargetA == targetA && state.interactionTargetB == targetB &&
        state.interactionVariant == variant && actor->lastInteractionCapture != 0U &&
        now - actor->lastInteractionCapture <= 50ULL;
    if (!augmentsPair) {
        ++state.interactionRevision;
        if (state.interactionRevision == 0U) state.interactionRevision = 1U;
        state.interactionTargetA = targetA;
        state.interactionTargetB = targetB;
        state.interactionFlags = InteractionActorIsSource;
        if (ParticipantCharacter(target)) state.interactionFlags |= InteractionTargetIsParticipant;
        state.interactionPayloadLow = 0U;
        state.interactionPayloadHigh = 0U;
        state.interactionVariant = variant;
        for (std::size_t i = 0U; i < 3U; ++i)
            state.interactionTargetPosition[i] = position[i];
        state.stateFlags |= InteractionValid;
        gInteractionCaptured.fetch_add(1U, std::memory_order_relaxed);
    }
    state.interactionTransition = transition;
    state.interactionTransitionFlags = (firstFlag & 0xFFU) | ((secondFlag & 0xFFU) << 8U);
    actor->lastInteractionCapture = now;
    gPairedTransitionCaptured.fetch_add(1U, std::memory_order_relaxed);
    gLastInteractionTransition.store(transition, std::memory_order_relaxed);
    gLastInteractionTargetA.store(targetA, std::memory_order_relaxed);
    gLastInteractionTargetB.store(targetB, std::memory_order_relaxed);
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
    if (actor.character != 0U && Client()) {
        static_cast<void>(character_direct_step_runtime::Stop(actor.character));
        // A Client role/task can outlive the BT leaf that created it. Always
        // retire that executor through the retail cleanup before authority is
        // released, otherwise an old task can keep fighting future PoseSync.
        static_cast<void>(ClearRoleTaskExecutor(actor));
    }
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    actor.completedInteractionRevision = 0U;
    actor.completedActionRevision = 0U;
    actor.completedRoleTaskRevision = 0U;
    actor.completedLifecycleRevision = 0U;
    actor.lastInteractionAttempt = 0U;
    actor.interactionHoldUntil = 0U;
    actor.interactionActionRevision = 0U;
    actor.interactionActionPathHash = 0U;
    actor.interactionOwnsCycle = false;
    return true;
}

bool ApplyAuthoritativeInteraction(Actor& actor, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    if ((state.stateFlags & InteractionValid) == 0U ||
        actor.completedInteractionRevision == state.interactionRevision) return true;
    if (actor.lastInteractionAttempt != 0U && now - actor.lastInteractionAttempt < 250ULL) return true;
    actor.lastInteractionAttempt = now;
    auto target = FindCharacterByIdentity(state.interactionTargetA, state.interactionTargetB);
    if (target == 0U && (state.interactionFlags & InteractionTargetIsParticipant) != 0U) {
        target = FindParticipantNear(state.interactionTargetPosition, actor.character);
        if (target != 0U) gInteractionRemapped.fetch_add(1U, std::memory_order_relaxed);
    }
    std::uint32_t targetA{}, targetB{}, targetType{};
    if (target == 0U || target == actor.character ||
        !CharacterIdentity(target, targetA, targetB, targetType)) {
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
        return true; // retain the revision and retry after the bounded backoff
    }
    const auto actorIsSource = (state.interactionFlags & InteractionActorIsSource) != 0U;
    const auto sourceA = actorIsSource ? actor.entityA : targetA;
    const auto sourceB = actorIsSource ? actor.entityB : targetB;
    const auto destinationA = actorIsSource ? targetA : actor.entityA;
    const auto destinationB = actorIsSource ? targetB : actor.entityB;
    const auto payload = static_cast<std::uint64_t>(state.interactionPayloadLow) |
        (static_cast<std::uint64_t>(state.interactionPayloadHigh) << 32U);
    bool pairApplied = false;
    bool transitionApplied = false;
    __try {
        if ((state.interactionFlags & InteractionPairValid) != 0U) {
            std::uintptr_t managerRoot{};
            const auto original = reinterpret_cast<CharacterInteraction>(
                gInteractionOriginal.load(std::memory_order_acquire));
            if (original == nullptr ||
                !Read(gBase + kInteractionManagerGlobalRva, managerRoot) || managerRoot == 0U) {
                gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
                return false;
            }
            pairApplied = (original(reinterpret_cast<void*>(managerRoot + 0x90U),
                sourceA, sourceB, destinationA, destinationB, payload,
                state.interactionVariant) & 0xFFU) != 0U;
        }
        if (state.interactionTransition != 0U) {
            std::uintptr_t actionManager{};
            if (!Read(actor.character + 0x36CU, actionManager) || actionManager == 0U) {
                gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
                return false;
            }
            // Locomotion and paired actions share Character's native cycle
            // controller. Relinquish the follower before replaying the Host's
            // resolved transition so a Move/Idle refresh cannot replace it.
            static_cast<void>(character_direct_step_runtime::Stop(actor.character));
            const auto firstFlag = state.interactionTransitionFlags & 0xFFU;
            const auto secondFlag = (state.interactionTransitionFlags >> 8U) & 0xFFU;
            if (state.interactionTransition == 1U) {
                const auto transition = reinterpret_cast<PairedTransitionA>(
                    gPairedTransitionAOriginal.load(std::memory_order_acquire));
                if (transition == nullptr) return false;
                transition(reinterpret_cast<void*>(actionManager), target,
                    state.interactionVariant, firstFlag);
            } else {
                const auto transition = reinterpret_cast<PairedTransitionB>(
                    gPairedTransitionBOriginal.load(std::memory_order_acquire));
                if (transition == nullptr) return false;
                transition(reinterpret_cast<void*>(actionManager), target,
                    state.interactionVariant, firstFlag, secondFlag);
            }
            transitionApplied = true;
            gPairedTransitionApplied.fetch_add(1U, std::memory_order_relaxed);
            actor.interactionOwnsCycle = true;
            actor.interactionActionRevision = state.actionRevision;
            actor.interactionActionPathHash = state.actionPathHash;
            actor.interactionHoldUntil = now + kPairedTransitionMaximumHold;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFaults.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    // The event is revisioned and reaches each retail boundary only once. A
    // duplicate pair can still need the captured transition when it predates
    // AISync authority on this process.
    actor.completedInteractionRevision = state.interactionRevision;
    if (pairApplied || transitionApplied)
        gInteractionApplied.fetch_add(1U, std::memory_order_relaxed);
    else
        gInteractionRejected.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool PairedTransitionOwnsCycle(Actor& actor, const ULONGLONG now) noexcept {
    if (!actor.interactionOwnsCycle) return false;
    const auto& state = actor.desired;
    const bool expired = actor.interactionHoldUntil == 0U || now >= actor.interactionHoldUntil;
    const bool actionChanged = actor.interactionActionRevision != 0U &&
        (state.actionRevision != actor.interactionActionRevision ||
         state.actionPathHash != actor.interactionActionPathHash ||
         (state.stateFlags & ActionValid) == 0U);
    if (!expired && !actionChanged) return true;
    actor.interactionOwnsCycle = false;
    actor.interactionHoldUntil = 0U;
    actor.interactionActionRevision = 0U;
    actor.interactionActionPathHash = 0U;
    actor.lastProxyRefresh = 0U;
    actor.completedRevision = 0U;
    return false;
}

bool ApplyAuthoritativePose(Actor& actor, const ULONGLONG now) noexcept {
    const auto& state = actor.desired;
    if ((state.stateFlags & PoseValid) == 0U) return false;
    if (actor.completedRevision == state.revision && actor.lastProxyRefresh != 0U &&
        now - actor.lastProxyRefresh < 250ULL) return true;
    std::string error;
    const bool moving = (state.stateFlags & Moving) != 0U;
    // This supplies only spatial/locomotion intent. CharacterDirectStep enters
    // the retail Move/Idle cycle, which owns gait selection and animation; no
    // animation phase, clip or bone transform is transported by AISync.
    const bool applied = moving
        ? character_direct_step_runtime::ArmWorldReplication(actor.character,
            state.velocityX, state.velocityZ, 1.1F,
            state.position[0], state.position[1], state.position[2], state.yaw,
            state.moveType, 0xFFFFFFFFU, error)
        : character_direct_step_runtime::HoldWorldReplicationIdle(actor.character,
            state.position[0], state.position[1], state.position[2], state.yaw,
            state.moveType, error);
    if (!applied) {
        gPoseRejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    actor.completedRevision = state.revision;
    actor.lastProxyRefresh = now;
    gPoseApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}
bool SameState(const State& a, const State& b) noexcept {
    if (a.stateFlags != b.stateFlags || a.moveType != b.moveType ||
        a.actionTypeRva != b.actionTypeRva || a.actionPathHash != b.actionPathHash ||
        a.actionRevision != b.actionRevision || a.actionStatus != b.actionStatus ||
        a.actionDepth != b.actionDepth ||
        a.roleTaskRevision != b.roleTaskRevision || a.roleTaskTypeRva != b.roleTaskTypeRva ||
        a.lifecycleRevision != b.lifecycleRevision || a.lifecycleFlags != b.lifecycleFlags ||
        a.interactionRevision != b.interactionRevision ||
        a.interactionTargetA != b.interactionTargetA ||
        a.interactionTargetB != b.interactionTargetB ||
        a.interactionFlags != b.interactionFlags ||
        a.interactionTransition != b.interactionTransition ||
        a.interactionTransitionFlags != b.interactionTransitionFlags ||
        a.interactionPayloadLow != b.interactionPayloadLow ||
        a.interactionPayloadHigh != b.interactionPayloadHigh ||
        a.interactionVariant != b.interactionVariant) return false;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (std::abs(a.position[i] - b.position[i]) > 0.01F ||
            std::abs(a.interactionTargetPosition[i] - b.interactionTargetPosition[i]) > 0.01F)
            return false;
    return std::abs(a.yaw - b.yaw) <= 0.01F &&
        std::abs(a.velocityX - b.velocityX) <= 0.05F &&
        std::abs(a.velocityZ - b.velocityZ) <= 0.05F;
}
void Publish(Actor& actor) noexcept {
    const auto now = GetTickCount64();
    if (now - actor.lastSend < kSendPeriod) return;
    if ((actor.desired.stateFlags & InteractionValid) != 0U &&
        actor.lastInteractionCapture != 0U && now - actor.lastInteractionCapture > kTimeout) {
        actor.desired.stateFlags &= ~InteractionValid;
        actor.desired.interactionFlags = 0U;
        actor.desired.interactionTransition = 0U;
        actor.desired.interactionTransitionFlags = 0U;
    }
    CaptureWithdrawState(actor);
    CapturePose(actor, now);
    CaptureActiveAction(actor);
    CaptureRoleTask(actor);
    const bool changed = !SameState(actor.desired, actor.previous);
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
}

// BehaviourManager is not guaranteed to tick while an Alien is withdrawn,
// backstage or traversing a vent. AISync authority must therefore not depend on
// that manager callback for transport liveness. The CharacterManager pump is
// certified every frame and keeps the last admitted Host actor authoritative
// while its AI manager is dormant.
void PublishDormantHostActor(Actor& actor, const ULONGLONG now,
    const bool characterPresent) noexcept {
    if (now - actor.lastSend < kRefreshPeriod) return;

    if (characterPresent) CaptureWithdrawState(actor);
    if (!actor.hostDormant) {
        actor.hostDormant = true;
        MarkLifecycle(actor, LifecycleSpatialDiscontinuity |
            (actor.traversalAction ? LifecycleTraversalAction : 0U));
    } else {
        RefreshLifecycleFlags(actor, actor.desired.lifecycleFlags);
    }

    // If the same Character remains registered (usual backstage path), keep
    // its actual authoritative transform fresh even though BehaviourManager is
    // sleeping. If retail temporarily removes it from the active registry,
    // retain the last safe pose solely as an authority heartbeat.
    if (characterPresent) {
        CapturePose(actor, now);
    }
    actor.desired.stateFlags &= ~Moving;
    actor.desired.velocityX = 0.0F;
    actor.desired.velocityZ = 0.0F;

    const auto changed = !SameState(actor.desired, actor.previous);
    auto state = actor.desired;
    if (changed) ++state.revision;
    const auto write = gWrite.load(std::memory_order_relaxed);
    if (write - gRead.load(std::memory_order_acquire) >= kRing) {
        gDropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    actor.desired.revision = state.revision;
    actor.previous = state;
    actor.lastSend = now;
    gOutgoing[write % kRing] = state;
    gWrite.store(write + 1U, std::memory_order_release);
    gObserved.fetch_add(1U, std::memory_order_relaxed);
}

void PumpHostAuthority() noexcept {
    if (gRole.load(std::memory_order_acquire) != Role::Host || !Stable()) return;
    const auto now = GetTickCount64();

    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.duplicate || actor.faulted) continue;
        if (actor.lastManagerUpdate != 0U &&
            now - actor.lastManagerUpdate <= kManagerDormancyPeriod) continue;

        // Resolve through stable retail identity rather than trusting a stale
        // Character pointer. This also detects the rare case where a backstage
        // transition replaces the Character object while retaining the same
        // logical NPC identity.
        const auto current = FindCharacterByIdentity(actor.entityA, actor.entityB);
        if (current == actor.character) {
            PublishDormantHostActor(actor, now, true);
            continue;
        }
        if (current == 0U) {
            PublishDormantHostActor(actor, now, false);
            continue;
        }

        // Same logical NPC, new Character object. Re-admit it with a new birth
        // so Clients perform one lifecycle barrier/snap instead of remaining
        // bound to the stale pre-vent instance.
        const auto previousActor = actor;
        actor = {};
        auto* rebound = Admit(current);
        if (rebound == nullptr) {
            actor = previousActor;
            PublishDormantHostActor(actor, now, false);
            continue;
        }
        rebound->lastManagerUpdate = now;
        MarkLifecycle(*rebound, LifecycleSpatialDiscontinuity);
        Publish(*rebound);
    }
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
    if (!EngineThread()) { original(manager, previousTime, delta); return; }

    std::uintptr_t c{};
    if (!Read(reinterpret_cast<std::uintptr_t>(manager) + 0x128U, c) || c == 0U) {
        if (OwnsAI()) gOwnerMismatch.fetch_add(1U, std::memory_order_relaxed);
        original(manager, previousTime, delta); return;
    }

    // PlayerSync Characters are retail NPC-backed presentation proxies. They
    // must never run an autonomous retail brain on ANY peer: PlayerSync owns
    // their locomotion/equipment/actions, while the local BT would otherwise
    // choose targets/scripts independently. Keep Character housekeeping but
    // bypass the same decision slice used by AISync clients. This remains
    // active even when AISync traffic is isolated/off.
    if (PlayerRepresentation(c)) {
        Actor playerProxy{};
        playerProxy.character = c;
        playerProxy.blocked = true;
        auto* previous = gUpdating;
        gUpdating = &playerProxy;
        gSuspended.fetch_add(1U, std::memory_order_relaxed);
        original(manager, previousTime, delta);
        gUpdating = previous;
        return;
    }

    if (!OwnsAI()) { original(manager, previousTime, delta); return; }
    if (!Client() && !Stable()) { original(manager, previousTime, delta); return; }
    gManagerUpdates.fetch_add(1U, std::memory_order_relaxed);

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
        const auto now = GetTickCount64();
        if (actor->hostDormant) {
            actor->hostDormant = false;
            CaptureWithdrawState(*actor);
            MarkLifecycle(*actor, LifecycleSpatialDiscontinuity |
                (actor->traversalAction ? LifecycleTraversalAction : 0U));
            // Do not let the dormant heartbeat defer the first live snapshot.
            actor->lastSend = 0U;
        }
        actor->lastManagerUpdate = now;
        Publish(*actor);
    } else if (Client()) {
        const auto now = GetTickCount64();
        const bool withdrawn = ProxyWithdrawn(actor->desired);
        if (!Stable() || gRole.load(std::memory_order_acquire) == Role::FrozenClient ||
            actor->faulted || now - actor->lastReceive > kTimeout ||
            (!withdrawn && ((actor->desired.stateFlags & PoseValid) == 0U ||
                !CharacterReady(*actor)))) {
            static_cast<void>(Release(*actor));
        } else if (!ApplyAuthoritativeWithdraw(*actor, actor->desired)) {
            static_cast<void>(Release(*actor));
        } else if (withdrawn) {
            // Host has no frontstage AI to converge against. Keep the proxy
            // quiesced and let the retail Alien withdraw controller own its
            // visibility/backstage presentation until Host returns.
            static_cast<void>(character_direct_step_runtime::Stop(actor->character));
            actor->lastProxyRefresh = now;
        } else if (!ApplyAuthoritativeInteraction(*actor, now) ||
            (!PairedTransitionOwnsCycle(*actor, now) &&
                !ApplyAuthoritativePose(*actor, now))) {
            static_cast<void>(Release(*actor));
        }
    }
}
void __fastcall PrepassDetour(void* context, void*) noexcept {
    std::uintptr_t manager{}, owner{};
    std::uint32_t type{};
    // FUN_00733850 assigns (Character+34AC)->+4 to the embedded AI manager.
    // FUN_00712020 invokes task arbitration BEFORE the normal manager frame.
    // Player representations are presentation-only on Host and Clients, so
    // their task arbitration is always suppressed.
    if (EngineThread() && Read(reinterpret_cast<std::uintptr_t>(context) + 4U, manager) &&
        Read(manager + 0x128U, owner)) {
        if (PlayerRepresentation(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(gPrepassOriginal.load());
    if (original != nullptr) original(context);
}
void __fastcall CoordinatorDetour(void* tree, void*, std::uint32_t execute) noexcept {
    const auto original = reinterpret_cast<Coordinator>(gCoordinatorOriginal.load(std::memory_order_acquire));
    // CATHODE forced-target/relance also call this outside the ordinary AI frame.
    std::uintptr_t owner{};
    std::uint32_t type{};
    if (EngineThread() && Read(reinterpret_cast<std::uintptr_t>(tree) + 0xCU, owner)) {
        if (PlayerRepresentation(owner)) return;
        if (Client() && Eligible(owner, type)) return;
    }
    if (original != nullptr) original(tree, execute);
}

bool SuppressClientAIImpact(void* receiver) noexcept {
    if (!Client() || !EngineThread() || receiver == nullptr) return false;
    std::uintptr_t victim{};
    std::uint32_t type{};
    // Both certified Character impact receivers keep their owning Character at
    // +0x24. On a Client, AISync-owned NPC consequences are Host-authoritative:
    // local/remote weapon presentation may run, but it must not independently
    // damage or retarget the replicated AI.
    return Read(reinterpret_cast<std::uintptr_t>(receiver) + 0x24U, victim) &&
        victim != 0U && Eligible(victim, type);
}
void __fastcall CharacterImpactDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (SuppressClientAIImpact(receiver)) return;
    original(receiver, hit);
}
void __fastcall CharacterImpactSecondaryDetour(void* receiver, void*, int* hit) noexcept {
    const auto original = reinterpret_cast<CharacterImpact>(
        gCharacterImpactSecondaryOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (SuppressClientAIImpact(receiver)) return;
    original(receiver, hit);
}
std::uint32_t __fastcall InteractionDetour(void* manager, void*,
    const std::uint32_t sourceA, const std::uint32_t sourceB,
    const std::uint32_t targetA, const std::uint32_t targetB,
    const std::uint64_t payload, const std::int32_t variant) noexcept {
    const auto original = reinterpret_cast<CharacterInteraction>(
        gInteractionOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    const auto result = original(manager, sourceA, sourceB, targetA, targetB, payload, variant);
    if ((result & 0xFFU) != 0U && gRole.load(std::memory_order_acquire) == Role::Host &&
        EngineThread() && Stable()) {
        CaptureInteraction(sourceA, sourceB, targetA, targetB, payload, variant);
    }
    return result;
}
void __fastcall PairedTransitionADetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionA>(
        gPairedTransitionAOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    original(manager, target, variant, firstFlag);
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 1U, firstFlag, 0U);
}
void __fastcall PairedTransitionBDetour(void* manager, void*,
    const std::uintptr_t target, const std::int32_t variant,
    const std::uint32_t firstFlag, const std::uint32_t secondFlag) noexcept {
    const auto original = reinterpret_cast<PairedTransitionB>(
        gPairedTransitionBOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    original(manager, target, variant, firstFlag, secondFlag);
    if (gRole.load(std::memory_order_acquire) == Role::Host && EngineThread() && Stable())
        CapturePairedTransition(reinterpret_cast<std::uintptr_t>(manager), target,
            variant, 2U, firstFlag, secondFlag);
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
    if (base < 0x10000U || !hooks.MutationAllowed() ||
        !character_direct_step_runtime::Installed()) {
        error = "AISync V2 requires the certified retail x86 executable and the generic Character replication runtime.";
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
        Hook(hooks, "aisync_character_interaction", 0x00254820U,
            reinterpret_cast<void*>(&InteractionDetour),
            {0x83,0xEC,0x24,0x53,0x8B,0x5C,0x24,0x2C,0x56,0x8B,0xF1},
            &gInteractionTrampoline, &gInteractionOriginal, error) &&
        Hook(hooks, "aisync_paired_transition_a", 0x0025C4E0U,
            reinterpret_cast<void*>(&PairedTransitionADetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionATrampoline, &gPairedTransitionAOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_paired_transition_b", 0x0025BF60U,
            reinterpret_cast<void*>(&PairedTransitionBDetour),
            {0x6A,0xFF,0x68,0xC1,0x3F,0x12,0x01},
            &gPairedTransitionBTrampoline, &gPairedTransitionBOriginal, error,
            0x00D23FC1U) &&
        Hook(hooks, "aisync_character_impact", 0x003CDCF0U,
            reinterpret_cast<void*>(&CharacterImpactDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x64},
            &gCharacterImpactTrampoline, &gCharacterImpactOriginal, error) &&
        Hook(hooks, "aisync_character_impact_secondary", 0x003CE020U,
            reinterpret_cast<void*>(&CharacterImpactSecondaryDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x24},
            &gCharacterImpactSecondaryTrampoline, &gCharacterImpactSecondaryOriginal, error);
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
            actor = {};
        }
        gGeneration = generation;
        gIncomingUsed.fill(false);
    }
    // Host publication must survive AI-manager dormancy (vent/backstage).
    // CharacterManager invokes this pump independently of each NPC brain.
    PumpHostAuthority();

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

        const bool hadRemote = actor.remoteBirth != 0U;
        const bool birthChanged = value.birth != actor.remoteBirth;
        const bool actionChanged = hadRemote && value.actionRevision != actor.desired.actionRevision;
        const bool taskChanged = hadRemote && value.roleTaskRevision != actor.desired.roleTaskRevision;
        const bool lifecycleChanged = hadRemote && value.lifecycleRevision != actor.desired.lifecycleRevision;
        const bool poseMissing = (value.stateFlags & PoseValid) == 0U;

        // Action and role-task revisions are authority barriers. We do not
        // synthesize/copy BT objects from another process; instead retire any
        // stale local execution so only Host-resolved carriers and PoseSync can
        // drive the proxy. This is especially important for PERFORM_ROLE, whose
        // admitted task is a second decision layer outside normal BT selection.
        if (birthChanged || actionChanged || taskChanged || lifecycleChanged) {
            if (!QuiesceProxyExecution(actor)) {
                actor.faulted = true;
                gFaults.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (poseMissing) {
            static_cast<void>(Release(actor));
        }

        // Backstage/withdraw is a real Alien runtime state, not a pose. Mirror
        // the retail withdraw controller first. While Host is withdrawn/dormant
        // the Client must NOT keep forcing the last corridor transform: doing so
        // leaves a visible, brainless Alien fighting the backstage subsystem.
        if (!actor.faulted && (birthChanged || lifecycleChanged) &&
            !ApplyAuthoritativeWithdraw(actor, value)) {
            actor.faulted = true;
            gFaults.fetch_add(1U, std::memory_order_relaxed);
        }

        // On frontstage re-entry, leave withdraw first, then perform exactly one
        // authoritative placement. Normal PoseSync resumes only after this snap.
        if (!actor.faulted && !ProxyWithdrawn(value) &&
            (value.stateFlags & PoseValid) != 0U &&
            (birthChanged || lifecycleChanged)) {
            if (!SnapAuthoritativePose(actor, value)) {
                actor.faulted = true;
                gFaults.fetch_add(1U, std::memory_order_relaxed);
            } else {
                gLifecycleApplied.fetch_add(1U, std::memory_order_relaxed);
            }
        } else if (!actor.faulted && ProxyWithdrawn(value) &&
            (birthChanged || lifecycleChanged)) {
            gLifecycleApplied.fetch_add(1U, std::memory_order_relaxed);
        }

        actor.remoteBirth = value.birth; actor.remoteRevision = value.revision;
        actor.desired = value; actor.lastReceive = now;
        actor.completedActionRevision = value.actionRevision;
        actor.completedRoleTaskRevision = value.roleTaskRevision;
        actor.completedLifecycleRevision = value.lifecycleRevision;
        gApplied.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ObservePlayerRepresentationSpawn(const std::uint32_t spawnKey) noexcept {
    if (spawnKey == 0U || gGenerations == nullptr) return;
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_acquire) == spawnKey &&
            gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed) == generation) return;
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto value = gOwnedSpawnKeys[i].load(std::memory_order_relaxed);
        const auto valueGeneration = gOwnedSpawnKeyGeneration[i].load(std::memory_order_relaxed);
        if (value != 0U && valueGeneration == generation) continue;
        // Clear the publication before recycling a stale-generation slot.
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(generation, std::memory_order_relaxed);
        gOwnedSpawnKeys[i].store(spawnKey, std::memory_order_release);
        gPlayerRepresentationsRegistered.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
}
void ObservePlayerRepresentation(const std::uintptr_t c) noexcept {
    if (c == 0U || gGenerations == nullptr) return;
    std::uint32_t runtimeId{};
    if (Read(c + 0x50U, runtimeId)) ObservePlayerRepresentationSpawn(runtimeId);
    const auto generation = gGenerations->CurrentGeneration();
    for (std::size_t i = 0U; i < kActors; ++i) {
        auto& owned = gOwned[i];
        auto value = owned.load(std::memory_order_relaxed);
        if (value == c || ((value == 0U || gOwnedGeneration[i].load() != generation) &&
                owned.compare_exchange_strong(value, c))) {
            gOwnedGeneration[i].store(generation, std::memory_order_release);
            break;
        }
    }
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) {
        // Never stop CharacterDirectStep here: this slot is owned by PlayerSync.
        *actor = {};
        gProvisionalPlayerActorsRetired.fetch_add(1U, std::memory_order_relaxed);
    }
}
void ForgetCharacter(const std::uintptr_t c) noexcept {
    const bool playerRepresentation = RegisteredPlayerRepresentation(c);
    std::uint32_t runtimeId{};
    const bool hasRuntimeId = playerRepresentation && Read(c + 0x50U, runtimeId);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwned[i].load(std::memory_order_relaxed) != c) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (hasRuntimeId) {
        for (std::size_t i = 0U; i < kActors; ++i) {
            if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != runtimeId) continue;
            gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
            gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
        }
    }
    if (!EngineThread()) return;
    if (auto* actor = Find(c); actor != nullptr) {
        if (!playerRepresentation) static_cast<void>(Release(*actor));
        *actor = {};
    }
}
void ForgetSpawnKey(const std::uint32_t id) noexcept {
    if (id == 0U) return;
    const auto generation = gGenerations == nullptr ? 0U : gGenerations->CurrentGeneration();
    const bool playerRepresentation = RegisteredPlayerSpawnKey(id);
    for (std::size_t i = 0U; i < kActors; ++i) {
        if (gOwnedSpawnKeys[i].load(std::memory_order_relaxed) != id ||
            (generation != 0U && gOwnedSpawnKeyGeneration[i].load() != generation)) continue;
        gOwnedSpawnKeys[i].store(0U, std::memory_order_release);
        gOwnedSpawnKeyGeneration[i].store(0U, std::memory_order_relaxed);
    }
    for (std::size_t i = 0U; i < kActors; ++i) {
        const auto character = gOwned[i].load(std::memory_order_relaxed);
        std::uint32_t runtimeId{};
        if (character == 0U || !Read(character + 0x50U, runtimeId) || runtimeId != id) continue;
        gOwned[i].store(0U, std::memory_order_release);
        gOwnedGeneration[i].store(0U, std::memory_order_relaxed);
    }
    if (!EngineThread()) return;
    for (auto& actor : gActors) {
        if (actor.character == 0U || actor.runtimeId != id) continue;
        if (!playerRepresentation) static_cast<void>(Release(actor));
        actor = {};
    }
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
    Counters value{gObserved.load(), gApplied.load(), gDropped.load(), gRefused.load(),
        gSuspended.load(), gFaults.load(), gMissing.load()};
    value.managerUpdates = gManagerUpdates.load(); value.admitted = gAdmitted.load();
    value.identityRejected = gIdentityRejected.load(); value.treeUnavailable = gTreeUnavailable.load();
    value.ownerMismatch = gOwnerMismatch.load(); value.remoteQueued = gRemoteQueued.load();
    value.poseCaptured = gPoseCaptured.load(); value.poseApplied = gPoseApplied.load();
    value.poseRejected = gPoseRejected.load(); value.actionCaptured = gActionCaptured.load();
    value.actionTransitions = gActionTransitions.load();
    value.roleTaskCaptured = gRoleTaskCaptured.load();
    value.roleTaskTransitions = gRoleTaskTransitions.load();
    value.lifecycleCaptured = gLifecycleCaptured.load();
    value.lifecycleApplied = gLifecycleApplied.load();
    value.lifecycleSnaps = gLifecycleSnaps.load();
    value.executionBarriers = gExecutionBarriers.load();
    value.withdrawSamples = gWithdrawSamples.load();
    value.withdrawTransitions = gWithdrawTransitions.load();
    value.withdrawApplied = gWithdrawApplied.load();
    value.interactionCaptured = gInteractionCaptured.load();
    value.interactionApplied = gInteractionApplied.load();
    value.interactionRejected = gInteractionRejected.load();
    value.interactionRemapped = gInteractionRemapped.load();
    value.pairedTransitionCaptured = gPairedTransitionCaptured.load();
    value.pairedTransitionApplied = gPairedTransitionApplied.load();
    value.playerRepresentationsRegistered = gPlayerRepresentationsRegistered.load();
    value.provisionalPlayerActorsRetired = gProvisionalPlayerActorsRetired.load();
    value.role = static_cast<std::uint32_t>(gRole.load());
    value.nativeGeneration = gActiveNativeGeneration.load(); value.worldGeneration = gActiveWorldGeneration.load();
    value.lastEntityA = gLastEntityA.load(); value.lastEntityB = gLastEntityB.load();
    value.lastCharacterClass = gLastCharacterClass.load();
    value.lastActionTypeRva = gLastActionTypeRva.load();
    value.lastActionPathHash = gLastActionPathHash.load();
    value.lastActionStatus = gLastActionStatus.load();
    value.lastRoleTaskTypeRva = gLastRoleTaskTypeRva.load();
    value.lastRoleTaskKeyA = gLastRoleTaskKeyA.load();
    value.lastRoleTaskKeyB = gLastRoleTaskKeyB.load();
    value.lastLifecycleFlags = gLastLifecycleFlags.load();
    value.lastWithdrawState = gLastWithdrawState.load();
    value.lastInteractionTargetA = gLastInteractionTargetA.load();
    value.lastInteractionTargetB = gLastInteractionTargetB.load();
    value.lastInteractionTransition = gLastInteractionTransition.load();
    value.stable = gGenerations != nullptr && gGenerations->IsStableGeneration(value.nativeGeneration);
    return value;
}
void LogDiagnostics(Diagnostics& diagnostics) {
    const auto ai = DiagnosticsSnapshot();
    diagnostics.Log("info", "aisync_v2_snapshot", "AISync V2 high-level Character authority counters; animation/audio and downstream CATHODE consequences remain retail-local.",
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
         {"poseCaptured", Diagnostics::Unsigned(ai.poseCaptured), true},
         {"poseApplied", Diagnostics::Unsigned(ai.poseApplied), true},
         {"poseRejected", Diagnostics::Unsigned(ai.poseRejected), true},
         {"actionCaptured", Diagnostics::Unsigned(ai.actionCaptured), true},
         {"actionTransitions", Diagnostics::Unsigned(ai.actionTransitions), true},
         {"roleTaskCaptured", Diagnostics::Unsigned(ai.roleTaskCaptured), true},
         {"roleTaskTransitions", Diagnostics::Unsigned(ai.roleTaskTransitions), true},
         {"lifecycleCaptured", Diagnostics::Unsigned(ai.lifecycleCaptured), true},
         {"lifecycleApplied", Diagnostics::Unsigned(ai.lifecycleApplied), true},
         {"lifecycleSnaps", Diagnostics::Unsigned(ai.lifecycleSnaps), true},
         {"executionBarriers", Diagnostics::Unsigned(ai.executionBarriers), true},
         {"withdrawSamples", Diagnostics::Unsigned(ai.withdrawSamples), true},
         {"withdrawTransitions", Diagnostics::Unsigned(ai.withdrawTransitions), true},
         {"withdrawApplied", Diagnostics::Unsigned(ai.withdrawApplied), true},
         {"interactionCaptured", Diagnostics::Unsigned(ai.interactionCaptured), true},
         {"interactionApplied", Diagnostics::Unsigned(ai.interactionApplied), true},
         {"interactionRejected", Diagnostics::Unsigned(ai.interactionRejected), true},
         {"interactionRemapped", Diagnostics::Unsigned(ai.interactionRemapped), true},
         {"pairedTransitionCaptured", Diagnostics::Unsigned(ai.pairedTransitionCaptured), true},
         {"pairedTransitionApplied", Diagnostics::Unsigned(ai.pairedTransitionApplied), true},
         {"playerRepresentationsRegistered", Diagnostics::Unsigned(ai.playerRepresentationsRegistered), true},
         {"provisionalPlayerActorsRetired", Diagnostics::Unsigned(ai.provisionalPlayerActorsRetired), true},
         {"lastEntityA", Diagnostics::Unsigned(ai.lastEntityA), true},
         {"lastEntityB", Diagnostics::Unsigned(ai.lastEntityB), true},
         {"lastCharacterClass", Diagnostics::Unsigned(ai.lastCharacterClass), true},
         {"lastActionTypeRva", Diagnostics::Unsigned(ai.lastActionTypeRva), true},
         {"lastActionPathHash", Diagnostics::Unsigned(ai.lastActionPathHash), true},
         {"lastActionStatus", Diagnostics::Unsigned(ai.lastActionStatus), true},
         {"lastRoleTaskTypeRva", Diagnostics::Unsigned(ai.lastRoleTaskTypeRva), true},
         {"lastRoleTaskKeyA", Diagnostics::Unsigned(ai.lastRoleTaskKeyA), true},
         {"lastRoleTaskKeyB", Diagnostics::Unsigned(ai.lastRoleTaskKeyB), true},
         {"lastLifecycleFlags", Diagnostics::Unsigned(ai.lastLifecycleFlags), true},
         {"lastWithdrawState", Diagnostics::Unsigned(ai.lastWithdrawState), true},
         {"lastInteractionTargetA", Diagnostics::Unsigned(ai.lastInteractionTargetA), true},
         {"lastInteractionTargetB", Diagnostics::Unsigned(ai.lastInteractionTargetB), true},
         {"lastInteractionTransition", Diagnostics::Unsigned(ai.lastInteractionTransition), true},
         {"remoteQueued", Diagnostics::Unsigned(ai.remoteQueued), true},
         {"observed", Diagnostics::Unsigned(ai.observed), true},
         {"applied", Diagnostics::Unsigned(ai.applied), true},
         {"dropped", Diagnostics::Unsigned(ai.dropped), true},
         {"refused", Diagnostics::Unsigned(ai.refused), true},
         {"suspended", Diagnostics::Unsigned(ai.suspended), true},
         {"faults", Diagnostics::Unsigned(ai.faults), true},
         {"missingIdentity", Diagnostics::Unsigned(ai.missingIdentity), true}});
}
} // namespace aimp::ai_sync
