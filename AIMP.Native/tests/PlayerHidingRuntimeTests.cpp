#include "aimp/PlayerHidingRuntime.h"
#include "aimp/ExperimentalProtocol.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
int failures{};
void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
template<class T, class Buffer> void Store(Buffer& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
std::array<std::uint8_t, 0x4400> actor{}, second{}, local{};
std::array<std::uint8_t, 0x100> definition{}, root{}, context{};
std::array<std::uint8_t, 0x3C + 0x400 * 0x9C> manager{}, replacement{};
std::uintptr_t localAddress{};
constexpr std::array<float, 3> anchor{10, 20, 30};
constexpr std::size_t Flags(std::size_t index) { return 0x3C + index * 0x9C + 0x96; }
void Icon(std::size_t index, const char* prompt, float x, std::uint32_t key) {
    const auto offset = 0x3C + index * 0x9C;
    std::memset(manager.data() + offset, 0, 0x9C);
    std::memcpy(manager.data() + offset, prompt, std::strlen(prompt) + 1);
    for (const auto position : {0x40U, 0x4CU}) {
        Store(manager, offset + position, x);
        Store(manager, offset + position + 4, 20.0F);
        Store(manager, offset + position + 8, 30.0F);
    }
    Store(manager, offset + 0x58, key);
    Store(manager, offset + 0x5C, std::uint32_t{1});
    manager[Flags(index)] = 7;
}
}

