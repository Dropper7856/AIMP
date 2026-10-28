#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace aimp::character_incarnations {
// Local construction evidence only. Never transmit these addresses/serials as
// a participant's portable incarnation; that is allocated by its session owner.
enum class Origin : std::uint8_t { Created, ExistingAtBinding };
struct Ticket {
    std::uint64_t generation{},serial{};
    std::uintptr_t character{};
    std::uint32_t runtimeId{};
    Origin origin{Origin::Created};
    bool operator==(const Ticket&) const noexcept=default;
};
enum class Result : std::uint8_t { Ok, Duplicate, Invalid, WrongGeneration, Missing, Capacity, Uncertain };
// Hook/engine/IPC threads exchange values only. No native code, reads or
// reference releases run while holding its mutex. Retired address slots can be
// reused only by a new constructor at that address. Their tombstones persist
// otherwise; serials never repeat, including across native world changes.
class Registry final {
public:
    static constexpr std::size_t kCapacity=512;
    Registry() noexcept=default;
    Registry(const Registry&)=delete;
    Registry& operator=(const Registry&)=delete;
    // A real successful constructor return, not a polling observation. Even
    // identical pointer/runtime values issue a fresh ticket and revoke the old.
    [[nodiscard]] Result Created(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtimeId,Ticket&) noexcept;
    // Qualified initial attachment to an already existing local actor. This
    // cannot certify a remote spawn result as newly created. Duplicates retain
    // the same ticket; it never upgrades ExistingAtBinding to Created.
    [[nodiscard]] Result BindExisting(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtimeId,Ticket&) noexcept;
    [[nodiscard]] Result Find(std::uint64_t generation,std::uintptr_t character,Ticket&) const noexcept;
    [[nodiscard]] bool Current(const Ticket&) const noexcept;
    // Engine publishes the exact THE_PLAYER association. Readers receive only
    // a still-current ticket; they never resolve a new actor from an old one.
    [[nodiscard]] Result PublishLocal(const Ticket&) noexcept;
    [[nodiscard]] Ticket Local() const noexcept;
    // Capture the ticket BEFORE any destruction callback/native cleanup. An
    // old queued notification cannot retire a replacement at the same address.
    [[nodiscard]] Result Retire(const Ticket&) noexcept;
    // Explicit generation evidence only; never reset to recover a lost table.
    [[nodiscard]] Result AdvanceWorld(std::uint64_t generation) noexcept;
private:
    friend struct RegistryTestAccess;
    struct Entry {Ticket ticket{};bool active{};};
    [[nodiscard]] Result WorldLocked(std::uint64_t generation) noexcept;
    [[nodiscard]] Result IssueLocked(std::uint64_t,std::uintptr_t,std::uint32_t,Origin,Ticket&) noexcept;
    mutable std::mutex gate_;
    std::array<Entry,kCapacity> entries_{};
    std::uint64_t generation_{},serial_{};
    Ticket local_{};
    bool uncertain_{};
};
// Production singleton contains no borrowed native references and owns no
// target storage. Lifecycle hooks are its construction/destruction producers.
[[nodiscard]] Registry& NativeRegistry() noexcept;
inline constexpr std::size_t kLocalStateBytes=40;
// Local IPC only, bound to its DLL connection. A ticket is not a portable
// participant identity and can never be used as a network authority grant.
[[nodiscard]] bool EncodeLocalState(const Ticket&,std::span<std::uint8_t>) noexcept;
[[nodiscard]] bool DecodeLocalState(std::span<const std::uint8_t>,Ticket&) noexcept;
}
