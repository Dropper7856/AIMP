#include "aimp/AISyncNavigation.h"
#include "aimp/CrashGuard.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/CharacterResourceAdmission.h"
#include "aimp/HookManager.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace aimp::ai_sync::navigation {
namespace {
constexpr std::uintptr_t kScheduler = 0x2CD0U, kRequestType = 0x00EB1A70U;
constexpr std::uintptr_t kDirectedType = 0x00EB19ACU, kCreateDirected = 0x002823B0U;
constexpr std::uintptr_t kTurnType = 0x00EB19CCU, kCreateTurn = 0x00282450U;
constexpr std::uintptr_t kStanceType = 0x00EB19BCU, kCreateStance = 0x002823F0U;
// Kind 2 owns a local waypoint/path manager. Only its RESOLVED world goal is
// captured; Clients receive kind 1 and never reconstruct or copy the manager.
constexpr std::uint32_t kResolvedPathKind = 2U;
constexpr std::uintptr_t kResolvedPathType = 0x00EB1A60U;
constexpr std::uintptr_t kCreate = 0x00283BF0U, kSetGoal = 0x002777B0U;
constexpr std::uintptr_t kSubmit = 0x00283CE0U, kCancel = 0x00283DB0U;
constexpr std::uintptr_t kUpdateGoal = 0x002C7550U;
constexpr std::uintptr_t kReleaseRef = 0x0000D770U;
struct Ref final { std::uintptr_t object{}, control{}; };
struct Lease final {
    Ref ref{};
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, revision{}, requestRevision{}, nativeKind{};
    NavigationGoalState appliedGoal{};
    std::uint64_t expires{};
    bool finished{}, failed{}, convergence{};
};
std::array<Lease, kNavigationCapacity> gLeases{};
std::uintptr_t gBase{};
std::atomic<std::uint64_t> gCaptured{}, gSubmitted{}, gUpdated{}, gCompleted{};
std::atomic<std::uint64_t> gFailed{}, gCancelled{}, gExpired{};
std::atomic<std::uint64_t> gGaitApplied{};
std::atomic<std::uint64_t> gDirectedCaptured{}, gEndpointsCaptured{}, gDirectedSubmitted{};
std::atomic<std::uint64_t> gTurnCaptured{}, gStanceCaptured{};
std::atomic<std::uint64_t> gPathGoalsCaptured{};
constexpr std::size_t kSubmissionCapacity = 512U;
std::array<SubmissionObservation, kSubmissionCapacity> gSubmissions{};
std::atomic<std::uint64_t> gSubmissionRead{}, gSubmissionWrite{}, gSubmissionSequence{};
std::atomic<std::uint64_t> gSubmissionEpoch{1U}, gSubmissionsCaptured{}, gSubmissionsDropped{};
std::atomic<SubmissionGuard> gSubmissionGuard{};
struct GoalHint final {
    std::uintptr_t character{}, action{}, actionType{}, request{};
    std::uint32_t runtimeId{};
    std::uint64_t capturedAt{};
};
std::array<GoalHint, kNavigationCapacity> gGoalHints{};
SRWLOCK gGoalHintsLock = SRWLOCK_INIT;
std::atomic<void*> gDirectedStartOriginal{}, gDirectedUpdateOriginal{};
void* gDirectedStartTrampoline{};
void* gDirectedUpdateTrampoline{};
template<typename T> bool Read(const std::uintptr_t address, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
constexpr std::uintptr_t TypeRva(const std::uint32_t kind) noexcept {
    switch (kind) {
    case kPointRequest: return kRequestType;
    case kResolvedPathKind: return kResolvedPathType;
    case kTurnRequest: return kTurnType;
    case kStanceRequest: return kStanceType;
    case kDirectedRequest: return kDirectedType;
    default: return 0U;
    }
}
constexpr std::uintptr_t FactoryRva(const std::uint32_t kind) noexcept {
    switch (kind) {
    case kPointRequest: return kCreate;
    case kTurnRequest: return kCreateTurn;
    case kStanceRequest: return kCreateStance;
    case kDirectedRequest: return kCreateDirected;
    default: return 0U;
    }
}
void SetTurnValues(const std::uintptr_t request, const NavigationGoalState& goal) noexcept {
    const bool point = (goal.flags & kWorldEndpoint) != 0U;
    *reinterpret_cast<std::uint32_t*>(request + 0x30U) = point ? 0U : 1U;
    if (point) {
        for (std::size_t i = 0U; i < 3U; ++i)
            *reinterpret_cast<float*>(request + 0x1CU + i * sizeof(float)) = goal.position[i];
    } else *reinterpret_cast<float*>(request + 0x28U) = goal.heading;
    *reinterpret_cast<float*>(request + 0x2CU) = goal.turnTolerance;
    // Both are scalar producer choices, not addresses or locomotion gait.
    *reinterpret_cast<std::uint32_t*>(request + 0x34U) = goal.moveType & 1U;
    *reinterpret_cast<std::uint8_t*>(request + 0x38U) = static_cast<std::uint8_t>((goal.moveType >> 1U) & 1U);
}
bool Match(const std::uintptr_t rva, const std::initializer_list<unsigned char> bytes) noexcept {
    for (std::size_t i = 0U; const auto expected : bytes) {
        unsigned char actual{};
        if (!Read(gBase + rva + i++, actual) || actual != expected) return false;
    }
    return true;
}
bool ReadPoint(const std::uintptr_t address, float (&point)[3]) noexcept {
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!Read(address + i * sizeof(float), point[i]) || !std::isfinite(point[i]) ||
            std::abs(point[i]) > 100000.0F) return false;
    return true;
}
bool CaptureShortGoal(const std::uintptr_t request, const std::uint32_t kind,
    NavigationGoalState& goal) noexcept {
    goal = {};
    goal.flags = kLiveGoal; goal.kind = kind;
    goal.revision = goal.requestRevision = 1U; // the Actor assigns portable epochs
    if (kind == kTurnRequest) {
        std::uint32_t mode{}, style{};
        std::uint8_t aim{};
        if (!Read(request + 0x30U, mode) || mode > 1U ||
            !Read(request + 0x2CU, goal.turnTolerance) ||
            !Read(request + 0x34U, style) || style > 1U ||
            !Read(request + 0x38U, aim) || aim > 1U) return false;
        goal.moveType = style | (static_cast<std::uint32_t>(aim) << 1U);
        if (mode == 1U) {
            if (!Read(request + 0x28U, goal.heading)) return false;
        } else {
            if (!ReadPoint(request + 0x1CU, goal.position)) return false;
            goal.flags |= kWorldEndpoint;
        }
    } else if (kind == kStanceRequest) {
        if (!Read(request + 0x1CU, goal.moveType)) return false;
    } else return false;
    return ValidGoal(goal);
}
void ObserveDirectedGoal(const std::uintptr_t action) noexcept {
    GoalHint hint{};
    std::uintptr_t scheduler{}, type{};
    std::uint32_t kind{}, status{};
    std::uint8_t retained{};
    float point[3]{};
    if (!Read(action, hint.actionType) || hint.actionType < gBase ||
        !Read(action + 0x24U, hint.character) || hint.character == 0U ||
        !Read(hint.character + 0x50U, hint.runtimeId) ||
        !Read(action + 0x70U, hint.request) || hint.request == 0U ||
        !Read(hint.request, type) || type != gBase + kDirectedType ||
        !Read(hint.request + 0x10U, kind) || kind != kDirectedRequest ||
        !Read(hint.request + 0x14U, scheduler) || scheduler != hint.character + kScheduler ||
        !Read(hint.request + 0xCU, status) || (status != 1U && status != 2U) ||
        !Read(hint.request + 0x18U, retained) || retained == 0U ||
        !ReadPoint(action + 0x50U, point)) return;
    hint.action = action;
    hint.capturedAt = GetTickCount64();
    if (!TryAcquireSRWLockExclusive(&gGoalHintsLock)) return;
    auto* selected = &gGoalHints.front();
    for (auto& entry : gGoalHints) {
        if (entry.character == hint.character || entry.character == 0U) { selected = &entry; break; }
        if (entry.capturedAt < selected->capturedAt) selected = &entry;
    }
    *selected = hint;
    ReleaseSRWLockExclusive(&gGoalHintsLock);
}
bool CaptureDirectedEndpoint(const std::uintptr_t character, const std::uintptr_t request,
    float (&point)[3]) noexcept {
    GoalHint hint{};
    if (!TryAcquireSRWLockShared(&gGoalHintsLock)) return false;
    for (const auto& entry : gGoalHints)
        if (entry.character == character && entry.request == request) { hint = entry; break; }
    ReleaseSRWLockShared(&gGoalHintsLock);
    std::uintptr_t currentType{}, owner{}, currentRequest{};
    std::uint32_t runtimeId{};
    // Both the Character incarnation and the producer's current Ref must still
    // match. No stale action pointer is accepted solely because an allocator
    // recycled the address of a kind-9 request.
    return hint.action != 0U && Read(character + 0x50U, runtimeId) && runtimeId == hint.runtimeId &&
        Read(hint.action, currentType) && currentType == hint.actionType &&
        Read(hint.action + 0x24U, owner) && owner == character &&
        Read(hint.action + 0x70U, currentRequest) && currentRequest == request &&
        ReadPoint(hint.action + 0x50U, point);
}
using DirectedProducer = void(__thiscall*)(void*);
void __fastcall DirectedStartDetour(void* action, void*) noexcept {
    const auto original = reinterpret_cast<DirectedProducer>(gDirectedStartOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    original(action);
    ObserveDirectedGoal(reinterpret_cast<std::uintptr_t>(action));
}
void __fastcall DirectedUpdateDetour(void* action, void*) noexcept {
    const auto original = reinterpret_cast<DirectedProducer>(gDirectedUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    original(action);
    ObserveDirectedGoal(reinterpret_cast<std::uintptr_t>(action));
}
bool Live(const Lease& lease) noexcept {
    std::uint32_t id{};
    std::uintptr_t owner{}, scheduler{};
    return lease.character != 0U && Read(lease.character + 0x50U, id) && id == lease.runtimeId &&
        Read(lease.character + kScheduler, scheduler) && scheduler == lease.character &&
        lease.ref.object != 0U && Read(lease.ref.object + 0x14U, owner) &&
        owner == lease.character + kScheduler;
}
void Release(Lease& lease, const bool cancel) noexcept {
    __try {
        // A terminal request still has a scheduler reference while +18 is set.
        // Retail Cancel clears that bit and removes both active and terminal
        // entries. Releasing only our Ref leaks completed goals in the scheduler.
        if (lease.ref.object != 0U && cancel && Live(lease)) {
            reinterpret_cast<void(__thiscall*)(void*)>(gBase + kCancel)(
                reinterpret_cast<void*>(lease.ref.object));
            gCancelled.fetch_add(1U, std::memory_order_relaxed);
        }
        if (lease.ref.control != 0U)
            reinterpret_cast<void(__thiscall*)(Ref*)>(gBase + kReleaseRef)(&lease.ref);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        gFailed.fetch_add(1U, std::memory_order_relaxed);
    }
    lease = {};
}
Lease* Find(const std::uintptr_t character) noexcept {
    for (auto& lease : gLeases) if (lease.character == character) return &lease;
    return nullptr;
}
} // namespace

bool Initialize(const std::uintptr_t base, std::string& error) {
    gBase = base;
    // Complete machine-code boundaries; all these entries use ECX=this.
    // Create has one output Ref argument and RET 4. SetGoal has RET 4;
    // Submit/Cancel have no stack arguments. Never trust Ghidra's hidden args.
    std::uintptr_t directedStatus{}, directedTick{}, turnStatus{}, stanceStatus{}, pathStatus{};
    if (!Match(kCreate, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreate + 0x18U, {0xC2,0x04,0x00}) ||
        !Match(kCreateDirected, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreateDirected + 0x18U, {0xC2,0x04,0x00}) ||
        !Match(kCreateTurn, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreateTurn + 0x18U, {0xC2,0x04,0x00}) ||
        !Match(kCreateStance, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreateStance + 0x18U, {0xC2,0x04,0x00}) ||
        !Read(gBase + kTurnType + 8U, turnStatus) || turnStatus != gBase + 0x002777E0U ||
        !Read(gBase + kStanceType + 8U, stanceStatus) || stanceStatus != gBase + 0x002777E0U ||
        !Read(gBase + kResolvedPathType + 8U, pathStatus) || pathStatus != gBase + 0x00282560U ||
        !Match(0x0028186FU, {0x8D,0x77,0x1C}) ||
        !Match(0x0028187BU, {0xE8,0x00,0xE8,0xDB,0xFF}) ||
        !Match(0x00279A50U, {0x8B,0x41,0x30,0x83,0xEC,0x0C,0x48,0x74,0x53}) ||
        !Match(0x0027ED01U, {0xC7,0x42,0x10,0x04,0x00,0x00,0x00}) ||
        !Match(0x0027EC0BU, {0xC7,0x42,0x10,0x07,0x00,0x00,0x00}) ||
        !Read(gBase + kDirectedType + 8U, directedStatus) || directedStatus != gBase + 0x002815A0U ||
        !Read(gBase + kDirectedType + 12U, directedTick) || directedTick != gBase + 0x002813A0U ||
        !Match(0x002813A0U, {0x83,0xEC,0x08,0x53,0x55,0x56,0x8B,0xF1,0x80,0x7E,0x34,0x00}) ||
        !Match(0x002815A0U, {0x8B,0x44,0x24,0x0C,0x83,0xEC,0x10,0x83,0xE8,0x02}) ||
        !Match(kSetGoal, {0x8B,0x44,0x24,0x04}) ||
        !Match(kSubmit, {0x64,0xA1,0x00,0x00,0x00,0x00,0x6A,0xFF}) ||
        !Match(kCancel, {0x64,0xA1,0x00,0x00,0x00,0x00,0x6A,0xFF}) ||
        !Match(kUpdateGoal, {0x83,0x79,0x68,0x03}) ||
        !Match(kReleaseRef, {0x56,0x8B,0x71,0x04}) ||
        !Match(0x00262661U, {0xC2,0x04,0x00}) ||
        !Match(0x002612B1U, {0xC6,0x46,0x1A,0x3B}) ||
        !Match(0x00284C28U, {0x89,0xB8,0x94,0x01,0x00,0x00,0xC7,0x80,0x90,0x01,0x00,0x00,0x00,0x00,0x00,0x00}) ||
        !Match(0x00284041U, {0x89,0x83,0x94,0x01,0x00,0x00,0xC7,0x83,0x90,0x01,0x00,0x00,0x00,0x00,0x00,0x00})) {
        error = "AISync native navigation ABI mismatch";
        gBase = 0U;
        return false;
    }
    return true;
}
bool InstallGoalObservers(HookManager& hooks, std::string& error) {
    if (gBase == 0U) { error = "AISync navigation is uninitialized"; return false; }
    HookRequest start;
    start.name = "aisync_directed_world_goal_start";
    start.target = reinterpret_cast<void*>(gBase + 0x00286570U);
    start.detour = reinterpret_cast<void*>(&DirectedStartDetour);
    start.expected.bytes = {0x6AU,0xFFU,0x68U,0U,0U,0U,0U};
    const auto handler = static_cast<std::uint32_t>(gBase + 0x00D27148U);
    std::memcpy(start.expected.bytes.data() + 3U, &handler, sizeof(handler));
    start.expected.mask.assign(start.expected.bytes.size(), 'x');
    start.patchLength = start.expected.bytes.size(); start.relocationSafePrefix = true;
    start.trampolineOut = &gDirectedStartTrampoline;
    start.publishedTrampolineOut = &gDirectedStartOriginal;
    if (!hooks.Install(start, error)) {
        error = start.name + ": " + error;
        return false;
    }
    HookRequest update;
    update.name = "aisync_directed_world_goal_update";
    update.target = reinterpret_cast<void*>(gBase + 0x00286860U);
    update.detour = reinterpret_cast<void*>(&DirectedUpdateDetour);
    update.expected.bytes = {0x55U,0x8BU,0xECU,0x83U,0xE4U,0xF0U};
    update.expected.mask.assign(update.expected.bytes.size(), 'x');
    update.patchLength = update.expected.bytes.size(); update.relocationSafePrefix = true;
    update.trampolineOut = &gDirectedUpdateTrampoline;
    update.publishedTrampolineOut = &gDirectedUpdateOriginal;
    if (hooks.Install(update, error)) return true;
    error = update.name + ": " + error;
    return false;
}
void SetSubmissionGuard(const SubmissionGuard guard) noexcept {
    const auto previous = gSubmissionGuard.exchange(guard, std::memory_order_acq_rel);
    if (previous != guard) ResetSubmissionObservations();
}
void ResetSubmissionObservations() noexcept {
    if (gSubmissionEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U == 0U)
        gSubmissionEpoch.fetch_add(1U, std::memory_order_acq_rel);
}
void ObserveSubmission(const std::uintptr_t request, const std::uint32_t requestRevision) noexcept {
    const auto guard = gSubmissionGuard.load(std::memory_order_acquire);
    const auto epoch = gSubmissionEpoch.load(std::memory_order_acquire);
    std::uintptr_t type{}, scheduler{}, character{}, schedulerOwner{};
    std::uint32_t kind{}, status{};
    SubmissionObservation observation{};
    if (gBase == 0U || guard == nullptr || request == 0U || requestRevision == 0U ||
        !Read(request + 0x10U, kind) || (kind != kTurnRequest && kind != kStanceRequest) ||
        !Read(request, type) || type != gBase + TypeRva(kind) ||
        !Read(request + 0x14U, scheduler) || scheduler == 0U ||
        !Read(scheduler, character) || character == 0U || scheduler != character + kScheduler ||
        !Read(character + kScheduler, schedulerOwner) || schedulerOwner != character ||
        !guard(character) || !Read(character + 0x50U, observation.runtimeId) ||
        !Read(request + 0xCU, status) || (status != 1U && status != 2U && status != 3U) ||
        !CaptureShortGoal(request, kind, observation.goal)) return;
    observation.character = character;
    observation.goal.requestRevision = requestRevision;
    observation.epoch = epoch;
    observation.sequence = gSubmissionSequence.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (epoch != gSubmissionEpoch.load(std::memory_order_acquire) || !guard(character)) return;
    // Guarded engine-thread producer, engine-thread consumer. Atomic cursors
    // publish complete value payloads; no allocator/native Ref is involved.
    const auto write = gSubmissionWrite.load(std::memory_order_relaxed);
    if (write - gSubmissionRead.load(std::memory_order_acquire) >= kSubmissionCapacity) {
        gSubmissionsDropped.fetch_add(1U, std::memory_order_relaxed); return;
    }
    gSubmissions[write % kSubmissionCapacity] = observation;
    gSubmissionWrite.store(write + 1U, std::memory_order_release);
    gSubmissionsCaptured.fetch_add(1U, std::memory_order_relaxed);
}
bool TryPopSubmission(SubmissionObservation& observation) noexcept {
    auto read = gSubmissionRead.load(std::memory_order_relaxed);
    const auto write = gSubmissionWrite.load(std::memory_order_acquire);
    while (read != write) {
        const auto candidate = gSubmissions[read % kSubmissionCapacity];
        gSubmissionRead.store(++read, std::memory_order_release);
        if (candidate.epoch != gSubmissionEpoch.load(std::memory_order_acquire)) continue;
        observation = candidate;
        return true;
    }
    observation = {};
    return false;
}
Sample Capture(const std::uintptr_t character) noexcept {
    Sample sample{};
    std::uintptr_t owner{}, sentinel{}, node{}, request{}, type{}, schedulerOwner{};
    std::uint32_t status{}, kind{}, count{};
    std::uint8_t active{}, flags{};
    const auto scheduler = character + kScheduler;
    if (gBase == 0U || !Read(scheduler, schedulerOwner) || schedulerOwner != character ||
        !Read(scheduler + 4U, sentinel) || !Read(scheduler + 8U, count) ||
        count == 0U || count > 64U || sentinel == 0U || !Read(sentinel, node) || node == sentinel ||
        !Read(node + 8U, request) || request == 0U ||
        !Read(request, type) || !Read(request + 0x10U, kind) ||
        TypeRva(kind) == 0U || type != gBase + TypeRva(kind) ||
        !Read(request + 0x14U, owner) || owner != scheduler ||
        !Read(request + 0xCU, status) || (status != 1U && status != 2U) ||
        !Read(request + 0x18U, active) || active == 0U) return sample;
    sample.goal.kind = kind;
    sample.goal.flags = kLiveGoal;
    if (kind == kPointRequest || kind == kResolvedPathKind) {
        const bool path = kind == kResolvedPathKind;
        std::uint32_t pathState{};
        if (path && (!Read(request + 0x5CU, pathState) || pathState != 3U)) return {};
        if (!Read(request + (path ? 0xACU : 0xB8U), flags)) return {};
        // +1C is only the original MoveRequest convenience cache. The native
        // pending-goal consumer 0044D320 updates the navigation block at +28,
        // and never rewrites that cache after accepting a dynamic target.
        const auto blockOffset = path ? 0x1CU : 0x28U;
        const auto goalOffset = blockOffset + ((flags & 0x40U) != 0U ? 0xCU : 0U);
        if (!ReadPoint(request + goalOffset, sample.goal.position) ||
            !Read(request + blockOffset + 0x2CU, sample.goal.stoppingDistance) ||
            !Read(request + blockOffset + 0x50U, sample.goal.moveType)) return {};
        sample.goal.kind = kPointRequest;
        if (path) gPathGoalsCaptured.fetch_add(1U, std::memory_order_relaxed);
    } else if (kind == kTurnRequest || kind == kStanceRequest) {
        if (!CaptureShortGoal(request, kind, sample.goal)) return {};
        if (kind == kTurnRequest) gTurnCaptured.fetch_add(1U, std::memory_order_relaxed);
        else gStanceCaptured.fetch_add(1U, std::memory_order_relaxed);
    } else {
        std::uint8_t rebase{};
        if (!Read(request + 0x1CU, sample.goal.heading) ||
            !Read(request + 0x20U, sample.goal.turnTolerance) ||
            !Read(request + 0x24U, sample.goal.distance) || !Read(request + 0x34U, rebase) ||
            !ReadPoint(status != 2U || rebase != 0U ? character + 0x70U : request + 0x28U,
                sample.goal.origin)) return {};
        float endpoint[3]{};
        if (CaptureDirectedEndpoint(character, request, endpoint)) {
            for (std::size_t i = 0U; i < 3U; ++i) sample.goal.position[i] = endpoint[i];
            sample.goal.flags |= kWorldEndpoint;
            sample.goal.stoppingDistance = 0.1F;
            sample.goal.moveType = 2U; // retail MoveRequest constructor default, not the gait
            gEndpointsCaptured.fetch_add(1U, std::memory_order_relaxed);
        }
        gDirectedCaptured.fetch_add(1U, std::memory_order_relaxed);
    }
    // Capture assigns the portable revision in AISync, not the native object.
    sample.goal.revision = 1U;
    sample.goal.requestRevision = 1U;
    if (!ValidGoal(sample.goal)) return {};
    sample.request = request;
    gCaptured.fetch_add(1U, std::memory_order_relaxed);
    return sample;
}
HeadObservation InspectHead(const std::uintptr_t character) noexcept {
    HeadObservation result{};
    std::uintptr_t owner{}, sentinel{}, node{}, request{}, table{};
    const auto scheduler = character + kScheduler;
    if (!Read(scheduler, owner) || owner != character ||
        !Read(scheduler + 8U, result.count) || result.count > 64U) return result;
    if (result.count == 0U) { result.readable = true; return result; }
    if (!Read(scheduler + 4U, sentinel) || sentinel == 0U ||
        !Read(sentinel, node) || node == sentinel || !Read(node + 8U, request) || request == 0U ||
        !Read(request, table) || table < gBase ||
        !Read(request + 0x10U, result.kind) || !Read(request + 0xCU, result.status)) return result;
    result.typeRva = static_cast<std::uint32_t>(table - gBase);
    result.readable = true;
    return result;
}
static bool ApplyPinned(const std::uintptr_t character, const NavigationGoalState& goal,
    const std::uint64_t expiresAt, std::string& error, const bool convergence) noexcept {
    if (gBase == 0U || (goal.flags & kLiveGoal) == 0U || !ValidGoal(goal)) return false;
    // A captured world endpoint is a decision above steering, even when the
    // Host producer chose kind 9. Let the LOCAL retail path request reach it.
    // A genuine direction-only command retains its own native kind-9 executor.
    const auto nativeKind = LocalRequestKind(goal);
    auto* lease = Find(character);
    if (lease != nullptr && (lease->convergence != convergence || lease->nativeKind != nativeKind)) {
        Release(*lease, true);
        lease = nullptr;
    }
    if (lease != nullptr && !Live(*lease)) {
        Release(*lease, false);
        lease = nullptr;
    }
    if (lease != nullptr && lease->requestRevision == goal.requestRevision) {
        lease->expires = expiresAt;
        const bool sameCommand = SameNativeCommand(lease->appliedGoal, goal);
        if (lease->finished && lease->failed) {
            // A terminal retail request cannot be revived by metadata or a
            // changed target on the same Host admission. Only a new portable
            // requestRevision may grant it ownership again.
            lease->revision = goal.revision;
            lease->appliedGoal = goal;
            return false;
        }
        if (lease->finished && (lease->revision == goal.revision || sameCommand)) {
            lease->revision = goal.revision;
            lease->appliedGoal = goal;
            return !lease->failed; // failure cannot own movement
        }
        std::uint32_t status{};
        if (!Live(*lease) || !Read(lease->ref.object + 0xCU, status)) return false;
        if (status == 3U && !lease->finished) {
            lease->finished = true;
            gCompleted.fetch_add(1U, std::memory_order_relaxed);
            if (lease->revision == goal.revision || sameCommand) {
                lease->revision = goal.revision;
                lease->appliedGoal = goal;
                return true;
            }
        }
        if (status == 4U || status == 5U || status == 6U) {
            error = "Retail AISync navigation request failed or was interrupted";
            lease->finished = true;
            lease->failed = true;
            lease->revision = goal.revision;
            lease->appliedGoal = goal;
            gFailed.fetch_add(1U, std::memory_order_relaxed);
            return false; // caller resumes Host steering, without releasing the actor
        }
        if (!lease->finished && sameCommand) {
            // Snapshot metadata may advance after an ordered one-shot, or the
            // Host may publish a new directed origin for the same world goal.
            // Acknowledging it must not execute a posture twice or restart a
            // local route that already reaches this exact native command.
            lease->revision = goal.revision;
            lease->appliedGoal = goal;
            return true;
        }
        if (!lease->finished && lease->revision != goal.revision) {
            __try {
                const auto request = lease->ref.object;
                if (nativeKind == kDirectedRequest) {
                    // Exactly the value writes in 00686860. +34 asks 006813A0
                    // to rebase at the LOCAL Character position. The Host's
                    // origin is provenance, never a Client transform/origin write.
                    *reinterpret_cast<float*>(request + 0x1CU) = goal.heading;
                    *reinterpret_cast<float*>(request + 0x20U) = goal.turnTolerance;
                    *reinterpret_cast<float*>(request + 0x24U) = goal.distance;
                    *reinterpret_cast<std::uint8_t*>(request + 0x34U) = 1U;
                    lease->revision = goal.revision;
                    gUpdated.fetch_add(1U, std::memory_order_relaxed);
                } else if (nativeKind == kTurnRequest) {
                    SetTurnValues(request, goal);
                    lease->revision = goal.revision;
                    gUpdated.fetch_add(1U, std::memory_order_relaxed);
                } else if (nativeKind == kStanceRequest) {
                    *reinterpret_cast<std::uint32_t*>(request + 0x1CU) = goal.moveType;
                    lease->revision = goal.revision;
                    gUpdated.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    reinterpret_cast<void(__thiscall*)(void*, const float*)>(gBase + kUpdateGoal)(
                        reinterpret_cast<void*>(request), goal.position);
                    *reinterpret_cast<float*>(request + 0x54U) = goal.stoppingDistance;
                    *reinterpret_cast<std::uint32_t*>(request + 0x78U) = goal.moveType;
                    // The native setter queues one target while a path is active.
                    // Retry on the next engine tick if its pending slot was busy.
                    std::uint8_t pending{};
                    if (Read(request + 0xB8U, pending) &&
                        std::memcmp(reinterpret_cast<const void*>(request +
                            ((pending & 0x40U) != 0U ? 0x34U : 0x28U)), goal.position, 12U) == 0) {
                        lease->revision = goal.revision;
                        gUpdated.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
                if (lease->revision == goal.revision) lease->appliedGoal = goal;
            } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        if (!lease->finished) return true;
    }
    if (lease != nullptr) Release(*lease, true);
    else for (auto& entry : gLeases) if (entry.character == 0U) { lease = &entry; break; }
    if (lease == nullptr) { error = "AISync navigation capacity exhausted"; return false; }
    __try {
        std::uintptr_t schedulerOwner{};
        if (!Read(character + kScheduler, schedulerOwner) || schedulerOwner != character ||
            !Read(character + 0x50U, lease->runtimeId)) return false;
        lease->character = character; lease->revision = goal.revision; lease->expires = expiresAt;
        lease->convergence = convergence;
        lease->nativeKind = nativeKind;
        lease->requestRevision = goal.requestRevision;
        lease->appliedGoal = goal;
        reinterpret_cast<Ref*(__thiscall*)(void*, Ref*)>(gBase + FactoryRva(nativeKind))(
            reinterpret_cast<void*>(character + kScheduler), &lease->ref);
        if (lease->ref.object == 0U || lease->ref.control == 0U) {
            Release(*lease, false); return false;
        }
        const auto request = lease->ref.object;
        std::uintptr_t type{};
        if (!Read(request, type) || type != gBase + TypeRva(nativeKind)) {
            Release(*lease, false); return false;
        }
        if (nativeKind == kPointRequest) {
            reinterpret_cast<void(__thiscall*)(void*, const float*)>(gBase + kSetGoal)(
                reinterpret_cast<void*>(request), goal.position);
            // +78 is a MoveRequest parameter. It is NOT requested gait +194.
            *reinterpret_cast<float*>(request + 0x54U) = goal.stoppingDistance;
            *reinterpret_cast<std::uint32_t*>(request + 0x78U) = goal.moveType;
            *reinterpret_cast<std::uint8_t*>(request + 0xB8U) &= ~0x10U;
        } else if (nativeKind == kTurnRequest) {
            SetTurnValues(request, goal);
        } else if (nativeKind == kStanceRequest) {
            *reinterpret_cast<std::uint32_t*>(request + 0x1CU) = goal.moveType;
        } else {
            *reinterpret_cast<float*>(request + 0x1CU) = goal.heading;
            *reinterpret_cast<float*>(request + 0x20U) = goal.turnTolerance;
            *reinterpret_cast<float*>(request + 0x24U) = goal.distance;
            *reinterpret_cast<std::uint8_t*>(request + 0x34U) = 1U;
        }
        *reinterpret_cast<std::uint8_t*>(request + 0x18U) = 1U;
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(character));
        const auto accepted = reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + kSubmit)(
            reinterpret_cast<void*>(request));
        if (accepted == 0U) { Release(*lease, false); return false; }
        gSubmitted.fetch_add(1U, std::memory_order_relaxed);
        if (nativeKind == kDirectedRequest) gDirectedSubmitted.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        error = "Retail AISync navigation raised an exception";
        gFailed.fetch_add(1U, std::memory_order_relaxed);
        Release(*lease, false);
        return false;
    }
}
bool Apply(const std::uintptr_t character, const NavigationGoalState& goal,
    const std::uint64_t expiresAt, std::string& error, const bool convergence) noexcept {
    if (gBase == 0U || character < 0x10000U) return false;
    const character_direct_step_runtime::MovementResourceLease resources(character);
    if (!resources) {
        error = "AISync movement resources are not ready";
        return false;
    }
    // Pin outside the SEH leaf: a stop/goal setter can call into native graphs.
    return ApplyPinned(character, goal, expiresAt, error, convergence);
}
bool ApplyGait(const std::uintptr_t character, const std::uint32_t moveType) noexcept {
    std::uintptr_t movement{};
    std::uint32_t requested{};
    if (moveType > 3U || !Read(character + 0x42A8U, movement) || movement == 0U ||
        !Read(movement + 0x194U, requested) || requested > 3U) return false;
    if (requested == moveType) return true;
    __try {
        // FUN_00684900 / 00683E80 author +194 and reset +190 outside the
        // request itself. Client BT/action suppression removes those writers.
        // Effective gait, physical velocity and route remain engine-owned.
        *reinterpret_cast<std::uint32_t*>(movement + 0x194U) = moveType;
        *reinterpret_cast<std::uint32_t*>(movement + 0x190U) = 0U;
        gGaitApplied.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Cancel(const std::uintptr_t character) noexcept {
    if (auto* lease = Find(character)) Release(*lease, true);
}
bool CancelOwned(const std::uintptr_t character, const bool convergence,
    const std::uint32_t requestRevision) noexcept {
    auto* lease = Find(character);
    if (lease == nullptr || !SameLeaseOwner(lease->convergence, lease->requestRevision,
        convergence, requestRevision)) return false;
    Release(*lease, true);
    return true;
}
void Reset(const bool cancelNative) noexcept {
    ResetSubmissionObservations();
    for (auto& lease : gLeases) Release(lease, cancelNative);
    if (TryAcquireSRWLockExclusive(&gGoalHintsLock)) {
        gGoalHints = {};
        ReleaseSRWLockExclusive(&gGoalHintsLock);
    }
}
void Pump(const std::uint64_t now) noexcept {
    for (auto& lease : gLeases) {
        if (lease.character == 0U || now < lease.expires) continue;
        const auto character = lease.character;
        const bool live = Live(lease);
        Release(lease, true);
        if (live) static_cast<void>(character_direct_step_runtime::Stop(character));
        gExpired.fetch_add(1U, std::memory_order_relaxed);
    }
}
Counters Snapshot() noexcept {
    return {gCaptured.load(), gSubmitted.load(), gUpdated.load(), gCompleted.load(),
        gFailed.load(), gCancelled.load(), gExpired.load(), gGaitApplied.load(),
        gDirectedCaptured.load(), gEndpointsCaptured.load(), gDirectedSubmitted.load(),
        gTurnCaptured.load(), gStanceCaptured.load(), gSubmissionsCaptured.load(), gSubmissionsDropped.load(),
        gPathGoalsCaptured.load()};
}
} // namespace aimp::ai_sync::navigation
