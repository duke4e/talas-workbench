#include "talas/app/SoundWorkbenchModel.h"
#include "talas/core/BuiltinNodes.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectSoundDefinition voiceDefinition()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Warm Voice";
    definition.inputs.push_back({
        "frequency",
        talas::core::PortType::Control,
        "osc",
        "frequency",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {},
        {},
    });
    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue = expressionParameter("frequency", "440 hz");
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    });
    definition.exposedParameters.push_back(std::move(frequency));
    return definition;
}

void testDefinitionBrowserSearchScopesAndStableSelectionHistory()
{
    const talas::app::SoundDefinitionBrowserEntry entry{
        "voice",
        "Warm Voice",
    };
    std::unordered_set<std::string> bookmarks{"voice"};
    std::vector<std::string> recent{"voice"};
    for (const auto query : {"WARM", "voice"})
    {
        require(
            talas::app::soundDefinitionBrowserMatches(
                entry,
                query,
                talas::app::SoundDefinitionBrowserScope::All,
                bookmarks,
                recent),
            "definition search should match display and stable identities without case sensitivity");
    }
    require(
        talas::app::soundDefinitionBrowserMatches(
            entry,
            {},
            talas::app::SoundDefinitionBrowserScope::Bookmarked,
            bookmarks,
            recent),
        "bookmarked scope should use stable definition ids");
    require(
        talas::app::soundDefinitionBrowserMatches(
            entry,
            {},
            talas::app::SoundDefinitionBrowserScope::Recent,
            bookmarks,
            recent),
        "recent scope should use stable definition ids");

    talas::app::toggleSoundDefinitionBookmark(bookmarks, "voice");
    require(bookmarks.empty(), "bookmark toggle should remove an existing identity");
    talas::app::toggleSoundDefinitionBookmark(bookmarks, "voice");
    require(bookmarks.contains("voice"), "bookmark toggle should restore an identity");
    talas::app::recordRecentSoundDefinition(recent, "other", 2);
    talas::app::recordRecentSoundDefinition(recent, "voice", 2);
    require(
        recent == std::vector<std::string>({"voice", "other"}),
        "definition recency should be deduplicated, ordered, and bounded");

    talas::project::WorkspaceSessionState session;
    talas::app::selectSoundWorkbenchSessionNode(
        session,
        std::nullopt,
        "rootNode");
    talas::app::selectSoundWorkbenchSessionNode(
        session,
        std::optional<std::string>{"voice"},
        "osc");
    require(
        talas::app::soundWorkbenchSessionSelection(
            session,
            std::nullopt) == "rootNode"
            && talas::app::soundWorkbenchSessionSelection(
                session,
                std::optional<std::string_view>{"voice"}) == "osc",
        "root and definition selections should project directly through WorkspaceSession by structural context");
    require(
        talas::app::workspaceSessionViewMode(
            talas::app::WorkbenchWorkspaceMode::Performance)
                == talas::project::WorkspaceViewMode::Performance
            && talas::app::workspaceSessionViewMode(
                talas::app::WorkbenchWorkspaceMode::Analysis)
                == talas::project::WorkspaceViewMode::Structure
            && talas::app::workspaceSessionViewMode(
                talas::app::WorkbenchWorkspaceMode::Samples)
                == talas::project::WorkspaceViewMode::Samples
            && talas::app::workbenchWorkspaceMode(
                talas::project::WorkspaceViewMode::Performance)
                == talas::app::WorkbenchWorkspaceMode::Performance
            && talas::app::workbenchWorkspaceMode(
                talas::project::WorkspaceViewMode::Structure)
                == talas::app::WorkbenchWorkspaceMode::Patch
            && talas::app::workbenchWorkspaceMode(
                talas::project::WorkspaceViewMode::Samples)
                == talas::app::WorkbenchWorkspaceMode::Samples,
        "Workbench projection should preserve Structure, Performance, and Samples session boundaries exactly");
}

