#include "aimp/PlayerHidingRuntime.h"

#include <Windows.h>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>

namespace aimp::player_hiding {
namespace {
constexpr float kMaximumCoordinate = 1000000.0F;
// FUN_0078B100, the NPC_SetInvisible apply_hide/apply_show target: thiscall
// (Character, u8) RET 4. Writes +2D9 and hides the render instances (+3F0) and
// the equipment/torch (+1100). Retail AI ignores a target whose +2D9 is set.
constexpr std::uintptr_t kSetInvisibleRva = 0x0038B100U;
constexpr std::array<std::uint8_t, 20> kSetInvisiblePrologue{
    0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x8D, 0x8E,
    0xF0, 0x03, 0x00, 0x00, 0x88, 0x9E, 0xD9, 0x02, 0x00, 0x00};
constexpr std::uintptr_t kSetInvisibleReturnOffset = 0x37U;
constexpr std::array<std::uint8_t, 5> kSetInvisibleEpilogue{0x5E, 0x5B, 0xC2, 0x04, 0x00};
constexpr std::uintptr_t kInvisibleOffset = 0x2D9U;
constexpr std::uintptr_t kPlayerTableRoot = 0x012F0C88U;
constexpr std::uint64_t kRetryMilliseconds = 500U;

// Retail interactable manager (ctor FUN_007C0570), shared with ItemSync. Each
// UI_Icon registers one slot (FUN_0056B530 -> FUN_007AFCE0). FUN_007C51E0 only
// selects a slot whose +96 has 0x04 (in use) and both 0x01 and 0x02; 0x40 frees
// it next frame. Retail rewrites bits 0-1 only on UI_Icon events (FUN_0056AB10),
// so clearing them is retail's own "disabled icon" state until the next event.
constexpr std::uintptr_t kInteractableManagerRva = 0x012F0DE8U;
constexpr std::uintptr_t kSlotTable = 0x3CU;
constexpr std::uintptr_t kSlotStride = 0x9CU;
constexpr std::uint32_t kSlotCount = 0x400U;
constexpr std::uintptr_t kSlotPromptPosition = 0x40U;
constexpr std::uintptr_t kSlotEntityPosition = 0x4CU;
constexpr std::uintptr_t kSlotKey = 0x58U;
constexpr std::uintptr_t kSlotFlags = 0x96U;
constexpr std::uint8_t kSlotInUse = 0x04U, kSlotFreeing = 0x40U, kSlotSelectable = 0x03U;
// Plain ASCII unlocked_text copied by strncpy_s into slot+00 (0x40 bytes).
constexpr char kHidingPrompt[] = "AI_UI_INTERACT_HIDING";
// The anchor is NPC_SetHidingNearestLocation's search point, about 1-1.2 m in
// front of the door; the icon sits on the prop.
constexpr float kSpotRadius = 3.0F;

struct Spot final {
    std::uintptr_t manager{};
    std::uintptr_t slot{};
    std::uint32_t key[2]{};
    std::uint8_t retailBits{};
    bool locked{};
};

struct Slot final {
    std::uintptr_t character{};
    std::uint64_t epoch{}, retryAt{}, spotRetryAt{};
    // This runtime hid the body. Never reveal a body hidden by retail itself
    // (death, despawn, cinematic): only undo our own concealment.
    bool concealedByUs{};
    Spot spot{};
    DiagnosticsSnapshot diagnostics{};
};
std::array<Slot, 40> gSlots{};
// Spots of forgotten Characters, freed by the next engine pump.
std::array<Spot, 40> gOrphanSpots{};
std::mutex gMutex;
std::uintptr_t gModuleBase{};
std::uint64_t gEpoch{};
int gAbi{-1};
HistorySnapshot gHistory{};

bool SameSpot(const Spot& left, const Spot& right) noexcept {
    return left.locked && right.locked && left.manager == right.manager &&
        left.key[0] == right.key[0] && left.key[1] == right.key[1];
}

// Spot ownership is shared when delayed network edges briefly put two owners
// in the same locker. Only the last release restores the original icon bits.
const Spot* OtherOwner(const Spot& spot) noexcept {
    for (const auto& state : gSlots)
        if (&state.spot != &spot && SameSpot(state.spot, spot)) return &state.spot;
    for (const auto& orphan : gOrphanSpots)
        if (&orphan != &spot && SameSpot(orphan, spot)) return &orphan;
    return nullptr;
}

Slot* Find(const std::uintptr_t character) noexcept {
    for (auto& slot : gSlots) if (slot.character == character) return &slot;
    return nullptr;
}

void Orphan(const Spot& spot) noexcept {
    if (!spot.locked) return;
    for (auto& orphan : gOrphanSpots) if (!orphan.locked) { orphan = spot; return; }
}

#if defined(_MSC_VER) && defined(_M_IX86)
bool ValidateAbi() noexcept {
    if (gAbi >= 0) return gAbi == 1;
    __try {
        const auto* code = reinterpret_cast<const std::uint8_t*>(gModuleBase + kSetInvisibleRva);
        gAbi = std::memcmp(code, kSetInvisiblePrologue.data(), kSetInvisiblePrologue.size()) == 0 &&
            std::memcmp(code + kSetInvisibleReturnOffset, kSetInvisibleEpilogue.data(),
                kSetInvisibleEpilogue.size()) == 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gAbi = 0;
    }
    return gAbi == 1;
}

// A representation can reuse player-authored definition flags. Identify the
// actual local player through retail's table, never its archetype.
bool IsRemoteCharacter(const std::uintptr_t character) noexcept {
    const auto definition = *reinterpret_cast<const std::uintptr_t*>(character + 0x64U);
    if (definition < 0x10000U) return false;
    const auto root = *reinterpret_cast<const std::uintptr_t*>(gModuleBase + kPlayerTableRoot);
    if (root < 0x10000U) return false;
    const auto context = *reinterpret_cast<const std::uintptr_t*>(root + 8U);
    if (context < 0x10000U) return false;
    const auto count = *reinterpret_cast<const std::uint32_t*>(context + 0x84U);
    const auto players = *reinterpret_cast<const std::uintptr_t*>(context + 0x80U);
    if (count == 0U || count > 4U || players < 0x10000U) return false;
    for (std::uint32_t i = 0U; i < count; ++i)
        if (reinterpret_cast<const std::uintptr_t*>(players)[i] == character) return false;
    return true;
}

bool GuardRemoteCharacter(const std::uintptr_t character) noexcept {
    __try { return IsRemoteCharacter(character); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Returns false on fault/guard rejection. `concealed` is the readback.
bool ApplyVisibility(const std::uintptr_t character, const bool hide, bool& concealed,
    const char*& stage) noexcept {
    __try {
        stage = "abi";
        if (!ValidateAbi()) return false;
        stage = "local-player-guard";
        if (!IsRemoteCharacter(character)) return false;
        stage = hide ? "hide" : "show";
        using SetInvisible = void(__thiscall*)(void*, std::uint8_t);
        reinterpret_cast<SetInvisible>(gModuleBase + kSetInvisibleRva)(
            reinterpret_cast<void*>(character), hide ? 1U : 0U);
        concealed = *reinterpret_cast<const std::uint8_t*>(character + kInvisibleOffset) != 0U;
        return concealed == hide;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        stage = "fault";
        return false;
    }
}

bool ReadConcealed(const std::uintptr_t character, bool& concealed) noexcept {
    __try {
        concealed = *reinterpret_cast<const std::uint8_t*>(character + kInvisibleOffset) != 0U;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t Manager() noexcept {
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(gModuleBase + kInteractableManagerRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0U;
    }
}

// Slots move: a deactivated icon is freed and may re-register elsewhere.
bool LiveSlot(const std::uintptr_t slot, const std::uint32_t (&key)[2]) noexcept {
    const auto flags = *reinterpret_cast<const std::uint8_t*>(slot + kSlotFlags);
    return (flags & kSlotInUse) != 0U && (flags & kSlotFreeing) == 0U &&
        std::memcmp(reinterpret_cast<const void*>(slot + kSlotKey), key, sizeof(key)) == 0;
}

float SlotDistance(const std::uintptr_t slot, const std::array<float, 3>& anchor) noexcept {
    float best = INFINITY;
    for (const auto offset : {kSlotPromptPosition, kSlotEntityPosition}) {
        const auto* position = reinterpret_cast<const float*>(slot + offset);
        const auto dx = position[0] - anchor[0], dy = position[1] - anchor[1], dz = position[2] - anchor[2];
        const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(distance) && distance < best) best = distance;
    }
    return best;
}

// Resolve by key first (the spot may have moved), else the nearest hiding
// prompt to the anchor. Returns false when no candidate exists or on fault.
bool ResolveSpot(const std::uintptr_t manager, const std::array<float, 3>* anchor, Spot& spot,
    float& distance) noexcept {
    __try {
        if (spot.locked && spot.manager != manager) spot = {};
        if (spot.locked && LiveSlot(spot.slot, spot.key)) return true;
        std::uintptr_t best{};
        float bestDistance{kSpotRadius};
        for (std::uint32_t i = 0U; i < kSlotCount; ++i) {
            const auto slot = manager + kSlotTable + i * kSlotStride;
            const auto flags = *reinterpret_cast<const std::uint8_t*>(slot + kSlotFlags);
            if ((flags & kSlotInUse) == 0U || (flags & kSlotFreeing) != 0U) continue;
            if (spot.locked) {
                if (LiveSlot(slot, spot.key)) {
                    spot.slot = slot;
                    spot.retailBits = flags & kSlotSelectable;
                    if (const auto* other = OtherOwner(spot)) spot.retailBits = other->retailBits;
                    return true;
                }
                continue;
            }
            if (anchor == nullptr ||
                std::strncmp(reinterpret_cast<const char*>(slot), kHidingPrompt, 0x40U) != 0) continue;
            const auto candidate = SlotDistance(slot, *anchor);
            if (candidate <= bestDistance) { best = slot; bestDistance = candidate; }
        }
        if (spot.locked || best == 0U) return false;
        spot.slot = best;
        spot.manager = manager;
        std::memcpy(spot.key, reinterpret_cast<const void*>(best + kSlotKey), sizeof(spot.key));
        spot.retailBits = *reinterpret_cast<const std::uint8_t*>(best + kSlotFlags) & kSlotSelectable;
        spot.locked = true;
        if (const auto* other = OtherOwner(spot)) spot.retailBits = other->retailBits;
        distance = bestDistance;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Keep the spot non-selectable. A non-zero value means retail rewrote it after
// an icon event: that is the new state to restore on release.
bool HoldSpot(Spot& spot) noexcept {
    __try {
        if (!LiveSlot(spot.slot, spot.key)) return false;
        auto& flags = *reinterpret_cast<std::uint8_t*>(spot.slot + kSlotFlags);
        if ((flags & kSlotSelectable) != 0U) {
            spot.retailBits = flags & kSlotSelectable;
            for (auto& state : gSlots)
                if (SameSpot(state.spot, spot)) state.spot.retailBits = spot.retailBits;
            for (auto& orphan : gOrphanSpots)
                if (SameSpot(orphan, spot)) orphan.retailBits = spot.retailBits;
            flags = static_cast<std::uint8_t>(flags & ~kSlotSelectable);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Restore retail's bits only on the same live icon; a freed/reused slot or an
// unloaded level already carries retail's own state.
bool ReleaseSpot(const std::uintptr_t manager, Spot& spot) noexcept {
    if (!spot.locked) return false;
    if (OtherOwner(spot) != nullptr || spot.manager != manager) {
        spot = {};
        return true;
    }
    float ignored{};
    __try {
        if (manager != 0U && ResolveSpot(manager, nullptr, spot, ignored)) {
            auto& flags = *reinterpret_cast<std::uint8_t*>(spot.slot + kSlotFlags);
            flags = static_cast<std::uint8_t>((flags & ~kSlotSelectable) | spot.retailBits);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    spot = {};
    return true;
}
#else
bool GuardRemoteCharacter(std::uintptr_t) noexcept { return false; }
bool ApplyVisibility(std::uintptr_t, bool, bool&, const char*& stage) noexcept {
    stage = "unsupported";
    return false;
}
bool ReadConcealed(std::uintptr_t, bool&) noexcept { return false; }
std::uintptr_t Manager() noexcept { return 0U; }
bool ResolveSpot(std::uintptr_t, const std::array<float, 3>*, Spot&, float&) noexcept { return false; }
bool HoldSpot(Spot&) noexcept { return false; }
bool ReleaseSpot(std::uintptr_t, Spot& spot) noexcept {
    const auto locked = spot.locked;
    spot = {};
    return locked;
}
#endif

void UpdateSpot(Slot& state, const std::uintptr_t manager, std::uint64_t& locked,
    std::uint64_t& released, std::uint64_t& missing) noexcept {
    auto& spot = state.spot;
    if (!state.diagnostics.desired) {
        if (ReleaseSpot(manager, spot)) ++released;
        return;
    }
    if (manager == 0U) return;
    const auto wasLocked = spot.locked;
    if (!wasLocked && GetTickCount64() < state.spotRetryAt) return;
    float distance{-1.0F};
    if (!ResolveSpot(manager, &state.diagnostics.anchor, spot, distance)) {
        // The icon vanished (deactivated): search the anchor again later.
        spot = {};
        state.spotRetryAt = GetTickCount64() + kRetryMilliseconds;
        ++missing;
        return;
    }
    if (!wasLocked) {
        ++locked;
        state.diagnostics.spotDistance = distance;
    }
    if (!HoldSpot(spot)) spot = {};
}

void PumpOnEngineThread(const std::uintptr_t character, const std::uintptr_t manager) noexcept {
    Slot state{};
    {
        std::scoped_lock lock(gMutex);
        auto* slot = Find(character);
        if (slot == nullptr || gModuleBase == 0U) return;
        // This guard also protects interaction writes, not just visibility.
        if (!GuardRemoteCharacter(character)) {
            slot->diagnostics.stage = "local-player-guard";
            ++slot->diagnostics.failures;
            ++gHistory.failures;
            return;
        }
        UpdateSpot(*slot, manager, slot->diagnostics.spotsLocked,
            slot->diagnostics.spotsReleased, slot->diagnostics.spotsMissing);
        slot->diagnostics.spotLocked = slot->spot.locked;
        state = *slot;
    }

    const auto desired = state.diagnostics.desired;
    bool concealed{}, applied{}, success{true};
    const auto readable = ReadConcealed(character, concealed);
    // Hide whenever the owner is hidden and the body is visible (retail may
    // reveal it on its own). Reveal only what this runtime concealed.
    const auto needHide = readable && desired && !concealed;
    const auto needShow = readable && !desired && state.concealedByUs;
    if ((needHide || needShow) && GetTickCount64() >= state.retryAt) {
        applied = true;
        success = ApplyVisibility(character, needHide, concealed, state.diagnostics.stage);
    } else if (readable && !needHide && !needShow) {
        state.diagnostics.stage = desired ? "hidden" : "visible";
    }

    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr || slot->epoch != state.epoch) {
        // Forget already handed the actual spot to the orphan release path.
        return;
    }
    slot->diagnostics.concealed = concealed;
    slot->diagnostics.stage = state.diagnostics.stage;
    if (applied) {
        if (success) {
            slot->concealedByUs = needHide;
            if (needHide) {
                ++slot->diagnostics.hides;
                ++gHistory.hides;
                gHistory.lastHiddenCharacter = character;
            } else {
                ++slot->diagnostics.shows;
                ++gHistory.shows;
                gHistory.lastShownCharacter = character;
            }
            slot->retryAt = 0U;
        } else {
            ++slot->diagnostics.failures;
            ++gHistory.failures;
            slot->retryAt = slot->diagnostics.desired != desired ? 0U : GetTickCount64() + kRetryMilliseconds;
        }
    }
    // Fully visible and spot freed: the mailbox is no longer needed. A new
    // hidden edge allocates it again.
    if (!slot->diagnostics.desired && !slot->concealedByUs && !slot->spot.locked) *slot = {};
}
}

bool ValidAnchor(const std::array<float, 3>& anchor) noexcept {
    for (const auto value : anchor)
        if (!std::isfinite(value) || std::fabs(value) > kMaximumCoordinate) return false;
    return true;
}

bool Decode(std::string_view text, bool& hidden, std::array<float, 3>& anchor) noexcept {
    if (text == "0") {
        hidden = false;
        anchor = {};
        return true;
    }
    if (text.size() > 96U || text.substr(0U, 2U) != "1,") return false;
    text.remove_prefix(2U);
    std::array<float, 3> result{};
    for (std::size_t i = 0U; i < result.size(); ++i) {
        const auto comma = text.find(',');
        if ((i < 2U) != (comma != std::string_view::npos)) return false;
        const auto field = text.substr(0U, comma);
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(), result[i]);
        if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) return false;
        if (i < 2U) text.remove_prefix(comma + 1U);
    }
    if (!ValidAnchor(result)) return false;
    hidden = true;
    anchor = result;
    return true;
}

void Initialize(const std::uintptr_t moduleBase) noexcept {
    std::scoped_lock lock(gMutex);
    gModuleBase = moduleBase;
    gSlots = {};
    gOrphanSpots = {};
    gAbi = -1;
    gHistory = {};
    ++gEpoch;
}

bool Publish(const std::uintptr_t character, const bool hidden, const std::array<float, 3>& anchor,
    std::string& error) noexcept {
    if (character < 0x10000U) { error = "Invalid remote hiding Character."; return false; }
    if (hidden && !ValidAnchor(anchor)) { error = "Invalid remote hiding anchor."; return false; }
    std::scoped_lock lock(gMutex);
    auto* slot = Find(character);
    if (slot == nullptr) {
        // A visible owner without prior state needs no mailbox.
        if (!hidden) return true;
        for (auto& candidate : gSlots) if (candidate.character == 0U) { slot = &candidate; break; }
        if (slot == nullptr) { error = "Remote hiding mailbox capacity reached."; return false; }
        slot->character = character;
        slot->epoch = ++gEpoch;
    }
    if (slot->diagnostics.desired != hidden) {
        slot->retryAt = 0U;
        if (hidden) {
            ++gHistory.hiddenEdges;
            gHistory.lastHiddenRequestedCharacter = character;
        } else ++gHistory.visibleEdges;
    }
    // A different spot while still hidden: free the previous one first.
    if (hidden && slot->diagnostics.desired && slot->diagnostics.anchor != anchor) {
        Orphan(slot->spot);
        slot->spot = {};
        slot->spotRetryAt = 0U;
    }
    slot->diagnostics.desired = hidden;
    if (hidden) slot->diagnostics.anchor = anchor;
    return true;
}

bool Hidden(const std::uintptr_t character) noexcept {
    std::scoped_lock lock(gMutex);
    const auto* slot = Find(character);
    return slot != nullptr && slot->diagnostics.desired;
}

void PumpAllOnEngineThread() noexcept {
    std::array<std::uintptr_t, 40> characters{};
    const auto manager = Manager();
    {
        std::scoped_lock lock(gMutex);
        if (gModuleBase == 0U) return;
        for (std::size_t i = 0U; i < gSlots.size(); ++i) characters[i] = gSlots[i].character;
        for (auto& orphan : gOrphanSpots) static_cast<void>(ReleaseSpot(manager, orphan));
    }
    for (const auto character : characters) if (character != 0U) PumpOnEngineThread(character, manager);
}

void Forget(const std::uintptr_t character) noexcept {
    std::scoped_lock lock(gMutex);
    if (auto* slot = Find(character)) {
        Orphan(slot->spot);
        *slot = {};
    }
}

// Level unload/frontend: retail rebuilds every interaction slot, so held spots
// are dropped rather than written into a dying or reused table.
void ForgetAll() noexcept {
    std::scoped_lock lock(gMutex);
    gSlots = {};
    gOrphanSpots = {};
    ++gEpoch;
}

DiagnosticsSnapshot Snapshot(const std::uintptr_t character) noexcept {
    std::scoped_lock lock(gMutex);
    const auto* slot = Find(character);
    return slot == nullptr ? DiagnosticsSnapshot{} : slot->diagnostics;
}

HistorySnapshot History() noexcept {
    std::scoped_lock lock(gMutex);
    return gHistory;
}
}
