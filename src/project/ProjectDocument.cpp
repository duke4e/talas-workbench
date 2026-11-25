#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
bool layoutMatchesScope(
    const ProjectStructureLayout& layout,
    std::optional<std::string_view> soundDefinitionId)
{
    if (!soundDefinitionId.has_value())
        return !layout.soundDefinitionId.has_value();
    return layout.soundDefinitionId == *soundDefinitionId;
}

SnapshotTargetIdentity eventTarget(std::string eventName,
                                   talas::expr::ValueDomain domain)
{
    SnapshotTargetIdentity target;
    target.kind = SnapshotTargetKind::EventValue;
    target.eventName = std::move(eventName);
    target.domain = domain;
    return target;
}

ProjectPerformanceControlKind presentationKind(VirtualControlKind kind)
{
    switch (kind)
    {
    case VirtualControlKind::Fader:
        return ProjectPerformanceControlKind::Fader;
    case VirtualControlKind::Gate:
        return ProjectPerformanceControlKind::Gate;
    case VirtualControlKind::Toggle:
        return ProjectPerformanceControlKind::Toggle;
    }
    return ProjectPerformanceControlKind::Fader;
}

std::string stableControlId(const SnapshotTargetIdentity& target)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    auto hash = offset;
    for (const auto value : snapshotTargetIdentityString(target))
    {
        hash ^= static_cast<unsigned char>(value);
        hash *= prime;
    }
    constexpr std::array<char, 16> digits = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string result = "performance-";
    for (int shift = 60; shift >= 0; shift -= 4)
        result.push_back(digits[(hash >> shift) & 0xf]);
    return result;
}

const ProjectSoundDefinition* findDefinition(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

bool targetExists(const ProjectDocument& document,
                  const SnapshotTargetIdentity& target)
{
    if (target.kind == SnapshotTargetKind::EventValue)
    {
        return std::ranges::any_of(
            document.virtualControls,
            [&](const auto& control) {
                return control.eventName == target.eventName
                    && control.domain == target.domain;
            });
    }
    if (target.kind != SnapshotTargetKind::SoundParameter
        || target.instancePath.empty())
    {
        return false;
    }

    const ProjectGraph* graph = &document.graph;
    const ProjectSoundDefinition* definition = nullptr;
    for (const auto& instanceId : target.instancePath)
    {
        const auto instance = std::ranges::find_if(
            graph->nodes,
            [&](const auto& node) {
                return node.instanceId == instanceId;
            });
        if (instance == graph->nodes.end()
            || !instance->soundDefinitionId.has_value())
        {
            return false;
        }
        definition = findDefinition(document, *instance->soundDefinitionId);
        if (definition == nullptr)
            return false;
        graph = &definition->graph;
    }
    return definition != nullptr
        && definition->definitionId == target.definitionId
        && std::ranges::any_of(
            definition->exposedParameters,
            [&](const auto& parameter) {
                return parameter.parameterId == target.parameterId
                    && parameter.kind != talas::core::ParameterKind::Resource
                    && parameter.kind != talas::core::ParameterKind::Text;
            });
}
}

const ProjectStructureLayout* findProjectStructureLayout(
    const ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId)
{
    const auto found = std::ranges::find_if(
        document.presentation.structureLayouts,
        [&](const auto& layout) {
            return layoutMatchesScope(layout, soundDefinitionId);
        });
    return found == document.presentation.structureLayouts.end()
        ? nullptr
        : &*found;
}

ProjectStructureLayout* findProjectStructureLayout(
    ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId)
{
    return const_cast<ProjectStructureLayout*>(
        findProjectStructureLayout(
            std::as_const(document),
            soundDefinitionId));
}

const ProjectNodePresentation* findProjectNodePresentation(
    const ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId)
{
    const auto* layout = findProjectStructureLayout(
        document,
        soundDefinitionId);
    if (layout == nullptr)
        return nullptr;
    const auto found = std::ranges::find_if(
        layout->nodes,
        [&](const auto& node) {
            return node.nodeId == nodeId;
        });
    return found == layout->nodes.end() ? nullptr : &*found;
}

ProjectNodePresentation* findProjectNodePresentation(
    ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId)
{
    return const_cast<ProjectNodePresentation*>(
        findProjectNodePresentation(
            std::as_const(document),
            soundDefinitionId,
            nodeId));
}

