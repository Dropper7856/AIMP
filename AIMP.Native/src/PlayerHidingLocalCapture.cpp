#include "aimp/PlayerHidingLocalCapture.h"
#include "aimp/PlayerHidingPresentationRuntime.h"
#include <Windows.h>
#include <mutex>

namespace aimp::player_hiding_local {
namespace { std::mutex gMutex; Snapshot gSnapshot{}; DiagnosticsSnapshot gDiagnostics{}; }
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
    if (!capture.valid) ++gDiagnostics.unavailable;
    else if (!gSnapshot.valid || gSnapshot.ticket != capture.ticket ||
        gSnapshot.state.phase != capture.state.phase || gSnapshot.state.transition != capture.state.transition) {
        if (capture.state.phase == 1) ++gDiagnostics.entering;
        else if (capture.state.phase == 2) ++gDiagnostics.hidden;
        else if (capture.state.phase == 3) ++gDiagnostics.exiting;
        else ++gDiagnostics.visible;
    }
    gSnapshot = capture;
}
Snapshot Read() noexcept { std::scoped_lock lock(gMutex); return gSnapshot; }
DiagnosticsSnapshot Diagnostics() noexcept { std::scoped_lock lock(gMutex); return gDiagnostics; }
void ForgetAll() noexcept { std::scoped_lock lock(gMutex); gSnapshot = {}; }
}
