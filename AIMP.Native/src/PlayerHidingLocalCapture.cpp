#include "aimp/PlayerHidingLocalCapture.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include <Windows.h>
#include <mutex>

namespace aimp::player_hiding_local {
namespace { std::mutex gMutex; Snapshot gSnapshot{}; }
void PumpOnEngineThread() noexcept {
    auto& registry = character_incarnations::NativeRegistry();
    const auto ticket = registry.Local();
    Snapshot capture{};
    if (ticket.character != 0 && registry.Current(ticket)) {
        capture.ticket = ticket;
        capture.valid = player_hiding_presentation::CaptureLocalOnEngineThread(ticket.character, capture.state)
            && registry.Current(ticket) && registry.Local() == ticket;
        capture.capturedAt = GetTickCount64();
    }
    std::scoped_lock lock(gMutex);
    gSnapshot = capture;
}
Snapshot Read() noexcept { std::scoped_lock lock(gMutex); return gSnapshot; }
void ForgetAll() noexcept { std::scoped_lock lock(gMutex); gSnapshot = {}; }
}
