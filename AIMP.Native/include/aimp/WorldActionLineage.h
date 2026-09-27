#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::world_actions {

// Process-local causality only. Tokens are borrowed addresses, never retained
// native references and never a wire identity. Caller serializes access.
// Suppressed is a local fail-closed lineage guard. It is never serialized;
// it only prevents an individual cyclic/over-depth chain from being mistaken
// for a fresh network ROOT without disabling WS3 for the whole generation.
enum class Origin : std::uint8_t { None, LocalRoot, RemoteRoot, Suppressed };

// Sender-local provenance that exists before a WS3 ROOT is selected. Unlike
// Origin, this does not suppress capture by itself. It lets WorkBuild propagate
// a known local gameplay cause through internal World work until the first
// replayable public World action can claim the network ROOT.
enum class CausalSource : std::uint8_t { None, Player, AI, UI, AIMP, World };

// 0.11.4.73: provenance is no longer only a domain tag. Every independent
// local cause receives one process-local chain id. Siblings and descendants
// inherit the same id, grouping independently represented branches without suppressing siblings.
struct CausalStamp final {
    CausalSource source{};
    std::uint64_t chainId{};

    [[nodiscard]] bool Valid() const noexcept {
        return source != CausalSource::None && chainId != 0U;
    }
    [[nodiscard]] bool SameChain(const CausalStamp& other) const noexcept {
        return chainId != 0U && chainId == other.chainId;
    }
};


template <std::size_t Capacity = 16384U, std::size_t Probes = 32U>
class CausalProvenanceTable final {
public:
    static_assert(Capacity != 0U && (Capacity & (Capacity - 1U)) == 0U);
    static_assert(Probes <= Capacity);
    [[nodiscard]] CausalStamp Find(std::uintptr_t token) const noexcept {
        if (token == 0U) return {};
        const auto start = Hash(token);
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.token == token) return slot.stamp;
        }
        return {};
    }
    [[nodiscard]] bool Put(std::uintptr_t token, CausalStamp stamp) noexcept {
        if (token == 0U || !stamp.Valid()) return false;
        const auto start = Hash(token);
        std::size_t empty = Capacity;
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto index = (start + i) & (Capacity - 1U);
            auto& slot = slots_[index];
            if (slot.token == token) { slot.stamp = stamp; return true; }
            if (slot.token == 0U && empty == Capacity) empty = index;
        }
        if (empty == Capacity) return false;
        slots_[empty] = {token, stamp};
        ++size_;
        return true;
    }
    void Erase(std::uintptr_t token) noexcept {
        if (token == 0U) return;
        const auto start = Hash(token);
        for (std::size_t i = 0; i < Probes; ++i) {
            auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.token == token) { slot = {}; --size_; }
        }
    }
    void Clear() noexcept {
        for (auto& slot : slots_) slot = {};
        size_ = 0U;
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
private:
    struct Slot { std::uintptr_t token{}; CausalStamp stamp{}; };
    [[nodiscard]] static std::size_t Hash(std::uintptr_t token) noexcept {
        token ^= token >> 16U;
        token *= static_cast<std::uintptr_t>(0x9E3779B1U);
        return static_cast<std::size_t>(token) & (Capacity - 1U);
    }
    std::array<Slot, Capacity> slots_{};
    std::size_t size_{};
};

// Lifetime of one causal identity. Membership never implies replay coverage.
// Only children actually constructed inside a published invocation inherit Origin.
template <std::size_t Capacity = 16384U, std::size_t Probes = 32U>
class CausalGroupTable final {
public:
    static_assert(Capacity != 0U && (Capacity & (Capacity - 1U)) == 0U);
    static_assert(Probes <= Capacity);

