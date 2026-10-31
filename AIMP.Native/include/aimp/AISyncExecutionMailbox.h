#pragma once
#include "aimp/AISyncExecutionCodec.h"
#include "aimp/AISyncRouteCodec.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <string_view>

namespace aimp::ai_sync::execution {
struct MailboxEngine {
    void* context{};
    transactions::Result (*bind)(void*, const transactions::Scope&, std::uint32_t) noexcept{};
    bool (*execute)(void*, std::span<const std::uint8_t>, const transactions::Scope&, std::span<Receipt>, std::size_t&) noexcept{};
    bool (*retry)(void*, const transactions::Record&, std::uint64_t, const transactions::Scope&, Receipt&) noexcept{};
    transactions::Result (*checkpoint)(void*, const transactions::Scope&, std::uint64_t) noexcept{};
    bool (*carrier)(void*,std::span<const std::uint8_t>,const transactions::Scope&,std::uint64_t arrivalTick) noexcept{};
    bool (*route)(void*,std::span<const std::uint8_t>,const transactions::Scope&) noexcept{};
};
struct MailboxPacket {
    bool receipt{};
    std::size_t size{};
    std::uint64_t token{};
    std::array<std::uint8_t, kMaximumReceiptBytes> bytes{};
};
struct MailboxFailure {
    std::uint64_t count{};
    std::uint32_t site{},reason{};
    std::uint64_t sequence{};
    std::uint32_t kind{},opcode{},stage{};
};
// Worker/engine handoff only. Binding is not an ownership grant. Engine callbacks
// run with NO mailbox lock held. The caller owns this object through quiescence.
class ExecutionMailbox final {
public:
    static constexpr std::size_t kIncoming = 256, kOutgoing = 256;
    [[nodiscard]] bool QueueBinding(const Control&, std::uint32_t localNativeGeneration) noexcept;
    [[nodiscard]] bool QueueBatch(std::span<const std::uint8_t>, bool retry = false) noexcept;
    [[nodiscard]] bool QueueCarrier(std::span<const std::uint8_t>) noexcept;
    [[nodiscard]] bool QueueRoute(std::span<const std::uint8_t>) noexcept;
    void Retire() noexcept;
    // Revoke only the old world. A newer Offer queued by IPC must survive.
    void RetireGeneration(std::uint32_t nativeGeneration) noexcept;
    [[nodiscard]] bool Fence(Recovery) noexcept; // Preserve produced receipts, append terminal recovery.
    [[nodiscard]] bool Fence(const transactions::Scope&, Recovery) noexcept;
    [[nodiscard]] bool Matches(const transactions::Scope&) noexcept;
    void Pump(const MailboxEngine&) noexcept; // Qualified engine thread only.
    [[nodiscard]] bool Current(const transactions::Scope&, std::uint32_t nativeGeneration) const noexcept;
    [[nodiscard]] bool Bound(const transactions::Scope&, std::uint32_t nativeGeneration) noexcept;
    [[nodiscard]] bool BindingActive() const noexcept { return activeRevision_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] bool Peek(MailboxPacket&) noexcept;
    [[nodiscard]] bool Commit(std::uint64_t token) noexcept;
    [[nodiscard]] MailboxFailure LastFailure() noexcept;
private:
    struct Work {
        Control binding{};
        std::uint32_t nativeGeneration{};
        std::uint64_t revision{},queuedTick{};
        bool bind{};
        bool retry{};
        bool checkpoint{};
        bool carrier{};
        bool route{};
        std::size_t size{};
        std::array<std::uint8_t, (std::max)(kMaximumBatchBytes,::aimp::ai_sync::route::kMaxBytes)> bytes{};
    };
    void InvalidateLocked() noexcept;
    void FailLocked(Recovery, std::uint32_t site = __builtin_LINE(),std::uint32_t stage = 0,
        const transactions::Record* operation = nullptr) noexcept;
    bool PublishLocked(const Control&) noexcept;
    bool PublishLocked(const Receipt&) noexcept;
    std::mutex gate_;
    std::array<Work, kIncoming> incoming_{};
    std::array<MailboxPacket, kOutgoing> outgoing_{};
    std::size_t inRead_{}, inCount_{}, outRead_{}, outCount_{};
    Control requested_{};
    std::uint32_t nativeGeneration_{};
    std::uint64_t revision_{1}, nextToken_{};
    std::uint64_t lastCarrier_{};
    struct SessionWatermark { transactions::SessionId session{}; std::uint64_t consumer{}, authority{}, world{}; };
    std::array<SessionWatermark, 128> sessions_{};
    std::size_t sessionCount_{};
    std::atomic<std::uint64_t> activeRevision_{};
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
    Work processing_{}; // Engine-thread-only, stable across synchronous callbacks.
    bool processingActive_{};
    bool bindingPending_{}; // gate_, including a popped bind awaiting native retirement.
    MailboxFailure failure_{}; // gate_, retained across replacement bindings.
};
[[nodiscard]] bool SessionTextMatches(const transactions::SessionId&, std::string_view) noexcept;
} // namespace aimp::ai_sync::execution
