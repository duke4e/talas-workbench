#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace talas::app
{
enum class WorkbenchOperationKind : std::uint8_t
{
    None,
    CacheFreeze,
    AudioExport,
    ProjectPackage,
    PartialTrackDerivative,
    DiskCacheCapture,
};

enum class WorkbenchOperationPhase : std::uint8_t
{
    Idle,
    Starting,
    Running,
    Cancelling,
    Publishing,
    Finishing,
    Succeeded,
    Cancelled,
    Failed,
    Invalidated,
};

enum class WorkbenchOperationOutcome : std::uint8_t
{
    Succeeded,
    Cancelled,
    Failed,
};

struct WorkbenchOperationToken
{
    WorkbenchOperationKind kind = WorkbenchOperationKind::None;
    std::uint64_t generation = 0;
    std::uint64_t authorityGeneration = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return kind != WorkbenchOperationKind::None
            && generation != 0 && authorityGeneration != 0;
    }
};

struct WorkbenchOperationSnapshot
{
    WorkbenchOperationKind kind = WorkbenchOperationKind::None;
    WorkbenchOperationPhase phase = WorkbenchOperationPhase::Idle;
    std::uint64_t generation = 0;
    std::uint64_t sourceAuthorityGeneration = 0;
    std::uint64_t currentAuthorityGeneration = 0;
    std::uint64_t lastSuccessfulGeneration = 0;
    std::uint16_t progressPermille = 0;

    [[nodiscard]] bool active() const noexcept
    {
        return phase == WorkbenchOperationPhase::Starting
            || phase == WorkbenchOperationPhase::Running
            || phase == WorkbenchOperationPhase::Cancelling
            || phase == WorkbenchOperationPhase::Publishing
            || phase == WorkbenchOperationPhase::Finishing;
    }
};

// One fixed-state coordinator for off-audio Workbench operations. Publication
// and authority invalidation compete on the same packed phase word: exactly one
// wins, so a worker either owns terminal publication or must cancel without
// installing its private artifact.
class WorkbenchOperationCoordinator
{
public:
    WorkbenchOperationCoordinator() = default;

    [[nodiscard]] std::optional<WorkbenchOperationToken> begin(
        WorkbenchOperationKind kind) noexcept;
    [[nodiscard]] bool publishProgress(
        WorkbenchOperationToken token,
        double progress) noexcept;
    [[nodiscard]] bool requestCancel(
        WorkbenchOperationToken token) noexcept;
    [[nodiscard]] bool shouldCancel(
        WorkbenchOperationToken token) const noexcept;
    [[nodiscard]] bool admitPublication(
        WorkbenchOperationToken token) noexcept;
    [[nodiscard]] bool invalidatePublication(
        WorkbenchOperationToken token) noexcept;
    [[nodiscard]] bool finish(
        WorkbenchOperationToken token,
        WorkbenchOperationOutcome outcome) noexcept;
    void advanceAuthority() noexcept;

    [[nodiscard]] WorkbenchOperationSnapshot snapshot() const noexcept;

private:
    static constexpr std::uint64_t stateShift = 8;
    static constexpr std::uint64_t progressGenerationShift = 16;
    static constexpr std::uint64_t maximumGeneration =
        UINT64_MAX >> progressGenerationShift;

    [[nodiscard]] static constexpr std::uint64_t state(
        std::uint64_t generation,
        WorkbenchOperationKind kind,
        WorkbenchOperationPhase phase) noexcept
    {
        return (generation << stateShift)
            | (static_cast<std::uint64_t>(kind) << 4U)
            | static_cast<std::uint64_t>(phase);
    }

    [[nodiscard]] static constexpr std::uint64_t generation(
        std::uint64_t stateValue) noexcept
    {
        return stateValue >> stateShift;
    }

    [[nodiscard]] static constexpr WorkbenchOperationKind kind(
        std::uint64_t stateValue) noexcept
    {
        return static_cast<WorkbenchOperationKind>(
            (stateValue >> 4U) & 0x0fU);
    }

    [[nodiscard]] static constexpr WorkbenchOperationPhase phase(
        std::uint64_t stateValue) noexcept
    {
        return static_cast<WorkbenchOperationPhase>(
            stateValue & 0x0fU);
    }

    [[nodiscard]] static constexpr std::uint64_t progressState(
        std::uint64_t generationValue,
        std::uint16_t permille) noexcept
    {
        return (generationValue << progressGenerationShift) | permille;
    }

    std::atomic<std::uint64_t> state_{0};
    std::atomic<std::uint64_t> progress_{0};
    std::atomic<std::uint64_t> sourceAuthorityGeneration_{0};
    std::atomic<std::uint64_t> currentAuthorityGeneration_{1};
    std::atomic<std::uint64_t> lastSuccessfulGeneration_{0};
    std::atomic<std::uint64_t> nextGeneration_{1};
};
}
