#pragma once

#include "aimp/ChannelInspector.h"
#include "aimp/Common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace aimp {

class Diagnostics;
class HookManager;
class ParticipantRegistry;
class SignatureResolver;
struct SymbolSpec;

struct RuntimeObservationSnapshot {
    std::uintptr_t globalRoot{};
    std::uintptr_t netz{};
    std::uintptr_t characterManager{};
    std::uintptr_t participantWrapper{};
    std::uintptr_t contextPool{};
    std::uint32_t participantCount{};
    std::uint32_t freeChannelMask{};
    bool participantRegistryAvailable{};
    bool channelSnapshotAvailable{};
};

class NetZDiscovery final {
public:
    NetZDiscovery(
        Diagnostics& diagnostics,
        const SignatureResolver& resolver,
        const ChannelInspector& inspector,
        const ParticipantRegistry& participants,
        std::uintptr_t moduleBase,
        std::size_t imageSize) noexcept;

    ~NetZDiscovery();

    NetZDiscovery(const NetZDiscovery&) = delete;
    NetZDiscovery& operator=(const NetZDiscovery&) = delete;

    [[nodiscard]] static constexpr ModuleStatus Status() noexcept {
        return ModuleStatus::PartiallyImplemented;
    }

    void Poll();
    void Invalidate(std::string_view reason) noexcept;

    bool InstallOptionalDispatchObservationHook(
        HookManager& hooks,
        const SymbolSpec& dispatchSpec,
        std::string& error);

    void CollectHookCounters();

    [[nodiscard]] std::uintptr_t CurrentNetz() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint32_t ActiveDispatchCalls() const noexcept;
    [[nodiscard]] RuntimeObservationSnapshot RuntimeSnapshot() const noexcept;

private:
    static void __fastcall DispatchDetour(void* self, void* ignoredEdx) noexcept;
    void LogSnapshot(const NetzSnapshot& snapshot);

    Diagnostics* diagnostics_{};
    const SignatureResolver* resolver_{};
    const ChannelInspector* inspector_{};
    const ParticipantRegistry* participants_{};
    std::uintptr_t moduleBase_{};
    std::size_t imageSize_{};
    std::atomic<std::uintptr_t> currentNetz_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::uint64_t lastSnapshotDigest_{};
    std::uint64_t lastParticipantDigest_{};
    std::uint64_t pollCount_{};
    std::uint64_t lastLoggedDispatchCount_{};
    std::uint32_t lastLoggedDispatchThread_{};
    mutable std::mutex runtimeSnapshotMutex_;
    RuntimeObservationSnapshot runtimeSnapshot_{};

    static std::atomic<NetZDiscovery*> activeInstance_;
    static void* originalDispatch_;
    static std::atomic<std::uint64_t> dispatchCount_;
    static std::atomic<std::uint32_t> dispatchThread_;
    static std::atomic<std::uint32_t> activeDispatchCalls_;
};

} // namespace aimp
