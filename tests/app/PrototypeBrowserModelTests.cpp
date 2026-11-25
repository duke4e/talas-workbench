#include "talas/app/PrototypeBrowserModel.h"
#include "talas/core/BuiltinNodes.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::app::PrototypeBrowserEntry oscillator()
{
    return {
        "talas.oscillator",
        "Oscillator",
        "Oscillators",
        talas::app::PrototypeMaturityFilter::Full,
    };
}

void testSearchMatchesNameCategoryAndStableTypeIdWithoutCaseSensitivity()
{
    const std::unordered_set<std::string> favorites;
    const std::vector<std::string> recent;
    for (const auto query : {"OSCILL", "oscillators", "talas.osc"})
    {
        require(
            talas::app::prototypeBrowserMatches(
                oscillator(),
                query,
                talas::app::PrototypeBrowserScope::All,
                talas::app::PrototypeMaturityFilter::All,
                favorites,
                recent),
            "prototype search should match familiar and stable identifiers case-insensitively");
    }
    require(
        !talas::app::prototypeBrowserMatches(
            oscillator(),
            "filter",
            talas::app::PrototypeBrowserScope::All,
            talas::app::PrototypeMaturityFilter::All,
            favorites,
            recent),
        "unrelated search terms should not leak into results");
}

void testScopeAndMaturityFiltersCompose()
{
    const std::unordered_set<std::string> favorites{"talas.oscillator"};
    const std::vector<std::string> recent{"talas.filter", "talas.oscillator"};
    require(
        talas::app::prototypeBrowserMatches(
            oscillator(),
            {},
            talas::app::PrototypeBrowserScope::Favorites,
            talas::app::PrototypeMaturityFilter::Full,
            favorites,
            recent),
        "favorite full-runtime prototype should survive both filters");
    require(
        !talas::app::prototypeBrowserMatches(
            oscillator(),
            {},
            talas::app::PrototypeBrowserScope::Favorites,
            talas::app::PrototypeMaturityFilter::Subset,
            favorites,
            recent),
        "maturity filter should still apply inside Favorites");
    require(
        talas::app::prototypeBrowserMatches(
            oscillator(),
            {},
            talas::app::PrototypeBrowserScope::Recent,
            talas::app::PrototypeMaturityFilter::All,
            favorites,
            recent),
        "recent scope should recognise a previously added prototype");
}

void testRecentHistoryIsDeduplicatedAndBounded()
{
    std::vector<std::string> recent{"b", "a", "c"};
    talas::app::recordRecentPrototype(recent, "a", 3);
    require(
        recent == std::vector<std::string>({"a", "b", "c"}),
        "reusing a prototype should move it to the front without duplication");
    talas::app::recordRecentPrototype(recent, "d", 3);
    require(
        recent == std::vector<std::string>({"d", "a", "b"}),
        "recent history should evict the oldest entry at its limit");
}

void testRecentEntriesShareOneMruGroup()
{
    require(
        talas::app::prototypeBrowserGroupName(
            talas::app::PrototypeBrowserScope::Recent,
            "Oscillators")
            == "Recent"
            && talas::app::prototypeBrowserGroupName(
                   talas::app::PrototypeBrowserScope::Recent,
                   "Filters")
                == "Recent",
        "Recent entries from interleaved categories should share one MRU group");
    require(
        talas::app::prototypeBrowserGroupName(
            talas::app::PrototypeBrowserScope::All,
            "Oscillators")
            == "Oscillators",
        "non-Recent scopes should retain category grouping");
}

void testCapabilityTooltipKeepsMaturityAndExecutionAxesDistinct()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    require(oscillator != nullptr, "tooltip fixture should find oscillator");

    const auto tooltip =
        talas::app::prototypeCapabilityTooltip(*oscillator);
    require(
        tooltip.find(oscillator->supportSummary) == 0,
        "tooltip should lead with the established maturity summary");
    require(
        tooltip.find("Offline: Supported") != std::string::npos
            && tooltip.find("Prepared realtime: Supported")
                != std::string::npos,
        "tooltip should expose execution contexts without changing the badge");
    require(
        tooltip.find("Limits: offline-and-prepared-shared-checked-finite-sine-or-immutable-cyclic-wavetable-oscillator-kernel")
            != std::string::npos,
        "tooltip should keep the promoted Oscillator limitation contract visible");
}

