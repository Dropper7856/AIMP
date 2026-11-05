#pragma once
#include "aimp/CharacterIncarnation.h"
#include "aimp/PlayerHidingPresentation.h"

namespace aimp::player_hiding_local {
struct Snapshot final {
    character_incarnations::Ticket ticket{};
    player_hiding_presentation::State state{};
    std::uint64_t capturedAt{};
    bool valid{};
};
void PumpOnEngineThread() noexcept;
Snapshot Read() noexcept;
struct DiagnosticsSnapshot final { std::uint64_t entering{}, hidden{}, exiting{}, visible{}, unavailable{}; };
DiagnosticsSnapshot Diagnostics() noexcept;
void ForgetAll() noexcept;
}
