#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aimp {

enum class ModuleStatus : std::uint8_t {
    Implemented,
    PartiallyImplemented,
    VoluntaryStub,
    BlockedByResearch,
};

constexpr std::string_view ToString(const ModuleStatus status) noexcept {
    switch (status) {
    case ModuleStatus::Implemented:
        return "implemented";
    case ModuleStatus::PartiallyImplemented:
        return "partially_implemented";
    case ModuleStatus::VoluntaryStub:
        return "voluntary_stub";
    case ModuleStatus::BlockedByResearch:
        return "blocked_by_research";
    }
    return "unknown";
}

struct ModuleReport {
    std::string_view name;
    ModuleStatus status;
    std::string_view reason;
};

inline constexpr std::uint16_t kProtocolVersion = 10;
inline constexpr std::size_t kMaximumIpcPayload = 1024U * 1024U;
inline constexpr std::size_t kMaximumTransportObservationPayload = 64U * 1024U;

} // namespace aimp