void testDescriptorSearchComposesRateOriginVendorAndAllTokens()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.oscillator");
    require(descriptor != nullptr, "search fixture should find oscillator");
    const auto entry = talas::app::prototypeBrowserEntry(*descriptor);
    const std::unordered_set<std::string> favorites;
    const std::vector<std::string> recent;

    require(
        talas::app::prototypeBrowserMatches(
            entry,
            "OSCILLATOR sample Talas subset transport-aware",
            talas::app::PrototypeBrowserScope::All,
            talas::app::PrototypeMaturityFilter::All,
            favorites,
            recent,
            talas::app::PrototypeRateFilter::Sample,
            talas::app::PrototypeOriginFilter::BuiltIn),
        "descriptor search should compose case-insensitive name, rate, vendor, support, rate filter, and origin filter");
    require(
        !talas::app::prototypeBrowserMatches(
            entry,
            "oscillator missing-token",
            talas::app::PrototypeBrowserScope::All,
            talas::app::PrototypeMaturityFilter::All,
            favorites,
            recent),
        "descriptor search should require every query token to match");
    require(
        !talas::app::prototypeBrowserMatches(
            entry,
            {},
            talas::app::PrototypeBrowserScope::All,
            talas::app::PrototypeMaturityFilter::All,
            favorites,
            recent,
            talas::app::PrototypeRateFilter::Frame,
            talas::app::PrototypeOriginFilter::BuiltIn)
            && !talas::app::prototypeBrowserMatches(
                entry,
                {},
                talas::app::PrototypeBrowserScope::All,
                talas::app::PrototypeMaturityFilter::All,
                favorites,
                recent,
                talas::app::PrototypeRateFilter::All,
                talas::app::PrototypeOriginFilter::Project),
        "rate and origin filters should independently reject nonmatching descriptor facts");
}
}

// The system prototypes
// hold one example of each kind of Sound, by category, a category's
// Sounds listed when it is chosen, and Find prototype selects the Sound
// named or offers each that matches.
// Ctrl+] makes the Prototypes' entries larger and
// Ctrl+[ smaller, within a few steps of their usual size.
void testZoomingThePrototypes()
{
    require(talas::app::prototypeRowHeight(0) == 38 && talas::app::zoomedPrototypeStep(0, true) == 1
            && talas::app::prototypeRowHeight(1) > talas::app::prototypeRowHeight(0)
            && talas::app::prototypeRowHeight(-1) < talas::app::prototypeRowHeight(0),
        "zooming in should make the entries larger, zooming out smaller");
    require(talas::app::zoomedPrototypeStep(talas::app::prototypeZoomSteps, true) == talas::app::prototypeZoomSteps
            && talas::app::zoomedPrototypeStep(-talas::app::prototypeZoomSteps, false) == -talas::app::prototypeZoomSteps,
        "zooming should stop a few steps from the usual size");
}

void testSystemPrototypesHoldOneOfEachSoundByCategory()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto palette = talas::app::prototypePalette(registry);
    std::unordered_set<std::string> kinds;
    for (const auto& type : palette)
    {
        require(kinds.insert(type.typeId).second && !type.category.empty() && type.replacedBy.empty(),
            "each kind of Sound should appear once, in a category, legacy kinds left out");
    }
    require(palette.size() > 300, "the palette should hold every built-in Sound");
    require(std::ranges::is_sorted(palette, [](const auto& left, const auto& right) {
        return left.category != right.category ? left.category < right.category : left.displayName < right.displayName;
    }), "the prototypes should be listed by category");
    const std::unordered_set<std::string> none;
    const std::vector<std::string> recent;
    const auto matching = [&](std::string_view query) {
        std::vector<std::string> found;
        for (const auto& type : palette)
        {
            if (talas::app::prototypeBrowserMatches(talas::app::prototypeBrowserEntry(type), query,
                    talas::app::PrototypeBrowserScope::All, talas::app::PrototypeMaturityFilter::All, none, recent))
                found.push_back(type.typeId);
        }
        return found;
    };
    const auto& oscillatorCategory = std::ranges::find(palette, std::string{"talas.oscillator"}, &talas::core::NodeTypeDescriptor::typeId)->category;
    const auto inCategory = matching(oscillatorCategory);
    require(std::ranges::find(inCategory, std::string{"talas.oscillator"}) != inCategory.end(),
        "choosing a category should list its Sounds");
    const auto found = matching("Oscillator");
    require(found.size() > 1 && std::ranges::find(found, std::string{"talas.oscillatorBank"}) != found.end(),
        "a name several Sounds share should offer each of them");
    const auto named = matching("Time Offset");
    require(std::ranges::find(named, std::string{"talas.timeOffset"}) != named.end(), "a full name should find its Sound");
    // Some Sounds appear in more than one category, each
    // kind still one prototype.
    std::vector<std::size_t> all(palette.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    const auto rows = talas::app::prototypePaletteRows(palette, all);
    std::unordered_map<std::string, std::set<std::string>> categoriesOf;
    for (const auto& row : rows)
        categoriesOf[palette[row.index].typeId].insert(row.category);
    require(rows.size() > palette.size() && categoriesOf["talas.sample"].size() == 2
            && categoriesOf["talas.sample"].contains("Sources") && categoriesOf["talas.sample"].contains("Sampling")
            && categoriesOf["talas.oscillator"].size() == 1,
        "some Sounds, as the Sample, should appear in more than one category");
    require(std::ranges::is_sorted(rows, [&](const auto& left, const auto& right) { return left.category < right.category; }),
        "the rows should go by category");
    // The Annotation is a Sound, not an editor, found among the
    // Variables and Annotations.
    require(categoriesOf["talas.annotation"].contains("Variables & Annotation"),
        "the Annotation should be a prototype among the Variables and Annotations");
    const auto sources = matching("Sources");
    require(std::ranges::find(sources, std::string{"talas.sample"}) != sources.end(),
        "choosing a category should list the Sounds also in it");
}

