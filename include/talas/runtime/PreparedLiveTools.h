#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/TypedEvent.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
}

namespace talas::runtime
{
class RealtimeAudioEngine;
struct PreparedLiveToolsResult;

struct PreparedLiveToolVariable
{
    std::string variableId;
    talas::project::ProjectLiveToolVariableKind kind =
        talas::project::ProjectLiveToolVariableKind::Number;
    double initialNumber = 0.0;
    bool initialBoolean = false;
    std::optional<std::uint8_t> stateScopeIndex;
};

struct PreparedLiveToolCommand
{
    talas::project::ProjectLiveToolCommandKind kind =
        talas::project::ProjectLiveToolCommandKind::Stop;
    std::uint16_t preparedCellIndex = 0;
    std::string variableId;
    std::string eventName;
    talas::expr::ValueDomain eventDomain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    std::optional<TypedEventTarget> typedTarget;
};

struct PreparedLiveToolTransition
{
    std::string transitionId;
    std::uint8_t fromStateIndex = 0;
    std::uint8_t toStateIndex = 0;
    talas::project::ProjectLiveToolTrigger trigger;
    std::optional<talas::expr::Expression> guard;
    std::vector<PreparedLiveToolCommand> commands;
};

struct PreparedLiveTool
{
    std::string toolId;
    std::vector<std::string> stateIds;
    std::uint8_t initialStateIndex = 0;
    std::vector<PreparedLiveToolVariable> variables;
    std::vector<PreparedLiveToolTransition> transitions;
};

class PreparedLiveTools
{
public:
    [[nodiscard]] const PreparedLiveSetBinding& binding() const noexcept;
    [[nodiscard]] std::span<const PreparedLiveTool> tools() const noexcept;
    [[nodiscard]] std::optional<std::size_t> findTool(
        std::string_view toolId) const noexcept;

private:
    PreparedLiveSetBinding binding_;
    std::vector<PreparedLiveTool> tools_;

    friend struct PreparedLiveToolsResult;
    friend PreparedLiveToolsResult prepareProjectLiveTools(
        const talas::project::ProjectDocument&,
        const PreparedLiveSet&);
    friend PreparedLiveToolsResult bindPreparedLiveToolTargets(
        const PreparedLiveTools&,
        const RealtimeAudioEngine&);
};

struct PreparedLiveToolsResult
{
    std::shared_ptr<const PreparedLiveTools> prepared;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return prepared != nullptr && error.empty();
    }
};

[[nodiscard]] PreparedLiveToolsResult prepareProjectLiveTools(
    const talas::project::ProjectDocument& document,
    const PreparedLiveSet& liveSet);

// Off-audio binding pass. Replaces string-only scalar destinations with exact
// numeric handles owned by the accepted realtime engine and its stage.
[[nodiscard]] PreparedLiveToolsResult bindPreparedLiveToolTargets(
    const PreparedLiveTools& prepared,
    const RealtimeAudioEngine& engine);

struct PreparedLiveToolRuntimeVariable
{
    double number = 0.0;
    bool boolean = false;

    bool operator==(const PreparedLiveToolRuntimeVariable&) const = default;
};

struct PreparedLiveToolRuntimeState
{
    std::array<std::uint8_t, talas::project::maximumProjectLiveTools>
        stateIndices{};
    std::array<std::uint16_t, talas::project::maximumProjectLiveTools>
        variableOffsets{};
    std::array<std::uint8_t, talas::project::maximumProjectLiveTools>
        variableCounts{};
    std::array<std::uint64_t, talas::project::maximumProjectLiveTools>
        lastTriggerSequences{};
    std::array<PreparedLiveToolRuntimeVariable,
               talas::project::maximumPreparedLiveToolVariables>
        variables{};
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
    std::size_t toolCount = 0;
    std::size_t variableCount = 0;

    bool operator==(const PreparedLiveToolRuntimeState&) const = default;
};

class PreparedLiveToolRuntimeStatePublication
{
public:
    [[nodiscard]] bool publishAll(
        const PreparedLiveToolRuntimeState& state) noexcept;
    [[nodiscard]] bool publishTool(
        const PreparedLiveToolRuntimeState& state,
        std::size_t toolIndex) noexcept;
    [[nodiscard]] bool read(
        PreparedLiveToolRuntimeState& state) const noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] bool beginWrite(std::uint64_t& generation) noexcept;
    void endWrite(std::uint64_t generation) noexcept;

    std::atomic<std::uint64_t> publicationGeneration_ = 0;
    std::atomic<std::uint64_t> documentAuthorityGeneration_ = 0;
    std::atomic<std::uint64_t> stageGeneration_ = 0;
    std::atomic<std::uint64_t> stageId_ = 0;
    std::atomic<std::uint64_t> toolCount_ = 0;
    std::atomic<std::uint64_t> variableCount_ = 0;
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumProjectLiveTools>
        stateIndices_{};
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumProjectLiveTools>
        variableOffsets_{};
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumProjectLiveTools>
        variableCounts_{};
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumProjectLiveTools>
        lastTriggerSequences_{};
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumPreparedLiveToolVariables>
        variableNumbers_{};
    std::array<std::atomic<std::uint64_t>,
               talas::project::maximumPreparedLiveToolVariables>
        variableBooleans_{};
};

[[nodiscard]] bool initializePreparedLiveToolRuntimeState(
    const PreparedLiveTools& prepared,
    PreparedLiveToolRuntimeState& state) noexcept;

enum class PreparedLiveToolStepOutcome : std::uint8_t
{
    NoMatchingTransition,
    Applied,
    StaleAuthority,
    InvalidTrigger,
    GuardRejected,
    CommandRejected,
};

struct PreparedLiveToolTriggerEvent
{
    talas::project::ProjectLiveToolTriggerKind kind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    std::string_view eventName;
    talas::expr::ValueDomain eventDomain = talas::expr::ValueDomain::Scalar;
    std::string_view variableId;
    double variableNumber = 0.0;
    bool variableBoolean = false;
    bool hasVariableValue = false;
    std::string_view midiSourceIdentifier;
    std::int32_t midiChannel = 1;
    std::int32_t midiProgram = 0;
    std::string_view oscAddress;
    std::uint64_t musicalTick = 0;
    std::uint64_t sequence = 0;
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint64_t stageId = 0;
};

struct PreparedLiveToolEvaluationInputs
{
    void* context = nullptr;
    talas::expr::ResolveValueFn resolveEvent = nullptr;
    double sampleRate = 48'000.0;
    double tempoBpm = 120.0;
};

using ApplyPreparedLiveToolCommands = bool (*)(
    void*,
    std::span<const PreparedLiveToolCommand>) noexcept;

struct PreparedLiveToolCommandSink
{
    void* context = nullptr;
    ApplyPreparedLiveToolCommands apply = nullptr;
};

struct PreparedLiveToolStepResult
{
    PreparedLiveToolStepOutcome outcome =
        PreparedLiveToolStepOutcome::InvalidTrigger;
    std::uint8_t stateIndex = 0;
    std::uint8_t commandCount = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome == PreparedLiveToolStepOutcome::Applied;
    }
};

[[nodiscard]] PreparedLiveToolStepResult stepPreparedLiveTool(
    const PreparedLiveTools& prepared,
    std::size_t toolIndex,
    const PreparedLiveToolTriggerEvent& trigger,
    const PreparedLiveToolEvaluationInputs& inputs,
    const PreparedLiveToolCommandSink& sink,
    PreparedLiveToolRuntimeState& state) noexcept;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}
