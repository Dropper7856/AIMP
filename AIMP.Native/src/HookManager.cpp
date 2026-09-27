#include "aimp/HookManager.h"

#include "aimp/CrashGuard.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <sstream>

namespace aimp {
namespace {

class FrozenThreads final {
public:
    FrozenThreads() = default;
    ~FrozenThreads() {
        Resume();
    }

    FrozenThreads(const FrozenThreads&) = delete;
    FrozenThreads& operator=(const FrozenThreads&) = delete;

    bool Freeze(std::string& error) {
        const auto processId = GetCurrentProcessId();
        const auto currentThreadId = GetCurrentThreadId();
        std::vector<DWORD> threadIds;
        threadIds.reserve(128);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = "CreateToolhelp32Snapshot failed";
            return false;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL available = Thread32First(snapshot, &entry);
        while (available) {
            if (entry.th32OwnerProcessID == processId &&
                entry.th32ThreadID != currentThreadId) {
                threadIds.push_back(entry.th32ThreadID);
            }
            entry.dwSize = sizeof(entry);
            available = Thread32Next(snapshot, &entry);
        }
        CloseHandle(snapshot);

        threads_.reserve(threadIds.size());
        for (const auto threadId : threadIds) {
            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME |
                    THREAD_GET_CONTEXT |
                    THREAD_QUERY_INFORMATION,
                FALSE,
                threadId);
            if (thread == nullptr) {
                const auto openError = GetLastError();
                // The thread list is only a snapshot. Short-lived worker
                // threads may exit between Thread32Next and OpenThread. That
                // race is harmless: there is no remaining thread to freeze.
                if (openError == ERROR_INVALID_PARAMETER ||
                    openError == ERROR_NOT_FOUND) {
                    continue;
                }
                ResumeNow();
                std::ostringstream stream;
                stream << "OpenThread failed while preparing process freeze"
                       << " (thread=" << threadId
                       << ", error=" << openError << ')';
                error = stream.str();
                return false;
            }

            DWORD exitCode = STILL_ACTIVE;
            if (GetExitCodeThread(thread, &exitCode) &&
                exitCode != STILL_ACTIVE) {
                CloseHandle(thread);
                continue;
            }
            threads_.push_back(thread);
        }

        // No heap allocation, logging, or string mutation is permitted after
        // the first successful SuspendThread until ResumeNow completes.
        for (std::size_t index = 0; index < threads_.size();) {
            if (SuspendThread(threads_[index]) == static_cast<DWORD>(-1)) {
                const auto suspendError = GetLastError();
                DWORD exitCode = STILL_ACTIVE;
                if (GetExitCodeThread(threads_[index], &exitCode) &&
                    exitCode != STILL_ACTIVE) {
                    CloseHandle(threads_[index]);
                    threads_.erase(threads_.begin() + index);
                    continue;
                }

                suspendedCount_ = index;
                ResumeNow();
                std::ostringstream stream;
                stream << "SuspendThread failed"
                       << " (error=" << suspendError << ')';
                error = stream.str();
                return false;
            }
            suspendedCount_ = index + 1;
            ++index;
        }
        return true;
    }

    bool InstructionPointersOutside(
        const void* beginAddress,
        const std::size_t bytes) const noexcept {
        const auto begin =
            reinterpret_cast<std::uintptr_t>(beginAddress);
        if (begin > std::numeric_limits<std::uintptr_t>::max() - bytes) {
            return false;
        }
        const auto end = begin + bytes;
        for (std::size_t index = 0; index < suspendedCount_; ++index) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(threads_[index], &context)) {
                DWORD exitCode = STILL_ACTIVE;
                if (GetExitCodeThread(threads_[index], &exitCode) &&
                    exitCode != STILL_ACTIVE) {
                    continue;
                }
                return false;
            }
#if defined(_M_IX86)
            const auto instructionPointer =
                static_cast<std::uintptr_t>(context.Eip);
#else
            return false;
#endif
            if (instructionPointer >= begin && instructionPointer < end) {
                return false;
            }
        }
        return true;
    }

    void ResumeNow() noexcept {
        for (std::size_t index = suspendedCount_; index > 0; --index) {
            ResumeThread(threads_[index - 1]);
        }
        suspendedCount_ = 0;
        for (const auto thread : threads_) {
            CloseHandle(thread);
        }
        threads_.clear();
    }