// Find prototype... comes up with a pull-down list
// of the Sounds whose names contain the partial name typed, to choose from.
void testFindingAPrototypeByPartOfItsName()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto palette = talas::app::prototypePalette(registry);
    const auto choices = talas::app::prototypeFindChoices(palette, " SAMPLE ");
    const auto lowered = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return text;
    };
    std::unordered_set<std::string> once;
    require(choices.size() > 1 && std::ranges::find(choices, std::string{"Sample"}) != choices.end()
            && std::ranges::all_of(choices, [&](const std::string& name) {
                   return lowered(name).find("sample") != std::string::npos && once.insert(name).second;
               }),
        "the list should hold each Sound whose name contains the partial name, once");
    require(talas::app::prototypeFindChoices(palette, "zzqq").empty() && talas::app::prototypeFindChoices(palette, "  ").empty(),
        "nothing matching, or nothing typed, lists no Sounds");
}

// A click on a prototype's icon selects that
// prototype, whichever category it is listed in; a click on a category's
// name shows or hides that category and selects no prototype.
void testClickingAPrototypeSelectsIt()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto palette = talas::app::prototypePalette(registry);
    std::vector<std::size_t> all(palette.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    const auto rows = talas::app::prototypePaletteRows(palette, all);
    const auto sample = std::ranges::find_if(rows, [&](const talas::app::PrototypePaletteRow& row) {
        return palette[row.index].displayName == "Sample";
    });
    require(sample != rows.end(), "the palette should list the Sample prototype");
    const auto clicked = talas::app::prototypeListClick({sample->index, sample->category});
    require(clicked.selected == std::optional<std::size_t>{sample->index} && !clicked.toggledCategory.has_value()
            && palette[*clicked.selected].displayName == "Sample",
        "a click on a prototype's icon should select that prototype");
    for (const auto& row : rows)
    {
        require(talas::app::prototypeListClick({row.index, row.category}).selected == std::optional<std::size_t>{row.index},
            "a click on any prototype listed, in any of its categories, should select it");
    }
    const auto category = talas::app::prototypeListClick({std::nullopt, sample->category});
    require(!category.selected.has_value() && category.toggledCategory == std::optional<std::string>{sample->category},
        "a click on a category's name should show or hide its prototypes, selecting none");
}

