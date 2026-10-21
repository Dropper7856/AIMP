#include "aimp/VoiceNativeAudio.h"
#include "aimp/VoiceBank.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace aimp::voice_sync;
    native_audio::Parameters parameters{};
    const auto* bytes = reinterpret_cast<const unsigned char*>(&parameters);
    // 8558E6/8558F0 load the incoming position/orientation using MOVAPS.
    // Both local allocations and array strides must satisfy that native ABI.
    static_assert(alignof(native_audio::Parameters) == 16);
    static_assert(sizeof(native_audio::Parameters) == 0xD0);
    assert(reinterpret_cast<std::uintptr_t>(parameters.position.data()) % 16 == 0);
    assert(reinterpret_cast<std::uintptr_t>(parameters.orientation.data()) % 16 == 0);
    native_audio::Parameters pairs[2]{};
    for (const auto& pair : pairs) {
        assert(reinterpret_cast<std::uintptr_t>(pair.position.data()) % 16 == 0);
        assert(reinterpret_cast<std::uintptr_t>(pair.orientation.data()) % 16 == 0);
    }
    assert(bytes[0x20] == 1 && bytes[0x21] == 1);
    // These bytes are engine counts. Nonzero defaults make Play dereference
    // optional arrays past the basic position/orientation portion.
    assert(bytes[0xBC] == 0 && bytes[0xBD] == 0);
    for (std::size_t i = 0x22; i < 0xC4; ++i) assert(bytes[i] == 0);
    const auto format = native_audio::kMonoFormat;
    assert((format & 0x3FFFFU) == 4U); // exactly one centre channel
    assert(((format >> 24) & 31U) == 4U); // float32 stride
    assert((format & 0x60000000U) == 0x20000000U); // native float source path
    std::uint32_t version{}, bankId{};
    std::memcpy(&version, kVoiceBank.data() + 8, 4);
    std::memcpy(&bankId, kVoiceBank.data() + 12, 4);
    assert(version == 88U && bankId == kVoiceBankId);
    assert(std::memcmp(kVoiceBank.data() + 28, "HIRC", 4) == 0);
    std::cout << "Native voice ABI layout, optional counts and mono format contracts passed.\n";
}
