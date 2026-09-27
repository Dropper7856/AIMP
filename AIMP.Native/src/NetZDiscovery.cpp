#include "aimp/NetZDiscovery.h"

#include "aimp/CrashGuard.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/ParticipantRegistry.h"
#include "aimp/SignatureResolver.h"

#include <Windows.h>

#include <sstream>

namespace aimp {
namespace {

std::uint64_t Mix(std::uint64_t digest, const std::uint64_t value) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        digest ^= (value >> (index * 8U)) & 0xFFU;
        digest *= kPrime;
    }
    return digest;
}

std::uint64_t DigestIdentity(
    const std::array<std::byte, 16>& identity) noexcept {
    std::uint64_t digest = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (const auto value : identity) {
        digest ^= std::to_integer<std::uint8_t>(value);
        digest *= kPrime;
    }
    return digest;
}

} // namespace

std::atomic<NetZDiscovery*> NetZDiscovery::activeInstance_{nullptr};
void* NetZDiscovery::originalDispatch_ = nullptr;
std::atomic<std::uint64_t> NetZDiscovery::dispatchCount_{0};
std::atomic<std::uint32_t> NetZDiscovery::dispatchThread_{0};
std::atomic<std::uint32_t> NetZDiscovery::activeDispatchCalls_{0};

NetZDiscovery::NetZDiscovery(
    Diagnostics& diagnostics,
    const SignatureResolver& resolver,
    const ChannelInspector& inspector,
    const ParticipantRegistry& participants,
    const std::uintptr_t moduleBase,
    const std::size_t imageSize) noexcept
    : diagnostics_(&diagnostics),
      resolver_(&resolver),
      inspector_(&inspector),
      participants_(&participants),
      moduleBase_(moduleBase),
      imageSize_(imageSize) {}

NetZDiscovery::~NetZDiscovery() {
    auto* expected = this;
    activeInstance_.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel);
}

