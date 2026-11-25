#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/MidiDefaultMap.h"
#include "talas/project/ProjectEventValueSpelling.h"

#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectMidiMapperMap.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/core/ValScalarArray.h"
#include "talas/core/IntervalCollectArray.h"
#include "talas/core/AnalogSequencerArray.h"
#include "talas/core/AnalogSequencerExtraValues.h"

#include "talas/expr/Expression.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
constexpr std::size_t maximumDomainSearchAttempts = 100000;
constexpr std::size_t maximumInferredEventValues = 64;
constexpr std::size_t numericWitnessCount = 4;

using DomainMap = std::unordered_map<std::string, talas::expr::ValueDomain>;
using VariableValueMap =
    std::unordered_map<std::string, talas::expr::ExpressionValue>;

struct DomainResolutionContext
{
    const DomainMap* domains = nullptr;
    const DomainMap* localDomains = nullptr;
    const VariableValueMap* variableValues = nullptr;
    std::size_t witness = 0;
};

struct ExpressionConstraint
{
    talas::expr::Expression expression;
    talas::expr::ValueDomain expectedDomain =
        talas::expr::ValueDomain::Scalar;
    std::vector<std::string> references;
    std::string source;
    std::string nodeId;
    std::string parameterId;
    DomainMap localDomains;
    VariableValueMap variableValues;
};

[[nodiscard]] const ProjectParameterExpression* findParameter(
    const ProjectNode& node,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        node.parameters,
        [&](const auto& parameter) {
            return parameter.parameterId == parameterId;
        });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectSoundDefinition* findDefinition(
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

[[nodiscard]] DomainMap localEventDomains(
    const ProjectSoundDefinition& definition)
{
    DomainMap domains;
    for (const auto& exposed : definition.exposedParameters)
    {
        for (const auto& binding : exposed.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::EventValue)
                domains.emplace(binding.eventName, binding.eventDomain);
        }
    }
    return domains;
}

// The keyboard's Event Values, and the MIDI time code, which no Virtual
// Control sets; !KeyPitch12 is one since format 116.
[[nodiscard]] bool isBuiltinKeyboardEvent(const ProjectDocument& document, std::string_view name)
{
    const auto val = document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1;
    if (name == "KeyPitch12")
        return val && !document.keyPitchIgnoresPitchBend;
    // The signal processor's !Random and !Random1000 since format 117.
    if (name == "Random" || name == "Random1000")
        return val && !document.randomEventSourcesAreFaders;
    // The whole Sound's clocks, !RealTime and !Time, since
    // format 122.
    if (name == "RealTime" || name == "Time")
        return val && !document.timeEventSourcesAreFaders;
    return name == "Pitch" || name == "KeyPitch" || name == "KeyNumber"
        || name == "KeyVelocity" || name == "KeyTimbre"
        || name == "KeyDown" || name == "MIDITimeCode";
}

[[nodiscard]] double representativeValue(
    talas::expr::ValueDomain domain,
    const DomainMap& domains,
    std::string_view name,
    std::size_t witness)
{
    const auto ordinal = static_cast<std::size_t>(std::ranges::count_if(
        domains,
        [&](const auto& entry) {
            return std::string_view{entry.first} < name;
        }));
    const auto rank =
        static_cast<double>(ordinal + 1)
        / static_cast<double>(domains.size() + 1);
    constexpr std::array lowerBounds = {0.05, 0.1, 0.15, 0.2};
    constexpr std::array widths = {0.9, 0.8, 0.7, 0.6};
    const auto normalized =
        lowerBounds[witness] + widths[witness] * rank;
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
        return 0.1 + normalized * 0.8;
    case talas::expr::ValueDomain::Hertz:
        return 100.0 + normalized * 900.0;
    case talas::expr::ValueDomain::Seconds:
        return 0.1 + normalized * 1.9;
    case talas::expr::ValueDomain::NoteNumber:
        return 36.0 + normalized * 48.0;
    case talas::expr::ValueDomain::Samples:
        return 64.0 + normalized * 4032.0;
    }
    return normalized;
}

bool resolveDomainValue(
    void* user,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    const auto* resolution =
        static_cast<const DomainResolutionContext*>(user);
    if (resolution == nullptr || resolution->domains == nullptr)
        return false;
    auto domain = std::optional<talas::expr::ValueDomain>{};
    if (resolution->localDomains != nullptr)
    {
        const auto local = resolution->localDomains->find(
            std::string(name));
        if (local != resolution->localDomains->end())
            domain = local->second;
    }
    if (!domain.has_value())
    {
        const auto global = resolution->domains->find(
            std::string(name));
        if (global != resolution->domains->end())
            domain = global->second;
    }
    if (!domain.has_value())
        return false;
    output = {
        representativeValue(
            *domain,
            *resolution->domains,
            name,
            resolution->witness),
        *domain,
    };
    return true;
}

bool resolveDomainVariable(
    void* user,
    std::string_view name,
    talas::expr::ExpressionValue& out) noexcept
{
    const auto* resolution =
        static_cast<const DomainResolutionContext*>(user);
    if (resolution == nullptr || resolution->variableValues == nullptr)
        return false;
    const auto found = std::ranges::find_if(
        *resolution->variableValues,
        [&](const auto& entry) { return entry.first == name; });
    if (found == resolution->variableValues->end())
        return false;
    out = found->second;
    return true;
}