void testPerformanceControlsUseStableExposedParameterIdentity()
{
    talas::project::ProjectDocument document;
    document.soundDefinitions.push_back(voiceDefinition());
    document.graph.nodes.push_back({
        "lead",
        {},
        {expressionParameter("frequency", "220 hz")},
        std::optional<std::string>{"voice"},
    });

    const auto controls = talas::app::soundPerformanceControls(document, "lead");
    require(controls.size() == 1,
        "one exposed parameter should create one Performance control");
    require(controls.front().identity.instanceId == "lead" &&
                controls.front().identity.definitionId == "voice" &&
                controls.front().identity.parameterId == "frequency",
        "Performance control identity should bind instance, definition, and "
        "stable parameter id");
    require(controls.front().value.expression ==
                    std::optional<std::string>{"220 hz"} &&
                controls.front().defaultValue.expression ==
                    std::optional<std::string>{"440 hz"},
        "Performance controls should expose sparse override and definition "
        "default separately");

    const auto target = talas::app::soundPerformanceStructureTarget(document,
        controls.front().identity);
    require(target ==
                std::optional<talas::app::SoundPerformanceStructureTarget>{
                    {"lead", "voice", "frequency"}},
        "Performance control Structure navigation should revalidate the "
        "exact instance, definition, and parameter identity");
    for (const auto& staleIdentity : {
             talas::app::SoundPerformanceControlIdentity{
                 "removed", "voice", "frequency"},
             talas::app::SoundPerformanceControlIdentity{
                 "lead", "removed", "frequency"},
             talas::app::SoundPerformanceControlIdentity{
                 "lead", "voice", "removed"},
         })
    {
        require(
            !talas::app::soundPerformanceStructureTarget(document, staleIdentity)
                 .has_value(),
            "every stale Performance identity field must fail closed without "
            "guessing another Structure target");
    }
}

void testDefinitionBrowserProjectsUsageAndBreadcrumbDisplayNames()
{
    talas::project::ProjectDocument document;
    auto voice = voiceDefinition();
    voice.graph.nodes.push_back({
        "nestedVoice",
        {},
        {},
        std::optional<std::string>{"nested"},
    });
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectSoundDefinition nested;
    nested.definitionId = "nested";
    nested.displayName = "Nested Layer";
    document.soundDefinitions.push_back(std::move(nested));
    document.graph.nodes.push_back({
        "lead",
        {},
        {},
        std::optional<std::string>{"voice"},
    });

    const auto entries = talas::app::soundDefinitionBrowserEntries(document);
    const auto voiceEntry = std::ranges::find(entries,
        std::string{"voice"},
        &talas::app::SoundDefinitionBrowserEntry::definitionId);
    const auto nestedEntry = std::ranges::find(entries,
        std::string{"nested"},
        &talas::app::SoundDefinitionBrowserEntry::definitionId);
    require(voiceEntry != entries.end() && voiceEntry->rootInstanceCount == 1 &&
                voiceEntry->nestedInstanceCount == 0 &&
                nestedEntry != entries.end() &&
                nestedEntry->rootInstanceCount == 0 &&
                nestedEntry->nestedInstanceCount == 1,
        "definition browser metadata should expose deterministic root and "
        "nested usage counts");
    require(talas::app::soundHierarchyBreadcrumbs(document,
                std::vector<std::string>{"voice", "nested"}) ==
                std::vector<talas::app::SoundHierarchyBreadcrumb>({
                    {std::nullopt, "Root"},
                    {"voice", "Warm Voice"},
                    {"nested", "Nested Layer"},
                }),
        "breadcrumbs should retain stable ids while projecting current "
        "display names");
}

