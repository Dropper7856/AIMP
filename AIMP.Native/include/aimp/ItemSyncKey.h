#pragma once

#include "aimp/WorldReplicationTypes.h"

#include <cmath>
#include <cstdint>

namespace aimp::item_sync {

// ItemSync item key, carried as a 32-hex WorldEntityId on the wire.
//   low  = owner CATHODE runtime key {entity ShortGuid, composite instance id}
//          of the level-authored GCIP_WorldPickup / PickupSpawner /
//          MultiplePickupSpawner that requested the pickup. It is identical on
//          every peer running the same level.
//   high = "IT" tag + pickup position quantized to 0.1 m (int16 per axis).
//          Only MultiplePickupSpawner needs it: all its pickups share one owner.
// Untagged drops have no owner; ItemSync-tagged drops use the spawned form.
inline constexpr std::uint64_t kItemKeyTag = 0x4954000000000000ULL;
// Placed pickups: target InventoryItem A/B in low, source AddToInventory A
// in high[31:0]. Both belong to the same validated authored scope B.
inline constexpr std::uint64_t kPlacedItemKeyTag = 0x49504C4300000000ULL; // "IPLC"
inline constexpr std::uint64_t kPlacedItemKeyTagMask = 0xFFFFFFFF00000000ULL;
enum class ItemKeyKind : std::uint8_t { Spawned, Placed };
inline constexpr std::uint64_t kItemKeyTagMask = 0xFFFF000000000000ULL;
inline constexpr float kItemKeyPositionScale = 10.0F;

struct ItemKey final {
    std::uint32_t ownerShortGuid{};
    std::uint32_t ownerInstance{};
    std::int16_t x{}, y{}, z{};
    ItemKeyKind kind{ItemKeyKind::Spawned};
    std::uint32_t sourceShortGuid{};
};

[[nodiscard]] inline std::int16_t QuantizeItemCoordinate(const float value) noexcept {
    if (!std::isfinite(value)) return 0;
    const float scaled = std::round(value * kItemKeyPositionScale);
    if (scaled >= 32767.0F) return 32767;
    if (scaled <= -32768.0F) return -32768;
    return static_cast<std::int16_t>(scaled);
}

[[nodiscard]] inline float ItemCoordinate(const std::int16_t value) noexcept {
    return static_cast<float>(value) / kItemKeyPositionScale;
}

[[nodiscard]] inline WorldEntityId EncodeItemKey(const ItemKey& key) noexcept {
    WorldEntityId id{};
    id.low = (static_cast<std::uint64_t>(key.ownerShortGuid) << 32U) | key.ownerInstance;
    if (key.kind == ItemKeyKind::Placed) {
        id.high = kPlacedItemKeyTag | key.sourceShortGuid;
        return id;
    }
    id.high = kItemKeyTag |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(key.x)) << 32U) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(key.y)) << 16U) |
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(key.z));
    return id;
}

[[nodiscard]] inline bool DecodeItemKey(const WorldEntityId& id, ItemKey& key) noexcept {
    key = {};
    if ((id.high & kPlacedItemKeyTagMask) == kPlacedItemKeyTag) {
        key.kind = ItemKeyKind::Placed;
        key.ownerShortGuid = static_cast<std::uint32_t>(id.low >> 32U);
        key.ownerInstance = static_cast<std::uint32_t>(id.low);
        key.sourceShortGuid = static_cast<std::uint32_t>(id.high);
        return key.ownerShortGuid != 0U && key.ownerInstance != 0U && key.sourceShortGuid != 0U;
    }
    if ((id.high & kItemKeyTagMask) != kItemKeyTag) return false;
    key.ownerShortGuid = static_cast<std::uint32_t>(id.low >> 32U);
    key.ownerInstance = static_cast<std::uint32_t>(id.low);
    key.x = static_cast<std::int16_t>(static_cast<std::uint16_t>(id.high >> 32U));
    key.y = static_cast<std::int16_t>(static_cast<std::uint16_t>(id.high >> 16U));
    key.z = static_cast<std::int16_t>(static_cast<std::uint16_t>(id.high));
    return key.ownerShortGuid != 0U || key.ownerInstance != 0U;
}

} // namespace aimp::item_sync
