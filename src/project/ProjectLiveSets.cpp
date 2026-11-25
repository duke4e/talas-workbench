#include "talas/project/ProjectLiveSets.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectScore.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/VirtualControlOsc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
bool validUtf8(std::string_view value) noexcept
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU)
        {
            if (first < 0x20U || first == 0x7fU)
                return false;
            ++index;
            continue;
        }
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if ((first & 0xe0U) == 0xc0U)
        {
            continuationCount = 1;
            codePoint = first & 0x1fU;
        }
        else if ((first & 0xf0U) == 0xe0U)
        {
            continuationCount = 2;
            codePoint = first & 0x0fU;
        }
        else if ((first & 0xf8U) == 0xf0U)
        {
            continuationCount = 3;
            codePoint = first & 0x07U;
        }
        else
        {
            return false;
        }
        if (index + continuationCount >= value.size())
            return false;
        for (std::size_t offset = 1; offset <= continuationCount; ++offset)
        {
            const auto byte =
                static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        const auto minimum = continuationCount == 1 ? 0x80U
            : continuationCount == 2 ? 0x800U : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10ffffU
            || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
        {
            return false;
        }
        index += continuationCount + 1U;
    }
    return true;
}

bool validDisplayName(std::string_view value) noexcept
{
    return !value.empty()
        && value.size() <= maximumProjectLiveSetDisplayNameBytes
        && validUtf8(value);
}

void addDiagnostic(
    ProjectLiveSetValidationResult& result,
    ProjectLiveSetValidationCode code,
    std::string path,
    std::string message)
{
    if (result.diagnostics.size() >= maximumProjectLiveSetDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back({code, std::move(path), std::move(message)});
}

bool consumeRecord(
    ProjectLiveSetValidationResult& result,
    std::size_t& inspected,
    std::string_view path)
{
    if (inspected >= maximumProjectLiveSetInspectedRecords)
    {
        addDiagnostic(
            result,
            ProjectLiveSetValidationCode::CapacityExceeded,
            std::string(path),
            "Live-set aggregate inspected-record limit exceeded");
        return false;
    }
    ++inspected;
    return true;
}

std::string midiMappingKey(const ProjectLiveSetMidiProgramMapping& mapping)
{
    return mapping.sourceIdentifier + "\n"
        + std::to_string(mapping.channel) + "\n"
        + std::to_string(mapping.program);
}

bool validMidiProgramMapping(
    const ProjectLiveSetMidiProgramMapping& mapping) noexcept
{
    return mapping.channel >= 1 && mapping.channel <= 16
        && mapping.program >= 0 && mapping.program <= 127
        && isValidMidiSourceIdentifier(mapping.sourceIdentifier);
}

bool emptyMidiProgramMapping(
    const ProjectLiveSetMidiProgramMapping& mapping) noexcept
{
    return mapping.sourceIdentifier.empty()
        && mapping.channel == 1 && mapping.program == 0;
}

bool validDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

const ProjectLiveSet* findLiveSet(
    const ProjectLiveSetLibrary& library,
    std::string_view liveSetId)
{
    const auto found = std::ranges::find(
        library.liveSets,
        liveSetId,
        &ProjectLiveSet::liveSetId);
    return found == library.liveSets.end() ? nullptr : &*found;
}

const ProjectLiveSetCell* findCell(
    const ProjectLiveSet& liveSet,
    std::string_view cellId)
{
    const auto found = std::ranges::find(
        liveSet.cells,
        cellId,
        &ProjectLiveSetCell::cellId);
    return found == liveSet.cells.end() ? nullptr : &*found;
}

bool targetExists(
    const ProjectDocument& document,
    const ProjectLiveSetCellTarget& target)
{
    return std::visit(
        [&](const auto& value) {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Target, ProjectLiveSetSoundTarget>)
            {
                const auto definition = std::ranges::find(
                    document.soundDefinitions,
                    value.soundDefinitionId,
                    &ProjectSoundDefinition::definitionId);
                if (definition == document.soundDefinitions.end())
                    return false;
                if (!value.sourceInstanceId.has_value())
                    return definition->inputs.empty();
                const auto node = std::ranges::find(
                    document.graph.nodes,
                    *value.sourceInstanceId,
                    &ProjectNode::instanceId);
                return node != document.graph.nodes.end()
                    && node->soundDefinitionId.has_value()
                    && *node->soundDefinitionId == value.soundDefinitionId;
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetSnapshotTarget>)
            {
                const auto bank = std::ranges::find(
                    document.snapshotLibrary.banks,
                    value.bankId,
                    &ProjectSnapshotBank::bankId);
                return bank != document.snapshotLibrary.banks.end()
                    && std::ranges::find(
                           bank->snapshots,
                           value.snapshotId,
                           &ProjectSnapshot::snapshotId)
                        != bank->snapshots.end();
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetScoreRegionTarget>)
            {
                const auto marker = std::ranges::find(
                    document.timeline.score.markers,
                    value.markerId,
                    &ScoreMarker::markerId);
                return marker != document.timeline.score.markers.end()
                    && marker->end.has_value();
            }
            else
            {
                return true;
            }
        },
        target);
}

