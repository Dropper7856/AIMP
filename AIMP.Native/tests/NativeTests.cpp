#include "aimp/BuildProfile.h"
#include "aimp/CapabilityMatrix.h"
#include "aimp/CaptureCatalog.h"
#include "aimp/CathodePrimitiveExecutor.h"
#include "aimp/CharacterDirectStepRuntime.h"
#include "aimp/Diagnostics.h"
#include "aimp/DummyPlayerController.h"
#include "aimp/ExperimentalProtocol.h"
#include "aimp/GameCommandExecutor.h"
#include "aimp/GameThreadDispatcher.h"
#include "aimp/GameThreadObservationHooks.h"
#include "aimp/HistoricalParticipantTransaction.h"
#include "aimp/HookManager.h"
#include "aimp/InboundQueue.h"
#include "aimp/LauncherIPC.h"
#include "aimp/LevelGenerationTracker.h"
#include "aimp/LifecycleCatalog.h"
#include "aimp/NativeLimits.h"
#include "aimp/Protocol.h"
#include "aimp/ProductVersion.h"
#include "aimp/ReadyReplayGate.h"
#include "aimp/SignatureResolver.h"
#include "aimp/SpawnCharacterTracker.h"
#include "aimp/WorldInteractionAdapters.h"
#include "aimp/WorldAnimatedAdapters.h"
#include "aimp/WorldReplicationCodec.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aimp {

struct LauncherIPCTestAccess final {
    static void MarkConnected(LauncherIPC& ipc) {
        ipc.connected_.store(true, std::memory_order_release);
    }

    static void PrimeDisconnectedEpoch(LauncherIPC& ipc) {
        ipc.experimentalNegotiated_.store(
            true,
            std::memory_order_release);
        ipc.lastPublishedGeneration_.store(41, std::memory_order_release);
        ipc.lastPublishedCaptureCount_.store(42, std::memory_order_release);
        ipc.lastPublishedCaptureGeneration_.store(
            43,
            std::memory_order_release);
        ipc.lastPublishedStability_.store(1, std::memory_order_release);
        ipc.lastPublishedScheduleState_.store(1, std::memory_order_release);
        ipc.lastPublishedPendingCommandCount_.store(
            2,
            std::memory_order_release);
        ipc.lastPublishedSpawnTrackerVersion_.store(
            3,
            std::memory_order_release);
        std::lock_guard lock(ipc.experimentalStateMutex_);
        ipc.experimentalResponses_.emplace(
            "00000000-0000-0000-0000-000000000001",
            LauncherIPC::ExperimentalCachedResponse{});
    }

    static void PrimeExperimental(
        LauncherIPC& ipc,
        const std::string_view portableDummyId) {
        MarkConnected(ipc);
        ipc.helloAcknowledged_.store(true, std::memory_order_release);
        ipc.experimentalNegotiated_.store(
            true,
            std::memory_order_release);
        std::lock_guard lock(ipc.experimentalStateMutex_);
        ipc.negotiatedDummyId_ = portableDummyId;
    }

    static void PrimeGeneric(LauncherIPC& ipc) {
        MarkConnected(ipc);
        ipc.helloAcknowledged_.store(true, std::memory_order_release);
        ipc.sessionConfigured_.store(true, std::memory_order_release);
        ipc.netzReadySent_.store(true, std::memory_order_release);
    }

    static bool Handle(
        LauncherIPC& ipc,
        const protocol::MessageType type,
        const std::vector<std::byte>& payload,
        const std::uint64_t sequence = 1) {
        protocol::Frame frame{};
        frame.type = type;
        frame.sequence = sequence;
        frame.payload = payload;
        return ipc.HandleFrame(std::move(frame));
    }

    static std::size_t PendingExperimental(const LauncherIPC& ipc) {
        std::lock_guard lock(ipc.experimentalStateMutex_);
        return ipc.experimentalPending_.size();
    }

    static std::size_t CachedExperimental(const LauncherIPC& ipc) {
        std::lock_guard lock(ipc.experimentalStateMutex_);
        return ipc.experimentalResponses_.size();
    }

    static std::vector<protocol::Frame> TakeQueuedFrames(
        LauncherIPC& ipc) {
        std::deque<std::vector<std::byte>> encoded;
        {
            std::lock_guard lock(ipc.outgoingMutex_);
            encoded = std::move(ipc.outgoing_);
            ipc.outgoing_.clear();
            ipc.outgoingBytes_ = 0;
        }
        std::vector<protocol::Frame> frames;
        for (const auto& wire : encoded) {
            protocol::Frame frame{};
            std::size_t consumed = 0;
            if (protocol::DecodeFrame(wire, frame, consumed) ==
                    protocol::DecodeResult::Complete &&
                consumed == wire.size()) {
                frames.push_back(std::move(frame));
            }
        }
        return frames;
    }

    static void Disconnect(LauncherIPC& ipc) {
        ipc.Disconnect("unit_test_disconnect");
    }

    static std::size_t QueuedFrames(const LauncherIPC& ipc) {
        std::lock_guard lock(ipc.outgoingMutex_);
        return ipc.outgoing_.size();
    }

    static std::size_t QueuedBytes(const LauncherIPC& ipc) {
        std::lock_guard lock(ipc.outgoingMutex_);
        return ipc.outgoingBytes_;
    }

    static bool PublicationMarkersReset(const LauncherIPC& ipc) {
        std::lock_guard lock(ipc.experimentalStateMutex_);
        return
            !ipc.experimentalNegotiated_.load(std::memory_order_acquire) &&
            ipc.lastPublishedGeneration_.load(std::memory_order_acquire) ==
                (std::numeric_limits<std::uint64_t>::max)() &&
            ipc.lastPublishedCaptureCount_.load(std::memory_order_acquire) ==
                (std::numeric_limits<std::uint64_t>::max)() &&
            ipc.lastPublishedCaptureGeneration_.load(
                std::memory_order_acquire) ==
                (std::numeric_limits<std::uint64_t>::max)() &&
            ipc.lastPublishedStability_.load(std::memory_order_acquire) ==
                0xFFU &&
            ipc.lastPublishedScheduleState_.load(
                std::memory_order_acquire) == 0xFFU &&
            ipc.lastPublishedPendingCommandCount_.load(
                std::memory_order_acquire) ==
                (std::numeric_limits<std::uint64_t>::max)() &&
            ipc.lastPublishedSpawnTrackerVersion_.load(
                std::memory_order_acquire) ==
                (std::numeric_limits<std::uint64_t>::max)() &&
            ipc.experimentalResponses_.empty() &&
            ipc.experimentalPending_.empty();
    }
};

} // namespace aimp

namespace {

int failures = 0;
std::uint64_t checks = 0;

void Check(const bool condition, const char* expression, const int line) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

std::vector<std::byte> ReadFileBytes(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const auto end = input.tellg();
    if (end <= 0) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return input ? bytes : std::vector<std::byte>{};
}

std::vector<std::byte> Bytes(const std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return std::vector<std::byte>(begin, begin + value.size());
}

void TestCapabilities() {
    aimp::CapabilityMatrix matrix;
    const auto initial = matrix.Snapshot();
    CHECK(initial.size() ==
          static_cast<std::size_t>(aimp::Capability::Count));
    CHECK(std::all_of(
        initial.begin(),
        initial.end(),
        [](const aimp::CapabilityRecord& record) {
            return record.state == aimp::CapabilityState::Unresolved;
        }));

    aimp::ApplyFailClosedMutationPolicy(matrix);
    constexpr aimp::Capability mutations[]{
        aimp::Capability::ScheduleGameThreadWork,
        aimp::Capability::CreateDummyParticipant,
        aimp::Capability::ReplayCharacterSpawn,
        aimp::Capability::BindDummyCharacter,
        aimp::Capability::DestroyDummyCharacter,
        aimp::Capability::ApplyInboundDescriptors,
    };
    for (const auto capability : mutations) {
        CHECK(matrix.Get(capability).state ==
              aimp::CapabilityState::Blocked);
        CHECK(!matrix.IsEnabled(capability));
    }
    // An exact file identity is intentionally not an input to the matrix and
    // therefore cannot turn a mutation capability on by itself.
    const bool exactHashMatched = true;
    CHECK(exactHashMatched);
    CHECK(!matrix.IsEnabled(
        aimp::Capability::CreateDummyParticipant));

    matrix.Set(
        aimp::Capability::ObserveNetZ,
        aimp::CapabilityState::StaticValidated,
        "test");
    CHECK(matrix.IsAtLeastResolved(
        aimp::Capability::ObserveNetZ));
    CHECK(aimp::ToString(aimp::CapabilityState::RuntimeValidated) ==
          "RuntimeValidated");
    CHECK(aimp::ToString(
              aimp::Capability::ObserveCharacterRequests) ==
          "ObserveCharacterRequests");
}

void TestIpcDisconnectEpochReset() {
    aimp::Diagnostics diagnostics;
    aimp::InboundQueue inbound;
    aimp::LauncherIPC ipc(diagnostics, inbound);
    const auto payload = Bytes("obsolete_response");
    CHECK(!ipc.Queue(
        aimp::protocol::MessageType::CapabilityState,
        0,
        0,
        payload));
    aimp::LauncherIPCTestAccess::MarkConnected(ipc);
    CHECK(ipc.Queue(
        aimp::protocol::MessageType::CapabilityState,
        0,
        1,
        payload));
    CHECK(ipc.Queue(
        aimp::protocol::MessageType::CaptureState,
        0,
        2,
        payload));
    CHECK(aimp::LauncherIPCTestAccess::QueuedFrames(ipc) == 2);
    CHECK(aimp::LauncherIPCTestAccess::QueuedBytes(ipc) > 0);

    aimp::LauncherIPCTestAccess::PrimeDisconnectedEpoch(ipc);
    aimp::LauncherIPCTestAccess::Disconnect(ipc);
    CHECK(aimp::LauncherIPCTestAccess::QueuedFrames(ipc) == 0);
    CHECK(aimp::LauncherIPCTestAccess::QueuedBytes(ipc) == 0);
    CHECK(
        aimp::LauncherIPCTestAccess::PublicationMarkersReset(ipc));
}

void TestCaptureCatalog() {
    std::array<std::byte, 0x130> request{};
    aimp::CaptureCatalog catalog;
    catalog.InvalidateGeneration(7);
    CHECK(catalog.CaptureFromHook(
        aimp::CharacterRequestKind::SpawnF0,
        reinterpret_cast<void*>(0x1234),
        request.data(),
        0xF0,
        7,
        0x14A70));
    CHECK(catalog.Pump() == 1);
    auto summary = catalog.Summary();
    CHECK(summary.currentGeneration == 7);
    CHECK(summary.hasF0);
    CHECK(!summary.has130);
    CHECK(summary.lastThreadId == GetCurrentThreadId());
    CHECK(summary.currentCaptureCount == 1);

    CHECK(catalog.CaptureFromHook(
        aimp::CharacterRequestKind::SpawnF0,
        reinterpret_cast<void*>(0x1234),
        request.data(),
        0xF0,
        7,
        0x14A70));
    catalog.InvalidateGeneration(8);
    CHECK(catalog.Pump() == 1);
    summary = catalog.Summary();
    CHECK(summary.currentGeneration == 8);
    CHECK(!summary.hasF0);
    CHECK(!summary.has130);
    CHECK(summary.lastThreadId == 0);
    CHECK(summary.currentCaptureCount == 0);
    CHECK(!catalog.Latest(
        aimp::CharacterRequestKind::SpawnF0,
        8).has_value());

    CHECK(catalog.CaptureFromHook(
        aimp::CharacterRequestKind::Spawn130,
        reinterpret_cast<void*>(0x5678),
        request.data(),
        0x130,
        8,
        0x14C90));
    CHECK(catalog.Pump() == 1);
    summary = catalog.Summary();
    CHECK(summary.has130);
    const auto latest130 = catalog.Latest(
        aimp::CharacterRequestKind::Spawn130,
        8);
    CHECK(latest130.has_value());
    if (latest130.has_value()) {
        CHECK(latest130->sink == 0x5678U);
        const auto hex = aimp::CharacterRequestHex(*latest130);
        CHECK(hex.size() == 0x130U * 2U);
        CHECK(std::all_of(
            hex.begin(),
            hex.end(),
            [](const char value) {
                return value == '0';
            }));
    }
    CHECK(!catalog.CaptureFromHook(
        aimp::CharacterRequestKind::Spawn130,
        reinterpret_cast<void*>(0x5678),
        request.data(),
        0xF0,
        8,
        0x14C90));

    aimp::CaptureCatalog bounded;
    bounded.InvalidateGeneration(1);
    for (std::size_t index = 0;
         index < aimp::CaptureCatalog::kHookSlots;
         ++index) {
        CHECK(bounded.CaptureFromHook(
            aimp::CharacterRequestKind::SpawnF0,
            reinterpret_cast<void*>(0x1234),
            request.data(),
            0xF0,
            1,
            0x14A70));
    }
    CHECK(!bounded.CaptureFromHook(
        aimp::CharacterRequestKind::SpawnF0,
        reinterpret_cast<void*>(0x1234),
        request.data(),
        0xF0,
        1,
        0x14A70));
    CHECK(bounded.Pump() == aimp::CaptureCatalog::kHookSlots);
    CHECK(bounded.Snapshot().size() ==
          aimp::CaptureCatalog::kHistoryCapacity);
    CHECK(bounded.Summary().dropped == 1);
}

void TestLevelGeneration() {
    aimp::LevelGenerationTracker tracker;
    const aimp::EngineRootObservation first{
        0x1000,
        0x2000,
        0x3000,
        0x4000,
        0x5000};
    auto state = tracker.Observe(first);
    CHECK(state.generation == 1);
    CHECK(state.stability == aimp::LevelStability::Transition);
    state = tracker.Observe(first);
    CHECK(state.stability == aimp::LevelStability::Transition);
    state = tracker.Observe(first);
    CHECK(state.stability == aimp::LevelStability::Stable);
    CHECK(state.generation == 1);

    auto changed = first;
    changed.netz = 0x2100;
    state = tracker.Observe(changed);
    CHECK(state.generation == 2);
    CHECK(state.stability == aimp::LevelStability::Transition);
    tracker.Observe(changed);
    state = tracker.Observe(changed);
    CHECK(state.stability == aimp::LevelStability::Stable);
    state = tracker.BeginTransition();
    CHECK(state.generation == 3);
    CHECK(state.stability == aimp::LevelStability::Transition);
    CHECK(tracker.CurrentGeneration() == 3);
}

void TestSpawnCharacterTracker() {
    aimp::LevelGenerationTracker generations;
    const aimp::EngineRootObservation root{
        0x1000, 0x2000, 0x3000, 0x4000, 0x5000};
    generations.Observe(root);
    aimp::SpawnCharacterTracker tracker(generations);
    tracker.Begin(1, 73);
    auto state = tracker.Snapshot();
    CHECK(state.state == aimp::SpawnCharacterTrackingState::Pending);
    CHECK(state.spawnKey == 73);
    tracker.ObserveCreated(72, 0x10000, 0x2C37BBB0, 0x1C374418, 0x20000);
    CHECK(tracker.Snapshot().state ==
          aimp::SpawnCharacterTrackingState::Pending);
    tracker.ObserveCreated(73, 0x10000, 0x2C37BBB0, 0x1C374418, 0x20000);
    state = tracker.Snapshot();
    CHECK(state.state == aimp::SpawnCharacterTrackingState::Ready);
    CHECK(state.characterAddress == 0x2C37BBB0U);
    CHECK(state.recordAddress == 0x1C374418U);
    tracker.ResetGeneration(2);
    state = tracker.Snapshot();
    CHECK(state.state == aimp::SpawnCharacterTrackingState::None);
    CHECK(state.characterAddress == 0U);
}

void TestLifecycleCatalog() {
    aimp::LifecycleCatalog catalog;
    catalog.InvalidateGeneration(2);
    CHECK(catalog.CaptureFromHook(
        aimp::LifecycleEventKind::CharacterCreateBegin,
        1,
        1,
        2,
        3,
        4));
    CHECK(catalog.Pump() == 1);
    CHECK(catalog.Snapshot().empty());
    CHECK(catalog.CaptureFromHook(
        aimp::LifecycleEventKind::CharacterFinalizeEnd,
        2,
        1,
        2,
        3,
        4));
    CHECK(catalog.Pump() == 1);
    const auto events = catalog.Snapshot();
    CHECK(events.size() == 1);
    const auto allEvents = catalog.SnapshotAll();
    CHECK(allEvents.size() == 2);
    CHECK(allEvents.front().levelGeneration == 1);
    CHECK(events.front().levelGeneration == 2);
    CHECK(events.front().performanceCounter != 0);
    CHECK(std::string(aimp::ToString(events.front().kind)) ==
          "character_finalize_end");
}

void TestDispatcher() {
    aimp::GameThreadDispatcher dispatcher(2);
    dispatcher.SetGeneration(7);
    const auto threadId = GetCurrentThreadId();
    std::uint32_t preBindExecutions = 0;
    aimp::GameThreadCommand preBind{};
    preBind.requestId = 1;
    preBind.levelGeneration = 7;
    preBind.execute = [&] { ++preBindExecutions; };
    CHECK(dispatcher.Enqueue(std::move(preBind)) ==
          aimp::DispatchEnqueueResult::Queued);
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(!dispatcher.IsEnabled());
    CHECK(dispatcher.DrainOnEngineThread(1) == 0);
    CHECK(dispatcher.BindEngineThread(
              threadId,
              aimp::EngineThreadSource::MainThreadCharacterProcessing,
              100) == aimp::EngineThreadBindResult::Bound);
    CHECK(dispatcher.IsEnabled());
    CHECK(dispatcher.EngineThreadId() == threadId);
    CHECK(dispatcher.EngineThreadBindingSource() ==
          aimp::EngineThreadSource::MainThreadCharacterProcessing);
    CHECK(dispatcher.BindEngineThread(
              threadId + 1,
              aimp::EngineThreadSource::CharacterManagerUpdate,
              101) == aimp::EngineThreadBindResult::ThreadMismatch);
    CHECK(dispatcher.EngineThreadId() == threadId);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(preBindExecutions == 1);

    aimp::DispatchEnqueueResult nestedResult =
        aimp::DispatchEnqueueResult::Invalid;
    aimp::DispatchCompletion completion =
        aimp::DispatchCompletion::Cleared;
    aimp::GameThreadCommand create{};
    create.requestId = 2;
    create.levelGeneration = 7;
    create.expiresAtTickMs = GetTickCount64() + 10000;
    create.kind = aimp::GameThreadCommandKind::CreateDummy;
    create.execute = [&] {
        aimp::GameThreadCommand nested{};
        nested.requestId = 3;
        nested.levelGeneration = 7;
        nested.kind = aimp::GameThreadCommandKind::CreateDummy;
        nested.execute = [] {};
        nestedResult = dispatcher.Enqueue(std::move(nested));
    };
    create.completion = [&](const aimp::DispatchCompletion result) {
        completion = result;
    };
    CHECK(dispatcher.Enqueue(std::move(create)) ==
          aimp::DispatchEnqueueResult::Queued);

    std::size_t wrongThreadDrain = 99;
    std::thread wrongThread([&] {
        wrongThreadDrain = dispatcher.DrainOnEngineThread(1);
    });
    wrongThread.join();
    CHECK(wrongThreadDrain == 0);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(completion == aimp::DispatchCompletion::Executed);
    CHECK(nestedResult ==
          aimp::DispatchEnqueueResult::CreateAlreadyPending);

    aimp::GameThreadCommand duplicate{};
    duplicate.requestId = 2;
    duplicate.levelGeneration = 7;
    duplicate.execute = [] {};
    CHECK(dispatcher.Enqueue(std::move(duplicate)) ==
          aimp::DispatchEnqueueResult::Duplicate);

    aimp::GameThreadCommand expired{};
    expired.requestId = 4;
    expired.levelGeneration = 7;
    expired.expiresAtTickMs = GetTickCount64();
    expired.execute = [] {};
    CHECK(dispatcher.Enqueue(std::move(expired)) ==
          aimp::DispatchEnqueueResult::Expired);

    std::uint32_t cancelled = 0;
    for (std::uint64_t requestId = 5; requestId <= 6; ++requestId) {
        aimp::GameThreadCommand command{};
        command.requestId = requestId;
        command.levelGeneration = requestId == 5 ? 7 : 0;
        command.execute = [] {};
        command.completion = [&](const aimp::DispatchCompletion result) {
            if (result ==
                aimp::DispatchCompletion::GenerationInvalidated) {
                ++cancelled;
            }
        };
        CHECK(dispatcher.Enqueue(std::move(command)) ==
              aimp::DispatchEnqueueResult::Queued);
    }
    aimp::GameThreadCommand full{};
    full.requestId = 7;
    full.levelGeneration = 7;
    full.execute = [] {};
    CHECK(dispatcher.Enqueue(std::move(full)) ==
          aimp::DispatchEnqueueResult::CapacityReached);
    dispatcher.SetGeneration(8);
    CHECK(cancelled == 1);
    CHECK(dispatcher.Generation() == 8);
    CHECK(dispatcher.IsEnabled());
    CHECK(dispatcher.EngineThreadId() == threadId);
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);

