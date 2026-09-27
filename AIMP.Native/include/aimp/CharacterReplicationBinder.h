#pragma once

#include "aimp/Common.h"

namespace aimp {

class CharacterReplicationBinder final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "CharacterReplicationBinder",
            ModuleStatus::BlockedByResearch,
            "The channel-to-Character-to-context chain is static evidence; the inbound sink and live owner are unmeasured."};
    }

    CharacterReplicationBinder() = delete;
};

} // namespace aimp