bool validTargetShape(const ProjectLiveSetCellTarget& target) noexcept
{
    return std::visit(
        [](const auto& value) {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Target, ProjectLiveSetSoundTarget>)
            {
                return isValidProjectLiveSetIdentifier(
                           value.soundDefinitionId)
                    && (!value.sourceInstanceId.has_value()
                        || isValidProjectLiveSetIdentifier(
                            *value.sourceInstanceId));
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetSnapshotTarget>)
            {
                return isValidProjectLiveSetIdentifier(value.bankId)
                    && isValidProjectLiveSetIdentifier(value.snapshotId);
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetScoreRegionTarget>)
            {
                return isValidProjectLiveSetIdentifier(value.markerId);
            }
            else
            {
                return true;
            }
        },
        target);
}

bool validQuantization(
    const ProjectLiveSetQuantization& quantization) noexcept
{
    switch (quantization.kind)
    {
    case ProjectLiveSetQuantizationKind::TickInterval:
        return quantization.tickInterval > 0
            && quantization.tickInterval
                <= maximumProjectLiveSetQuantizationTicks;
    case ProjectLiveSetQuantizationKind::Immediate:
    case ProjectLiveSetQuantizationKind::NextBeat:
    case ProjectLiveSetQuantizationKind::NextBar:
        return quantization.tickInterval == 0;
    }
    return false;
}

bool canonicalInitialValue(const ProjectLiveToolVariable& variable) noexcept
{
    switch (variable.kind)
    {
    case ProjectLiveToolVariableKind::Number:
        return !variable.initialBoolean;
    case ProjectLiveToolVariableKind::Boolean:
        return variable.initialNumber == 0.0;
    }
    return false;
}

bool validTriggerShape(const ProjectLiveToolTrigger& trigger) noexcept
{
    switch (trigger.kind)
    {
    case ProjectLiveToolTriggerKind::Manual:
        return trigger.eventName.empty() && trigger.variableId.empty()
            && trigger.eventDomain == talas::expr::ValueDomain::Scalar
            && emptyMidiProgramMapping(trigger.midiProgram)
            && trigger.osc.address.empty() && trigger.periodTicks == 0;
    case ProjectLiveToolTriggerKind::EventValue:
        return isValidProjectLiveSetIdentifier(trigger.eventName)
            && validDomain(trigger.eventDomain)
            && trigger.variableId.empty()
            && emptyMidiProgramMapping(trigger.midiProgram)
            && trigger.osc.address.empty() && trigger.periodTicks == 0;
    case ProjectLiveToolTriggerKind::VariableChange:
        return trigger.eventName.empty()
            && trigger.eventDomain == talas::expr::ValueDomain::Scalar
            && isValidProjectLiveSetIdentifier(trigger.variableId)
            && emptyMidiProgramMapping(trigger.midiProgram)
            && trigger.osc.address.empty() && trigger.periodTicks == 0;
    case ProjectLiveToolTriggerKind::MidiProgram:
        return trigger.eventName.empty() && trigger.variableId.empty()
            && trigger.eventDomain == talas::expr::ValueDomain::Scalar
            && validMidiProgramMapping(trigger.midiProgram)
            && trigger.osc.address.empty() && trigger.periodTicks == 0;
    case ProjectLiveToolTriggerKind::Osc:
        return trigger.eventName.empty() && trigger.variableId.empty()
            && trigger.eventDomain == talas::expr::ValueDomain::Scalar
            && emptyMidiProgramMapping(trigger.midiProgram)
            && isValidOscAddress(trigger.osc.address)
            && trigger.periodTicks == 0;
    case ProjectLiveToolTriggerKind::PeriodicTicks:
        return trigger.eventName.empty() && trigger.variableId.empty()
            && trigger.eventDomain == talas::expr::ValueDomain::Scalar
            && emptyMidiProgramMapping(trigger.midiProgram)
            && trigger.osc.address.empty() && trigger.periodTicks > 0
            && trigger.periodTicks
                <= maximumProjectLiveSetQuantizationTicks;
    }
    return false;
}

