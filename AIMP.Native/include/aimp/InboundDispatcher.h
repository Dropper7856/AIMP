#pragma once

#include "aimp/Common.h"

namespace aimp {

class InboundDispatcher final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "InboundDispatcher",
            ModuleStatus::BlockedByResearch,
            "Framing, reader construction, sink binding, and the required engine thread are not proven."};
    }

    InboundDispatcher() = delete;
};

} // namespace aimp
