#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace aimp::ai_sync::profiling {
// Value-only cumulative measurements. No allocations, locks, native reads or
// per-frame logging. Timers are nested: their costs must not be added together.
enum class Section : std::size_t {
    EngineCallback, EngineOriginal, Dispatcher, AiPump, PairPump, CommandPump,
    RoutePump, BehaviourHook, BehaviourOriginal, AnimationHook, AnimationOriginal,
    EngineInterval, EngineCpu, ActorInterval, BehaviourDelta, MailboxQueueAge, Count
};
inline constexpr std::array<const char*,static_cast<std::size_t>(Section::Count)> kNames{
    "engine_callback", "engine_original", "dispatcher", "aisync_pump", "pair_pump", "command_pump",
    "route_pump", "behaviour_hook", "behaviour_original", "animation_hook", "animation_original",
    "engine_interval", "engine_thread_cpu_interval", "actor_interval", "behaviour_delta", "mailbox_queue_age"};
struct Counter {
    std::atomic<std::uint64_t> count{},totalUs{},maxUs{},over10ms{},over100ms{};
};
inline std::array<Counter,kNames.size()> gCounters{};
struct Sample { std::uint64_t count{},totalUs{},maxUs{},over10ms{},over100ms{}; };
inline std::uint64_t Clock() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)?static_cast<std::uint64_t>(value.QuadPart):0U;
}
inline void Value(Section section,std::uint64_t us) noexcept {
    auto& counter=gCounters[static_cast<std::size_t>(section)];
    counter.count.fetch_add(1,std::memory_order_relaxed);
    counter.totalUs.fetch_add(us,std::memory_order_relaxed);
    auto previous=counter.maxUs.load(std::memory_order_relaxed);
    while(us>previous&&!counter.maxUs.compare_exchange_weak(previous,us,std::memory_order_relaxed)){}
    if(us>=10000U)counter.over10ms.fetch_add(1,std::memory_order_relaxed);
    if(us>=100000U)counter.over100ms.fetch_add(1,std::memory_order_relaxed);
}
inline void End(Section section,std::uint64_t start) noexcept {
    const auto stop=Clock();
    static const std::uint64_t frequency=[]() noexcept {
        LARGE_INTEGER value{};return QueryPerformanceFrequency(&value)?static_cast<std::uint64_t>(value.QuadPart):0U;
    }();
    if(start&&stop>=start&&frequency)Value(section,(stop-start)*1000000U/frequency);
}
class Timer final {
public:
    explicit Timer(Section section) noexcept:section_(section),start_(Clock()){}
    ~Timer(){End(section_,start_);}
    Timer(const Timer&)=delete;
    Timer& operator=(const Timer&)=delete;
private:
    Section section_;std::uint64_t start_;
};
inline Sample Read(Section section) noexcept {
    const auto& counter=gCounters[static_cast<std::size_t>(section)];
    return {counter.count.load(std::memory_order_relaxed),counter.totalUs.load(std::memory_order_relaxed),
        counter.maxUs.load(std::memory_order_relaxed),counter.over10ms.load(std::memory_order_relaxed),
        counter.over100ms.load(std::memory_order_relaxed)};
}
inline void EngineCadence() noexcept {
    // Called by the certified main-thread Character processing seam. This is
    // engine callback cadence, not a presentation/render FPS measurement.
    static thread_local std::uint64_t previous{};
    const auto now=GetTickCount64();
    if(previous&&now>=previous)Value(Section::EngineInterval,(now-previous)*1000U);
    previous=now;
    static thread_local std::uint64_t previousCpu{};
    FILETIME created{},exited{},kernel{},user{};
    if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) {
        const auto cpu=(static_cast<std::uint64_t>(kernel.dwHighDateTime)<<32U)+kernel.dwLowDateTime+
            (static_cast<std::uint64_t>(user.dwHighDateTime)<<32U)+user.dwLowDateTime;
        if(previousCpu&&cpu>=previousCpu)Value(Section::EngineCpu,(cpu-previousCpu)/10U);
        previousCpu=cpu;
    }
}
} // namespace aimp::ai_sync::profiling