void NetZDiscovery::Poll() {
    ++pollCount_;
    const auto* rootVariable = resolver_->Resolve(SymbolId::GlobalStateRoot);
    const auto* expectedVtable = resolver_->Resolve(SymbolId::NetzVtable);
    if (rootVariable == nullptr || expectedVtable == nullptr) {
        Invalidate("profile_root_or_vtable_unresolved");
        return;
    }

    const auto root = crash_guard::SafeRead<std::uintptr_t>(rootVariable);
    std::uintptr_t netz = 0;
    std::uintptr_t characterManager = 0;
    std::uintptr_t participantWrapper = 0;
    std::uintptr_t contextPool = 0;
    if (root.has_value() && *root != 0) {
        const auto netzValue = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(*root + 0x04));
        const auto characterManagerValue = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(*root + 0x08));
        const auto participantWrapperValue = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(*root + 0x0C));
        const auto contextPoolValue = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(*root + 0x4C));
        if (netzValue.has_value()) {
            netz = *netzValue;
        }
        if (characterManagerValue.has_value()) {
            characterManager = *characterManagerValue;
        }
        if (participantWrapperValue.has_value()) {
            participantWrapper = *participantWrapperValue;
        }
        if (contextPoolValue.has_value()) {
            contextPool = *contextPoolValue;
        }
    }

    if (netz != 0) {
        const auto vtable = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(netz));
        if (!vtable.has_value() ||
            *vtable != reinterpret_cast<std::uintptr_t>(expectedVtable)) {
            netz = 0;
        }
    }

    const auto previous =
        currentNetz_.exchange(netz, std::memory_order_acq_rel);
    if (previous != netz) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        lastSnapshotDigest_ = 0;
        lastParticipantDigest_ = 0;
        if (previous != 0) {
            diagnostics_->Log(
                "info",
                "netz_lost",
                "The previously observed NetZ pointer was invalidated.",
                {{"address", Diagnostics::Pointer(
                                 reinterpret_cast<const void*>(previous))},
                 {"generation", Diagnostics::Unsigned(Generation()), true}});
        }
        if (netz != 0) {
            diagnostics_->Log(
                "info",
                "netz_detected",
                "Exact-profile read-only discovery found a NetZ instance.",
                {{"address", Diagnostics::Pointer(
                                 reinterpret_cast<const void*>(netz))},
                 {"root", Diagnostics::Pointer(
                              reinterpret_cast<const void*>(root.value_or(0)))},
                 {"characterManager", Diagnostics::Pointer(
                                          reinterpret_cast<const void*>(
                                              characterManager))},
                 {"participantWrapper", Diagnostics::Pointer(
                                           reinterpret_cast<const void*>(
                                               participantWrapper))},
                 {"contextPool", Diagnostics::Pointer(
                                     reinterpret_cast<const void*>(contextPool))},
                 {"generation", Diagnostics::Unsigned(Generation()), true}});
        }
    }

    RuntimeObservationSnapshot runtime{};
    runtime.globalRoot = root.value_or(0);
    runtime.netz = netz;
    runtime.characterManager = characterManager;
    runtime.participantWrapper = participantWrapper;
    runtime.contextPool = contextPool;

    if (netz == 0 || (pollCount_ % 4U) != 0) {
        std::lock_guard lock(runtimeSnapshotMutex_);
        if (runtimeSnapshot_.globalRoot == runtime.globalRoot &&
            runtimeSnapshot_.netz == runtime.netz &&
            runtimeSnapshot_.characterManager ==
                runtime.characterManager &&
            runtimeSnapshot_.participantWrapper ==
                runtime.participantWrapper &&
            runtimeSnapshot_.contextPool == runtime.contextPool) {
            runtime.participantCount =
                runtimeSnapshot_.participantCount;
            runtime.freeChannelMask =
                runtimeSnapshot_.freeChannelMask;
            runtime.participantRegistryAvailable =
                runtimeSnapshot_.participantRegistryAvailable;
            runtime.channelSnapshotAvailable =
                runtimeSnapshot_.channelSnapshotAvailable;
        }
        runtimeSnapshot_ = runtime;
        return;
    }

    NetzSnapshot snapshot{};
    if (inspector_->Inspect(netz, snapshot)) {
        runtime.channelSnapshotAvailable = snapshot.consistent;
        runtime.freeChannelMask = snapshot.freeMask;
        auto digest = snapshot.descriptorDigest;
        digest = Mix(digest, snapshot.freeMask);
        digest = Mix(digest, snapshot.activeMask);
        digest = Mix(digest, snapshot.nextLogicalId);
        digest = Mix(digest, snapshot.transport);
        for (const auto& channel : snapshot.channels) {
            digest = Mix(digest, channel.logicalId);
            digest = Mix(digest, channel.entryCount);
            digest = Mix(digest, channel.mode);
            for (const auto& descriptor : channel.descriptors) {
                digest = Mix(digest, descriptor.auxiliaryAddress);
            }
        }
        if (digest != lastSnapshotDigest_) {
            lastSnapshotDigest_ = digest;
            LogSnapshot(snapshot);
        }
    }

    if (participantWrapper != 0 && participants_ != nullptr) {
        const auto participantOwner = crash_guard::SafeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(participantWrapper));
        ParticipantTableSnapshot participants{};
        if (participantOwner.has_value() &&
            participants_->Inspect(*participantOwner, participants)) {
            runtime.participantRegistryAvailable =
                participants.valid && participants.consistent;
            runtime.participantCount = participants.count;
            std::uint64_t digest = 1469598103934665603ULL;
            digest = Mix(digest, participants.count);
            for (const auto& participant : participants.participants) {
                digest = Mix(
                    digest,
                    DigestIdentity(participant.identity128));
                digest = Mix(digest, participant.flagsC8);
                digest = Mix(digest, participant.associatedObjectD0);
                digest = Mix(digest, participant.associatedValueD4);
                digest = Mix(digest, participant.keyF4);
            }
            if (digest != lastParticipantDigest_) {
                lastParticipantDigest_ = digest;
                diagnostics_->Log(
                    "info",
                    "participant_table_snapshot",
                    "Read-only snapshot of the bounded inline participant table.",
                    {{"owner", Diagnostics::Pointer(
                                   reinterpret_cast<const void*>(
                                       participants.owner))},
                     {"count", Diagnostics::Unsigned(participants.count), true},
                     {"capacity", "8", true}});
                for (const auto& participant : participants.participants) {
                    diagnostics_->Log(
                        "info",
                        "participant_entry_snapshot",
                        "Native values are process-local observations, not network identities.",
                        {{"index", Diagnostics::Unsigned(participant.index), true},
                         {"identityDigest",
                          Diagnostics::Unsigned(
                              DigestIdentity(participant.identity128)),
                          true},
                         {"flagsC8", Diagnostics::Unsigned(participant.flagsC8), true},
                         {"associatedObjectD0", Diagnostics::Pointer(
                                                    reinterpret_cast<const void*>(
                                                        participant.associatedObjectD0))},
                         {"associatedValueD4",
                          Diagnostics::Unsigned(participant.associatedValueD4),
                          true},
                         {"keyF4", Diagnostics::Unsigned(participant.keyF4), true}});
                }
            }
        }
    }
    {
        std::lock_guard lock(runtimeSnapshotMutex_);
        runtimeSnapshot_ = runtime;
    }
}

