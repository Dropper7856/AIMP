#include "aimp/PlayerPostureRuntime.h"
#include "aimp/PlayerTorchRuntime.h"
#include "aimp/ExperimentalProtocol.h"
#include "aimp/Common.h"
#include "aimp/PlayerCharacters.h"

#include <algorithm>
#include <cstddef>
#include <tuple>
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
    for (const auto direction : {std::array<float, 3>{0, 0.6F, 0.8F},
             std::array<float, 3>{0, 1, 0}, std::array<float, 3>{0, -1, 0},
             std::array<float, 3>{1, 0, 0}}) {
        Check(aimp::player_torch::BuildAimTransform(direction, {2, 3, 4}, light), "Full camera ray is supported");
        Check(std::abs(light[2] - direction[0]) < 0.00001F &&
            std::abs(light[6] - direction[1]) < 0.00001F && std::abs(light[10] - direction[2]) < 0.00001F,
            "Beam forward follows replicated camera ray including vertical aim");
        const std::array<float, 3> right{light[0], light[4], light[8]}, up{light[1], light[5], light[9]};
        Check(std::abs(right[0] * up[0] + right[1] * up[1] + right[2] * up[2]) < 0.00001F &&
            std::abs(up[0] * up[0] + up[1] * up[1] + up[2] * up[2] - 1.0F) < 0.00001F,
            "Beam basis remains orthonormal at vertical aim");
        Check(light[3] == 2 && light[7] == 3 && light[11] == 4, "Remote origin preserved");
    }
    const auto validLight = light;
    Check(!aimp::player_torch::BuildAimTransform({0, 0, 0}, {2, 3, 4}, light) && light == validLight,
        "Invalid camera ray cannot mutate the transform");
    static_assert(aimp::kProtocolVersion == 32U);
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
    const std::array<float, 3> aim{0, 0.6F, 0.8F};
    Check(aimp::player_torch::Publish(character, true, error, &aim), "Replicated camera ray admitted");
    Check(aimp::player_torch::Snapshot(character).hasDirection, "Camera direction is retained");
    Check(aimp::player_torch::Publish(character, false, error), "Battery exhaustion switches off");
    Check(!aimp::player_torch::Snapshot(character).hasDirection && !aimp::player_torch::Snapshot(character).desired,
        "Off resets aim and does not inherit another lifetime");
    for (const auto* text : {"0,0.6,0.8", "0,1,0", "0,-1,0", "1,0,0"}) {
        const auto json = torchPrefix + "\"1\",\"playerTorchDirection\":\"" + text + "\"}}";
        Check(aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(json.data(), json.size())), request)
            && request.characterDirectMove.hasPlayerTorchDirection, "Native IPC admits full camera direction");
    }
    for (const auto* text : {"0,0", "0,0,0", "NaN,0,1", "2,0,0", "0,0,1,0", "0,,1"}) {
        const auto json = torchPrefix + "\"1\",\"playerTorchDirection\":\"" + text + "\"}}";
        Check(!aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(json.data(), json.size())), request),
            "Native IPC rejects invalid aim");
    }
    const auto offAim = torchPrefix + "\"0\",\"playerTorchDirection\":\"0,0,1\"}}";
    Check(!aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(offAim.data(), offAim.size())), request),
        "Native IPC rejects aim on disabled torch");
    aimp::player_torch::ForgetAll();
    Check(!aimp::player_torch::Snapshot(character).desired, "Torch lifetime cleanup");
    // Crew representation choice on SpawnCharacter: optional, closed set.
    const std::string spawnPrefix = R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"SpawnCharacter","arguments":{"x":"1.000","y":"2.000","z":"3.000")";
    for (const auto& [suffix, expected, hash] : {
        std::tuple{std::string("}}"), true, 0x21F92DEAU},
        std::tuple{std::string(R"(,"character":"DALLAS"}})"), true, 0xE4CDDDE2U},
        std::tuple{std::string(R"(,"character":"ASH"}})"), true, 0x5BAD7BDFU},
        std::tuple{std::string(R"(,"character":"dallas"}})"), false, 0U},
        std::tuple{std::string(R"(,"character":"BISHOP"}})"), false, 0U},
        std::tuple{std::string(R"(,"character":null}})"), false, 0U},
        std::tuple{std::string(R"(,"other":"DALLAS"}})"), false, 0U}}) {
        aimp::experimental::GameCommandRequest spawn{};
        const auto json = spawnPrefix + suffix;
        Check(aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(json.data(), json.size())), spawn)
            == expected, "IPC spawn character validation");
        if (!expected) continue;
        const auto identity = std::find_if(aimp::player_characters::kAll.begin(),
            aimp::player_characters::kAll.end(), [hash](const auto& c) { return c.nameHash == hash; });
        Check(spawn.spawnCharacter.characterHash == hash && identity != aimp::player_characters::kAll.end() &&
            spawn.spawnCharacter.displayModelGuid == identity->displayModelGuid,
            "IPC spawn character identity decoded");
    }
    for (const auto& identity : aimp::player_characters::kAll) {
        std::uint32_t hash = 2166136261U;
        for (const auto c : identity.name) { hash ^= static_cast<std::uint8_t>(c); hash *= 16777619U; }
        Check(hash == identity.nameHash && aimp::player_characters::IsRepresentation(hash),
            "Crew name hash is the retail FNV-1a identity");
    }
    Check(!aimp::player_characters::IsRepresentation(0U), "Unknown database is not a representation");
    return failures == 0 ? 0 : 1;
}