[[nodiscard]] bool evaluateConstraint(
    const ExpressionConstraint& constraint,
    const DomainMap& domains)
{
    for (std::size_t witness = 0;
         witness < numericWitnessCount;
         ++witness)
    {
        DomainResolutionContext resolution{
            &domains,
            &constraint.localDomains,
            &constraint.variableValues,
            witness};
        talas::expr::EvaluationContext context;
        context.sampleRate = 48000.0;
        context.tempoBpm = 120.0;
        context.user = &resolution;
        context.resolveEvent = resolveDomainValue;
        context.resolveVariable = resolveDomainVariable;
        talas::expr::ExpressionValue value;
        bool evaluated = false;
        if (constraint.expression.isStateful())
        {
            talas::expr::ExpressionState state;
            evaluated = talas::expr::prepareExpressionState(
                            constraint.expression, state)
                && talas::expr::tryEvaluateStateful(
                    constraint.expression,
                    context,
                    state,
                    {constraint.expectedDomain},
                    value);
        }
        else
        {
            evaluated = talas::expr::tryEvaluate(
                constraint.expression,
                context,
                {constraint.expectedDomain},
                value);
        }
        if (evaluated)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool allReferencesAssigned(
    const ExpressionConstraint& constraint,
    const DomainMap& domains)
{
    return std::ranges::all_of(
        constraint.references,
        [&](const auto& reference) {
            return domains.contains(reference)
                || constraint.localDomains.contains(reference);
        });
}

[[nodiscard]] bool containsExplicitDomainUnit(std::string_view source)
{
    constexpr std::array units = {
        std::string_view{" hz"},
        std::string_view{" ms"},
        std::string_view{" s"},
        std::string_view{" nn"},
        std::string_view{" samples"},
        std::string_view{" beats"},
    };
    return std::ranges::any_of(
        units,
        [&](std::string_view unit) { return source.find(unit) != source.npos; });
}

[[nodiscard]] std::array<talas::expr::ValueDomain, 5> candidateDomains(
    std::string_view eventName,
    std::span<const ExpressionConstraint> constraints)
{
    constexpr std::array defaultOrder = {
        talas::expr::ValueDomain::Scalar,
        talas::expr::ValueDomain::Hertz,
        talas::expr::ValueDomain::Seconds,
        talas::expr::ValueDomain::NoteNumber,
        talas::expr::ValueDomain::Samples,
    };
    auto preferred = talas::expr::ValueDomain::Scalar;
    for (const auto& constraint : constraints)
    {
        if (constraint.expectedDomain == talas::expr::ValueDomain::Scalar
            || containsExplicitDomainUnit(constraint.source)
            || std::ranges::find(constraint.references, eventName)
                == constraint.references.end())
        {
            continue;
        }
        preferred = constraint.expectedDomain;
        break;
    }
    if (preferred == talas::expr::ValueDomain::Scalar)
        return defaultOrder;

    auto ordered = defaultOrder;
    const auto found = std::ranges::find(ordered, preferred);
    std::rotate(ordered.begin(), found, found + 1);
    return ordered;
}

void addUnique(
    std::vector<std::string>& values,
    std::string_view value)
{
    if (std::ranges::find(values, value) == values.end())
        values.emplace_back(value);
}

struct PrefixerDiscoveryView
{
    ProjectSoundExpansionResult expanded;
    std::unordered_set<std::string> instantiatedDefinitions;
    std::unordered_map<std::string, std::uint32_t> scopeByNode;
};

[[nodiscard]] std::optional<PrefixerDiscoveryView>
makePrefixerDiscoveryView(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    const auto hasNamingWrapper = [](const ProjectGraph& graph) {
        return std::ranges::any_of(graph.nodes, [](const auto& node) {
            return node.typeId == "talas.prefixer"
                || node.typeId == "talas.replicator"
                || node.typeId == "talas.stereoizer"
                || node.typeId == "talas.surroundifer";
        });
    };
    const bool rootHasNamingWrapper = hasNamingWrapper(document.graph);
    const bool rootHasSoundInstance = std::ranges::any_of(
        document.graph.nodes,
        [](const auto& node) { return node.soundDefinitionId.has_value(); });
    if (!rootHasNamingWrapper && !rootHasSoundInstance
        && std::ranges::none_of(
            document.soundDefinitions,
            [&](const auto& definition) {
                return hasNamingWrapper(definition.graph);
            }))
        return std::nullopt;

    PrefixerDiscoveryView view;
    view.expanded = expandProjectSounds(document, registry);
    if (!view.expanded.ok())
        return std::nullopt;
    for (const auto& scope : view.expanded.manifest.scopes)
        view.instantiatedDefinitions.insert(scope.definitionId);
    if (!rootHasNamingWrapper && !rootHasSoundInstance
        && std::ranges::none_of(
            document.soundDefinitions,
            [&](const auto& definition) {
                return view.instantiatedDefinitions.contains(
                    definition.definitionId)
                    && hasNamingWrapper(definition.graph);
            }))
        return std::nullopt;
    for (const auto& identity : view.expanded.manifest.nodes)
        view.scopeByNode.emplace(
            identity.flatNodeId, identity.scopeOrdinal);
    return view;
}

// Expansion prepares only the root nodes that reach an output. Unconnected
// root nodes are still authored and keep their Event Value controls.
[[nodiscard]] ProjectGraph unexpandedRootGraph(
    const ProjectDocument& document,
    const std::optional<PrefixerDiscoveryView>& view)
{
    ProjectGraph graph;
    if (!view.has_value())
        return graph;
    std::unordered_set<std::string> reachable;
    for (const auto& node : reachableProjectRootGraph(document).nodes)
        reachable.insert(node.instanceId);
    for (const auto& node : document.graph.nodes)
        if (!reachable.contains(node.instanceId))
            graph.nodes.push_back(node);
    for (const auto& connection : document.graph.connections)
        if (!reachable.contains(connection.toNode))
            graph.connections.push_back(connection);
    return graph;
}

[[nodiscard]] DomainMap localDomainsForScope(
    const PrefixerDiscoveryView& view,
    std::uint32_t scopeOrdinal)
{
    DomainMap domains;
    std::unordered_set<std::uint32_t> visited;
    while (scopeOrdinal != 0 && visited.insert(scopeOrdinal).second)
    {
        for (const auto& binding : view.expanded.manifest.eventValues)
            if (binding.scopeOrdinal == scopeOrdinal)
                domains.try_emplace(binding.eventName, binding.domain);
        const auto scope = std::ranges::find(
            view.expanded.manifest.scopes, scopeOrdinal,
            &ExpandedSoundScopeIdentity::scopeOrdinal);
        if (scope == view.expanded.manifest.scopes.end())
            break;
        scopeOrdinal = scope->parentScopeOrdinal;
    }
    return domains;
}

[[nodiscard]] std::vector<std::string> definitionScopeForScope(
    const PrefixerDiscoveryView& view,
    std::uint32_t scopeOrdinal)
{
    std::vector<std::string> result;
    std::unordered_set<std::uint32_t> visited;
    while (scopeOrdinal != 0 && visited.insert(scopeOrdinal).second)
    {
        const auto scope = std::ranges::find(
            view.expanded.manifest.scopes, scopeOrdinal,
            &ExpandedSoundScopeIdentity::scopeOrdinal);
        if (scope == view.expanded.manifest.scopes.end())
            break;
        result.push_back(scope->definitionId);
        scopeOrdinal = scope->parentScopeOrdinal;
    }
    std::ranges::reverse(result);
    return result;
}

[[nodiscard]] std::uint32_t parameterSourceScopeOrdinal(
    const PrefixerDiscoveryView& view,
    std::string_view flatNodeId,
    std::string_view parameterId,
    std::uint32_t fallbackScopeOrdinal)
{
    const auto binding = std::ranges::find_if(
        view.expanded.manifest.nodeParameters,
        [&](const auto& candidate) {
            return candidate.flatNodeId == flatNodeId
                && candidate.parameterId == parameterId;
        });
    return binding == view.expanded.manifest.nodeParameters.end()
        ? fallbackScopeOrdinal
        : binding->sourceScopeOrdinal;
}

[[nodiscard]] std::optional<talas::expr::ValueDomain>
analogSequencerArrayDomain(std::string_view parameterId)
{
    if (parameterId == "durations")
        return talas::expr::ValueDomain::Seconds;
    if (parameterId == "pitches" || parameterId == "keyPitches")
        return talas::expr::ValueDomain::NoteNumber;
    if (parameterId == "dutyCycles" || parameterId == "velocities"
        || parameterId == "keyDowns" || parameterId == "keyVelocities"
        || parameterId == "keyTimbres")
    {
        return talas::expr::ValueDomain::Scalar;
    }
    return std::nullopt;
}

// A field's expression, or the one a Smalltalk program in it gives as the
// Sound compiles, so that a hot value the
// program makes, as faderMaker's !fader06, has its control too.
[[nodiscard]] talas::expr::ParseResult parseFieldExpression(const ProjectDocument& document, std::string_view source)
{
    auto parsed = talas::expr::parse(source, document.expressionLanguage, document.expressionDefinitions);
    if (parsed.ok())
        return parsed;
    if (const auto program = smalltalk::fieldProgramText(
            source, !document.smalltalkNumbersAreFloats, document.expressionDefinitions.rounding))
    {
        auto given = talas::expr::parse(*program, document.expressionLanguage, document.expressionDefinitions);
        if (given.ok())
            return given;
    }
    return parsed;
}

[[nodiscard]] bool expressionReferencesEventValue(
    const ProjectDocument& document,
    std::string_view source,
    std::string_view eventName)
{
    const auto parsed = parseFieldExpression(document, source);
    return parsed.ok()
        && parsed.expression.has_value()
        && std::ranges::find(
               parsed.expression->eventReferences(), eventName)
            != parsed.expression->eventReferences().end();
}

// The list fields whose braced elements and bare Event Values the runtime
// makes live, each with the domain its elements take: a frequency or
// bandwidth in hertz, an interval in half steps, a time point or duration
// in seconds.
[[nodiscard]] std::vector<std::pair<std::string, talas::expr::ValueDomain>> liveListFields(const ProjectNode& node)
{
    using Domain = talas::expr::ValueDomain;
    std::vector<std::pair<std::string, Domain>> fields;
    if (node.typeId == "talas.matrix4" || node.typeId == "talas.matrix8")
    {
        for (const auto& parameter : node.parameters)
            if (parameter.parameterId.starts_with("insToOut"))
                fields.emplace_back(parameter.parameterId, Domain::Scalar);
    }
    else if (node.typeId == "talas.firFilter" || node.typeId == "talas.waveshaper")
        fields = {{"coefficients", Domain::Scalar}};
    else if (node.typeId == "talas.inputOutputCharacteristic")
        fields = {{"inValues", Domain::Scalar}, {"outValues", Domain::Scalar}};
    else if (node.typeId == "talas.syntheticSpectrumFromArray")
        fields = {{"amplitudes", Domain::Scalar}, {"frequencies", Domain::Hertz}, {"bandwidths", Domain::Hertz}};
    else if (node.typeId == "talas.scaleVocoder")
        fields = {{"intervals", Domain::NoteNumber}};
    else if (node.typeId == "talas.warpedTimeIndex")
        fields = {{"idealTimePoints0", Domain::Seconds}, {"idealTimePoints1", Domain::Seconds},
            {"currentTimePoints", Domain::Seconds}};
    else if (node.typeId == "talas.multisegmentEnvelope")
        fields = {{"durations", Domain::Seconds}, {"breakPoints", Domain::Scalar}};
    else if (node.typeId == "talas.multislopeFunctionGenerator")
        fields = {{"timePoints", Domain::Seconds}, {"slopes", Domain::Scalar}};
    return fields;
}

// A list field's elements that may read Event Values: the block's value at
// each step of a collected Interval, as {(1 to: 16) collect: [:i | !Amp
// suffix2: i]} gives, or else each braced element and
// each bare Event Value; none for a literal Array, which its own reader
// takes.
[[nodiscard]] std::vector<std::string> liveListElements(std::string_view text)
{
    if (talas::core::val_scalar_array::beginsArray(text))
        return {};
    if (const auto collected = talas::core::interval_collect_array::expand(text);
        collected.status == talas::core::interval_collect_array::Status::Expanded)
    {
        return collected.elements;
    }
    std::vector<std::string> elements;
    const auto separator = [](char value) { return value == ' ' || value == '\t' || value == '\n' || value == '\r'; };
    std::size_t offset = 0;
    while (offset < text.size())
    {
        while (offset < text.size() && separator(text[offset]))
            ++offset;
        if (offset == text.size())
            break;
        const auto start = offset;
        if (text[offset] == '{')
        {
            const auto close = text.find('}', offset);
            if (close == std::string_view::npos)
                return {};
            elements.emplace_back(text.substr(start + 1, close - start - 1));
            offset = close + 1;
            continue;
        }
        while (offset < text.size() && !separator(text[offset]))
            ++offset;
        if (text[start] == '!')
            elements.emplace_back(text.substr(start, offset - start));
    }
    return elements;
}

[[nodiscard]] bool liveListReferencesEventValue(
    const ProjectDocument& document, const ProjectNode& node, std::string_view eventName)
{
    return std::ranges::any_of(liveListFields(node), [&](const auto& field) {
        const auto* parameter = findParameter(node, field.first);
        return parameter != nullptr && parameter->text.has_value()
            && std::ranges::any_of(liveListElements(*parameter->text), [&](const std::string& element) {
                   return expressionReferencesEventValue(document, element, eventName);
               });
    });
}

[[nodiscard]] bool analogSequencerTextReferencesEventValue(
    const ProjectDocument& document,
    const ProjectNode& node,
    std::string_view eventName)
{
    for (const auto parameterId : {
             "durations", "dutyCycles", "pitches", "velocities",
             "keyDowns", "keyPitches", "keyVelocities", "keyTimbres"})
    {
        const auto* parameter = findParameter(node, parameterId);
        if (parameter == nullptr || !parameter->text.has_value())
            continue;
        const auto array = talas::core::parseAnalogSequencerArray(
            *parameter->text);
        if (array.has_value()
            && std::ranges::any_of(array->members, [&](const auto& member) {
                return expressionReferencesEventValue(
                    document, member.source, eventName);
            }))
        {
            return true;
        }
    }
    const auto* extraValues = findParameter(node, "extraValues");
    if (extraValues == nullptr || !extraValues->text.has_value())
        return false;
    const auto arrays = talas::core::parseAnalogSequencerExtraValues(
        *extraValues->text);
    return arrays.has_value()
        && std::ranges::any_of(*arrays, [&](const auto& array) {
            return std::ranges::any_of(
                array.expressions, [&](const auto& source) {
                    return expressionReferencesEventValue(
                        document, source, eventName);
                });
        });
}

}

VirtualControl makeDefaultVirtualControl(
    std::string eventName,
    talas::expr::ValueDomain domain,
    VirtualControlKind kind)
{
    VirtualControl control;
    control.eventName = std::move(eventName);
    control.domain = domain;
    control.kind = kind;
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
        control.value = 0.0;
        control.minimum = 0.0;
        control.maximum = 1.0;
        break;
    case talas::expr::ValueDomain::Hertz:
        control.value = 440.0;
        control.minimum = 20.0;
        control.maximum = 20000.0;
        break;
    case talas::expr::ValueDomain::Seconds:
        control.value = 0.0;
        control.minimum = 0.0;
        control.maximum = 10.0;
        break;
    case talas::expr::ValueDomain::NoteNumber:
        control.value = 60.0;
        control.minimum = 0.0;
        control.maximum = 127.0;
        break;
    case talas::expr::ValueDomain::Samples:
        control.value = 0.0;
        control.minimum = 0.0;
        control.maximum = 48000.0;
        break;
    }
    return control;
}

