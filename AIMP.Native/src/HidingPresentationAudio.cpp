#include "aimp/HidingPresentationAudio.h"
#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"
#include "aimp/VoiceNativeAudio.h"
#include "aimp/Diagnostics.h"
#include "aimp/RuntimeFeatureFlags.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace aimp::hiding_presentation_audio {
namespace {
using Event = voice_sync::native_audio::Event;
using Parameters = voice_sync::native_audio::Parameters;
using ResolveEvent = std::uint8_t(__thiscall*)(void*, const char*, const Event**);
using NativePlay = std::uint32_t(__thiscall*)(void*, const Event*, const Parameters*, void*, void*, char);
using Metadata = void(__thiscall*)(void*, std::uintptr_t, std::uintptr_t, float);
struct Cycle final {
    std::uintptr_t address{}, graph{}, key{};
    std::uint32_t birth{}, animation{};
    std::uint8_t serial{};
};
struct Slot final {
    std::uintptr_t character{};
    std::uint64_t epoch{}, highest{};
    std::uint32_t hierarchy{}, icon{};
    Cycle cycle{};
    DiagnosticsSnapshot diagnostics{};
};
std::array<Slot, 40U> gSlots{};
std::mutex gMutex;
std::atomic<std::uintptr_t> gBase{};
std::atomic<bool> gReady{}, gFilterInstalled{};
std::uint64_t gEpoch{};
struct MetadataRow {
    std::uint32_t eventId{};
    std::uintptr_t lastKey{};
    std::array<char, 96U> name{};
    std::uint64_t observed{}, suppressed{}, reported{}, lastTick{};
    float weight{}, threshold{};
};
std::array<MetadataRow, 64U> gMetadataRows{};
std::mutex gMetadataMutex;
std::atomic<std::uint64_t> gMetadataDropped{}, gMetadataReplaced{};

template<class T> bool Read(std::uintptr_t at, T& value) noexcept {
    return at >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(at), sizeof(value));
}
void ObserveMetadata(std::uintptr_t specification, std::uintptr_t key,
    float weight, bool suppressed) noexcept {
    if (!runtime_features::Enabled(runtime_features::Feature::NativeDiagnostics)) return;
    std::uintptr_t event{};
    std::uint32_t eventId{};
    if (!Read(specification + 0x1CU, event) || !Read(event, eventId)) return;
    // Native callbacks never wait for diagnostic export and retain no Refs.
    std::unique_lock lock(gMetadataMutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++gMetadataDropped; return; }
    MetadataRow* selected = &gMetadataRows.front();
    for (auto& row : gMetadataRows) {
        if (row.observed == 0U || row.eventId == eventId) { selected = &row; break; }
        if (row.lastTick < selected->lastTick) selected = &row;
    }
    auto& row = *selected;
    if (row.observed == 0U || row.eventId != eventId) {
        if (row.observed != 0U) ++gMetadataReplaced;
        row = {}; row.eventId = eventId;
        std::uintptr_t name{};
        if (Read(event + 4U, name) && name >= 0x10000U) {
            for (std::size_t i = 0U; i + 1U < row.name.size(); ++i)
                if (!Read(name + i, row.name[i]) || row.name[i] == '\0') break;
        }
    }
    ++row.observed;
    if (suppressed) ++row.suppressed;
    row.lastKey = key; row.lastTick = GetTickCount64(); row.weight = weight;
    static_cast<void>(Read(specification + 0x20U, row.threshold));
}
bool Match(std::uintptr_t base, std::uintptr_t rva, std::initializer_list<std::uint8_t> bytes) noexcept {
    std::array<std::uint8_t, 16U> actual{};
    return bytes.size() <= actual.size() &&
        crash_guard::TryCopy(actual.data(), reinterpret_cast<const void*>(base + rva), bytes.size()) &&
        std::memcmp(actual.data(), bytes.begin(), bytes.size()) == 0;
}
Slot* Find(std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}
bool ReadCycle(std::uintptr_t character, std::uintptr_t base, Cycle& cycle) noexcept {
    std::uintptr_t type{}, owner{}, runtime{};
    std::uint8_t state{}, flags{};
    return Read(character + 0x50U, cycle.birth) && cycle.birth != 0U &&
        Read(character + 0x36CU, cycle.address) && Read(cycle.address, type) && type == base + 0xEADB58U &&
        Read(cycle.address + 4U, owner) && owner == character &&
        Read(cycle.address + 0x1AU, state) && state == 0x12U &&
        Read(cycle.address + 8U, flags) && (flags & 6U) == 0U &&
        Read(cycle.address + 0x19U, cycle.serial) &&
        Read(cycle.address + 0x14U, cycle.graph) && cycle.graph != 0U &&
        Read(cycle.address + 0x6CU, cycle.animation) &&
        Read(character + 0x730U, runtime) && runtime >= 0x10000U &&
        (cycle.key = runtime + 0x54U) != 0U;
}
bool SameCycle(const Cycle& a, const Cycle& b) noexcept {
    return a.address == b.address && a.graph == b.graph && a.key == b.key &&
        a.birth == b.birth && a.animation == b.animation && a.serial == b.serial;
}
bool Remote(std::uintptr_t character, std::uintptr_t base) noexcept {
    std::uintptr_t root{}, context{}, players{};
    std::uint32_t count{};
    if (!Read(base + 0x12F0C88U, root) || !Read(root + 8U, context) ||
        !Read(context + 0x80U, players) || !Read(context + 0x84U, count) ||
        players < 0x10000U || count == 0U || count > 4U) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uintptr_t player{};
        if (!Read(players + i * 4U, player) || player == character) return false;
    }
    return true;
}
bool Finite(const std::array<float, 4U>& vector) noexcept {
    return std::isfinite(vector[0]) && std::isfinite(vector[1]) && std::isfinite(vector[2]) &&
        std::abs(vector[0]) < 1000000.F && std::abs(vector[1]) < 1000000.F && std::abs(vector[2]) < 1000000.F;
}
bool Pose(std::uintptr_t character, Parameters& parameters, bool& emitterPose) noexcept {
    std::array<float, 4U> body{};
    if (!Read(character + 0x70U, body) || !Finite(body)) return false;
    // These are retained native SOUND_OBJECTs, not bone pointers. Their +10/+20
    // vectors are the retail world position/orientation also used by voice.
    // Prefer the head/speech emitter; never trust an uninitialized faraway pose.
    for (const auto offset : {0x1320U, 0x1324U}) {
        std::uintptr_t emitter{};
        std::uint32_t object{};
        std::array<float, 4U> position{}, orientation{};
        if (!Read(character + offset, emitter) || !Read(emitter, object) || object == 0U ||
            !Read(emitter + 0x10U, position) || !Read(emitter + 0x20U, orientation) ||
            !Finite(position) || !Finite(orientation)) continue;
        float distance{}, length{};
        for (std::size_t i = 0; i < 3U; ++i) {
            const auto delta = position[i] - body[i]; distance += delta * delta;
            length += orientation[i] * orientation[i];
        }
        if (distance > 16.F || length < 0.0001F || length > 100.F) continue;
        parameters.position = position;
        const auto scale = 1.F / std::sqrt(length);
        for (std::size_t i = 0; i < 3U; ++i) parameters.orientation[i] = orientation[i] * scale;
        parameters.position[3] = parameters.orientation[3] = 0.F;
        emitterPose = true;
        return true;
    }
    // No invented bone offset. A remote rig without its retail audio component
    // still has an exact world origin and receives proper positional audio.
    parameters.position = body; parameters.position[3] = 0.F;
    return true;
}
int CaptureException(EXCEPTION_POINTERS* exception, DiagnosticsSnapshot& d) noexcept {
    d.stage = "native-exception";
    if (exception != nullptr && exception->ExceptionRecord != nullptr) {
        d.exceptionCode = exception->ExceptionRecord->ExceptionCode;
        d.exceptionAddress = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
bool Post(std::uintptr_t base, std::uintptr_t character, const char* name,
    DiagnosticsSnapshot& d) noexcept {
    __try {
        std::uintptr_t root{}, playback{}, registry{};
        std::uint8_t ready{};
        if (!Read(base + 0x12B1758U, root) || !Read(root + 0xE5U, ready) || ready == 0U ||
            !Read(root + 0xC0U, playback) || playback == 0U ||
            !Read(root + 0xC4U, registry) || registry == 0U) {
            d.stage = "audio-interface"; return false;
        }
        const Event* event{};
        if (reinterpret_cast<ResolveEvent>(base + 0x4572A0U)(reinterpret_cast<void*>(registry), name, &event) == 0U ||
            !Read(reinterpret_cast<std::uintptr_t>(event), d.eventId) || d.eventId == 0U) {
            d.stage = "event-unavailable"; return false;
        }
        Parameters parameters{};
        if (!Pose(character, parameters, d.usedRetailEmitterPose)) { d.stage = "invalid-pose"; return false; }
        // Exact retail event record, aligned basic 3D parameters, no callback
        // cookie and no persistent SOUND_OBJECT. 00856580 allocates one from
        // manager10, increments +30 for a successful post, and installs its own
        // End callback. 00856C40 reclaims it when that count returns to zero.
        d.playingId = reinterpret_cast<NativePlay>(base + 0x456580U)(reinterpret_cast<void*>(playback),
            event, &parameters, nullptr, nullptr, 0);
        d.stage = d.playingId != 0U ? "posted" : "native-rejected";
        return d.playingId != 0U;
    } __except (CaptureException(GetExceptionInformation(), d)) { return false; }
}
void __fastcall MetadataFilter(void* manager, void*, std::uintptr_t specification,
    std::uintptr_t key, float weight) {
    // Single certified CALL site: ECX=audioRoot+F4, three stack arguments,
    // callee RET0C. A cdecl replacement would corrupt the engine stack.
    const auto base = gBase.load();
    std::uintptr_t event{};
    std::uint32_t eventId{};
    if (Read(specification + 0x1CU, event) && Read(event, eventId)) {
        Slot candidate{};
        {
            const std::lock_guard lock(gMutex);
            for (const auto& slot : gSlots) {
                if (slot.character != 0U && slot.cycle.key == key && slot.diagnostics.playingId != 0U &&
                    slot.diagnostics.eventId == eventId) { candidate = slot; break; }
            }
        }
        Cycle current{};
        if (candidate.character != 0U && ReadCycle(candidate.character, base, current) &&
            SameCycle(candidate.cycle, current)) {
            const std::lock_guard lock(gMutex);
            if (auto* slot = Find(candidate.character); slot != nullptr && slot->epoch == candidate.epoch &&
                SameCycle(slot->cycle, current)) {
                ++slot->diagnostics.metadataObserved; ++slot->diagnostics.metadataSuppressed;
                slot->diagnostics.metadataWeight = weight;
                static_cast<void>(Read(specification + 0x20U, slot->diagnostics.metadataThreshold));
                ObserveMetadata(specification, key, weight, true);
                return;
            }
        }
    }
    ObserveMetadata(specification, key, weight, false);
    reinterpret_cast<Metadata>(base + 0x2C00B0U)(manager, specification, key, weight);
}
}

bool Initialize(std::uintptr_t base, std::string& error) noexcept {
    gReady.store(false); gFilterInstalled.store(false); gBase.store(base); ForgetAll();
    if (base == 0U ||
        !Match(base, 0x456580U, {0x55,0x8B,0xEC,0x83,0xEC,0x48}) ||
        !Match(base, 0x4568ADU, {0xC2,0x14,0x00}) ||
        !Match(base, 0x4572A0U, {0x55,0x8B,0xEC,0x53,0x8B,0x5D,0x0C,0x56,0x8B,0xF1}) ||
        !Match(base, 0x4572FAU, {0xC2,0x08,0x00}) ||
        !Match(base, 0x2C00B0U, {0x55,0x8B,0xEC,0x83,0xE4,0xF0}) ||
        !Match(base, 0x2C048AU, {0xC2,0x0C,0x00})) {
        error = "Hiding audio retail SOUND_INTERFACE/metadata ABI mismatch"; return false;
    }
    gReady.store(true); error.clear(); return true;
}
bool InstallMetadataFilter(HookManager& hooks, std::string& error) {
    if (!gReady.load()) { error = "Hiding audio ABI was not initialized"; return false; }
    if (gFilterInstalled.load()) return true;
    const auto base = gBase.load();
    if (!hooks.InstallCallRedirect({"hiding_audio_exact_metadata_duplicate_filter",
        reinterpret_cast<void*>(base + 0x39A455U), reinterpret_cast<void*>(base + 0x2C00B0U),
        reinterpret_cast<void*>(&MetadataFilter)}, error)) return false;
    gFilterInstalled.store(true); return true;
}
bool PlayOnEngineThread(std::uintptr_t character, const player_hiding_presentation::State& state,
    std::uint64_t effectiveAge) noexcept {
    if (character < 0x10000U || !player_hiding_presentation::Valid(state)) return false;
    Slot work{};
    {
        const std::lock_guard lock(gMutex);
        auto* slot = Find(character);
        if (slot == nullptr) {
            for (auto& empty : gSlots) if (empty.character == 0U) { slot = &empty; break; }
            if (slot == nullptr) return false;
            *slot = {}; slot->character = character; slot->epoch = ++gEpoch;
        }
        auto& d = slot->diagnostics;
        if (state.phase == 0U) { slot->cycle = {}; d.playingId = 0U; d.stage = "cleared"; return false; }
        if (state.transition <= slot->highest) { ++d.duplicate; return false; }
        slot->highest = state.transition; slot->cycle = {};
        slot->hierarchy = state.hierarchyHash; slot->icon = state.entityShortGuid;
        d.transition = state.transition; d.phase = state.phase; d.kind = state.kind;
        d.eventId = d.playingId = d.exceptionCode = 0U; d.exceptionAddress = 0U;
        d.usedRetailEmitterPose = false; d.eventName = "";
        const auto selected = Selection(state.kind, state.phase);
        if (selected.name == nullptr) { d.stage = "hidden"; return false; }
        d.eventName = selected.name;
        if (!state.animate) { ++d.baseline; d.stage = "baseline"; return false; }
        if (effectiveAge > 500U) { ++d.stale; d.stage = "stale"; return false; }
        ++d.attempted;
        work = *slot;
    }
    auto& d = work.diagnostics;
    const auto base = gBase.load();
    bool posted{};
    if (!gReady.load() || !gFilterInstalled.load()) d.stage = "not-installed";
    else if (!Remote(character, base)) d.stage = "not-remote";
    else if (!ReadCycle(character, base, work.cycle) ||
        work.cycle.animation != Selection(state.kind, state.phase).animation) d.stage = "cycle-mismatch";
    else posted = Post(base, character, d.eventName, d);
    if (posted) ++d.posted; else ++d.failed;
    {
        const std::lock_guard lock(gMutex);
        if (auto* slot = Find(character); slot != nullptr && slot->epoch == work.epoch &&
            slot->highest == state.transition) {
            // No engine call while holding the value-mailbox mutex.
            *slot = work;
        }
    }
    return posted;
}
void Forget(std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex); if (auto* slot = Find(character)) *slot = {};
}
void ForgetAll() noexcept { const std::lock_guard lock(gMutex); gSlots = {}; ++gEpoch; }
DiagnosticsSnapshot Snapshot(std::uintptr_t character) noexcept {
    const std::lock_guard lock(gMutex);
    const auto* slot = Find(character);
    auto d = slot != nullptr ? slot->diagnostics : DiagnosticsSnapshot{};
    d.initialized = gReady.load(); d.filterInstalled = gFilterInstalled.load();
    return d;
}
void LogMetadataDiagnostics(aimp::Diagnostics& diagnostics) {
    if (!runtime_features::Enabled(runtime_features::Feature::NativeDiagnostics)) return;
    std::array<MetadataRow, 64U> rows{};
    {
        const std::lock_guard lock(gMetadataMutex);
        for (std::size_t i = 0U; i < rows.size(); ++i) {
            auto& row = gMetadataRows[i];
            if (row.observed == row.reported) continue;
            rows[i] = row; row.reported = row.observed;
        }
    }
    for (const auto& row : rows) if (row.observed != 0U)
        diagnostics.Log("info", "native_animation_audio_metadata",
            "Process-wide retail metadata observations; key is local only. Forwarding does not prove playback or audibility.",
            {{"eventId", Diagnostics::Unsigned(row.eventId), true}, {"eventName", row.name.data()},
             {"lastKey", Diagnostics::Pointer(reinterpret_cast<const void*>(row.lastKey))},
             {"observed", Diagnostics::Unsigned(row.observed), true},
             {"forwarded", Diagnostics::Unsigned(row.observed - row.suppressed), true},
             {"suppressed", Diagnostics::Unsigned(row.suppressed), true},
             {"weight", std::to_string(row.weight)}, {"threshold", std::to_string(row.threshold)},
             {"sampleTick", Diagnostics::Unsigned(row.lastTick), true},
             {"dropped", Diagnostics::Unsigned(gMetadataDropped.load()), true},
             {"replaced", Diagnostics::Unsigned(gMetadataReplaced.load()), true}});
}
}
