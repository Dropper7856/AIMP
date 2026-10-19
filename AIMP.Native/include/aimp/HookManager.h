#pragma once

#include "aimp/BuildProfile.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace aimp {

struct HookRequest {
    std::string name;
    void* target{};
    void* detour{};
    std::size_t patchLength{};
    MaskedBytes expected;
    bool relocationSafePrefix{};
    void** trampolineOut{};
    // Optional lock-free publication used by detours. HookManager stores the
    // trampoline here before patched process threads are resumed, closing the
    // otherwise dangerous install-time window where a detour sees nullptr.
    std::atomic<void*>* publishedTrampolineOut{};
};

// Certified x86 direct-CALL redirection. This is intentionally separate from
// HookRequest: no trampoline is built and no prologue relocation assumption is
// required. InstallCallRedirect verifies that callSite is an E8 rel32 whose
// current destination is exactly expectedTarget before replacing only that
// five-byte CALL with another E8 rel32.
struct CallRedirectRequest {
    std::string name;
    void* callSite{};
    void* expectedTarget{};
    void* detour{};
};

// Complete, exact-build instructions changed as one transaction. No trampoline
// or relocations: every site is verified before any bytes are changed.
struct BytePatchRequest {
    void* target{};
    std::vector<std::byte> expected, replacement;
};

class HookManager final {
public:
    HookManager(bool exactBuildValidated, bool explicitOptIn) noexcept;
    ~HookManager();

    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    bool Install(const HookRequest& request, std::string& error);
    bool InstallCallRedirect(const CallRedirectRequest& request, std::string& error);
    bool InstallPatchBatch(const std::string& name,
        std::span<const BytePatchRequest> patches, std::string& error);
    // Restores target prologues but deliberately retains trampolines. The
    // caller must prove detour quiescence before releasing them.
    bool UninstallAll(std::string& error);
    bool UninstallFrom(std::size_t installedCount, std::string& error);
    bool ReleaseRetiredTrampolines(
        bool detoursQuiescent,
        std::string& error);
    [[nodiscard]] std::size_t InstalledCount() const noexcept;
    [[nodiscard]] std::size_t RetiredCount() const noexcept;
    [[nodiscard]] bool MutationAllowed() const noexcept;

private:
    struct Hook;

    bool exactBuildValidated_{};
    bool explicitOptIn_{};
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Hook>> hooks_;
    std::vector<std::unique_ptr<Hook>> retired_;
};

} // namespace aimp