void testCompileDiagnosticsMapToTheVisibleStructuralContext()
{
    talas::project::ProjectDocument document;
    auto voice = voiceDefinition();
    voice.graph.nodes.push_back({
        "nestedVoice",
        {},
        {},
        std::optional<std::string>{"nested"},
    });
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectSoundDefinition nested;
    nested.definitionId = "nested";
    nested.displayName = "Nested Voice";
    nested.graph.nodes.push_back({
        "innerOsc",
        "talas.oscillator",
        {},
        {},
    });
    document.soundDefinitions.push_back(std::move(nested));
    talas::project::ProjectSoundDefinition unrelated;
    unrelated.definitionId = "unrelated";
    unrelated.displayName = "Unrelated Voice";
    unrelated.graph.nodes.push_back({
        "nestedVoice",
        {},
        {},
        std::optional<std::string>{"nested"},
    });
    document.soundDefinitions.push_back(std::move(unrelated));
    document.graph.nodes.push_back({
        "lead",
        {},
        {},
        std::optional<std::string>{"voice"},
    });
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}, {}});

    const std::vector<talas::app::SoundWorkbenchDiagnosticIdentity> diagnostics{
        {"flat-expanded-osc", "lead/voice::osc"},
        {"flat-expanded-inner", "lead/nestedVoice/nested::innerOsc"},
        {"output", "output"},
        {"unrelated", "missing/other::node"},
    };

    require(talas::app::soundWorkbenchDiagnosticNodeIds(document,
                std::nullopt,
                diagnostics) == std::vector<std::string>({"lead", "output"}),
        "root diagnostics should mark the root Sound boundary or direct root "
        "node");
    require(talas::app::soundWorkbenchDiagnosticNodeIds(document,
                std::optional<std::string>{"voice"},
                diagnostics) ==
                std::vector<std::string>({"osc", "nestedVoice"}),
        "definition diagnostics should mark the exact local node or nested "
        "Sound boundary");
    require(talas::app::soundWorkbenchDiagnosticNodeIds(document,
                std::optional<std::string>{"nested"},
                diagnostics) == std::vector<std::string>({"innerOsc"}),
        "nested definition diagnostics should resolve the "
        "definition-qualified local node");
    require(talas::app::soundWorkbenchDiagnosticNodeIds(document,
                std::optional<std::string>{"unrelated"},
                diagnostics)
                .empty(),
        "an unrelated definition must not inherit a diagnostic merely "
        "because one local node id matches an ancestry segment");
}

void testAuditionDocumentSubstitutesInputsWithoutMutatingTheProject()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    source.metadata.title = "Original";
    source.soundDefinitions.push_back(voiceDefinition());
    const auto before = source;

    const auto audition =
        talas::app::makeSoundDefinitionAuditionDocument(source, "voice", registry);
    require(audition.ok(),
        "audio-output definition with a control input should "
        "build an audition patch");
    require(source.metadata.title == before.metadata.title &&
                source.graph.nodes.empty() &&
                source.soundDefinitions.front().graph.nodes.size() ==
                    before.soundDefinitions.front().graph.nodes.size(),
        "audition document construction must not mutate project topology");
    require(
        audition.document->graph.nodes.size() == 3
            && audition.document->graph.connections.size() == 2,
        "audition patch should contain the Sound instance, bounded control substitute, and output sink");
    require(
        audition.document->graph.nodes[1].typeId == "talas.constant"
            && audition.document->graph.connections.front().toPort
                == "frequency",
        "control boundaries should receive a deterministic local test source");

    auto unsupported = voiceDefinition();
    unsupported.definitionId = "eventVoice";
    unsupported.displayName = "Event Voice";
    unsupported.inputs.front().type = talas::core::PortType::Event;
    source.soundDefinitions.push_back(std::move(unsupported));
    const auto rejected = talas::app::makeSoundDefinitionAuditionDocument(
        source,
        "eventVoice",
        registry);
    require(
        !rejected.ok()
            && rejected.message.find("Event") != std::string::npos,
        "unsupported audition substitution should fail closed with the boundary type");

    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.0.0";
    component.stateSchemaVersion = 1;
    component.displayName = "Unavailable audition component";
    component.bundleId = "org.example.audition";
    component.recoveryHint = "Install the matching owner-approved bundle";
    talas::project::ProjectNode missing;
    missing.instanceId = "missing";
    missing.typeId = "org.example.audition";
    missing.preservedComponent = std::move(component);
    source.graph.nodes.push_back(std::move(missing));
    talas::project::reconcileProjectPresentation(source);
    const auto unavailableSource =
        talas::app::makeSoundDefinitionAuditionDocument(
            source,
            "voice",
            registry);
    require(
        !unavailableSource.ok()
            && unavailableSource.message.find("Missing component type")
                != std::string::npos,
        "Sound audition must reject an unresolved source project before deriving a reduced graph");
}

