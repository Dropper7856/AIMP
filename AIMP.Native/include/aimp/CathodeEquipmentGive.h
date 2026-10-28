#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace aimp {
class SignatureResolver;

namespace cathode_equipment {
// Owned native references, accessed only on the certified game thread. Keep
// these holders alive across polling: GiveToCharacter queues equipment work.
enum class Stage : std::uint8_t { Empty, WaitingOutput, GiveDispatched, Equipped, Failed };
enum class Result : std::uint8_t {
    Pending, Equipped, Unavailable, InvalidCharacter, InvalidCatalog,
    CompositeMissing, CreateFailed, FinalizeFailed, OutputMismatch,
    CharacterBridgeFailed, BindFailed, PreflightFailed, DispatchRejected,
    SlotConflict, NativeFault, TimedOut, ResourceInitializationFailed
};
struct Record final {
    std::uint64_t generation{};
    std::uint32_t runtimeId{};
    std::string compositeName;
    std::uint32_t equipmentEntityId{};
    std::uint32_t outputId{};
    std::uint32_t spawnId{};
    std::uint32_t targetInputId{};
    std::uint32_t expectedSlot{};
    std::uint32_t compositeId{};
    std::uint32_t equipmentSlot{(std::numeric_limits<std::uint32_t>::max)()};
    void* root{};
    void* composite{};
    void* weapon{};
    void* characterNode{};
    void* giveNode{};
    void* flagNode{};
    void* bindingPin{};
    void* work{};
    void* resolvedCharacter{};
    void* resolvedEquipment{};
    std::uint32_t polls{};
    std::uint32_t exceptionCode{};
    bool giveStarted{};
    bool outputQuerySucceeded{};
    bool activateHeldRequested{}, activateHeldDispatched{};
    bool releaseHeldRequested{}, releaseHeldDispatched{};
    // One-shot Give policy, never a per-update refill. The record is retained
    // after Equipped, so retrying/polling an already-given item cannot refill it.
    bool initialResourcesCompleted{};
    bool initialResourcesAttempted{};
    std::uint32_t resourcePolls{};
    std::int32_t initialResourcesBefore{-1};
    std::int32_t initialResourcesAfter{-1};
    std::int32_t nativeResourceCapacity{-1};
    const char* initialResourcePolicy{"not_observed"};
    Stage stage{Stage::Empty};
    Result result{Result::Pending};
    const char* operation{"idle"};
};

// Returns an ASLR-adjusted module base only for the exact validated retail
// profile and verified native entry points. No generic-signature fallback.
[[nodiscard]] std::uintptr_t Validate(const SignatureResolver& resolver) noexcept;
[[nodiscard]] Result Step(std::uintptr_t base, std::uintptr_t character,
    Record& record) noexcept;
[[nodiscard]] bool IsAttached(
    std::uintptr_t character,
    const Record& record) noexcept;
// Release a completed record after CharacterFullDestroyEnd (same generation),
// or a normal failure before Give. Faulted/in-flight records are quarantined.
void ReleaseCompleted(std::uintptr_t base, Record& record) noexcept;
[[nodiscard]] const char* Describe(Result result) noexcept;
} // namespace cathode_equipment
} // namespace aimp
