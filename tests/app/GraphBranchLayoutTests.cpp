#include "talas/app/GraphBranchLayout.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using talas::app::GraphNodeSet;

// a -> mix, b -> env -> mix, mix -> out; c -> env as well.
talas::project::ProjectGraph branchedGraph()
{
    talas::project::ProjectGraph graph;
    for (const auto* id : {"a", "b", "c", "env", "mix", "out"})
        graph.nodes.push_back({id, "talas.noise", {}});
    graph.connections = {
        {"a", "audioOut", "mix", "inputs"},
        {"b", "audioOut", "env", "left"},
        {"c", "audioOut", "env", "right"},
        {"env", "audioOut", "mix", "inputs"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    return graph;
}

// Clicking the dot where a line meets a Sound hides
// everything to its left, an ellipsis showing there are hidden Sounds;
// clicking the ellipsis shows them again, and a double-click opens the
// whole branch.
void testHiddenBranches()
{
    const auto graph = branchedGraph();
    require(talas::app::hiddenGraphNodes(graph, {}).empty(), "nothing is hidden at first");
    require(talas::app::hiddenGraphNodes(graph, {"env"}) == GraphNodeSet{"b", "c"},
        "hiding env's branch should hide the Sounds to its left");
    require(talas::app::hiddenGraphNodes(graph, {"mix"}) == GraphNodeSet{"a", "b", "c", "env"},
        "hiding the mixer's branch should hide everything to its left");
    auto withFeed = graph;
    withFeed.connections.push_back({"b", "audioOut", "out", "audioIn"});
    require(talas::app::hiddenGraphNodes(withFeed, {"env"}) == GraphNodeSet{"c"},
        "a Sound that also feeds a shown Sound should stay shown");
    auto collapsed = talas::app::toggledGraphBranch(graph, {}, "env");
    require(collapsed == GraphNodeSet{"env"}, "a click on the dot should hide the branch");
    collapsed = talas::app::toggledGraphBranch(graph, collapsed, "env");
    require(collapsed.empty(), "a click on the ellipsis should show the branch again");
    require(talas::app::toggledGraphBranch(graph, {}, "a").empty(), "a Sound without inputs has no branch to hide");
    const auto opened = talas::app::fullyOpenedGraphBranch(graph, {"mix", "env", "a"}, "mix");
    require(opened.empty(), "a double-click should open every hidden branch to the left");
    require(talas::app::fullyOpenedGraphBranch(graph, {"mix", "env"}, "env") == GraphNodeSet{"mix"},
        "opening a branch should leave the branches to its right as they are");
    require(talas::app::graphNodesShiftDragged(graph, "mix") == GraphNodeSet{"a", "b", "c", "env", "mix"}
            && talas::app::graphNodesShiftDragged(graph, "a") == GraphNodeSet{"a"},
        "a drag with Shift moves the Sound and all of its inputs");
    require(talas::app::graphBranchOf(graph, "mix") == GraphNodeSet{"a", "b", "c", "env"}
            && talas::app::graphNodeHasInputs(graph, "env") && !talas::app::graphNodeHasInputs(graph, "a"),
        "a Sound's branch should be everything upstream of it");
    require(!talas::app::graphNodeDeletable(graph, {"env"}, "env") && talas::app::graphNodeDeletable(graph, {}, "env")
            && talas::app::graphNodeDeletable(graph, {"env"}, "mix"),
        "a Sound can be deleted only while its inputs are shown");
    require(talas::app::replacementAsksToConfirm(false, false) && !talas::app::replacementAsksToConfirm(true, true)
            && !talas::app::replacementAsksToConfirm(false, true)
            && talas::app::replacementConfirmation("noise", "oscillator") == "Replace noise with oscillator?",
        "one source replacing another should ask to confirm the replacement");
}

// A SoundCollectionVariable dragged into a Mixer's
// Inputs is not a Sound, strictly speaking, so it does not appear in the
// signal flow; the Mixer's other Inputs do, and one feeding nothing stays
// in view to be placed.
void testSoundCollectionVariablesStayOutOfTheSignalFlow()
{
    talas::project::ProjectGraph graph;
    graph.nodes = {{"voices", "talas.soundCollectionVariable", {}}, {"osc", "talas.oscillator", {}},
        {"mix", "talas.mixer", {}}, {"out", "talas.audioOutput", {}}, {"spare", "talas.soundCollectionVariable", {}}};
    graph.connections = {{"voices", "audioOut", "mix", "inputs"}, {"osc", "audioOut", "mix", "inputs"},
        {"mix", "audioOut", "out", "audioIn"}};
    require(talas::app::hiddenGraphNodes(graph, {}) == GraphNodeSet{"voices"},
        "a SoundCollectionVariable in the Mixer's Inputs should not appear in the signal flow graph");
}

// Clean up lays the signal flow out from right to
// left in columns, inputs in the order of what they feed.
void testCleanUpLayout()
{
    const auto graph = branchedGraph();
    const std::vector<talas::project::ProjectNodePresentation> current{{"a", {300, 500}}, {"b", {40, 30}},
        {"c", {700, 10}}, {"env", {20, 400}}, {"mix", {900, 900}}, {"out", {100, 100}}};
    const auto layout = talas::app::cleanedUpGraphLayout(graph, current, {200, 100});
    const auto at = [&](std::string_view id) {
        return std::ranges::find(layout, id, &talas::project::ProjectNodePresentation::nodeId)->position;
    };
    require(layout.size() == 6, "every Sound should be placed");
    require(at("out") == talas::project::CanvasPosition{20 + 3 * 200, 10},
        "the output should stand in the rightmost column at the graph's top");
    require(at("mix") == talas::project::CanvasPosition{20 + 2 * 200, 10}, "the mixer should stand left of the output");
    require(at("a") == talas::project::CanvasPosition{20 + 200, 10} && at("env") == talas::project::CanvasPosition{20 + 200, 110},
        "the mixer's inputs should stand in one column, in the order of its lines");
    require(at("b") == talas::project::CanvasPosition{20, 10} && at("c") == talas::project::CanvasPosition{20, 110},
        "the envelope's inputs should stand in the leftmost column");
    auto looped = graph;
    looped.connections.push_back({"mix", "audioOut", "a", "audioIn"});
    require(talas::app::cleanedUpGraphLayout(looped, current).size() == 6, "a loop should still be laid out");
    require(talas::app::cleanedUpGraphLayout(graph, {}).empty(), "no positions, nothing to place");
}

// Compressor feeds both Vocoder1 and
// PeakDetector1, a line drawn from each; count is used twice as an input
// to compressor, so one thick line labelled 2 joins them.
void testSharedAndRepeatedInputs()
{
    talas::project::ProjectGraph graph;
    for (const auto* id : {"count", "compressor", "Vocoder1", "PeakDetector1"})
        graph.nodes.push_back({id, "talas.product", {}});
    graph.connections = {
        {"count", "value", "compressor", "left"},
        {"count", "value", "compressor", "right"},
        {"compressor", "value", "Vocoder1", "left"},
        {"compressor", "value", "PeakDetector1", "inputs"},
    };
    require(talas::app::graphNodesFedBy(graph, "compressor") == GraphNodeSet{"PeakDetector1", "Vocoder1"},
        "several Sounds can share an input");
    require(talas::app::graphConnectionUseCounts(graph) == std::vector<std::size_t>{2, 2, 1, 1},
        "count is used twice within compressor; the shared input's lines are each its own");
    graph.connections.push_back({"count", "value", "compressor", "inputs"});
    require(talas::app::graphConnectionUseCounts(graph).front() == 3, "the label counts every use");
    require(talas::app::graphConnectionUseCounts({}).empty(), "no lines, no counts");
}

// A Sound dragged from the prototypes is a copy, a
// number appended to its name.
void testCopiesAreNumbered()
{
    const GraphNodeSet taken{"filter1", "filter2", "oscillator1"};
    const auto isTaken = [&](std::string_view name) { return taken.contains(name); };
    require(talas::app::numberedCopyName("filter", isTaken) == "filter3"
            && talas::app::numberedCopyName("oscillator", isTaken) == "oscillator2"
            && talas::app::numberedCopyName("noise", isTaken) == "noise1",
        "a copy takes the first number its name lacks");
}
}

// A click on a Sound's icon selects it alone;
// Shift adds it to the selection, or takes it away if it was there.
void testAClickSelectsTheSound()
{
    using Change = talas::app::GraphClickSelection;
    require(talas::app::graphClickSelection(false, false) == Change::SelectOnly
            && talas::app::graphClickSelection(false, true) == Change::SelectOnly,
        "clicking a Sound's icon should select it alone");
    require(talas::app::graphClickSelection(true, false) == Change::Add && talas::app::graphClickSelection(true, true) == Change::Remove,
        "Shift+click should add the Sound to the selection, or take it away");
}

int main()
{
    try
    {
        testHiddenBranches();
        testSoundCollectionVariablesStayOutOfTheSignalFlow();
        testAClickSelectsTheSound();
        testCleanUpLayout();
        testSharedAndRepeatedInputs();
        testCopiesAreNumbered();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "graph branch layout tests passed\n";
    return 0;
}
