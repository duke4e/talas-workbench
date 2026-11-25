#include "talas/app/PrototypeBrowserModel.h"

#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/GraphBranchLayout.h"
#include "talas/project/ProjectEditor.h"

#include <algorithm>
#include <array>
#include <utility>
#include <cctype>

namespace talas::app
{
namespace
{
std::string folded(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
    {
        result.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(character))));
    }
    return result;
}

bool containsTypeId(
    const std::vector<std::string>& values,
    std::string_view typeId)
{
    return std::ranges::find(values, typeId) != values.end();
}

PrototypeMaturityFilter maturity(
    talas::core::NodeSupportLevel level)
{
    switch (level)
    {
    case talas::core::NodeSupportLevel::FullRuntime:
        return PrototypeMaturityFilter::Full;
    case talas::core::NodeSupportLevel::FunctionalSubset:
        return PrototypeMaturityFilter::Subset;
    case talas::core::NodeSupportLevel::Placeholder:
        return PrototypeMaturityFilter::Placeholder;
    case talas::core::NodeSupportLevel::Utility:
        return PrototypeMaturityFilter::Utility;
    }
    return PrototypeMaturityFilter::Utility;
}

bool matchesRate(
    const PrototypeBrowserEntry& entry,
    PrototypeRateFilter filter)
{
    if (filter == PrototypeRateFilter::All)
        return true;
    const auto expected = [&] {
        switch (filter)
        {
        case PrototypeRateFilter::Constant:
            return talas::core::PortSignalRate::Constant;
        case PrototypeRateFilter::Block:
            return talas::core::PortSignalRate::Block;
        case PrototypeRateFilter::Sample:
            return talas::core::PortSignalRate::Sample;
        case PrototypeRateFilter::Event:
            return talas::core::PortSignalRate::Event;
        case PrototypeRateFilter::Frame:
            return talas::core::PortSignalRate::Frame;
        case PrototypeRateFilter::VariableOrUnknown:
        case PrototypeRateFilter::All:
            return talas::core::PortSignalRate::VariableOrUnknown;
        }
        return talas::core::PortSignalRate::VariableOrUnknown;
    }();
    return std::ranges::find(entry.rates, expected) != entry.rates.end();
}

bool matchesOrigin(
    talas::core::NodeOrigin origin,
    PrototypeOriginFilter filter)
{
    if (filter == PrototypeOriginFilter::All)
        return true;
    switch (filter)
    {
    case PrototypeOriginFilter::BuiltIn:
        return origin == talas::core::NodeOrigin::BuiltIn;
    case PrototypeOriginFilter::Project:
        return origin == talas::core::NodeOrigin::Project;
    case PrototypeOriginFilter::PreservedComponent:
        return origin == talas::core::NodeOrigin::PreservedComponent;
    case PrototypeOriginFilter::IsolatedComponent:
        return origin == talas::core::NodeOrigin::IsolatedComponent;
    case PrototypeOriginFilter::External:
        return origin == talas::core::NodeOrigin::External;
    case PrototypeOriginFilter::All:
        return true;
    }
    return false;
}

void appendSearchTerm(std::string& index, std::string_view value)
{
    if (!index.empty())
        index.push_back(' ');
    index.append(value);
}
}