    aimp::GameThreadCommand stale{};
    stale.requestId = 8;
    stale.levelGeneration = 7;
    stale.execute = [] {};
    CHECK(dispatcher.Enqueue(std::move(stale)) ==
          aimp::DispatchEnqueueResult::GenerationMismatch);

    aimp::DispatchCompletion expiration =
        aimp::DispatchCompletion::Executed;
    aimp::GameThreadCommand delayed{};
    delayed.requestId = 9;
    delayed.levelGeneration = 8;
    const auto expirationTick = GetTickCount64() + 20;
    delayed.expiresAtTickMs = expirationTick;
    delayed.execute = [] {};
    delayed.completion = [&](const auto result) { expiration = result; };
    CHECK(dispatcher.Enqueue(std::move(delayed)) ==
          aimp::DispatchEnqueueResult::Queued);
    while (GetTickCount64() < expirationTick) {
        Sleep(1);
    }
    CHECK(dispatcher.ExpirePending() == 1);
    CHECK(expiration == aimp::DispatchCompletion::Expired);

    const auto snapshot = dispatcher.Snapshot();
    CHECK(snapshot.ready);
    CHECK(snapshot.source ==
          aimp::EngineThreadSource::MainThreadCharacterProcessing);
    CHECK(snapshot.firstCallbackPerformanceCounter == 100);
    CHECK(snapshot.mismatchedThreadId == threadId + 1);
    dispatcher.Clear();
    CHECK(!dispatcher.IsEnabled());

    aimp::GameThreadDispatcher producerFallback(2);
    producerFallback.SetGeneration(11);
    producerFallback.SetHookAvailability(true, true, true);
    std::uint32_t producerExecutions = 0;
    aimp::GameThreadCommand producerQueued{};
    producerQueued.requestId = 10;
    producerQueued.levelGeneration = 0;
    producerQueued.execute = [&] { ++producerExecutions; };
    CHECK(producerFallback.Enqueue(std::move(producerQueued)) ==
          aimp::DispatchEnqueueResult::Queued);
    CHECK(producerFallback.BindEngineThread(
              threadId,
              aimp::EngineThreadSource::CharacterReadinessProducer,
              200) == aimp::EngineThreadBindResult::Bound);
    CHECK(producerFallback.BindEngineThread(
              threadId,
              aimp::EngineThreadSource::CharacterReadinessProducer,
              201) == aimp::EngineThreadBindResult::AlreadyBound);
    CHECK(producerFallback.BindEngineThread(
              threadId + 1,
              aimp::EngineThreadSource::MainThreadCharacterProcessing,
              202) == aimp::EngineThreadBindResult::ThreadMismatch);
    producerFallback.SetGeneration(12);
    CHECK(producerFallback.IsEnabled());
    CHECK(producerFallback.DrainOnEngineThread(1) == 1);
    CHECK(producerExecutions == 1);
    const auto producerSnapshot = producerFallback.Snapshot();
    CHECK(producerSnapshot.source ==
          aimp::EngineThreadSource::CharacterReadinessProducer);
    CHECK(producerSnapshot.callbackCount == 2);
    CHECK(producerSnapshot.mismatchCount == 1);
    CHECK(producerSnapshot.readinessProducerHookInstalled);
}

void TestGameThreadRuntimeEvidence() {
    aimp::GameThreadRuntimeEvidence evidence;
    evidence.ResetGeneration(3);
    constexpr std::uint32_t kThread = 27844;
    evidence.ObserveMainThreadCharacterProcessing(kThread, 10);
    evidence.ObserveCharacterManagerUpdate(
        kThread,
        999,
        0x12340000U);

    std::array<std::byte, 8> ready{
        std::byte{0x78},
        std::byte{0x56},
        std::byte{0x34},
        std::byte{0x12},
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00}};
    evidence.ObserveReadinessProducer(
        kThread,
        1001,
        3,
        0x1000,
        0x12345678,
        0x2000);
    evidence.ObserveReadinessPublisher(
        kThread,
        1002,
        3,
        0x1018,
        ready,
        true);
    evidence.ObserveReadinessBarrier(
        kThread,
        1003,
        3,
        0x101C,
        ready,
        true,
        1,
        true,
        1,
        true,
        0x3000,
        0,
        true,
        1,
        true,
        2,
        true,
        2,
        true);

    auto snapshot = evidence.Snapshot();
    CHECK(snapshot.mainThreadCharacterProcessing.calls == 1);
    CHECK(snapshot.characterManagerUpdate.calls == 1);
    CHECK(snapshot.lastCharacterManager == 0x12340000U);
    CHECK(snapshot.readiness.producer.calls == 1);
    CHECK(snapshot.readiness.publisher.calls == 1);
    CHECK(snapshot.readiness.barrier.calls == 1);
    CHECK(snapshot.readiness.lastPublisherMessage == ready);
    CHECK(snapshot.readiness.participantCountBeforeReadable);
    CHECK(snapshot.readiness.participantCountBeforeBarrier == 1);
    CHECK(snapshot.readiness.barrierRecordReadable);
    CHECK(snapshot.readiness.barrierRecord == 0x3000);
    CHECK(snapshot.readiness.readyCountBeforeReadable);
    CHECK(snapshot.readiness.readyCountBeforeBarrier == 0);
    CHECK(snapshot.readiness.readyCountAfterReadable);
    CHECK(snapshot.readiness.readyCountAfterBarrier == 1);
    CHECK(snapshot.readiness.recordStateBeforeReadable);
    CHECK(snapshot.readiness.recordStateBeforeBarrier == 2);

    evidence.ResetGeneration(4);
    snapshot = evidence.Snapshot();
    CHECK(snapshot.generation == 4);
    CHECK(snapshot.mainThreadCharacterProcessing.calls == 0);
    CHECK(snapshot.lastCharacterManager == 0U);
    evidence.ObserveMainThreadCharacterProcessing(kThread, 2000);
    evidence.ObserveMainThreadCharacterProcessing(kThread + 1, 2001);
    CHECK(evidence.Snapshot()
              .mainThreadCharacterProcessing
              .observedOnMultipleThreads);
}

void TestReadyReplayGate() {
    constexpr std::uint64_t kGeneration = 9;
    constexpr std::uint32_t kSpawnKey = 0x12345678;
    const aimp::ReadyMessage8 naturalMessage{
        std::byte{0x78},
        std::byte{0x56},
        std::byte{0x34},
        std::byte{0x12},
        std::byte{0xA5},
        std::byte{0x5A},
        std::byte{0x01},
        std::byte{0x80}};
    const aimp::NaturalReadyObservation natural{
        kGeneration,
        kSpawnKey,
        naturalMessage,
        2,
        0,
        1};
    const aimp::ReadyReplayCheck replay{
        kGeneration,
        kSpawnKey,
        naturalMessage,
        2,
        1};

    aimp::ReadyReplayGate gate;
    CHECK(gate.Snapshot().state ==
          aimp::ReadyReplayGateState::Idle);
    CHECK(gate.AcceptNaturalReady(natural).code ==
          aimp::ReadyReplayGateCode::NotArmed);
    CHECK(gate.ScheduleReplay(replay).code ==
          aimp::ReadyReplayGateCode::NotArmed);
    CHECK(gate.ConsumeExecutionPermit(replay).code ==
          aimp::ReadyReplayGateCode::NotArmed);
    CHECK(gate.Arm(0, kSpawnKey).code ==
          aimp::ReadyReplayGateCode::InvalidGeneration);
    CHECK(gate.Arm(kGeneration, 0).code ==
          aimp::ReadyReplayGateCode::InvalidSpawnKey);
    CHECK(gate.Arm(kGeneration, kSpawnKey).Succeeded());
    CHECK(gate.Snapshot().state ==
          aimp::ReadyReplayGateState::Armed);
    CHECK(gate.Arm(kGeneration, kSpawnKey).code ==
          aimp::ReadyReplayGateCode::AlreadyArmed);
    CHECK(gate.ScheduleReplay(replay).code ==
          aimp::ReadyReplayGateCode::NaturalReadyMissing);
    CHECK(gate.ConsumeExecutionPermit(replay).code ==
          aimp::ReadyReplayGateCode::ReplayNotScheduled);

    auto badNatural = natural;
    badNatural.levelGeneration++;
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::GenerationMismatch);
    badNatural = natural;
    badNatural.spawnKey++;
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::SpawnKeyMismatch);
    badNatural = natural;
    badNatural.message[0] = std::byte{0x79};
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::MessageSpawnKeyMismatch);
    badNatural = natural;
    badNatural.participantCount = 1;
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::UnexpectedParticipantCount);
    badNatural = natural;
    badNatural.readyCountBefore = 1;
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::UnexpectedReadyTransition);
    badNatural = natural;
    badNatural.readyCountAfter = 2;
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::UnexpectedReadyTransition);

    CHECK(gate.AcceptNaturalReady(natural).Succeeded());
    CHECK(gate.Snapshot().state ==
          aimp::ReadyReplayGateState::NaturalReadyAccepted);
    CHECK(gate.Snapshot().naturalMessage == naturalMessage);
    CHECK(gate.AcceptNaturalReady(natural).code ==
          aimp::ReadyReplayGateCode::DuplicateNaturalReady);
    badNatural = natural;
    badNatural.message[7] = std::byte{0x81};
    CHECK(gate.AcceptNaturalReady(badNatural).code ==
          aimp::ReadyReplayGateCode::MessageMismatch);
    CHECK(gate.ConsumeExecutionPermit(replay).code ==
          aimp::ReadyReplayGateCode::ReplayNotScheduled);

    auto badReplay = replay;
    badReplay.levelGeneration++;
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::GenerationMismatch);
    badReplay = replay;
    badReplay.spawnKey++;
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::SpawnKeyMismatch);
    badReplay = replay;
    badReplay.message[0] = std::byte{0x79};
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::MessageSpawnKeyMismatch);
    badReplay = replay;
    badReplay.message[7] = std::byte{0x81};
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::MessageMismatch);
    badReplay = replay;
    badReplay.participantCount = 3;
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::UnexpectedParticipantCount);
    badReplay = replay;
    badReplay.readyCount = 0;
    CHECK(gate.ScheduleReplay(badReplay).code ==
          aimp::ReadyReplayGateCode::UnexpectedReadyCount);

    CHECK(gate.ScheduleReplay(replay).Succeeded());
    CHECK(gate.Snapshot().state ==
          aimp::ReadyReplayGateState::ReplayScheduled);
    CHECK(gate.ScheduleReplay(replay).code ==
          aimp::ReadyReplayGateCode::DuplicateSchedule);

    badReplay = replay;
    badReplay.levelGeneration++;
    CHECK(gate.ConsumeExecutionPermit(badReplay).code ==
          aimp::ReadyReplayGateCode::GenerationMismatch);
    badReplay = replay;
    badReplay.message[6] = std::byte{0x02};
    CHECK(gate.ConsumeExecutionPermit(badReplay).code ==
          aimp::ReadyReplayGateCode::MessageMismatch);
    badReplay = replay;
    badReplay.participantCount = 1;
    CHECK(gate.ConsumeExecutionPermit(badReplay).code ==
          aimp::ReadyReplayGateCode::UnexpectedParticipantCount);
    badReplay = replay;
    badReplay.readyCount = 2;
    CHECK(gate.ConsumeExecutionPermit(badReplay).code ==
          aimp::ReadyReplayGateCode::UnexpectedReadyCount);

    CHECK(gate.ConsumeExecutionPermit(replay).Succeeded());
    CHECK(gate.Snapshot().state ==
          aimp::ReadyReplayGateState::ReplayPermitConsumed);
    CHECK(gate.ConsumeExecutionPermit(replay).code ==
          aimp::ReadyReplayGateCode::DuplicateExecution);
    CHECK(gate.ScheduleReplay(replay).code ==
          aimp::ReadyReplayGateCode::DuplicateSchedule);
    CHECK(aimp::ToString(
              aimp::ReadyReplayGateCode::DuplicateExecution) ==
          "duplicate_execution");

    gate.Reset();
    const auto reset = gate.Snapshot();
    CHECK(reset.state == aimp::ReadyReplayGateState::Idle);
    CHECK(reset.levelGeneration == 0);
    CHECK(reset.spawnKey == 0);
    CHECK(reset.naturalMessage == aimp::ReadyMessage8{});
    CHECK(gate.Arm(kGeneration + 1, kSpawnKey + 1).Succeeded());
}

class MockDummyExecutor final : public aimp::IDummyNativeExecutor {
public:
    std::optional<aimp::DummyNativeStep> failAcquire;
    std::optional<aimp::DummyNativeStep> failRelease;
    bool failVerify{};
    bool movementSucceeds{true};
    std::vector<aimp::DummyNativeStep> acquired;
    std::vector<aimp::DummyNativeStep> released;
    std::uint32_t beginCreateCalls{};
    std::uint32_t moveCalls{};
    std::uint32_t operationThreadId{};
    aimp::DummySpawnOffset lastSpawnOffset{};

    bool BeginCreate(
        const aimp::DummyRequestIdentity&,
        const std::uint64_t,
        const aimp::DummySpawnOffset& spawnOffset,
        std::string&) override {
        ++beginCreateCalls;
        operationThreadId = GetCurrentThreadId();
        lastSpawnOffset = spawnOffset;
        return true;
    }

    bool Acquire(
        const aimp::DummyNativeStep step,
        std::string& error) override {
        operationThreadId = GetCurrentThreadId();
        if (failAcquire == step) {
            error = "synthetic_acquire_failure";
            return false;
        }
        acquired.push_back(step);
        return true;
    }

    bool Release(
        const aimp::DummyNativeStep step,
        std::string& error) noexcept override {
        operationThreadId = GetCurrentThreadId();
        released.push_back(step);
        if (failRelease == step) {
            error = "synthetic_release_failure";
            return false;
        }
        return true;
    }

    bool Verify(std::string& error) override {
        operationThreadId = GetCurrentThreadId();
        if (failVerify) {
            error = "synthetic_verify_failure";
            return false;
        }
        return true;
    }

    bool Move(
        std::string_view,
        float,
        std::uint32_t,
        std::string& method,
        std::string& error) override {
        ++moveCalls;
        operationThreadId = GetCurrentThreadId();
        method = movementSucceeds ? "NativeLocomotion" : "NotApplied";
        if (!movementSucceeds) {
            error = "synthetic_move_failure";
        }
        return movementSucceeds;
    }
};

