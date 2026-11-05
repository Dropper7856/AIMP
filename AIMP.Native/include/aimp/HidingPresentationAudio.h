#pragma once

#include "aimp/PlayerHidingPresentation.h"
#include <cstdint>
#include <string>

namespace aimp { class HookManager; }
namespace aimp::hiding_presentation_audio {

struct AuthoredEvent final { const char* name{}; std::uint32_t animation{}; };
[[nodiscard]] constexpr AuthoredEvent Selection(std::uint32_t kind, std::uint32_t phase) noexcept {
    if (kind == 1U && phase == 1U) return {"play_hiding_locker_in", 0xCF2A9F36U};
    if (kind == 1U && phase == 3U) return {"play_hiding_locker_out", 0x5E642560U};
    if (kind == 2U && phase == 1U) return {"play_hiding_locker_low_in", 0x80645982U};
    if (kind == 2U && phase == 3U) return {"play_hiding_locker_low_out", 0x3FDD3AF4U};
    return {};
}

bool Initialize(std::uintptr_t moduleBase, std::string& error) noexcept;
// Redirects only the type9 metadata consumer CALL at 0079A455. Successful
// explicit posts suppress only that same event on the exact admitted remote
// cycle. Other actors, animations and metadata retain the original consumer.
bool InstallMetadataFilter(HookManager&, std::string& error);
// Engine thread, after successful remote scripted-clip admission. Caller owns
// the Character lifetime. effectiveAge includes network age and local waiting.
// Consumes each revision once, including baseline/stale/failed attempts. A true
// result means native Play returned a playing ID, not proof of audibility.
bool PlayOnEngineThread(std::uintptr_t character,
    const player_hiding_presentation::State&, std::uint64_t effectiveAgeMilliseconds) noexcept;
// Value-only cleanup. Native Play owns each transient emitter through its End
// callback; no native pointers/references are retained or released here.
void Forget(std::uintptr_t character) noexcept;
void ForgetAll() noexcept;

struct DiagnosticsSnapshot final {
    std::uint64_t transition{}, attempted{}, posted{}, failed{}, duplicate{}, stale{}, baseline{};
    std::uint64_t metadataObserved{}, metadataSuppressed{};
    std::uint32_t phase{}, kind{}, eventId{}, playingId{}, exceptionCode{};
    std::uintptr_t exceptionAddress{};
    float metadataWeight{}, metadataThreshold{};
    bool initialized{}, filterInstalled{}, usedRetailEmitterPose{};
    const char* eventName{""};
    const char* stage{"pending"};
};
[[nodiscard]] DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept;
}
