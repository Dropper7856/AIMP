#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace aimp {

class CaptureCatalog;
class HookManager;
class LevelGenerationTracker;
class LifecycleCatalog;
class SignatureResolver;
class SpawnCharacterTracker;

class CharacterObservationHooks final {
public:
    CharacterObservationHooks(
        CaptureCatalog& catalog,
        LifecycleCatalog& lifecycle,
        LevelGenerationTracker& generation,
        SpawnCharacterTracker* spawnTracker = nullptr) noexcept;

    bool InstallExactProfile(
        SignatureResolver& resolver,
        HookManager& hooks,
        std::string& error);
    // Functional lifetime safety only, installed automatically for action input.
    // Reuses the full bundle when present; otherwise installs only the two
    // shared destruction hooks, without the diagnostic capture hooks.
    bool InstallActionLifetimeHooks(SignatureResolver& resolver,
        HookManager& hooks, std::string& error);
    [[nodiscard]] bool LifetimeHooksInstalled() const noexcept;
    void BeginLogicalShutdown() noexcept;

    [[nodiscard]] bool Installed() const noexcept;
    [[nodiscard]] static std::uint32_t ActiveDetours() noexcept;
    [[nodiscard]] static bool CaptureServicesAttached() noexcept;
    [[nodiscard]] bool ProcessLifetimeRequired() const noexcept;

private:
    CaptureCatalog* catalog_{};
    LifecycleCatalog* lifecycle_{};
    LevelGenerationTracker* generation_{};
    SpawnCharacterTracker* spawnTracker_{};
    void* f0Trampoline_{};
    void* f130Trampoline_{};
    void* characterCreateTrampoline_{};
    void* characterDestroyTrampoline_{};
    void* characterFullDestroyTrampoline_{};
    void* characterFinalizeTrampoline_{};
    void* participantAppendTrampoline_{};
    void* channelAllocateTrampoline_{};
    void* channelReleaseTrampoline_{};
    void* contextAllocateTrampoline_{};
    void* contextReleaseTrampoline_{};
    bool installed_{};
    bool lifetimeHooksInstalled_{};
    bool processLifetimeRequired_{};
};

} // namespace aimp