class MockGameCommandExecutor final : public aimp::IGameCommandExecutor {
public:
    [[nodiscard]] bool CanExecute(
        const std::string_view command) const noexcept override {
        return command == "PopupMessage";
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecutePopupMessage(
        const aimp::PopupMessageCommand&) const override {
        ++executions;
        executionThreadId = GetCurrentThreadId();
        return {true, "MOCK_EXECUTED", "Synthetic command executed."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteSpawnCharacter(
        const aimp::SpawnCharacterCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteSpawnNetZCharacter(
        const aimp::SpawnNetZCharacterCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteCharacterDirectMove(
        const aimp::CharacterDirectMoveCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteCharacterDirectStep(
        const aimp::CharacterDirectStepCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldRegistryProbe(
        const aimp::WorldRegistryProbeCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldManifestLoad(
        const aimp::WorldManifestLoadCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldNetProbe(
        const aimp::WorldNetProbeCommand&) const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldInteractionStats() const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldCompositeInputStats() const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldStateDiscoveryBaseline() const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldStateDiscoveryAfter() const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    [[nodiscard]] aimp::GameCommandExecutionResult ExecuteWorldStateDiscoveryStatus() const override {
        return {false, "MOCK_UNAVAILABLE", "Synthetic command unavailable."};
    }

    mutable std::uint32_t executions{};
    mutable std::uint32_t executionThreadId{};
};

aimp::DummyRequestIdentity TestDummyIdentity() {
    return {
        "11111111-1111-1111-1111-111111111111",
        2,
        "22222222-2222-2222-2222-222222222222",
        "33333333-3333-3333-3333-333333333333",
        "Dummy"};
}

void TestDummyController() {
    constexpr aimp::DummyNativeStep createOrder[]{
        aimp::DummyNativeStep::Participant,
        aimp::DummyNativeStep::Channel,
        aimp::DummyNativeStep::Descriptors,
        aimp::DummyNativeStep::Character,
        aimp::DummyNativeStep::ReplicationContext,
        aimp::DummyNativeStep::Binding,
    };
    constexpr aimp::DummyNativeStep cleanupOrder[]{
        aimp::DummyNativeStep::Binding,
        aimp::DummyNativeStep::Character,
        aimp::DummyNativeStep::ReplicationContext,
        aimp::DummyNativeStep::Descriptors,
        aimp::DummyNativeStep::Channel,
        aimp::DummyNativeStep::Participant,
    };

    for (std::size_t failureIndex = 0;
         failureIndex < std::size(createOrder);
         ++failureIndex) {
        aimp::DummyPlayerController controller;
        MockDummyExecutor executor;
        executor.failAcquire = createOrder[failureIndex];
        const auto result =
            controller.Create(TestDummyIdentity(), 10, executor);
        CHECK(result.state == aimp::DummyState::Failed);
        CHECK(result.statusCode == "DUMMY_CREATE_BLOCKED");
        std::vector<aimp::DummyNativeStep> expectedRelease;
        for (const auto step : cleanupOrder) {
            const auto acquired = std::find(
                createOrder,
                createOrder + failureIndex,
                step);
            if (acquired != createOrder + failureIndex) {
                expectedRelease.push_back(step);
            }
        }
        CHECK(executor.released == expectedRelease);
        const auto acquireCount = executor.acquired.size();
        const auto retry =
            controller.Create(TestDummyIdentity(), 10, executor);
        CHECK(retry.statusCode == "DUMMY_ALREADY_EXISTS");
        CHECK(executor.acquired.size() == acquireCount);
        controller.InvalidateGeneration(11);
        CHECK(controller.State() == aimp::DummyState::Absent);
    }

    aimp::DummyPlayerController controller;
    MockDummyExecutor executor;
    const auto created =
        controller.Create(TestDummyIdentity(), 20, executor);
    CHECK(created.state == aimp::DummyState::Active);
    CHECK(created.achieved == aimp::DummyAchievement::C);
    const auto moved = controller.Move(
        20,
        "Forward",
        1.0F,
        500,
        executor);
    CHECK(moved.achieved == aimp::DummyAchievement::D);
    const auto removed = controller.Remove(20, executor);
    CHECK(removed.state == aimp::DummyState::Absent);
    CHECK(executor.released == std::vector<aimp::DummyNativeStep>(
        std::begin(cleanupOrder),
        std::end(cleanupOrder)));

    aimp::DummyPlayerController verifyController;
    MockDummyExecutor verifyFailure;
    verifyFailure.failVerify = true;
    const auto verifyResult = verifyController.Create(
        TestDummyIdentity(),
        30,
        verifyFailure);
    CHECK(verifyResult.state == aimp::DummyState::Failed);
    CHECK(verifyFailure.released ==
          std::vector<aimp::DummyNativeStep>(
              std::begin(cleanupOrder),
              std::end(cleanupOrder)));

    aimp::DummyPlayerController blockedController;
    aimp::BlockedDummyExecutor blocked;
    const auto blockedResult = blockedController.Create(
        TestDummyIdentity(),
        40,
        blocked);
    CHECK(blockedResult.statusCode == "DUMMY_CREATE_BLOCKED");
    CHECK(blockedResult.achieved == aimp::DummyAchievement::None);
}

void TestHistoricalParticipantTransaction() {
    namespace hp = aimp::historical;

    static_assert(hp::kParticipantRecordSize == 0x158);
    static_assert(hp::kParticipantTableSize == 0xAC0);
    std::array<std::byte, hp::kParticipantTableSize> records{};
    std::uint32_t count = 1;
    std::uint8_t selectedLocalIndex = 0;
    hp::ParticipantTableView table{
        std::span<std::byte>(records),
        &count,
        &selectedLocalIndex};

    const auto writeU32 = [](
                              const std::span<std::byte> record,
                              const std::size_t offset,
                              const std::uint32_t value) {
        std::memcpy(
            record.data() + offset,
            &value,
            sizeof(value));
    };
    const auto readU32 = [](
                             const std::span<const std::byte> record,
                             const std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(
            &value,
            record.data() + offset,
            sizeof(value));
        return value;
    };
    hp::ParticipantIdentity128 localIdentity{};
    localIdentity.fill(std::byte{0x11});
    const auto initialize = [&]() {
        records.fill(std::byte{0});
        count = 1;
        selectedLocalIndex = 0;
        auto local = table.records.first(
            hp::kParticipantRecordSize);
        std::memcpy(
            local.data() + hp::kIdentityOffset,
            localIdentity.data(),
            localIdentity.size());
        writeU32(local, hp::kFlagsOffset, 0xDU);
        writeU32(
            local,
            hp::kDefaultVtableOffset,
            0x12345678U);
        auto inactive = table.records.subspan(
            hp::kParticipantRecordSize,
            hp::kParticipantRecordSize);
        std::fill(
            inactive.begin(),
            inactive.end(),
            std::byte{0xA5});
    };
    initialize();

    hp::DummyParticipantRecordSpec spec{};
    spec.nativeIdentity.fill(std::byte{0x42});
    spec.bindingKeyF4 = 0xD00DF00DU;
    spec.displayName = "Runtime Dummy";

    hp::HistoricalParticipantTransaction transaction;
    hp::ParticipantRecord prepared{};
    const auto constructed =
        transaction.ConstructDummyRecord(table, spec, prepared);
    CHECK(constructed.Succeeded());
    CHECK(std::memcmp(
              prepared.data(),
              spec.displayName.data(),
              spec.displayName.size()) == 0);
    CHECK(prepared[spec.displayName.size()] == std::byte{0});
    CHECK(std::memcmp(
              prepared.data() + hp::kIdentityOffset,
              spec.nativeIdentity.data(),
              spec.nativeIdentity.size()) == 0);
    const auto preparedView =
        std::span<const std::byte>(prepared);
    CHECK(readU32(preparedView, hp::kFlagsOffset) ==
          hp::kRemoteUnboundFlags);
    CHECK(readU32(
              preparedView,
              hp::kDefaultVtableOffset) ==
          0x12345678U);
    CHECK(readU32(
              preparedView,
              hp::kAssociatedObjectOffset) == 0);
    CHECK(readU32(
              preparedView,
              hp::kAssociatedValueOffset) == 0);
    CHECK(readU32(
              preparedView,
              hp::kDefaultScaleOffset) ==
          0x3F800000U);
    CHECK(readU32(
              preparedView,
              hp::kBindingKeyOffset) ==
          spec.bindingKeyF4);
    CHECK(readU32(
              preparedView,
              hp::kTrailingIndexOffset) ==
          0xFFFFFFFFU);

    const auto appended =
        transaction.AppendSynthetic(table, spec, 17);
    CHECK(appended.Succeeded());
    CHECK(appended.mutated);
    CHECK(!appended.rollbackPerformed);
    CHECK(transaction.Active());
    CHECK(transaction.Generation() == 17);
    CHECK(transaction.DummyIndex() == 1);
    CHECK(transaction.Record() == prepared);
    CHECK(count == 2);
    CHECK(selectedLocalIndex == 0);
    auto dummy = table.records.subspan(
        hp::kParticipantRecordSize,
        hp::kParticipantRecordSize);
    CHECK(std::memcmp(
              dummy.data(),
              prepared.data(),
              prepared.size()) == 0);

    // Binding is allowed to update flags and local pointers; identity and F4
    // remain the transaction discriminators.
    writeU32(
        dummy,
        hp::kFlagsOffset,
        hp::kRemoteUnboundFlags |
            hp::kBoundParticipantBit);
    writeU32(
        dummy,
        hp::kAssociatedObjectOffset,
        0xCAFEBABEU);
    writeU32(
        dummy,
        hp::kAssociatedValueOffset,
        0x10203040U);
    CHECK(transaction.Validate(table, 17).Succeeded());
    CHECK(transaction.Validate(table, 18).code ==
          hp::ParticipantTransactionCode::GenerationMismatch);

    const auto rolledBack =
        transaction.RollbackLastSlot(table, 17);
    CHECK(rolledBack.Succeeded());
    CHECK(rolledBack.mutated);
    CHECK(rolledBack.rollbackPerformed);
    CHECK(!transaction.Active());
    CHECK(count == 1);
    CHECK(selectedLocalIndex == 0);
    CHECK(std::all_of(
        dummy.begin(),
        dummy.end(),
        [](const std::byte value) {
            return value == std::byte{0xA5};
        }));

    initialize();
    count = static_cast<std::uint32_t>(
        aimp::kNativeParticipantCapacity);
    CHECK(transaction.AppendSynthetic(table, spec, 17).code ==
          hp::ParticipantTransactionCode::CapacityReached);
    CHECK(count == aimp::kNativeParticipantCapacity);

    initialize();
    auto duplicateIdentitySpec = spec;
    duplicateIdentitySpec.nativeIdentity = localIdentity;
    CHECK(transaction.AppendSynthetic(
              table,
              duplicateIdentitySpec,
              17).code ==
          hp::ParticipantTransactionCode::DuplicateIdentity);
    CHECK(count == 1);

    initialize();
    writeU32(
        table.records.first(hp::kParticipantRecordSize),
        hp::kBindingKeyOffset,
        spec.bindingKeyF4);
    CHECK(transaction.AppendSynthetic(table, spec, 17).code ==
          hp::ParticipantTransactionCode::DuplicateBindingKey);
    CHECK(count == 1);

    initialize();
    auto invalidKeySpec = spec;
    invalidKeySpec.bindingKeyF4 = 0;
    CHECK(transaction.AppendSynthetic(
              table,
              invalidKeySpec,
              17).code ==
          hp::ParticipantTransactionCode::InvalidBindingKey);

    initialize();
    auto invalidIdentitySpec = spec;
    invalidIdentitySpec.nativeIdentity.fill(std::byte{0});
    CHECK(transaction.AppendSynthetic(
              table,
              invalidIdentitySpec,
              17).code ==
          hp::ParticipantTransactionCode::InvalidIdentity);

    initialize();
    auto longNameSpec = spec;
    const std::string longName(hp::kNameCapacity, 'x');
    longNameSpec.displayName = longName;
    CHECK(transaction.AppendSynthetic(
              table,
              longNameSpec,
              17).code ==
          hp::ParticipantTransactionCode::DisplayNameTooLong);

    initialize();
    CHECK(transaction.AppendSynthetic(table, spec, 21).Succeeded());
    count = 3;
    const auto unsafeRollback =
        transaction.RollbackLastSlot(table, 21);
    CHECK(unsafeRollback.code ==
          hp::ParticipantTransactionCode::RegistryChanged);
    CHECK(!unsafeRollback.mutated);
    CHECK(count == 3);
    transaction.InvalidateGeneration();
    CHECK(!transaction.Active());
    CHECK(transaction.RollbackLastSlot(table, 21).code ==
          hp::ParticipantTransactionCode::NoActiveTransaction);
    CHECK(hp::ToString(
              hp::ParticipantTransactionCode::DummyNotLast) ==
          "dummy_not_last");
}

void TestIpcDummyCommandsUseValidatedDispatcher() {
    constexpr std::string_view kDummyId =
        "11111111-1111-1111-1111-111111111111";
    const auto createPayload = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":2,"launcherPlayerId":"22222222-2222-2222-2222-222222222222","portableBindingId":"33333333-3333-3333-3333-333333333333","displayName":"Dummy"},"spawnOffset":{"x":1.25,"y":0,"z":-2.5}})");
    const auto collidingCreatePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":2,"launcherPlayerId":"22222222-2222-2222-2222-222222222222","portableBindingId":"33333333-3333-3333-3333-333333333333","displayName":"Dummy2"},"spawnOffset":{"x":1.25,"y":0,"z":-2.5}})");
    const auto concurrentCreatePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":4,"launcherPlayerId":"66666666-6666-6666-6666-666666666666","portableBindingId":"77777777-7777-7777-7777-777777777777","displayName":"Dummy4"},"spawnOffset":{"x":3,"y":0,"z":0}})");
    const auto movePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"Forward","amount":2.5,"durationMilliseconds":500})");
    const auto removePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"cccccccc-cccc-cccc-cccc-cccccccccccc","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111"})");
    const auto cancelledCreatePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"dddddddd-dddd-dddd-dddd-dddddddddddd","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":3,"launcherPlayerId":"44444444-4444-4444-4444-444444444444","portableBindingId":"55555555-5555-5555-5555-555555555555","displayName":"Dummy3"},"spawnOffset":{"x":2,"y":0,"z":0}})");
    const auto failClosedCreatePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"ffffffff-ffff-ffff-ffff-ffffffffffff","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":5,"launcherPlayerId":"88888888-8888-8888-8888-888888888888","portableBindingId":"99999999-9999-9999-9999-999999999999","displayName":"Dummy5"},"spawnOffset":{"x":4,"y":0,"z":0}})");
    const auto disconnectedCreatePayload = Bytes(
        R"({"schemaVersion":1,"requestId":"abababab-abab-abab-abab-abababababab","levelGeneration":1,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":6,"launcherPlayerId":"12121212-1212-1212-1212-121212121212","portableBindingId":"34343434-3434-3434-3434-343434343434","displayName":"Dummy6"},"spawnOffset":{"x":5,"y":0,"z":0}})");

    aimp::Diagnostics diagnostics;
    aimp::InboundQueue inbound;
    aimp::CapabilityMatrix capabilities;
    aimp::CaptureCatalog captures;
    aimp::LevelGenerationTracker generations;
    aimp::GameThreadDispatcher dispatcher(8);
    aimp::DummyPlayerController dummy;
    MockDummyExecutor executor;
    aimp::LauncherIPC ipc(diagnostics, inbound);

    const aimp::EngineRootObservation roots{
        0x1000,
        0x2000,
        0x3000,
        0x4000,
        0x5000};
    CHECK(generations.Observe(roots).generation == 1);
    dispatcher.SetGeneration(1);
    const auto gameThreadId = GetCurrentThreadId();
    CHECK(dispatcher.BindEngineThread(
              gameThreadId,
              aimp::EngineThreadSource::MainThreadCharacterProcessing,
              1) == aimp::EngineThreadBindResult::Bound);

    constexpr aimp::Capability required[]{
        aimp::Capability::ScheduleGameThreadWork,
        aimp::Capability::CreateDummyParticipant,
        aimp::Capability::ReplayCharacterSpawn,
        aimp::Capability::BindDummyCharacter,
        aimp::Capability::DestroyDummyCharacter,
    };
    for (const auto capability : required) {
        capabilities.Set(
            capability,
            aimp::CapabilityState::RuntimeValidated,
            "synthetic_dispatch_test_only");
    }
    ipc.AttachExperimentalServices(
        capabilities,
        captures,
        generations,
        dispatcher,
        dummy,
        executor);
    aimp::LauncherIPCTestAccess::PrimeExperimental(ipc, kDummyId);

    bool handled = false;
    std::uint32_t pipeThreadId = 0;
    std::thread pipeThread([&] {
        pipeThreadId = GetCurrentThreadId();
        handled = aimp::LauncherIPCTestAccess::Handle(
            ipc,
            aimp::protocol::MessageType::DummyCreate,
            createPayload);
    });
    pipeThread.join();
    CHECK(handled);
    CHECK(pipeThreadId != gameThreadId);
    CHECK(executor.beginCreateCalls == 0);
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(
        aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 1);

    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        createPayload,
        2));
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(executor.beginCreateCalls == 0);

    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        concurrentCreatePayload,
        3));
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(executor.beginCreateCalls == 0);
    auto frames =
        aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::DummyCreateResult);

    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        collidingCreatePayload,
        4));
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::ExperimentalError);
    CHECK(dispatcher.PendingCount() == 1);

    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(executor.beginCreateCalls == 1);
    CHECK(executor.operationThreadId == gameThreadId);
    CHECK(executor.lastSpawnOffset.x == 1.25F);
    CHECK(executor.lastSpawnOffset.y == 0.0F);
    CHECK(executor.lastSpawnOffset.z == -2.5F);
    CHECK(dummy.State() == aimp::DummyState::Active);
    CHECK(
        aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 0);
    CHECK(
        aimp::LauncherIPCTestAccess::CachedExperimental(ipc) == 2);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(std::count_if(
              frames.begin(),
              frames.end(),
              [](const aimp::protocol::Frame& frame) {
                  return frame.type ==
                      aimp::protocol::MessageType::DummyCreateResult;
              }) == 1);
    CHECK(std::count_if(
              frames.begin(),
              frames.end(),
              [](const aimp::protocol::Frame& frame) {
                  return frame.type ==
                      aimp::protocol::MessageType::DummyBindingState;
              }) == 1);

    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        createPayload,
        5));
    CHECK(executor.beginCreateCalls == 1);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::DummyCreateResult);

    handled = false;
    std::thread moveThread([&] {
        handled = aimp::LauncherIPCTestAccess::Handle(
            ipc,
            aimp::protocol::MessageType::DummyMove,
            movePayload,
            6);
    });
    moveThread.join();
    CHECK(handled);
    CHECK(executor.moveCalls == 0);
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(executor.moveCalls == 1);
    CHECK(executor.operationThreadId == gameThreadId);
    CHECK(dummy.Achievement() == aimp::DummyAchievement::D);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(std::count_if(
              frames.begin(),
              frames.end(),
              [](const aimp::protocol::Frame& frame) {
                  return frame.type ==
                      aimp::protocol::MessageType::DummyMoveResult;
              }) == 1);

    handled = false;
    std::thread removeThread([&] {
        handled = aimp::LauncherIPCTestAccess::Handle(
            ipc,
            aimp::protocol::MessageType::DummyRemove,
            removePayload,
            7);
    });
    removeThread.join();
    CHECK(handled);
    CHECK(dummy.State() == aimp::DummyState::Active);
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(executor.operationThreadId == gameThreadId);
    CHECK(dummy.State() == aimp::DummyState::Absent);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(std::count_if(
              frames.begin(),
              frames.end(),
              [](const aimp::protocol::Frame& frame) {
                  return frame.type ==
                      aimp::protocol::MessageType::DummyRemoveResult;
              }) == 1);

    const auto beginCalls = executor.beginCreateCalls;
    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        cancelledCreatePayload,
        8));
    CHECK(dispatcher.PendingCount() == 1);
    dispatcher.SetGeneration(2);
    CHECK(executor.beginCreateCalls == beginCalls);
    CHECK(dispatcher.PendingCount() == 0);
    CHECK(
        aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 0);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(std::count_if(
              frames.begin(),
              frames.end(),
              [](const aimp::protocol::Frame& frame) {
                  return frame.type ==
                      aimp::protocol::MessageType::DummyCreateResult;
              }) == 1);

    for (const auto capability : required) {
        capabilities.Set(
            capability,
            aimp::CapabilityState::Blocked,
            "synthetic_fail_closed_check");
    }
    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        failClosedCreatePayload,
        9));
    CHECK(executor.beginCreateCalls == beginCalls);
    CHECK(dispatcher.PendingCount() == 0);
    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::DummyCreateResult);

    for (const auto capability : required) {
        capabilities.Set(
            capability,
            aimp::CapabilityState::RuntimeValidated,
            "synthetic_disconnect_epoch_check");
    }
    dispatcher.SetGeneration(1);
    CHECK(dispatcher.BindEngineThread(
              gameThreadId,
              aimp::EngineThreadSource::CharacterManagerUpdate,
              2) == aimp::EngineThreadBindResult::AlreadyBound);
    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::DummyCreate,
        disconnectedCreatePayload,
        10));
    CHECK(dispatcher.PendingCount() == 1);
    aimp::LauncherIPCTestAccess::Disconnect(ipc);
    CHECK(
        aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 0);
    const auto dispatcherAfterDisconnect = dispatcher.Snapshot();
    CHECK(dispatcherAfterDisconnect.ready);
    CHECK(dispatcherAfterDisconnect.engineThreadId == gameThreadId);
    CHECK(
        dispatcherAfterDisconnect.source ==
        aimp::EngineThreadSource::MainThreadCharacterProcessing);
    dispatcher.Clear();
    CHECK(executor.beginCreateCalls == beginCalls);
    CHECK(
        aimp::LauncherIPCTestAccess::PublicationMarkersReset(ipc));
}