// AR Drag&Drop, found among the Prototypes'
// envelopes, comes with !KeyDown as the trigger of its AR and !KeyVelocity
// as the AR's amplitude, written in as the Prototypes make it: the AR's own
// defaults stay a Gate and a Scale of 1, so an AR saved without them keeps
// them. Dropped on the line between a Noise and a Chopper, its VCA takes the
// line and the AR its Gain (AR Drag&Drop (AR, Noise (white))).
void testArDragAndDropComesWithKeyDownAndKeyVelocity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* ar = registry.findNodeType("talas.ar");
    require(ar != nullptr, "the AR prototype should be there");
    const auto palette = talas::app::prototypePalette(registry);
    const auto listed = std::ranges::find(
        palette, std::string{talas::app::arDragAndDropPrototypeId}, &talas::core::NodeTypeDescriptor::typeId);
    require(listed != palette.end() && listed->displayName == "AR Drag&Drop" && listed->category == ar->category,
        "AR Drag&Drop should be listed among the envelopes");
    const auto found = talas::app::prototypeFindChoices(palette, "drag&drop");
    require(std::ranges::find(found, std::string{"AR Drag&Drop"}) != found.end(),
        "searching the Prototypes should find AR Drag&Drop");

    const auto assembly = talas::app::prototypeAssembly(
        registry, talas::app::arDragAndDropPrototypeId, [](std::string_view) { return false; });
    require(assembly.has_value() && assembly->head.typeId == "talas.vca" && assembly->source.typeId == "talas.ar"
            && assembly->sourceOutput == "value" && assembly->headControl == "gain",
        "AR Drag&Drop should be a VCA whose Gain an AR sets");
    const auto field = [](const talas::project::ProjectNode& node, std::string_view id) {
        const auto parameter = std::ranges::find(node.parameters, id, &talas::project::ProjectParameterExpression::parameterId);
        return parameter == node.parameters.end() ? std::optional<std::string>{} : parameter->expression;
    };
    require(field(assembly->source, "gate") == std::optional<std::string>{"!KeyDown"}
            && field(assembly->source, "scale") == std::optional<std::string>{"!KeyVelocity"},
        "AR Drag&Drop's AR should come with !KeyDown as its trigger and !KeyVelocity as its amplitude");
    const auto defaultOf = [&](std::string_view id) {
        const auto parameter = std::ranges::find(ar->parameters, id, &talas::core::ParameterDescriptor::id);
        return parameter == ar->parameters.end() ? std::string{} : parameter->defaultExpression;
    };
    require(defaultOf("gate") == "1.0" && defaultOf("scale") == "1.0",
        "the AR's own Gate and Scale should keep their defaults, so saved ARs play as before");
    const auto numbered = talas::app::prototypeAssembly(registry, talas::app::arDragAndDropPrototypeId,
        [](std::string_view name) { return name == "arDragDrop1" || name == "ar1"; });
    require(numbered.has_value() && numbered->head.instanceId == "arDragDrop2" && numbered->source.instanceId == "ar2",
        "AR Drag&Drop's Sounds should take names the graph does not hold");
    require(!talas::app::prototypeAssembly(registry, "talas.vca", [](std::string_view) { return false; }).has_value(),
        "a prototype of one Sound should be no assembly");

    talas::project::ProjectDocument document;
    document.graph.nodes = {{"noise", "talas.noise", {}}, {"chopper", "talas.chopper", {}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"noise", "audioOut", "chopper", "audioIn"}, {"chopper", "audioOut", "out", "audioIn"}};
    std::string error;
    std::string head;
    const auto placed = talas::app::documentWithPrototypeAssembly(document, registry, talas::app::arDragAndDropPrototypeId,
        std::nullopt, {300, 0}, talas::core::Connection{"noise", "audioOut", "chopper", "audioIn"}, error, &head);
    require(placed.has_value(), "AR Drag&Drop should drop on the line: " + error);
    const auto joins = [&](talas::core::Connection connection) {
        return std::ranges::find(placed->graph.connections, connection) != placed->graph.connections.end();
    };
    const auto envelope = std::ranges::find_if(
        placed->graph.nodes, [](const talas::project::ProjectNode& node) { return node.typeId == "talas.ar"; });
    require(envelope != placed->graph.nodes.end() && joins({"noise", "audioOut", head, "audioIn"})
            && joins({head, "audioOut", "chopper", "audioIn"}) && joins({envelope->instanceId, "value", head, "gain"})
            && !joins({"noise", "audioOut", "chopper", "audioIn"})
            && field(*envelope, "gate") == std::optional<std::string>{"!KeyDown"}
            && field(*envelope, "scale") == std::optional<std::string>{"!KeyVelocity"},
        "dropped between Noise and Chopper, AR Drag&Drop's VCA should shape the noise by its keyed AR");
}

int main()
{
    testArDragAndDropComesWithKeyDownAndKeyVelocity();
    testClickingAPrototypeSelectsIt();
    testFindingAPrototypeByPartOfItsName();
    testSearchMatchesNameCategoryAndStableTypeIdWithoutCaseSensitivity();
    testScopeAndMaturityFiltersCompose();
    testRecentHistoryIsDeduplicatedAndBounded();
    testRecentEntriesShareOneMruGroup();
    testCapabilityTooltipKeepsMaturityAndExecutionAxesDistinct();
    testDescriptorSearchComposesRateOriginVendorAndAllTokens();
    testSystemPrototypesHoldOneOfEachSoundByCategory();
    testZoomingThePrototypes();
    return 0;
}
