#include "app/workbench/WorkbenchGraphCanvas.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/app/WorkbenchGraphViewportIndex.h"
#include "talas/project/ProjectValidation.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr std::size_t nodeCount = 1000;
constexpr std::size_t connectionCount = 2000;
constexpr std::size_t soundDefinitionCount = 512;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

talas::project::ProjectNode node(std::string id, std::string type)
{
    return {std::move(id), std::move(type), {}};
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {
        std::move(fromNode), std::move(fromPort),
        std::move(toNode), std::move(toPort),
    };
}

talas::project::ProjectDocument largeGraphDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.reserve(nodeCount);
    document.graph.connections.reserve(connectionCount);
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    constexpr auto mixerCount = nodeCount - 2U;
    for (std::size_t index = 0; index < mixerCount; ++index)
    {
        document.graph.nodes.push_back(
            node("mix" + std::to_string(index), "talas.overlappingMixer"));
    }
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    for (std::size_t input = 1; input <= 4; ++input)
    {
        document.graph.connections.push_back(
            connection(
                "osc", "audioOut", "mix0",
                "in" + std::to_string(input)));
    }
    for (std::size_t index = 1; index < mixerCount; ++index)
    {
        const auto previous = "mix" + std::to_string(index - 1U);
        const auto current = "mix" + std::to_string(index);
        document.graph.connections.push_back(
            connection(previous, "leftOut", current, "in1"));
        document.graph.connections.push_back(
            connection(previous, "rightOut", current, "in2"));
    }
    const auto finalMixer = "mix" + std::to_string(mixerCount - 1U);
    document.graph.connections.push_back(
        connection(finalMixer, "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(
        connection(finalMixer, "rightOut", "out", "rightIn"));
    talas::project::reconcileProjectPresentation(document);
    auto& layout = document.presentation.structureLayouts.front();
    for (std::size_t index = 0; index < layout.nodes.size(); ++index)
    {
        layout.nodes[index].position = {
            static_cast<std::int32_t>((index % 40U) * 224U),
            static_cast<std::int32_t>((index / 40U) * 128U),
        };
    }
    return document;
}

talas::project::ProjectDocument manySoundDefinitionsDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.reserve(soundDefinitionCount + 2U);
    document.soundDefinitions.reserve(soundDefinitionCount);
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.connections.push_back(
        connection("osc", "audioOut", "out", "audioIn"));
    for (std::size_t index = 0; index < soundDefinitionCount; ++index)
    {
        const auto id = "definition" + std::to_string(index);
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = id;
        definition.displayName = "Definition " + std::to_string(index);
        document.soundDefinitions.push_back(std::move(definition));
        talas::project::ProjectNode instance;
        instance.instanceId = "sound" + std::to_string(index);
        instance.soundDefinitionId = id;
        document.graph.nodes.push_back(std::move(instance));
    }
    talas::project::reconcileProjectPresentation(document);
    return document;
}