void TestIpcGameCommandQueuesBeforeEngineBinding() {
    aimp::Diagnostics diagnostics;
    aimp::InboundQueue inbound;
    aimp::CapabilityMatrix capabilities;
    aimp::CaptureCatalog captures;
    aimp::LevelGenerationTracker generations;
    aimp::GameThreadDispatcher dispatcher(4);
    aimp::DummyPlayerController dummy;
    MockDummyExecutor dummyExecutor;
    MockGameCommandExecutor gameCommands;
    aimp::LauncherIPC ipc(diagnostics, inbound);

    const aimp::EngineRootObservation roots{
        0x1000,
        0x2000,
        0x3000,
        0x4000,
        0x5000};
    const auto generation = generations.Observe(roots).generation;
    dispatcher.SetGeneration(generation);
    dispatcher.SetHookAvailability(true, true, true);
    ipc.AttachExperimentalServices(
        capabilities,
        captures,
        generations,
        dispatcher,
        dummy,
        dummyExecutor);
    ipc.AttachGameCommandServices(gameCommands);
    aimp::LauncherIPCTestAccess::PrimeGeneric(ipc);

    const auto payload = Bytes(
        R"({"schemaVersion":1,"requestId":"91919191-9191-9191-9191-919191919191","levelGeneration":0,"portableDummyId":"81818181-8181-8181-8181-818181818181","command":"PopupMessage","arguments":{"headerText":"AIMP","mainText":"Queued","duration":"5.0","icon":"Alert","soundEvent":"OBJECTIVE_NEW"}})");
    CHECK(aimp::LauncherIPCTestAccess::Handle(
        ipc,
        aimp::protocol::MessageType::GameCommandExecute,
        payload));
    CHECK(dispatcher.PendingCount() == 1);
    CHECK(gameCommands.executions == 0);
    CHECK(aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 1);

    auto frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::GameCommandResult);
    const std::string queuedJson(
        reinterpret_cast<const char*>(frames.front().payload.data()),
        frames.front().payload.size());
    CHECK(queuedJson.find("QUEUED_WAITING_FOR_ENGINE_THREAD") !=
          std::string::npos);

    const auto engineThreadId = GetCurrentThreadId();
    CHECK(dispatcher.BindEngineThread(
              engineThreadId,
              aimp::EngineThreadSource::MainThreadCharacterProcessing,
              1000) == aimp::EngineThreadBindResult::Bound);
    CHECK(dispatcher.DrainOnEngineThread(1) == 1);
    CHECK(gameCommands.executions == 1);
    CHECK(gameCommands.executionThreadId == engineThreadId);
    CHECK(aimp::LauncherIPCTestAccess::PendingExperimental(ipc) == 0);

    frames = aimp::LauncherIPCTestAccess::TakeQueuedFrames(ipc);
    CHECK(frames.size() == 1);
    CHECK(frames.front().type ==
          aimp::protocol::MessageType::GameCommandResult);
    const std::string executedJson(
        reinterpret_cast<const char*>(frames.front().payload.data()),
        frames.front().payload.size());
    CHECK(executedJson.find(R"("statusCode":"EXECUTED")") !=
          std::string::npos);
}

void TestNativeLimits() {
    CHECK(aimp::kNativeParticipantCapacity == 8);
    CHECK(aimp::ParticipantSlotAvailable(0));
    CHECK(aimp::ParticipantSlotAvailable(7));
    CHECK(!aimp::ParticipantSlotAvailable(8));
    CHECK(!aimp::ParticipantSlotAvailable(9));
    CHECK(aimp::kNativeChannelCapacity == 32);
    CHECK(aimp::kMaximumCharacterDirectMoveDirectionMagnitude == 2.0F);
    constexpr auto observedWalkX = 0.466112F;
    constexpr auto observedWalkZ = 1.052107F;
    CHECK(observedWalkX * observedWalkX + observedWalkZ * observedWalkZ <=
          aimp::kMaximumCharacterDirectMoveDirectionMagnitude *
              aimp::kMaximumCharacterDirectMoveDirectionMagnitude);
    CHECK(!aimp::ChannelSlotAvailable(0));
    CHECK(aimp::ChannelSlotAvailable(1));
    CHECK(aimp::ChannelSlotAvailable(0x80000000U));
}

void TestExperimentalProtocol() {
    constexpr std::string_view requestId =
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    constexpr std::string_view dummyId =
        "11111111-1111-1111-1111-111111111111";
    CHECK(aimp::experimental::IsResolutionMode("ExactProfile"));
    CHECK(aimp::experimental::IsResolutionMode(
        "LocalDiscoveredProfile"));
    CHECK(aimp::experimental::IsResolutionMode(
        "GenericDiscovery"));
    CHECK(!aimp::experimental::IsResolutionMode("exact_profile"));
    CHECK(!aimp::experimental::IsResolutionMode("diagnostic_only"));
    const auto configure = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":0,"portableDummyId":"11111111-1111-1111-1111-111111111111","enabled":true})");
    aimp::experimental::TestModeConfigureRequest configureRequest;
    CHECK(aimp::experimental::DecodeTestModeConfigure(
        configure,
        configureRequest));
    CHECK(configureRequest.requestId == requestId);
    CHECK(configureRequest.portableDummyId == dummyId);
    CHECK(configureRequest.enabled);

    const auto query = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111"})");
    aimp::experimental::CapabilityQueryRequest queryRequest;
    CHECK(aimp::experimental::DecodeCapabilityQuery(
        query,
        queryRequest));
    CHECK(queryRequest.levelGeneration == 3);

    const auto create = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":2,"launcherPlayerId":"22222222-2222-2222-2222-222222222222","portableBindingId":"33333333-3333-3333-3333-333333333333","displayName":"Dummy"},"spawnOffset":{"x":1.25,"y":0,"z":-2.5}})");
    aimp::experimental::DummyCreateRequest createRequest;
    CHECK(aimp::experimental::DecodeDummyCreate(
        create,
        createRequest));
    CHECK(createRequest.identity.peerId == 2);
    CHECK(createRequest.offsetX == 1.25F);
    CHECK(createRequest.offsetZ == -2.5F);

    const auto tooLongName = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":2,"launcherPlayerId":"22222222-2222-2222-2222-222222222222","portableBindingId":"33333333-3333-3333-3333-333333333333","displayName":"1234567890123456789012345"},"spawnOffset":{"x":1,"y":0,"z":0}})");
    CHECK(!aimp::experimental::DecodeDummyCreate(
        tooLongName,
        createRequest));
    const auto leadingSpaceName = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","identity":{"portableDummyId":"11111111-1111-1111-1111-111111111111","peerId":2,"launcherPlayerId":"22222222-2222-2222-2222-222222222222","portableBindingId":"33333333-3333-3333-3333-333333333333","displayName":" Dummy"},"spawnOffset":{"x":1,"y":0,"z":0}})");
    CHECK(!aimp::experimental::DecodeDummyCreate(
        leadingSpaceName,
        createRequest));
    auto invalidUtf8 = create;
    const auto dummyPattern = Bytes("Dummy");
    const auto name = std::search(
        invalidUtf8.begin(),
        invalidUtf8.end(),
        dummyPattern.begin(),
        dummyPattern.end());
    if (name != invalidUtf8.end()) {
        *name = std::byte{0xC3};
        *(name + 1) = std::byte{0x28};
    }
    CHECK(!aimp::experimental::DecodeDummyCreate(
        invalidUtf8,
        createRequest));

    const auto move = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"Forward","amount":2.5,"durationMilliseconds":500})");
    aimp::experimental::DummyMoveRequest moveRequest;
    CHECK(aimp::experimental::DecodeDummyMove(move, moveRequest));
    CHECK(moveRequest.command == "Forward");
    CHECK(moveRequest.durationMilliseconds == 500);

    const auto gameCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"PopupMessage","arguments":{"headerText":"AIMP","mainText":"Bonjour","duration":"5.0","icon":"Alert","soundEvent":"OBJECTIVE_NEW"}})");
    aimp::experimental::GameCommandRequest gameCommandRequest;
    CHECK(aimp::experimental::DecodeGameCommand(
        gameCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "PopupMessage");
    CHECK(gameCommandRequest.popupMessage.headerText == "AIMP");
    CHECK(gameCommandRequest.popupMessage.mainText == "Bonjour");
    CHECK(gameCommandRequest.popupMessage.durationSeconds == 5.0F);
    CHECK(gameCommandRequest.popupMessage.soundEvent == 1U);

    const auto spawnCharacterCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"SpawnCharacter","arguments":{"x":"-47.270","y":"19.258","z":"-17.890"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        spawnCharacterCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "SpawnCharacter");
    CHECK(gameCommandRequest.spawnCharacter.x == -47.270F);
    CHECK(gameCommandRequest.spawnCharacter.y == 19.258F);
    CHECK(gameCommandRequest.spawnCharacter.z == -17.890F);

    const auto characterDirectMoveCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.466112","directionZ":"1.052107","intensity":"1.000","frameCount":"180"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterDirectMoveCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "CHARACTER_DIRECT_MOVE");
    CHECK(gameCommandRequest.characterDirectMove.characterAddress ==
        0x2BD73220U);
    CHECK(gameCommandRequest.characterDirectMove.directionX == 0.466112F);
    CHECK(gameCommandRequest.characterDirectMove.directionZ == 1.052107F);
    CHECK(gameCommandRequest.characterDirectMove.intensity == 1.0F);
    CHECK(gameCommandRequest.characterDirectMove.frameCount == 180U);
    CHECK(!gameCommandRequest.characterDirectMove.worldSpace);

    const auto characterWorldDirectMoveCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000322","directionZ":"0.059105","intensity":"1.250","frameCount":"30","directionSpace":"world"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterWorldDirectMoveCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.characterDirectMove.worldSpace);
    CHECK(!gameCommandRequest.characterDirectMove.hasReplicationPosition);
    CHECK(gameCommandRequest.characterDirectMove.intensity == 1.25F);

    const auto characterWorldReplicationCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.059000","directionZ":"0.000000","intensity":"1.100","frameCount":"30","directionSpace":"world","replicationPositionX":"12.500","replicationPositionY":"19.258","replicationPositionZ":"-8.250","replicationYaw":"1.250000","moveType":"2"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterWorldReplicationCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.characterDirectMove.worldSpace);
    CHECK(gameCommandRequest.characterDirectMove.hasReplicationPosition);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionX == 12.5F);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionY == 19.258F);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionZ == -8.25F);
    CHECK(gameCommandRequest.characterDirectMove.hasReplicationYaw);
    CHECK(gameCommandRequest.characterDirectMove.replicationYaw == 1.25F);
    CHECK(gameCommandRequest.characterDirectMove.hasMoveType);
    CHECK(gameCommandRequest.characterDirectMove.moveType == 2U);

    const auto characterYawStopCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000000","directionZ":"0.000000","intensity":"1.100","frameCount":"0","directionSpace":"world","replicationYaw":"-2.500000"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterYawStopCommand, gameCommandRequest));
    CHECK(gameCommandRequest.characterDirectMove.hasReplicationYaw);
    CHECK(gameCommandRequest.characterDirectMove.replicationYaw == -2.5F);

    const auto invalidLocalReplicationPosition = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.059000","directionZ":"0.000000","intensity":"1.100","frameCount":"30","directionSpace":"local","replicationPositionX":"12.500","replicationPositionY":"19.258","replicationPositionZ":"-8.250","replicationYaw":"1.250000","moveType":"2"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidLocalReplicationPosition,
        gameCommandRequest));

    const auto characterReplicationStopPosition = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000000","directionZ":"0.000000","intensity":"1.100","frameCount":"0","directionSpace":"world","replicationPositionX":"12.500","replicationPositionY":"19.258","replicationPositionZ":"-8.250","replicationYaw":"1.250000","moveType":"2"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterReplicationStopPosition,
        gameCommandRequest));
    CHECK(gameCommandRequest.characterDirectMove.frameCount == 0U);
    CHECK(gameCommandRequest.characterDirectMove.hasReplicationPosition);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionX == 12.5F);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionY == 19.258F);
    CHECK(gameCommandRequest.characterDirectMove.replicationPositionZ == -8.25F);

    const auto invalidCharacterDirectionSpace = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000322","directionZ":"0.059105","intensity":"1.100","frameCount":"30","directionSpace":"camera"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidCharacterDirectionSpace,
        gameCommandRequest));

    const auto characterDirectStepCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_STEP","arguments":{"characterAddress":"0x2BD73220","deltaX":"0.000","deltaY":"0.000","deltaZ":"0.100","deltaYaw":"0.000","frameCount":"180"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterDirectStepCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "CHARACTER_DIRECT_STEP");
    CHECK(gameCommandRequest.characterDirectStep.characterAddress ==
        0x2BD73220U);
    CHECK(gameCommandRequest.characterDirectStep.deltaX == 0.0F);
    CHECK(gameCommandRequest.characterDirectStep.deltaY == 0.0F);
    CHECK(gameCommandRequest.characterDirectStep.deltaZ == 0.1F);
    CHECK(gameCommandRequest.characterDirectStep.deltaYaw == 0.0F);
    CHECK(gameCommandRequest.characterDirectStep.frameCount == 180U);

    const auto invalidCharacterDirectStepCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_STEP","arguments":{"characterAddress":"0x00000000","deltaX":"0.000","deltaY":"0.000","deltaZ":"0.100","deltaYaw":"0.000","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidCharacterDirectStepCommand,
        gameCommandRequest));

    const auto oversizedCharacterDirectStepCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_STEP","arguments":{"characterAddress":"0x2BD73220","deltaX":"0.000","deltaY":"0.000","deltaZ":"10.100","deltaYaw":"0.000","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        oversizedCharacterDirectStepCommand,
        gameCommandRequest));

    const auto oversizedCharacterDirectStepFramesCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_STEP","arguments":{"characterAddress":"0x2BD73220","deltaX":"0.020","deltaY":"0.000","deltaZ":"0.000","deltaYaw":"0.000","frameCount":"601"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        oversizedCharacterDirectStepFramesCommand,
        gameCommandRequest));

    const auto invalidCharacterDirectMoveCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x00000000","directionX":"0.000322","directionZ":"0.059105","intensity":"1.100","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidCharacterDirectMoveCommand,
        gameCommandRequest));

    const auto invalidCharacterDirectMoveIntensity = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000322","directionZ":"0.059105","intensity":"0.900","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidCharacterDirectMoveIntensity,
        gameCommandRequest));

    const auto zeroCharacterDirectMoveDirection = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000","directionZ":"0.000","intensity":"1.100","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        zeroCharacterDirectMoveDirection,
        gameCommandRequest));

    const auto oversizedCharacterDirectMoveDirection = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"2.000","directionZ":"2.000","intensity":"1.100","frameCount":"180"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        oversizedCharacterDirectMoveDirection,
        gameCommandRequest));

    const auto stopCharacterDirectMoveCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000","directionZ":"0.000","intensity":"1.100","frameCount":"0"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        stopCharacterDirectMoveCommand,
        gameCommandRequest));

    const auto characterLongMoveCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"CHARACTER_DIRECT_MOVE","arguments":{"characterAddress":"0x2BD73220","directionX":"0.000","directionZ":"0.059","intensity":"1.100","frameCount":"480"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        characterLongMoveCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.characterDirectMove.frameCount == 480U);

    const auto spawnNetZCharacterCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"SpawnNetZCharacter","arguments":{"x":"-47.270","y":"19.258","z":"-17.890","participantKey":"1","displayName":"RemotePlayer"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        spawnNetZCharacterCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "SpawnNetZCharacter");
    CHECK(gameCommandRequest.spawnNetZCharacter.x == -47.270F);
    CHECK(gameCommandRequest.spawnNetZCharacter.y == 19.258F);
    CHECK(gameCommandRequest.spawnNetZCharacter.z == -17.890F);
    CHECK(gameCommandRequest.spawnNetZCharacter.participantKey == 1U);
    CHECK(gameCommandRequest.spawnNetZCharacter.displayName == "RemotePlayer");

    const auto invalidSpawnNetZCharacterCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"SpawnNetZCharacter","arguments":{"x":"-47.270","y":"19.258","z":"-17.890","participantKey":"0","displayName":"RemotePlayer"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidSpawnNetZCharacterCommand,
        gameCommandRequest));

    const auto invalidSpawnCharacterCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"SpawnCharacter","arguments":{"x":"nan","y":"19.258","z":"-17.890"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidSpawnCharacterCommand,
        gameCommandRequest));

    const auto worldManifestLoadCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_MANIFEST_LOAD","arguments":{"manifestPath":"C:\\AIMP\\WorldManifestCache\\example.aimpwm"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldManifestLoadCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_MANIFEST_LOAD");
    CHECK(gameCommandRequest.worldManifestLoad.manifestPath ==
        "C:\\AIMP\\WorldManifestCache\\example.aimpwm");

    const auto worldNetProbeCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_NET_PROBE","arguments":{"entityId":"112233445566778899AABBCCDDEEFF00"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldNetProbeCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_NET_PROBE");
    CHECK(gameCommandRequest.worldNetProbe.entityHigh == 0x1122334455667788ULL);
    CHECK(gameCommandRequest.worldNetProbe.entityLow == 0x99AABBCCDDEEFF00ULL);

    const auto invalidWorldNetProbeCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_NET_PROBE","arguments":{"entityId":"00000000000000000000000000000000"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidWorldNetProbeCommand,
        gameCommandRequest));

    const auto worldInteractionStatsCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_INTERACTION_STATS","arguments":{}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldInteractionStatsCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_INTERACTION_STATS");

    const auto invalidWorldInteractionStatsCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_INTERACTION_STATS","arguments":{"unexpected":"1"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidWorldInteractionStatsCommand,
        gameCommandRequest));

    const auto worldCompositeInputStatsCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_COMPOSITE_INPUT_STATS","arguments":{}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldCompositeInputStatsCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_COMPOSITE_INPUT_STATS");

    const auto invalidWorldCompositeInputStatsCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_COMPOSITE_INPUT_STATS","arguments":{"unexpected":"1"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidWorldCompositeInputStatsCommand,
        gameCommandRequest));

    const auto ws3AdmissionCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_WS3_ADMISSION_SET","arguments":{"forbiddenBehaviorInputs":"1","manifestGuidPrefilter":"1","payloadlessOnly":"1","certifiedCallbackShape":"1","zeroTail":"1","playerTargetLocalOnly":"1","aiTargetLocalOnly":"1","uiTargetLocalOnly":"1","aimpTargetLocalOnly":"1","worldContinuation":"1","unknownCausality":"1","manifestOwnerInput":"0","manifestTargetContract":"1","localRootDescendants":"1"}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        ws3AdmissionCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_WS3_ADMISSION_SET");
    CHECK(gameCommandRequest.worldSync3AdmissionSecurityMask == 0x37FFU);

    const auto invalidWs3AdmissionCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_WS3_ADMISSION_SET","arguments":{"forbiddenBehaviorInputs":"1","manifestGuidPrefilter":"1","payloadlessOnly":"1","certifiedCallbackShape":"1","zeroTail":"1","playerTargetLocalOnly":"1","aiTargetLocalOnly":"1","uiTargetLocalOnly":"1","aimpTargetLocalOnly":"1","worldContinuation":"open","unknownCausality":"1","manifestOwnerInput":"0","manifestTargetContract":"1","localRootDescendants":"1"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidWs3AdmissionCommand,
        gameCommandRequest));

    const auto worldStateDiscoveryBaselineCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_STATE_DISCOVERY_BASELINE","arguments":{}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldStateDiscoveryBaselineCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_STATE_DISCOVERY_BASELINE");

    const auto worldStateDiscoveryAfterCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_STATE_DISCOVERY_AFTER","arguments":{}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldStateDiscoveryAfterCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_STATE_DISCOVERY_AFTER");

    const auto worldStateDiscoveryStatusCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_STATE_DISCOVERY_STATUS","arguments":{}})");
    CHECK(aimp::experimental::DecodeGameCommand(
        worldStateDiscoveryStatusCommand,
        gameCommandRequest));
    CHECK(gameCommandRequest.command == "WORLD_STATE_DISCOVERY_STATUS");

    const auto invalidWorldStateDiscoveryCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"WORLD_STATE_DISCOVERY_BASELINE","arguments":{"unexpected":"1"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidWorldStateDiscoveryCommand,
        gameCommandRequest));

    std::array<std::byte, 0x130> bootstrapPayload{};
    CHECK(aimp::GameCommandExecutor::PrepareBootstrapCivilianPayload(
        bootstrapPayload));
    constexpr char kCivilian[] = "CIVILIAN";
    CHECK(std::memcmp(
        bootstrapPayload.data() + 0xA4,
        kCivilian,
        sizeof(kCivilian)) == 0);
    std::uint32_t bootstrapEntityHigh = 0;
    std::uint32_t bootstrapResource = 0;
    std::memcpy(
        &bootstrapEntityHigh,
        bootstrapPayload.data() + 0x2C,
        sizeof(bootstrapEntityHigh));
    std::memcpy(
        &bootstrapResource,
        bootstrapPayload.data() + 0xFC,
        sizeof(bootstrapResource));
    CHECK(bootstrapEntityHigh == 0x5FDA4212U);
    CHECK(bootstrapResource == 0xD9A16D5BU);
    CHECK(bootstrapPayload[0xF4] == std::byte{0});
    CHECK(bootstrapPayload[0x100] == std::byte{0});

    auto dynamicPayload = bootstrapPayload;
    const std::uint32_t entityHigh = 0x5FDA4212U;
    std::memcpy(
        dynamicPayload.data() + 0x2C,
        &entityHigh,
        sizeof(entityHigh));
    const aimp::SpawnCharacterCommand spawnCommand{
        -47.270F,
        19.258F,
        -17.890F};
    CHECK(aimp::GameCommandExecutor::PrepareDynamicSpawnPayload(
        dynamicPayload,
        0x1234U,
        0x89ABCDEFU,
        7U,
        spawnCommand));
    std::uint32_t storedSpawnKey = 0;
    std::uint32_t storedExtendedSpawnKey = 0;
    std::uint32_t storedEntityKey = 0;
    std::uint32_t storedSelection = 0;
    float storedX = 0.0F;
    float storedY = 0.0F;
    float storedZ = 0.0F;
    std::memcpy(&storedSpawnKey, dynamicPayload.data(), sizeof(storedSpawnKey));
    std::memcpy(
        &storedExtendedSpawnKey,
        dynamicPayload.data() + 0xF0,
        sizeof(storedExtendedSpawnKey));
    std::memcpy(
        &storedEntityKey,
        dynamicPayload.data() + 0x28,
        sizeof(storedEntityKey));
    std::memcpy(
        &storedSelection,
        dynamicPayload.data() + 0x38,
        sizeof(storedSelection));
    std::memcpy(&storedX, dynamicPayload.data() + 0x50, sizeof(storedX));
    std::memcpy(&storedY, dynamicPayload.data() + 0x54, sizeof(storedY));
    std::memcpy(&storedZ, dynamicPayload.data() + 0x58, sizeof(storedZ));
    CHECK(storedSpawnKey == 0x1234U);
    CHECK(storedExtendedSpawnKey == 0x1234U);
    CHECK(storedEntityKey == 0x89ABCDEFU);
    CHECK(storedSelection == 7U);
    CHECK(storedX == spawnCommand.x);
    CHECK(storedY == spawnCommand.y);
    CHECK(storedZ == spawnCommand.z);
    CHECK(std::memcmp(
        dynamicPayload.data() + 0x2C,
        &entityHigh,
        sizeof(entityHigh)) == 0);

    CHECK(aimp::GameCommandExecutor::PrepareNetZSpawnPayload(
        dynamicPayload,
        1U));
    std::uint32_t storedParticipantKey = 0;
    std::memcpy(
        &storedParticipantKey,
        dynamicPayload.data() + 0x48,
        sizeof(storedParticipantKey));
    CHECK(storedParticipantKey == 1U);
    CHECK(dynamicPayload[0x4C] == std::byte{1});

    std::array<std::byte, 0x158> localParticipant{};
    std::array<std::byte, 0x158> remoteParticipant{};
    const std::uint32_t participantVtable = 0x015E7228U;
    std::memcpy(
        localParticipant.data() + 0xCC,
        &participantVtable,
        sizeof(participantVtable));
    const std::array<std::byte, 16> participantIdentity{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
        std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
        std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}};
    CHECK(aimp::GameCommandExecutor::PrepareRemoteParticipantRecord(
        localParticipant,
        remoteParticipant,
        1U,
        "RemotePlayer",
        participantIdentity));
    std::uint32_t remoteFlags = 0;
    std::uint32_t remoteObject = 1;
    std::uint32_t remoteValue = 1;
    std::uint32_t remoteKey = 0;
    std::memcpy(
        &remoteFlags,
        remoteParticipant.data() + 0xC8,
        sizeof(remoteFlags));
    std::memcpy(
        &remoteObject,
        remoteParticipant.data() + 0xD0,
        sizeof(remoteObject));
    std::memcpy(
        &remoteValue,
        remoteParticipant.data() + 0xD4,
        sizeof(remoteValue));
    std::memcpy(
        &remoteKey,
        remoteParticipant.data() + 0xF4,
        sizeof(remoteKey));
    CHECK(remoteFlags == 0x05U);
    CHECK(remoteObject == 0U);
    CHECK(remoteValue == 0U);
    CHECK(remoteKey == 1U);
    CHECK(std::memcmp(
        remoteParticipant.data(),
        "RemotePlayer",
        sizeof("RemotePlayer")) == 0);
    CHECK(std::memcmp(
        remoteParticipant.data() + 0x80,
        participantIdentity.data(),
        participantIdentity.size()) == 0);

    const auto invalidGameCommand = Bytes(
        R"({"schemaVersion":1,"requestId":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","levelGeneration":3,"portableDummyId":"11111111-1111-1111-1111-111111111111","command":"PopupMessage","arguments":{"headerText":"","mainText":"","duration":"5.0","icon":"Alert","soundEvent":"OBJECTIVE_NEW"}})");
    CHECK(!aimp::experimental::DecodeGameCommand(
        invalidGameCommand,
        gameCommandRequest));

    aimp::CapabilityMatrix matrix;
    aimp::ApplyFailClosedMutationPolicy(matrix);
    const auto recordsArray = matrix.Snapshot();
    const std::vector<aimp::CapabilityRecord> records(
        recordsArray.begin(),
        recordsArray.end());
    aimp::experimental::RequestEnvelope envelope{
        std::string(requestId),
        3,
        std::string(dummyId)};
    const auto capabilityBytes =
        aimp::experimental::EncodeCapabilityState(
            envelope,
            "ai-retail-profile",
            "ExactProfile",
            true,
            false,
            records,
            {"ScheduleGameThreadWork"},
            "Load a level");
    const std::string capabilityJson(
        reinterpret_cast<const char*>(capabilityBytes.data()),
        capabilityBytes.size());
    CHECK(capabilityJson.find(
              R"("resolutionMode":"ExactProfile")") !=
          std::string::npos);
    CHECK(capabilityJson.find(R"("state":"Blocked")") !=
          std::string::npos);
    CHECK(!std::regex_search(
        capabilityJson,
        std::regex(R"(0x[0-9A-Fa-f]{8})")));

    const auto captureBytes =
        aimp::experimental::EncodeCaptureState(
            envelope,
            true,
            true,
            true,
            1,
            true,
            true,
            false,
            2,
            "tick_correlation",
            27844,
            4,
            "CharacterReadinessProducer",
            10,
            20,
            3,
            true,
            true,
            true,
            "Ready",
            73,
            0x2C37BBB0,
            0x1C374418,
            true);
    const std::string captureJson(
        reinterpret_cast<const char*>(captureBytes.data()),
        captureBytes.size());
    CHECK(captureJson.find(
              R"("gameThreadCandidateId":27844)") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("gameThreadSourceCount":4)") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("engineThreadSource":"CharacterReadinessProducer")") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("characterReadinessProducerHookInstalled":true)") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("remoteCharacterState":"Ready")") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("remoteCharacterAddress":741850032)") !=
          std::string::npos);
    CHECK(captureJson.find(
              R"("characterCreateHookInstalled":true)") !=
          std::string::npos);

    aimp::DummyOperationResult operation{};
    operation.state = aimp::DummyState::Failed;
    operation.statusCode = "DUMMY_CREATE_BLOCKED";
    operation.message = "Blocked";
    operation.blockedGates = {"ScheduleGameThreadWork"};
    const auto resultBytes =
        aimp::experimental::EncodeDummyOperationResult(
            envelope,
            operation);
    const std::string resultJson(
        reinterpret_cast<const char*>(resultBytes.data()),
        resultBytes.size());
    CHECK(resultJson.find(R"("state":"Failed")") !=
          std::string::npos);
    CHECK(resultJson.find(R"("achievedLevel":"None")") !=
          std::string::npos);

    aimp::GameCommandExecutionResult commandResult{};
    commandResult.executed = true;
    commandResult.statusCode = "POPUP_QUEUED";
    commandResult.message = "Queued";
    const auto commandResultBytes =
        aimp::experimental::EncodeGameCommandResult(
            envelope,
            "PopupMessage",
            commandResult);
    const std::string commandResultJson(
        reinterpret_cast<const char*>(commandResultBytes.data()),
        commandResultBytes.size());
    CHECK(commandResultJson.find(R"("command":"PopupMessage")") !=
          std::string::npos);
    CHECK(commandResultJson.find(R"("executed":true)") !=
          std::string::npos);
}

