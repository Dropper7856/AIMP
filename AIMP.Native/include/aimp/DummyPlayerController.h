#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aimp {

enum class DummyState : std::uint8_t {
    Absent,
    Creating,
    Active,
    Removing,
    Failed,
};

enum class DummyAchievement : std::uint8_t {
    None,
    A,
    B,
    C,
    D,
};

enum class DummyNativeStep : std::uint8_t {
    Participant,
    Channel,
    Descriptors,
    Character,
    ReplicationContext,
    Binding,
};

struct DummyRequestIdentity {
    std::string portableDummyId;
    std::uint64_t peerId{};
    std::string launcherPlayerId;
    std::string portableBindingId;
    std::string displayName;
};

struct DummySpawnOffset {
    float x{};
    float y{};
    float z{};
};

struct DummyOperationResult {
    DummyState state{DummyState::Absent};
    DummyAchievement achieved{DummyAchievement::None};
    std::string statusCode;
    std::string message;
    bool rollbackPerformed{};
    std::vector<std::string> completedSteps;
    std::vector<std::string> blockedGates;
};

class IDummyNativeExecutor {
public:
    virtual ~IDummyNativeExecutor() = default;
    // Called on the validated game thread before the first acquisition.
    // Implementations may copy/validate the request context here, but must
    // not acquire native resources; rollback starts with Acquire().
    virtual bool BeginCreate(
        const DummyRequestIdentity& identity,
        std::uint64_t generation,
        const DummySpawnOffset& spawnOffset,
        std::string& error) = 0;
    virtual bool Acquire(
        DummyNativeStep step,
        std::string& error) = 0;
    virtual bool Release(
        DummyNativeStep step,
        std::string& error) noexcept = 0;
    virtual bool Verify(std::string& error) = 0;
    virtual bool Move(
        std::string_view command,
        float amount,
        std::uint32_t durationMilliseconds,
        std::string& method,
        std::string& error) = 0;
};

class DummyPlayerController final {
public:
    DummyOperationResult Create(
        const DummyRequestIdentity& identity,
        std::uint64_t generation,
        const DummySpawnOffset& spawnOffset,
        IDummyNativeExecutor& executor);
    DummyOperationResult Create(
        const DummyRequestIdentity& identity,
        std::uint64_t generation,
        IDummyNativeExecutor& executor);
    DummyOperationResult Remove(
        std::uint64_t generation,
        IDummyNativeExecutor& executor);
    DummyOperationResult Move(
        std::uint64_t generation,
        std::string_view command,
        float amount,
        std::uint32_t durationMilliseconds,
        IDummyNativeExecutor& executor);
    void InvalidateGeneration(std::uint64_t newGeneration) noexcept;

    [[nodiscard]] DummyState State() const noexcept;
    [[nodiscard]] DummyAchievement Achievement() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::string PortableDummyId() const;

private:
    DummyOperationResult RollbackLocked(
        IDummyNativeExecutor& executor,
        std::string_view failure,
        std::vector<DummyNativeStep>& acquired);

    mutable std::mutex mutex_;
    DummyState state_{DummyState::Absent};
    DummyAchievement achievement_{DummyAchievement::None};
    std::uint64_t generation_{};
    DummyRequestIdentity identity_{};
    std::vector<DummyNativeStep> acquired_;
};

class BlockedDummyExecutor final : public IDummyNativeExecutor {
public:
    bool BeginCreate(
        const DummyRequestIdentity& identity,
        std::uint64_t generation,
        const DummySpawnOffset& spawnOffset,
        std::string& error) override;
    bool Acquire(DummyNativeStep step, std::string& error) override;
    bool Release(
        DummyNativeStep step,
        std::string& error) noexcept override;
    bool Verify(std::string& error) override;
    bool Move(
        std::string_view command,
        float amount,
        std::uint32_t durationMilliseconds,
        std::string& method,
        std::string& error) override;
};

[[nodiscard]] std::string_view ToString(DummyState state) noexcept;
[[nodiscard]] std::string_view ToString(DummyAchievement level) noexcept;
[[nodiscard]] std::string_view ToString(DummyNativeStep step) noexcept;

} // namespace aimp
