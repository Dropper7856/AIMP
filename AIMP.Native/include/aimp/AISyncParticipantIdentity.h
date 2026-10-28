#pragma once
#include <cstdint>
#include "aimp/CharacterIncarnation.h"

namespace aimp::ai_sync {
#pragma pack(push,1)
// Local IPC only. The incarnation is portable; the remaining identity fields
// attest which exact native construction represents it in THIS process.
struct ParticipantBinding final {
    std::uint64_t peer{};
    std::uint32_t character{}, incarnation{}, runtimeId{}, reserved{};
    std::uint64_t creationSerial{};
    bool operator==(const ParticipantBinding&) const noexcept=default;
};
#pragma pack(pop)
static_assert(sizeof(ParticipantBinding)==32U);
[[nodiscard]] constexpr bool ValidParticipantBinding(const ParticipantBinding& value) noexcept {
    if(value.peer==0U)return value==ParticipantBinding{};
    if(value.character<0x10000U||value.reserved!=0U)return false;
    if(value.incarnation==0U)return value.runtimeId==0U&&value.creationSerial==0U;
    return value.runtimeId!=0U&&value.creationSerial!=0U&&(value.character&15U)==0U&&value.character<=0xFFFFFFAFU;
}
// Value comparison only: the caller separately pins/revalidates the current
// registry/world and actor. Matching bytes alone never admit an interaction.
[[nodiscard]] constexpr bool MatchesParticipantTicket(const ParticipantBinding& binding,
    const character_incarnations::Ticket& ticket,std::uint64_t generation,bool local) noexcept {
    return ValidParticipantBinding(binding)&&binding.incarnation!=0U&&generation!=0U&&
        ticket.generation==generation&&ticket.character==binding.character&&ticket.runtimeId==binding.runtimeId&&
        ticket.serial==binding.creationSerial&&(ticket.origin==character_incarnations::Origin::Created||
            (local&&ticket.origin==character_incarnations::Origin::ExistingAtBinding));
}
// The zero-incarnation case is explicitly legacy. Strict executors must
// additionally require expectedIncarnation!=0 before using this comparison.
[[nodiscard]] constexpr bool MatchesParticipantTarget(std::uint64_t expectedPeer,std::uint32_t expectedIncarnation,
    std::uint64_t actualPeer,std::uint32_t actualIncarnation) noexcept {
    return expectedPeer!=0U&&expectedPeer==actualPeer&&
        (expectedIncarnation==0U||expectedIncarnation==actualIncarnation);
}
}