void TestCharacterDirectStepNativeLifecycle() {
    constexpr auto rvas =
        aimp::character_direct_step_runtime::NativeMovementRvas();
    CHECK(rvas.activateLocomotionCycle == 0x00257900U);
    CHECK(rvas.enterIdleCycle == 0x0025BDD0U);
    CHECK(rvas.movementActionPostGaitSelection == 0x0005B874U);
    CHECK(rvas.npcLocomotionCycleUpdate == 0x0026D4B0U);
    CHECK(rvas.npcLocomotionVtable == 0x00EABF80U);
    CHECK(rvas.npcLocomotionPreTransformControl == 0x0026D4FEU);
    CHECK(rvas.npcLocomotionPostSubmit == 0x0026D5BAU);
    CHECK(rvas.submitLocomotion == 0x0028D6D0U);
    CHECK(rvas.teleportCharacter == 0x003ECCA0U);
    CHECK(aimp::character_direct_step_runtime::ReplicatedYawOffset() == 0x0174U);
    CHECK(aimp::character_direct_step_runtime::NativeLocomotionFlagWidth() == 1U);

    CHECK(aimp::character_direct_step_runtime::MaximumConcurrentTargets() == 40U);
    constexpr auto registry =
        aimp::character_direct_step_runtime::NativeCharacterRegistry();
    CHECK(registry.table == 0x01307A48U);
    CHECK(registry.stride == 0x00B0U);
    CHECK(registry.capacity == 18U);
    CHECK(registry.character == 0x0000U);
    CHECK(registry.physicalBody == 0x000CU);
    CHECK(registry.secondaryBody == 0x0044U);
    CHECK(registry.active == 0x00A8U);
    constexpr auto cycleOffsets =
        aimp::character_direct_step_runtime::NativeCycleOffsets();
    CHECK(cycleOffsets.magnitude == 0x004CU);
    CHECK(cycleOffsets.worldHeading == 0x0050U);
    CHECK(cycleOffsets.intensity == 0x005CU);

    aimp::character_direct_step_runtime::NativeCycleControlPlan control{};
    CHECK(aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        0.0F, 1.15073F, 1.1F, true, 0.0F, control));
    CHECK(std::abs(control.magnitude - 1.15073F) < 0.0001F);
    CHECK(std::abs(control.worldHeading) < 0.0001F);
    CHECK(std::abs(control.intensity - 1.1F) < 0.0001F);

    CHECK(aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        0.0F, -1.15073F, 1.1F, true, 0.0F, control));
    CHECK(std::abs(control.magnitude - 1.15073F) < 0.0001F);
    CHECK(std::abs(std::abs(control.worldHeading) - 3.14159265359F) < 0.0001F);

    CHECK(aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        1.15073F, 0.0F, 1.1F, true, -2.0F, control));
    CHECK(std::abs(control.worldHeading - 1.57079632679F) < 0.0001F);

    CHECK(aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        0.0F, 1.15073F, 1.1F, false, 1.57079632679F, control));
    CHECK(std::abs(control.worldHeading - 1.57079632679F) < 0.0001F);

    CHECK(aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        0.0F, 0.0F, 0.0F, true, 0.0F, control));
    CHECK(std::abs(control.magnitude) < 0.0001F);
    CHECK(std::abs(control.worldHeading) < 0.0001F);
    CHECK(std::abs(control.intensity) < 0.0001F);

    CHECK(!aimp::character_direct_step_runtime::TryBuildNativeCycleControlPlan(
        (std::numeric_limits<float>::quiet_NaN)(),
        0.0F,
        1.1F,
        true,
        0.0F,
        control));

    // Orientation-aware network control: a source Character facing +90 deg
    // and moving forward travels along world +X. The receiver-native heading
    // must cancel the receiver's own orientation subtraction so the retail
    // transform reconstructs local forward, not left/right.
    constexpr float halfPi = 1.57079632679F;
    constexpr float receiverOrientation = 0.7337664366F;
    CHECK(aimp::character_direct_step_runtime::
        TryBuildOrientationAwareNativeCycleControlPlan(
            1.15073F,
            0.0F,
            1.1F,
            halfPi,
            receiverOrientation,
            control));
    CHECK(std::abs(control.magnitude - 1.15073F) < 0.0001F);
    CHECK(std::abs(control.worldHeading - receiverOrientation) < 0.0001F);
    CHECK(std::abs(control.intensity - 1.1F) < 0.0001F);

    CHECK(aimp::character_direct_step_runtime::
        TryBuildOrientationAwareNativeCycleControlPlan(
            0.0F,
            1.15073F,
            1.1F,
            halfPi,
            receiverOrientation,
            control));
    const auto relativeHeading = std::remainder(
        control.worldHeading - receiverOrientation,
        6.28318530717958647692F);
    CHECK(std::abs(relativeHeading + halfPi) < 0.0001F);

    CHECK(!aimp::character_direct_step_runtime::
        TryBuildOrientationAwareNativeCycleControlPlan(
            (std::numeric_limits<float>::quiet_NaN)(),
            0.0F,
            1.1F,
            0.0F,
            0.0F,
            control));

    constexpr auto offsets =
        aimp::character_direct_step_runtime::PhysicalOffsets();
    CHECK(offsets.x == 0x1010U);
    CHECK(offsets.y == 0x1014U);
    CHECK(offsets.z == 0x1018U);
    CHECK(offsets.yaw == 0x1020U);
}


