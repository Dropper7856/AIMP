#include "aimp/AnimationSyncPropsRuntime.h"
#include "aimp/HookManager.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace props = aimp::animation_sync::props;
constexpr std::size_t kImageBytes = 0x520000U;
constexpr std::uintptr_t kTransformRva = 0xBE2B0U;
constexpr std::uintptr_t kSentinelRva = 0x1000U;

void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// The certified twelve-byte prologue followed by a synthetic thiscall body:
// write 10.0f to matrix[0], return matrix, restore ESP and pop both arguments.
// The write distinguishes a real trampoline call from the detour's null-original
// fallback, which also returns matrix. No retail engine instructions are run.
constexpr std::array<std::uint8_t, 27> kTransformCode{
    0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x74,0x01,0x00,0x00,
    0x8B,0x45,0x08,0xC7,0x00,0x00,0x00,0x20,0x41,0x8B,0xE5,0x5D,0xC2,0x08,0x00};
constexpr std::array<std::uint8_t, 6> kSentinelCode{0xB8,0x2A,0x00,0x00,0x00,0xC3};

struct Image final {
    std::uint8_t* memory = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, kImageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    bool mayRelease{true};
    ~Image() {
        // If rollback failed, a hook may still point into this image. Keep it
        // allocated until process exit rather than freeing executable storage.
        if (memory != nullptr && mayRelease) VirtualFree(memory, 0U, MEM_RELEASE);
    }
    std::uintptr_t Base() const noexcept { return reinterpret_cast<std::uintptr_t>(memory); }
};

struct ScopedHooks final {
    Image& image;
    aimp::HookManager hooks;
    ScopedHooks(Image& value, const bool exactBuild, const bool optIn)
        : image(value), hooks(exactBuild, optIn) {}
    ~ScopedHooks() {
        try {
            props::BeginLogicalShutdown();
            std::string error;
            if (!hooks.UninstallAll(error) ||
                !hooks.ReleaseRetiredTrampolines(props::ActiveDetours() == 0U, error)) {
                image.mayRelease = false;
                std::cerr << "Hook cleanup failed: " << error << '\n';
            }
        } catch (...) {
            image.mayRelease = false;
            std::cerr << "Hook cleanup failed; retaining the synthetic image.\n";
        }
    }
};

void Prepare(Image& image) {
    Require(image.memory != nullptr, "Synthetic image allocation failed");
    const auto copy = [&](const std::uintptr_t rva, const std::initializer_list<std::uint8_t> bytes) {
        std::memcpy(image.memory + rva, bytes.begin(), bytes.size());
    };
    std::memcpy(image.memory + kTransformRva, kTransformCode.data(), kTransformCode.size());
    std::memcpy(image.memory + kSentinelRva, kSentinelCode.data(), kSentinelCode.size());
    copy(0xD6690U, {0x6A,0xFF,0x64,0xA1,0x00,0x00,0x00,0x00});
    copy(0xD72A0U, {0x83,0xEC,0x30,0x53,0x8B,0x5C,0x24,0x38});
    copy(0x4F62C0U, {0x55,0x8B,0xEC,0x6A,0xFF});
    copy(0x4C0F50U, {0x55,0x8B,0xEC,0x6A,0xFF});
    copy(0xA1D0U, {0x8B,0x44,0x24,0x04,0x56,0x8B,0xF1});
    copy(0x673C0U, {0x55,0x8B,0x6C,0x24,0x08,0x56});
    copy(0x51E1B0U, {0x56,0x68});
    copy(0x51E1D0U, {0x68});
    Require(FlushInstructionCache(GetCurrentProcess(), image.memory, kImageBytes) != FALSE,
        "Synthetic instruction cache flush failed");
}

int __cdecl SentinelDetour() { return 7; }
using Sentinel = int(__cdecl*)();
using Transform = float*(__thiscall*)(void*, float*, const std::uint32_t*);

void CheckTransform(const Image& image) {
    std::array<float, 12> matrix{};
    matrix[0] = -1.0F;
    matrix[1] = 25.0F;
    const auto transform = reinterpret_cast<Transform>(image.Base() + kTransformRva);
    // No local Character is registered. Observation returns before any engine
    // object access, while the actual production detour calls its trampoline.
    Require(transform(nullptr, matrix.data(), nullptr) == matrix.data() &&
        matrix[0] == 10.0F && matrix[1] == 25.0F,
        "Attachment detour did not preserve the transform call and result");
    Require(props::ActiveDetours() == 0U, "Attachment detour did not leave its active scope");
}

