#include "aimp/AISyncAnimation.h"

#include "aimp/CrashGuard.h"
#include "aimp/HookManager.h"

#include <intrin.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace aimp::ai_sync::animation {
namespace {

constexpr std::uintptr_t kSourceTypeRva = 0x00EAE5D0U;
constexpr std::uintptr_t kVictimTypeRva = 0x00EAE690U;
constexpr std::uintptr_t kScriptedTypeRva = 0x00EADB58U;
constexpr std::uintptr_t kCodeBegin = 0x00001000U;
constexpr std::uintptr_t kCodeEnd = 0x00D9F000U;
constexpr std::uint32_t kMaximumObservedListeners = 16U;

std::uintptr_t gBase{};
std::atomic<bool> gInstalled{};
std::atomic<HumanPairClassifier> gHumanPair{};
std::atomic<ManagedSourcePredicate> gManagedSource{};
std::atomic<Observer> gObserver{};
std::atomic<ManagedSourcePredicate> gCanReplayScripted{};

void* gCoupledClassificationTrampoline{};
void* gCoupledPlayerContinue{};
void* gAlignmentClassificationTrampoline{};
void* gAlignmentPlayerContinue{};
std::uintptr_t gUnitFloatAddress{};
void* gSourceUpdateTrampoline{};
void* gMarkerCallbackTrampoline{};
void* gMarkerDispatchTrampoline{};
std::atomic<void*> gSourceUpdateOriginal{};
std::atomic<void*> gMarkerCallbackOriginal{};
std::atomic<void*> gMarkerDispatchOriginal{};

std::atomic<std::uint64_t> gSourceUpdates{}, gCoupledPlayerUpdates{};
std::atomic<std::uint64_t> gHumanAlignmentUpdates{};
std::atomic<std::uint64_t> gMarkerCallbacks{}, gMarkerCallbacksSkipped{}, gMarkerDispatches{};
std::atomic<std::uint64_t> gBoundedListenerScans{};
std::atomic<std::uint64_t> gScriptedCaptured{}, gScriptedUnsupported{}, gScriptedApplied{}, gScriptedFailed{};
std::atomic<std::uint32_t> gLastSourceUpdateCallerRva{}, gLastMarker{}, gLastMarkerCallerRva{};

template<class T>
bool Read(const std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U &&
        crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}

bool Match(const std::uintptr_t rva,
    const std::initializer_list<std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 64U> bytes{};
    return expected.size() <= bytes.size() &&
        crash_guard::TryCopy(bytes.data(), reinterpret_cast<const void*>(gBase + rva), expected.size()) &&
        std::memcmp(bytes.data(), expected.begin(), expected.size()) == 0;
}

std::uint32_t CallerRva(const void* caller) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(caller);
    return address >= gBase + kCodeBegin && address < gBase + kCodeEnd
        ? static_cast<std::uint32_t>(address - gBase) : 0U;
}

bool CurrentCycle(const std::uintptr_t cycle, const std::uintptr_t expectedType,
    const std::uint8_t expectedState, std::uintptr_t& owner) noexcept {
    std::uintptr_t type{}, current{};
    std::uint8_t state{};
    return Read(cycle, type) && type == gBase + expectedType &&
        Read(cycle + 4U, owner) && owner != 0U &&
        Read(owner + 0x36CU, current) && current == cycle &&
        Read(cycle + 0x1AU, state) && state == expectedState;
}

bool NativeGraph(const std::uintptr_t cycle, std::uintptr_t& graph) noexcept {
    std::uintptr_t action{};
    // cycle+14 is the animation action, not its event-listener graph.
    // 0066C3xx / 006761C5 load action+50 before registering the cycle.
    return Read(cycle + 0x14U, action) && action != 0U &&
        Read(action + 0x50U, graph) && graph != 0U;
}