void TestWorldReplicationCodec() {
    WorldManifestIdentity manifest{};
    manifest.mapContentId = {0x0102030405060708ULL, 0x1112131415161718ULL};
    const WorldEntityId entity{0x1122334455667788ULL, 0x8877665544332211ULL};

    WorldIntent intent{};
    intent.manifest = manifest;
    intent.entity = entity;
    intent.levelGeneration = 7U;
    intent.sourcePeer = 0U;
    intent.clientSequence = 9U;
    intent.requestedKind = WorldMutationKind::Event;
    intent.operation = kWorldNetworkProbeOperation;
    intent.payload.size = 3U;
    intent.payload.bytes[0] = std::byte{1};
    intent.payload.bytes[1] = std::byte{2};
    intent.payload.bytes[2] = std::byte{3};

    std::vector<std::byte> encoded;
    CHECK(world_replication_codec::EncodeIntent(intent, encoded));
    CHECK(encoded.size() == 79U);
    WorldIntent decodedIntent{};
    CHECK(world_replication_codec::DecodeIntent(encoded, decodedIntent));
    CHECK(decodedIntent.manifest == intent.manifest);
    CHECK(decodedIntent.entity == intent.entity);
    CHECK(decodedIntent.clientSequence == intent.clientSequence);
    CHECK(decodedIntent.payload.size == 3U);
    CHECK(decodedIntent.payload.bytes[2] == std::byte{3});

    auto trailing = encoded;
    trailing.push_back(std::byte{0});
    CHECK(!world_replication_codec::DecodeIntent(trailing, decodedIntent));

    WorldMutation mutation{};
    mutation.manifest = manifest;
    mutation.kind = WorldMutationKind::Event;
    mutation.flags = WorldMutationFlagReliable | WorldMutationFlagSyntheticProbe;
    mutation.entity = entity;
    mutation.levelGeneration = 7U;
    mutation.sequence = 11U;
    mutation.timelineTick = 13U;
    mutation.sourcePeer = 17U;
    mutation.operation = kWorldNetworkProbeOperation;
    mutation.payload.size = 2U;
    mutation.payload.bytes[0] = std::byte{4};
    mutation.payload.bytes[1] = std::byte{5};
    CHECK(world_replication_codec::EncodeMutation(mutation, encoded));
    CHECK(encoded.size() == 98U);
    WorldMutation decodedMutation{};
    CHECK(world_replication_codec::DecodeMutation(encoded, decodedMutation));
    CHECK(decodedMutation.sequence == 11U);
    CHECK(decodedMutation.revision == 0U);
    CHECK(decodedMutation.flags == mutation.flags);
    CHECK(decodedMutation.payload.size == 2U);

    mutation.kind = WorldMutationKind::State;
    mutation.flags = WorldMutationFlagReliable | WorldMutationFlagReplaceable |
        WorldMutationFlagReplayable;
    mutation.sequence = 12U;
    mutation.revision = 1U;
    mutation.timelineTick = 14U;
    mutation.operation = 0x1234U;
    CHECK(world_replication_codec::EncodeMutation(mutation, encoded));
    CHECK(world_replication_codec::DecodeMutation(encoded, decodedMutation));
    CHECK(decodedMutation.revision == 1U);

    WorldMutationResult result{11U, WorldMutationResultCode::Accepted, true, true};
    CHECK(world_replication_codec::EncodeResult(result, encoded));
    CHECK(encoded.size() == 20U);
    WorldMutationResult decodedResult{};
    CHECK(world_replication_codec::DecodeResult(encoded, decodedResult));
    CHECK(decodedResult.sequence == result.sequence);
    CHECK(decodedResult.code == result.code);
    CHECK(decodedResult.entityResolved);
    CHECK(decodedResult.dispatched);
}

void TestPeerControlValidation() {
    const auto validJoin = Bytes(
        R"({"peerId":1234605616436508552,"launcherPlayerId":"71583ac1-5ab8-46b9-a1c4-52fbb3e0e24a","displayName":"Lambert","isHost":false,"ready":true})");
    aimp::PeerJoinControl peer;
    CHECK(aimp::ipc_validation::DecodePeerJoin(validJoin, peer));
    CHECK(peer.peerId == 0x1122334455667788ULL);
    CHECK(peer.launcherPlayerId ==
          "71583ac1-5ab8-46b9-a1c4-52fbb3e0e24a");
    CHECK(peer.displayName == "Lambert");
    CHECK(!peer.isHost);
    CHECK(peer.ready);

    const auto extraField = Bytes(
        R"({"peerId":1,"launcherPlayerId":"71583ac1-5ab8-46b9-a1c4-52fbb3e0e24a","displayName":"Lambert","isHost":false,"ready":true,"extra":1})");
    CHECK(!aimp::ipc_validation::DecodePeerJoin(extraField, peer));
    const auto whitespaceName = Bytes(
        R"({"peerId":1,"launcherPlayerId":"71583ac1-5ab8-46b9-a1c4-52fbb3e0e24a","displayName":" Lambert ","isHost":false,"ready":true})");
    CHECK(!aimp::ipc_validation::DecodePeerJoin(whitespaceName, peer));
    const auto formatCharacterName = Bytes(
        R"({"peerId":1,"launcherPlayerId":"71583ac1-5ab8-46b9-a1c4-52fbb3e0e24a","displayName":"Lam\u200Dbert","isHost":false,"ready":true})");
    CHECK(!aimp::ipc_validation::DecodePeerJoin(formatCharacterName, peer));
    const auto emptyGuid = Bytes(
        R"({"peerId":1,"launcherPlayerId":"00000000-0000-0000-0000-000000000000","displayName":"Lambert","isHost":false,"ready":true})");
    CHECK(!aimp::ipc_validation::DecodePeerJoin(emptyGuid, peer));

    std::uint64_t peerId = 0;
    const auto validLeave = Bytes(R"({"peerId":1234605616436508552})");
    CHECK(aimp::ipc_validation::DecodePeerLeave(validLeave, peerId));
    CHECK(peerId == 0x1122334455667788ULL);
    const auto zeroLeave = Bytes(R"({"peerId":0})");
    CHECK(!aimp::ipc_validation::DecodePeerLeave(zeroLeave, peerId));
    const auto extraLeave = Bytes(R"({"peerId":1,"extra":false})");
    CHECK(!aimp::ipc_validation::DecodePeerLeave(extraLeave, peerId));
    const auto nonCanonicalNumber = Bytes(R"({"peerId":01})");
    CHECK(!aimp::ipc_validation::DecodePeerLeave(nonCanonicalNumber, peerId));
}

std::vector<std::byte> BuildVirtualImage(const std::wstring& path) {
    const auto file = ReadFileBytes(path);
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
        return {};
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, file.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return {};
    }
    const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
    if (ntOffset + sizeof(IMAGE_NT_HEADERS32) > file.size()) {
        return {};
    }
    IMAGE_NT_HEADERS32 nt{};
    std::memcpy(&nt, file.data() + ntOffset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return {};
    }

    std::vector<std::byte> image(nt.OptionalHeader.SizeOfImage);
    const auto headerBytes = (std::min<std::size_t>)(
        nt.OptionalHeader.SizeOfHeaders,
        file.size());
    std::copy_n(file.begin(), headerBytes, image.begin());

    const auto sectionOffset =
        ntOffset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    for (std::uint16_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        const auto offset =
            sectionOffset + index * sizeof(IMAGE_SECTION_HEADER);
        if (offset + sizeof(section) > file.size()) {
            return {};
        }
        std::memcpy(&section, file.data() + offset, sizeof(section));
        if (section.PointerToRawData > file.size() ||
            section.SizeOfRawData > file.size() - section.PointerToRawData ||
            section.VirtualAddress > image.size() ||
            section.SizeOfRawData > image.size() - section.VirtualAddress) {
            return {};
        }
        std::copy_n(
            file.begin() + section.PointerToRawData,
            section.SizeOfRawData,
            image.begin() + section.VirtualAddress);
    }
    return image;
}

void TestProtocol() {
    CHECK(aimp::protocol::IsKnownMessageType(16));
    CHECK(aimp::protocol::IsKnownMessageType(17));
    CHECK(aimp::protocol::IsKnownMessageType(18));
    CHECK(aimp::protocol::IsKnownMessageType(27));
    CHECK(!aimp::protocol::IsKnownMessageType(28));
    CHECK(!aimp::protocol::IsKnownMessageType(199));
    for (std::uint16_t value = 200; value <= 216; ++value) {
        CHECK(aimp::protocol::IsKnownMessageType(value));
    }
    CHECK(!aimp::protocol::IsKnownMessageType(217));
    CHECK(!aimp::protocol::IsKnownMessageType(218));
    CHECK(!aimp::protocol::IsKnownMessageType(219));
    const std::array payload{
        std::byte{0x00},
        std::byte{0x11},
        std::byte{0xFE},
        std::byte{0xFF}};
    std::vector<std::byte> encoded;
    CHECK(aimp::protocol::EncodeFrame(
        aimp::protocol::MessageType::InboundNetzFrame,
        0,
        0x0102030405060708ULL,
        payload,
        encoded));
    CHECK(encoded.size() == aimp::protocol::kWireHeaderBytes + payload.size());
    CHECK(encoded[0] == std::byte{'A'});
    CHECK(encoded[1] == std::byte{'I'});
    CHECK(encoded[2] == std::byte{'M'});
    CHECK(encoded[3] == std::byte{'P'});

    aimp::protocol::Frame decoded{};
    std::size_t consumed = 0;
    CHECK(aimp::protocol::DecodeFrame(encoded, decoded, consumed) ==
        aimp::protocol::DecodeResult::Complete);
    CHECK(consumed == encoded.size());
    CHECK(decoded.type == aimp::protocol::MessageType::InboundNetzFrame);
    CHECK(decoded.flags == 0);
    CHECK(decoded.sequence == 0x0102030405060708ULL);
    CHECK(std::equal(decoded.payload.begin(), decoded.payload.end(), payload.begin()));

    auto invalidMagic = encoded;
    invalidMagic[0] = std::byte{0};
    CHECK(aimp::protocol::DecodeFrame(invalidMagic, decoded, consumed) ==
        aimp::protocol::DecodeResult::InvalidMagic);

    auto invalidVersion = encoded;
    invalidVersion[4] = std::byte{2};
    CHECK(aimp::protocol::DecodeFrame(invalidVersion, decoded, consumed) ==
        aimp::protocol::DecodeResult::UnsupportedVersion);

    auto invalidType = encoded;
    invalidType[6] = std::byte{0xFF};
    invalidType[7] = std::byte{0x7F};
    CHECK(aimp::protocol::DecodeFrame(invalidType, decoded, consumed) ==
        aimp::protocol::DecodeResult::InvalidType);

    auto truncated = encoded;
    truncated.pop_back();
    CHECK(aimp::protocol::DecodeFrame(truncated, decoded, consumed) ==
        aimp::protocol::DecodeResult::NeedMoreData);

    auto invalidFlags = encoded;
    invalidFlags[8] = std::byte{1};
    CHECK(aimp::protocol::DecodeFrame(invalidFlags, decoded, consumed) ==
        aimp::protocol::DecodeResult::InvalidFlags);
    std::vector<std::byte> rejectedEncoding;
    CHECK(!aimp::protocol::EncodeFrame(
        aimp::protocol::MessageType::Heartbeat,
        1,
        1,
        {},
        rejectedEncoding));

    auto oversized = encoded;
    const std::uint32_t tooLarge =
        static_cast<std::uint32_t>(aimp::kMaximumIpcPayload + 1);
    for (std::size_t index = 0; index < 4; ++index) {
        oversized[10 + index] =
            static_cast<std::byte>((tooLarge >> (index * 8U)) & 0xFFU);
    }
    CHECK(aimp::protocol::DecodeFrame(oversized, decoded, consumed) ==
        aimp::protocol::DecodeResult::PayloadTooLarge);

    std::vector<std::byte> maximumPayload(aimp::kMaximumIpcPayload);
    CHECK(aimp::protocol::EncodeFrame(
        aimp::protocol::MessageType::OutboundNetzFrame,
        0,
        2,
        maximumPayload,
        rejectedEncoding));
    maximumPayload.push_back(std::byte{0});
    CHECK(!aimp::protocol::EncodeFrame(
        aimp::protocol::MessageType::OutboundNetzFrame,
        0,
        3,
        maximumPayload,
        rejectedEncoding));
}

void TestQueue() {
    aimp::InboundQueue queue({2, 6, 4});
    aimp::InboundItem first{};
    first.payload.resize(3);
    CHECK(queue.Enqueue(std::move(first)));

    aimp::InboundItem second{};
    second.payload.resize(3);
    CHECK(queue.Enqueue(std::move(second)));

    aimp::InboundItem capacityRejected{};
    capacityRejected.payload.resize(1);
    CHECK(!queue.Enqueue(std::move(capacityRejected)));

    aimp::InboundItem output{};
    CHECK(queue.TryPop(output));
    CHECK(output.payload.size() == 3);

    aimp::InboundItem oversize{};
    oversize.payload.resize(5);
    CHECK(!queue.Enqueue(std::move(oversize)));

    const auto stats = queue.Stats();
    CHECK(stats.items == 1);
    CHECK(stats.bytes == 3);
    CHECK(stats.accepted == 2);
    CHECK(stats.rejectedCapacity == 1);
    CHECK(stats.rejectedOversize == 1);
    queue.Clear();
    CHECK(queue.Stats().items == 0);
}

void TestSignatureSearch() {
    const std::array haystack{
        std::byte{0x10},
        std::byte{0xAA},
        std::byte{0x22},
        std::byte{0xCC},
        std::byte{0x30}};
    const aimp::MaskedBytes unique{{0xAA, 0x00, 0xCC}, "x?x"};
    const auto found = aimp::SignatureResolver::FindUnique(haystack, unique);
    CHECK(found.has_value());
    CHECK(found.value_or(99) == 1);

    const aimp::MaskedBytes missing{{0xAA, 0xBB}, "xx"};
    CHECK(!aimp::SignatureResolver::FindUnique(haystack, missing).has_value());

    const std::array ambiguous{
        std::byte{0xAA},
        std::byte{0x01},
        std::byte{0xAA},
        std::byte{0x02}};
    const aimp::MaskedBytes oneByte{{0xAA}, "x"};
    CHECK(!aimp::SignatureResolver::FindUnique(ambiguous, oneByte).has_value());
}

int __cdecl SyntheticDetour() {
    return 7;
}

