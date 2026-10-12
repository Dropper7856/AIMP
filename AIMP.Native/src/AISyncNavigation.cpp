#include "aimp/AISyncNavigation.h"
#include "aimp/CrashGuard.h"
#include "aimp/CharacterDirectStepRuntime.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace aimp::ai_sync::navigation {
namespace {
constexpr std::uintptr_t kScheduler = 0x2CD0U, kRequestType = 0x00EB1A70U;
constexpr std::uintptr_t kCreate = 0x00283BF0U, kSetGoal = 0x002777B0U;
constexpr std::uintptr_t kSubmit = 0x00283CE0U, kCancel = 0x00283DB0U;
constexpr std::uintptr_t kUpdateGoal = 0x002C7550U;
constexpr std::uintptr_t kReleaseRef = 0x0000D770U;
struct Ref final { std::uintptr_t object{}, control{}; };
struct Lease final {
    Ref ref{};
    std::uintptr_t character{};
    std::uint32_t runtimeId{}, revision{}, requestRevision{};
    std::uint64_t expires{};
    bool finished{}, failed{};
};
std::array<Lease, 32U> gLeases{};
std::uintptr_t gBase{};
std::atomic<std::uint64_t> gCaptured{}, gSubmitted{}, gUpdated{}, gCompleted{};
std::atomic<std::uint64_t> gFailed{}, gCancelled{}, gExpired{};
template<typename T> bool Read(const std::uintptr_t address, T& value) noexcept {
    __try { value = *reinterpret_cast<const T*>(address); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Match(const std::uintptr_t rva, const std::initializer_list<unsigned char> bytes) noexcept {
    for (std::size_t i = 0U; const auto expected : bytes) {
        unsigned char actual{};
        if (!Read(gBase + rva + i++, actual) || actual != expected) return false;
    }
    return true;
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
        if (lease.ref.object != 0U && cancel && Live(lease) && !lease.finished) {
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
    if (!Match(kCreate, {0x51,0x56,0x8B,0x74,0x24,0x0C,0x56}) ||
        !Match(kCreate + 0x18U, {0xC2,0x04,0x00}) ||
        !Match(kSetGoal, {0x8B,0x44,0x24,0x04}) ||
        !Match(kSubmit, {0x64,0xA1,0x00,0x00,0x00,0x00,0x6A,0xFF}) ||
        !Match(kCancel, {0x64,0xA1,0x00,0x00,0x00,0x00,0x6A,0xFF}) ||
        !Match(kUpdateGoal, {0x83,0x79,0x68,0x03}) ||
        !Match(kReleaseRef, {0x56,0x8B,0x71,0x04}) ||
        !Match(0x00262661U, {0xC2,0x04,0x00}) ||
        !Match(0x002612B1U, {0xC6,0x46,0x1A,0x3B})) {
        error = "AISync native navigation ABI mismatch";
        gBase = 0U;
        return false;
    }
    return true;
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
        !Read(request, type) || type != gBase + kRequestType ||
        !Read(request + 0x10U, kind) || kind != 1U ||
        !Read(request + 0x14U, owner) || owner != scheduler ||
        !Read(request + 0xCU, status) || (status != 1U && status != 2U) ||
        !Read(request + 0x18U, active) || active == 0U ||
        !Read(request + 0xB8U, flags)) return sample;
    const auto goalOffset = (flags & 0x40U) != 0U ? 0x34U : 0x1CU;
    for (std::size_t i = 0U; i < 3U; ++i)
        if (!Read(request + goalOffset + i * 4U, sample.goal.position[i])) return {};
    if (!Read(request + 0x54U, sample.goal.stoppingDistance) ||
        !Read(request + 0x78U, sample.goal.moveType)) return {};
    // Capture assigns the portable revision in AISync, not the native object.
    sample.goal.revision = 1U;
    sample.goal.requestRevision = 1U;
    sample.goal.flags = 1U;
    if (!ValidGoal(sample.goal)) return {};
    sample.request = request;
    gCaptured.fetch_add(1U, std::memory_order_relaxed);
    return sample;
}
bool Apply(const std::uintptr_t character, const NavigationGoalState& goal,
    const std::uint64_t expiresAt, std::string& error) noexcept {
    if (gBase == 0U || goal.flags != 1U || !ValidGoal(goal)) return false;
    auto* lease = Find(character);
    if (lease != nullptr && !Live(*lease)) {
        Release(*lease, false);
        lease = nullptr;
    }
    if (lease != nullptr && lease->requestRevision == goal.requestRevision &&
        (!lease->finished || lease->revision == goal.revision)) {
        lease->expires = expiresAt;
        if (lease->finished) return !lease->failed; // failure cannot own movement
        std::uint32_t status{};
        if (!Live(*lease) || !Read(lease->ref.object + 0xCU, status)) return false;
        if (status == 3U && lease->revision == goal.revision) {
            lease->finished = true;
            gCompleted.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        if (status == 4U || status == 5U || status == 6U) {
            error = "Retail AISync navigation request failed or was interrupted";
            lease->finished = true;
            lease->failed = true;
            lease->revision = goal.revision;
            gFailed.fetch_add(1U, std::memory_order_relaxed);
            return false; // caller resumes Host steering, without releasing the actor
        }
        if (status == 3U) {
            lease->finished = true;
            return Apply(character, goal, expiresAt, error);
        }
        if (lease->revision != goal.revision) {
            __try {
                const auto request = lease->ref.object;
                reinterpret_cast<void(__thiscall*)(void*, const float*)>(gBase + kUpdateGoal)(
                    reinterpret_cast<void*>(request), goal.position);
                *reinterpret_cast<float*>(request + 0x54U) = goal.stoppingDistance;
                *reinterpret_cast<std::uint32_t*>(request + 0x78U) = goal.moveType;
                // The native setter queues one target while a path is active.
                // Retry on the next engine tick if its pending slot was busy.
                std::uint8_t pending{};
                if (Read(request + 0xB8U, pending) && (pending & 0x40U) != 0U &&
                    std::memcmp(reinterpret_cast<const void*>(request + 0x34U), goal.position, 12U) == 0) {
                    lease->revision = goal.revision;
                    gUpdated.fetch_add(1U, std::memory_order_relaxed);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        return true;
    }
    if (lease != nullptr) Release(*lease, true);
    else for (auto& entry : gLeases) if (entry.character == 0U) { lease = &entry; break; }
    if (lease == nullptr) { error = "AISync navigation capacity exhausted"; return false; }
    __try {
        std::uintptr_t schedulerOwner{};
        if (!Read(character + kScheduler, schedulerOwner) || schedulerOwner != character ||
            !Read(character + 0x50U, lease->runtimeId)) return false;
        lease->character = character; lease->revision = goal.revision; lease->expires = expiresAt;
        lease->requestRevision = goal.requestRevision;
        reinterpret_cast<Ref*(__thiscall*)(void*, Ref*)>(gBase + kCreate)(
            reinterpret_cast<void*>(character + kScheduler), &lease->ref);
        if (lease->ref.object == 0U || lease->ref.control == 0U) {
            Release(*lease, false); return false;
        }
        const auto request = lease->ref.object;
        std::uintptr_t type{};
        if (!Read(request, type) || type != gBase + kRequestType) {
            Release(*lease, false); return false;
        }
        reinterpret_cast<void(__thiscall*)(void*, const float*)>(gBase + kSetGoal)(
            reinterpret_cast<void*>(request), goal.position);
        // Same value-only initialization as retail Move-to-point producers.
        *reinterpret_cast<float*>(request + 0x54U) = goal.stoppingDistance;
        *reinterpret_cast<std::uint32_t*>(request + 0x78U) = goal.moveType;
        *reinterpret_cast<std::uint8_t*>(request + 0x18U) = 1U;
        *reinterpret_cast<std::uint8_t*>(request + 0xB8U) &= ~0x10U;
        static_cast<void>(character_direct_step_runtime::ReleaseAIIntentOwnership(character));
        const auto accepted = reinterpret_cast<std::uint8_t(__thiscall*)(void*)>(gBase + kSubmit)(
            reinterpret_cast<void*>(request));
        if (accepted == 0U) { Release(*lease, false); return false; }
        gSubmitted.fetch_add(1U, std::memory_order_relaxed);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        error = "Retail AISync navigation raised an exception";
        gFailed.fetch_add(1U, std::memory_order_relaxed);
        Release(*lease, false);
        return false;
    }
}
void Cancel(const std::uintptr_t character) noexcept {
    if (auto* lease = Find(character)) Release(*lease, true);
}
void Reset(const bool cancelNative) noexcept {
    for (auto& lease : gLeases) Release(lease, cancelNative);
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
        gFailed.load(), gCancelled.load(), gExpired.load()};
}
} // namespace aimp::ai_sync::navigation