bool ManagedCycle(const std::uintptr_t cycle, Observation& observation) noexcept {
    const auto managed = gManagedSource.load(std::memory_order_acquire);
    if (managed == nullptr) return false;
    std::uintptr_t type{}, owner{};
    if (!Read(cycle, type)) return false;
    if (type == gBase + kSourceTypeRva) {
        if (!CurrentCycle(cycle, kSourceTypeRva, 0x4EU, owner) || !managed(owner)) return false;
        observation.source = owner;
        static_cast<void>(Read(cycle + 0x44U, observation.victim));
    } else if (type == gBase + kVictimTypeRva) {
        std::uint32_t cachedBirth{}, currentBirth{};
        if (!CurrentCycle(cycle, kVictimTypeRva, 0x4FU, owner) ||
            !Read(cycle + 0x68U, observation.source) || !managed(observation.source) ||
            !Read(cycle + 0x6CU, cachedBirth) ||
            !Read(observation.source + 0x50U, currentBirth) || currentBirth != cachedBirth) return false;
        // Keep observing an acquired victim after the source has changed cycle.
        // That ordering is precisely what the graph observations must expose.
        observation.victim = owner;
    } else return false;
    observation.cycle = cycle;
    return true;
}

void Observe(const Observation& observation) noexcept {
    const auto observer = gObserver.load(std::memory_order_acquire);
    if (observer != nullptr) observer(observation);
}

bool MarkerObservation(const std::uintptr_t graph, const std::uint32_t index,
    Observation& observation) noexcept {
    std::uintptr_t listenerSets{}, listeners{}, markers{};
    std::uint32_t setCount{}, listenerCount{};
    if (!Read(graph + 0x88U, setCount) || index >= setCount ||
        !Read(graph + 0x84U, listenerSets) || listenerSets == 0U ||
        !Read(listenerSets + static_cast<std::uintptr_t>(index) * 12U, listeners) || listeners == 0U ||
        !Read(listenerSets + static_cast<std::uintptr_t>(index) * 12U + 4U, listenerCount)) return false;
    if (listenerCount > kMaximumObservedListeners)
        gBoundedListenerScans.fetch_add(1U, std::memory_order_relaxed);
    const auto limit = listenerCount < kMaximumObservedListeners ? listenerCount : kMaximumObservedListeners;
    for (std::uint32_t i = 0U; i < limit; ++i) {
        std::uintptr_t cycle{}, cycleGraph{};
        if (!Read(listeners + static_cast<std::uintptr_t>(i) * 4U, cycle) ||
            !ManagedCycle(cycle, observation) || !NativeGraph(cycle, cycleGraph) || cycleGraph != graph) continue;
        if (!Read(graph + 0xA8U, markers) || markers == 0U ||
            !Read(markers + static_cast<std::uintptr_t>(index) * 4U, observation.marker)) return false;
        observation.graph = graph;
        observation.markerIndex = index;
        return true;
    }
    return false;
}

int __cdecl HumanCoupledBoundary(const std::uintptr_t cycle,
    const std::uintptr_t victim) noexcept {
    const auto classify = gHumanPair.load(std::memory_order_acquire);
    if (classify == nullptr || !classify(cycle, victim)) return 0;
    std::uintptr_t source{}, target{}, victimOwner{}, victimCycle{}, linked{}, linkedSource{};
    std::uintptr_t definition{}, definitionTable{}, getter{};
    std::uint32_t targetBirth{}, currentTargetBirth{}, sourceBirth{}, linkedSourceBirth{};
    std::uint8_t prepared{}, acquired{};
    // 006556D0's player branch consumes the victim's current cycle and the
    // source animation definition. Require the native acquired 4E/4F handshake
    // as well as the caller's published participant identity before selecting it.
    if (!CurrentCycle(cycle, kSourceTypeRva, 0x4EU, source) ||
        !Read(cycle + 0x44U, target) || target != victim ||
        !Read(cycle + 0x48U, targetBirth) || !Read(victim + 0x50U, currentTargetBirth) ||
        targetBirth != currentTargetBirth || !Read(cycle + 0xE7U, prepared) || prepared == 0U ||
        !Read(cycle + 0x4CU, definition) || definition == 0U ||
        !Read(definition, definitionTable) || !Read(definitionTable + 4U, getter) ||
        getter < gBase + kCodeBegin || getter >= gBase + kCodeEnd ||
        !Read(victim + 0x36CU, victimCycle) ||
        !CurrentCycle(victimCycle, kVictimTypeRva, 0x4FU, victimOwner) || victimOwner != victim ||
        !Read(victimCycle + 0xB0U, linked) || linked != cycle ||
        !Read(victimCycle + 0x68U, linkedSource) || linkedSource != source ||
        !Read(victimCycle + 0x6CU, linkedSourceBirth) ||
        !Read(source + 0x50U, sourceBirth) || sourceBirth != linkedSourceBirth ||
        !Read(victimCycle + 0xD9U, acquired) || acquired == 0U) return 0;
    gCoupledPlayerUpdates.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}