void TestHookManager() {
    constexpr std::array<std::byte, 6> machineCode{
        std::byte{0xB8}, // mov eax, 42
        std::byte{0x2A},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0xC3}, // ret
    };
    void* region = VirtualAlloc(
        nullptr,
        64,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    CHECK(region != nullptr);
    if (region == nullptr) {
        return;
    }
    std::memcpy(region, machineCode.data(), machineCode.size());
    FlushInstructionCache(GetCurrentProcess(), region, machineCode.size());

    using Function = int(__cdecl*)();
    const auto target = reinterpret_cast<Function>(region);
    CHECK(target() == 42);

    aimp::HookManager manager(true, true);
    void* trampoline = nullptr;
    std::atomic<void*> publishedTrampoline{nullptr};
    std::string error;
    aimp::HookRequest bad{};
    bad.name = "synthetic_bad_signature";
    bad.target = region;
    bad.detour = reinterpret_cast<void*>(&SyntheticDetour);
    bad.patchLength = 5;
    bad.expected = {{0x90, 0x2A, 0x00, 0x00, 0x00, 0xC3}, "xxxxxx"};
    bad.relocationSafePrefix = true;
    bad.trampolineOut = &trampoline;
    bad.publishedTrampolineOut = &publishedTrampoline;
    CHECK(!manager.Install(bad, error));
    CHECK(trampoline == nullptr);
    CHECK(publishedTrampoline.load(std::memory_order_acquire) == nullptr);
    CHECK(target() == 42);

    aimp::HookRequest good = bad;
    good.name = "synthetic_valid";
    good.expected = {{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3}, "xxxxxx"};
    error.clear();
    CHECK(manager.Install(good, error));
    CHECK(trampoline != nullptr);
    CHECK(publishedTrampoline.load(std::memory_order_acquire) ==
          trampoline);
    CHECK(manager.InstalledCount() == 1);
    CHECK(target() == 7);
    CHECK(reinterpret_cast<Function>(trampoline)() == 42);

    void* secondRegion = VirtualAlloc(
        nullptr,
        64,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    CHECK(secondRegion != nullptr);
    if (secondRegion == nullptr) {
        error.clear();
        manager.UninstallAll(error);
        manager.ReleaseRetiredTrampolines(true, error);
        VirtualFree(region, 0, MEM_RELEASE);
        return;
    }
    std::memcpy(secondRegion, machineCode.data(), machineCode.size());
    FlushInstructionCache(
        GetCurrentProcess(),
        secondRegion,
        machineCode.size());
    const auto secondTarget = reinterpret_cast<Function>(secondRegion);
    void* secondTrampoline = nullptr;
    std::atomic<void*> secondPublished{nullptr};
    auto second = good;
    second.name = "synthetic_scoped_rollback";
    second.target = secondRegion;
    second.trampolineOut = &secondTrampoline;
    second.publishedTrampolineOut = &secondPublished;
    const auto checkpoint = manager.InstalledCount();
    error.clear();
    CHECK(manager.Install(second, error));
    CHECK(manager.InstalledCount() == 2);
    CHECK(secondTarget() == 7);

    error.clear();
    CHECK(manager.UninstallFrom(checkpoint, error));
    CHECK(manager.InstalledCount() == 1);
    CHECK(manager.RetiredCount() == 1);
    CHECK(target() == 7);
    CHECK(secondTarget() == 42);
    CHECK(reinterpret_cast<Function>(secondTrampoline)() == 42);

    error.clear();
    CHECK(manager.UninstallAll(error));
    CHECK(manager.InstalledCount() == 0);
    CHECK(manager.RetiredCount() == 2);
    CHECK(target() == 42);
    CHECK(reinterpret_cast<Function>(trampoline)() == 42);

    error.clear();
    CHECK(manager.ReleaseRetiredTrampolines(true, error));
    CHECK(manager.RetiredCount() == 0);
    CHECK(trampoline == nullptr);
    CHECK(secondTrampoline == nullptr);
    CHECK(publishedTrampoline.load(std::memory_order_acquire) == nullptr);
    CHECK(secondPublished.load(std::memory_order_acquire) == nullptr);
    VirtualFree(secondRegion, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
}

void TestCathodePrimitivePayload() {
    using namespace aimp::cathode_primitives;

    WorldPayload payload{};
    TriggerInputRequest noSource{};
    noSource.inputShortGuid = 0x11223344U;
    CHECK(EncodeTriggerInputPayload(noSource, payload));
    CHECK(payload.size == kTriggerInputPayloadBytes);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[0]) == kPayloadVersion);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[1]) ==
        static_cast<std::uint8_t>(PrimitiveKind::TriggerInput));

    TriggerInputRequest decoded{};
    CHECK(DecodeTriggerInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    CHECK(decoded.inputShortGuid == noSource.inputShortGuid);
    CHECK(!decoded.hasSourceEntity);
    CHECK(!decoded.sourceEntity.Valid());

    TriggerInputRequest withSource{};
    withSource.inputShortGuid = 0xA1B2C3D4U;
    withSource.hasSourceEntity = true;
    withSource.sourceEntity = {0x0123456789ABCDEFULL, 0x1020304050607080ULL};
    CHECK(EncodeTriggerInputPayload(withSource, payload));
    decoded = {};
    CHECK(DecodeTriggerInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    CHECK(decoded.inputShortGuid == withSource.inputShortGuid);
    CHECK(decoded.hasSourceEntity);
    CHECK(decoded.sourceEntity == withSource.sourceEntity);

    payload.bytes[3] = std::byte{1};
    CHECK(!DecodeTriggerInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    payload.bytes[3] = std::byte{0};
    payload.bytes[2] = std::byte{0x80};
    CHECK(!DecodeTriggerInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));

    TriggerInputRequest invalid{};
    CHECK(!EncodeTriggerInputPayload(invalid, payload));
}


void TestWorldInteractionAdapterPayload() {
    using namespace aimp::world_interactions;

    ObservedInputEdge edge{};
    edge.sourceCategory = SourceCategory::GenericWorld;
    edge.schedulerParameterShortGuid = 0xA1B2C3D4U;
    edge.targetInputShortGuid = 0xF2FEBB21U;
    edge.observationId = 0x1020304050607080ULL;
    edge.sourceEntity = {0x0123456789ABCDEFULL, 0x8877665544332211ULL};
    edge.relativeDelaySeconds = 1.25F;

    WorldPayload payload{};
    CHECK(EncodeObservedInputEdgePayload(edge, payload));
    CHECK(payload.size == kObservedInputEdgePayloadBytes);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[0]) == kPayloadVersion);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[1]) == kObservedInputEdgeKind);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[2]) ==
        static_cast<std::uint8_t>(SourceCategory::GenericWorld));
    CHECK(payload.size == 48U);

    ObservedInputEdge decoded{};
    CHECK(DecodeObservedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    CHECK(decoded.sourceCategory == edge.sourceCategory);
    CHECK(decoded.schedulerParameterShortGuid == edge.schedulerParameterShortGuid);
    CHECK(decoded.targetInputShortGuid == edge.targetInputShortGuid);
    CHECK(decoded.observationId == edge.observationId);
    CHECK(decoded.sourceEntity == edge.sourceEntity);
    CHECK(std::fabs(decoded.relativeDelaySeconds - edge.relativeDelaySeconds) < 0.0001F);

    payload.bytes[3] = std::byte{1};
    CHECK(!DecodeObservedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    payload.bytes[3] = std::byte{0};
    payload.bytes[12] = std::byte{1};
    CHECK(!DecodeObservedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));
    payload.bytes[12] = std::byte{0};
    payload.bytes[44] = std::byte{1};
    CHECK(!DecodeObservedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decoded));

    edge.schedulerParameterShortGuid = 0U;
    CHECK(!EncodeObservedInputEdgePayload(edge, payload));
    edge.schedulerParameterShortGuid = 0xA1B2C3D4U;
    edge.targetInputShortGuid = 0U;
    CHECK(!EncodeObservedInputEdgePayload(edge, payload));

    ObservedCallbackInputEdge callbackEdge{};
    callbackEdge.sourceCategory = SourceCategory::GenericWorld;
    callbackEdge.schedulerParameterShortGuid = 0x1BF58EB1U;
    callbackEdge.targetInputShortGuid = 0xD2B3CC2BU;
    callbackEdge.callbackConstructorFlags = kCallbackReferenceConstructorFlags;
    callbackEdge.callbackPayloadSize = 0U;
    callbackEdge.callbackFamilyCallerRva = kCallbackReferenceFamilyCallerRva;
    callbackEdge.observationId = 0x2233445566778899ULL;
    callbackEdge.sourceEntity = {0x1111222233334444ULL, 0x5555666677778888ULL};
    callbackEdge.relativeDelaySeconds = 0.125F;
    CHECK(EncodeObservedCallbackInputEdgePayload(callbackEdge, payload));
    CHECK(payload.size == kObservedCallbackInputEdgePayloadBytes);
    CHECK(payload.size == 56U);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[1]) == kObservedCallbackInputEdgeKind);
    ObservedCallbackInputEdge decodedCallback{};
    CHECK(DecodeObservedCallbackInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCallback));
    CHECK(decodedCallback.sourceCategory == callbackEdge.sourceCategory);
    CHECK(decodedCallback.schedulerParameterShortGuid == callbackEdge.schedulerParameterShortGuid);
    CHECK(decodedCallback.targetInputShortGuid == callbackEdge.targetInputShortGuid);
    CHECK(decodedCallback.callbackConstructorFlags == kCallbackReferenceConstructorFlags);
    CHECK(decodedCallback.callbackPayloadSize == 0U);
    CHECK(decodedCallback.callbackFamilyCallerRva == kCallbackReferenceFamilyCallerRva);
    CHECK(decodedCallback.observationId == callbackEdge.observationId);
    CHECK(decodedCallback.sourceEntity == callbackEdge.sourceEntity);
    CHECK(std::fabs(decodedCallback.relativeDelaySeconds - callbackEdge.relativeDelaySeconds) < 0.0001F);
    payload.bytes[52] = std::byte{1};
    CHECK(!DecodeObservedCallbackInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCallback));
    callbackEdge.callbackFamilyCallerRva = 0x12345678U;
    CHECK(!EncodeObservedCallbackInputEdgePayload(callbackEdge, payload));
    callbackEdge.callbackFamilyCallerRva = kCallbackReferenceFamilyCallerRva;
    callbackEdge.callbackConstructorFlags = 0x10000000U;
    CHECK(!EncodeObservedCallbackInputEdgePayload(callbackEdge, payload));

    ObservedCharacterCallbackInputEdge characterEdge{};
    characterEdge.sourceCategory = SourceCategory::GenericWorld;
    characterEdge.schedulerParameterShortGuid = 0xEB826BC5U;
    characterEdge.targetInputShortGuid = 0x06478FC6U;
    characterEdge.callbackInputShortGuid = kCharacterCallbackInputShortGuid;
    characterEdge.callbackConstructorFlags = kCharacterCallbackConstructorFlags;
    characterEdge.callbackExecutionVtableRva = kCharacterCallbackExecutionVtableRva;
    characterEdge.callbackFamilyCallerRva = kCharacterCallbackFamilyCallerRva;
    characterEdge.observationId = 0x9988776655443322ULL;
    characterEdge.sourceEntity = {0x1111111122222222ULL, 0x3333333344444444ULL};
    characterEdge.callbackEntity = {0x5555555566666666ULL, 0x7777777788888888ULL};
    characterEdge.relativeDelaySeconds = 0.0F;
    CHECK(EncodeObservedCharacterCallbackInputEdgePayload(characterEdge, payload));
    CHECK(payload.size == 80U);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[1]) == kObservedCharacterCallbackInputEdgeKind);
    ObservedCharacterCallbackInputEdge decodedCharacter{};
    CHECK(DecodeObservedCharacterCallbackInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCharacter));
    CHECK(decodedCharacter.schedulerParameterShortGuid == characterEdge.schedulerParameterShortGuid);
    CHECK(decodedCharacter.targetInputShortGuid == characterEdge.targetInputShortGuid);
    CHECK(decodedCharacter.callbackEntity == characterEdge.callbackEntity);
    CHECK(decodedCharacter.sourceEntity == characterEdge.sourceEntity);
    payload.bytes[28] = std::byte{1};
    CHECK(!DecodeObservedCharacterCallbackInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCharacter));
    payload.bytes[28] = std::byte{0};
    characterEdge.callbackConstructorFlags = 0x80000000U;
    CHECK(!EncodeObservedCharacterCallbackInputEdgePayload(characterEdge, payload));

    ObservedTypedMessage message{};
    message.messageType = 0x100050U;
    message.observationId = 0xAABBCCDDEEFF0011ULL;
    message.sourceRuntimeKey = {0x11112222U, 0x33334444U};
    message.targetRuntimeKey = {0x55556666U, 0x77778888U};
    message.sourceWorldEntity = {0x1111111122222222ULL, 0x3333333344444444ULL};
    message.targetWorldEntity = {0x5555555566666666ULL, 0x7777777788888888ULL};
    message.relativeDelaySeconds = 0.75;
    const std::array<std::byte, 5> messageData{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x50}};

    // Baseline typed traffic remains wire-compatible v2 / 48-byte header.
    CHECK(EncodeObservedTypedMessagePayload(message, messageData, payload));
    CHECK(payload.size == kObservedTypedMessageHeaderBytes + messageData.size());
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[0]) == kObservedTypedMessagePayloadVersion);
    ObservedTypedMessage decodedMessage{};
    std::span<const std::byte> decodedData{};
    CHECK(DecodeObservedTypedMessagePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size),
        decodedMessage, decodedData));
    CHECK(decodedMessage.messageType == message.messageType);
    CHECK(decodedMessage.observationId == message.observationId);
    CHECK(decodedMessage.sourceRuntimeKey == message.sourceRuntimeKey);
    CHECK(decodedMessage.targetRuntimeKey == message.targetRuntimeKey);
    CHECK(!decodedMessage.stableWorldIdentityIncluded);
    CHECK(!decodedMessage.sourceWorldEntity.Valid());
    CHECK(!decodedMessage.targetWorldEntity.Valid());
    CHECK(std::fabs(decodedMessage.relativeDelaySeconds - message.relativeDelaySeconds) < 0.000001);
    CHECK(decodedData.size() == messageData.size());
    CHECK(std::equal(decodedData.begin(), decodedData.end(), messageData.begin()));

    // Fix3.14 bounded diagnostic extension: v3 adds two stable WorldEntityIds.
    message.stableWorldIdentityIncluded = true;
    CHECK(EncodeObservedTypedMessagePayload(message, messageData, payload));
    CHECK(payload.size == kObservedTypedMessageIdentityHeaderBytes + messageData.size());
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[0]) ==
        kObservedTypedMessageIdentityPayloadVersion);
    CHECK(DecodeObservedTypedMessagePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size),
        decodedMessage, decodedData));
    CHECK(decodedMessage.stableWorldIdentityIncluded);
    CHECK(decodedMessage.sourceWorldEntity == message.sourceWorldEntity);
    CHECK(decodedMessage.targetWorldEntity == message.targetWorldEntity);
    CHECK(decodedData.size() == messageData.size());
    CHECK(std::equal(decodedData.begin(), decodedData.end(), messageData.begin()));
    payload.bytes[42] = std::byte{1};
    CHECK(!DecodeObservedTypedMessagePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size),
        decodedMessage, decodedData));

    ObservedLogicSwitchState switchState{};
    switchState.observationId = 0xA1A2A3A4A5A6A7A8ULL;
    switchState.contextRuntimeKey = aimp::CathodeRuntimeKey{0x11223344U, 0x55667788U};
    switchState.enabled = true;
    CHECK(EncodeObservedLogicSwitchStatePayload(switchState, payload));
    CHECK(payload.size == kObservedLogicSwitchStatePayloadBytes);
    CHECK(payload.size == 24U);
    ObservedLogicSwitchState decodedSwitch{};
    CHECK(DecodeObservedLogicSwitchStatePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedSwitch));
    CHECK(decodedSwitch.observationId == switchState.observationId);
    CHECK(decodedSwitch.contextRuntimeKey == switchState.contextRuntimeKey);
    CHECK(decodedSwitch.enabled == switchState.enabled);
    payload.bytes[3] = std::byte{1};
    CHECK(!DecodeObservedLogicSwitchStatePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedSwitch));

    CompositePublicInputEvent compositeInput{};
    compositeInput.observationId = 0x123456789ABCDEF0ULL;
    compositeInput.publicInputShortGuid = 0xCAFEBABEU;
    compositeInput.retailRoot = true;
    CHECK(EncodeCompositePublicInputPayload(compositeInput, payload));
    CHECK(payload.size == kCompositePublicInputPayloadBytes);
    CHECK(payload.size == 16U);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[0]) == kCompositePublicRootPayloadVersion);
    CHECK(std::to_integer<std::uint8_t>(payload.bytes[1]) == kCompositePublicInputKind);
    CompositePublicInputEvent decodedCompositeInput{};
    CHECK(DecodeCompositePublicInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCompositeInput));
    CHECK(decodedCompositeInput.retailRoot);
    CHECK(decodedCompositeInput.observationId == compositeInput.observationId);
    CHECK(decodedCompositeInput.publicInputShortGuid == compositeInput.publicInputShortGuid);
    payload.bytes[0] = std::byte{2};
    CHECK(!DecodeCompositePublicInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCompositeInput));
    payload.bytes[0] = static_cast<std::byte>(kCompositePublicRootPayloadVersion);
    payload.bytes[2] = std::byte{1};
    CHECK(!DecodeCompositePublicInputPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedCompositeInput));
    compositeInput.publicInputShortGuid = 0U;
    CHECK(!EncodeCompositePublicInputPayload(compositeInput, payload));
}


void TestWorldAnimatedAdapterPayload() {
    using namespace aimp::world_animated;

    DoorActionPayload door{DoorAction::ForceOpen, 0x1020304050607080ULL};
    WorldPayload payload{};
    CHECK(EncodeDoorActionPayload(door, payload));
    CHECK(payload.size == kDoorActionPayloadBytes);
    CHECK(payload.size == 16U);
    DoorActionPayload decodedDoor{};
    CHECK(DecodeDoorActionPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedDoor));
    CHECK(decodedDoor.action == door.action);
    CHECK(decodedDoor.observationId == door.observationId);
    payload.bytes[2] = std::byte{1};
    CHECK(!DecodeDoorActionPayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedDoor));

    AnimatedInputEdge edge{};
    edge.targetCategory = AnimatedTargetCategory::PlayEnvironmentAnimation;
    edge.schedulerParameterShortGuid = 0xA1B2C3D4U;
    edge.targetInputShortGuid = 0xF2FEBB21U;
    edge.observationId = 0x8877665544332211ULL;
    edge.sourceEntity = {0x0123456789ABCDEFULL, 0x1020304050607080ULL};
    edge.relativeDelaySeconds = 2.5F;
    payload = {};
    CHECK(EncodeAnimatedInputEdgePayload(edge, payload));
    CHECK(payload.size == kAnimatedInputEdgePayloadBytes);
    CHECK(payload.size == 48U);
    AnimatedInputEdge decodedEdge{};
    CHECK(DecodeAnimatedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedEdge));
    CHECK(decodedEdge.targetCategory == edge.targetCategory);
    CHECK(decodedEdge.schedulerParameterShortGuid == edge.schedulerParameterShortGuid);
    CHECK(decodedEdge.targetInputShortGuid == edge.targetInputShortGuid);
    CHECK(decodedEdge.observationId == edge.observationId);
    CHECK(decodedEdge.sourceEntity == edge.sourceEntity);
    CHECK(std::fabs(decodedEdge.relativeDelaySeconds - edge.relativeDelaySeconds) < 0.0001F);
    payload.bytes[12] = std::byte{1};
    CHECK(!DecodeAnimatedInputEdgePayload(
        std::span<const std::byte>(payload.bytes.data(), payload.size), decodedEdge));
}