private:
    void Resume() noexcept {
        ResumeNow();
    }

    std::vector<HANDLE> threads_;
    std::size_t suspendedCount_{};
};

bool MakeRelativeJump(
    const void* instruction,
    const void* destination,
    std::array<std::byte, 5>& output) noexcept {
    const auto source = reinterpret_cast<std::uintptr_t>(instruction);
    const auto target = reinterpret_cast<std::uintptr_t>(destination);
    const auto after = static_cast<std::int64_t>(source) + 5;
    const auto displacement = static_cast<std::int64_t>(target) - after;
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    output[0] = std::byte{0xE9};
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(output.data() + 1, &relative, sizeof(relative));
    return true;
}

bool MakeRelativeCall(
    const void* instruction,
    const void* destination,
    std::array<std::byte, 5>& output) noexcept {
    const auto source = reinterpret_cast<std::uintptr_t>(instruction);
    const auto target = reinterpret_cast<std::uintptr_t>(destination);
    const auto after = static_cast<std::int64_t>(source) + 5;
    const auto displacement = static_cast<std::int64_t>(target) - after;
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    output[0] = std::byte{0xE8};
    const auto relative = static_cast<std::int32_t>(displacement);
    std::memcpy(output.data() + 1, &relative, sizeof(relative));
    return true;
}

bool ReplaceBytes(
    void* target,
    const std::span<const std::byte> replacement,
    const std::span<const std::byte> expectedCurrent,
    std::string& error) {
    std::vector<std::byte> current(expectedCurrent.size());
    FrozenThreads frozen;
    if (!frozen.Freeze(error)) {
        return false;
    }

    const char* failure = nullptr;
    if (!frozen.InstructionPointersOutside(target, replacement.size())) {
        failure =
            "a suspended thread is inside the patch prefix or its context is unavailable";
    }
    if (!crash_guard::TryCopy(current.data(), target, current.size()) ||
        !std::equal(current.begin(), current.end(), expectedCurrent.begin())) {
        failure = "target changed before transactional patch";
    }

    DWORD oldProtection = 0;
    if (failure == nullptr &&
        !VirtualProtect(
            target,
            replacement.size(),
            PAGE_EXECUTE_READWRITE,
            &oldProtection)) {
        failure = "VirtualProtect(PAGE_EXECUTE_READWRITE) failed";
    }

    if (failure == nullptr) {
        std::memcpy(target, replacement.data(), replacement.size());
        FlushInstructionCache(GetCurrentProcess(), target, replacement.size());

        DWORD ignored = 0;
        if (!VirtualProtect(target, replacement.size(), oldProtection, &ignored)) {
            // Fail closed: restore the pre-patch bytes while the page is still
            // writable. A second protection restore is best-effort, but no
            // caller may free a trampoline while replacement bytes remain.
            std::memcpy(target, expectedCurrent.data(), expectedCurrent.size());
            FlushInstructionCache(
                GetCurrentProcess(),
                target,
                expectedCurrent.size());
            VirtualProtect(
                target,
                expectedCurrent.size(),
                oldProtection,
                &ignored);
            failure = "VirtualProtect(restore) failed; bytes were rolled back";
        }
    }

    frozen.ResumeNow();
    if (failure != nullptr) {
        error = failure;
        return false;
    }
    return true;
}

} // namespace

struct HookManager::Hook {
    std::string name;
    void* target{};
    void* detour{};
    void* trampoline{};
    std::vector<std::byte> original;
    std::vector<std::byte> installed;
    void** trampolineOut{};
    std::atomic<void*>* publishedTrampolineOut{};
};

HookManager::HookManager(
    const bool exactBuildValidated,
    const bool explicitOptIn) noexcept
    : exactBuildValidated_(exactBuildValidated), explicitOptIn_(explicitOptIn) {}

HookManager::~HookManager() {
    try {
        std::string ignored;
        UninstallAll(ignored);
        // No generic hook manager can prove detour quiescence. Retired
        // trampolines intentionally remain allocated unless the owner called
        // ReleaseRetiredTrampolines with a demonstrated zero active count.
        for (auto& hook : retired_) {
            hook.release();
        }
        for (auto& hook : hooks_) {
            hook.release();
        }
    } catch (...) {
        for (auto& hook : retired_) {
            hook.release();
        }
        for (auto& hook : hooks_) {
            hook.release();
        }
    }
}

