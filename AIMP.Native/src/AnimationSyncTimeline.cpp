#include "aimp/AnimationSyncTimeline.h"
#include "aimp/CrashGuard.h"
#include <Windows.h>
#include <atomic>
#include <array>
#include <cstring>

namespace aimp::animation_sync::timeline {
namespace {
std::atomic<std::uintptr_t> gBase{};
template<class T> bool Load(std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000U && crash_guard::TryCopy(&value, reinterpret_cast<const void*>(address), sizeof(value));
}
bool Bytes(std::uintptr_t rva, const std::uint8_t* expected, std::size_t size) noexcept {
    std::array<std::uint8_t, 32> bytes{};
    return size <= bytes.size() && crash_guard::TryCopy(bytes.data(),
        reinterpret_cast<const void*>(gBase.load() + rva), size) && std::memcmp(bytes.data(), expected, size) == 0;
}
bool Leaf(std::uintptr_t graph, std::uintptr_t& leaf, unsigned depth = 0) noexcept {
    if (depth > 8) return false;
    const auto base = gBase.load();
    std::uintptr_t type{}, child{};
    if (!base || !Load(graph, type)) return false;
    if (type == base + 0xEB2DE8U) {
        // Scripted alternating playback node: native getters choose +68.
        return Load(graph + 0x68U, child) && Leaf(child, leaf, depth + 1);
    }
    if (type == base + 0xEB2D50U) {
        std::uintptr_t begin{}, end{};
        return Load(graph + 0x20U, begin) && Load(graph + 0x24U, end) && end > begin &&
            end - begin <= 64U && Load(begin, child) && Leaf(child, leaf, depth + 1);
    }
    if (type != base + 0xEB27F0U) return false;
    std::uintptr_t duration{}, clock{}, seek{}, animator{}, clip{};
    if (!Load(type + 0x4CU, duration) || duration != base + 0x29BE30U ||
        !Load(type + 0x50U, clock) || clock != base + 0x29BE20U ||
        !Load(type + 0x54U, seek) || seek != base + 0x29C0B0U ||
        !Load(graph + 0x50U, animator) || !Load(animator + 0xA0U, clip) || clip < 0x10000U) return false;
    leaf = graph;
    return true;
}
bool Abi() noexcept {
    constexpr std::uint8_t clock[]{0x8B,0x41,0x50,0xD9,0x40,0x08,0xC3};
    constexpr std::uint8_t duration[]{0x8B,0x49,0x50,0x8B,0x89,0xA0,0,0,0,0xE9,0x42,0xB6,0x3A,0};
    constexpr std::uint8_t seek[]{0xF3,0x0F,0x10,0x44,0x24,0x04,0x51,0x8B,0x49,0x50,0xF3,0x0F,0x11,0x04,0x24,0xE8,0x4C,0x3E,0xFF,0xFF,0xC2,0x04,0};
    return Bytes(0x29BE20U, clock, sizeof(clock)) && Bytes(0x29BE30U, duration, sizeof(duration)) &&
        Bytes(0x29C0B0U, seek, sizeof(seek));
}
}
void Initialize(std::uintptr_t base) noexcept { gBase.store(base); }
bool Read(std::uintptr_t graph, float& phase, float& duration) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    std::uintptr_t leaf{};
    if (!Abi() || !Leaf(graph, leaf)) return false;
    __try {
        using Getter = float(__thiscall*)(void*);
        const auto base = gBase.load();
        phase = reinterpret_cast<Getter>(base + 0x29BE20U)(reinterpret_cast<void*>(leaf));
        duration = reinterpret_cast<Getter>(base + 0x29BE30U)(reinterpret_cast<void*>(leaf));
        return std::isfinite(phase) && std::isfinite(duration) && phase >= 0.0F && duration > 0.0F;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    (void)graph; (void)phase; (void)duration; return false;
#endif
}
bool Seek(std::uintptr_t graph, float phase) noexcept {
#if defined(_MSC_VER) && defined(_M_IX86)
    std::uintptr_t leaf{};
    if (!std::isfinite(phase) || phase < 0.0F || !Abi() || !Leaf(graph, leaf)) return false;
    __try {
        // Seek only the selected leaf. Unlike the container's +54 fan-out this
        // cannot touch inactive blend children. 0068FF10 updates clocks only;
        // it does not dispatch the cycle's gameplay/marker callbacks.
        using Setter = void(__thiscall*)(void*, float);
        reinterpret_cast<Setter>(gBase.load() + 0x29C0B0U)(reinterpret_cast<void*>(leaf), phase);
        // 0068FF10 preserves the old clock at animator+C0. This is a remote
        // snapshot placement, so consume the skipped interval immediately;
        // future animation work must start at the resulting clock.
        std::uintptr_t animator{};
        float current{};
        return Load(leaf + 0x50U, animator) && Load(animator + 8U, current) && std::isfinite(current) &&
            crash_guard::TryWrite(reinterpret_cast<void*>(animator + 0xC0U), &current, sizeof(current));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    (void)graph; (void)phase; return false;
#endif
}
}
