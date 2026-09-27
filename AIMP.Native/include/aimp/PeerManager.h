#pragma once

#include "aimp/Common.h"

namespace aimp {

class PeerManager final {
public:
    [[nodiscard]] static constexpr ModuleReport Report() noexcept {
        return {
            "PeerManager",
            ModuleStatus::VoluntaryStub,
            "Peer ownership lives in the launcher; the DLL has no native binding until IdentityMapper is proven."};
    }

    PeerManager() = delete;
};

} // namespace aimp