bool triggerExists(
    const ProjectDocument& document,
    const ProjectLiveTool& tool,
    const ProjectLiveToolTrigger& trigger) noexcept
{
    switch (trigger.kind)
    {
    case ProjectLiveToolTriggerKind::EventValue:
        return std::ranges::any_of(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName == trigger.eventName
                    && control.domain == trigger.eventDomain;
            });
    case ProjectLiveToolTriggerKind::VariableChange:
        return std::ranges::any_of(
            tool.variables,
            [&](const auto& variable) {
                return variable.variableId == trigger.variableId;
            });
    case ProjectLiveToolTriggerKind::Manual:
    case ProjectLiveToolTriggerKind::MidiProgram:
    case ProjectLiveToolTriggerKind::Osc:
    case ProjectLiveToolTriggerKind::PeriodicTicks:
        return true;
    }
    return false;
}

std::optional<std::string> triggerMappingKey(
    const ProjectLiveToolTrigger& trigger)
{
    if (trigger.kind == ProjectLiveToolTriggerKind::MidiProgram)
        return "midi\n" + midiMappingKey(trigger.midiProgram);
    if (trigger.kind == ProjectLiveToolTriggerKind::Osc)
        return "osc\n" + trigger.osc.address;
    return std::nullopt;
}

struct GuardResolutionContext
{
    const ProjectDocument* document = nullptr;
    const ProjectLiveTool* tool = nullptr;
    bool highWitness = false;
};

bool resolveGuardVariable(
    void* user,
    std::string_view variableId,
    talas::expr::ExpressionValue& output) noexcept
{
    const auto* context = static_cast<const GuardResolutionContext*>(user);
    if (context == nullptr || context->tool == nullptr)
        return false;
    const auto variable = std::ranges::find(
        context->tool->variables,
        variableId,
        &ProjectLiveToolVariable::variableId);
    if (variable == context->tool->variables.end())
        return false;
    const auto witness = variable->kind
            == ProjectLiveToolVariableKind::Boolean
        ? (context->highWitness ? 1.0 : 0.0)
        : (context->highWitness ? 0.75 : 0.25);
    output = {witness, talas::expr::ValueDomain::Scalar};
    return true;
}

bool resolveGuardEvent(
    void* user,
    std::string_view eventName,
    talas::expr::ExpressionValue& output) noexcept
{
    const auto* context = static_cast<const GuardResolutionContext*>(user);
    if (context == nullptr || context->document == nullptr)
        return false;
    const auto control = std::ranges::find(
        context->document->virtualControls,
        eventName,
        &VirtualControl::eventName);
    if (control == context->document->virtualControls.end())
        return false;
    output = {
        context->highWitness ? 0.75 : 0.25,
        control->domain,
    };
    return true;
}

