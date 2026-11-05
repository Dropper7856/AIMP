#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace aimp::player_hiding {
// Launcher form: "0" visible, "1,x,y,z" hidden at that hiding-spot anchor.
bool Decode(std::string_view text, bool& hidden, std::array<float, 3>& anchor) noexcept;
bool ValidAnchor(const std::array<float, 3>& anchor) noexcept;
void Initialize(std::uintptr_t moduleBase) noexcept;
// IPC only publishes state. Native objects are accessed by the engine pump.
bool Publish(std::uintptr_t character, bool hidden, const std::array<float, 3>& anchor,
    std::string& error) noexcept;
// True while the owner is hidden: the locomotion driver must not walk,
// teleport or turn the representation.
bool Hidden(std::uintptr_t character) noexcept;
// Engine thread: conceal/reveal representations and mark their hiding spots
// occupied (non-selectable) or free on this PC.
void PumpAllOnEngineThread() noexcept;
// Character destroyed/replaced: drop its state; its hiding spot is freed by
// the next engine pump.
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;
struct DiagnosticsSnapshot final {
    std::uint64_t hides{}, shows{}, failures{};
    std::uint64_t spotsLocked{}, spotsReleased{}, spotsMissing{};
    bool desired{}, concealed{}, spotLocked{};
    std::array<float, 3> anchor{};
    // Distance from the anchor to the locked hiding interaction (tuning aid).
    float spotDistance{-1.0F};
    const char* stage{"pending"};
};
DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;
// Cumulative since Initialize, including actors whose per-stay mailbox has
// already been released. Zero counters in Snapshot alone are not history.
struct HistorySnapshot final {
    std::uint64_t hiddenEdges{}, visibleEdges{}, hides{}, shows{}, failures{};
    std::uintptr_t lastHiddenRequestedCharacter{}, lastHiddenCharacter{}, lastShownCharacter{};
};
HistorySnapshot History() noexcept;
}