PrototypeBrowserEntry prototypeBrowserEntry(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    PrototypeBrowserEntry entry{
        descriptor.typeId,
        descriptor.displayName,
        descriptor.category,
        maturity(descriptor.supportLevel),
    };
    entry.vendor = descriptor.provenance.vendor;
    entry.origin = descriptor.provenance.origin;
    const auto appendPort = [&](const auto& port) {
        if (std::ranges::find(entry.rates, port.signal.rate)
            == entry.rates.end())
        {
            entry.rates.push_back(port.signal.rate);
        }
        appendSearchTerm(entry.searchIndex, port.id);
        appendSearchTerm(
            entry.searchIndex,
            descriptorPortTypeName(port.type));
        appendSearchTerm(
            entry.searchIndex,
            talas::core::portSignalRateName(port.signal.rate));
        appendSearchTerm(
            entry.searchIndex,
            talas::core::portEdgeSemanticsName(port.signal.edge));
    };
    for (const auto& port : descriptor.inputs)
    {
        if (!port.compilerPopulated)
            appendPort(port);
    }
    for (const auto& port : descriptor.outputs)
    {
        if (!port.compilerPopulated)
            appendPort(port);
    }
    std::ranges::sort(entry.rates);

    appendSearchTerm(entry.searchIndex, descriptor.typeId);
    appendSearchTerm(entry.searchIndex, descriptor.displayName);
    appendSearchTerm(entry.searchIndex, descriptor.category);
    for (const auto also : prototypeAlsoListedIn(descriptor.typeId))
        appendSearchTerm(entry.searchIndex, also);
    appendSearchTerm(entry.searchIndex, descriptor.supportSummary);
    appendSearchTerm(
        entry.searchIndex,
        talas::core::nodeSupportLevelName(descriptor.supportLevel));
    appendSearchTerm(entry.searchIndex, descriptor.provenance.vendor);
    appendSearchTerm(
        entry.searchIndex,
        talas::core::nodeOriginName(descriptor.provenance.origin));
    appendSearchTerm(
        entry.searchIndex,
        descriptorOriginLabel(descriptor.provenance.origin));
    appendSearchTerm(
        entry.searchIndex,
        talas::core::executionSupportName(descriptor.capabilities.offline));
    appendSearchTerm(
        entry.searchIndex,
        talas::core::executionSupportName(
            descriptor.capabilities.preparedRealtime));
    appendSearchTerm(
        entry.searchIndex,
        talas::core::executionBindingName(
            descriptor.capabilities.offlineBinding));
    appendSearchTerm(
        entry.searchIndex,
        talas::core::executionBindingName(
            descriptor.capabilities.preparedRealtimeBinding));
    appendSearchTerm(
        entry.searchIndex,
        talas::core::resourceAccessModeName(
            descriptor.capabilities.resourceAccess));
    for (const auto& limitation : descriptor.capabilities.limitations)
        appendSearchTerm(entry.searchIndex, limitation);
    appendSearchTerm(
        entry.searchIndex,
        descriptorNodeCapabilitySummary(descriptor));
    entry.searchIndex = folded(entry.searchIndex);
    return entry;
}

bool prototypeBrowserMatches(
    const PrototypeBrowserEntry& entry,
    std::string_view query,
    PrototypeBrowserScope scope,
    PrototypeMaturityFilter maturity,
    const std::unordered_set<std::string>& favorites,
    const std::vector<std::string>& recentTypeIds,
    PrototypeRateFilter rate,
    PrototypeOriginFilter origin)
{
    if (maturity != PrototypeMaturityFilter::All
        && entry.maturity != maturity)
    {
        return false;
    }
    if (scope == PrototypeBrowserScope::Favorites
        && !favorites.contains(entry.typeId))
    {
        return false;
    }
    if (scope == PrototypeBrowserScope::Recent
        && !containsTypeId(recentTypeIds, entry.typeId))
    {
        return false;
    }
    if (!matchesRate(entry, rate) || !matchesOrigin(entry.origin, origin))
        return false;
    if (query.empty())
        return true;

    const auto index = entry.searchIndex.empty()
        ? folded(
              entry.displayName + " " + entry.category + " " + entry.typeId
              + " " + entry.vendor)
        : entry.searchIndex;
    const auto foldedQuery = folded(query);
    std::size_t offset = 0;
    while (offset < foldedQuery.size())
    {
        while (offset < foldedQuery.size()
               && std::isspace(
                   static_cast<unsigned char>(foldedQuery[offset])))
        {
            ++offset;
        }
        const auto start = offset;
        while (offset < foldedQuery.size()
               && !std::isspace(
                   static_cast<unsigned char>(foldedQuery[offset])))
        {
            ++offset;
        }
        if (start != offset
            && index.find(foldedQuery.substr(start, offset - start))
                == std::string::npos)
        {
            return false;
        }
    }
    return true;
}

std::string prototypeBrowserGroupName(
    PrototypeBrowserScope scope,
    std::string_view category)
{
    return scope == PrototypeBrowserScope::Recent
        ? std::string{"Recent"}
        : std::string{category};
}

std::string prototypeCapabilityTooltip(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    return descriptorNodeCapabilitySummary(descriptor);
}

void recordRecentPrototype(
    std::vector<std::string>& recentTypeIds,
    std::string typeId,
    std::size_t limit)
{
    std::erase(recentTypeIds, typeId);
    if (limit == 0 || typeId.empty())
    {
        if (limit == 0)
            recentTypeIds.clear();
        return;
    }
    recentTypeIds.insert(recentTypeIds.begin(), std::move(typeId));
    if (recentTypeIds.size() > limit)
        recentTypeIds.resize(limit);
}

int zoomedPrototypeStep(int step, bool in) noexcept
{
    return std::clamp(step + (in ? 1 : -1), -prototypeZoomSteps, prototypeZoomSteps);
}

