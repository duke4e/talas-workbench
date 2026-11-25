#pragma once

#include "talas/project/ProjectLiveSets.h"
#include "talas/project/ProjectMusicalTime.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/runtime/SnapshotPerformance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::runtime
{
class PreparedRuntimeGraph;
struct PreparedLiveSetResult;

struct PreparedLiveSetBinding
{
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::uint32_t sampleRate = 0;
    bool editorDocumentAuthority = false;

    bool operator==(const PreparedLiveSetBinding&) const = default;
};

struct PreparedLiveSetSoundProgram
{
    std::string soundDefinitionId;
    std::optional<std::string> sourceInstanceId;
    std::shared_ptr<const PreparedRuntimeGraph> graph;
};

struct PreparedLiveSetSnapshotProgram
{
    std::string bankId;
    std::string snapshotId;
    std::uint64_t rampFrames = 0;
    std::vector<SnapshotDirectEventValueUpdate> eventValueUpdates;
    std::vector<SnapshotDirectVariableUpdate> variableUpdates;
    std::shared_ptr<const PreparedRuntimeGraph> overlayGraph;
};

struct PreparedLiveSetScoreRegion
{
    std::string markerId;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;

    bool operator==(const PreparedLiveSetScoreRegion&) const = default;
};

struct PreparedLiveSetStopProgram
{
    bool operator==(const PreparedLiveSetStopProgram&) const = default;
};

struct PreparedLiveSetTrackMuteProgram
{
    bool muted = true;

    bool operator==(const PreparedLiveSetTrackMuteProgram&) const = default;
};

struct PreparedLiveSetFailedProgram
{
    std::string error;

    bool operator==(const PreparedLiveSetFailedProgram&) const = default;
};

using PreparedLiveSetProgram = std::variant<
    PreparedLiveSetSoundProgram,
    PreparedLiveSetSnapshotProgram,
    PreparedLiveSetScoreRegion,
    PreparedLiveSetTrackMuteProgram,
    PreparedLiveSetStopProgram,
    PreparedLiveSetFailedProgram>;

struct PreparedLiveSetCell
{
    std::string liveSetId;
    std::string cellId;
    std::uint32_t row = 0;
    std::uint32_t column = 0;
    talas::project::ProjectLiveSetQuantization quantization;
    PreparedLiveSetProgram program = PreparedLiveSetFailedProgram{};

    [[nodiscard]] bool ready() const noexcept
    {
        return !std::holds_alternative<PreparedLiveSetFailedProgram>(program);
    }
};

struct PreparedLiveSetTrackRouting
{
    std::string liveSetId;
    std::array<std::uint32_t, talas::project::projectLiveSetColumns>
        destinations{};
    std::array<std::uint8_t, talas::project::projectLiveSetColumns>
        renderOrder{};
    std::array<bool, talas::project::projectLiveSetColumns>
        hasIncoming{};
    bool hasRoutes = false;
};

struct PreparedLiveSetPreparationServices
{
    using PrepareSound = bool (*)(
        void* context,
        const talas::project::ProjectLiveSetSoundTarget& target,
        std::shared_ptr<const PreparedRuntimeGraph>& graph,
        std::string& error);
    using PrepareSnapshot = bool (*)(
        void* context,
        const talas::project::ProjectSnapshotBank& bank,
        const talas::project::ProjectSnapshot& snapshot,
        PreparedLiveSetSnapshotProgram& program,
        std::string& error);

    void* context = nullptr;
    PrepareSound prepareSound = nullptr;
    PrepareSnapshot prepareSnapshot = nullptr;
};

class PreparedLiveSet
{
public:
    [[nodiscard]] const PreparedLiveSetBinding& binding() const noexcept;
    [[nodiscard]] const talas::project::ProjectMusicalTime&
    musicalTime() const noexcept;
    [[nodiscard]] std::span<const PreparedLiveSetCell> cells() const noexcept;
    [[nodiscard]] const PreparedLiveSetTrackRouting* trackRouting(
        std::string_view liveSetId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> findCell(
        std::string_view liveSetId,
        std::string_view cellId) const noexcept;

private:
    PreparedLiveSetBinding binding_;
    talas::project::ProjectMusicalTime musicalTime_;
    std::vector<PreparedLiveSetCell> cells_;
    std::vector<PreparedLiveSetTrackRouting> trackRouting_;

    friend struct PreparedLiveSetResult;
    friend PreparedLiveSetResult prepareProjectLiveSet(
        const talas::project::ProjectDocument&,
        PreparedLiveSetBinding,
        PreparedLiveSetPreparationServices);
};

enum class PreparedLiveSetEffectKind : std::uint8_t
{
    ActivateSound,
    ActivateOverlaySnapshot,
    ApplyDirectSnapshot,
    SeekScoreRegion,
    SetTrackMute,
    Stop,
};

// Immutable non-owning view into one complete PreparedLiveSet cell. Both the
// realtime callback and offline parity harness consume this closed reducer so
// target interpretation cannot drift by execution mode.
struct PreparedLiveSetEffect
{
    PreparedLiveSetEffectKind kind = PreparedLiveSetEffectKind::Stop;
    std::uint16_t cellIndex = 0;
    const PreparedRuntimeGraph* graph = nullptr;
    std::span<const SnapshotDirectEventValueUpdate> eventValueUpdates;
    std::span<const SnapshotDirectVariableUpdate> variableUpdates;
    std::uint64_t rampFrames = 0;
    std::uint64_t scoreStartFrame = 0;
    std::uint64_t scoreEndFrame = 0;
    std::uint32_t column = 0;
    bool muted = false;
};

struct PreparedLiveSetExecutionState
{
    std::uint64_t activeRequestGeneration = 0;
    std::uint64_t transportFrame = 0;
    std::uint64_t scoreEndFrame = 0;
    std::uint32_t activeCellIndexPlusOne = 0;
    bool playing = false;
    bool scoreRegionActive = false;

    bool operator==(const PreparedLiveSetExecutionState&) const = default;
};

enum class PreparedLiveSetApplicationResult : std::uint8_t
{
    Rejected,
    Applied,
    PendingRenderConfirmation,
};

using PreparedLiveSetApplyFunction = PreparedLiveSetApplicationResult (*)(
    void*, const PreparedLiveSetEffect&) noexcept;

struct PreparedLiveSetExecutionCallbacks
{
    void* context = nullptr;
    PreparedLiveSetApplyFunction activateSound = nullptr;
    PreparedLiveSetApplyFunction activateOverlaySnapshot = nullptr;
    PreparedLiveSetApplyFunction applyDirectSnapshot = nullptr;
    PreparedLiveSetApplyFunction seekScoreRegion = nullptr;
    PreparedLiveSetApplyFunction setTrackMute = nullptr;
    PreparedLiveSetApplyFunction stop = nullptr;
};

struct PreparedLiveSetExecutionOutcome
{
    PreparedLiveSetExecutionState state;
    bool completionRequiresRenderConfirmation = false;
};

[[nodiscard]] std::optional<PreparedLiveSetEffect>
preparedLiveSetEffect(const PreparedLiveSet& prepared,
                      std::size_t cellIndex) noexcept;

[[nodiscard]] bool reducePreparedLiveSetEffect(
    const PreparedLiveSetEffect& effect,
    std::uint64_t requestGeneration,
    PreparedLiveSetExecutionState& state) noexcept;

[[nodiscard]] std::optional<PreparedLiveSetExecutionOutcome>
executePreparedLiveSetEffect(
    const PreparedLiveSetEffect& effect,
    std::uint64_t requestGeneration,
    PreparedLiveSetExecutionState initialState,
    const PreparedLiveSetExecutionCallbacks& callbacks) noexcept;

struct PreparedLiveSetResult
{
    std::shared_ptr<const PreparedLiveSet> prepared;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return prepared != nullptr && error.empty();
    }
};

[[nodiscard]] PreparedLiveSetResult prepareProjectLiveSet(
    const talas::project::ProjectDocument& document,
    PreparedLiveSetBinding binding,
    PreparedLiveSetPreparationServices services);

[[nodiscard]] std::optional<std::uint64_t>
preparedLiveSetLaunchFrame(
    const PreparedLiveSet& prepared,
    std::size_t cellIndex,
    std::uint64_t currentAbsoluteFrame) noexcept;

struct PreparedLiveSetTransportLoop
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
};

// With a Timeline loop the musical position is the looped playhead: the
// grid is resolved there and the returned absolute frame lies the same
// distance ahead. A grid point past the loop end waits for the first grid
// point of the next pass, or for the loop start when the loop has none.
[[nodiscard]] std::optional<std::uint64_t>
preparedLiveSetLaunchFrame(
    const PreparedLiveSet& prepared,
    std::size_t cellIndex,
    std::uint64_t currentAbsoluteFrame,
    std::optional<PreparedLiveSetTransportLoop> loop) noexcept;
}