    [[nodiscard]] bool Create(std::uint64_t chainId, CausalSource source) noexcept {
        if (chainId == 0U || source == CausalSource::None) return false;
        const auto start = Hash(chainId);
        std::size_t empty = Capacity;
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto index = (start + i) & (Capacity - 1U);
            auto& slot = slots_[index];
            if (slot.chainId == chainId) return slot.source == source;
            if (slot.chainId == 0U && empty == Capacity) empty = index;
        }
        if (empty == Capacity) return false;
        slots_[empty].chainId = chainId;
        slots_[empty].source = source;
        ++size_;
        return true;
    }

    [[nodiscard]] bool AddRef(std::uint64_t chainId) noexcept {
        auto* slot = FindMutable(chainId);
        if (slot == nullptr || slot->refs == 0xFFFFFFFFU) return false;
        ++slot->refs;
        return true;
    }

    [[nodiscard]] bool ReleaseRef(std::uint64_t chainId) noexcept {
        auto* slot = FindMutable(chainId);
        if (slot == nullptr || slot->refs == 0U) return false;
        --slot->refs;
        if (slot->refs == 0U) {
            *slot = {};
            --size_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool DiscardIfUnreferenced(std::uint64_t chainId) noexcept {
        auto* slot = FindMutable(chainId);
        if (slot == nullptr || slot->refs != 0U) return false;
        *slot = {};
        --size_;
        return true;
    }

    void Clear() noexcept {
        for (auto& slot : slots_) slot = {};
        size_ = 0U;
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

private:
    struct Slot final {
        std::uint64_t chainId{};
        CausalSource source{};
        std::uint32_t refs{};
    };

    [[nodiscard]] Slot* FindMutable(std::uint64_t chainId) noexcept {
        if (chainId == 0U) return nullptr;
        const auto start = Hash(chainId);
        for (std::size_t i = 0; i < Probes; ++i) {
            auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.chainId == chainId) return &slot;
        }
        return nullptr;
    }

    [[nodiscard]] const Slot* FindConst(std::uint64_t chainId) const noexcept {
        if (chainId == 0U) return nullptr;
        const auto start = Hash(chainId);
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.chainId == chainId) return &slot;
        }
        return nullptr;
    }

    [[nodiscard]] static std::size_t Hash(std::uint64_t chainId) noexcept {
        chainId ^= chainId >> 33U;
        chainId *= 0xff51afd7ed558ccdULL;
        chainId ^= chainId >> 33U;
        return static_cast<std::size_t>(chainId) & (Capacity - 1U);
    }

    std::array<Slot, Capacity> slots_{};
    std::size_t size_{};
};

template <std::size_t Capacity = 16384U, std::size_t Probes = 32U>
class LineageTable final {
public:
    static_assert(Capacity != 0U && (Capacity & (Capacity - 1U)) == 0U);
    static_assert(Probes <= Capacity);
    [[nodiscard]] Origin Find(std::uintptr_t token) const noexcept {
        if (token == 0U) return Origin::None;
        const auto start = Hash(token);
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.token == token) return slot.origin;
        }
        return Origin::None;
    }
    [[nodiscard]] bool Put(std::uintptr_t token, Origin origin) noexcept {
        if (token == 0U || origin == Origin::None) return false;
        const auto start = Hash(token);
        std::size_t empty = Capacity;
        // Scan through holes left by Erase before selecting an empty slot.
        for (std::size_t i = 0; i < Probes; ++i) {
            const auto index = (start + i) & (Capacity - 1U);
            auto& slot = slots_[index];
            if (slot.token == token) { slot.origin = origin; return true; }
            if (slot.token == 0U && empty == Capacity) empty = index;
        }
        if (empty == Capacity) return false; // Never evict a live lineage.
        slots_[empty] = {token, origin};
        ++size_;
        return true;
    }
    void Erase(std::uintptr_t token) noexcept {
        if (token == 0U) return;
        const auto start = Hash(token);
        for (std::size_t i = 0; i < Probes; ++i) {
            auto& slot = slots_[(start + i) & (Capacity - 1U)];
            if (slot.token == token) { slot = {}; --size_; }
        }
    }
    void Clear() noexcept {
        for (auto& slot : slots_) slot = {};
        size_ = 0U;
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
private:
    struct Slot { std::uintptr_t token{}; Origin origin{}; };
    [[nodiscard]] static std::size_t Hash(std::uintptr_t token) noexcept {
        token ^= token >> 16U;
        token *= static_cast<std::uintptr_t>(0x9E3779B1U);
        return static_cast<std::size_t>(token) & (Capacity - 1U);
    }
    std::array<Slot, Capacity> slots_{};
    std::size_t size_{};
};
} // namespace aimp::world_actions