int prototypeRowHeight(int step) noexcept
{
    constexpr std::array heights{26, 32, 38, 48, 60};
    return heights[static_cast<std::size_t>(std::clamp(step, -prototypeZoomSteps, prototypeZoomSteps) + prototypeZoomSteps)];
}

std::vector<std::string_view> prototypeAlsoListedIn(std::string_view typeId)
{
    // A delay that feeds back among the reverbs, an LFO and a follower
    // among the control signals, a Sample and Noise among the sources, and
    // the MIDIVoice, whose Script plays it, among the Scripts; and the
    // Annotation, a Sound, among the Variables and Annotations.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> listings{{
        {"talas.delayWithFeedback", "Reverb, Delay, Feedback"},
        {"talas.lfo", "Envelopes & Control Signals"},
        {"talas.amplitudeFollower", "Envelopes & Control Signals"},
        {"talas.sample", "Sources"},
        {"talas.noise", "Sources"},
        {"talas.midiVoice", "Scripts"},
        {"talas.annotation", "Variables & Annotation"},
    }};
    std::vector<std::string_view> categories;
    for (const auto& [listed, category] : listings)
    {
        if (listed == typeId)
            categories.push_back(category);
    }
    return categories;
}

std::vector<std::string> prototypeFindChoices(
    const std::vector<talas::core::NodeTypeDescriptor>& palette, std::string_view partialName)
{
    const auto lowered = [](std::string_view text) {
        std::string result{text};
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    };
    std::string needle = lowered(partialName);
    while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.front())) != 0)
        needle.erase(needle.begin());
    while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.back())) != 0)
        needle.pop_back();
    std::vector<std::string> choices;
    if (needle.empty())
        return choices;
    for (const auto& type : palette)
    {
        if (lowered(type.displayName).find(needle) != std::string::npos
            && std::ranges::find(choices, type.displayName) == choices.end())
        {
            choices.push_back(type.displayName);
        }
    }
    return choices;
}

PrototypeListClick prototypeListClick(const PrototypeListRow& row)
{
    if (row.nodeIndex.has_value())
        return {row.nodeIndex, std::nullopt};
    return {std::nullopt, row.category};
}

std::vector<PrototypePaletteRow> prototypePaletteRows(
    const std::vector<talas::core::NodeTypeDescriptor>& palette, const std::vector<std::size_t>& matches)
{
    std::vector<PrototypePaletteRow> rows;
    for (const auto index : matches)
    {
        if (index >= palette.size())
            continue;
        rows.push_back({index, palette[index].category});
        for (const auto also : prototypeAlsoListedIn(palette[index].typeId))
        {
            if (also != palette[index].category)
                rows.push_back({index, std::string{also}});
        }
    }
    std::ranges::stable_sort(rows, [&](const PrototypePaletteRow& left, const PrototypePaletteRow& right) {
        if (left.category != right.category)
            return left.category < right.category;
        return palette[left.index].displayName < palette[right.index].displayName;
    });
    return rows;
}

bool isPrototypeAssembly(std::string_view typeId) noexcept
{
    return typeId == arDragAndDropPrototypeId;
}

std::optional<talas::core::NodeTypeDescriptor> prototypeAssemblyDescriptor(
    const talas::core::NodeRegistry& registry, std::string_view prototypeId)
{
    if (!isPrototypeAssembly(prototypeId))
        return std::nullopt;
    const auto* head = registry.findNodeType("talas.vca");
    const auto* envelope = registry.findNodeType("talas.ar");
    if (head == nullptr || envelope == nullptr)
        return std::nullopt;
    auto descriptor = *head;
    descriptor.typeId = std::string{prototypeId};
    descriptor.displayName = "AR Drag&Drop";
    descriptor.category = envelope->category;
    return descriptor;
}

namespace
{
// A field as a prototype comes with it, its default.
talas::project::ProjectParameterExpression prototypeParameter(const talas::core::ParameterDescriptor& descriptor)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = descriptor.id;
    switch (descriptor.kind)
    {
    case talas::core::ParameterKind::Expression:
        parameter.expression = descriptor.defaultExpression;
        break;
    case talas::core::ParameterKind::Choice:
        parameter.choice = descriptor.defaultChoice;
        break;
    case talas::core::ParameterKind::Boolean:
        parameter.boolean = descriptor.defaultBoolean.value_or(false);
        break;
    case talas::core::ParameterKind::Resource:
        parameter.resource = descriptor.defaultResource;
        break;
    case talas::core::ParameterKind::Text:
        parameter.text = descriptor.defaultText;
        break;
    }
    return parameter;
}
}

