#include "aimp/VoiceSync.h"
#include "aimp/VoiceBank.h"
#include "aimp/AISync.h"
#include "aimp/Diagnostics.h"
#include "aimp/HookManager.h"
#include "aimp/LevelGenerationTracker.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace aimp::voice_sync {
namespace {
constexpr std::size_t kSamples = 320U, kRingSize = 2560U, kSpeakers = 16U;
struct AudioBuffer final {
    float* data;
    std::uint32_t channels, state;
    std::uint16_t maximum, valid;
};
struct AudioFormat final { std::uint32_t rate, packed; };
static_assert(sizeof(AudioBuffer) == 16U);
struct BankCommand final {
    std::uint32_t operation, callback, cookie, mode, id, pool, memory, size, reserved;
};
static_assert(sizeof(BankCommand) == 36U);
using Execute = void(__cdecl*)(std::uint32_t, AudioBuffer*);
using Format = void(__cdecl*)(std::uint32_t, AudioFormat*);
using Gain = float(__cdecl*)(std::uint32_t);
using SetCallbacks = void(__cdecl*)(Execute, Format, Gain);
using RegisterPlaying = int(__thiscall*)(void*, const std::uint32_t*,
    std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint32_t);
using PostEvent = std::uint32_t(__cdecl*)(std::uint32_t, std::uint32_t,
    std::uint32_t, void*, void*, std::uint32_t, void*, std::uint32_t);
using StopEvent = int(__cdecl*)(std::uint32_t, std::uint32_t, std::uint32_t,
    std::int32_t, std::uint32_t, std::uint32_t);
using QueueBank = int(__thiscall*)(void*, BankCommand);
using SoundConstructor = void*(__thiscall*)(void*, std::uint32_t, const float*, float,
    const std::uint32_t*, float, const std::uint8_t*, std::uint32_t);
using SoundDispatch = void(__thiscall*)(void*, const void*, std::uint32_t);

struct Speaker final {
    // The slot and its storage survive EndOfEvent and every round transition.
    std::atomic<std::uint32_t> playing{}, read{}, write{};
    std::atomic<bool> active{}, ended{true};
    std::array<float, kRingSize> samples{};
    std::uint64_t peer{}, lastFrame{}, lastStimulus{}; // engine thread only
    std::uintptr_t character{};
    std::uint32_t birth{}, object{}, sequence{};
    bool buffering{true}; // audio callback only
};
struct Configuration final {
    std::uint32_t magic{}, native{}, round{}, flags{};
    std::array<std::byte, 16> session{};
    std::uint64_t localPeer{};
    float volume{};
    std::uint32_t reserved{};
};
struct PcmFrame final {
    std::uint32_t magic{}, native{}, round{}, flags{};
    std::array<std::byte, 16> session{};
    std::uint64_t peer{};
    std::uint32_t reserved{}, sequence{};
    std::array<std::int16_t, kSamples> pcm{};
};
static_assert(sizeof(Configuration) == 48U);
static_assert(sizeof(PcmFrame) == 688U);
std::uintptr_t gBase{};
LevelGenerationTracker* gGenerations{};
Diagnostics* gDiagnostics{};
std::atomic<bool> gInstalled{}, gTransport{};
std::atomic<float> gVolume{1.0F};
std::atomic<Execute> gOriginalExecute{};
std::atomic<Format> gOriginalFormat{};
std::atomic<Gain> gOriginalGain{};
std::atomic<void*> gSetterOriginal{};
void* gSetterTrampoline{};
std::array<Speaker, kSpeakers> gSpeakers{};
thread_local Speaker* gPosting{};
std::mutex gQueueLock;
Configuration gPending{}, gCurrent{};
bool gPendingConfiguration{};
std::array<PcmFrame, 32> gFrames{};
std::size_t gFrameRead{}, gFrameCount{};
std::atomic<int> gBankState{}; // 0 idle, 1 pending, 2 loaded, -1 failed
std::atomic<int> gBankResult{};
bool gBankReported{};

template<class T> bool Read(const std::uintptr_t address, T& value) noexcept {
    if (address < 0x10000U) return false;
    __try { std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> T Function(const std::uintptr_t va) noexcept {
    return reinterpret_cast<T>(gBase + va - 0x00400000U);
}
Speaker* ForPlaying(const std::uint32_t id) noexcept {
    if (id == 0U) return nullptr;
    for (auto& speaker : gSpeakers)
        if (speaker.playing.load(std::memory_order_acquire) == id) return &speaker;
    return nullptr;
}

void __cdecl Render(const std::uint32_t id, AudioBuffer* buffer) noexcept {
    auto* speaker = ForPlaying(id);
    if (speaker == nullptr) {
        if (const auto original = gOriginalExecute.load(std::memory_order_acquire)) original(id, buffer);
        return;
    }
    if (buffer == nullptr || buffer->data == nullptr || buffer->maximum == 0U) return;
    // No locks, allocation, game objects or IPC on Wwise's render thread.
    const bool active = gTransport.load(std::memory_order_acquire) && speaker->active.load(std::memory_order_acquire);
    auto read = speaker->read.load(std::memory_order_relaxed);
    const auto write = speaker->write.load(std::memory_order_acquire);
    if (speaker->buffering && write - read >= kSamples * 2U) speaker->buffering = false;
    const auto count = active && !speaker->buffering
        ? (std::min)(static_cast<std::uint32_t>(buffer->maximum), write - read) : 0U;
    for (std::uint32_t i = 0U; i < count; ++i) buffer->data[i] = speaker->samples[(read + i) % kRingSize];
    std::fill(buffer->data + count, buffer->data + buffer->maximum, 0.0F);
    read += count;
    speaker->read.store(read, std::memory_order_release);
    buffer->valid = active ? buffer->maximum : 0U;
    buffer->state = active ? 45U : 46U;
}
void __cdecl SetFormat(const std::uint32_t id, AudioFormat* format) noexcept {
    if (ForPlaying(id) != nullptr) {
        if (format != nullptr) *format = {16000U, 0xA4800004U};
    } else if (const auto original = gOriginalFormat.load(std::memory_order_acquire)) original(id, format);
}
float __cdecl GetGain(const std::uint32_t id) noexcept {
    if (ForPlaying(id) != nullptr) return gVolume.load(std::memory_order_relaxed);
    const auto original = gOriginalGain.load(std::memory_order_acquire);
    return original != nullptr ? original(id) : 1.0F;
}
void __cdecl SetCallbacksDetour(Execute execute, Format format, Gain gain) noexcept {
    const auto original = reinterpret_cast<SetCallbacks>(gSetterOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (execute == nullptr || format == nullptr) { original(execute, format, gain); return; }
    if (execute != &Render) gOriginalExecute.store(execute, std::memory_order_release);
    if (format != &SetFormat) gOriginalFormat.store(format, std::memory_order_release);
    if (gain != &GetGain) gOriginalGain.store(gain, std::memory_order_release);
    original(&Render, &SetFormat, &GetGain);
}

// Redirect only C2D165's registration call. C391E0 returns RET14, accepting
// five stack arguments. command[1] is the InterlockedIncrement playing ID.
// Publish after registration succeeds, BEFORE C2D180 enqueues the audio command.
// This closes the format-callback race without waiting on the audio thread.
int __fastcall RegisterPlayingDetour(void* manager, void*, const std::uint32_t* command,
    const std::uintptr_t callback, const std::uintptr_t cookie, const std::uint32_t flags,
    const std::uint32_t object) noexcept {
    const auto result = Function<RegisterPlaying>(0x00C391E0U)(manager, command, callback, cookie, flags, object);
    if (result == 1 && gPosting != nullptr && command != nullptr && command[0] == kVoiceEventId)
        gPosting->playing.store(command[1], std::memory_order_release);
    return result;
}
void __cdecl EventDone(const std::uint32_t type, void* info) noexcept {
    if (type != 1U || info == nullptr) return;
    // AkCallbackInfo88 starts with the cookie pointer. Slots are process resident.
    auto* speaker = *static_cast<Speaker**>(info);
    for (auto& slot : gSpeakers) if (speaker == &slot) {
        slot.active.store(false, std::memory_order_release);
        slot.ended.store(true, std::memory_order_release);
        return;
    }
}
void __cdecl BankDone(const std::uint32_t id, const void* memory, const int result,
    const int, void*) noexcept {
    if (id != kVoiceBankId || memory != kVoiceBank.data()) return;
    gBankResult.store(result, std::memory_order_relaxed);
    gBankState.store(result == 1 ? 2 : -1, std::memory_order_release);
}
bool BeginBankLoad() noexcept {
    std::uintptr_t manager{};
    if (!Read(gBase + 0x017F9684U, manager) || manager == 0U) return false;
    const BankCommand command{0U, reinterpret_cast<std::uint32_t>(&BankDone), 0U,
        1U, kVoiceBankId, 0xFFFFFFFFU, reinterpret_cast<std::uint32_t>(kVoiceBank.data()),
        static_cast<std::uint32_t>(kVoiceBank.size()), 0U};
    gBankState.store(1, std::memory_order_release);
    const auto result = Function<QueueBank>(0x00C3BCC0U)(reinterpret_cast<void*>(manager), command);
    if (result != 1) { gBankResult.store(result); gBankState.store(-1); }
    return result == 1;
}

void StopSpeaker(Speaker& speaker) noexcept {
    if (!speaker.active.exchange(false, std::memory_order_acq_rel)) return;
    const auto playing = speaker.playing.load(std::memory_order_acquire);
    if (playing != 0U) static_cast<void>(Function<StopEvent>(0x00C2D240U)(
        kVoiceEventId, 0U, speaker.object, 0, 4U, playing));
}
void StopAll() noexcept { for (auto& speaker : gSpeakers) StopSpeaker(speaker); }
Speaker* GetSpeaker(const std::uint64_t peer, const std::uintptr_t character) noexcept {
    std::uint32_t birth{}, head{}, object{};
    if (!Read(character + 0x50U, birth) || !Read(character + 0x1320U, head) ||
        !Read(head, object) || object == 0U || object == 0xFFFFFFFFU) return nullptr;
    for (auto& speaker : gSpeakers) {
        if (speaker.active.load(std::memory_order_acquire) && speaker.peer == peer) {
            if (speaker.character == character && speaker.birth == birth && speaker.object == object) return &speaker;
            StopSpeaker(speaker);
        }
    }
    if (gBankState.load(std::memory_order_acquire) != 2) return nullptr;
    for (auto& speaker : gSpeakers) {
        if (!speaker.ended.load(std::memory_order_acquire)) continue;
        speaker.playing.store(0U, std::memory_order_release);
        speaker.peer = peer; speaker.character = character; speaker.birth = birth;
        speaker.object = object; speaker.sequence = 0U; speaker.lastStimulus = 0U;
        speaker.read.store(0U); speaker.write.store(0U); speaker.buffering = true;
        speaker.ended.store(false, std::memory_order_release);
        speaker.active.store(true, std::memory_order_release);
        gPosting = &speaker;
        const auto playing = Function<PostEvent>(0x00C2D750U)(kVoiceEventId, object, 1U,
            reinterpret_cast<void*>(&EventDone), &speaker, 0U, nullptr, 0U);
        gPosting = nullptr;
        if (playing == 0U) {
            speaker.active.store(false); speaker.ended.store(true); speaker.playing.store(0U);
            if (gDiagnostics != nullptr) gDiagnostics->Log("error", "voicesync_stream_failed",
                "Native PostEvent refused the spatial voice stream.");
            return nullptr;
        }
        if (gDiagnostics != nullptr) gDiagnostics->Log("info", "voicesync_stream_started",
            "Native spatial voice stream started on the participant HEAD emitter.",
            {{"peer", Diagnostics::Unsigned(peer), true},
             {"playingId", Diagnostics::Unsigned(playing), true}});
        return &speaker;
    }
    return nullptr;
}
bool EmitSound(const std::uintptr_t character, const float intensity) noexcept {
    // Same category/flags/constructor/queue as the retail NoiseTracking event.
    // Dispatch has TWO stack args (event*,flag), RET8; ctor has SEVEN, RET1C.
    __try {
        const auto root = *reinterpret_cast<std::uintptr_t*>(gBase + 0x012F0C88U);
        if (root == 0U) return false;
        const auto manager = *reinterpret_cast<void**>(root + 0x18U);
        if (manager == nullptr) return false;
        alignas(16) std::byte event[60]{};
        const std::uint8_t flags[6]{1U, 0U, 0U, 0U, 0U, 0U};
        const auto time = static_cast<float>(*reinterpret_cast<double*>(gBase + 0x012410A8U));
        const auto* position = reinterpret_cast<float*>(character + 0x70U);
        const auto* identity = reinterpret_cast<std::uint32_t*>(character + 0x54U);
        Function<SoundConstructor>(0x004468B0U)(event, 0U, position, time, identity, intensity, flags, 0U);
        Function<SoundDispatch>(0x004574D0U)(manager, event, 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Apply(const PcmFrame& frame) noexcept {
    if (frame.native != gCurrent.native || frame.round != gCurrent.round ||
        frame.session != gCurrent.session || !gTransport.load(std::memory_order_acquire)) return;
    const auto character = ai_sync::ResolveLivingParticipant(frame.peer, frame.native, frame.round);
    if (character == 0U) return;
    const auto now = GetTickCount64();
    const bool local = frame.peer == gCurrent.localPeer;
    if ((frame.flags == 1U) != local || (local && (gCurrent.flags & 2U) == 0U)) return;
    // Local Host has no playback stream; retain separate per-peer stimulus timing.
    static std::uint64_t localStimulus{};
    double energy{};
    for (const auto sample : frame.pcm) { const auto v = sample / 32768.0; energy += v * v; }
    const auto rms = static_cast<float>(std::sqrt(energy / static_cast<double>(kSamples)));
    auto* speaker = local ? nullptr : GetSpeaker(frame.peer, character);
    if (!local && speaker != nullptr) {
        if (speaker->sequence != 0U && static_cast<std::int32_t>(frame.sequence - speaker->sequence) <= 0) return;
        speaker->sequence = frame.sequence; speaker->lastFrame = now;
        const auto write = speaker->write.load(std::memory_order_relaxed);
        const auto read = speaker->read.load(std::memory_order_acquire);
        if (write - read + kSamples <= kRingSize) {
            for (std::size_t i = 0; i < kSamples; ++i)
                speaker->samples[(write + i) % kRingSize] = frame.pcm[i] / 32768.0F;
            speaker->write.store(write + static_cast<std::uint32_t>(kSamples), std::memory_order_release);
        }
    }
    // Playback volume is deliberately independent of what the Host's AI hears.
    auto& lastStimulus = local ? localStimulus : (speaker != nullptr ? speaker->lastStimulus : localStimulus);
    if ((gCurrent.flags & 2U) != 0U && rms >= 0.008F && now - lastStimulus >= 250U) {
        float normalIntensity{};
        if (Read(gBase + 0x0123D07CU, normalIntensity) && std::isfinite(normalIntensity) && normalIntensity > 0.0F) {
            const auto intensity = normalIntensity * (std::clamp)(rms / 0.08F, 0.15F, 1.5F);
            if (EmitSound(character, intensity)) lastStimulus = now;
        }
    }
}
} // namespace

bool Installed() noexcept { return gInstalled.load(std::memory_order_acquire); }
bool Configure(const std::span<const std::byte> bytes, const bool authorityHost) noexcept {
    if (!Installed() || bytes.size() != sizeof(Configuration)) return false;
    Configuration configuration{};
    std::memcpy(&configuration, bytes.data(), sizeof(configuration));
    if (configuration.magic != 0x31435641U || configuration.flags > 3U || configuration.reserved != 0U ||
        !std::isfinite(configuration.volume) || configuration.volume < 0.0F || configuration.volume > 1.0F ||
        ((configuration.flags & 1U) != 0U && (configuration.native == 0U || configuration.round == 0U ||
            configuration.localPeer == 0U || ((configuration.flags & 2U) != 0U) != authorityHost))) return false;
    std::lock_guard lock(gQueueLock);
    gPending = configuration; gPendingConfiguration = true;
    gFrameRead = gFrameCount = 0U;
    return true;
}
bool QueueFrame(const std::span<const std::byte> bytes) noexcept {
    if (!Installed() || bytes.size() != sizeof(PcmFrame)) return false;
    PcmFrame frame{};
    std::memcpy(&frame, bytes.data(), sizeof(frame));
    if (frame.magic != 0x31465641U || frame.flags > 1U || frame.peer == 0U || frame.reserved != 0U) return false;
    std::lock_guard lock(gQueueLock);
    if (gFrameCount == gFrames.size()) return false;
    gFrames[(gFrameRead + gFrameCount++) % gFrames.size()] = frame;
    return true;
}
void TransportLost() noexcept {
    gTransport.store(false, std::memory_order_release);
    std::lock_guard lock(gQueueLock);
    gPending = {}; gPendingConfiguration = true; gFrameRead = gFrameCount = 0U;
}
void PumpOnEngineThread() noexcept {
    if (!Installed()) return;
    std::array<PcmFrame, 8> frames{};
    std::size_t count{};
    Configuration configuration{};
    bool changed{};
    {
        std::lock_guard lock(gQueueLock);
        changed = gPendingConfiguration;
        if (changed) { configuration = gPending; gPendingConfiguration = false; }
        while (gFrameCount != 0U && count < frames.size()) {
            frames[count++] = gFrames[gFrameRead];
            gFrameRead = (gFrameRead + 1U) % gFrames.size(); --gFrameCount;
        }
    }
    if (changed) {
        const bool same = configuration.native == gCurrent.native && configuration.round == gCurrent.round &&
            configuration.session == gCurrent.session && configuration.localPeer == gCurrent.localPeer;
        if (!same || (configuration.flags & 1U) == 0U) StopAll();
        gCurrent = configuration; gVolume.store(configuration.volume, std::memory_order_release);
        gTransport.store((configuration.flags & 1U) != 0U, std::memory_order_release);
    }
    if (gGenerations == nullptr || !gGenerations->IsStableGeneration(gCurrent.native) ||
        !gTransport.load(std::memory_order_acquire)) { StopAll(); return; }
    if (gBankState.load(std::memory_order_acquire) == 0) static_cast<void>(BeginBankLoad());
    const auto bankState = gBankState.load(std::memory_order_acquire);
    if (!gBankReported && (bankState == 2 || bankState == -1)) {
        gBankReported = true;
        if (gDiagnostics != nullptr) gDiagnostics->Log(bankState == 2 ? "info" : "error", "voicesync_bank",
            bankState == 2 ? "Native spatial voice bank loaded." : "Native spatial voice bank load failed.",
            {{"result", std::to_string(gBankResult.load()), true}});
    }
    for (std::size_t i = 0; i < count; ++i) Apply(frames[i]);
    for (auto& speaker : gSpeakers) if (speaker.active.load(std::memory_order_acquire) &&
        (GetTickCount64() - speaker.lastFrame > 1000U ||
            ai_sync::ResolveLivingParticipant(speaker.peer, gCurrent.native, gCurrent.round) != speaker.character))
        StopSpeaker(speaker);
}
bool Install(const std::uintptr_t base, HookManager& hooks, LevelGenerationTracker& generations,
    Diagnostics& diagnostics, std::string& error) {
    if (Installed()) return true;
    gBase = base; gGenerations = &generations; gDiagnostics = &diagnostics;
    const auto checkpoint = hooks.InstalledCount();
    HookRequest setter{};
    setter.name = "voicesync_audio_input_callbacks";
    setter.target = reinterpret_cast<void*>(base + 0x00895E60U);
    setter.detour = reinterpret_cast<void*>(&SetCallbacksDetour);
    setter.patchLength = 6U;
    setter.expected.bytes = {0x8BU, 0x4CU, 0x24U, 0x04U, 0x85U, 0xC9U};
    setter.expected.mask = "xxxxxx";
    setter.relocationSafePrefix = true;
    setter.trampolineOut = &gSetterTrampoline;
    setter.publishedTrampolineOut = &gSetterOriginal;
    if (!hooks.Install(setter, error)) return false;
    if (!hooks.InstallCallRedirect({"voicesync_publish_playing_identity",
        reinterpret_cast<void*>(base + 0x0082D165U), reinterpret_cast<void*>(base + 0x008391E0U),
        reinterpret_cast<void*>(&RegisterPlayingDetour)}, error)) {
        std::string rollback;
        static_cast<void>(hooks.UninstallFrom(checkpoint, rollback));
        return false;
    }
    Execute execute{}; Format format{}; Gain gain{};
    static_cast<void>(Read(base + 0x017FA1C0U, execute));
    static_cast<void>(Read(base + 0x017FA1C4U, format));
    static_cast<void>(Read(base + 0x017FA1C8U, gain));
    // Valid shipped callbacks supply fallback if movie renderer has not initialized yet.
    SetCallbacksDetour(execute != nullptr ? execute : Function<Execute>(0x00A1BCD0U),
        format != nullptr ? format : Function<Format>(0x00A1BC30U),
        gain != nullptr ? gain : Function<Gain>(0x00A1BD50U));
    gInstalled.store(true, std::memory_order_release);
    return true;
}
} // namespace aimp::voice_sync