bool HookManager::Install(const HookRequest& request, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!MutationAllowed()) {
        error = "hook mutation requires a validated AI.exe target and explicit opt-in";
        return false;
    }
#if !defined(_M_IX86)
    error = "HookManager supports MSVC x86 only";
    return false;
#endif
    if (request.target == nullptr ||
        request.detour == nullptr ||
        request.trampolineOut == nullptr ||
        request.patchLength < 5 ||
        request.patchLength > 32 ||
        !request.relocationSafePrefix ||
        !request.expected.IsValid() ||
        request.expected.bytes.size() < request.patchLength) {
        error = "invalid or non-certified hook request";
        return false;
    }
    if (std::any_of(hooks_.begin(), hooks_.end(), [&](const auto& hook) {
            return hook->target == request.target;
        })) {
        error = "target is already hooked";
        return false;
    }

    std::vector<std::byte> signature(request.expected.bytes.size());
    if (!crash_guard::TryCopy(
            signature.data(),
            request.target,
            signature.size()) ||
        !request.expected.Matches(signature)) {
        error = "target bytes do not match the build profile";
        return false;
    }

    auto hook = std::make_unique<Hook>();
    hook->name = request.name;
    hook->target = request.target;
    hook->detour = request.detour;
    hook->trampolineOut = request.trampolineOut;
    hook->publishedTrampolineOut = request.publishedTrampolineOut;
    hook->original.resize(request.patchLength);
    if (!crash_guard::TryCopy(
            hook->original.data(),
            request.target,
            hook->original.size())) {
        error = "unable to copy target prefix";
        return false;
    }

    const auto trampolineBytes = request.patchLength + 5;
    hook->trampoline = VirtualAlloc(
        nullptr,
        trampolineBytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (hook->trampoline == nullptr) {
        error = "VirtualAlloc trampoline failed";
        return false;
    }
    std::memcpy(
        hook->trampoline,
        hook->original.data(),
        hook->original.size());

    std::array<std::byte, 5> jumpBack{};
    const auto* returnAddress =
        static_cast<const std::byte*>(request.target) + request.patchLength;
    auto* trampolineTail =
        static_cast<std::byte*>(hook->trampoline) + request.patchLength;
    if (!MakeRelativeJump(trampolineTail, returnAddress, jumpBack)) {
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
        error = "trampoline return is outside rel32 range";
        return false;
    }
    std::memcpy(trampolineTail, jumpBack.data(), jumpBack.size());

    DWORD ignored = 0;
    if (!VirtualProtect(
            hook->trampoline,
            trampolineBytes,
            PAGE_EXECUTE_READ,
            &ignored)) {
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
        error = "VirtualProtect trampoline failed";
        return false;
    }
    FlushInstructionCache(
        GetCurrentProcess(),
        hook->trampoline,
        trampolineBytes);

    hook->installed.assign(request.patchLength, std::byte{0x90});
    std::array<std::byte, 5> jumpToDetour{};
    if (!MakeRelativeJump(request.target, request.detour, jumpToDetour)) {
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
        error = "detour is outside rel32 range";
        return false;
    }
    std::copy(
        jumpToDetour.begin(),
        jumpToDetour.end(),
        hook->installed.begin());

    // Publish the trampoline before process threads are resumed by ReplaceBytes.
    *request.trampolineOut = hook->trampoline;
    if (request.publishedTrampolineOut != nullptr) {
        request.publishedTrampolineOut->store(
            hook->trampoline,
            std::memory_order_release);
    }
    if (!ReplaceBytes(
            request.target,
            hook->installed,
            hook->original,
            error)) {
        *request.trampolineOut = nullptr;
        if (request.publishedTrampolineOut != nullptr) {
            request.publishedTrampolineOut->store(
                nullptr,
                std::memory_order_release);
        }
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
        return false;
    }

    hooks_.push_back(std::move(hook));
    return true;
}

