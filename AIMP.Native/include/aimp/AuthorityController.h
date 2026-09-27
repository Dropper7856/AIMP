#pragma once

#include "aimp/Common.h"

namespace aimp {

class AuthorityController final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "AuthorityController",
            ModuleStatus::BlockedByResearch,
            "Host-authoritative policy is specified, but native validation points and event formats are unknown."};
    }

    AuthorityController() = delete;
};

} // namespace aimp