const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    const ProjectDocument& document,
    std::string_view eventName,
    talas::expr::ValueDomain domain)
{
    const auto found = std::ranges::find_if(
        document.presentation.performanceControls,
        [&](const auto& control) {
            return control.target.kind == SnapshotTargetKind::EventValue
                && control.target.eventName == eventName
                && control.target.domain == domain;
        });
    return found == document.presentation.performanceControls.end()
        ? nullptr
        : &*found;
}

const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    const ProjectDocument& document,
    const SnapshotTargetIdentity& target)
{
    const auto found = std::ranges::find(
        document.presentation.performanceControls,
        target,
        &ProjectPerformanceControlPresentation::target);
    return found == document.presentation.performanceControls.end()
        ? nullptr
        : &*found;
}

ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    ProjectDocument& document,
    const SnapshotTargetIdentity& target)
{
    return const_cast<ProjectPerformanceControlPresentation*>(
        findProjectPerformanceControlPresentation(
            std::as_const(document),
            target));
}

const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentationById(
    const ProjectDocument& document,
    std::string_view controlId)
{
    const auto found = std::ranges::find(
        document.presentation.performanceControls,
        controlId,
        &ProjectPerformanceControlPresentation::controlId);
    return found == document.presentation.performanceControls.end()
        ? nullptr
        : &*found;
}

ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentationById(
    ProjectDocument& document,
    std::string_view controlId)
{
    return const_cast<ProjectPerformanceControlPresentation*>(
        findProjectPerformanceControlPresentationById(
            std::as_const(document),
            controlId));
}

ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    ProjectDocument& document,
    std::string_view eventName,
    talas::expr::ValueDomain domain)
{
    return const_cast<ProjectPerformanceControlPresentation*>(
        findProjectPerformanceControlPresentation(
            std::as_const(document),
            eventName,
            domain));
}

bool isProjectPerformanceTargetAvailable(
    const ProjectDocument& document,
    const SnapshotTargetIdentity& target)
{
    return targetExists(document, target);
}

bool isValidSoundParameterClassFields(const ProjectSoundExposedParameter& parameter) noexcept
{
    if (parameter.minimum.has_value() && parameter.maximum.has_value() && !(*parameter.minimum < *parameter.maximum))
        return false;
    if ((parameter.minimum.has_value() && !std::isfinite(*parameter.minimum))
        || (parameter.maximum.has_value() && !std::isfinite(*parameter.maximum)))
        return false;
    if (parameter.field.has_value())
    {
        const auto& field = *parameter.field;
        const auto unit = [](double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; };
        if (!unit(field.left) || !unit(field.top) || !unit(field.right) || !unit(field.bottom)
            || !(field.left < field.right) || !(field.top < field.bottom))
            return false;
    }
    using Kind = talas::core::ParameterKind;
    switch (parameter.fieldKind)
    {
    case ProjectSoundFieldKind::Default:
        break;
    case ProjectSoundFieldKind::Value:
    case ProjectSoundFieldKind::HotValue:
        if (parameter.kind != Kind::Expression)
            return false;
        break;
    case ProjectSoundFieldKind::String:
    case ProjectSoundFieldKind::CodeString:
    case ProjectSoundFieldKind::Complex:
    case ProjectSoundFieldKind::PolesAndZeroes:
    case ProjectSoundFieldKind::Grammar:
        if (parameter.kind != Kind::Text)
            return false;
        break;
    case ProjectSoundFieldKind::SymbolList:
    case ProjectSoundFieldKind::RadioButtons:
    case ProjectSoundFieldKind::RadioButtonsTwoColumns:
        if (parameter.kind != Kind::Choice)
            return false;
        break;
    }
    return parameter.comment.size() <= maximumSoundParameterCommentBytes;
}

namespace
{
constexpr std::array<std::pair<ProjectSoundFieldKind, std::string_view>, 10> soundFieldKindNames{{
    {ProjectSoundFieldKind::Value, "value"},
    {ProjectSoundFieldKind::HotValue, "hotValue"},
    {ProjectSoundFieldKind::String, "string"},
    {ProjectSoundFieldKind::CodeString, "codeString"},
    {ProjectSoundFieldKind::SymbolList, "symbolList"},
    {ProjectSoundFieldKind::RadioButtons, "radioButtons"},
    {ProjectSoundFieldKind::RadioButtonsTwoColumns, "radioButtonsTwoColumns"},
    {ProjectSoundFieldKind::Complex, "complex"},
    {ProjectSoundFieldKind::PolesAndZeroes, "polesAndZeroes"},
    {ProjectSoundFieldKind::Grammar, "grammar"},
}};
}