std::optional<std::pair<double, double>> documentedEventValueRange(std::string_view eventName, double sampleRate)
{
    std::string name;
    for (const auto character : eventName)
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    struct Range
    {
        std::string_view name;
        double minimum;
        double maximum;
    };
    // The table's names, and its names ending in * as the start of a name.
    constexpr std::array<Range, 34> names{{
        {"angle", -0.5, 1.5}, {"beats", 1, 16}, {"bpm", 0, 2000}, {"centervalue", -1, 1},
        {"cycles", 0, 100}, {"dcoffset", -1, 1}, {"direction", -1, 1}, {"fmntshift", 0.75, 1.25},
        {"freqjitter", 0, 1}, {"freqlow", 0, 1000}, {"freqmid", 1000, 8000}, {"freqhigh", 8000, 0},
        {"gain", 0, 10}, {"gaindb", -128, 128}, {"looplength", -1, 1}, {"logfreq", 0, 127},
        {"keynumber", 0, 127}, {"pitch", 0, 127}, {"offset", -1, 1}, {"onduration", 0, 30},
        {"panner", -0.5, 1.5}, {"radius", -2, 2}, {"distance", -2, 2}, {"rate", 0, 2},
        {"ratio", 0, 100}, {"q", 0, 10}, {"scale", -2, 2}, {"scale02", 0, 2}, {"steps", 1, 128},
        {"swing", 0, 0.5}, {"timeconstant", 0.0001, 5}, {"timeindex", -1, 1}, {"threshdb", -60, 0},
        {"upinterval", 0, 24},
    }};
    constexpr std::array<Range, 7> starts{{
        {"smallinterval", 0, 12}, {"boostcut", -12, 12}, {"interval", -24, 24}, {"cutoff", 0, 10000},
        {"coef", -1, 1}, {"tune", -1, 1}, {"freq", 0, 10000},
    }};
    const auto found = std::ranges::find(names, std::string_view{name}, &Range::name);
    if (found != names.end())
        return std::pair{found->minimum, found->name == "freqhigh" ? sampleRate * 0.5 : found->maximum};
    for (const auto& start : starts)
    {
        if (std::string_view{name}.starts_with(start.name))
            return std::pair{start.minimum, start.maximum};
    }
    return std::nullopt;
}

