#include "aimp/AnimationSyncMovementPolicy.h"
#include <cassert>
#include <iostream>

using aimp::animation_sync::MovementPolicy;
using aimp::animation_sync::SelectMovementPolicy;
using aimp::animation_sync::FollowNetworkPath;

static_assert(SelectMovementPolicy(false, false, false) == MovementPolicy::None);
static_assert(SelectMovementPolicy(false, true, true) == MovementPolicy::None);
static_assert(SelectMovementPolicy(true, true, false) == MovementPolicy::NativeRoot);
static_assert(SelectMovementPolicy(true, false, false) == MovementPolicy::FirstPersonPath);
static_assert(SelectMovementPolicy(true, true, true) == MovementPolicy::LockerVisual);
static_assert(SelectMovementPolicy(true, false, true) == MovementPolicy::LockerVisual);
static_assert(!FollowNetworkPath(MovementPolicy::None));
static_assert(!FollowNetworkPath(MovementPolicy::NativeRoot));
static_assert(FollowNetworkPath(MovementPolicy::FirstPersonPath));
static_assert(!FollowNetworkPath(MovementPolicy::LockerVisual));

int main() {
    for (const bool owned : {false, true})
        for (const bool native : {false, true})
            for (const bool locker : {false, true}) {
                const auto policy = SelectMovementPolicy(owned, native, locker);
                assert(FollowNetworkPath(policy) == (owned && !native && !locker));
            }
    std::cout << "AnimationSync movement policy tests passed\n";
}
