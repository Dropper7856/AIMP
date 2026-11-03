#pragma once
#include <cstdint>

namespace aimp::world_actions {
// These primitive types consume one process's input/HUD/interaction state.
// They are not shared world consequences, even inside a World composite.
constexpr bool ReceiverLocalPlayerControlPrimitive(std::uint32_t type) noexcept {
    return type == 0x1728F297U || type == 0x324DBAB5U ||
        type == 0x73E92EC1U || type == 0x71E671A8U;
}
constexpr bool SuppressRemotePlayerControl(std::uint32_t type, bool remote) noexcept {
    return remote && ReceiverLocalPlayerControlPrimitive(type);
}
}
