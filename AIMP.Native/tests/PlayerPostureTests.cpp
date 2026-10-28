#include "aimp/PlayerPostureRuntime.h"
#include "aimp/PlayerTorchRuntime.h"
#include "aimp/ExperimentalProtocol.h"
#include "aimp/Common.h"

#include <cstddef>
#include <cmath>
#include <limits>
#include <iostream>
#include <span>
#include <string>

namespace {
int failures{};
void Check(const bool result, const char* name) {
    if (!result) { std::cerr << name << '\n'; ++failures; }
}
}

int main() {
    std::array<float, 12> light{};
    Check(aimp::player_torch::BuildYawTransform(0.0F, {2.0F, 3.0F, 4.0F}, light),
        "Remote torch transform");
    Check(light[0] == 1.0F && light[5] == 1.0F && light[10] == 1.0F
        && light[3] == 2.0F && light[7] == 3.0F && light[11] == 4.0F,
        "Torch uses native compact transform translation");
    Check(aimp::player_torch::BuildYawTransform(1.570796327F, {2.0F, 3.0F, 4.0F}, light)
        && std::abs(light[2] - 1.0F) < 0.00001F && std::abs(light[8] + 1.0F) < 0.00001F,
        "Torch follows remote yaw");
    const auto previousLight = light;
    Check(!aimp::player_torch::BuildYawTransform(std::numeric_limits<float>::quiet_NaN(),
        {2.0F, 3.0F, 4.0F}, light) && light == previousLight, "Reject invalid torch direction");
    Check(!aimp::player_torch::BuildYawTransform(0.0F,
        {std::numeric_limits<float>::infinity(), 3.0F, 4.0F}, light) && light == previousLight,
        "Reject invalid torch position");
    static_assert(aimp::kProtocolVersion == 24U);
    using namespace aimp::player_posture;
    State value{};
    Check(Decode("0,0,0,0,0,0,0", value), "Standing state");
    Check(Decode("13,123,456,1,7,4,891", value), "Active native vent entry");
    Check(value.traversalIndex == 7U && value.ventIdB == 456U, "Portable identities");
    Check(Decode("2,0,0,0,0,0,0", value), "Deep crouch is independent of CROUCHED");
    Check(Decode("0,0,0,1,7,4,0", value), "Zero authored owner ID is not the invalid sentinel");
    for (const auto* text : {"16,0,0,0,0,0,0", "8,0,0,0,7,4,891", "0,0,0,1,7,3,891",
        "0,0,0,1,65536,4,891", "0,0,0,18446744073709551616,7,4,891", "0,0,0,0,0,0,0,1",
        "0,0,0,0,0,0", "4,0,0,0,0,0,0", "0,0,0,1,7,4,4294967295"})
        Check(!Decode(text, value), text);

    // The mailbox is tested without accessing game memory or invoking retail.
    Initialize(0x400000U);
    constexpr std::uintptr_t character = 0x100000U;
    std::string error;
    State baseline{0U, 0U, 0U, 12U, 7U, 4U, 891U};
    Check(Publish(character, baseline, error), "Bind completed traversal baseline");
    Check(Snapshot(character).pendingTraversals == 0U, "Do not replay history on bind");
    baseline.flags = 8U;
    baseline.traversalRevision = 13U;
    Check(Publish(character, baseline, error), "Queue new traversal");
    Check(Publish(character, baseline, error), "Duplicate heartbeat");
    Check(Snapshot(character).pendingTraversals == 1U, "Deduplicate traversal revision");
    for (std::uint64_t i = 14U; i <= 20U; ++i) {
        baseline.traversalRevision = i;
        Check(Publish(character, baseline, error), "Queue within bound");
    }
    baseline.traversalRevision = 21U;
    Check(!Publish(character, baseline, error), "Backpressure instead of lost entry/exit");
    baseline.traversalRevision = 11U;
    Check(!Publish(character, baseline, error), "Reject stale revision");
    Check(Publish(character, State{}, error), "Temporary Character sample clears the traversal key");
    baseline.traversalRevision = 11U;
    Check(!Publish(character, baseline, error), "Cleared key does not rewind traversal history");
    Forget(character);
    Check(Snapshot(character).pendingTraversals == 0U, "Clear lifecycle state");
    baseline.traversalRevision = 1U;
    Check(Publish(character, baseline, error), "New lifetime accepts revision restart");
    Check(Snapshot(character).pendingTraversals == 1U, "Active traversal starts on bind");
    Forget(character);
    Check(Publish(character, baseline, error), "Publish independent posture mailbox");
    ForgetAll();
    Check(Snapshot(character).pendingTraversals == 0U, "Level cleanup also clears orphan posture mailboxes");

    const std::string prefix = R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.059","directionZ":"0","intensity":"1.100","frameCount":"30","directionSpace":"world","replicationPositionX":"12.5","replicationPositionY":"19.258","replicationPositionZ":"-8.25","replicationYaw":"1.25","moveType":"2","playerPosture":)";
    aimp::experimental::GameCommandRequest request{};
    for (const auto& [suffix, expected] : {
        std::pair{std::string(R"("13,123,456,1,7,4,891"}})"), true},
        std::pair{std::string(R"("0,0,0,0,0,0,0"}})"), true},
        std::pair{std::string(R"("0,0,0,1,7,3,891"}})"), false},
        std::pair{std::string(R"(null}})"), false}}) {
        const auto json = prefix + suffix;
        const auto bytes = std::as_bytes(std::span(json.data(), json.size()));
        Check(aimp::experimental::DecodeGameCommand(bytes, request) == expected, "IPC posture validation");
        if (expected) Check(request.characterDirectMove.hasPlayerPosture, "IPC posture presence");
    }
    const std::string torchPrefix = prefix.substr(0, prefix.find("\"playerPosture\"")) + "\"playerTorch\":";
    for (const auto* text : {"\"0\"", "\"1\"", "true", "1", "null", "\"true\"", "\"01\""}) {
        const auto json = torchPrefix + text + "}}";
        const auto expected = std::string_view(text) == "\"0\"" || std::string_view(text) == "\"1\"";
        Check(aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(json.data(), json.size())), request)
            == expected, "IPC torch effective boolean validation");
        if (expected) Check(request.characterDirectMove.hasPlayerTorch &&
            request.characterDirectMove.playerTorchEnabled == (std::string_view(text) == "\"1\""),
            "IPC torch state decoded");
    }
    aimp::player_torch::Initialize(0x400000U);
    Check(aimp::player_torch::Publish(character, true, error), "Torch mailbox publication");
    Check(aimp::player_torch::Snapshot(character).desired, "Torch remains on without movement heartbeats");
    aimp::player_torch::ForgetAll();
    Check(!aimp::player_torch::Snapshot(character).desired, "Torch lifetime cleanup");
    return failures == 0 ? 0 : 1;
}