bool commandExists(
    const ProjectDocument& document,
    const ProjectLiveSetLibrary& library,
    const ProjectLiveToolCommand& command) noexcept
{
    switch (command.kind)
    {
    case ProjectLiveToolCommandKind::ActivateCell:
    {
        if (!isValidProjectLiveSetIdentifier(command.liveSetId)
            || !isValidProjectLiveSetIdentifier(command.cellId))
        {
            return false;
        }
        const auto* liveSet = findLiveSet(library, command.liveSetId);
        return liveSet != nullptr
            && findCell(*liveSet, command.cellId) != nullptr
            && command.variableId.empty() && command.eventName.empty()
            && command.eventDomain == talas::expr::ValueDomain::Scalar
            && std::isfinite(command.value) && command.value == 0.0;
    }
    case ProjectLiveToolCommandKind::Stop:
        return command.liveSetId.empty() && command.cellId.empty()
            && command.variableId.empty() && command.eventName.empty()
            && command.eventDomain == talas::expr::ValueDomain::Scalar
            && std::isfinite(command.value) && command.value == 0.0;
    case ProjectLiveToolCommandKind::SetProjectVariable:
        return command.liveSetId.empty() && command.cellId.empty()
            && isValidProjectLiveSetIdentifier(command.variableId)
            && command.eventName.empty() && std::isfinite(command.value)
            && command.eventDomain == talas::expr::ValueDomain::Scalar
            && std::ranges::any_of(
                document.variableLibrary.variables,
                [&](const auto& variable) {
                    return variable.owner.kind
                            == ProjectVariableOwnerKind::Project
                        && variable.variableId == command.variableId
                        && variable.domain
                            == talas::expr::ValueDomain::Scalar;
                });
    case ProjectLiveToolCommandKind::SetWorkspaceVariable:
        return command.liveSetId.empty() && command.cellId.empty()
            && isValidProjectLiveSetIdentifier(command.variableId)
            && command.eventName.empty() && std::isfinite(command.value)
            && command.eventDomain == talas::expr::ValueDomain::Scalar
            && std::ranges::any_of(
                document.variableLibrary.variables,
                [&](const auto& variable) {
                    return variable.owner.kind
                            == ProjectVariableOwnerKind::WorkspaceSession
                        && variable.variableId == command.variableId
                        && variable.domain
                            == talas::expr::ValueDomain::Scalar;
                });
    case ProjectLiveToolCommandKind::SetEventValue:
        return command.liveSetId.empty() && command.cellId.empty()
            && command.variableId.empty()
            && isValidProjectLiveSetIdentifier(command.eventName)
            && validDomain(command.eventDomain)
            && std::isfinite(command.value)
            && std::ranges::any_of(
                document.virtualControls,
                [&](const auto& control) {
                    return control.eventName == command.eventName
                        && control.domain == command.eventDomain;
                });
    }
    return false;
}

bool validGuardExpression(
    const ProjectDocument& document,
    const ProjectLiveTool& tool,
    std::string_view source)
{
    if (source.empty())
        return true;
    const auto parsed = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::VariablesV1,
        liveToolGuardDefinitions(document));
    if (!parsed.ok() || parsed.expression->isStateful()
        || !parsed.expression->identifiers().empty())
    {
        return false;
    }
    const auto localVariable = [&](std::string_view variableId) {
        return std::ranges::any_of(
            tool.variables,
            [&](const auto& variable) {
                return variable.variableId == variableId;
            });
    };
    if (!std::ranges::all_of(
            parsed.expression->variableReferences(), localVariable))
    {
        return false;
    }
    const auto availableEvent = [&](std::string_view eventName) {
        return std::ranges::any_of(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName == eventName;
            });
    };
    if (!std::ranges::all_of(
            parsed.expression->eventReferences(), availableEvent))
    {
        return false;
    }
    for (const auto highWitness : {false, true})
    {
        GuardResolutionContext resolution{
            &document,
            &tool,
            highWitness,
        };
        talas::expr::EvaluationContext context;
        context.sampleRate = 48000.0;
        context.tempoBpm = 120.0;
        context.user = &resolution;
        context.resolveVariable = resolveGuardVariable;
        context.resolveEvent = resolveGuardEvent;
        talas::expr::ExpressionValue value;
        if (talas::expr::tryEvaluate(
                *parsed.expression,
                context,
                {talas::expr::ValueDomain::Scalar},
                value))
        {
            return true;
        }
    }
    return false;
}
}

bool isValidProjectLiveSetIdentifier(std::string_view value) noexcept
{
    return value.size() <= maximumProjectLiveSetIdentifierBytes
        && isValidScoreIdentifier(value);
}