std::string_view projectSoundFieldKindName(ProjectSoundFieldKind kind) noexcept
{
    const auto found = std::ranges::find(soundFieldKindNames, kind, &std::pair<ProjectSoundFieldKind, std::string_view>::first);
    return found != soundFieldKindNames.end() ? found->second : std::string_view{};
}

std::optional<ProjectSoundFieldKind> parseProjectSoundFieldKind(std::string_view name) noexcept
{
    const auto found = std::ranges::find(soundFieldKindNames, name, &std::pair<ProjectSoundFieldKind, std::string_view>::second);
    if (found == soundFieldKindNames.end())
        return std::nullopt;
    return found->first;
}

bool isValidVirtualControlWidget(
    const VirtualControlWidget& widget, double minimum, double maximum) noexcept
{
    const auto plain = [](const std::string& text, std::size_t limit) {
        return text.size() <= limit
            && std::ranges::none_of(text, [](char character) {
                   return static_cast<unsigned char>(character) < 0x20 || character == 0x7F;
               });
    };
    const auto colour = [](std::optional<std::uint32_t> value) { return !value.has_value() || *value <= 0xFFFFFFu; };
    return plain(widget.label, maximumVirtualControlLabelBytes) && plain(widget.tab, maximumVirtualControlTabBytes)
        && std::isfinite(widget.grid) && widget.grid >= 0.0 && widget.grid <= maximum - minimum
        && (widget.taper == VirtualControlTaper::Linear || minimum > 0.0) && colour(widget.onColour)
        && colour(widget.offColour)
        && (!widget.bounds.has_value()
            || (widget.bounds->left >= 0 && widget.bounds->top >= 0 && widget.bounds->width > 0 && widget.bounds->height > 0
                && widget.bounds->right() <= 100000 && widget.bounds->bottom() <= 100000));
}

bool isValidVirtualControlLayoutSettings(const VirtualControlLayoutSettings& settings) noexcept
{
    const auto letter = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
    };
    const auto digit = [](char character) { return character >= '0' && character <= '9'; };
    const auto& bank = settings.presetBankId;
    if (settings.layout.empty() || settings.layout.size() > maximumVirtualControlLabelBytes
        || std::ranges::any_of(settings.layout, [](char character) {
               return static_cast<unsigned char>(character) < 0x20 || character == 0x7F;
           })
        || bank.size() > 128 || (!bank.empty() && !letter(bank.front()))
        || !std::ranges::all_of(bank, [&](char character) { return letter(character) || digit(character) || character == '-'; })
        || settings.widgets.size() > maximumVirtualControlLayoutWidgets)
        return false;
    for (std::size_t index = 0; index < settings.widgets.size(); ++index)
    {
        const auto& widget = settings.widgets[index];
        if (widget.eventName.empty() || widget.eventName.size() > 128 || !letter(widget.eventName.front())
            || !std::ranges::all_of(widget.eventName, [&](char character) { return letter(character) || digit(character); })
            || !std::isfinite(widget.minimum) || !std::isfinite(widget.maximum) || !(widget.minimum < widget.maximum)
            || !isValidVirtualControlWidget(widget.widget, widget.minimum, widget.maximum)
            || std::ranges::any_of(settings.widgets.begin(), settings.widgets.begin() + static_cast<long>(index),
                [&](const VirtualControlLayoutWidget& earlier) { return earlier.eventName == widget.eventName; }))
            return false;
    }
    return true;
}

const ProjectSoundPort* replaceableSoundInput(const ProjectSoundDefinition& definition) noexcept
{
    const ProjectSoundPort* first = nullptr;
    for (const auto& input : definition.inputs)
    {
        if (input.replacesSound)
            return &input;
        if (first == nullptr && input.type == talas::core::PortType::Audio)
            first = &input;
    }
    return first;
}

