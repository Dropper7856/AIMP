#include "aimp/VoiceNativeAudio.h"
#include "aimp/VoiceBank.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

namespace aimp::voice_sync::native_audio {
namespace {
std::uintptr_t gBase{};
const Event kEvent{kVoiceEventId, "Play_AIMP_Voice", "", 0.0F, 0.0F};
using Subsystem = void*(__thiscall*)(void*, int);
using AllocateObject = std::uintptr_t(__thiscall*)(void*, const char*);
using NativePlay = std::uint32_t(__thiscall*)(void*, const Event*, const Parameters*, void*, void*, char);
using NativePosition = void(__thiscall*)(void*, void*, const float*, const float*, char);
using NativeEnd = void(__cdecl*)(std::uint32_t, const std::uint32_t*);
template<class T> T Function(const std::uintptr_t va) noexcept {
    return reinterpret_cast<T>(gBase + va - 0x400000U);
}
template<class T> bool Read(const std::uintptr_t at, T& value) noexcept {
    if (at < 0x10000U) return false;
    __try { std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof(value)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* Interface() noexcept {
    std::uintptr_t root{}, playback{};
    std::uint8_t ready{};
    if (!Read(gBase + 0x012B1758U, root) || root == 0U || !Read(root + 0xE5U, ready) || ready == 0U ||
        !Read(root + 0xC0U, playback)) return nullptr;
    return reinterpret_cast<void*>(playback);
}
bool Pose(const std::uintptr_t character, Parameters& parameters) noexcept {
    std::uintptr_t head{};
    if (Read(character + 0x1320U, head) && head != 0U &&
        Read(head + 0x10U, parameters.position) && Read(head + 0x20U, parameters.orientation)) {
        if (std::isfinite(parameters.position[0]) && std::isfinite(parameters.position[1]) &&
            std::isfinite(parameters.position[2])) return true;
    }
    std::array<float, 4> body{};
    if (!Read(character + 0x70U, body)) return false;
    parameters.position = body;
    return std::isfinite(body[0]) && std::isfinite(body[1]) && std::isfinite(body[2]);
}
int CapturePlayException(EXCEPTION_POINTERS* exception, PlayResult& result) noexcept {
    result.failure = PlayFailure::NativeException;
    if (exception != nullptr && exception->ExceptionRecord != nullptr) {
        result.exceptionCode = exception->ExceptionRecord->ExceptionCode;
        result.exceptionAddress = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
}
bool Initialize(const std::uintptr_t base, std::string& error) noexcept {
    gBase = base;
    std::array<unsigned char, 6> play{};
    std::array<unsigned char, 3> playReturn{}, positionReturn{}, allocateReturn{};
    if (!Read(base + 0x00456580U, play) || play != std::array<unsigned char, 6>{0x55,0x8B,0xEC,0x83,0xEC,0x48} ||
        !Read(base + 0x004568ADU, playReturn) || playReturn != std::array<unsigned char, 3>{0xC2,0x14,0} ||
        !Read(base + 0x00455939U, positionReturn) || positionReturn != std::array<unsigned char, 3>{0xC2,0x10,0} ||
        !Read(base + 0x00456C3AU, allocateReturn) || allocateReturn != std::array<unsigned char, 3>{0xC2,4,0}) {
        error = "VoiceSync native SOUND_INTERFACE ABI mismatch";
        return false;
    }
    return true;
}
bool Allocate(std::uintptr_t& emitter, std::uint32_t& object) noexcept {
    emitter = 0U; object = 0U;
    __try {
        if (Interface() == nullptr) return false;
        auto* root = *reinterpret_cast<void**>(gBase + 0x012B1758U);
        auto* manager = Function<Subsystem>(0x008546B0U)(root, 10);
        if (manager == nullptr) return false;
        emitter = Function<AllocateObject>(0x00856BD0U)(manager, "AIMP_VOICE");
        return emitter != 0U && Read(emitter, object) && object != 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
PlayResult Play(const std::uintptr_t emitter, const std::uintptr_t character) noexcept {
    PlayResult result{};
    __try {
        auto* playback = Interface();
        Parameters parameters{};
        if (playback == nullptr) { result.failure = PlayFailure::InterfaceUnavailable; return result; }
        if (emitter == 0U) { result.failure = PlayFailure::InvalidEmitter; return result; }
        if (!Pose(character, parameters)) { result.failure = PlayFailure::InvalidPose; return result; }
        // This path sets SOUND_OBJECT event metadata, acoustic flags, source zone,
        // room/aux state and playing reference count, exactly as native sounds.
        result.playing = Function<NativePlay>(0x00856580U)(playback, &kEvent, &parameters,
            reinterpret_cast<void*>(emitter), nullptr, 0);
        if (result.playing == 0U) result.failure = PlayFailure::NativeRejected;
    } __except (CapturePlayException(GetExceptionInformation(), result)) {}
    return result;
}
bool Position(const std::uintptr_t emitter, const std::uint32_t object,
    const std::uintptr_t character) noexcept {
    __try {
        std::uint32_t current{};
        auto* playback = Interface();
        Parameters parameters{};
        if (playback == nullptr || !Read(emitter, current) || current != object || !Pose(character, parameters)) return false;
        Function<NativePosition>(0x00855850U)(playback, reinterpret_cast<void*>(emitter),
            parameters.position.data(), parameters.orientation.data(), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ForwardEnd(const std::uint32_t object, const std::uint32_t playing) noexcept {
    // Native 856040 End reads only cookie, object ID and playing ID, then queues
    // its game-thread lifetime update. Our native Play passed no callback cookie.
    const std::uint32_t info[3]{0U, object, playing};
    Function<NativeEnd>(0x00856040U)(1U, info);
}
std::uintptr_t EndCallbackAddress() noexcept { return gBase + 0x00456040U; }
}