void NetZDiscovery::Invalidate(const std::string_view reason) noexcept {
    try {
        const auto previous =
            currentNetz_.exchange(0, std::memory_order_acq_rel);
        {
            std::lock_guard lock(runtimeSnapshotMutex_);
            runtimeSnapshot_ = {};
        }
        if (previous == 0) {
            return;
        }
        generation_.fetch_add(1, std::memory_order_acq_rel);
        lastSnapshotDigest_ = 0;
        lastParticipantDigest_ = 0;
        diagnostics_->Log(
            "info",
            "netz_invalidated",
            reason,
            {{"address", Diagnostics::Pointer(
                             reinterpret_cast<const void*>(previous))},
             {"generation", Diagnostics::Unsigned(Generation()), true}});
    } catch (...) {
        currentNetz_.store(0, std::memory_order_release);
    }
}

RuntimeObservationSnapshot NetZDiscovery::RuntimeSnapshot() const noexcept {
    try {
        std::lock_guard lock(runtimeSnapshotMutex_);
        return runtimeSnapshot_;
    } catch (...) {
        return {};
    }
}

bool NetZDiscovery::InstallOptionalDispatchObservationHook(
    HookManager& hooks,
    const SymbolSpec& dispatchSpec,
    std::string& error) {
    if (dispatchSpec.id != SymbolId::NetzDispatch ||
        dispatchSpec.approvedPatchLength == 0 ||
        !dispatchSpec.trampolineRelocationSafe) {
        error = "dispatch symbol is not approved for an observation trampoline";
        return false;
    }
    auto* target = resolver_->ResolveMutable(dispatchSpec.id);
    if (target == nullptr) {
        error = "dispatch RVA/signature did not resolve";
        return false;
    }

    NetZDiscovery* empty = nullptr;
    if (!activeInstance_.compare_exchange_strong(
            empty,
            this,
            std::memory_order_acq_rel)) {
        error = "another discovery hook instance is active";
        return false;
    }

    HookRequest request{};
    request.name = "NETZ_DISPATCH observation";
    request.target = target;
    request.detour = reinterpret_cast<void*>(&DispatchDetour);
    request.patchLength = dispatchSpec.approvedPatchLength;
    request.expected = dispatchSpec.expected;
    request.relocationSafePrefix = dispatchSpec.trampolineRelocationSafe;
    request.trampolineOut = &originalDispatch_;
    if (!hooks.Install(request, error)) {
        activeInstance_.store(nullptr, std::memory_order_release);
        originalDispatch_ = nullptr;
        return false;
    }
    return true;
}

