#include "aimp/ParticipantRegistry.h"

#include "aimp/CrashGuard.h"
#include "aimp/NativeLimits.h"

namespace aimp {
namespace {

constexpr std::size_t kEntryStride = 0x158;
constexpr std::size_t kTablePointerOffset = 0xAC0;
constexpr std::size_t kCountOffset = 0xAC4;

template <typename T>
bool ReadAt(const std::uintptr_t base, const std::size_t offset, T& output) noexcept {
    const auto value = crash_guard::SafeRead<T>(
        reinterpret_cast<const void*>(base + offset));
    if (!value.has_value()) {
        return false;
    }
    output = *value;
    return true;
}

} // namespace

bool ParticipantRegistry::Inspect(
    const std::uintptr_t ownerAddress,
    ParticipantTableSnapshot& output) const noexcept {
    try {
        output = {};
        output.owner = ownerAddress;
        if (ownerAddress == 0 ||
            !ReadAt(ownerAddress, kTablePointerOffset, output.table) ||
            !ReadAt(ownerAddress, kCountOffset, output.count)) {
            output.rejectionReason = "owner_or_header_unreadable";
            return false;
        }
        if (output.count > kNativeParticipantCapacity) {
            output.rejectionReason = "participant_count_exceeds_static_capacity_8";
            return false;
        }
        if (output.table != ownerAddress) {
            output.rejectionReason = "inline_table_self_pointer_not_observed";
            return false;
        }
        if (output.count != 0 &&
            !crash_guard::IsReadable(
                reinterpret_cast<const void*>(output.table),
                output.count * kEntryStride)) {
            output.rejectionReason = "participant_entries_unreadable";
            return false;
        }

        output.participants.reserve(output.count);
        for (std::uint32_t index = 0; index < output.count; ++index) {
            const auto entry = output.table + index * kEntryStride;
            ParticipantSnapshot participant{};
            participant.index = index;
            if (!crash_guard::TryCopy(
                    participant.identity128.data(),
                    reinterpret_cast<const void*>(entry + 0x80),
                    participant.identity128.size()) ||
                !ReadAt(entry, 0xC8, participant.flagsC8) ||
                !ReadAt(entry, 0xD0, participant.associatedObjectD0) ||
                !ReadAt(entry, 0xD4, participant.associatedValueD4) ||
                !ReadAt(entry, 0xF4, participant.keyF4)) {
                output.rejectionReason = "participant_entry_unreadable";
                output.participants.clear();
                return false;
            }
            output.participants.push_back(participant);
        }

        std::uintptr_t tableAfter = 0;
        std::uint32_t countAfter = 0;
        if (!ReadAt(ownerAddress, kTablePointerOffset, tableAfter) ||
            !ReadAt(ownerAddress, kCountOffset, countAfter)) {
            output.rejectionReason = "participant_header_reread_failed";
            output.participants.clear();
            return false;
        }
        output.consistent =
            tableAfter == output.table &&
            countAfter == output.count;
        if (output.consistent) {
            for (std::uint32_t index = 0; index < output.count; ++index) {
                const auto entry = output.table + index * kEntryStride;
                ParticipantSnapshot reread{};
                if (!crash_guard::TryCopy(
                        reread.identity128.data(),
                        reinterpret_cast<const void*>(entry + 0x80),
                        reread.identity128.size()) ||
                    !ReadAt(entry, 0xC8, reread.flagsC8) ||
                    !ReadAt(entry, 0xD0, reread.associatedObjectD0) ||
                    !ReadAt(entry, 0xD4, reread.associatedValueD4) ||
                    !ReadAt(entry, 0xF4, reread.keyF4)) {
                    output.consistent = false;
                    break;
                }
                const auto& observed = output.participants[index];
                if (reread.identity128 != observed.identity128 ||
                    reread.flagsC8 != observed.flagsC8 ||
                    reread.associatedObjectD0 != observed.associatedObjectD0 ||
                    reread.associatedValueD4 != observed.associatedValueD4 ||
                    reread.keyF4 != observed.keyF4) {
                    output.consistent = false;
                    break;
                }
            }
        }
        if (!output.consistent) {
            output.rejectionReason =
                "participant_data_changed_during_snapshot";
            output.participants.clear();
            return false;
        }
        output.valid = true;
        return true;
    } catch (...) {
        output.rejectionReason = "participant_snapshot_allocation_failure";
        output.participants.clear();
        return false;
    }
}

} // namespace aimp