void testDiagnosticNavigationTargetsExactStructuralNode()
{
    talas::project::ProjectDocument document;
    auto voice = voiceDefinition();
    voice.graph.nodes.push_back({
        "local",
        "talas.oscillator",
        {},
        {},
    });
    document.soundDefinitions.push_back(std::move(voice));
    document.graph.nodes.push_back({
        "lead",
        {},
        {},
        std::optional<std::string>{"voice"},
    });
    document.graph.nodes.push_back({
        "out",
        "talas.audioOutput",
        {},
        {},
    });

    const auto root = talas::app::soundWorkbenchDiagnosticTarget(
        document,
        {"out", "out"});
    require(
        root.has_value()
            && !root->definitionId.has_value()
            && root->nodeId == "out",
        "root diagnostic navigation should resolve the exact root node");

    const auto nested = talas::app::soundWorkbenchDiagnosticTarget(
        document,
        {"flattened-runtime-id", "lead/voice::local"});
    require(
        nested.has_value()
            && nested->definitionId
                == std::optional<std::string>{"voice"}
            && nested->nodeId == "local",
        "expanded runtime diagnostics should navigate to their exact reusable-Sound local node");

    require(
        !talas::app::soundWorkbenchDiagnosticTarget(
             document,
             {"flattened-runtime-id", "missing/voice::local"})
             .has_value(),
        "invalid structural ancestry must not navigate to a coincidentally matching local node");
}
}

// Closing a changed Sound's editor asks
// to keep the changes; Drop gives the Sound back as it was when opened.
void testClosingTheSoundEditor()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    talas::project::ProjectSoundDefinition other = pad;
    other.definitionId = "other";
    document.soundDefinitions = {pad, other};
    const auto opened = pad;
    require(!talas::app::soundEditorChanged(opened, document), "an unchanged Sound closes without asking");
    document.soundDefinitions[0].graph.nodes.front().parameters.push_back({"frequency", std::string{"440 hz"}, {}, {}, {}, {}});
    document.soundDefinitions[1].displayName = "Other";
    require(talas::app::soundEditorChanged(opened, document), "a changed Sound asks whether to keep the changes");
    const auto dropped = talas::app::soundEditorDropped(document, opened);
    require(dropped.has_value() && dropped->soundDefinitions[0] == opened && dropped->soundDefinitions[1].displayName == "Other"
            && !talas::app::soundEditorChanged(opened, *dropped),
        "Drop should give the Sound back as it was opened, the other Sounds as they are");
    auto saved = document.soundDefinitions[0];
    require(!talas::app::soundEditorChanged(saved, document), "after a save, the Sound as saved asks nothing");
    document.soundDefinitions.erase(document.soundDefinitions.begin());
    require(!talas::app::soundEditorChanged(opened, document) && !talas::app::soundEditorDropped(document, opened).has_value(),
        "a Sound since removed has nothing to keep or drop");
}

int main()
{
    testClosingTheSoundEditor();
    testDefinitionBrowserSearchScopesAndStableSelectionHistory();
    testPerformanceControlsUseStableExposedParameterIdentity();
    testDefinitionBrowserProjectsUsageAndBreadcrumbDisplayNames();
    testCompileDiagnosticsMapToTheVisibleStructuralContext();
    testDiagnosticNavigationTargetsExactStructuralNode();
    testAuditionDocumentSubstitutesInputsWithoutMutatingTheProject();
    return 0;
}