bool HookManager::InstallCallRedirect(
    const CallRedirectRequest& request,
    std::string& error) {
    std::lock_guard lock(mutex_);
    if (!MutationAllowed()) {
        error = "call redirection requires a validated AI.exe target and explicit opt-in";
        return false;
    }
#if !defined(_M_IX86)
    error = "HookManager direct-CALL redirection supports MSVC x86 only";
    return false;
#endif
    if (request.callSite == nullptr ||
        request.expectedTarget == nullptr ||
        request.detour == nullptr) {
        error = "invalid direct-CALL redirect request";
        return false;
    }
    if (std::any_of(hooks_.begin(), hooks_.end(), [&](const auto& hook) {
            return hook->target == request.callSite;
        })) {
        error = "direct-CALL site is already patched";
        return false;
    }

    std::array<std::byte, 5> original{};
    if (!crash_guard::TryCopy(original.data(), request.callSite, original.size()) ||
        std::to_integer<std::uint8_t>(original[0]) != 0xE8U) {
        error = "direct-CALL site is not a readable E8 rel32 instruction";
        return false;
    }
    std::int32_t originalDisplacement = 0;
    std::memcpy(&originalDisplacement, original.data() + 1U, sizeof(originalDisplacement));
    const auto callSite = reinterpret_cast<std::uintptr_t>(request.callSite);
    const auto originalTarget = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(callSite + 5U) + originalDisplacement);
    if (originalTarget != reinterpret_cast<std::uintptr_t>(request.expectedTarget)) {
        error = "direct-CALL destination does not match the certified retail target";
        return false;
    }

    std::array<std::byte, 5> replacement{};
    if (!MakeRelativeCall(request.callSite, request.detour, replacement)) {
        error = "direct-CALL detour is outside rel32 range";
        return false;
    }

    auto hook = std::make_unique<Hook>();
    hook->name = request.name;
    hook->target = request.callSite;
    hook->detour = request.detour;
    hook->original.assign(original.begin(), original.end());
    hook->installed.assign(replacement.begin(), replacement.end());
    if (!ReplaceBytes(
            request.callSite,
            hook->installed,
            hook->original,
            error)) {
        return false;
    }

    hooks_.push_back(std::move(hook));
    return true;
}

bool HookManager::UninstallAll(std::string& error) {
    return UninstallFrom(0, error);
}

bool HookManager::UninstallFrom(
    const std::size_t installedCount,
    std::string& error) {
    std::lock_guard lock(mutex_);
    if (installedCount > hooks_.size()) {
        error = "hook rollback checkpoint exceeds the installed hook count";
        return false;
    }
    retired_.reserve(retired_.size() + hooks_.size() - installedCount);
    while (hooks_.size() > installedCount) {
        auto& hook = *hooks_.back();
        std::vector<std::byte> current(hook.installed.size());
        if (!crash_guard::TryCopy(current.data(), hook.target, current.size()) ||
            current != hook.installed) {
            error = "hook target changed externally; active hook and trampoline retained";
            return false;
        }
        if (!ReplaceBytes(
                hook.target,
                hook.original,
                hook.installed,
                error)) {
            return false;
        }
        retired_.push_back(std::move(hooks_.back()));
        hooks_.pop_back();
    }
    return true;
}

bool HookManager::ReleaseRetiredTrampolines(
    const bool detoursQuiescent,
    std::string& error) {
    std::lock_guard lock(mutex_);
    if (!hooks_.empty()) {
        error = "cannot release trampolines while a target prologue is still hooked";
        return false;
    }
    if (!detoursQuiescent) {
        error = "detour quiescence was not demonstrated";
        return false;
    }
    for (auto& hook : retired_) {
        if (hook->trampolineOut != nullptr) {
            *hook->trampolineOut = nullptr;
        }
        if (hook->publishedTrampolineOut != nullptr) {
            hook->publishedTrampolineOut->store(
                nullptr,
                std::memory_order_release);
        }
        if (hook->trampoline != nullptr) {
            VirtualFree(hook->trampoline, 0, MEM_RELEASE);
            hook->trampoline = nullptr;
        }
    }
    retired_.clear();
    return true;
}

std::size_t HookManager::InstalledCount() const noexcept {
    std::lock_guard lock(mutex_);
    return hooks_.size();
}

std::size_t HookManager::RetiredCount() const noexcept {
    std::lock_guard lock(mutex_);
    return retired_.size();
}

bool HookManager::MutationAllowed() const noexcept {
    return exactBuildValidated_ && explicitOptIn_;
}

} // namespace aimp
