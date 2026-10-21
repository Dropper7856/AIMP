#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aimp::voice_sync::native_audio {
// v88 SOUND_EVENT: native registry stores records at 0x14-byte strides.
struct Event final {
    std::uint32_t id{};
    const char* name{};
    const char* flags{};
    float reserved{}, reverbRadius{};
};
// SOUND_INTERFACE::Play has 0xC4 bytes of fields. Native Position loads both
// vectors using MOVAPS: the storage (including every array element) must be
// 16-byte aligned. C++ rounds the allocation size up to 0xD0 accordingly.
// Remaining RTPC/multi-position counts and pointers are zero.
struct alignas(16) Parameters final {
    std::array<float, 4> position{};
    std::array<float, 4> orientation{0, 1, 0, 0};
    std::uint8_t positioned{1U}, environment{1U};
    std::array<std::byte, 0xA2> remaining{};
    std::array<std::byte, 0x0C> alignmentPadding{};
};
static_assert(sizeof(Event) == 0x14U);
static_assert(alignof(Parameters) == 16U);
static_assert(sizeof(Parameters) == 0xD0U);
static_assert(offsetof(Parameters, remaining) + sizeof(Parameters::remaining) == 0xC4U);
static_assert(offsetof(Parameters, positioned) == 0x20U);
static_assert(offsetof(Parameters, environment) == 0x21U);
static_assert(offsetof(Parameters, orientation) == 0x10U);
enum class PlayFailure : std::uint32_t {
    None, InterfaceUnavailable, InvalidEmitter, InvalidPose, NativeRejected, NativeException
};
struct PlayResult final {
    std::uint32_t playing{};
    PlayFailure failure{};
    std::uint32_t exceptionCode{};
    std::uintptr_t exceptionAddress{};
};
inline constexpr std::uint32_t kMonoFormat = 0xA4800004U;
bool Initialize(std::uintptr_t base, std::string& error) noexcept;
bool Allocate(std::uintptr_t& emitter, std::uint32_t& object) noexcept;
PlayResult Play(std::uintptr_t emitter, std::uintptr_t character) noexcept;
bool Position(std::uintptr_t emitter, std::uint32_t object, std::uintptr_t character) noexcept;
// The original retail End callback owns the engine object's playback count.
void ForwardEnd(std::uint32_t object, std::uint32_t playing) noexcept;
std::uintptr_t EndCallbackAddress() noexcept;
}