// 006556F0: ESI=the source 4E. A human proxy takes the existing player
// integration branch at 0065577B. The original function remains the sole writer
// of source/victim displacement, physical state, collision and graph progress.
__declspec(naked) void HumanCoupledShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push dword ptr [esi + 44h]
        push esi
        call HumanCoupledBoundary
        add esp, 8
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gCoupledClassificationTrampoline]
    participant:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gCoupledPlayerContinue]
    }
}

int __cdecl HumanAlignmentBoundary(const std::uintptr_t cycle,
    const std::uintptr_t character) noexcept {
    const auto classify = gHumanPair.load(std::memory_order_acquire);
    std::uintptr_t source{}, victim{};
    if (classify == nullptr || !CurrentCycle(cycle, kSourceTypeRva, 0x4EU, source) ||
        !Read(cycle + 0x44U, victim) || (character != source && character != victim)) return 0;
    // Preparation calls this before E7/the 4F handshake exists. Classification
    // still requires the published, exact human pair, just as the retail player
    // branch does; never fabricate an acquired victim or an animation event.
    if (!classify(cycle, victim)) return 0;
    gHumanAlignmentUpdates.fetch_add(1U, std::memory_order_relaxed);
    return 1;
}

// 0064E70F, EBX=source 4E, ESI=the Character being aligned. Retail uses
// victim+31C/+48 again here, independently of 006556D0's coupling branch.
// Human representations must keep the player branch's XMM0=XMM1=1, rather
// than applying the NPC time ramp to Character+1004. Native pose integration,
// collision and graph progression after 0064E771 remain untouched.
__declspec(naked) void HumanAlignmentShim() {
    __asm {
        pushfd
        pushad
        mov edi, esp
        sub esp, 210h
        and esp, 0FFFFFFF0h
        fxsave [esp]
        cld
        push esi
        push ebx
        call HumanAlignmentBoundary
        add esp, 8
        test eax, eax
        jnz participant
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        jmp dword ptr [gAlignmentClassificationTrampoline]
    participant:
        fxrstor [esp]
        mov esp, edi
        popad
        popfd
        push eax
        mov eax, dword ptr [gUnitFloatAddress]
        movss xmm1, dword ptr [eax]
        movaps xmm0, xmm1
        pop eax
        jmp dword ptr [gAlignmentPlayerContinue]
    }
}