void TestProfile(const std::wstring& aiPath) {
    const auto& profile = aimp::Retail2015Profile();
    CHECK(profile.version == 16);
    CHECK(profile.sections.size() == 5);
    CHECK(profile.symbols.size() == 108);
    const auto* returnToFrontend =
        profile.Find(aimp::SymbolId::GameplayReturnToFrontend);
    CHECK(returnToFrontend != nullptr);
    if (returnToFrontend != nullptr) {
        CHECK(returnToFrontend->rva == 0x00393880U);
        CHECK(returnToFrontend->expected.IsValid());
    }
    const auto* characterDirectMove =
        profile.Find(aimp::SymbolId::CharacterDirectMove);
    CHECK(characterDirectMove != nullptr);
    if (characterDirectMove != nullptr) {
        CHECK(characterDirectMove->rva == 0x0028D6D0U);
        CHECK(characterDirectMove->expected.IsValid());
    }
    const auto* cathodeBuildInputTarget =
        profile.Find(aimp::SymbolId::CathodeBuildInputTarget);
    CHECK(cathodeBuildInputTarget != nullptr);
    if (cathodeBuildInputTarget != nullptr) {
        CHECK(cathodeBuildInputTarget->rva == 0x00521F70U);
        CHECK(cathodeBuildInputTarget->expected.IsValid());
        CHECK(cathodeBuildInputTarget->approvedPatchLength == 6U);
        CHECK(cathodeBuildInputTarget->trampolineRelocationSafe);
    }
    const auto* cathodeQueue = profile.Find(aimp::SymbolId::CathodeQueueInput);
    CHECK(cathodeQueue != nullptr);
    if (cathodeQueue != nullptr) {
        CHECK(cathodeQueue->approvedPatchLength == 5U);
        CHECK(cathodeQueue->trampolineRelocationSafe);
    }
    const auto* cathodeTypedQueue = profile.Find(aimp::SymbolId::CathodeQueueTypedMessage);
    CHECK(cathodeTypedQueue != nullptr);
    if (cathodeTypedQueue != nullptr) {
        CHECK(cathodeTypedQueue->rva == 0x00530640U);
        CHECK(cathodeTypedQueue->approvedPatchLength == 5U);
        CHECK(cathodeTypedQueue->trampolineRelocationSafe);
    }
    const auto* cathodeBuildCallback = profile.Find(aimp::SymbolId::CathodeBuildCallbackValue);
    CHECK(cathodeBuildCallback != nullptr);
    if (cathodeBuildCallback != nullptr) {
        CHECK(cathodeBuildCallback->rva == 0x00526FF0U);
        CHECK(cathodeBuildCallback->expected.IsValid());
        CHECK(cathodeBuildCallback->approvedPatchLength == 5U);
        CHECK(cathodeBuildCallback->trampolineRelocationSafe);
    }
    const auto* logicSwitchConstructor =
        profile.Find(aimp::SymbolId::CathodeLogicSwitchConstructor);
    CHECK(logicSwitchConstructor != nullptr);
    if (logicSwitchConstructor != nullptr) {
        CHECK(logicSwitchConstructor->rva == 0x001B4BB0U);
        CHECK(logicSwitchConstructor->approvedPatchLength == 7U);
        CHECK(logicSwitchConstructor->trampolineRelocationSafe);
    }
    CHECK(profile.Find(aimp::SymbolId::CathodeInteractionVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeWorldPickupVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeRewireSystemVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeRewireLocationVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeRewireAccessPointVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeRewireTotalPowerVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeRewireVtable) != nullptr);
    const auto* interactionVtable = profile.Find(aimp::SymbolId::CathodeInteractionVtable);
    CHECK(interactionVtable != nullptr && interactionVtable->rva == 0x00E4268CU);
    const auto* pickupVtable = profile.Find(aimp::SymbolId::CathodeWorldPickupVtable);
    CHECK(pickupVtable != nullptr && pickupVtable->rva == 0x00E4263CU);
    CHECK(profile.Find(aimp::SymbolId::CathodePlayEnvironmentAnimationVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeAnimatedModelAttachmentNodeVtable) != nullptr);
    CHECK(profile.Find(aimp::SymbolId::CathodeDoorNativeVtable) != nullptr);
    const auto* startedOpening = profile.Find(aimp::SymbolId::CathodeDoorStartedOpening);
    CHECK(startedOpening != nullptr && startedOpening->approvedPatchLength == 6U &&
        startedOpening->trampolineRelocationSafe);
    const auto* forceClose = profile.Find(aimp::SymbolId::CathodeDoorForceClose);
    CHECK(forceClose != nullptr && forceClose->approvedPatchLength == 7U &&
        forceClose->trampolineRelocationSafe);
    for (const auto& symbol : profile.symbols) {
        CHECK(symbol.expected.bytes.empty() || symbol.expected.IsValid());
    }

    const auto validation = aimp::ValidateExecutable(aiPath, profile);
    CHECK(validation.fileRead);
    CHECK(validation.peValid);
    CHECK(validation.hashAvailable);
    CHECK(validation.hashMatch);
    CHECK(validation.metadataMatch);
    CHECK(validation.sectionLayoutMatch);
    CHECK(validation.protectedSectionsMatch);
    CHECK(validation.normalizedHashMatch);
    CHECK(validation.profileCompatible);
    CHECK(validation.recognized);
    CHECK(validation.validCandidate);
    CHECK(aimp::IsFixedRvaEligible(validation));
    CHECK(!validation.actual.canonicalPath.empty());
    CHECK(validation.actual.fileSize == profile.fileSize);
    CHECK(validation.actual.sectionCount ==
          validation.actual.sections.size());
    CHECK(!validation.actual.sections.empty());
    CHECK(std::any_of(
        validation.actual.sections.begin(),
        validation.actual.sections.end(),
        [](const aimp::PeSectionIdentity& section) {
            return section.name == ".text" &&
                (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }));
    CHECK(std::find(
              validation.actual.imports.begin(),
              validation.actual.imports.end(),
              "kernel32.dll") != validation.actual.imports.end());
    CHECK((validation.actual.fileCharacteristics &
           IMAGE_FILE_DLL) == 0);

    const auto renamedPath = L"AIMP.NotAI." +
        std::to_wstring(GetCurrentProcessId()) + L".exe";
    DeleteFileW(renamedPath.c_str());
    const bool renamedCreated =
        CreateHardLinkW(renamedPath.c_str(), aiPath.c_str(), nullptr) != FALSE ||
        CopyFileW(aiPath.c_str(), renamedPath.c_str(), FALSE) != FALSE;
    CHECK(renamedCreated);
    if (GetFileAttributesW(renamedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto renamedValidation =
            aimp::ValidateExecutable(renamedPath, profile);
        CHECK(renamedValidation.hashAvailable);
        CHECK(renamedValidation.hashMatch);
        CHECK(renamedValidation.recognized);
        CHECK(!renamedValidation.validCandidate);
        CHECK(!aimp::IsFixedRvaEligible(renamedValidation));
        CHECK(DeleteFileW(renamedPath.c_str()) != FALSE);
    }

    auto wrongHash = profile;
    wrongHash.sha256[0] ^= 0xFFU;
    const auto negativeHash = aimp::ValidateExecutable(aiPath, wrongHash);
    CHECK(negativeHash.fileRead);
    CHECK(negativeHash.peValid);
    CHECK(negativeHash.hashAvailable);
    CHECK(!negativeHash.hashMatch);
    CHECK(!negativeHash.recognized);
    CHECK(negativeHash.profileCompatible);
    CHECK(negativeHash.validCandidate);

    auto hashIndependentPolicy = validation;
    hashIndependentPolicy.hashAvailable = false;
    hashIndependentPolicy.hashMatch = false;
    hashIndependentPolicy.protectedSectionsMatch = false;
    hashIndependentPolicy.normalizedHashMatch = false;
    hashIndependentPolicy.profileCompatible = false;
    hashIndependentPolicy.recognized = false;
    CHECK(aimp::IsFixedRvaEligible(hashIndependentPolicy));
    hashIndependentPolicy.sectionLayoutMatch = false;
    CHECK(!aimp::IsFixedRvaEligible(hashIndependentPolicy));
    hashIndependentPolicy = validation;
    hashIndependentPolicy.validCandidate = false;
    CHECK(!aimp::IsFixedRvaEligible(hashIndependentPolicy));

    const auto fileBytes = ReadFileBytes(aiPath);
    const auto rdata = std::find_if(
        profile.sections.begin(),
        profile.sections.end(),
        [](const aimp::ProfileSectionIdentity& section) {
            return section.name == ".rdata";
        });
    const auto text = std::find_if(
        profile.sections.begin(),
        profile.sections.end(),
        [](const aimp::ProfileSectionIdentity& section) {
            return section.name == ".text";
        });
    const auto data = std::find_if(
        profile.sections.begin(),
        profile.sections.end(),
        [](const aimp::ProfileSectionIdentity& section) {
            return section.name == ".data";
        });
    CHECK(rdata != profile.sections.end());
    CHECK(text != profile.sections.end());
    CHECK(data != profile.sections.end());
    if (rdata != profile.sections.end() &&
        text != profile.sections.end() &&
        data != profile.sections.end() &&
        !rdata->normalizedRanges.empty()) {
        const auto range = rdata->normalizedRanges.front();
        auto launchSlotChanged = fileBytes;
        constexpr std::size_t kProductionPrefixBytes = 11;
        launchSlotChanged[
            rdata->rawOffset + range.offset + kProductionPrefixBytes] =
            std::byte{'D'};
        CHECK(aimp::IsProfileCompatible(
            launchSlotChanged,
            validation.actual,
            profile));

        auto prefixChanged = fileBytes;
        prefixChanged[rdata->rawOffset + range.offset] = std::byte{'X'};
        CHECK(!aimp::IsProfileCompatible(
            prefixChanged,
            validation.actual,
            profile));

        auto unterminatedSlot = fileBytes;
        std::fill_n(
            unterminatedSlot.begin() +
                rdata->rawOffset + range.offset,
            range.size,
            std::byte{'A'});
        constexpr std::string_view kProductionPrefix = "PRODUCTION/";
        std::transform(
            kProductionPrefix.begin(),
            kProductionPrefix.end(),
            unterminatedSlot.begin() +
                rdata->rawOffset + range.offset,
            [](const char value) {
                return static_cast<std::byte>(value);
            });
        CHECK(!aimp::IsProfileCompatible(
            unterminatedSlot,
            validation.actual,
            profile));

        auto invalidPathCharacter = fileBytes;
        invalidPathCharacter[
            rdata->rawOffset + range.offset + kProductionPrefixBytes] =
            std::byte{'?'};
        CHECK(!aimp::IsProfileCompatible(
            invalidPathCharacter,
            validation.actual,
            profile));

        const auto slotBegin = fileBytes.begin() +
            rdata->rawOffset + range.offset;
        const auto firstTerminator = std::find(
            slotBegin + kProductionPrefixBytes,
            slotBegin + range.size,
            std::byte{0});
        CHECK(firstTerminator != slotBegin + range.size);
        if (firstTerminator != slotBegin + range.size &&
            firstTerminator + 1 != slotBegin + range.size) {
            auto invalidPostTerminator = fileBytes;
            const auto invalidOffset = static_cast<std::size_t>(
                firstTerminator - fileBytes.begin() + 1);
            invalidPostTerminator[invalidOffset] = std::byte{0xFF};
            CHECK(!aimp::IsProfileCompatible(
                invalidPostTerminator,
                validation.actual,
                profile));
        }

        auto rdataOutsideSlotChanged = fileBytes;
        rdataOutsideSlotChanged[
            rdata->rawOffset + range.offset - 1] ^= std::byte{0x01};
        CHECK(!aimp::IsProfileCompatible(
            rdataOutsideSlotChanged,
            validation.actual,
            profile));

        auto textChanged = fileBytes;
        textChanged[text->rawOffset] ^= std::byte{0x01};
        CHECK(!aimp::IsProfileCompatible(
            textChanged,
            validation.actual,
            profile));

        auto dataChanged = fileBytes;
        dataChanged[data->rawOffset] ^= std::byte{0x01};
        CHECK(!aimp::IsProfileCompatible(
            dataChanged,
            validation.actual,
            profile));

        auto metadataChanged = validation.actual;
        ++metadataChanged.timeDateStamp;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            metadataChanged,
            profile));

        auto characteristicsChanged = validation.actual;
        characteristicsChanged.fileCharacteristics ^= 0x0002U;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            characteristicsChanged,
            profile));

        auto sectionCountChanged = validation.actual;
        ++sectionCountChanged.sectionCount;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            sectionCountChanged,
            profile));

        auto imageBaseChanged = validation.actual;
        imageBaseChanged.preferredImageBase += 0x10000U;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            imageBaseChanged,
            profile));

        auto entryPointChanged = validation.actual;
        ++entryPointChanged.entryPoint;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            entryPointChanged,
            profile));

        auto sectionLayoutChanged = validation.actual;
        ++sectionLayoutChanged.sections.front().virtualAddress;
        CHECK(!aimp::IsProfileCompatible(
            fileBytes,
            sectionLayoutChanged,
            profile));

        auto headerGapChanged = fileBytes;
        headerGapChanged[0x200] ^= std::byte{0x01};
        CHECK(!aimp::IsProfileCompatible(
            headerGapChanged,
            validation.actual,
            profile));
    }

    auto image = BuildVirtualImage(aiPath);
    CHECK(image.size() == profile.sizeOfImage);
    if (image.size() == profile.sizeOfImage) {
        aimp::SignatureResolver resolver(
            image.data(),
            image.size(),
            profile,
            true);
        CHECK(resolver.ValidateAllCodeSymbols());
        CHECK(resolver.Resolve(aimp::SymbolId::NetzDispatch) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ReceiveSpawnRequestF0) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ReceiveSpawnRequest130) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterRuntimeIdCounter) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ReplicationContextRelease) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::MainThreadCharacterProcessing) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterManagerUpdate) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterReadinessProducer) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterReadinessPublisher) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterReadinessBarrier) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ParticipantRecordConstruct) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ParticipantRecordCopy) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ParticipantAppend) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::ParticipantDetach) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterDestroyFull) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::UiRegistryRoot) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::UiFindNamedElement) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::PopupMessageEnqueue) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterDirectMove) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeDispatchInput) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeBuildInputTarget) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeQueueInput) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeReleaseInputTarget) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeInteractionVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeWorldPickupVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeRewireSystemVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeRewireLocationVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeRewireAccessPointVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeRewireTotalPowerVtable) != nullptr);
        CHECK(resolver.Resolve(aimp::SymbolId::CathodeRewireVtable) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeEntityManagerRoot) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeResolveEntity) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeExtractRuntimeKey) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeAssignEntityRef) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CathodeProviderRegistryRoot) != nullptr);

        auto genericProfile = profile;
        // Generic discovery must read the loaded PE section table rather than
        // reusing these historical range coordinates.
        genericProfile.textRva = 0;
        genericProfile.textSize = 1;
        aimp::SignatureResolver generic(
            image.data(),
            image.size(),
            genericProfile,
            false);
        const auto genericF0 = generic.ResolveWithEvidence(
            aimp::SymbolId::ReceiveSpawnRequestF0);
        CHECK(genericF0.kind ==
              aimp::ResolutionKind::GenericSignature);
        CHECK(genericF0.address != nullptr);
        CHECK(genericF0.sectionName == ".text");
        CHECK(genericF0.matchCount == 1);
        CHECK(genericF0.fixedByteCount >= 8);
        CHECK(generic.Resolve(
                  aimp::SymbolId::GlobalStateRoot) == nullptr);

        constexpr std::array essentialHooks{
            aimp::SymbolId::MainThreadCharacterProcessing,
            aimp::SymbolId::CharacterManagerUpdate,
            aimp::SymbolId::CharacterReadinessProducer,
        };
        for (const auto symbol : essentialHooks) {
            const auto evidence = generic.ResolveWithEvidence(symbol);
            const auto* spec = profile.Find(symbol);
            CHECK(spec != nullptr);
            CHECK(evidence.kind == aimp::ResolutionKind::GenericSignature);
            CHECK(evidence.matchCount == 1);
            CHECK(evidence.address == image.data() + spec->rva);
        }

        const auto* producer = profile.Find(
            aimp::SymbolId::CharacterReadinessProducer);
        CHECK(producer != nullptr);
        if (producer != nullptr) {
            auto ambiguousImage = image;
            const auto duplicateRva = profile.textRva +
                profile.textSize - producer->expected.bytes.size() - 64U;
            std::transform(
                producer->expected.bytes.begin(),
                producer->expected.bytes.end(),
                ambiguousImage.begin() + duplicateRva,
                [](const std::uint8_t value) {
                    return static_cast<std::byte>(value);
                });
            aimp::SignatureResolver ambiguousResolver(
                ambiguousImage.data(),
                ambiguousImage.size(),
                profile,
                false);
            const auto ambiguousProducer =
                ambiguousResolver.ResolveWithEvidence(
                    aimp::SymbolId::CharacterReadinessProducer);
            CHECK(ambiguousProducer.kind ==
                  aimp::ResolutionKind::Ambiguous);
            CHECK(ambiguousProducer.matchCount == 2);
            CHECK(ambiguousProducer.address == nullptr);
        }

        const auto* dispatch = profile.Find(aimp::SymbolId::NetzDispatch);
        CHECK(dispatch != nullptr);
        if (dispatch != nullptr) {
            image[dispatch->rva] ^= std::byte{0x01};
            aimp::SignatureResolver negative(
                image.data(),
                image.size(),
                profile,
                true);
            CHECK(negative.Resolve(aimp::SymbolId::NetzDispatch) == nullptr);
            CHECK(!negative.ValidateAllCodeSymbols());
            aimp::SignatureResolver genericAfterUnrelatedChange(
                image.data(),
                image.size(),
                profile,
                false);
            for (const auto symbol : essentialHooks) {
                CHECK(genericAfterUnrelatedChange.Resolve(symbol) != nullptr);
            }
        }
    }
}

void TestCompatibleProfile(const std::wstring& aiPath) {
    const auto& profile = aimp::Retail2015Profile();
    const auto validation = aimp::ValidateExecutable(aiPath, profile);
    CHECK(validation.fileRead);
    CHECK(validation.peValid);
    CHECK(validation.hashAvailable);
    CHECK(!validation.hashMatch);
    CHECK(validation.metadataMatch);
    CHECK(validation.sectionLayoutMatch);
    CHECK(validation.protectedSectionsMatch);
    CHECK(validation.normalizedHashMatch);
    CHECK(validation.profileCompatible);
    CHECK(!validation.recognized);
    CHECK(validation.validCandidate);
    CHECK(aimp::IsFixedRvaEligible(validation));
    CHECK(validation.reason ==
          "local_profile_compatible_launch_slot_change");

    auto image = BuildVirtualImage(aiPath);
    CHECK(image.size() == profile.sizeOfImage);
    if (image.size() == profile.sizeOfImage) {
        aimp::SignatureResolver resolver(
            image.data(),
            image.size(),
            profile,
            aimp::IsFixedRvaEligible(validation));
        CHECK(resolver.ValidateAllCodeSymbols());
        CHECK(resolver.Resolve(
                  aimp::SymbolId::MainThreadCharacterProcessing) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterManagerUpdate) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterReadinessProducer) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::UiRegistryRoot) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::UiFindNamedElement) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::PopupMessageEnqueue) != nullptr);
        CHECK(resolver.Resolve(
                  aimp::SymbolId::CharacterDirectMove) != nullptr);
    }
}

void TestDllFailClosed(const std::wstring& dllPath) {
    HMODULE module = LoadLibraryW(dllPath.c_str());
    CHECK(module != nullptr);
    if (module == nullptr) {
        return;
    }

    using GetState = std::uint32_t(WINAPI*)();
    using RequestShutdown = void(WINAPI*)();
    using ProtocolVersion = std::uint16_t(WINAPI*)();
    using DllVersion = const char*(WINAPI*)();
    const auto getState =
        reinterpret_cast<GetState>(GetProcAddress(module, "AimpGetState"));
    const auto requestShutdown =
        reinterpret_cast<RequestShutdown>(
            GetProcAddress(module, "AimpRequestShutdown"));
    const auto protocolVersion =
        reinterpret_cast<ProtocolVersion>(
            GetProcAddress(module, "AimpProtocolVersion"));
    const auto dllVersion =
        reinterpret_cast<DllVersion>(
            GetProcAddress(module, "AimpDllVersion"));
    CHECK(getState != nullptr);
    CHECK(requestShutdown != nullptr);
    CHECK(protocolVersion != nullptr);
    CHECK(dllVersion != nullptr);
    if (getState == nullptr ||
        requestShutdown == nullptr ||
        protocolVersion == nullptr ||
        dllVersion == nullptr) {
        FreeLibrary(module);
        return;
    }

    CHECK(protocolVersion() == aimp::kProtocolVersion);
    CHECK(std::string(dllVersion()) == aimp::kNativeVersion);
    const auto rejectDeadline = GetTickCount64() + 5000;
    while (getState() != 2 && GetTickCount64() < rejectDeadline) {
        Sleep(10);
    }
    CHECK(getState() == 2);

    requestShutdown();
    const auto stopDeadline = GetTickCount64() + 5000;
    while (getState() != 5 && GetTickCount64() < stopDeadline) {
        Sleep(10);
    }
    CHECK(getState() == 5);
    CHECK(FreeLibrary(module) != FALSE);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    TestCapabilities();
    TestIpcDisconnectEpochReset();
    TestCaptureCatalog();
    TestLevelGeneration();
    TestSpawnCharacterTracker();
    TestLifecycleCatalog();
    TestDispatcher();
    TestGameThreadRuntimeEvidence();
    TestReadyReplayGate();
    TestDummyController();
    TestIpcDummyCommandsUseValidatedDispatcher();
    TestIpcGameCommandQueuesBeforeEngineBinding();
    TestHistoricalParticipantTransaction();
    TestNativeLimits();
    TestExperimentalProtocol();
    TestCharacterDirectStepNativeLifecycle();
    TestProtocol();
    TestWorldReplicationCodec();
    TestCathodePrimitivePayload();
    TestWorldInteractionAdapterPayload();
    TestWorldAnimatedAdapterPayload();
    TestPeerControlValidation();
    TestQueue();
    TestSignatureSearch();
    TestHookManager();
    if (argc >= 2) {
        if (argv[1][0] != L'\0') {
            TestProfile(argv[1]);
        }
    } else {
        std::cout << "AI.exe profile test skipped: no path supplied.\n";
    }
    if (argc >= 3) {
        TestDllFailClosed(argv[2]);
    }
    if (argc >= 4) {
        TestCompatibleProfile(argv[3]);
    }

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All AIMP.Native tests passed (" << checks
              << " checks).\n";
    return 0;
}