std::optional<PrototypeAssembly> prototypeAssembly(const talas::core::NodeRegistry& registry,
    std::string_view prototypeId, const std::function<bool(std::string_view)>& taken)
{
    if (!isPrototypeAssembly(prototypeId))
        return std::nullopt;
    const auto* headType = registry.findNodeType("talas.vca");
    const auto* envelopeType = registry.findNodeType("talas.ar");
    if (headType == nullptr || envelopeType == nullptr)
        return std::nullopt;
    // Each Sound as the Prototypes make it: its fields at their defaults.
    const auto made = [&](const talas::core::NodeTypeDescriptor& type, std::string_view name) {
        talas::project::ProjectNode node;
        node.instanceId = numberedCopyName(name, taken);
        node.typeId = type.typeId;
        for (const auto& parameter : type.parameters)
        {
            if (parameter.kind == talas::core::ParameterKind::Expression && parameter.defaultExpression.empty())
                continue;
            node.parameters.push_back(prototypeParameter(parameter));
        }
        return node;
    };
    PrototypeAssembly assembly;
    assembly.head = made(*headType, "arDragDrop");
    assembly.headInput = "audioIn";
    const auto headName = assembly.head.instanceId;
    assembly.source = made(*envelopeType, "ar");
    if (assembly.source.instanceId == headName)
        assembly.source.instanceId = numberedCopyName("ar", [&](std::string_view name) { return taken(name) || name == headName; });
    assembly.sourceOutput = "value";
    assembly.headControl = "gain";
    // The AR comes triggered by !KeyDown, its amplitude
    // !KeyVelocity.
    for (auto& parameter : assembly.source.parameters)
    {
        if (parameter.parameterId == "gate")
            parameter.expression = "!KeyDown";
        else if (parameter.parameterId == "scale")
            parameter.expression = "!KeyVelocity";
    }
    return assembly;
}

std::optional<talas::project::ProjectDocument> documentWithPrototypeAssembly(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view prototypeId, std::optional<std::string_view> definitionId,
    talas::project::CanvasPosition position, const std::optional<talas::core::Connection>& line, std::string& error,
    std::string* headId)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
        {
            error = "Unknown Sound definition: " + std::string{*definitionId};
            return std::nullopt;
        }
        graph = &definition->graph;
    }
    auto assembly = prototypeAssembly(registry, prototypeId, [&](std::string_view name) {
        return std::ranges::any_of(
            graph->nodes, [&](const talas::project::ProjectNode& node) { return node.instanceId == name; });
    });
    if (!assembly.has_value())
    {
        error = "Unknown prototype cannot be added";
        return std::nullopt;
    }
    talas::project::ProjectEditor editor(document, registry);
    const auto head = assembly->head.instanceId;
    const auto source = assembly->source.instanceId;
    const talas::project::CanvasPosition sourcePosition{position.x, position.y + 150};
    const auto placed = line.has_value()
        ? editor.insertNodeOnConnection(definitionId, std::move(assembly->head), position, *line, assembly->headInput)
        : definitionId.has_value() ? editor.addNodeToSoundDefinition(*definitionId, std::move(assembly->head), position)
                                   : editor.addNode(std::move(assembly->head), position);
    if (!placed.ok())
    {
        error = placed.message;
        return std::nullopt;
    }
    const auto added = definitionId.has_value()
        ? editor.addNodeToSoundDefinition(*definitionId, std::move(assembly->source), sourcePosition)
        : editor.addNode(std::move(assembly->source), sourcePosition);
    const talas::core::Connection envelope{source, assembly->sourceOutput, head, assembly->headControl};
    const auto connected = !added.ok() ? added
        : definitionId.has_value()     ? editor.connectInSoundDefinition(*definitionId, envelope)
                                       : editor.connect(envelope);
    if (!connected.ok())
    {
        error = connected.message;
        return std::nullopt;
    }
    if (headId != nullptr)
        *headId = head;
    return editor.document();
}

std::vector<talas::core::NodeTypeDescriptor> prototypePalette(const talas::core::NodeRegistry& registry)
{
    auto types = registry.nodeTypes();
    // Legacy nodes stay compilable but the palette offers their
    // replacements.
    std::erase_if(types, [](const auto& type) { return !type.replacedBy.empty(); });
    // AR Drag&Drop among the envelopes.
    if (auto assembly = prototypeAssemblyDescriptor(registry, arDragAndDropPrototypeId))
        types.push_back(std::move(*assembly));
    std::ranges::sort(types, [](const auto& left, const auto& right) {
        if (left.category != right.category)
            return left.category < right.category;
        if (left.displayName != right.displayName)
            return left.displayName < right.displayName;
        return left.typeId < right.typeId;
    });
    return types;
}
}