const talas::expr::ExpressionDefinitionLibrary& liveToolGuardDefinitions(
    const ProjectDocument& document)
{
    static const talas::expr::ExpressionDefinitionLibrary none;
    return talas::expr::parse(
               "0",
               talas::expr::ExpressionLanguageVersion::VariablesV1,
               document.expressionDefinitions)
            .ok()
        ? document.expressionDefinitions
        : none;
}

ProjectLiveSetValidationResult validateProjectLiveSetLibrary(
    const ProjectDocument& document)
{
    ProjectLiveSetValidationResult result;
    const auto& library = document.liveSetLibrary;
    std::size_t inspected = 0;
    if (library.schemaVersion != 1)
    {
        addDiagnostic(
            result,
            ProjectLiveSetValidationCode::InvalidTarget,
            "$.liveSetLibrary.version",
            "Live-set library version must be 1");
    }
    if (library.liveSets.size() > maximumProjectLiveSets
        || library.tools.size() > maximumProjectLiveTools)
    {
        addDiagnostic(
            result,
            ProjectLiveSetValidationCode::CapacityExceeded,
            "$.liveSetLibrary",
            "Live-set or tool count exceeds its fixed limit");
        return result;
    }

    std::unordered_set<std::string> liveSetIds;
    std::unordered_set<std::string> cellIds;
    std::unordered_map<std::string, std::string> mappingOwners;
    std::size_t aggregateCells = 0;
    for (std::size_t liveSetIndex = 0;
         liveSetIndex < library.liveSets.size();
         ++liveSetIndex)
    {
        const auto& liveSet = library.liveSets[liveSetIndex];
        const auto path = "$.liveSetLibrary.liveSets["
            + std::to_string(liveSetIndex) + "]";
        if (!consumeRecord(result, inspected, path))
            return result;
        if (!isValidProjectLiveSetIdentifier(liveSet.liveSetId))
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::InvalidIdentifier,
                          path + ".liveSetId", "Invalid live-set identity");
        }
        else if (!liveSetIds.insert(liveSet.liveSetId).second)
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::DuplicateIdentifier,
                          path + ".liveSetId", "Duplicate live-set identity");
        }
        if (!validDisplayName(liveSet.displayName))
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::InvalidDisplayName,
                          path + ".displayName", "Invalid live-set display name");
        }
        if (liveSet.cells.size()
                > projectLiveSetRows * projectLiveSetColumns
            || aggregateCells
                > maximumProjectLiveSetCells - liveSet.cells.size())
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::CapacityExceeded,
                          path + ".cells", "Live-set cell count exceeds its fixed limit");
            return result;
        }
        aggregateCells += liveSet.cells.size();
        std::unordered_set<std::uint32_t> positions;
        for (std::size_t cellIndex = 0;
             cellIndex < liveSet.cells.size();
             ++cellIndex)
        {
            const auto& cell = liveSet.cells[cellIndex];
            const auto cellPath = path + ".cells["
                + std::to_string(cellIndex) + "]";
            if (!consumeRecord(result, inspected, cellPath))
                return result;
            const auto validCellIdentity =
                isValidProjectLiveSetIdentifier(cell.cellId);
            if (!validCellIdentity)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidIdentifier,
                              cellPath + ".cellId", "Invalid live-set cell identity");
            }
            else if (!cellIds.insert(cell.cellId).second)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::DuplicateIdentifier,
                              cellPath + ".cellId", "Duplicate live-set cell identity");
            }
            if (!validDisplayName(cell.displayName))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidDisplayName,
                              cellPath + ".displayName", "Invalid cell display name");
            }
            if (cell.row >= projectLiveSetRows
                || cell.column >= projectLiveSetColumns)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::DuplicateCellPosition,
                              cellPath, "Cell position is outside the fixed grid");
            }
            else if (!positions.insert(
                         cell.row * projectLiveSetColumns + cell.column).second)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::DuplicateCellPosition,
                              cellPath, "Duplicate cell position within live set");
            }
            if (!validQuantization(cell.quantization))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidQuantization,
                              cellPath + ".quantization", "Invalid cell quantization");
            }
            if (!validTargetShape(cell.target))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidTarget,
                              cellPath + ".target", "Invalid live-set target shape");
            }
            else if (!targetExists(document, cell.target))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::UnknownTarget,
                              cellPath + ".target", "Live-set target does not exist");
            }
            else if (std::holds_alternative<ProjectLiveSetTrackMuteTarget>(
                         cell.target)
                     && !std::ranges::any_of(
                         liveSet.cells,
                         [&](const ProjectLiveSetCell& candidate) {
                             return candidate.column == cell.column
                                 && std::holds_alternative<
                                     ProjectLiveSetSoundTarget>(
                                     candidate.target);
                         }))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::UnknownTarget,
                              cellPath + ".target",
                              "Track mute requires a Sound cell in the same column");
            }
            if (cell.mappings.size() > maximumProjectLiveSetMappingsPerCell)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::CapacityExceeded,
                              cellPath + ".mappings", "Cell mapping count exceeds its fixed limit");
                continue;
            }
            auto workbenchMappingSeen = false;
            for (std::size_t mappingIndex = 0;
                 mappingIndex < cell.mappings.size();
                 ++mappingIndex)
            {
                const auto& mapping = cell.mappings[mappingIndex];
                const auto mappingPath = cellPath + ".mappings["
                    + std::to_string(mappingIndex) + "]";
                if (!consumeRecord(result, inspected, mappingPath))
                    return result;
                std::string key;
                auto workbenchMapping = false;
                const auto valid = std::visit(
                    [&](const auto& value) {
                        using Mapping = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<
                                          Mapping,
                                          ProjectLiveSetMidiProgramMapping>)
                        {
                            if (!validMidiProgramMapping(value))
                                return false;
                            key = "midi\n" + midiMappingKey(value);
                            return true;
                        }
                        else if constexpr (std::is_same_v<
                                               Mapping,
                                               ProjectLiveSetOscMapping>)
                        {
                            if (!isValidOscAddress(value.address))
                                return false;
                            key = "osc\n" + value.address;
                            return true;
                        }
                        else
                        {
                            workbenchMapping = true;
                            return true;
                        }
                    },
                    mapping);
                if (!valid)
                {
                    addDiagnostic(result, ProjectLiveSetValidationCode::InvalidMapping,
                                  mappingPath, "Invalid live-set trigger mapping");
                }
                else if (workbenchMapping)
                {
                    if (std::exchange(workbenchMappingSeen, true))
                    {
                        addDiagnostic(
                            result,
                            ProjectLiveSetValidationCode::DuplicateMapping,
                            mappingPath,
                            "Duplicate Workbench activation mapping");
                    }
                }
                else if (validCellIdentity
                         && !mappingOwners.emplace(
                                 std::move(key),
                                 "cell:" + cell.cellId).second)
                {
                    addDiagnostic(result, ProjectLiveSetValidationCode::DuplicateMapping,
                                  mappingPath, "Duplicate live-set trigger mapping");
                }
            }
        }
        if (liveSet.routes.size() > maximumProjectLiveSetRoutes)
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::CapacityExceeded,
                          path + ".routes", "Live-set route count exceeds the acyclic track-graph limit");
        }
        std::array<std::uint32_t, projectLiveSetColumns> destinations{};
        for (std::size_t routeIndex = 0;
             routeIndex < liveSet.routes.size(); ++routeIndex)
        {
            const auto& route = liveSet.routes[routeIndex];
            const auto routePath = path + ".routes["
                + std::to_string(routeIndex) + "]";
            if (!consumeRecord(result, inspected, routePath))
                return result;
            if (route.fromColumn >= projectLiveSetColumns
                || route.toColumn >= projectLiveSetColumns
                || route.fromColumn == route.toColumn
                || (destinations[route.fromColumn]
                    & (1U << route.toColumn)) != 0)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidRoute,
                              routePath,
                              "Route requires two different valid columns and a unique directed edge");
                continue;
            }
            const auto sourceExists = std::ranges::any_of(
                liveSet.cells,
                [&](const ProjectLiveSetCell& cell) {
                    return cell.column == route.fromColumn
                        && std::holds_alternative<ProjectLiveSetSoundTarget>(
                            cell.target);
                });
            auto targetExists = false;
            auto targetIsBoundInput = true;
            for (const auto& cell : liveSet.cells)
            {
                if (cell.column != route.toColumn)
                    continue;
                const auto* sound = std::get_if<ProjectLiveSetSoundTarget>(
                    &cell.target);
                if (sound == nullptr)
                    continue;
                targetExists = true;
                const auto definition = std::ranges::find(
                    document.soundDefinitions,
                    sound->soundDefinitionId,
                    &ProjectSoundDefinition::definitionId);
                const auto input = sound->sourceInstanceId.has_value()
                    && definition != document.soundDefinitions.end()
                    && definition->inputs.size() == 1
                    && definition->inputs.front().type
                        == talas::core::PortType::Audio;
                const auto connection = input
                    ? std::ranges::find_if(
                        document.graph.connections,
                        [&](const auto& candidate) {
                            return candidate.toNode == *sound->sourceInstanceId
                                && candidate.toPort
                                    == definition->inputs.front().portId;
                        })
                    : document.graph.connections.end();
                const auto upstream = connection
                    == document.graph.connections.end()
                    ? document.graph.nodes.end()
                    : std::ranges::find(
                        document.graph.nodes, connection->fromNode,
                        &ProjectNode::instanceId);
                targetIsBoundInput = targetIsBoundInput
                    && upstream != document.graph.nodes.end()
                    && upstream->typeId == "talas.audioInput";
            }
            if (!sourceExists || !targetExists || !targetIsBoundInput)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidRoute,
                              routePath,
                              "Route source needs a Sound and destination needs an Audio-input Sound bound to talas.audioInput");
                continue;
            }
            destinations[route.fromColumn] |= 1U << route.toColumn;
        }
        std::array<std::uint8_t, projectLiveSetColumns> visit{};
        const auto hasCycle = [&](auto&& self, std::size_t column) -> bool {
            if (visit[column] == 1)
                return true;
            if (visit[column] == 2)
                return false;
            visit[column] = 1;
            for (std::size_t next = 0; next < destinations.size(); ++next)
                if ((destinations[column] & (1U << next)) != 0
                    && self(self, next))
                    return true;
            visit[column] = 2;
            return false;
        };
        for (std::size_t column = 0; column < destinations.size(); ++column)
            if (hasCycle(hasCycle, column))
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::RouteCycle,
                              path + ".routes", "Live-set track routes contain a cycle");
                break;
            }
    }

    std::unordered_set<std::string> toolIds;
    for (std::size_t toolIndex = 0;
         toolIndex < library.tools.size();
         ++toolIndex)
    {
        const auto& tool = library.tools[toolIndex];
        const auto path = "$.liveSetLibrary.tools["
            + std::to_string(toolIndex) + "]";
        if (!consumeRecord(result, inspected, path))
            return result;
        const auto validToolIdentity =
            isValidProjectLiveSetIdentifier(tool.toolId)
            && toolIds.insert(tool.toolId).second;
        if (!validToolIdentity)
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::InvalidTool,
                          path + ".toolId", "Invalid or duplicate live-tool identity");
        }
        if (!validDisplayName(tool.displayName)
            || tool.states.empty()
            || tool.states.size() > maximumProjectLiveToolStates
            || tool.variables.size() > maximumProjectLiveToolVariables
            || tool.transitions.size() > maximumProjectLiveToolTransitions)
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::InvalidTool,
                          path, "Invalid live-tool shape or fixed-capacity count");
            continue;
        }
        std::unordered_set<std::string> stateIds;
        for (std::size_t stateIndex = 0;
             stateIndex < tool.states.size();
             ++stateIndex)
        {
            const auto& state = tool.states[stateIndex];
            const auto statePath = path + ".states["
                + std::to_string(stateIndex) + "]";
            if (!consumeRecord(result, inspected, statePath))
                return result;
            if (!isValidProjectLiveSetIdentifier(state.stateId)
                || !validDisplayName(state.displayName)
                || !stateIds.insert(state.stateId).second)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidState,
                              statePath, "Invalid or duplicate live-tool state");
            }
        }
        if (!isValidProjectLiveSetIdentifier(tool.initialStateId)
            || !stateIds.contains(tool.initialStateId))
        {
            addDiagnostic(result, ProjectLiveSetValidationCode::InvalidState,
                          path + ".initialStateId", "Unknown initial live-tool state");
        }
        std::unordered_set<std::string> variableIds;
        for (std::size_t variableIndex = 0;
             variableIndex < tool.variables.size();
             ++variableIndex)
        {
            const auto& variable = tool.variables[variableIndex];
            const auto variablePath = path + ".variables["
                + std::to_string(variableIndex) + "]";
            if (!consumeRecord(result, inspected, variablePath))
                return result;
            const auto validState = !variable.stateId.has_value()
                || (isValidProjectLiveSetIdentifier(*variable.stateId)
                    && stateIds.contains(*variable.stateId));
            if (!isValidProjectLiveSetIdentifier(variable.variableId)
                || !validDisplayName(variable.displayName)
                || !std::isfinite(variable.initialNumber)
                || !canonicalInitialValue(variable)
                || !validState
                || !variableIds.insert(variable.variableId).second)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidVariable,
                              variablePath, "Invalid or duplicate live-tool variable");
            }
        }
        std::unordered_set<std::string> transitionIds;
        for (std::size_t transitionIndex = 0;
             transitionIndex < tool.transitions.size();
             ++transitionIndex)
        {
            const auto& transition = tool.transitions[transitionIndex];
            const auto transitionPath = path + ".transitions["
                + std::to_string(transitionIndex) + "]";
            if (!consumeRecord(result, inspected, transitionPath))
                return result;
            if (!isValidProjectLiveSetIdentifier(transition.transitionId)
                || !transitionIds.insert(transition.transitionId).second
                || !isValidProjectLiveSetIdentifier(
                    transition.fromStateId)
                || !isValidProjectLiveSetIdentifier(
                    transition.toStateId)
                || !stateIds.contains(transition.fromStateId)
                || !stateIds.contains(transition.toStateId)
                || transition.guardExpression.size()
                    > maximumProjectLiveToolGuardBytes
                || !validGuardExpression(
                    document,
                    tool,
                    transition.guardExpression)
                || !validTriggerShape(transition.trigger)
                || !triggerExists(document, tool, transition.trigger)
                || transition.commands.empty()
                || transition.commands.size()
                    > maximumProjectLiveToolCommandsPerTransition)
            {
                addDiagnostic(result, ProjectLiveSetValidationCode::InvalidTransition,
                              transitionPath, "Invalid live-tool transition");
                continue;
            }
            if (const auto key = triggerMappingKey(transition.trigger);
                key.has_value() && validToolIdentity)
            {
                const auto owner = "tool:" + tool.toolId;
                const auto [found, inserted] =
                    mappingOwners.emplace(*key, owner);
                if (!inserted && found->second != owner)
                {
                    addDiagnostic(
                        result,
                        ProjectLiveSetValidationCode::DuplicateMapping,
                        transitionPath + ".trigger",
                        "Live-tool trigger mapping conflicts with another target");
                }
            }
            for (std::size_t commandIndex = 0;
                 commandIndex < transition.commands.size();
                 ++commandIndex)
            {
                const auto commandPath = transitionPath + ".commands["
                    + std::to_string(commandIndex) + "]";
                if (!consumeRecord(result, inspected, commandPath))
                    return result;
                if (!commandExists(
                        document,
                        library,
                        transition.commands[commandIndex]))
                {
                    addDiagnostic(result, ProjectLiveSetValidationCode::InvalidCommand,
                                  commandPath, "Invalid or unavailable live-tool command");
                }
            }
            const auto programCommandCount = std::ranges::count_if(
                transition.commands,
                [](const ProjectLiveToolCommand& command) {
                    return command.kind
                            == ProjectLiveToolCommandKind::ActivateCell
                        || command.kind
                            == ProjectLiveToolCommandKind::Stop;
                });
            const auto hasActivateCell = std::ranges::any_of(
                transition.commands,
                [](const ProjectLiveToolCommand& command) {
                    return command.kind
                        == ProjectLiveToolCommandKind::ActivateCell;
                });
            if (programCommandCount > 1
                || (hasActivateCell
                    && transition.commands.size() != 1))
            {
                addDiagnostic(
                    result,
                    ProjectLiveSetValidationCode::InvalidCommand,
                    transitionPath + ".commands",
                    "A live-tool transition may contain one program command, and Activate Cell must be atomic by itself");
            }
        }
    }
    return result;
}
}