ProjectEventValueDiscoveryResult discoverProjectEventValueControls(
    const ProjectDocument& authored,
    const talas::core::NodeRegistry& registry,
    bool documentedRanges)
{
    // One fader however a Val name is cased,
    // named as the name was first seen.
    const auto unified = withEventValueSpellingsUnified(authored);
    const auto& document = unified.has_value() ? *unified : authored;
    const auto prefixerView = makePrefixerDiscoveryView(
        document, registry);
    const auto& rootGraph = prefixerView.has_value()
        ? prefixerView->expanded.document.graph
        : document.graph;
    const auto unexpandedRoot = unexpandedRootGraph(document, prefixerView);
    const auto definitionWasExpanded = [&](std::string_view id) {
        return prefixerView.has_value()
            && prefixerView->instantiatedDefinitions.contains(
                std::string{id});
    };

    ProjectEventValueDiscoveryResult result;
    DomainMap domains;

    const auto addKnownDomain = [&](std::string_view name,
                                    talas::expr::ValueDomain domain,
                                    std::string message) {
        const auto [found, inserted] =
            domains.emplace(std::string(name), domain);
        if (!inserted && found->second != domain)
        {
            result.diagnostics.push_back({
                {},
                {},
                std::string(name),
                std::move(message),
            });
        }
    };

    for (const auto& control : document.virtualControls)
    {
        addKnownDomain(
            control.eventName,
            control.domain,
            "Persisted Event Value controls use conflicting domains");
    }
    for (const auto& event : document.timeline.eventValues)
    {
        addKnownDomain(
            event.eventName,
            event.domain,
            "Timeline and persisted Event Value domains conflict");
    }
    addKnownDomain(
        "Pitch",
        talas::expr::ValueDomain::NoteNumber,
        "Pitch must use note-number domain");
    addKnownDomain(
        "KeyPitch",
        talas::expr::ValueDomain::NoteNumber,
        "KeyPitch must use note-number domain");
    if (document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        if (!document.keyPitchIgnoresPitchBend)
            addKnownDomain("KeyPitch12", talas::expr::ValueDomain::NoteNumber, "KeyPitch12 must use note-number domain");
        if (!document.randomEventSourcesAreFaders)
        {
            addKnownDomain("Random", talas::expr::ValueDomain::Scalar, "Random must use the scalar domain");
            addKnownDomain("Random1000", talas::expr::ValueDomain::Scalar, "Random1000 must use the scalar domain");
        }
        if (!document.timeEventSourcesAreFaders)
        {
            addKnownDomain("RealTime", talas::expr::ValueDomain::Scalar, "RealTime must use the scalar domain");
            addKnownDomain("Time", talas::expr::ValueDomain::Scalar, "Time must use the scalar domain");
        }
    }
    addKnownDomain(
        "KeyNumber",
        talas::expr::ValueDomain::NoteNumber,
        "KeyNumber must use note-number domain");
    addKnownDomain(
        "KeyVelocity",
        talas::expr::ValueDomain::Scalar,
        "KeyVelocity must use scalar domain");
    addKnownDomain(
        "KeyTimbre",
        talas::expr::ValueDomain::Scalar,
        "KeyTimbre must use scalar domain");
    addKnownDomain(
        "KeyDown",
        talas::expr::ValueDomain::Scalar,
        "KeyDown must use scalar domain");
    addKnownDomain(
        "MIDITimeCode",
        talas::expr::ValueDomain::Scalar,
        "MIDITimeCode must use scalar domain");
    const auto addMapperKeyboardDomains = [&](const ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            if (node.typeId != "talas.midiMapper")
                continue;
            const auto* map = findParameter(node, "map");
            if (map == nullptr || !map->text.has_value())
                continue;
            const auto parsed = parseProjectMidiMapperMap(*map->text);
            if (!parsed.ok())
                continue;
            for (std::size_t index = 0;
                 index < parsed.localAssociationCount; ++index)
            {
                const auto& association = parsed.localAssociations[index];
                if (association.localEventSource != "MIDIKeyDown"
                    && association.localEventSource != "MIDIKeyNumber"
                    && association.localEventSource != "MIDIKeyVelocity")
                {
                    continue;
                }
                addKnownDomain(
                    association.localEventValue,
                    association.localEventSource == "MIDIKeyNumber"
                        ? talas::expr::ValueDomain::NoteNumber
                        : talas::expr::ValueDomain::Scalar,
                    "MIDIMapper keyboard Map target has a conflicting Event Value domain");
            }
        }
    };
    addMapperKeyboardDomains(rootGraph);
    addMapperKeyboardDomains(unexpandedRoot);
    for (const auto& definition : document.soundDefinitions)
        if (!definitionWasExpanded(definition.definitionId))
            addMapperKeyboardDomains(definition.graph);

    std::vector<ExpressionConstraint> constraints;
    std::vector<std::string> firstReferences;
    const auto addExpressionConstraint = [&] (
                                             std::string_view source,
                                             talas::expr::ValueDomain expectedDomain,
                                             std::string diagnosticNodeId,
                                             std::string parameterId,
                                             const DomainMap& localDomains,
                                             const std::vector<std::string>&
                                                 definitionScope) {
        if (source.empty())
            return;
        const auto parsed = parseFieldExpression(document, source);
        if (!parsed.ok()
            || !parsed.expression.has_value()
            || parsed.expression->eventReferences().empty())
        {
            return;
        }
        if (!parsed.expression->identifiers().empty())
        {
            result.diagnostics.push_back({
                std::move(diagnosticNodeId),
                std::move(parameterId),
                parsed.expression->eventReferences().front(),
                "Cannot infer Event Value domains in an expression with unresolved identifiers",
            });
            return;
        }

        std::vector<std::string_view> scopeViews;
        scopeViews.reserve(definitionScope.size());
        for (const auto& definition : definitionScope)
            scopeViews.push_back(definition);
        VariableValueMap variableValues;
        for (const auto& reference :
             parsed.expression->variableReferences())
        {
            const auto* declaration =
                findVisibleProjectVariableDeclaration(
                    document, reference, scopeViews);
            if (declaration == nullptr)
            {
                result.diagnostics.push_back({
                    std::move(diagnosticNodeId),
                    std::move(parameterId),
                    parsed.expression->eventReferences().front(),
                    "Cannot infer Event Value domains because a project variable reference has no visible declaration",
                });
                return;
            }
            variableValues.try_emplace(
                reference,
                talas::expr::ExpressionValue{
                    declaration->initialValue,
                    declaration->domain});
        }

        ExpressionConstraint constraint{
            *parsed.expression,
            expectedDomain,
            parsed.expression->eventReferences(),
            std::string(source),
            std::move(diagnosticNodeId),
            std::move(parameterId),
            localDomains,
            std::move(variableValues),
        };
        for (const auto& reference : constraint.references)
        {
            if (!isBuiltinKeyboardEvent(document, reference)
                && !localDomains.contains(reference))
            {
                addUnique(firstReferences, reference);
            }
        }
        constraints.push_back(std::move(constraint));
    };
    const auto collectGraphConstraints =
        [&](const ProjectGraph& graph,
            std::string_view definitionId,
            const DomainMap& localDomains,
            const PrefixerDiscoveryView* expandedView) {
        for (const auto& node : graph.nodes)
        {
            DomainMap expandedLocalDomains;
            const DomainMap* nodeLocalDomains = &localDomains;
            std::vector<std::string> definitionScope;
            std::uint32_t nodeScopeOrdinal = 0;
            if (expandedView != nullptr)
            {
                const auto scope = expandedView->scopeByNode.find(
                    node.instanceId);
                if (scope != expandedView->scopeByNode.end())
                {
                    nodeScopeOrdinal = scope->second;
                    expandedLocalDomains = localDomainsForScope(
                        *expandedView, scope->second);
                    nodeLocalDomains = &expandedLocalDomains;
                    definitionScope = definitionScopeForScope(
                        *expandedView, scope->second);
                }
            }
            else if (!definitionId.empty())
            {
                definitionScope.emplace_back(definitionId);
            }
            const auto diagnosticNodeId =
                definitionId.empty()
                ? node.instanceId
                : std::string(definitionId) + "/"
                    + node.instanceId;
            if (node.soundDefinitionId.has_value())
            {
                const auto* sound = findDefinition(
                    document,
                    *node.soundDefinitionId);
                if (sound == nullptr)
                    continue;
                for (const auto& parameter : node.parameters)
                {
                    const auto exposed = std::ranges::find_if(
                        sound->exposedParameters,
                        [&](const auto& candidate) {
                            return candidate.parameterId
                                    == parameter.parameterId
                                || std::ranges::find(
                                       candidate.aliases,
                                       parameter.parameterId)
                                    != candidate.aliases.end();
                        });
                    if (exposed == sound->exposedParameters.end()
                        || exposed->kind
                            != talas::core::ParameterKind::Expression
                        || !parameter.expression.has_value())
                    {
                        continue;
                    }
                    addExpressionConstraint(
                        *parameter.expression,
                        exposed->domain,
                        diagnosticNodeId,
                        exposed->parameterId,
                        *nodeLocalDomains,
                        definitionScope);
                }
                continue;
            }

            const auto* descriptor =
                registry.findNodeType(node.typeId);
            if (descriptor == nullptr)
                continue;
            if (node.typeId == "talas.analogSequencer")
            {
                const auto* parameter = findParameter(node, "generatedIndex");
                if (parameter != nullptr && parameter->text.has_value())
                {
                    const auto target =
                        talas::core::parseAnalogSequencerGeneratedIndex(
                            *parameter->text);
                    if (target.has_value() && !target->empty())
                    {
                        const auto local = nodeLocalDomains->find(*target);
                        if (local != nodeLocalDomains->end())
                        {
                            if (local->second
                                != talas::expr::ValueDomain::Scalar)
                            {
                                result.diagnostics.push_back({
                                    diagnosticNodeId,
                                    "generatedIndex",
                                    *target,
                                    "AnalogSequencer GeneratedIndex target conflicts with a local Event Value domain",
                                });
                            }
                        }
                        else
                        {
                            addKnownDomain(
                                *target,
                                talas::expr::ValueDomain::Scalar,
                                "AnalogSequencer GeneratedIndex target must use scalar domain");
                            addUnique(firstReferences, *target);
                        }
                    }
                }
            }
            if (node.typeId == "talas.inputOutputCharacteristic")
            {
                for (const auto parameterId : {"inValues", "outValues"})
                {
                    const auto* parameter = findParameter(node, parameterId);
                    if (parameter == nullptr || !parameter->text.has_value())
                        continue;
                    const auto members =
                        talas::core::val_scalar_array::parse(
                            *parameter->text);
                    if (!members.has_value())
                        continue;
                    for (const auto& member : *members)
                        if (!member.eventName.empty())
                            addExpressionConstraint(
                                "!" + member.eventName,
                                talas::expr::ValueDomain::Scalar,
                                diagnosticNodeId,
                                parameterId,
                                *nodeLocalDomains,
                                definitionScope);
                }
            }
            for (const auto& [parameterId, domain] : liveListFields(node))
            {
                const auto* parameter = findParameter(node, parameterId);
                if (parameter == nullptr || !parameter->text.has_value())
                    continue;
                for (const auto& element : liveListElements(*parameter->text))
                    addExpressionConstraint(element, domain, diagnosticNodeId, parameterId, *nodeLocalDomains,
                        definitionScope);
            }
            if (node.typeId == "talas.analogSequencer")
            {
                for (const auto parameterId : {
                         "durations", "dutyCycles", "pitches",
                         "velocities", "keyDowns", "keyPitches",
                         "keyVelocities", "keyTimbres"})
                {
                    const auto* parameter = findParameter(node, parameterId);
                    if (parameter == nullptr || !parameter->text.has_value())
                        continue;
                    const auto expectedDomain =
                        analogSequencerArrayDomain(parameterId);
                    const auto array = talas::core::parseAnalogSequencerArray(
                        *parameter->text);
                    if (!expectedDomain.has_value() || !array.has_value())
                        continue;
                    for (const auto& member : array->members)
                        addExpressionConstraint(
                            member.source,
                            *expectedDomain,
                            diagnosticNodeId,
                            parameterId,
                            *nodeLocalDomains,
                            definitionScope);
                }

                const auto* extraValues = findParameter(node, "extraValues");
                if (extraValues != nullptr && extraValues->text.has_value())
                {
                    const auto arrays =
                        talas::core::parseAnalogSequencerExtraValues(
                            *extraValues->text);
                    if (arrays.has_value())
                        for (const auto& array : *arrays)
                            for (const auto& member : array.expressions)
                                addExpressionConstraint(
                                    member,
                                    talas::expr::ValueDomain::Scalar,
                                    diagnosticNodeId,
                                    "extraValues",
                                    *nodeLocalDomains,
                                    definitionScope);
                }
            }
            for (const auto& parameterDescriptor : descriptor->parameters)
            {
                if (parameterDescriptor.kind
                    != talas::core::ParameterKind::Expression)
                {
                    continue;
                }
                const auto* parameter =
                    findParameter(node, parameterDescriptor.id);
                const auto& source =
                    parameter != nullptr
                        && parameter->expression.has_value()
                    ? *parameter->expression
                    : parameterDescriptor.defaultExpression;
                DomainMap expressionLocalDomains;
                std::vector<std::string> expressionDefinitionScope;
                const DomainMap* expressionDomains = nodeLocalDomains;
                const std::vector<std::string>* expressionDefinitions =
                    &definitionScope;
                if (expandedView != nullptr)
                {
                    const auto sourceScopeOrdinal =
                        parameterSourceScopeOrdinal(
                            *expandedView,
                            node.instanceId,
                            parameterDescriptor.id,
                            nodeScopeOrdinal);
                    expressionLocalDomains = localDomainsForScope(
                        *expandedView, sourceScopeOrdinal);
                    expressionDefinitionScope = definitionScopeForScope(
                        *expandedView, sourceScopeOrdinal);
                    expressionDomains = &expressionLocalDomains;
                    expressionDefinitions = &expressionDefinitionScope;
                }
                addExpressionConstraint(
                    source,
                    parameterDescriptor.domain,
                    diagnosticNodeId,
                    parameterDescriptor.id,
                    *expressionDomains,
                    *expressionDefinitions);
            }
            // A SoundToGlobalController's
            // GeneratedEvent shows on the surface, its fader displaying the
            // result (!Result).
            if (node.typeId == "talas.soundToGlobalController" || node.typeId == "talas.triggeredSoundToGlobalController")
            {
                const auto* generated = findParameter(node, "generatedEvent");
                if (generated != nullptr && generated->text.has_value() && generated->text->find('!') != std::string::npos)
                {
                    addExpressionConstraint(*generated->text, talas::expr::ValueDomain::Scalar, diagnosticNodeId,
                        "generatedEvent", *nodeLocalDomains, definitionScope);
                }
            }
            // A MIDIOutputController's Value text
            // may read Event Values, as the prototype's !Amp does.
            if (node.typeId == "talas.midiOutputController" || node.typeId == "talas.midiOutputControllerTap")
            {
                const auto* means = findParameter(node, "valueMeans");
                const auto* value = findParameter(node, "value");
                const auto declared = std::ranges::find(descriptor->parameters, std::string_view{"value"},
                    &talas::core::ParameterDescriptor::id);
                const auto text = value != nullptr && value->text.has_value() ? *value->text
                    : declared != descriptor->parameters.end()              ? declared->defaultText
                                                                            : std::string{};
                if ((means == nullptr || means->choice != std::optional<std::string>{"legacyControllerValue"})
                    && text.find('!') != std::string::npos)
                {
                    addExpressionConstraint(text, talas::expr::ValueDomain::Scalar, diagnosticNodeId, "value",
                        *nodeLocalDomains, definitionScope);
                }
            }
        }
    };
    collectGraphConstraints(
        rootGraph, {}, {},
        prefixerView.has_value() ? &*prefixerView : nullptr);
    collectGraphConstraints(unexpandedRoot, {}, {}, nullptr);
    if (prefixerView.has_value())
        for (const auto& binding :
             prefixerView->expanded.manifest.eventValues)
            if (binding.value.expression.has_value())
            {
                const auto scope = definitionScopeForScope(
                    *prefixerView, binding.sourceScopeOrdinal);
                addExpressionConstraint(
                    *binding.value.expression,
                    binding.domain,
                    binding.sourcePath,
                    binding.value.parameterId,
                    localDomainsForScope(
                        *prefixerView, binding.sourceScopeOrdinal),
                    scope);
            }
    for (const auto& definition : document.soundDefinitions)
    {
        if (definitionWasExpanded(definition.definitionId))
            continue;
        const auto localDomains = localEventDomains(definition);
        collectGraphConstraints(
            definition.graph,
            definition.definitionId,
            localDomains,
            nullptr);
        for (const auto& exposed : definition.exposedParameters)
        {
            if (exposed.kind == talas::core::ParameterKind::Expression
                && exposed.defaultValue.expression.has_value())
            {
                addExpressionConstraint(
                    *exposed.defaultValue.expression,
                    exposed.domain,
                    definition.definitionId,
                    exposed.parameterId,
                    {},
                    {definition.definitionId});
            }
        }
    }

    for (const auto& reference : firstReferences)
        if (parseBuiltInMidiControllerName(reference))
            addKnownDomain(reference, talas::expr::ValueDomain::Scalar,
                "Built-in MIDI controller Event Values must be scalar");

    if (!result.diagnostics.empty())
        return result;

    std::vector<std::string> unknowns;
    for (const auto& reference : firstReferences)
    {
        if (!domains.contains(reference))
            unknowns.push_back(reference);
    }
    if (unknowns.size() > maximumInferredEventValues)
    {
        result.diagnostics.push_back({
            constraints.empty() ? std::string{} : constraints.front().nodeId,
            constraints.empty()
                ? std::string{}
                : constraints.front().parameterId,
            unknowns.front(),
            "Event Value domain inference exceeded its reference limit; add authoritative controls manually",
        });
        return result;
    }

    std::size_t attempts = 0;
    bool budgetExceeded = false;
    const auto search = [&](const auto& self, std::size_t index) -> bool {
        if (index == unknowns.size())
        {
            return std::ranges::all_of(
                constraints,
                [&](const auto& constraint) {
                    return evaluateConstraint(constraint, domains);
                });
        }

        const auto& name = unknowns[index];
        for (const auto domain :
             candidateDomains(name, constraints))
        {
            if (++attempts > maximumDomainSearchAttempts)
            {
                budgetExceeded = true;
                return false;
            }
            domains[name] = domain;
            const auto assignedConstraintsValid = std::ranges::all_of(
                constraints,
                [&](const auto& constraint) {
                    return !allReferencesAssigned(constraint, domains)
                        || evaluateConstraint(constraint, domains);
                });
            if (assignedConstraintsValid && self(self, index + 1))
                return true;
        }
        domains.erase(name);
        return false;
    };

    if (!search(search, 0))
    {
        const auto* constraint =
            constraints.empty() ? nullptr : &constraints.front();
        result.diagnostics.push_back({
            constraint != nullptr ? constraint->nodeId : std::string{},
            constraint != nullptr ? constraint->parameterId : std::string{},
            unknowns.empty() ? std::string{} : unknowns.front(),
            budgetExceeded
                ? "Event Value domain inference exceeded its bounded search budget; add authoritative controls manually"
                : "Event Value domains cannot satisfy all graph expressions",
        });
        return result;
    }

    const auto globalMap = projectGlobalMapEntries(document);
    for (const auto& reference : firstReferences)
    {
        if (std::ranges::any_of(
                document.virtualControls,
                [&](const auto& control) {
                    return control.eventName == reference;
                }))
        {
            continue;
        }
        const auto domain = domains.find(reference);
        if (domain == domains.end())
            continue;
        const auto builtIn = parseBuiltInMidiControllerName(reference);
        // The global map shows its names as it displays them (ADR-0024): !PitchBend from -1 to 1, a range of its
        // own, or nothing for #nothing.
        const auto mapped = std::ranges::find(globalMap, reference, &GlobalMapEntry::eventName);
        if (!builtIn && mapped != globalMap.end() && mapped->displayAs == MidiMapperMapDisplayAs::Nothing)
            continue;
        auto control = makeDefaultVirtualControl(
            reference, domain->second,
            builtIn ? builtIn->kind
                : mapped != globalMap.end() ? globalMapControlKind(*mapped)
                : VirtualControlKind::Fader);
        if (!builtIn && mapped != globalMap.end() && domain->second == talas::expr::ValueDomain::Scalar
            && control.kind == VirtualControlKind::Fader)
        {
            control.minimum = mapped->minimum;
            control.maximum = mapped->maximum;
            control.value = std::clamp(control.value, control.minimum, control.maximum);
        }
        // The pitch wheel's Event Source, `MIDIPitchBend, from -1 to 1 at
        // rest in the middle.
        else if (reference == "MIDIPitchBend" && domain->second == talas::expr::ValueDomain::Scalar
            && control.kind == VirtualControlKind::Fader)
        {
            control.minimum = -1.0;
            control.maximum = 1.0;
            control.value = 0.0;
        }
        // A fader of a name with a documented range takes it.
        else if (const auto range = documentedRanges && !builtIn && mapped == globalMap.end()
                     && domain->second == talas::expr::ValueDomain::Scalar && control.kind == VirtualControlKind::Fader
                ? documentedEventValueRange(reference) : std::nullopt)
        {
            control.minimum = range->first;
            control.maximum = range->second;
            control.value = std::clamp(control.value, control.minimum, control.maximum);
        }
        // A knob the map displays its controller as, or a
        // small fader, a numeric box that is a fader too.
        if (!builtIn && mapped != globalMap.end()
            && (mapped->displayAs == MidiMapperMapDisplayAs::Rotary || mapped->displayAs == MidiMapperMapDisplayAs::Pot))
            control.widget.style = VirtualControlWidgetStyle::Potentiometer;
        else if (!builtIn && mapped != globalMap.end() && mapped->displayAs == MidiMapperMapDisplayAs::SmallFader)
            control.widget.style = VirtualControlWidgetStyle::SmallFader;
        result.missingControls.push_back(std::move(control));
    }
    return result;
}