bool isValidVirtualControlDecoration(const VirtualControlDecoration& decoration) noexcept
{
    const auto& bounds = decoration.bounds;
    return decoration.text.size() <= maximumVirtualControlDecorationTextBytes
        && std::ranges::none_of(decoration.text,
            [](char character) {
                const auto code = static_cast<unsigned char>(character);
                return (code < 0x20 && character != '\n' && character != '\t') || code == 0x7F;
            })
        && bounds.left >= 0 && bounds.top >= 0 && bounds.width > 0 && bounds.height > 0 && bounds.right() <= 100000
        && bounds.bottom() <= 100000;
}

bool takeVirtualControlWidget(VirtualControl& control, const VirtualControl& pasted)
{
    if (!std::isfinite(pasted.minimum) || !std::isfinite(pasted.maximum) || !(pasted.minimum < pasted.maximum)
        || !isValidVirtualControlWidget(pasted.widget, pasted.minimum, pasted.maximum))
        return false;
    control.widget = pasted.widget;
    control.minimum = pasted.minimum;
    control.maximum = pasted.maximum;
    control.value = std::clamp(control.value, control.minimum, control.maximum);
    return true;
}

bool wakeDormantVirtualControl(std::vector<VirtualControl>& dormant, VirtualControl& control)
{
    const auto found = std::ranges::find(dormant, control.eventName, &VirtualControl::eventName);
    if (found == dormant.end())
        return false;
    const auto woke = takeVirtualControlWidget(control, *found);
    dormant.erase(found);
    return woke;
}

bool isValidProjectPerformanceIdentifier(std::string_view value) noexcept
{
    const auto isStart = [](char character) {
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || character == '_';
    };
    return !value.empty()
        && value.size() <= maximumPerformanceIdentifierBytes
        && isStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [](char character) {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9')
                    || character == '_'
                    || character == '-';
            });
}

bool projectPerformancePresentationHasCycle(
    const ProjectDocument& document)
{
    std::unordered_map<std::string, std::string> targetToControl;
    for (const auto& control : document.presentation.performanceControls)
    {
        if (control.kind == ProjectPerformanceControlKind::Macro)
        {
            targetToControl.emplace(
                snapshotTargetIdentityString(control.target),
                control.controlId);
        }
    }
    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (const auto& control : document.presentation.performanceControls)
    {
        if (control.kind != ProjectPerformanceControlKind::Macro)
            continue;
        for (const auto& slot : control.modulationSlots)
        {
            const auto destination = targetToControl.find(
                snapshotTargetIdentityString(slot.target));
            if (destination != targetToControl.end())
                edges[control.controlId].push_back(destination->second);
        }
    }
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    const auto visit = [&](const auto& self, const std::string& id) -> bool {
        if (visiting.contains(id))
            return true;
        if (visited.contains(id))
            return false;
        visiting.insert(id);
        for (const auto& destination : edges[id])
        {
            if (self(self, destination))
                return true;
        }
        visiting.erase(id);
        visited.insert(id);
        return false;
    };
    return std::ranges::any_of(edges, [&](const auto& edge) {
        return visit(visit, edge.first);
    });
}

