#include "aimp/PlayerHidingPresentationRuntime.h"
#include <Windows.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
int failures{};
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
}
template<class Buffer, class T> void Store(Buffer& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void OperationStub(std::uint8_t* code, std::uint32_t& calls, const std::uint8_t* ready,
    const std::size_t returnOffset) {
    // Real frame/RET8 ABI, synthetic resource operation: increments a counter
    // and returns AL. Does not load any game asset or run a retail function.
    std::vector<std::uint8_t> bytes{0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57,0xFF,0x05};
    const auto counter = reinterpret_cast<std::uintptr_t>(&calls);
    const auto* counterBytes = reinterpret_cast<const std::uint8_t*>(&counter);
    bytes.insert(bytes.end(), counterBytes, counterBytes + 4U);
    if (ready != nullptr) {
        bytes.push_back(0xA0);
        const auto address = reinterpret_cast<std::uintptr_t>(ready);
        const auto* addressBytes = reinterpret_cast<const std::uint8_t*>(&address);
        bytes.insert(bytes.end(), addressBytes, addressBytes + 4U);
    } else { bytes.push_back(0xB0); bytes.push_back(1U); }
    for (const auto byte : {0x5F,0x5E,0x8B,0xE5,0x5D,0xC2,0x08,0x00})
        bytes.push_back(static_cast<std::uint8_t>(byte));
    std::memcpy(code, bytes.data(), bytes.size());
    const std::uint8_t pop[3]{0xC2,0x08,0x00};
    std::memcpy(code + returnOffset, pop, sizeof(pop));
}
}
int main() {
    using namespace aimp::player_hiding_presentation;
    State state{1U, 1U, 0x5DC33E3EU, 123U, 1U, 0U, {1.0F, 2.0F, 3.0F}, 0.5F, true};
    Check(LocalPhase(0, false, state, 0, false, true) == 1U,
        "Accepted entry starts before a scripted NPC clip or hidden flag exists");
    state.elapsedMilliseconds = 750U;
    Check(LocalPhase(1, true, state, 0, false, true) == 1U,
        "First-person entry remains active on a different Character cycle");
    Check(LocalPhase(1, true, state, 0, true, true) == 2U,
        "Delayed native hiding flag settles the accepted entry");
    Check(LocalPhase(1, true, state, 0, false, false) == 0U,
        "Interrupted entry releases when its retail interaction flag clears");
    Check(LocalPhase(0, false, state, 0, false, true) == 0U,
        "An old entry latch cannot start late");
    state.phase = 3U; state.elapsedMilliseconds = 0U;
    Check(LocalPhase(2, false, state, 0, true, true) == 3U,
        "Exact exit edge identifies exit before its Character clip is sampled");
    Check(LocalPhase(3, true, state, 3, false, false) == 3U,
        "An active exit survives hidden flag cleanup");
    Check(LocalPhase(3, true, state, 0, false, false) == 0U,
        "Completed exit clears once without replaying the retained door latch");
    state.phase = 1U;
    Check(std::abs(ExitYaw(0.5F) - (0.5F - 3.14159265F)) < 0.0001F &&
        std::abs(ExitYaw(-0.5F) - (3.14159265F - 0.5F)) < 0.0001F,
        "Exit faces opposite the entry heading");
    Check(std::abs(ExitYaw(3.1F)) <= 3.3F && std::abs(ExitYaw(-3.1F)) <= 3.3F && std::abs(ExitYaw(0.0F)) <= 3.3F,
        "Exit heading stays inside the replicated yaw contract");
    aimp::ai_sync::native_intent::Payload intent{};
    for (std::uint32_t kind = 1U; kind <= 2U; ++kind) {
        for (std::uint32_t phase = 1U; phase <= 3U; ++phase) {
            const auto clip = ClassifyClip(ClipToken(kind, phase));
            Check(clip.phase == phase && clip.kind == kind, "All six actual script aliases classify");
        }
    }
    Check(ClassifyClip(0x12345678U).phase == 0U && ClipToken(3U, 1U) == 0U && ClipToken(1U, 4U) == 0U,
        "Unrelated animations cannot become hiding");
    Check(BuildPlaybackIntent(state, 569978346U, intent) && aimp::ai_sync::native_intent::Validate(intent),
        "Tall entry produces a valid contract20 command");
    const auto& w = intent.words;
    Check(w[0] == 20U && w[21] == 569978346U && w[22] == 0xCF2A9F36U,
        "Playback uses actual receiver resource and tall entry alias");
    Check(w[23] == UINT32_MAX && w[24] == UINT32_MAX && w[25] == 1U &&
        std::bit_cast<float>(w[26]) == 1.0F && std::bit_cast<float>(w[30]) == -1.0F,
        "Full clip, one completion, native speed and external-stop semantics");
    bool isolated = w[1] == 0U && w[20] == 0U && w[29] == 0U && w[33] == 0U && w[34] == 0U;
    for (std::size_t i = 12U; i < 20U; ++i) isolated = isolated && w[i] == 0U;
    Check(isolated, "Playback never imports spatial references, clocks, signals or camera flags");
    state.kind = 2U; state.entityShortGuid = 0x61B51D78U; state.phase = 3U;
    Check(BuildPlaybackIntent(state, 789U, intent) && intent.words[21] == 789U && intent.words[22] == 0x3FDD3AF4U,
        "Low exit keeps receiver-selected resource");
    state.phase = 2U;
    Check(!BuildPlaybackIntent(state, 789U, intent), "Settled hidden never creates an idle loop");
    state.phase = 1U;
    Check(!BuildPlaybackIntent(state, 0U, intent), "Missing receiver animation resource fails closed");
    state.entityShortGuid = 0x5DC33E3EU;
    Check(!BuildPlaybackIntent(state, 789U, intent), "Icon kind and authored key must agree");
    state.entityShortGuid = 0x61B51D78U;
    state.hierarchyHash = 0U;
    Check(!BuildPlaybackIntent(state, 789U, intent), "Template-only identity cannot select a prop instance");
    state.hierarchyHash = 123U; state.animate = false;
    Initialize(0U);
    std::string error;
    constexpr std::uintptr_t actor = 0x12340000U;
    Check(Publish(actor, state, error) && !Active(actor), "Baseline publication does not arm animation");
    state.animate = true;
    Check(Publish(actor, state, error) && !Active(actor), "Same revision cannot turn a baseline into a replay");
    state.phase = 3U;
    Check(!Publish(actor, state, error), "Same revision cannot change phase");
    state.transition = 2U;
    Check(Publish(actor, state, error) && Active(actor), "New live transition arms engine work");
    State old = state; old.transition = 1U;
    Check(Publish(actor, old, error) && Active(actor), "Old publication cannot replace the pending transition");
    Check(Publish(actor, {}, error) && !Active(actor), "Clear cancels pending value work without dereferencing actor");
    Check(Publish(actor, state, error) && !Active(actor), "Cleared revision cannot resurrect a stay");
    state.transition = 3U; state.phase = 2U; state.animate = false;
    Check(Publish(actor, state, error) && Active(actor), "Hidden baseline retains movement ownership");
    Forget(actor);
    Check(!Active(actor), "Lifecycle forget releases value ownership");
    state.transition = 1U; state.phase = 1U; state.animate = true;
    Check(Publish(actor, state, error) && Active(actor), "Fresh binding has its own revision history");
    ForgetAll();

    // Read-only native layout fixtures exercise early classification without
    // calling any retail function. No exact door latch means no guessed key.
    std::array<std::uint8_t, 0x4300U> local{};
    std::array<std::uint8_t, 0x200U> cycle{}, root{}, context{};
    const auto localAddress = reinterpret_cast<std::uintptr_t>(local.data());
    const auto cycleAddress = reinterpret_cast<std::uintptr_t>(cycle.data());
    std::uintptr_t players[1]{localAddress};
    auto* image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1300000U, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (image == nullptr) return 2;
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    const auto rootAddress = reinterpret_cast<std::uintptr_t>(root.data());
    std::memcpy(image + 0x12F0C88U, &rootAddress, sizeof(rootAddress));
    Store(root, 8U, reinterpret_cast<std::uintptr_t>(context.data()));
    Store(context, 0x80U, reinterpret_cast<std::uintptr_t>(players));
    Store(context, 0x84U, std::uint32_t{1U});
    Store(local, 0x50U, std::uint32_t{7U});
    Store(local, 0x36CU, cycleAddress);
    Store(cycle, 4U, localAddress);
    cycle[0x1AU] = 1U;
    Initialize(base);
    State captured{};
    Check(CaptureLocalOnEngineThread(localAddress, captured) && captured.phase == 0U,
        "Visible actual local Character returns empty State");
    Store(cycle, 0U, base + 0xEADB58U);
    Store(cycle, 0x14U, std::uintptr_t{0x12340000U});
    Store(cycle, 0x68U, std::uint32_t{2554777613U});
    Store(cycle, 0x6CU, std::uint32_t{0xCF2A9F36U});
    cycle[0x1AU] = 0x12U;
    Check(!CaptureLocalOnEngineThread(localAddress, captured),
        "Early active entry with flag13 false requires exact native door latch");
    local[0x4249U] = 1U;
    Check(!CaptureLocalOnEngineThread(localAddress, captured),
        "Delayed hidden flag cannot manufacture exact interaction identity");
    Store(local, 0x50U, std::uint32_t{0U});
    Check(!CaptureLocalOnEngineThread(localAddress, captured), "Unready Character is not exported");
    ForgetAll();

    std::array<std::uint8_t, 0x4300U> remote{};
    std::array<std::uint8_t, 0x80U> database{}, instance{}, table{}, resource{}, resourceControl{}, databaseControl{};
    const auto remoteAddress = reinterpret_cast<std::uintptr_t>(remote.data());
    const auto dbAddress = reinterpret_cast<std::uintptr_t>(database.data());
    const std::uint32_t clipKeys[2]{0xCF2A9F36U, 0U}, clipValues[2]{2721887201U, 0U};
    Store(remote, 0x50U, std::uint32_t{8U});
    Store(remote, 0x36CU, cycleAddress);
    Store(remote, 0x720U, reinterpret_cast<std::uintptr_t>(instance.data()));
    Store(remote, 0x42A8U, reinterpret_cast<std::uintptr_t>(resource.data()));
    Store(remote, 0x42ACU, reinterpret_cast<std::uintptr_t>(resourceControl.data()));
    Store(resourceControl, 4U, LONG{1}); Store(resourceControl, 8U, LONG{1});
    Store(databaseControl, 4U, LONG{1}); Store(databaseControl, 8U, LONG{1});
    Store(instance, 0x10U, dbAddress);
    Store(instance, 0x14U, reinterpret_cast<std::uintptr_t>(databaseControl.data()));
    Store(database, 0x10U, std::uint32_t{569978346U});
    Store(database, 0x20U, reinterpret_cast<std::uintptr_t>(table.data()));
    Store(table, 8U, reinterpret_cast<std::uintptr_t>(clipKeys));
    Store(table, 12U, reinterpret_cast<std::uintptr_t>(clipValues)); Store(table, 16U, std::uint32_t{1U});
    cycle = {}; Store(cycle, 4U, remoteAddress); cycle[0x1AU] = 1U;
    const auto put = [&](std::size_t at, std::uintptr_t value) { std::memcpy(image + at, &value, sizeof(value)); };
    put(0xEADB58U + 0x20U, base + 0x256BE0U);
    put(0xEADB58U + 0x60U, base + 0x26C4C0U);
    put(0xEADB58U + 4U, base + 0x24F290U);
    const std::uint8_t constructorBody[]{0x8B,0x06,0x8B,0x10,0x6A,0x01,0x8B,0xCE,0xFF,0xD2,
        0x8B,0x44,0x24,0x0C,0x8B,0x4C,0x24,0x08,0x50,0x51,0x8B,0xCE,0xE8,0x82,0xEE,0xFF,0xFF,
        0x8B,0x16,0x8B,0x42,0x60,0x8B,0xCE,0xFF,0xD0,0x5E,0xC2,0x08,0x00};
    const std::uint8_t copyBegin[]{0x8D,0x78,0x44,0xB9,0x16,0x00,0x00,0x00};
    const std::uint8_t copyEnd[]{0xF3,0xA5,0x5F,0x89,0x90,0x9C,0x00,0x00,0x00,0x5E,0xC2,0x08,0x00};
    std::memcpy(image + 0x257A13U, constructorBody, sizeof(constructorBody));
    std::memcpy(image + 0x256914U, copyBegin, sizeof(copyBegin));
    std::memcpy(image + 0x256936U, copyEnd, sizeof(copyEnd));
    std::uint32_t requested{}, queried{}, released{};
    std::uint8_t ready{};
    OperationStub(image + 0x639060U, requested, nullptr, 0x13AU);
    OperationStub(image + 0x636970U, queried, &ready, 0x14CU);
    OperationStub(image + 0x636BB0U, released, nullptr, 0x13AU);
    FlushInstructionCache(GetCurrentProcess(), image, 0x1300000U);
    // Nonzero original-Idle pointer is required for admission; this test never
    // reaches playback or cleanup because the synthetic stream stays pending.
    Initialize(base, base + 0x25BDD0U);
    state = {1U, 1U, 0x5DC33E3EU, 123U, 1U, 0U, {1.0F,2.0F,3.0F}, 0.5F, true};
    Check(Publish(remoteAddress, state, error), "Synthetic streamed transition published");
    Check(PumpOnEngineThread(remoteAddress) && std::strcmp(Snapshot(remoteAddress).stage, "stream-loading") == 0,
        "Streaming retains movement ownership before constructing any graph");
    Check(PumpOnEngineThread(remoteAddress) && requested == 1U && queried == 2U && released == 0U,
        "Repeated readiness polls acquire exactly once");
    Check(cycle[0x1AU] == 1U, "Pending stream never replaces native cycle");
    Check(Publish(remoteAddress, {}, error), "Clear pending stream");
    PumpAllOnEngineThread();
    Check(released == 1U && !Active(remoteAddress), "Cancellation before ready balances the stream request");
    LONG references{}; std::memcpy(&references, databaseControl.data() + 4U, sizeof(references));
    Check(references == 1, "Stream request keeps and releases its own database reference");
    PumpAllOnEngineThread();
    Check(released == 1U, "Stream request is released only once");
    ForgetAll(); Initialize(0U);
    VirtualFree(image, 0U, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