ProjectEventValueSyncResult synchronizeProjectEventValueControls(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    auto discovered = discoverProjectEventValueControls(document, registry, true);
    ProjectEventValueSyncResult result;
    result.diagnostics = std::move(discovered.diagnostics);
    if (!result.ok())
        return result;
    const auto firstAdded = document.virtualControls.size();
    result.addedCount = discovered.missingControls.size();
    // A dormant widget wakes on the control of its name.
    for (auto& control : discovered.missingControls)
        (void)wakeDormantVirtualControl(document.dormantVirtualControls, control);
    document.virtualControls.insert(
        document.virtualControls.end(),
        std::make_move_iterator(discovered.missingControls.begin()),
        std::make_move_iterator(discovered.missingControls.end()));
    reconcileProjectPresentation(document);
    for (std::size_t index = firstAdded;
         index < document.virtualControls.size();
         ++index)
    {
        const auto& control = document.virtualControls[index];
        findProjectPerformanceControlPresentation(
            document,
            control.eventName,
            control.domain)->position = {
                static_cast<std::int32_t>(index * 174),
                0,
            };
    }
    return result;
}

bool projectReferencesEventValue(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view eventName)
{
    const auto prefixerView = makePrefixerDiscoveryView(
        document, registry);
    const auto& rootGraph = prefixerView.has_value()
        ? prefixerView->expanded.document.graph
        : document.graph;
    const auto graphReferences =
        [&](const ProjectGraph& graph,
            const DomainMap& localDomains,
            const PrefixerDiscoveryView* expandedView) {
        for (const auto& node : graph.nodes)
        {
            std::uint32_t nodeScopeOrdinal = 0;
            bool ownerScopeShadows =
                expandedView == nullptr
                    && localDomains.contains(std::string(eventName));
            if (expandedView != nullptr)
            {
                const auto scope = expandedView->scopeByNode.find(
                    node.instanceId);
                if (scope != expandedView->scopeByNode.end())
                {
                    nodeScopeOrdinal = scope->second;
                    ownerScopeShadows = localDomainsForScope(
                        *expandedView, scope->second)
                        .contains(std::string(eventName));
                }
            }
            if (node.soundDefinitionId.has_value())
            {
                if (ownerScopeShadows)
                    continue;
                const auto* sound = findDefinition(
                    document,
                    *node.soundDefinitionId);
                if (sound == nullptr)
                    continue;
                for (const auto& parameter : node.parameters)
                {
                    if (!parameter.expression.has_value())
                        continue;
                    const auto exposed = std::ranges::find_if(
                        sound->exposedParameters,
                        [&](const auto& candidate) {
                            return candidate.kind
                                    == talas::core::ParameterKind::Expression
                                && (candidate.parameterId
                                        == parameter.parameterId
                                    || std::ranges::find(
                                           candidate.aliases,
                                           parameter.parameterId)
                                        != candidate.aliases.end());
                        });
                    if (exposed == sound->exposedParameters.end())
                        continue;
                    const auto parsed = talas::expr::parse(
                        *parameter.expression,
                        document.expressionLanguage,
                        document.expressionDefinitions);
                    if (!parsed.ok()
                        || !parsed.expression.has_value())
                    {
                        continue;
                    }
                    const auto references =
                        parsed.expression->eventReferences();
                    if (std::ranges::find(references, eventName)
                        != references.end())
                    {
                        return true;
                    }
                }
                continue;
            }
            const auto* descriptor =
                registry.findNodeType(node.typeId);
            if (descriptor == nullptr)
                continue;
            if (node.typeId == "talas.analogSequencer")
            {
                if (!ownerScopeShadows)
                {
                    const auto* parameter = findParameter(node, "generatedIndex");
                    if (parameter != nullptr && parameter->text.has_value())
                    {
                        const auto target =
                            talas::core::parseAnalogSequencerGeneratedIndex(
                                *parameter->text);
                        if (target.has_value() && *target == eventName)
                            return true;
                    }
                    if (analogSequencerTextReferencesEventValue(
                            document, node, eventName))
                    {
                        return true;
                    }
                }
            }
            if (!ownerScopeShadows && liveListReferencesEventValue(document, node, eventName))
                return true;
            if (!ownerScopeShadows
                && node.typeId == "talas.inputOutputCharacteristic")
            {
                for (const auto parameterId : {"inValues", "outValues"})
                {
                    const auto* parameter = findParameter(node, parameterId);
                    if (parameter == nullptr || !parameter->text.has_value())
                        continue;
                    const auto members =
                        talas::core::val_scalar_array::parse(
                            *parameter->text);
                    if (members.has_value()
                        && std::ranges::any_of(*members,
                            [&](const auto& member) {
                                return member.eventName == eventName;
                            }))
                        return true;
                }
            }
            for (const auto& parameterDescriptor : descriptor->parameters)
            {
                if (parameterDescriptor.kind
                    != talas::core::ParameterKind::Expression)
                {
                    continue;
                }
                const auto* parameter =
                    findParameter(node, parameterDescriptor.id);
                const auto& source =
                    parameter != nullptr
                        && parameter->expression.has_value()
                    ? *parameter->expression
                    : parameterDescriptor.defaultExpression;
                const auto parsed = talas::expr::parse(
                    source,
                    document.expressionLanguage,
                    document.expressionDefinitions);
                if (!parsed.ok()
                    || !parsed.expression.has_value())
                {
                    continue;
                }
                const auto expressionScopeOrdinal = expandedView != nullptr
                    ? parameterSourceScopeOrdinal(
                        *expandedView,
                        node.instanceId,
                        parameterDescriptor.id,
                        nodeScopeOrdinal)
                    : nodeScopeOrdinal;
                const auto expressionScopeShadows = expandedView != nullptr
                    ? localDomainsForScope(
                        *expandedView, expressionScopeOrdinal)
                        .contains(std::string(eventName))
                    : ownerScopeShadows;
                if (expressionScopeShadows)
                    continue;
                const auto references =
                    parsed.expression->eventReferences();
                if (std::ranges::find(
                        references,
                        eventName)
                    != references.end())
                {
                    return true;
                }
            }
        }
        return false;
    };
    if (graphReferences(
            rootGraph, {},
            prefixerView.has_value() ? &*prefixerView : nullptr)
        || graphReferences(
            unexpandedRootGraph(document, prefixerView), {}, nullptr))
        return true;
    if (prefixerView.has_value())
        for (const auto& binding :
             prefixerView->expanded.manifest.eventValues)
        {
            if (!binding.value.expression.has_value()
                || localDomainsForScope(
                       *prefixerView, binding.sourceScopeOrdinal)
                       .contains(std::string(eventName)))
                continue;
            const auto parsed = talas::expr::parse(
                *binding.value.expression,
                document.expressionLanguage,
                document.expressionDefinitions);
            if (parsed.ok()
                && parsed.expression.has_value()
                && std::ranges::find(
                       parsed.expression->eventReferences(),
                       eventName)
                    != parsed.expression->eventReferences().end())
                return true;
        }
    return std::ranges::any_of(
        document.soundDefinitions,
        [&](const auto& definition) {
            if (prefixerView.has_value()
                && prefixerView->instantiatedDefinitions.contains(
                    definition.definitionId))
                return false;
            if (graphReferences(
                    definition.graph,
                    localEventDomains(definition), nullptr))
            {
                return true;
            }
            return std::ranges::any_of(
                definition.exposedParameters,
                [&](const auto& exposed) {
                    if (exposed.kind
                            != talas::core::ParameterKind::Expression
                        || !exposed.defaultValue.expression.has_value())
                    {
                        return false;
                    }
                    const auto parsed = talas::expr::parse(
                        *exposed.defaultValue.expression,
                        document.expressionLanguage,
                        document.expressionDefinitions);
                    if (!parsed.ok()
                        || !parsed.expression.has_value())
                    {
                        return false;
                    }
                    const auto references =
                        parsed.expression->eventReferences();
                    return std::ranges::find(references, eventName)
                        != references.end();
                });
        });
}
}