void reconcileProjectPresentation(ProjectDocument& document)
{
    const auto previous = std::move(document.presentation);
    ProjectPresentationState reconciled;
    reconciled.schemaVersion = 2;
    const auto appendLayout = [&](const ProjectGraph& graph,
                                  std::optional<std::string> definitionId) {
        ProjectStructureLayout layout;
        layout.soundDefinitionId = definitionId;
        layout.nodes.reserve(graph.nodes.size());
        const auto previousLayout = std::ranges::find_if(
            previous.structureLayouts,
            [&](const auto& candidate) {
                return candidate.soundDefinitionId == definitionId;
            });
        for (const auto& node : graph.nodes)
        {
            if (previousLayout != previous.structureLayouts.end())
            {
                const auto presented = std::ranges::find_if(
                    previousLayout->nodes,
                    [&](const auto& candidate) {
                        return candidate.nodeId == node.instanceId;
                    });
                if (presented != previousLayout->nodes.end())
                {
                    layout.nodes.push_back(*presented);
                    continue;
                }
            }
            layout.nodes.push_back({node.instanceId, {}});
        }
        reconciled.structureLayouts.push_back(std::move(layout));
    };

    appendLayout(document.graph, std::nullopt);
    for (const auto& definition : document.soundDefinitions)
        appendLayout(definition.graph, definition.definitionId);

    reconciled.performanceControls.reserve(std::min(
        maximumPerformanceControlCount,
        document.virtualControls.size() + previous.performanceControls.size()));
    std::unordered_set<std::string> controlIds;
    std::unordered_set<std::string> targetIds;
    auto remainingModulationSlots = maximumPerformanceModulationSlotCount;
    const auto appendControl = [&](ProjectPerformanceControlPresentation item) {
        if (reconciled.performanceControls.size()
                >= maximumPerformanceControlCount
            || !targetExists(document, item.target))
        {
            return;
        }
        const auto targetId = snapshotTargetIdentityString(item.target);
        if (!targetIds.insert(targetId).second)
            return;
        if (!isValidProjectPerformanceIdentifier(item.controlId)
            || controlIds.contains(item.controlId))
        {
            item.controlId = stableControlId(item.target);
            auto suffix = std::size_t{1};
            const auto base = item.controlId;
            while (controlIds.contains(item.controlId))
                item.controlId = base + "-" + std::to_string(suffix++);
        }
        controlIds.insert(item.controlId);
        if (item.displayName.empty()
            || item.displayName.size() > maximumPerformanceDisplayNameBytes)
        {
            item.displayName = item.target.kind == SnapshotTargetKind::EventValue
                ? item.target.eventName
                : item.target.parameterId;
        }
        if (item.kind != ProjectPerformanceControlKind::Macro)
            item.modulationSlots.clear();
        else
        {
            std::unordered_set<std::string> destinations;
            std::erase_if(item.modulationSlots, [&](const auto& slot) {
                const auto destination = snapshotTargetIdentityString(slot.target);
                return !std::isfinite(slot.depth)
                    || slot.depth < -1.0 || slot.depth > 1.0
                    || slot.target == item.target
                    || !targetExists(document, slot.target)
                    || !destinations.insert(destination).second;
            });
            if (item.modulationSlots.size()
                > maximumPerformanceModulationSlotsPerControl)
            {
                item.modulationSlots.resize(
                    maximumPerformanceModulationSlotsPerControl);
            }
            if (item.modulationSlots.size() > remainingModulationSlots)
                item.modulationSlots.resize(remainingModulationSlots);
            remainingModulationSlots -= item.modulationSlots.size();
        }
        reconciled.performanceControls.push_back(std::move(item));
    };

    for (const auto& control : document.virtualControls)
    {
        const auto target = eventTarget(control.eventName, control.domain);
        const auto found = std::ranges::find_if(
            previous.performanceControls,
            [&](const auto& candidate) {
                return candidate.target == target;
            });
        if (found != previous.performanceControls.end())
        {
            appendControl(*found);
            continue;
        }
        ProjectPerformanceControlPresentation item;
        item.target = target;
        item.controlId = stableControlId(item.target);
        item.kind = presentationKind(control.kind);
        item.displayName = control.eventName;
        appendControl(std::move(item));
    }
    for (const auto& item : previous.performanceControls)
    {
        if (item.target.kind != SnapshotTargetKind::EventValue)
            appendControl(item);
    }

    std::unordered_set<std::string> meterIds;
    for (const auto& meter : previous.performanceMeters)
    {
        if (reconciled.performanceMeters.size() >= maximumPerformanceMeterCount
            || !isValidProjectPerformanceIdentifier(meter.meterId)
            || meter.outputChannel == 0
            || !meterIds.insert(meter.meterId).second)
        {
            continue;
        }
        reconciled.performanceMeters.push_back(meter);
    }
    document.presentation = std::move(reconciled);
}

talas::core::Graph makeGraph(const ProjectDocument& document)
{
    talas::core::Graph graph;
    for (const auto& node : document.graph.nodes)
        graph.addNode({node.instanceId, node.typeId});

    for (const auto& connection : document.graph.connections)
        graph.connect(connection);

    return graph;
}

ProjectDocument makeNewProjectDocument()
{
    ProjectDocument document;
    document.expressionDefinitions.fieldBeatTempoBpm = talas::expr::newProjectFieldBeatTempoBpm;
    return document;
}

ProjectDocument makeProjectDocument(const talas::core::Graph& graph)
{
    ProjectDocument document;
    for (const auto& node : graph.nodes())
        document.graph.nodes.push_back({node.instanceId, node.typeId, {}});

    document.graph.connections = graph.connections();
    reconcileProjectPresentation(document);
    return document;
}
}