int main() {
    using namespace aimp::player_hiding;
    const std::string commandPrefix = R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0","directionZ":"0","intensity":"1.1","frameCount":"0","directionSpace":"world","replicationPositionX":"10","replicationPositionY":"20","replicationPositionZ":"30","replicationYaw":"0","moveType":"2","playerHiding":)";
    aimp::experimental::GameCommandRequest request{};
    const auto decodeCommand = [&](const std::string& json) {
        return aimp::experimental::DecodeGameCommand(
            std::as_bytes(std::span(json.data(), json.size())), request);
    };
    Check(decodeCommand(commandPrefix + "\"1,10,20,30\"}}") &&
        request.characterDirectMove.hasPlayerHiding && request.characterDirectMove.playerHidden &&
        request.characterDirectMove.playerHidingAnchor == anchor, "Native IPC admits hidden state");
    Check(decodeCommand(commandPrefix + "\"0\"}}") && !request.characterDirectMove.playerHidden,
        "Native IPC admits the visible edge");
    Check(!decodeCommand(commandPrefix + "true}}"), "Native IPC refuses a non-string hiding field");
    Check(!decodeCommand(commandPrefix + "\"1,NaN,0,0\"}}"), "Native IPC refuses a malformed anchor");
    auto incomplete = commandPrefix + "\"0\"}}";
    const auto yawField = incomplete.find("\"replicationYaw\":\"0\",");
    incomplete.erase(yawField, std::strlen("\"replicationYaw\":\"0\","));
    Check(!decodeCommand(incomplete), "Native hiding requires the complete replication envelope");
    bool hidden{};
    std::array<float, 3> decoded{};
    Check(Decode("1,10,20,30", hidden, decoded) && hidden && decoded == anchor, "Native hidden codec");
    Check(Decode("0", hidden, decoded) && !hidden, "Native visible codec");
    for (const auto* text : {"1", "1,1,2", "1,1,2,3,4", "1,NaN,0,0", "1,1000001,0,0", "0,1,2,3"})
        Check(!Decode(text, hidden, decoded), "Malformed hiding state rejected");
    Check(!ValidAnchor({std::numeric_limits<float>::infinity(), 0, 0}), "Non-finite anchor rejected");

    constexpr std::size_t size = 0x1300000;
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (memory == nullptr) return 2;
    const auto image = reinterpret_cast<std::uintptr_t>(memory);
    const auto character = reinterpret_cast<std::uintptr_t>(actor.data());
    const auto other = reinterpret_cast<std::uintptr_t>(second.data());
    // Synthetic thiscall body: real validated entry/return bytes, render calls
    // replaced by NOPs. Exercises the native stack ABI and +2D9 readback only.
    const std::array<std::uint8_t, 20> prologue{
        0x53,0x8B,0x5C,0x24,0x08,0x56,0x8B,0xF1,0x8D,0x8E,
        0xF0,0x03,0x00,0x00,0x88,0x9E,0xD9,0x02,0x00,0x00};
    const std::array<std::uint8_t, 5> epilogue{0x5E,0x5B,0xC2,0x04,0x00};
    std::memset(memory + 0x38B100, 0x90, 0x3C);
    std::memcpy(memory + 0x38B100, prologue.data(), prologue.size());
    std::memcpy(memory + 0x38B137, epilogue.data(), epilogue.size());
    Store(actor, 0x64, reinterpret_cast<std::uintptr_t>(definition.data()));
    Store(second, 0x64, reinterpret_cast<std::uintptr_t>(definition.data()));
    Store(local, 0x64, reinterpret_cast<std::uintptr_t>(definition.data()));
    localAddress = reinterpret_cast<std::uintptr_t>(local.data());
    Store(context, 0x80, reinterpret_cast<std::uintptr_t>(&localAddress));
    Store(context, 0x84, std::uint32_t{1});
    Store(root, 8, reinterpret_cast<std::uintptr_t>(context.data()));
    *reinterpret_cast<std::uintptr_t*>(memory + 0x12F0C88) = reinterpret_cast<std::uintptr_t>(root.data());
    *reinterpret_cast<std::uintptr_t*>(memory + 0x12F0DE8) = reinterpret_cast<std::uintptr_t>(manager.data());
    FlushInstructionCache(GetCurrentProcess(), memory, size);
    Icon(0, "AI_UI_INTERACT_HIDING", 10, 100);
    Icon(1, "AI_UI_INTERACT_HIDING", 12, 101);
    Icon(2, "AI_UI_INTERACT_PICKUP", 10, 102);
    Initialize(image);
    std::string error;
    Check(Publish(character, true, anchor, error), "Entry publication accepted");
    Check(History().hiddenEdges == 1 && History().hides == 0 &&
        History().lastHiddenRequestedCharacter == character, "Publication history distinguishes requests from native hides");
    Check(actor[0x2D9] == 0 && manager[Flags(0)] == 7, "IPC never writes the engine objects");
    PumpAllOnEngineThread();
    Check(actor[0x2D9] == 1 && Snapshot(character).spotLocked, "Engine hides the remote body and locks the spot");
    Check(History().hides == 1 && History().lastHiddenCharacter == character,
        "History identifies the actual masked character");
    Check(manager[Flags(0)] == 4 && manager[Flags(1)] == 7 && manager[Flags(2)] == 7,
        "Only the occupied hiding interaction is disabled");
    manager[Flags(0)] = 0x87;
    PumpAllOnEngineThread();
    Check(manager[Flags(0)] == 0x84, "Retail icon event is suppressed without losing unrelated flags");
    Check(Publish(character, false, {}, error), "Exit publication accepted");
    PumpAllOnEngineThread();
    Check(actor[0x2D9] == 0 && manager[Flags(0)] == 0x87, "Exit reveals the body and restores icon bits");
    Check(Snapshot(character).hides == 0 && History().hides == 1 && History().shows == 1 &&
        History().lastShownCharacter == character, "History survives per-stay mailbox release");

    Check(Publish(character, true, anchor, error), "First concurrent occupant");
    Check(Publish(other, true, anchor, error), "Second concurrent occupant");
    PumpAllOnEngineThread();
    Check(Publish(character, false, {}, error), "First concurrent exit");
    PumpAllOnEngineThread();
    Check(manager[Flags(0)] == 0x84, "A remaining occupant retains the lock");
    Forget(other);
    second[0x2D9] = 77;
    PumpAllOnEngineThread();
    Check(manager[Flags(0)] == 0x87 && second[0x2D9] == 77,
        "Destruction releases the final lock without touching the dead actor");

    Check(Publish(localAddress, true, anchor, error), "Local guard test publication");
    PumpAllOnEngineThread();
    Check(local[0x2D9] == 0 && manager[Flags(0)] == 0x87, "Local player guard also protects the interaction table");
    Forget(localAddress);
    Check(Publish(character, true, anchor, error), "Re-enter before icon reuse");
    PumpAllOnEngineThread();
    Icon(0, "AI_UI_INTERACT_PICKUP", 10, 999);
    Check(Publish(character, false, {}, error), "Exit after slot reuse");
    PumpAllOnEngineThread();
    Check(manager[Flags(0)] == 7, "Recycled slots retain their own retail flags");

    Icon(0, "AI_UI_INTERACT_HIDING", 10, 100);
    Check(Publish(character, true, anchor, error), "Re-enter before manager replacement");
    PumpAllOnEngineThread();
    replacement = manager;
    replacement[Flags(0)] = 4;
    *reinterpret_cast<std::uintptr_t*>(memory + 0x12F0DE8) = reinterpret_cast<std::uintptr_t>(replacement.data());
    Check(Publish(character, false, {}, error), "Exit after manager replacement");
    PumpAllOnEngineThread();
    Check(replacement[Flags(0)] == 4, "Old manager ownership cannot restore into a new table");
    const auto history = History();
    ForgetAll();
    Check(History().hides == history.hides && History().lastHiddenCharacter == history.lastHiddenCharacter,
        "Cumulative history survives level cleanup");
    Initialize(0);
    Check(History().hides == 0 && History().lastHiddenCharacter == 0, "Initialization starts a new history");
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures == 0 ? 0 : 1;
}