void __fastcall SourceUpdateDetour(void* context, void*) noexcept {
    const auto original = reinterpret_cast<void(__thiscall*)(void*)>(
        gSourceUpdateOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return;
    Observation observation{};
    const bool tracked = ManagedCycle(reinterpret_cast<std::uintptr_t>(context), observation);
    if (tracked) {
        observation.boundary = Boundary::SourceUpdateEnter;
        observation.callerRva = CallerRva(_ReturnAddress());
        static_cast<void>(NativeGraph(observation.cycle, observation.graph));
        gSourceUpdates.fetch_add(1U, std::memory_order_relaxed);
        gLastSourceUpdateCallerRva.store(observation.callerRva, std::memory_order_relaxed);
        Observe(observation);
    }
    original(context); // Exactly the existing native update; no synthetic tick.
    if (tracked) {
        observation.boundary = Boundary::SourceUpdateLeave;
        Observe(observation);
    }
}

std::uint32_t __fastcall MarkerCallbackDetour(void* context, void*,
    const std::uintptr_t event, const std::uint32_t argumentB, const std::uint32_t argumentC) noexcept {
    const auto original = reinterpret_cast<std::uint32_t(__thiscall*)(void*, std::uintptr_t,
        std::uint32_t, std::uint32_t)>(gMarkerCallbackOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    Observation observation{};
    std::uintptr_t graph{};
    std::uint32_t index{};
    std::uint8_t eventFlags{};
    const auto callback = reinterpret_cast<std::uintptr_t>(context);
    if (Read(callback + 0x14U, graph) && Read(callback + 0x10U, index) &&
        Read(event + 0x118U, eventFlags) && MarkerObservation(graph, index, observation)) {
        observation.callerRva = CallerRva(_ReturnAddress());
        observation.boundary = (eventFlags & 4U) != 0U
            ? Boundary::MarkerCallback : Boundary::MarkerCallbackSkipped;
        gMarkerCallbacks.fetch_add(1U, std::memory_order_relaxed);
        if ((eventFlags & 4U) == 0U)
            gMarkerCallbacksSkipped.fetch_add(1U, std::memory_order_relaxed);
        gLastMarker.store(observation.marker, std::memory_order_relaxed);
        gLastMarkerCallerRva.store(observation.callerRva, std::memory_order_relaxed);
        Observe(observation);
    }
    return original(context, event, argumentB, argumentC); // Retail RET 0C.
}

std::uint32_t __fastcall MarkerDispatchDetour(void* context, void*, const std::uint32_t index) noexcept {
    const auto original = reinterpret_cast<std::uint32_t(__thiscall*)(void*, std::uint32_t)>(
        gMarkerDispatchOriginal.load(std::memory_order_acquire));
    if (original == nullptr) return 0U;
    Observation observation{};
    const bool tracked = MarkerObservation(reinterpret_cast<std::uintptr_t>(context), index, observation);
    if (tracked) {
        observation.boundary = Boundary::MarkerDispatchEnter;
        observation.callerRva = CallerRva(_ReturnAddress());
        gMarkerDispatches.fetch_add(1U, std::memory_order_relaxed);
        Observe(observation);
    }
    const auto result = original(context, index); // Retail RET 4; preserve EAX.
    if (tracked) {
        observation.boundary = Boundary::MarkerDispatchLeave;
        Observe(observation);
    }
    return result;
}

bool Hook(HookManager& hooks, const char* name, const std::uintptr_t rva,
    void* detour, const std::initializer_list<std::uint8_t> prefix,
    void** trampoline, std::atomic<void*>* published, std::string& error) {
    HookRequest request{};
    request.name = name;
    request.target = reinterpret_cast<void*>(gBase + rva);
    request.detour = detour;
    request.patchLength = prefix.size();
    request.expected.bytes.assign(prefix);
    request.expected.mask.assign(prefix.size(), 'x');
    request.relocationSafePrefix = true;
    request.trampolineOut = trampoline;
    request.publishedTrampolineOut = published;
    return hooks.Install(request, error);
}

bool InvokeScriptedTransition(const std::uintptr_t cycle,
    const std::array<std::uint32_t, 22U>& parameters) noexcept {
    using Transition = void(__thiscall*)(void*, const std::uint32_t*, std::uint32_t);
    const auto transition = reinterpret_cast<Transition>(gBase + 0x002579F0U);
    __try {
        transition(reinterpret_cast<void*>(cycle), parameters.data(), 0U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ScriptedContract() noexcept {
    std::uintptr_t update{}, initialize{}, marker{};
    return Read(gBase + kScriptedTypeRva + 0x20U, update) && update == gBase + 0x00256BE0U &&
        Read(gBase + kScriptedTypeRva + 0x60U, initialize) && initialize == gBase + 0x0026C4C0U &&
        Read(gBase + kScriptedTypeRva + 4U, marker) && marker == gBase + 0x0024F290U &&
        Match(0x002579F0U, {0x56,0x8B,0xF1,0x8A,0x46,0x1A,0x8B,0x4E,0x04}) &&
        Match(0x00257A13U, {0x8B,0x06,0x8B,0x10,0x6A,0x01,0x8B,0xCE,0xFF,0xD2,
            0x8B,0x44,0x24,0x0C,0x8B,0x4C,0x24,0x08,0x50,0x51,0x8B,0xCE,
            0xE8,0x82,0xEE,0xFF,0xFF,0x8B,0x16,0x8B,0x42,0x60,0x8B,0xCE,0xFF,0xD0,
            0x5E,0xC2,0x08,0x00}) &&
        Match(0x00256914U, {0x8D,0x78,0x44,0xB9,0x16,0x00,0x00,0x00}) &&
        Match(0x00256936U, {0xF3,0xA5,0x5F,0x89,0x90,0x9C,0x00,0x00,0x00,0x5E,0xC2,0x08,0x00}) &&
        Match(0x0026C4E5U, {0x8B,0x46,0x68}) &&
        Match(0x0026C503U, {0xE8,0x58,0xAE,0x3C,0x00}) &&
        Match(0x0026C844U, {0x8B,0x46,0x70}) &&
        Match(0x0026C866U, {0x8B,0x46,0x74}) &&
        Match(0x0026C888U, {0x8B,0x46,0x6C}) &&
        Match(0x0026C88CU, {0x8B,0x4E,0x78}) &&
        Match(0x0026C8A0U, {0xF3,0x0F,0x10,0x46,0x7C}) &&
        Match(0x0026C8C8U, {0xE8,0xD3,0x90,0x03,0x00});
}

} // namespace

bool Install(const std::uintptr_t base, HookManager& hooks,
    const Callbacks& callbacks, std::string& error) {
    if (base < 0x10000U || !hooks.MutationAllowed() || callbacks.humanPair == nullptr ||
        gInstalled.load(std::memory_order_acquire)) {
        error = "AISync animation requires the certified x86 build and a published human-pair classifier";
        return false;
    }
    gBase = base;
    // All displaced instructions and continuations are independently checked.
    // None of the four prefixes carries a relative or ASLR-adjusted operand.
    if (!Match(0x002556F0U, {0x8B,0x46,0x44,0x8B,0x80,0x1C,0x03,0x00,0x00}) ||
        !Match(0x002556F9U, {0x85,0xC0,0x74,0x06,0x80,0x78,0x48,0x00,0x75,0x78}) ||
        !Match(0x0025577BU, {0x8B,0x4E,0x4C,0x8B,0x11,0x8B,0x42,0x04,0xFF,0xD0}) ||
        !Match(0x00255999U, {0xC3}) ||
        !Match(0x0024E70FU, {0x8B,0x43,0x44,0x8B,0x80,0x1C,0x03,0x00,0x00}) ||
        !Match(0x0024E723U, {0x85,0xC0,0x74,0x06,0x80,0x78,0x48,0x00,0x75,0x44}) ||
        !Match(0x0024E771U, {0x80,0x7D,0x0C,0x00,0xF3,0x0F,0x11,0x86,0x04,0x10,0x00,0x00}) ||
        !Match(0x00DBED34U, {0x00,0x00,0x80,0x3F}) ||
        !Match(0x00263E00U, {0x56,0x8B,0xF1,0x8B,0x46,0x44}) ||
        !Match(0x00291220U, {0x8B,0x44,0x24,0x04,0xF6,0x80,0x18,0x01,0x00,0x00,0x04}) ||
        !Match(0x0029122DU, {0x8B,0x51,0x10,0x8B,0x49,0x14,0x52,0xE8,0xC7,0xFD,0xFF,0xFF}) ||
        !Match(0x00291239U, {0xC2,0x0C,0x00}) ||
        !Match(0x002761C5U, {0x8B,0x46,0x14,0x8B,0x48,0x50}) ||
        !Match(0x00291000U, {0x55,0x8B,0x6C,0x24,0x08}) ||
        !Match(0x00291040U, {0x8B,0x97,0x84,0x00,0x00,0x00,0x8B,0x04,0x13,
            0x8B,0x0C,0xB0,0x8B,0x87,0xA8,0x00,0x00,0x00,0x8B,0x11,
            0x8B,0x04,0xA8,0x8B,0x52,0x04,0x50,0xFF,0xD2}) ||
        !Match(0x00291068U, {0xC2,0x04,0x00}) || !ScriptedContract()) {
        error = "AISync animation: paired coupling or native marker ABI mismatch";
        return false;
    }
    gCoupledPlayerContinue = reinterpret_cast<void*>(base + 0x0025577BU);
    gAlignmentPlayerContinue = reinterpret_cast<void*>(base + 0x0024E771U);
    gUnitFloatAddress = base + 0x00DBED34U;
    gHumanPair.store(callbacks.humanPair, std::memory_order_release);
    gManagedSource.store(callbacks.managedSource, std::memory_order_release);
    gObserver.store(callbacks.observer, std::memory_order_release);
    gCanReplayScripted.store(callbacks.canReplayScripted, std::memory_order_release);
    const auto checkpoint = hooks.InstalledCount();
    const bool installed =
        Hook(hooks, "aisync_paired_coupled_human_execution", 0x002556F0U,
            reinterpret_cast<void*>(&HumanCoupledShim), {0x8B,0x46,0x44,0x8B,0x80,0x1C,0x03,0x00,0x00},
            &gCoupledClassificationTrampoline, nullptr, error) &&
        Hook(hooks, "aisync_paired_human_alignment", 0x0024E70FU,
            reinterpret_cast<void*>(&HumanAlignmentShim), {0x8B,0x43,0x44,0x8B,0x80,0x1C,0x03,0x00,0x00},
            &gAlignmentClassificationTrampoline, nullptr, error) &&
        Hook(hooks, "aisync_paired_native_source_update", 0x00263E00U,
            reinterpret_cast<void*>(&SourceUpdateDetour), {0x56,0x8B,0xF1,0x8B,0x46,0x44},
            &gSourceUpdateTrampoline, &gSourceUpdateOriginal, error) &&
        Hook(hooks, "aisync_native_graph_marker_callback", 0x00291220U,
            reinterpret_cast<void*>(&MarkerCallbackDetour), {0x8B,0x44,0x24,0x04,0xF6,0x80,0x18,0x01,0x00,0x00,0x04},
            &gMarkerCallbackTrampoline, &gMarkerCallbackOriginal, error) &&
        Hook(hooks, "aisync_native_graph_marker_dispatch", 0x00291000U,
            reinterpret_cast<void*>(&MarkerDispatchDetour), {0x55,0x8B,0x6C,0x24,0x08},
            &gMarkerDispatchTrampoline, &gMarkerDispatchOriginal, error);
    if (!installed) {
        std::string rollback;
        static_cast<void>(hooks.UninstallFrom(checkpoint, rollback));
        if (!rollback.empty()) error += "; rollback: " + rollback;
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    return true;
}

bool ValidScripted(const ScriptedState& state) noexcept {
    if (state.flags != kScriptedValid || state.resourceToken == 0U || state.animationToken == 0U ||
        (state.nativeFlags & ~kScriptedNativeFlags) != 0U ||
        state.startFrame < -1 || state.startFrame > 1000000 ||
        state.endFrame < -1 || state.endFrame > 1000000 ||
        state.completionCount < -1 || state.completionCount > 65535) return false;
    const auto bounded = [](const float value, const float low, const float high) noexcept {
        return std::isfinite(value) && value >= low && value <= high;
    };
    return bounded(state.playbackRate, 0.0F, 64.0F) &&
        bounded(state.rootBlendInSeconds, 0.0F, 60.0F) &&
        bounded(state.externalStopTimeSeconds, -1.0F, 1000000.0F) &&
        bounded(state.rootBlendOutSeconds, 0.0F, 60.0F) &&
        bounded(state.graphValue50, -100000.0F, 100000.0F);
}

bool InspectScripted(const std::uintptr_t character, ScriptedContext& context) noexcept {
    context = {};
    std::uintptr_t cycle{}, owner{};
    std::array<std::uint32_t, 22U> parameters{};
    if (gBase == 0U || !Read(character + 0x36CU, cycle) ||
        !CurrentCycle(cycle, kScriptedTypeRva, 0x12U, owner) || owner != character ||
        !crash_guard::TryCopy(parameters.data(), reinterpret_cast<const void*>(cycle + 0x44U), sizeof(parameters)) ||
        !Read(cycle + 0x9CU, context.initialCountOffset)) return false;
    for (std::size_t i = 0U; i < context.references.size(); ++i)
        context.references[i] = NativeReference{parameters[i * 2U], parameters[i * 2U + 1U]};
    context.externalClockOrigin = std::bit_cast<float>(parameters[8]);
    context.reserved84 = parameters[16];
    auto& state = context.state;
    state.flags = kScriptedValid;
    state.resourceToken = parameters[9];
    state.animationToken = parameters[10];
    state.startFrame = std::bit_cast<std::int32_t>(parameters[11]);
    state.endFrame = std::bit_cast<std::int32_t>(parameters[12]);
    state.completionCount = std::bit_cast<std::int32_t>(parameters[13]);
    state.playbackRate = std::bit_cast<float>(parameters[14]);
    state.rootBlendInSeconds = std::bit_cast<float>(parameters[15]);
    state.signalToken = parameters[17];
    state.externalStopTimeSeconds = std::bit_cast<float>(parameters[18]);
    state.rootBlendOutSeconds = std::bit_cast<float>(parameters[19]);
    state.graphValue50 = std::bit_cast<float>(parameters[20]);
    state.nativeFlags = parameters[21];
    return true;
}

bool CaptureScripted(const std::uintptr_t character, ScriptedState& state) noexcept {
    state = {};
    ScriptedContext context{};
    if (!InspectScripted(character, context)) return false;
    std::uintptr_t cycle{}, owner{}, graph{};
    std::uint8_t flags{};
    // A typed failed initializer and a completed graph are acknowledgements,
    // not invitations to start a new animation on another machine.
    if (!Read(character + 0x36CU, cycle) ||
        !CurrentCycle(cycle, kScriptedTypeRva, 0x12U, owner) || owner != character ||
        !Read(cycle + 0x14U, graph) || graph == 0U ||
        !Read(cycle + 8U, flags) || (flags & 6U) != 0U) return false;
    bool referencesAbsent = true;
    for (const auto& reference : context.references)
        referencesAbsent = referencesAbsent && reference.entity == 0U && reference.resource == 0U;
    if (!referencesAbsent || context.externalClockOrigin != 0.0F || context.reserved84 != 0U ||
        context.initialCountOffset != 0U || !ValidScripted(context.state)) {
        gScriptedUnsupported.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    state = context.state;
    gScriptedCaptured.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

bool ApplyScripted(const std::uintptr_t character, const ScriptedState& state,
    std::string& error) noexcept {
    const auto canReplay = gCanReplayScripted.load(std::memory_order_acquire);
    std::uintptr_t cycle{}, owner{};
    std::uint32_t runtimeId{};
    std::uint8_t currentState{};
    if (!gInstalled.load(std::memory_order_acquire) || !ValidScripted(state) ||
        canReplay == nullptr || !canReplay(character) ||
        !Read(character + 0x50U, runtimeId) || !Read(character + 0x36CU, cycle) ||
        !Read(cycle + 4U, owner) || owner != character || !Read(cycle + 0x1AU, currentState) ||
        currentState == 0x42U || currentState == 0x4EU || currentState == 0x4FU) {
        error = "AISync scripted animation: unsupported parameters, lifetime, or execution ownership";
        return false;
    }
    std::array<std::uint32_t, 22U> parameters{};
    parameters[9] = state.resourceToken;
    parameters[10] = state.animationToken;
    parameters[11] = std::bit_cast<std::uint32_t>(state.startFrame);
    parameters[12] = std::bit_cast<std::uint32_t>(state.endFrame);
    parameters[13] = std::bit_cast<std::uint32_t>(state.completionCount);
    parameters[14] = std::bit_cast<std::uint32_t>(state.playbackRate);
    parameters[15] = std::bit_cast<std::uint32_t>(state.rootBlendInSeconds);
    parameters[17] = state.signalToken;
    parameters[18] = std::bit_cast<std::uint32_t>(state.externalStopTimeSeconds);
    parameters[19] = std::bit_cast<std::uint32_t>(state.rootBlendOutSeconds);
    parameters[20] = std::bit_cast<std::uint32_t>(state.graphValue50);
    parameters[21] = state.nativeFlags;
    std::uintptr_t current{}, graph{};
    std::uint32_t currentRuntimeId{}, resource{}, animation{};
    std::uint8_t flags{};
    if (!InvokeScriptedTransition(cycle, parameters) ||
        !Read(character + 0x50U, currentRuntimeId) || currentRuntimeId != runtimeId ||
        !Read(character + 0x36CU, current) || current != cycle ||
        !CurrentCycle(current, kScriptedTypeRva, 0x12U, owner) || owner != character ||
        !Read(current + 0x68U, resource) || resource != state.resourceToken ||
        !Read(current + 0x6CU, animation) || animation != state.animationToken ||
        !Read(current + 0x14U, graph) || graph == 0U ||
        !Read(current + 8U, flags) || (flags & 4U) != 0U) {
        gScriptedFailed.fetch_add(1U, std::memory_order_relaxed);
        error = "AISync scripted animation: native resource/animation initializer did not admit the cycle";
        return false;
    }
    gScriptedApplied.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

Counters Snapshot() noexcept {
    Counters counters{};
    counters.sourceUpdates = gSourceUpdates.load(std::memory_order_relaxed);
    counters.coupledPlayerUpdates = gCoupledPlayerUpdates.load(std::memory_order_relaxed);
    counters.humanAlignmentUpdates = gHumanAlignmentUpdates.load(std::memory_order_relaxed);
    counters.markerCallbacks = gMarkerCallbacks.load(std::memory_order_relaxed);
    counters.markerCallbacksSkipped = gMarkerCallbacksSkipped.load(std::memory_order_relaxed);
    counters.markerDispatches = gMarkerDispatches.load(std::memory_order_relaxed);
    counters.boundedListenerScans = gBoundedListenerScans.load(std::memory_order_relaxed);
    counters.scriptedCaptured = gScriptedCaptured.load(std::memory_order_relaxed);
    counters.scriptedUnsupported = gScriptedUnsupported.load(std::memory_order_relaxed);
    counters.scriptedApplied = gScriptedApplied.load(std::memory_order_relaxed);
    counters.scriptedFailed = gScriptedFailed.load(std::memory_order_relaxed);
    counters.lastSourceUpdateCallerRva = gLastSourceUpdateCallerRva.load(std::memory_order_relaxed);
    counters.lastMarker = gLastMarker.load(std::memory_order_relaxed);
    counters.lastMarkerCallerRva = gLastMarkerCallerRva.load(std::memory_order_relaxed);
    return counters;
}

} // namespace aimp::ai_sync::animation