void NetZDiscovery::CollectHookCounters() {
    const auto count = dispatchCount_.load(std::memory_order_acquire);
    const auto thread = dispatchThread_.load(std::memory_order_acquire);
    if (count == lastLoggedDispatchCount_ &&
        thread == lastLoggedDispatchThread_) {
        return;
    }
    lastLoggedDispatchCount_ = count;
    lastLoggedDispatchThread_ = thread;
    diagnostics_->Log(
        "info",
        "netz_dispatch_observation",
        "Optional hook counters were copied by the bootstrap thread.",
        {{"calls", Diagnostics::Unsigned(count), true},
         {"candidateThreadId", Diagnostics::Unsigned(thread), true}});
}

std::uintptr_t NetZDiscovery::CurrentNetz() const noexcept {
    return currentNetz_.load(std::memory_order_acquire);
}

std::uint64_t NetZDiscovery::Generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

std::uint32_t NetZDiscovery::ActiveDispatchCalls() const noexcept {
    return activeDispatchCalls_.load(std::memory_order_acquire);
}

void __fastcall NetZDiscovery::DispatchDetour(
    void* self,
    void*) noexcept {
    activeDispatchCalls_.fetch_add(1, std::memory_order_acq_rel);
    if (activeInstance_.load(std::memory_order_acquire) != nullptr) {
        dispatchThread_.store(GetCurrentThreadId(), std::memory_order_release);
        dispatchCount_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto original =
        reinterpret_cast<void(__thiscall*)(void*)>(originalDispatch_);
    if (original != nullptr) {
        original(self);
    }
    activeDispatchCalls_.fetch_sub(1, std::memory_order_acq_rel);
}

void NetZDiscovery::LogSnapshot(const NetzSnapshot& snapshot) {
    diagnostics_->Log(
        snapshot.consistent ? "info" : "warning",
        "netz_channel_snapshot",
        snapshot.consistent
            ? "Read-only channel snapshot."
            : "Channel masks or registry headers changed during the lock-free read; treat as diagnostic only.",
        {{"netz", Diagnostics::Pointer(
                       reinterpret_cast<const void*>(snapshot.address))},
         {"activeMask", Diagnostics::Unsigned(snapshot.activeMask), true},
         {"freeMask", Diagnostics::Unsigned(snapshot.freeMask), true},
         {"nextLogicalId", Diagnostics::Unsigned(snapshot.nextLogicalId), true},
         {"transport", Diagnostics::Pointer(
                           reinterpret_cast<const void*>(snapshot.transport))},
         {"descriptorDigest",
          Diagnostics::Unsigned(snapshot.descriptorDigest),
          true},
         {"consistent", Diagnostics::Boolean(snapshot.consistent), true}});

    for (const auto& channel : snapshot.channels) {
        if (!channel.active &&
            channel.entryCount == 0 &&
            channel.logicalId == 0) {
            continue;
        }
        diagnostics_->Log(
            channel.registryValid ? "info" : "warning",
            "netz_channel",
            "Channel data is a process-local observation.",
            {{"index", Diagnostics::Unsigned(channel.index), true},
             {"logicalId", Diagnostics::Unsigned(channel.logicalId), true},
             {"active", Diagnostics::Boolean(channel.active), true},
             {"free", Diagnostics::Boolean(channel.free), true},
             {"entryCount", Diagnostics::Unsigned(channel.entryCount), true},
             {"entryCapacity", Diagnostics::Unsigned(channel.entryCapacity), true},
             {"mode", Diagnostics::Unsigned(channel.mode), true},
             {"registryStable", Diagnostics::Boolean(channel.registryStable), true},
             {"registryValid", Diagnostics::Boolean(channel.registryValid), true}});
        for (const auto& descriptor : channel.descriptors) {
            diagnostics_->Log(
                "info",
                "netz_descriptor",
                "Runtime ID is local to this registry and is not a portable network identity.",
                {{"channelIndex", Diagnostics::Unsigned(channel.index), true},
                 {"name", descriptor.name},
                 {"runtimeId", Diagnostics::Unsigned(descriptor.runtimeId), true},
                 {"decoderRva", Diagnostics::Unsigned(descriptor.decoderRva), true},
                 {"auxiliary", Diagnostics::Pointer(
                                   reinterpret_cast<const void*>(
                                       descriptor.auxiliaryAddress))}});
        }
    }
}

} // namespace aimp
