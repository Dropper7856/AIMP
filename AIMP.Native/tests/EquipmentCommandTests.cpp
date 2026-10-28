#include "aimp/ExperimentalProtocol.h"
#include <iostream>
#include <span>
#include <string>

int main() {
    const std::string base = R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"REMOTE_EQUIPMENT_PROVISION","arguments":{"characterAddress":"0x200000","compositeName":"Required_Assets\\Archetypes\\Equipment\\Flare_Held","equipmentEntityShortGuid":"0x1234","equipmentOutputShortGuid":"0x2345","spawnSchedulerParameterShortGuid":"0x3456","spawnTargetInputShortGuid":"0x4567","equipmentSlot":"9")";
    int failures{};
    const auto check = [&](const std::string& extra, bool valid, bool activated, bool released) {
        const auto json = base + extra + "}}";
        aimp::experimental::GameCommandRequest request;
        const auto decoded = aimp::experimental::DecodeGameCommand(
            std::as_bytes(std::span(json.data(), json.size())), request);
        if (decoded != valid || (decoded && (request.remoteEquipmentProvision.activateHeld != activated ||
            request.remoteEquipmentProvision.releaseHeld != released))) {
            ++failures; std::cerr << "Equipment admission failed: " << extra << '\n';
        }
    };
    check("", true, false, false);
    check(R"(,"activateHeld":"true")", true, true, false);
    check(R"(,"releaseHeld":"true")", true, false, true);
    check(R"(,"activateHeld":"false")", false, false, false);
    check(R"(,"releaseHeld":"false")", false, false, false);
    check(R"(,"activateHeld":true)", false, false, false);
    check(R"(,"activateHeld":"true","releaseHeld":"true")", false, false, false);
    check(R"(,"unknown":"true")", false, false, false);
    for (const auto* extra : {R"(,"activateHeld":"true")", R"(,"releaseHeld":"true")"}) {
        for (const auto* source : {"Flare_Held", "\"equipmentSlot\":\"9\""}) {
            auto invalid = base;
            const std::string needle(source);
            invalid.replace(invalid.find(needle), needle.size(), source[0] == 'F' ? "Other_Item" : "\"equipmentSlot\":\"4\"");
            invalid += extra; invalid += "}}";
            aimp::experimental::GameCommandRequest request;
            if (aimp::experimental::DecodeGameCommand(std::as_bytes(std::span(invalid.data(), invalid.size())), request))
                ++failures;
        }
    }
    if (failures != 0) return 1;
    std::cout << "Equipment command admission: 12 cases passed.\n";
    return 0;
}