std::uint64_t nearestRank(
    const std::vector<std::uint64_t>& sorted,
    std::size_t numerator,
    std::size_t denominator)
{
    const auto rank = (sorted.size() * numerator + denominator - 1U)
        / denominator;
    return sorted[std::max<std::size_t>(1U, rank) - 1U];
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = largeGraphDocument();
    require(document.graph.nodes.size() == nodeCount
            && document.graph.connections.size() == connectionCount,
        "large fixture did not author the exact accepted shape");
    require(talas::project::validateProjectDocument(document, registry).ok(),
        "large fixture is not a product-valid project graph");
    talas::runtime::ProjectRuntimeBridge bridge(
        document, registry);
    auto canvas = talas::app::workbench::makeGraphCanvas(
        bridge,
        registry,
        [](auto, auto, auto) {},
        [] {},
        [](const auto&) {},
        [](auto, auto, auto, auto) {},
        [](auto, auto) {},
        [](auto, auto, auto, auto) {},
        [](auto) {});
    juce::Component nativeHost;
    nativeHost.setSize(1440, 900);
    nativeHost.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    nativeHost.addAndMakeVisible(canvas.get());
    canvas->setSize(1440, 900);
    canvas->setSessionViewState(0.0, 0.0, 1.0);
    require(canvas->totalNodeCountForSmoke() == nodeCount,
        "large fixture did not retain all nodes");
    require(canvas->accessibilityItemCountForSmoke() >= nodeCount,
        "viewport culling truncated the complete accessibility order");
    require(canvas->isFocusContainer()
            && canvas->getNumChildComponents() == 1
            && canvas->getChildComponent(0) != nullptr
            && canvas->getChildComponent(0)->getAlpha() == 0.0F
            && canvas->getChildComponent(0)->isAccessible()
            && static_cast<std::size_t>(
                   canvas->getChildComponent(0)->getNumChildComponents())
                == canvas->accessibilityItemCountForSmoke(),
        "complete accessibility order was not isolated from the paint child tree");
    auto* graphAccessibility = canvas->getAccessibilityHandler();
    require(graphAccessibility != nullptr
            && graphAccessibility->getChildren().size()
                == canvas->accessibilityItemCountForSmoke(),
        "native graph tree did not expose every node/port item directly");
    require(canvas->visibleNodeCountForSmoke() > 0
            && canvas->visibleNodeCountForSmoke() < nodeCount,
        "large fixture did not cull nodes to the viewport");
    require(canvas->graphViewportMembershipCountForSmoke()
                <= (nodeCount + connectionCount)
                    * talas::app::WorkbenchGraphViewportIndex::maximumMembershipsPerItem,
        "large fixture exceeded the fixed spatial memory envelope");

    const auto structuralBefore =
        canvas->structuralGraphRebuildCountForSmoke();
    const auto accessibilityBefore =
        canvas->accessibilityGraphRebuildCountForSmoke();
    juce::Image image(juce::Image::ARGB, 1440, 900, true);
    std::vector<std::uint64_t> paintDurations;
    paintDurations.reserve(32);
    for (std::size_t warmup = 0; warmup < 4; ++warmup)
    {
        image.clear(image.getBounds(), juce::Colours::transparentBlack);
        juce::Graphics graphics(image);
        canvas->paintEntireComponent(graphics, false);
    }
    for (std::size_t sample = 0; sample < 32; ++sample)
    {
        image.clear(image.getBounds(), juce::Colours::transparentBlack);
        juce::Graphics graphics(image);
        const auto started = std::chrono::steady_clock::now();
        canvas->paintEntireComponent(graphics, false);
        const auto finished = std::chrono::steady_clock::now();
        paintDurations.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }
    // The canvas is a rounded card whose corners stay clear for the
    // Workbench's backdrop, so the fresh frame is probed inside it.
    require(image.getPixelAt(720, 450).getARGB() != 0U,
        "large fixture did not paint a fresh frame");
    require(canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore,
        "paint rebuilt structural or accessibility caches");

    canvas->setSessionViewState(-600.0, -320.0, 1.0);
    canvas->setSelectedNodeId("mix500");
    require(canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore,
        "pan or selection rebuilt structural or accessibility caches");
    require(bridge.editor().setParameter(
                "osc", {"frequency", std::string{"880 hz"}}).ok(),
        "large fixture parameter edit failed");
    const auto nonstructuralStarted = std::chrono::steady_clock::now();
    canvas->refreshModel();
    const auto nonstructuralDuration = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - nonstructuralStarted).count());
    require(canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore,
        "nonstructural parameter edit rebuilt graph caches");
    const std::array diagnostics{
        talas::runtime::Diagnostic{
            talas::runtime::DiagnosticCode::UnknownParameter,
            "osc", {}, "frequency", "fixture diagnostic", {}},
    };
    canvas->setCompileDiagnostics(
        diagnostics, bridge.editor().document());
    require(canvas->hasCompileDiagnostic("osc")
            && canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore,
        "diagnostic metadata rebuilt structural accessibility components");
    const talas::project::CanvasPosition movedMix500{
        static_cast<std::int32_t>((501U % 40U) * 224U + 32U),
        static_cast<std::int32_t>((501U / 40U) * 128U),
    };
    require(canvas->pressNodeKeyForSmoke(
                "mix500", juce::KeyPress{juce::KeyPress::rightKey})
            && canvas->interactiveNodeGeometryMatchesForSmoke(
                "mix500", movedMix500),
        "held keyboard nudge did not update hit and accessibility geometry");
    canvas->finishKeyboardNudgeForSmoke();
    require(canvas->interactiveNodeGeometryMatchesForSmoke(
                "mix500", movedMix500)
            && canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore,
        "committed keyboard nudge did not preserve the bounded cache contract");

    std::vector<std::string> everyNode;
    everyNode.reserve(nodeCount);
    for (const auto& projectNode : bridge.editor().document().graph.nodes)
        everyNode.push_back(projectNode.instanceId);
    canvas->setSelectedNodeIds(everyNode, "osc");
    const auto scratchCapacityBefore =
        canvas->graphViewportScratchCapacityForSmoke();
    require(canvas->pressNodeKeyForSmoke(
                "osc", juce::KeyPress{juce::KeyPress::rightKey}),
        "large multi-selection nudge was rejected");
    image.clear(image.getBounds(), juce::Colours::transparentBlack);
    {
        juce::Graphics graphics(image);
        canvas->paintEntireComponent(graphics, false);
    }
    require(canvas->graphViewportScratchCapacityForSmoke()
                == scratchCapacityBefore,
        "moving multi-selection allocated paint/hit scratch capacity");
    require(canvas->pressNodeKeyForSmoke(
                "osc", juce::KeyPress{juce::KeyPress::escapeKey}),
        "large multi-selection nudge did not cancel");
    canvas->refreshModel();
    std::vector<std::uint64_t> rebuildDurations;
    rebuildDurations.reserve(16);
    for (std::size_t sample = 0; sample < 16; ++sample)
    {
        const auto started = std::chrono::steady_clock::now();
        canvas->rebuildStructuralGraphCachesForSmoke();
        const auto finished = std::chrono::steady_clock::now();
        rebuildDurations.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }
    require(canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore + rebuildDurations.size()
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore + rebuildDurations.size(),
        "structural refresh did not invalidate both graph caches exactly once per edit");
    require(bridge.editor().disconnect("out", "rightIn").ok(),
        "large fixture structural edit failed");
    canvas->refreshModel();
    require(canvas->structuralGraphRebuildCountForSmoke()
                == structuralBefore + rebuildDurations.size() + 1U
            && canvas->accessibilityGraphRebuildCountForSmoke()
                == accessibilityBefore + rebuildDurations.size() + 1U,
        "real structural edit did not invalidate both graph caches exactly once");

    std::ranges::sort(paintDurations);
    std::ranges::sort(rebuildDurations);
    std::cout
        << "Large graph paint ns: p50="
        << nearestRank(paintDurations, 50U, 100U)
        << " p95=" << nearestRank(paintDurations, 95U, 100U)
        << " max=" << paintDurations.back() << '\n';
    std::cout
        << "Large graph cache rebuild ns: p50="
        << nearestRank(rebuildDurations, 50U, 100U)
        << " p95=" << nearestRank(rebuildDurations, 95U, 100U)
        << " max=" << rebuildDurations.back() << '\n';
    std::cout << "Large graph nonstructural edit and refresh ns: "
              << nonstructuralDuration << '\n';
    nativeHost.removeChildComponent(canvas.get());
    nativeHost.removeFromDesktop();

    const auto soundDocument = manySoundDefinitionsDocument();
    require(talas::project::validateProjectDocument(soundDocument, registry).ok(),
        "many-Sound-definition fixture is not a valid project graph");
    talas::runtime::ProjectRuntimeBridge soundBridge(soundDocument, registry);
    auto soundCanvas = talas::app::workbench::makeGraphCanvas(
        soundBridge,
        registry,
        [](auto, auto, auto) {},
        [] {},
        [](const auto&) {},
        [](auto, auto, auto, auto) {},
        [](auto, auto) {},
        [](auto, auto, auto, auto) {},
        [](auto) {});
    soundCanvas->setSize(1440, 900);
    const auto soundStructuralBefore =
        soundCanvas->structuralGraphRebuildCountForSmoke();
    require(soundBridge.editor().setParameter(
                "osc", {"frequency", std::string{"660 hz"}}).ok(),
        "many-Sound-definition nonstructural edit failed");
    const auto soundRefreshStarted = std::chrono::steady_clock::now();
    soundCanvas->refreshModel();
    const auto soundRefreshNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - soundRefreshStarted).count());
    require(soundCanvas->structuralGraphRebuildCountForSmoke()
                == soundStructuralBefore,
        "many-Sound-definition parameter edit rebuilt structural caches");
    require(soundRefreshNs < 50'000'000U,
        "many-Sound-definition parameter refresh exceeded the linear budget");
    std::cout << "Many-Sound-definition nonstructural refresh ns: "
              << soundRefreshNs << '\n';
    std::cout << "Workbench graph large fixture tests passed\n";
}