void Run(Image& image) {
    // These outputs outlive the HookManager, including every cleanup path.
    void* sentinelRaw{};
    std::atomic<void*> sentinelPublished{};
    ScopedHooks owner(image, true, true);
    auto& hooks = owner.hooks;
    std::string error;
    const auto sentinel = reinterpret_cast<Sentinel>(image.Base() + kSentinelRva);
    Require(sentinel() == 42, "Synthetic sentinel did not return its original value");
    CheckTransform(image);

    aimp::HookRequest request{};
    request.name = "animation_props_test_existing_hook";
    request.target = image.memory + kSentinelRva;
    request.detour = reinterpret_cast<void*>(&SentinelDetour);
    request.patchLength = 5U;
    request.expected = {{0xB8,0x2A,0x00,0x00,0x00,0xC3}, "xxxxxx"};
    request.relocationSafePrefix = true;
    request.trampolineOut = &sentinelRaw;
    request.publishedTrampolineOut = &sentinelPublished;
    Require(std::string(props::BuildTag()) == "AIMP-SYNCFIX2-20261108",
        "Wrong AnimationSync implementation linked into the test");

    // Reproduce the original atomic-only request and validate every contract
    // rejection without modifying the target or publishing a trampoline.
    const auto reject = [&](const aimp::HookRequest& invalid, const char* reason) {
        error.clear();
        Require(!hooks.Install(invalid, error) && error.find(reason) != std::string::npos,
            std::string("Expected detailed hook rejection: ") + reason + "; got: " + error);
        Require(hooks.InstalledCount() == 0U && sentinel() == 42 && sentinelRaw == nullptr &&
            sentinelPublished.load(std::memory_order_acquire) == nullptr,
            "Rejected request modified the target or trampoline outputs");
    };
    auto invalid = request;
    invalid.trampolineOut = nullptr;
    reject(invalid, "trampolineOut=null");
    invalid = request;
    invalid.target = nullptr;
    reject(invalid, "target=null");
    invalid = request;
    invalid.detour = nullptr;
    reject(invalid, "detour=null");
    invalid = request;
    invalid.patchLength = 4U;
    reject(invalid, "patchLength=4");
    invalid = request;
    invalid.patchLength = 33U;
    reject(invalid, "patchLength=33");
    invalid = request;
    invalid.relocationSafePrefix = false;
    reject(invalid, "relocationSafePrefix=false");
    invalid = request;
    invalid.expected.mask = "xxxxx";
    reject(invalid, "invalid expected bytes/mask");
    invalid = request;
    invalid.expected.mask = "xxxxxz";
    reject(invalid, "invalid expected bytes/mask");
    invalid = request;
    invalid.patchLength = 7U;
    reject(invalid, "expected signature shorter than patchLength");
    error.clear();
    const auto sentinelInstalled = hooks.Install(request, error);
    Require(sentinelInstalled, "Sentinel installation failed: " + error);
    const auto checkpoint = hooks.InstalledCount();

    const auto checkSentinel = [&] {
        Require(checkpoint == 1U && hooks.InstalledCount() == checkpoint && sentinel() == 7 &&
            sentinelRaw != nullptr && sentinelPublished.load(std::memory_order_acquire) == sentinelRaw &&
            reinterpret_cast<Sentinel>(sentinelRaw)() == 42,
            "Attachment installation or rollback changed the pre-existing hook");
    };
    const auto checkOriginal = [&] {
        Require(std::memcmp(image.memory + kTransformRva,
            kTransformCode.data(), kTransformCode.size()) == 0,
            "Attachment instructions were changed or not fully restored");
        CheckTransform(image);
    };

    for (const bool exactBuild : {false, true}) {
        // Test each mandatory authorization independently, with certified bytes.
        ScopedHooks denied(image, exactBuild, !exactBuild);
        error.clear();
        Require(!props::Install(denied.hooks, image.Base(), error) && !error.empty() &&
            denied.hooks.InstalledCount() == 0U,
            "Attachment hook accepted an uncertified build or missing opt-in");
        checkSentinel();
        checkOriginal();
    }

    // Both the target prologue and an auxiliary ABI must remain certified.
    for (const auto rva : {kTransformRva, std::uintptr_t{0xD6690U}}) {
        const auto original = image.memory[rva];
        image.memory[rva] = 0x90U;
        error.clear();
        const auto accepted = props::Install(hooks, image.Base(), error);
        image.memory[rva] = original;
        Require(FlushInstructionCache(GetCurrentProcess(), image.memory + rva, 1U) != FALSE,
            "Restored instruction cache flush failed");
        Require(!accepted && !error.empty(), "Attachment hook accepted an uncertified signature");
        checkSentinel();
        checkOriginal();
    }

    // Exercise real installation, atomic publication, detour pass-through and
    // retirement twice. This also covers output storage surviving Install().
    for (unsigned iteration = 0U; iteration < 2U; ++iteration) {
        error.clear();
        const auto installed = props::Install(hooks, image.Base(), error);
        Require(installed, "Attachment hook installation failed: " + error);
        Require(hooks.InstalledCount() == checkpoint + 1U &&
            image.memory[kTransformRva] == 0xE9U && image.memory[kTransformRva + 5U] == 0x90U,
            "Attachment installation did not add its six-byte detour");
        CheckTransform(image);
        props::BeginLogicalShutdown();
        const auto restored = hooks.UninstallFrom(checkpoint, error);
        Require(restored, "Attachment rollback failed: " + error);
        Require(hooks.RetiredCount() == iteration + 1U,
            "Rollback did not retain each attachment trampoline");
        checkSentinel();
        checkOriginal();
        Require(!hooks.ReleaseRetiredTrampolines(true, error) &&
            hooks.RetiredCount() == iteration + 1U,
            "Retirement must remain deferred while the sentinel is installed");
        checkSentinel();
    }
    error.clear();
    const auto restored = hooks.UninstallAll(error);
    Require(restored, "Final hook rollback failed: " + error);
    Require(hooks.InstalledCount() == 0U && hooks.RetiredCount() == 3U && sentinel() == 42,
        "Final rollback did not restore the sentinel and retain all trampolines");
    const auto released = hooks.ReleaseRetiredTrampolines(props::ActiveDetours() == 0U, error);
    Require(released, "Final trampoline release failed: " + error);
    Require(hooks.RetiredCount() == 0U && sentinelRaw == nullptr &&
        sentinelPublished.load(std::memory_order_acquire) == nullptr,
        "Final release did not clear persistent trampoline outputs");
    checkOriginal();
}
}

int main() {
    Image image;
    try {
        Prepare(image);
        Run(image);
        Require(image.mayRelease, "Hook cleanup could not prove safe image release");
        std::cout << "AnimationSync attachment hook installation tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
