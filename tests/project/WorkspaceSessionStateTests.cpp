#include "talas/project/ProjectEditBoundary.h"
#include "talas/project/WorkspaceSessionFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/WorkspaceSessionState.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireErrorAt(
    std::string_view source,
    std::string_view path,
    const std::string& message)
{
    const auto loaded = talas::project::parseWorkspaceSessionState(source);
    require(!loaded.ok(), message + " should fail");
    require(
        std::ranges::any_of(
            loaded.errors,
            [&](const auto& error) {
                return error.path == path;
            }),
        message + " should report the exact schema path");
}

talas::project::WorkspaceHierarchyBookmark bookmark(std::string definitionId)
{
    return {
        definitionId,
        {definitionId},
        definitionId,
    };
}

void eraseCurrentPerformanceSession(std::string& source)
{
    const auto start = source.find("  \"performance\": {\n");
    const auto end = source.find("  \"hierarchyBookmarks\": [\n", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current Performance state");
    source.erase(start, end - start);
}

void eraseCurrentLiveGridPerformanceState(std::string& source)
{
    const auto start = source.find("    \"selectedLiveSetId\": ");
    const auto end = source.find("    \"panX\": ", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find Live Grid identities");
    source.erase(start, end - start);
    const auto mode = source.find(",\n    \"mode\": ");
    const auto objectEnd = source.find("\n  },", mode);
    require(mode != std::string::npos && objectEnd != std::string::npos,
        "legacy WorkspaceSession fixture should find Performance mode");
    source.erase(mode, objectEnd - mode);
}

void eraseCurrentLiveToolPerformanceState(std::string& source)
{
    const auto start = source.find("    \"selectedLiveToolId\": ");
    const auto end = source.find("    \"panX\": ", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current Live Tool state");
    source.erase(start, end - start);
}

void eraseCurrentAnalysisSession(std::string& source)
{
    const auto start = source.find("  \"analysis\": {\n");
    const auto end = source.find("  \"samples\": {", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current Analysis state");
    source.erase(start, end - start);
}

void eraseCurrentAccessibilitySession(std::string& source)
{
    const auto start = source.find("  \"accessibility\": {\n");
    const auto end = source.find("  \"hierarchyBookmarks\": [\n", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current accessibility preferences");
    source.erase(start, end - start);
}

void eraseCurrentSoundLibrarySession(std::string& source)
{
    const auto start = source.find("  \"soundLibrary\": {\n");
    const auto end = source.find("  \"samples\": {", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current Sound Library state");
    source.erase(start, end - start);
}

void eraseCurrentSoundLibraryOrigin(std::string& source)
{
    constexpr std::string_view origin = "    \"origin\": null,\n";
    const auto offset = source.find(origin);
    require(offset != std::string::npos,
        "legacy WorkspaceSession fixture should find current Sound Library origin");
    source.erase(offset, origin.size());
}

void eraseCurrentVariableValues(std::string& source)
{
    const auto start = source.find("  \"variableValues\": [\n");
    const auto end = source.find("  \"samples\": {", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current variable values");
    source.erase(start, end - start);
}

void eraseCurrentSoundVariableValues(std::string& source)
{
    const auto start = source.find("  \"soundVariableValues\": [\n");
    const auto end = source.find("  \"samples\": {", start);
    require(start != std::string::npos && end != std::string::npos,
        "legacy WorkspaceSession fixture should find current Sound-variable values");
    source.erase(start, end - start);
}

void testDefaultSessionHasCanonicalVersionedSchema()
{
    const talas::project::WorkspaceSessionState session;
    const std::string expected = R"json({
  "format": "talas.workspace-session",
  "version": 13,
  "viewMode": "structure",
  "paneSplit": 0.5,
  "structureSplitOrientation": "adaptive",
  "secondaryPaneVisible": true,
  "primaryPaneFirst": true,
  "graphViewMode": "portsAndWires",
  "activeGraph": {
    "kind": "project"
  },
  "structureNavigationPath": [],
  "graphContexts": [
    {
      "scope": {
        "kind": "project"
      },
      "panX": 0,
      "panY": 0,
      "zoom": 1,
      "selectedNodeId": null
    }
  ],
  "focusedPerformanceEventName": null,
  "performance": {
    "selectedControlId": null,
    "selectedMeterId": null,
    "selectedLiveSetId": null,
    "selectedLiveSetCellId": null,
    "activeLiveSetId": null,
    "activeLiveSetCellId": null,
    "queuedLiveSetId": null,
    "queuedLiveSetCellId": null,
    "queuedLiveSetRequestGeneration": 0,
    "selectedLiveToolId": null,
    "liveToolStates": [    ],
    "panX": 0,
    "panY": 0,
    "zoom": 1,
    "editMode": true,
    "compact": false,
    "mode": "controls"
  },
  "accessibility": {
    "interfaceScalePercent": 100,
    "enhancedContrast": false,
    "reduceMotion": false,
    "contextHelpVisible": false
  },
  "hierarchyBookmarks": [
  ],
  "analysis": {
    "traces": [
    ],
    "selectedTraceId": null,
    "frozen": false,
    "refreshRateHz": 30,
    "historyFrames": 120,
    "activeSection": "traces",
    "audition": null
  },
  "soundLibrary": {
    "selectedEntryId": null,
    "collection": "all",
    "sourceFilter": "all",
    "compatibilityFilter": "all",
    "healthFilter": "all",
    "category": null,
    "origin": null,
    "favoriteEntryIds": [],
    "recentEntryIds": [],
    "inputSubstitutions": [
    ],
    "detailVisible": true
  },
  "variableValues": [
  ],
  "soundVariableValues": [
  ],
  "samples": {
    "activeMapId": null,
    "selectedRegionIds": [],
    "mapViewport": {
      "noteStart": 0,
      "noteSpan": 128,
      "velocityStart": 1,
      "velocitySpan": 127
    },
    "waveformViewport": {
      "normalizedStart": 0,
      "normalizedSpan": 1
    },
    "mapWaveformSplit": 0.5
  }
}
)json";
    require(
        talas::project::serializeWorkspaceSessionState(session) == expected,
        "default WorkspaceSession should have one canonical local-only schema");
}

void testSessionCanonicalizesIdentityCollectionsAndRoundTrips()
{
    talas::project::WorkspaceSessionState session;
    session.viewMode = talas::project::WorkspaceViewMode::Samples;
    session.paneSplit = 0.625;
    session.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::TopBottom;
    session.secondaryPaneVisible = false;
    session.primaryPaneFirst = false;
    session.graphViewMode =
        talas::project::WorkspaceGraphViewMode::Ports;
    session.activeSoundDefinitionId = "voice-a";
    session.structureNavigationPath = {"voice-a", "voice-b"};
    session.graphContexts = {
        {"voice-b", -48.5, 96.25, 1.5, "osc-b"},
        {std::nullopt, 12.0, 24.0, 0.75, "root"},
        {"voice-a", 0.0, 1.0, 1.0, std::nullopt},
    };
    session.focusedPerformanceEventName = "Gain";
    session.performance.selectedControlId = "gain";
    session.performance.selectedLiveSetId = "main";
    session.performance.selectedLiveSetCellId = "intro";
    session.performance.activeLiveSetId = "main";
    session.performance.activeLiveSetCellId = "verse";
    session.performance.queuedLiveSetId = "main";
    session.performance.queuedLiveSetCellId = "chorus";
    session.performance.queuedLiveSetRequestGeneration = 42;
    session.performance.panX = 240.0;
    session.performance.panY = -96.0;
    session.performance.zoom = 1.5;
    session.performance.editMode = false;
    session.performance.compact = true;
    session.performance.mode =
        talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid;
    session.performance.selectedLiveToolId = "conductor";
    session.performance.liveToolStates.push_back({
        "conductor",
        "armed",
        "sha256:" + std::string(64, 'a'),
        {
            {"armedOnly", talas::project::ProjectLiveToolVariableKind::Boolean,
                0.0, true},
            {"gain", talas::project::ProjectLiveToolVariableKind::Number,
                0.75, false},
        },
    });
    session.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent130;
    session.accessibility.enhancedContrast = true;
    session.accessibility.reduceMotion = true;
    session.accessibility.contextHelpVisible = true;
    session.hierarchyBookmarks = {
        bookmark("voice-b"),
        bookmark("voice-a"),
    };
    session.samples.activeMapId = "keys";
    session.samples.selectedRegionIds = {"loud", "soft"};
    session.samples.mapViewport = {36.0, 48.0, 32.0, 64.0};
    session.samples.waveformViewport = {0.125, 0.25};
    session.samples.mapWaveformSplit = 0.625;
    session.analysis.traces = {
        {"root-audio", {}, {}, "root", "audioOut",
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 2, 1.5},
        {"nested-control", {"voice-instance"}, "voice", "env", {},
            talas::project::WorkspaceAnalysisTraceKind::Control, false, 5, 0.5},
    };
    session.analysis.selectedTraceId = "nested-control";
    session.analysis.frozen = true;
    session.analysis.refreshRateHz = 24;
    session.analysis.historyFrames = 240;
    session.analysis.activeSection =
        talas::project::WorkspaceAnalysisSection::Runtime;
    session.analysis.audition =
        talas::project::WorkspaceAnalysisAuditionState{
            .traceId = "root-audio",
            .destinationInputPortId = "audioIn",
            .source = talas::project::WorkspaceAnalysisTestSourceKind::Sine,
            .sineFrequencyHz = 880.0,
            .includeUpstream = false,
        };
    session.soundLibrary.selectedEntryId = "example:feedback-city";
    session.soundLibrary.collection =
        talas::project::WorkspaceSoundLibraryCollection::Favorites;
    session.soundLibrary.sourceFilter =
        talas::project::WorkspaceSoundLibrarySourceFilter::OriginalExamples;
    session.soundLibrary.compatibilityFilter =
        talas::project::WorkspaceSoundLibraryCompatibilityFilter::Ready;
    session.soundLibrary.healthFilter =
        talas::project::WorkspaceSoundLibraryHealthFilter::Healthy;
    session.soundLibrary.category = "Generative";
    session.soundLibrary.origin = "Repository original";
    session.soundLibrary.favoriteEntryIds = {
        "example:feedback-city", "project:voice-a"};
    session.soundLibrary.recentEntryIds = {
        "component:oscillator", "example:feedback-city"};
    session.soundLibrary.inputSubstitutions = {
        {"frequency", talas::project::WorkspaceSoundLibraryAuditionSource::Sine,
            880.0, std::nullopt},
        {"sample", talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset,
            0.0, "asset-a"},
    };
    session.soundLibrary.detailVisible = false;

    const auto canonical =
        talas::project::serializeWorkspaceSessionState(session);
    const auto graphContextsOffset = canonical.find(R"json("graphContexts")json");
    const auto bookmarksOffset =
        canonical.find(R"json("hierarchyBookmarks")json");
    const auto projectContextOffset =
        canonical.find(R"json("kind": "project")json", graphContextsOffset);
    const auto voiceAContextOffset =
        canonical.find(R"json("definitionId": "voice-a")json",
            graphContextsOffset);
    const auto voiceBContextOffset =
        canonical.find(R"json("definitionId": "voice-b")json",
            graphContextsOffset);
    const auto voiceABookmarkOffset =
        canonical.find(R"json("voice-a")json", bookmarksOffset);
    const auto voiceBBookmarkOffset =
        canonical.find(R"json("voice-b")json", bookmarksOffset);
    require(projectContextOffset < voiceAContextOffset &&
                voiceAContextOffset < voiceBContextOffset &&
                voiceABookmarkOffset < voiceBBookmarkOffset,
        "session codec should canonicalize graph scopes and bookmarks by "
        "stable identity");

    const auto loaded = talas::project::parseWorkspaceSessionState(canonical);
    require(loaded.ok(), "canonical WorkspaceSession should reopen");
    require(
        loaded.state.viewMode == talas::project::WorkspaceViewMode::Samples
            && loaded.state.structureSplitOrientation
                == talas::project::WorkspaceStructureSplitOrientation::TopBottom
            && !loaded.state.secondaryPaneVisible
            && !loaded.state.primaryPaneFirst
            && loaded.state.graphViewMode
                == talas::project::WorkspaceGraphViewMode::Ports
            && loaded.state.activeSoundDefinitionId
                == std::optional<std::string>{"voice-a"}
            && loaded.state.structureNavigationPath
                == std::vector<std::string>({"voice-a", "voice-b"})
            && loaded.state.graphContexts.size() == 3
            && !loaded.state.graphContexts.front()
                    .soundDefinitionId.has_value()
            && loaded.state.graphContexts[1].soundDefinitionId
                == std::optional<std::string>{"voice-a"}
            && loaded.state.graphContexts[2].selectedNodeId
                == std::optional<std::string>{"osc-b"}
            && loaded.state.focusedPerformanceEventName
                == std::optional<std::string>{"Gain"}
            && loaded.state.performance == session.performance
            && loaded.state.analysis == session.analysis
            && loaded.state.samples == session.samples
            && loaded.state.accessibility == session.accessibility
            && loaded.state.soundLibrary == session.soundLibrary
            && loaded.state.variableValues == session.variableValues
            && loaded.state.soundVariableValues
                == session.soundVariableValues,
        "session codec should preserve every local workspace field");
    require(
        talas::project::serializeWorkspaceSessionState(loaded.state)
            == canonical,
        "WorkspaceSession canonical serialization should be byte-stable");
}

void testSessionSchemaFailsClosedWithBoundedDiagnostics()
{
    requireErrorAt(R"json({"format":"talas.workspace-session","version":14})json",
        "$.version",
        "future WorkspaceSession version");
    requireErrorAt(
        R"json({"format":"talas.workspace-session","version":4.0000000000000001})json",
        "$.version",
        "mathematically fractional WorkspaceSession version that rounds in binary64");
    requireErrorAt(
        R"json({"format":"talas.workspace-session","version":1,"viewMode":"structure","paneSplit":0.5,"activeGraph":{"kind":"project"},"structureNavigationPath":[],"graphContexts":[],"focusedPerformanceEventName":null,"bookmarkedSoundDefinitions":[],"future":true})json",
        "$",
        "unknown WorkspaceSession root field");
    requireErrorAt(
        R"json({"format":"talas.workspace-session","format":"talas.workspace-session","version":1,"viewMode":"structure","paneSplit":0.5,"activeGraph":{"kind":"project"},"structureNavigationPath":[],"graphContexts":[{"scope":{"kind":"project"},"panX":0,"panY":0,"zoom":1,"selectedNodeId":null}],"focusedPerformanceEventName":null,"bookmarkedSoundDefinitions":[]})json",
        "$",
        "duplicate WorkspaceSession root field");
    requireErrorAt(
        R"json({"format":"talas.workspace-session","version":1,"viewMode":"structure","paneSplit":1.5,"activeGraph":{"kind":"project"},"structureNavigationPath":[],"graphContexts":[{"scope":{"kind":"project"},"panX":0,"panY":0,"zoom":1,"selectedNodeId":null}],"focusedPerformanceEventName":null,"bookmarkedSoundDefinitions":[]})json",
        "$.paneSplit",
        "invalid WorkspaceSession pane split");
    requireErrorAt(
        R"json({"format":"talas.workspace-session","version":1,"viewMode":"structure","paneSplit":0.5,"activeGraph":{"kind":"project"},"structureNavigationPath":[],"graphContexts":[{"scope":{"kind":"project"},"panX":0,"panY":0,"zoom":1,"selectedNodeId":null},{"scope":{"kind":"project"},"panX":1,"panY":2,"zoom":1,"selectedNodeId":null}],"focusedPerformanceEventName":null,"bookmarkedSoundDefinitions":[]})json",
        "$.graphContexts[1].scope",
        "duplicate WorkspaceSession graph scope");

    auto invalidSamples = talas::project::serializeWorkspaceSessionState({});
    const auto span = invalidSamples.find("\"normalizedSpan\": 1");
    require(span != std::string::npos, "invalid Samples fixture should find its canonical field");
    invalidSamples.replace(
        span,
        std::string("\"normalizedSpan\": 1").size(),
        "\"normalizedSpan\": 2");
    requireErrorAt(
        invalidSamples,
        "$.samples.waveformViewport",
        "out-of-range normalized waveform viewport");

    auto invalidOrientation =
        talas::project::serializeWorkspaceSessionState({});
    const auto orientation = invalidOrientation.find(
        "\"structureSplitOrientation\": \"adaptive\"");
    require(
        orientation != std::string::npos,
        "invalid split-orientation fixture should find its canonical field");
    invalidOrientation.replace(
        orientation,
        std::string(
            "\"structureSplitOrientation\": \"adaptive\"")
            .size(),
        "\"structureSplitOrientation\": \"diagonal\"");
    requireErrorAt(
        invalidOrientation,
        "$.structureSplitOrientation",
        "unknown Structure split orientation");

    auto invalidGraphView =
        talas::project::serializeWorkspaceSessionState({});
    const auto graphView = invalidGraphView.find(
        "\"graphViewMode\": \"portsAndWires\"");
    require(
        graphView != std::string::npos,
        "invalid graph-view fixture should find its canonical field");
    invalidGraphView.replace(
        graphView,
        std::string("\"graphViewMode\": \"portsAndWires\"").size(),
        "\"graphViewMode\": \"wiresOnly\"");
    requireErrorAt(
        invalidGraphView,
        "$.graphViewMode",
        "unknown graph presentation mode");

    auto invalidVisibility =
        talas::project::serializeWorkspaceSessionState({});
    const auto visibility = invalidVisibility.find(
        "\"secondaryPaneVisible\": true");
    require(
        visibility != std::string::npos,
        "invalid pane visibility fixture should find its canonical field");
    invalidVisibility.replace(
        visibility,
        std::string("\"secondaryPaneVisible\": true").size(),
        "\"secondaryPaneVisible\": 1");
    requireErrorAt(
        invalidVisibility,
        "$.secondaryPaneVisible",
        "non-boolean secondary pane visibility");

    auto invalidPaneOrder =
        talas::project::serializeWorkspaceSessionState({});
    const auto paneOrder = invalidPaneOrder.find(
        "\"primaryPaneFirst\": true");
    require(
        paneOrder != std::string::npos,
        "invalid pane-order fixture should find its canonical field");
    invalidPaneOrder.replace(
        paneOrder,
        std::string("\"primaryPaneFirst\": true").size(),
        "\"primaryPaneFirst\": \"first\"");
    requireErrorAt(
        invalidPaneOrder,
        "$.primaryPaneFirst",
        "non-boolean primary pane order");

    talas::project::WorkspaceSessionState hiddenActiveSecondary;
    hiddenActiveSecondary.secondaryPaneVisible = false;
    hiddenActiveSecondary.activeSoundDefinitionId = "voice";
    hiddenActiveSecondary.structureNavigationPath = {"voice"};
    hiddenActiveSecondary.graphContexts.push_back({
        std::optional<std::string>{"voice"},
        0.0,
        0.0,
        1.0,
        std::nullopt,
    });
    requireErrorAt(
        talas::project::serializeWorkspaceSessionState(
            hiddenActiveSecondary),
        "$.activeGraph",
        "hidden secondary pane owning the active edit scope");

    talas::project::WorkspaceSessionState bookmarked;
    bookmarked.hierarchyBookmarks = {bookmark("voice")};
    auto wrongBookmarkPath =
        talas::project::serializeWorkspaceSessionState(bookmarked);
    const auto navigation =
        wrongBookmarkPath.find("\"navigationPath\": [\"voice\"]");
    require(navigation != std::string::npos,
        "invalid bookmark-path fixture should find its canonical field");
    wrongBookmarkPath.replace(navigation,
        std::string("\"navigationPath\": [\"voice\"]").size(),
        "\"navigationPath\": [\"other\"]");
    requireErrorAt(wrongBookmarkPath,
        "$.hierarchyBookmarks[0].navigationPath",
        "hierarchy bookmark whose path does not end at its stable key");

    bookmarked.hierarchyBookmarks = {bookmark("alpha"), bookmark("beta")};
    auto duplicateBookmark =
        talas::project::serializeWorkspaceSessionState(bookmarked);
    const auto betaDefinition =
        duplicateBookmark.find("\"definitionId\": \"beta\"");
    require(betaDefinition != std::string::npos,
        "duplicate bookmark fixture should find its second stable key");
    duplicateBookmark.replace(betaDefinition,
        std::string("\"definitionId\": \"beta\"").size(),
        "\"definitionId\": \"alpha\"");
    requireErrorAt(duplicateBookmark,
        "$.hierarchyBookmarks[1].definitionId",
        "duplicate hierarchy bookmark stable key");

    auto hiddenBookmark = bookmark("voice");
    hiddenBookmark.secondaryPaneVisible = false;
    bookmarked.hierarchyBookmarks = {hiddenBookmark};
    requireErrorAt(talas::project::serializeWorkspaceSessionState(bookmarked),
        "$.hierarchyBookmarks[0].activeGraph",
        "hidden hierarchy bookmark secondary pane owning the edit scope");

    auto deepBookmark = bookmark("terminal");
    deepBookmark.navigationPath.clear();
    for (std::size_t index = 0;
        index < talas::project::maximumWorkspaceNavigationDepth;
        ++index)
    {
        deepBookmark.navigationPath.push_back("scope-" + std::to_string(index));
    }
    deepBookmark.navigationPath.push_back("terminal");
    bookmarked.hierarchyBookmarks = {std::move(deepBookmark)};
    requireErrorAt(talas::project::serializeWorkspaceSessionState(bookmarked),
        "$.hierarchyBookmarks[0].navigationPath",
        "hierarchy bookmark exceeding the bounded navigation depth");
}

void testVersionOneMigratesToEmptySamplesStateAndCurrentWriter()
{
    const auto loaded = talas::project::parseWorkspaceSessionState(R"json({
  "format": "talas.workspace-session",
  "version": 1,
  "viewMode": "performance",
  "paneSplit": 0.625,
  "activeGraph": {
    "kind": "project"
  },
  "structureNavigationPath": [],
  "graphContexts": [
    {
      "scope": {
        "kind": "project"
      },
      "panX": 12,
      "panY": 24,
      "zoom": 0.75,
      "selectedNodeId": "root"
    }
  ],
  "focusedPerformanceEventName": null,
  "bookmarkedSoundDefinitions": ["voice"]
}
)json");
    require(
        loaded.ok() && loaded.state.schemaVersion == 13 &&
            loaded.state.viewMode ==
                talas::project::WorkspaceViewMode::Performance &&
            loaded.state.samples ==
                talas::project::WorkspaceSamplesSessionState{} &&
            loaded.state.structureSplitOrientation ==
                talas::project::WorkspaceStructureSplitOrientation::Adaptive &&
            loaded.state.secondaryPaneVisible &&
            loaded.state.primaryPaneFirst &&
            loaded.state.graphViewMode ==
                talas::project::WorkspaceGraphViewMode::PortsAndWires &&
            loaded.state.hierarchyBookmarks.size() == 1 &&
            loaded.state.hierarchyBookmarks.front().definitionId == "voice" &&
            loaded.state.hierarchyBookmarks.front().navigationPath ==
                std::vector<std::string>{"voice"} &&
            loaded.state.hierarchyBookmarks.front().activeSoundDefinitionId ==
                std::optional<std::string>{"voice"} &&
            loaded.state.hierarchyBookmarks.front().paneSplit == 0.625,
        "version-one WorkspaceSession should migrate byte-independently to "
        "current local layout defaults");
    require(talas::project::serializeWorkspaceSessionState(loaded.state)
                    .find("\"version\": 13") != std::string::npos,
        "WorkspaceSession writer should emit only the current version after "
        "migration");
}

void testVersionTwoMigratesToCurrentStructureLayoutDefaults()
{
    auto versionTwo = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentVariableValues(versionTwo);
    eraseCurrentPerformanceSession(versionTwo);
    eraseCurrentAnalysisSession(versionTwo);
    const auto version = versionTwo.find("\"version\": 13");
    require(version != std::string::npos,
        "version-two migration fixture should find version");
    versionTwo.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 2");
    for (const auto& field : {
             std::string{"  \"structureSplitOrientation\": \"adaptive\",\n"},
             std::string{"  \"secondaryPaneVisible\": true,\n"},
             std::string{"  \"primaryPaneFirst\": true,\n"},
             std::string{"  \"graphViewMode\": \"portsAndWires\",\n"},
         })
    {
        const auto offset = versionTwo.find(field);
        require(offset != std::string::npos, "version-two migration fixture should find E2 field");
        versionTwo.erase(offset, field.size());
    }
    const auto hierarchyBookmarks =
        versionTwo.find("  \"hierarchyBookmarks\": [\n  ],");
    require(hierarchyBookmarks != std::string::npos,
        "version-two migration fixture should find hierarchy bookmarks");
    versionTwo.replace(hierarchyBookmarks,
        std::string("  \"hierarchyBookmarks\": [\n  ],").size(),
        "  \"bookmarkedSoundDefinitions\": [],");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionTwo);
    require(loaded.ok() && loaded.state.schemaVersion == 13 &&
                loaded.state.structureSplitOrientation ==
                    talas::project::WorkspaceStructureSplitOrientation::Adaptive &&
                loaded.state.secondaryPaneVisible &&
                loaded.state.primaryPaneFirst &&
                loaded.state.graphViewMode ==
                    talas::project::WorkspaceGraphViewMode::PortsAndWires,
        "version-two WorkspaceSession should migrate to the exact current "
        "Structure defaults");
}

void testVersionThreeMigratesLegacyBookmarksToHierarchyViews()
{
    talas::project::WorkspaceSessionState current;
    current.paneSplit = 0.6;
    current.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::TopBottom;
    current.secondaryPaneVisible = false;
    current.primaryPaneFirst = false;
    current.graphViewMode = talas::project::WorkspaceGraphViewMode::Ports;
    (void)talas::project::captureWorkspaceSessionHierarchyBookmark(current,
        "voice");
    auto versionThree = talas::project::serializeWorkspaceSessionState(current);
    eraseCurrentVariableValues(versionThree);
    eraseCurrentPerformanceSession(versionThree);
    eraseCurrentAnalysisSession(versionThree);
    const auto version = versionThree.find("\"version\": 13");
    const auto bookmarks = versionThree.find("  \"hierarchyBookmarks\": [\n");
    const auto samples = versionThree.find("  \"samples\": {", bookmarks);
    require(version != std::string::npos && bookmarks != std::string::npos &&
                samples != std::string::npos,
        "version-three migration fixture should find canonical v4 fields");
    versionThree.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 3");
    versionThree.replace(bookmarks,
        samples - bookmarks,
        "  \"bookmarkedSoundDefinitions\": [\"voice\"],\n");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionThree);
    require(
        loaded.ok() && loaded.state.schemaVersion == 13 &&
            loaded.state.hierarchyBookmarks.size() == 1 &&
            loaded.state.hierarchyBookmarks.front().definitionId == "voice" &&
            loaded.state.hierarchyBookmarks.front().navigationPath ==
                std::vector<std::string>{"voice"} &&
            loaded.state.hierarchyBookmarks.front().paneSplit == 0.6 &&
            loaded.state.hierarchyBookmarks.front().structureSplitOrientation ==
                talas::project::WorkspaceStructureSplitOrientation::TopBottom &&
            !loaded.state.hierarchyBookmarks.front().secondaryPaneVisible &&
            !loaded.state.hierarchyBookmarks.front()
                .activeSoundDefinitionId.has_value() &&
            !loaded.state.hierarchyBookmarks.front().primaryPaneFirst &&
            loaded.state.hierarchyBookmarks.front().graphViewMode ==
                talas::project::WorkspaceGraphViewMode::Ports,
        "version-three bookmark ids should migrate to complete current-layout "
        "hierarchy bookmarks");
}

void testVersionFourMigratesToDefaultPerformanceCanvasState()
{
    auto versionFour = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentVariableValues(versionFour);
    eraseCurrentPerformanceSession(versionFour);
    eraseCurrentAnalysisSession(versionFour);
    const auto version = versionFour.find("\"version\": 13");
    require(version != std::string::npos,
        "version-four migration fixture should find current version");
    versionFour.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 4");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionFour);
    require(loaded.ok()
            && loaded.state.schemaVersion == 13
            && loaded.state.performance
                == talas::project::WorkspacePerformanceSessionState{},
        "version-four WorkspaceSession should migrate to exact default "
        "Performance-canvas state");
}

void testVersionFiveMigratesToDefaultAnalysisState()
{
    auto versionFive = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveGridPerformanceState(versionFive);
    eraseCurrentVariableValues(versionFive);
    eraseCurrentAnalysisSession(versionFive);
    eraseCurrentAccessibilitySession(versionFive);
    const auto version = versionFive.find("\"version\": 13");
    require(version != std::string::npos,
        "version-five migration fixture should find current version");
    versionFive.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 5");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionFive);
    require(loaded.ok()
            && loaded.state.schemaVersion == 13
            && loaded.state.analysis
                == talas::project::WorkspaceAnalysisSessionState{},
        "version-five WorkspaceSession should migrate to exact default "
        "Analysis state");
}

void testVersionSixMigratesToDefaultAccessibilityPreferences()
{
    auto versionSix = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveGridPerformanceState(versionSix);
    eraseCurrentVariableValues(versionSix);
    eraseCurrentAccessibilitySession(versionSix);
    eraseCurrentSoundLibrarySession(versionSix);
    const auto version = versionSix.find("\"version\": 13");
    require(version != std::string::npos,
        "version-six migration fixture should find current version");
    versionSix.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 6");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionSix);
    require(loaded.ok()
            && loaded.state.schemaVersion == 13
            && loaded.state.accessibility
                == talas::project::WorkspaceAccessibilityPreferences{},
        "version-six WorkspaceSession should migrate to exact default local accessibility preferences");
}

void testVersionSevenMigratesToDefaultSoundLibraryState()
{
    auto versionSeven = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveGridPerformanceState(versionSeven);
    eraseCurrentVariableValues(versionSeven);
    eraseCurrentSoundLibrarySession(versionSeven);
    const auto version = versionSeven.find("\"version\": 13");
    require(version != std::string::npos,
        "version-seven migration fixture should find current version");
    versionSeven.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 7");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionSeven);
    require(loaded.ok()
            && loaded.state.schemaVersion == 13
            && loaded.state.soundLibrary
                == talas::project::WorkspaceSoundLibraryState{},
        "version-seven WorkspaceSession should migrate to exact default local Sound Library state");
}

void testVersionEightWithoutOriginMigratesWithoutDiscardingSession()
{
    talas::project::WorkspaceSessionState current;
    current.paneSplit = 0.64;
    current.soundLibrary.selectedEntryId = "project:voice";
    current.soundLibrary.category = "Reusable";
    current.soundLibrary.favoriteEntryIds = {"project:voice"};
    auto versionEight = talas::project::serializeWorkspaceSessionState(current);
    eraseCurrentLiveGridPerformanceState(versionEight);
    eraseCurrentVariableValues(versionEight);
    eraseCurrentSoundLibraryOrigin(versionEight);
    const auto version = versionEight.find("\"version\": 13");
    require(version != std::string::npos,
        "version-eight migration fixture should find current version");
    versionEight.replace(version,
        std::string("\"version\": 12").size(),
        "\"version\": 8");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionEight);
    require(loaded.ok()
            && loaded.state.schemaVersion == 13
            && loaded.state.paneSplit == 0.64
            && loaded.state.soundLibrary.selectedEntryId
                == std::optional<std::string>{"project:voice"}
            && loaded.state.soundLibrary.category
                == std::optional<std::string>{"Reusable"}
            && !loaded.state.soundLibrary.origin.has_value()
            && loaded.state.soundLibrary.favoriteEntryIds
                == std::vector<std::string>{"project:voice"},
        "version-eight Sound Library state should migrate without inventing origin or discarding unrelated session state");
}

talas::project::ProjectVariableDeclaration workspaceVariable(
    std::string variableId,
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar,
    double initialValue = 0.0)
{
    return {
        .variableId = std::move(variableId),
        .displayName = "Workspace variable",
        .domain = domain,
        .initialValue = initialValue,
        .owner = {talas::project::ProjectVariableOwnerKind::WorkspaceSession},
        .snapshotEligible = false,
    };
}

void testVersionNineMigratesToEmptyWorkspaceVariableValues()
{
    auto versionNine = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveGridPerformanceState(versionNine);
    eraseCurrentVariableValues(versionNine);
    const auto version = versionNine.find("\"version\": 13");
    require(version != std::string::npos,
        "version-nine migration fixture should find current version");
    versionNine.replace(
        version,
        std::string("\"version\": 12").size(),
        "\"version\": 9");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionNine);
    require(
        loaded.ok() && loaded.state.schemaVersion == 13
            && loaded.state.variableValues.empty(),
        "version-nine WorkspaceSession should migrate without inventing local variable values");
}

void testVersionTenMigratesToEmptyWorkspaceSoundVariableValues()
{
    auto versionTen = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveGridPerformanceState(versionTen);
    eraseCurrentSoundVariableValues(versionTen);
    const auto version = versionTen.find("\"version\": 13");
    require(version != std::string::npos,
        "version-ten migration fixture should find current version");
    versionTen.replace(
        version,
        std::string("\"version\": 12").size(),
        "\"version\": 10");

    const auto loaded = talas::project::parseWorkspaceSessionState(versionTen);
    require(
        loaded.ok() && loaded.state.schemaVersion == 13
            && loaded.state.soundVariableValues.empty(),
        "version-ten WorkspaceSession should migrate without inventing local Sound selections");
}

void testVersionTwelveMigratesToEmptyLiveToolState()
{
    auto versionTwelve = talas::project::serializeWorkspaceSessionState({});
    eraseCurrentLiveToolPerformanceState(versionTwelve);
    const auto version = versionTwelve.find("\"version\": 13");
    require(version != std::string::npos,
        "version-twelve migration fixture should find current version");
    versionTwelve.replace(
        version,
        std::string("\"version\": 13").size(),
        "\"version\": 12");
    const auto loaded =
        talas::project::parseWorkspaceSessionState(versionTwelve);
    require(loaded.ok() && loaded.state.schemaVersion == 13
                && !loaded.state.performance.selectedLiveToolId.has_value()
                && loaded.state.performance.liveToolStates.empty(),
            "version-twelve WorkspaceSession should migrate without inventing Live Tool runtime state");
}

talas::project::ProjectSoundVariableDeclaration workspaceSoundVariable(
    std::string variableId,
    std::vector<std::string> candidates,
    std::uint16_t initialOrdinal = 0)
{
    talas::project::ProjectSoundVariableDeclaration declaration;
    declaration.variableId = std::move(variableId);
    declaration.displayName = declaration.variableId;
    declaration.owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    declaration.kind =
        talas::project::ProjectSoundVariableKind::SoundCollection;
    declaration.candidateDefinitionIds = std::move(candidates);
    declaration.initialSelectedOrdinal = initialOrdinal;
    declaration.snapshotEligible = true;
    return declaration;
}

talas::project::ProjectLiveTool workspaceLiveTool()
{
    talas::project::ProjectLiveTool tool;
    tool.toolId = "conductor";
    tool.displayName = "Conductor";
    tool.initialStateId = "idle";
    tool.states = {{"idle", "Idle"}, {"armed", "Armed"}};
    tool.variables.push_back({
        "gain",
        "Gain",
        talas::project::ProjectLiveToolVariableKind::Number,
        0.5,
        false,
        std::nullopt,
    });
    talas::project::ProjectLiveToolTransition transition;
    transition.transitionId = "stop";
    transition.fromStateId = "armed";
    transition.toStateId = "idle";
    transition.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    transition.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    tool.transitions.push_back(std::move(transition));
    return tool;
}

void testWorkspaceLiveToolStateUsesExactDefinitionAuthority()
{
    talas::project::ProjectDocument document;
    document.liveSetLibrary.tools.push_back(workspaceLiveTool());
    const auto fingerprint =
        talas::project::workspaceSessionLiveToolDefinitionFingerprint(
            document.liveSetLibrary.tools.front());
    require(fingerprint.size() == 71 && fingerprint.starts_with("sha256:"),
        "Live Tool definition should have one canonical session authority fingerprint");

    talas::project::WorkspaceSessionState session;
    session.performance.selectedLiveToolId = "conductor";
    session.performance.liveToolStates.push_back({
        "conductor",
        "armed",
        fingerprint,
        {{"gain", talas::project::ProjectLiveToolVariableKind::Number,
            0.75, false}},
    });
    const auto canonical =
        talas::project::serializeWorkspaceSessionState(session);
    const auto loaded = talas::project::parseWorkspaceSessionState(canonical);
    require(loaded.ok()
                && loaded.state.performance.liveToolStates
                    == session.performance.liveToolStates,
            "format-13 WorkspaceSession should round-trip bounded Live Tool state exactly");

    auto reconciled = loaded.state;
    talas::project::reconcileWorkspaceSessionState(reconciled, document);
    require(reconciled.performance.liveToolStates.size() == 1,
        "unchanged Live Tool definition should retain compatible session state");
    document.liveSetLibrary.tools.front().displayName = "Changed";
    talas::project::reconcileWorkspaceSessionState(reconciled, document);
    require(reconciled.performance.liveToolStates.empty()
                && reconciled.performance.selectedLiveToolId
                    == std::optional<std::string>{"conductor"},
            "definition change should discard stale runtime values without rebinding selected stable identity");
    document.liveSetLibrary.tools.clear();
    talas::project::reconcileWorkspaceSessionState(reconciled, document);
    require(!reconciled.performance.selectedLiveToolId.has_value(),
        "removed Live Tool should clear its selected session identity");
}

void testWorkspaceSoundVariableValuesUseExactCandidateAuthority()
{
    auto tone = workspaceSoundVariable(
        "tone",
        {"toneA", "toneB"},
        1);
    auto texture = workspaceSoundVariable(
        "texture",
        {"noise", "grain"});
    const auto toneFingerprint =
        talas::project::workspaceSessionSoundVariableDeclarationFingerprint(tone);
    const auto textureFingerprint =
        talas::project::workspaceSessionSoundVariableDeclarationFingerprint(texture);
    require(
        toneFingerprint.size()
                == talas::project::workspaceVariableDeclarationFingerprintBytes
            && textureFingerprint.size()
                == talas::project::workspaceVariableDeclarationFingerprintBytes
            && toneFingerprint != textureFingerprint,
        "workspace Sound-variable fingerprints should be canonical and declaration-specific");

    auto presentationEdit = tone;
    presentationEdit.displayName = "Current tone";
    presentationEdit.snapshotEligible = false;
    require(
        talas::project::workspaceSessionSoundVariableDeclarationFingerprint(
            presentationEdit) == toneFingerprint,
        "presentation-only Sound-variable edits should retain workspace value authority");
    auto reordered = tone;
    std::ranges::reverse(reordered.candidateDefinitionIds);
    require(
        talas::project::workspaceSessionSoundVariableDeclarationFingerprint(
            reordered) != toneFingerprint,
        "candidate order changes should revoke workspace selection authority");

    talas::project::WorkspaceSessionState state;
    state.soundVariableValues = {
        {"texture", "grain", textureFingerprint},
        {"tone", "toneB", toneFingerprint},
    };
    const auto canonical = talas::project::serializeWorkspaceSessionState(state);
    const auto toneOffset = canonical.find("\"variableId\": \"tone\"");
    const auto textureOffset = canonical.find("\"variableId\": \"texture\"");
    require(textureOffset < toneOffset,
        "workspace Sound-variable values should serialize in stable identity order");
    const auto loaded = talas::project::parseWorkspaceSessionState(canonical);
    require(
        loaded.ok() && loaded.state.soundVariableValues.size() == 2
            && loaded.state.soundVariableValues.front().variableId == "texture"
            && loaded.state.soundVariableValues.back().variableId == "tone"
            && loaded.state.soundVariableValues.back().selectedDefinitionId
                == "toneB",
        "workspace Sound-variable values should reopen in canonical identity order");
    require(
        loaded.ok()
            && talas::project::serializeWorkspaceSessionState(loaded.state)
                == canonical,
        "workspace Sound-variable values should round-trip byte-stably");

    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
        {.definitionId = "noise", .displayName = "Noise"},
        {.definitionId = "grain", .displayName = "Grain"},
    };
    document.soundVariableLibrary.variables = {tone, texture};
    state.soundVariableValues = {
        {"tone", "toneB", toneFingerprint},
        {"texture", "grain", textureFingerprint},
        {"texture", "noise", textureFingerprint},
        {"missing", "toneA", "sha256:" + std::string(64, 'a')},
    };
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(
        state.soundVariableValues.size() == 1
            && state.soundVariableValues.front().variableId == "tone"
            && state.soundVariableValues.front().selectedDefinitionId
                == "toneB",
        "reconciliation should retain only one exact candidate selection for a live workspace declaration");
    document.soundVariableLibrary.variables.front()
        .candidateDefinitionIds = {"toneA"};
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.soundVariableValues.empty(),
        "candidate removal should discard stale local Sound selection authority");
}

void testWorkspaceVariableValuesRoundTripWithClosedFingerprintAuthority()
{
    auto gain = workspaceVariable(
        "gain",
        talas::expr::ValueDomain::Scalar,
        -6.0);
    auto rate = workspaceVariable(
        "rate",
        talas::expr::ValueDomain::Hertz,
        440.0);
    const auto gainFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(gain);
    const auto rateFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(rate);
    require(
        gainFingerprint.size()
                == talas::project::workspaceVariableDeclarationFingerprintBytes
            && rateFingerprint.size()
                == talas::project::workspaceVariableDeclarationFingerprintBytes
            && gainFingerprint != rateFingerprint,
        "workspace declaration fingerprints should be canonical and identity-specific");

    auto presentationOnlyEdit = gain;
    presentationOnlyEdit.displayName = "Output gain";
    presentationOnlyEdit.initialValue = -12.0;
    presentationOnlyEdit.snapshotEligible = true;
    require(
        talas::project::workspaceSessionVariableDeclarationFingerprint(
            presentationOnlyEdit)
            == gainFingerprint,
        "display, initial value, and snapshot eligibility edits should retain local value authority");
    auto domainEdit = gain;
    domainEdit.domain = talas::expr::ValueDomain::Hertz;
    require(
        talas::project::workspaceSessionVariableDeclarationFingerprint(domainEdit)
            != gainFingerprint,
        "domain edits should change local value authority");
    auto reowned = gain;
    reowned.owner.kind = talas::project::ProjectVariableOwnerKind::Project;
    require(
        talas::project::workspaceSessionVariableDeclarationFingerprint(reowned)
            .empty(),
        "non-workspace declarations should not mint workspace persistence authority");

    talas::project::WorkspaceSessionState state;
    state.variableValues = {
        {"rate", talas::expr::ValueDomain::Hertz, 880.0, rateFingerprint},
        {"gain", talas::expr::ValueDomain::Scalar, -3.0, gainFingerprint},
    };
    const auto canonical = talas::project::serializeWorkspaceSessionState(state);
    const auto gainOffset = canonical.find("\"variableId\": \"gain\"");
    const auto rateOffset = canonical.find("\"variableId\": \"rate\"");
    require(gainOffset < rateOffset,
        "workspace variable values should serialize in stable identity order");
    const auto loaded = talas::project::parseWorkspaceSessionState(canonical);
    require(
        loaded.ok() && loaded.state.variableValues.size() == 2
            && loaded.state.variableValues.front().variableId == "gain"
            && talas::project::serializeWorkspaceSessionState(loaded.state)
                == canonical,
        "workspace variable values should reopen byte-stably with exact domain and fingerprint evidence");

    auto invalidDomain = canonical;
    const auto domain = invalidDomain.find("\"domain\": \"scalar\"");
    require(domain != std::string::npos,
        "invalid-domain fixture should find its canonical token");
    invalidDomain.replace(
        domain,
        std::string("\"domain\": \"scalar\"").size(),
        "\"domain\": \"future\"");
    requireErrorAt(invalidDomain,
        "$.variableValues[0].domain",
        "unknown workspace variable domain");

    auto overlongFingerprint = canonical;
    const auto fingerprint = overlongFingerprint.find(gainFingerprint);
    require(fingerprint != std::string::npos,
        "overlong-fingerprint fixture should find its canonical authority");
    overlongFingerprint.insert(
        fingerprint + gainFingerprint.size(),
        "0");
    requireErrorAt(overlongFingerprint,
        "$",
        "overlong workspace variable fingerprint");

    auto overCapacity = talas::project::serializeWorkspaceSessionState({});
    const auto empty = overCapacity.find("  \"variableValues\": [\n  ]");
    require(empty != std::string::npos,
        "over-capacity variable fixture should find the canonical empty array");
    std::string values = "  \"variableValues\": [\n";
    for (std::size_t index = 0;
         index <= talas::project::maximumWorkspaceVariableValues;
         ++index)
    {
        values += "    {\"variableId\": \"v" + std::to_string(index)
            + "\", \"domain\": \"scalar\", \"value\": 0, "
              "\"declarationFingerprint\": \""
            + std::string(talas::project::workspaceVariableDeclarationFingerprintBytes, 'a')
            + "\"}";
        values += index == talas::project::maximumWorkspaceVariableValues
            ? "\n"
            : ",\n";
    }
    values += "  ]";
    overCapacity.replace(
        empty,
        std::string("  \"variableValues\": [\n  ]").size(),
        values);
    requireErrorAt(overCapacity,
        "$",
        "over-capacity workspace variable collection");
}

void testWorkspaceVariableReconciliationRequiresExactLiveDeclaration()
{
    auto gain = workspaceVariable(
        "gain",
        talas::expr::ValueDomain::Scalar,
        -6.0);
    auto rate = workspaceVariable(
        "rate",
        talas::expr::ValueDomain::Hertz,
        440.0);
    auto bias = workspaceVariable("bias");
    talas::project::ProjectDocument document;
    document.variableLibrary.variables = {gain, rate, bias};

    const auto gainFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(gain);
    const auto rateFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(rate);
    const auto biasFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(bias);
    talas::project::WorkspaceSessionState state;
    state.variableValues = {
        {"gain", talas::expr::ValueDomain::Scalar, -3.0, gainFingerprint},
        {"rate", talas::expr::ValueDomain::Hertz, 880.0, rateFingerprint},
        {"rate", talas::expr::ValueDomain::Hertz, 1760.0, rateFingerprint},
        {"bias", talas::expr::ValueDomain::Hertz, 0.25, biasFingerprint},
        {"missing", talas::expr::ValueDomain::Scalar, 1.0,
            "sha256:" + std::string(64, 'a')},
        {"poison", talas::expr::ValueDomain::Scalar,
            std::numeric_limits<double>::infinity(),
            "sha256:" + std::string(64, 'b')},
    };
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(
        state.variableValues.size() == 1
            && state.variableValues.front().variableId == "gain"
            && state.variableValues.front().value == -3.0,
        "reconciliation should retain only one exact live workspace declaration and drop duplicates, stale identities, wrong domains, and non-finite values");

    document.variableLibrary.variables.front().displayName = "Gain";
    document.variableLibrary.variables.front().initialValue = -18.0;
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.variableValues.size() == 1,
        "presentation and initial-value edits should retain the modified local value");
    document.variableLibrary.variables.front().variableId = "renamedGain";
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.variableValues.empty(),
        "renaming a declaration should create a new identity and drop the old local value");
}

void testSoundLibraryStateIsClosedBoundedAndReconciled()
{
    talas::project::WorkspaceSessionState state;
    state.soundLibrary.selectedEntryId = "example:feedback-city";
    state.soundLibrary.collection =
        talas::project::WorkspaceSoundLibraryCollection::Recent;
    state.soundLibrary.sourceFilter =
        talas::project::WorkspaceSoundLibrarySourceFilter::OriginalExamples;
    state.soundLibrary.compatibilityFilter =
        talas::project::WorkspaceSoundLibraryCompatibilityFilter::Ready;
    state.soundLibrary.healthFilter =
        talas::project::WorkspaceSoundLibraryHealthFilter::Healthy;
    state.soundLibrary.category = "Generative";
    state.soundLibrary.origin = "Repository original";
    state.soundLibrary.favoriteEntryIds = {
        "project:voice", "example:feedback-city"};
    state.soundLibrary.recentEntryIds = {
        "example:feedback-city", "component:oscillator"};
    state.soundLibrary.inputSubstitutions = {
        {"frequency", talas::project::WorkspaceSoundLibraryAuditionSource::Sine,
            440.0, std::nullopt},
        {"asset", talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset,
            0.0, "audio-a"},
    };
    state.soundLibrary.detailVisible = false;

    const auto canonical =
        talas::project::serializeWorkspaceSessionState(state);
    const auto loaded =
        talas::project::parseWorkspaceSessionState(canonical);
    require(loaded.ok() && loaded.state.soundLibrary == state.soundLibrary,
        "closed Sound Library state should round-trip canonically");

    auto duplicateFavorite = canonical;
    const auto favorites = duplicateFavorite.find(
        "\"favoriteEntryIds\": [\"project:voice\", \"example:feedback-city\"]");
    require(favorites != std::string::npos,
        "duplicate favorite fixture should find canonical ids");
    duplicateFavorite.replace(favorites,
        std::string("\"favoriteEntryIds\": [\"project:voice\", \"example:feedback-city\"]").size(),
        "\"favoriteEntryIds\": [\"project:voice\", \"project:voice\"]");
    requireErrorAt(duplicateFavorite,
        "$.soundLibrary.favoriteEntryIds[1]",
        "duplicate Sound Library favorite");

    auto overCapacityState = state;
    overCapacityState.soundLibrary.favoriteEntryIds.clear();
    for (std::size_t index = 0;
         index <= talas::project::maximumWorkspaceSoundLibraryFavorites;
         ++index)
    {
        overCapacityState.soundLibrary.favoriteEntryIds.push_back(
            "example:item-" + std::to_string(index));
    }
    requireErrorAt(
        talas::project::serializeWorkspaceSessionState(overCapacityState),
        "$.soundLibrary.favoriteEntryIds",
        "over-capacity Sound Library favorites");

    auto contradictory = canonical;
    const auto sine = contradictory.find(
        "\"source\": \"sine\",\n        \"value\": 440");
    require(sine != std::string::npos,
        "contradictory audition fixture should find sine source");
    contradictory.replace(sine,
        std::string("\"source\": \"sine\",\n        \"value\": 440").size(),
        "\"source\": \"silence\",\n        \"value\": 440");
    requireErrorAt(contradictory,
        "$.soundLibrary.inputSubstitutions[0].value",
        "contradictory Sound Library audition value");

    auto unknownField = canonical;
    const auto detail = unknownField.find("\"detailVisible\": false");
    require(detail != std::string::npos,
        "unknown Sound Library field fixture should find final member");
    unknownField.insert(detail, "\"invented\": true,\n    ");
    requireErrorAt(unknownField,
        "$.soundLibrary",
        "unknown Sound Library state field");

    const std::vector<std::string> currentEntries = {
        "example:feedback-city", "project:voice"};
    auto reconciled = state.soundLibrary;
    reconciled.favoriteEntryIds.push_back("missing:favorite");
    reconciled.recentEntryIds.push_back("missing:recent");
    talas::project::reconcileWorkspaceSoundLibraryState(
        reconciled,
        currentEntries);
    require(reconciled.selectedEntryId
                == std::optional<std::string>{"example:feedback-city"}
            && reconciled.favoriteEntryIds
                == std::vector<std::string>({"project:voice", "example:feedback-city"})
            && reconciled.recentEntryIds
                == std::vector<std::string>({"example:feedback-city"}),
        "Sound Library reconciliation should prune only stale projected identities");

    reconciled.selectedEntryId = "missing:selected";
    talas::project::reconcileWorkspaceSoundLibraryState(
        reconciled,
        currentEntries);
    require(!reconciled.selectedEntryId.has_value(),
        "Sound Library reconciliation should clear a stale selected identity");

    talas::project::ProjectDocument document;
    const auto projectBefore = talas::project::serializeProjectDocument(document);
    state.soundLibrary.detailVisible = !state.soundLibrary.detailVisible;
    require(talas::project::serializeProjectDocument(document) == projectBefore,
        "Sound Library workspace state must remain absent from ProjectDocument bytes");
}

void testAccessibilityPreferencesAreClosedAndLocalOnly()
{
    talas::project::WorkspaceSessionState state;
    state.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent150;
    state.accessibility.enhancedContrast = true;
    state.accessibility.reduceMotion = true;
    state.accessibility.contextHelpVisible = true;
    const auto canonical =
        talas::project::serializeWorkspaceSessionState(state);
    const auto loaded =
        talas::project::parseWorkspaceSessionState(canonical);
    require(loaded.ok()
            && loaded.state.accessibility == state.accessibility,
        "closed accessibility preferences should round-trip canonically");

    auto fractional = canonical;
    const auto scale = fractional.find(
        "\"interfaceScalePercent\": 150");
    require(scale != std::string::npos,
        "fractional scale fixture should find its canonical token");
    fractional.replace(scale,
        std::string("\"interfaceScalePercent\": 150").size(),
        "\"interfaceScalePercent\": 150.0");
    requireErrorAt(
        fractional,
        "$.accessibility.interfaceScalePercent",
        "fractional interface scale token");

    auto unknown = canonical;
    const auto knownScale = unknown.find(
        "\"interfaceScalePercent\": 150");
    unknown.replace(knownScale,
        std::string("\"interfaceScalePercent\": 150").size(),
        "\"interfaceScalePercent\": 125");
    requireErrorAt(
        unknown,
        "$.accessibility.interfaceScalePercent",
        "unsupported interface scale preset");

    auto unknownField = canonical;
    const auto help = unknownField.find(
        "\"contextHelpVisible\": true");
    require(help != std::string::npos,
        "unknown accessibility field fixture should find the final member");
    unknownField.insert(
        help,
        "\"inventedPreference\": true,\n    ");
    requireErrorAt(
        unknownField,
        "$.accessibility",
        "unknown accessibility preference");

    talas::project::ProjectDocument document;
    document.metadata.title = "Accessibility preferences remain local";
    require(talas::project::serializeProjectDocument(document).find(
                "interfaceScalePercent") == std::string::npos,
        "accessibility preferences must be structurally absent from ProjectDocument bytes");

    state.accessibility.interfaceScale =
        static_cast<talas::project::WorkspaceInterfaceScale>(125);
    const auto normalized = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(state));
    require(normalized.ok()
            && normalized.state.accessibility
                == talas::project::WorkspaceAccessibilityPreferences{},
        "an invalid in-memory accessibility authority should serialize only as closed defaults");
}

void testAnalysisSessionSchemaRejectsAmbiguousOrUnboundedTargets()
{
    talas::project::WorkspaceSessionState state;
    state.analysis.traces = {
        {"trace-a", {}, {}, "root", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 0, 1.0},
        {"trace-a", {}, {}, "other", {},
            talas::project::WorkspaceAnalysisTraceKind::Control, true, 1, 1.0},
    };
    state.analysis.selectedTraceId = "trace-a";
    requireErrorAt(talas::project::serializeWorkspaceSessionState(state),
        "$.analysis.traces[1].traceId",
        "duplicate Analysis trace id");

    state.analysis.traces = {
        {"nested", {"voice"}, {}, "osc", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 0, 1.0},
    };
    state.analysis.selectedTraceId = "nested";
    requireErrorAt(talas::project::serializeWorkspaceSessionState(state),
        "$.analysis.traces[0]",
        "nested Analysis trace without containing definition");

    state.analysis.traces.front().containingDefinitionId = "voice";
    state.analysis.traces.front().colourSlot =
        talas::project::maximumWorkspaceAnalysisColourSlot + 1;
    requireErrorAt(talas::project::serializeWorkspaceSessionState(state),
        "$.analysis.traces[0].colourSlot",
        "out-of-range Analysis colour slot");

    state.analysis.traces.front().colourSlot = 0;
    state.analysis.refreshRateHz =
        talas::project::maximumWorkspaceAnalysisRefreshRateHz + 1;
    requireErrorAt(talas::project::serializeWorkspaceSessionState(state),
        "$.analysis.refreshRateHz",
        "out-of-range Analysis refresh rate");
}

void testAnalysisAuditionSchemaIsClosedAndExactlyBound()
{
    talas::project::WorkspaceSessionState state;
    state.analysis.traces = {
        {"trace-a", {}, {}, "root", "audioOut",
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 0, 1.0},
    };
    state.analysis.audition =
        talas::project::WorkspaceAnalysisAuditionState{
            .traceId = "trace-a",
            .destinationInputPortId = "audioIn",
            .source = talas::project::WorkspaceAnalysisTestSourceKind::Sine,
            .sineFrequencyHz = 440.0,
        };
    const auto canonical =
        talas::project::serializeWorkspaceSessionState(state);
    const auto loaded =
        talas::project::parseWorkspaceSessionState(canonical);
    require(loaded.ok()
            && loaded.state.analysis.audition == state.analysis.audition,
        "Analysis audition request should round-trip canonically");

    auto staleTrace = canonical;
    const auto trace = staleTrace.find(
        R"json("traceId": "trace-a")json",
        staleTrace.find(R"json("audition")json"));
    require(trace != std::string::npos,
        "audition fixture should contain exact trace id");
    staleTrace.replace(
        trace,
        std::string{R"json("traceId": "trace-a")json"}.size(),
        R"json("traceId": "ghost")json");
    requireErrorAt(staleTrace,
        "$.analysis.audition.traceId",
        "stale audition trace identity");

    auto missingDestination = state;
    missingDestination.analysis.audition->destinationInputPortId.reset();
    requireErrorAt(
        talas::project::serializeWorkspaceSessionState(missingDestination),
        "$.analysis.audition.destinationInputPortId",
        "temporary source without exact destination");

    auto contradictoryAsset = state;
    contradictoryAsset.analysis.audition->frozenAssetId = "audio";
    requireErrorAt(
        talas::project::serializeWorkspaceSessionState(contradictoryAsset),
        "$.analysis.audition.frozenAssetId",
        "non-asset source carrying an asset identity");

    auto unknownSource = state;
    unknownSource.analysis.audition->source =
        static_cast<talas::project::WorkspaceAnalysisTestSourceKind>(255);
    talas::project::ProjectDocument unknownSourceDocument;
    unknownSourceDocument.graph.nodes.push_back(
        {"root", "talas.filter", {}});
    talas::project::reconcileWorkspaceSessionState(
        unknownSource, unknownSourceDocument);
    require(unknownSource.analysis.traces.size() == 1
            && !unknownSource.analysis.audition.has_value(),
        "reconciliation should clear an unknown Analysis source kind");

    auto rejectedTrace = state;
    rejectedTrace.analysis.traces.front().instancePath = {"voice"};
    rejectedTrace.analysis.traces.front().containingDefinitionId.reset();
    requireErrorAt(
        talas::project::serializeWorkspaceSessionState(rejectedTrace),
        "$.analysis.audition.traceId",
        "audition identity referencing a structurally rejected trace");
}

void testWorkspaceSessionPreservesVisibleStructureContextsExplicitly()
{
    talas::project::WorkspaceSessionState rootOnly;
    rootOnly.graphContexts.push_back(
        {"voice-a", 8.0, 9.0, 1.25, "osc-a"});
    rootOnly.graphContexts.push_back(
        {"voice-b", 18.0, 19.0, 0.75, "osc-b"});

    const auto reopenedRootOnly = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(rootOnly));
    require(reopenedRootOnly.ok() &&
                reopenedRootOnly.state.structureNavigationPath.empty() &&
                !reopenedRootOnly.state.activeSoundDefinitionId.has_value() &&
                reopenedRootOnly.state.graphContexts.size() == 3,
        "root-only visibility should survive reopen without discarding "
        "cached graph transforms or resurrecting a definition pane");

    auto rootWithSecondary = rootOnly;
    rootWithSecondary.structureNavigationPath = {"voice-b"};
    const auto reopenedSplit = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(rootWithSecondary));
    require(
        reopenedSplit.ok()
            && reopenedSplit.state.structureNavigationPath
                == std::vector<std::string>{"voice-b"}
            && !reopenedSplit.state.activeSoundDefinitionId.has_value(),
        "root-active with a visible secondary definition should remain distinct from root-only visibility");
}

void testWorkspaceSessionBuildersCannotExceedCodecOnlyCollectionLimits()
{
    talas::project::WorkspaceSessionState state;
    for (int index = 0; index < 600; ++index)
    {
        (void) talas::project::ensureWorkspaceGraphSessionState(
            state,
            "voice-" + std::to_string(index));
    }
    std::vector<std::string> bookmarkIds;
    for (int index = 0; index < 300; ++index)
        bookmarkIds.push_back("bookmark-" + std::to_string(index));
    const auto allBookmarksRetained =
        talas::project::replaceWorkspaceSessionHierarchyBookmarks(state,
            [&]
            {
                std::vector<talas::project::WorkspaceHierarchyBookmark> result;
                result.reserve(bookmarkIds.size());
                for (auto& definitionId : bookmarkIds)
                    result.push_back(bookmark(std::move(definitionId)));
                return result;
            }());

    const auto reopened = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(state));
    require(reopened.ok() &&
                reopened.state.graphContexts.size() ==
                    talas::project::maximumWorkspaceGraphContexts &&
                reopened.state.hierarchyBookmarks.size() ==
                    talas::project::maximumWorkspaceBookmarks &&
                !allBookmarksRetained,
        "public WorkspaceSession builders should apply the same bounded "
        "collection policy as the codec");

    auto invalid = bookmark("invalid");
    invalid.navigationPath = {"ancestor", "invalid"};
    invalid.secondaryPaneVisible = false;
    auto duplicatePath = bookmark("duplicate");
    duplicatePath.navigationPath = {"duplicate", "duplicate"};
    auto invalidSplit = bookmark("split");
    invalidSplit.paneSplit = 1.0;
    require(!talas::project::replaceWorkspaceSessionHierarchyBookmarks(state,
                {invalid, duplicatePath, invalidSplit}) &&
                state.hierarchyBookmarks.empty(),
        "public hierarchy bookmark builder should discard structurally "
        "impossible or unbounded views");
}

void testHierarchyBookmarksRoundTripAndRestoreCompleteWorkspaceView()
{
    talas::project::WorkspaceSessionState state;
    state.structureNavigationPath = {"parent", "child"};
    state.activeSoundDefinitionId = "child";
    state.paneSplit = 0.625;
    state.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::TopBottom;
    state.secondaryPaneVisible = true;
    state.primaryPaneFirst = false;
    state.graphViewMode = talas::project::WorkspaceGraphViewMode::Ports;
    (void)talas::project::ensureWorkspaceGraphSessionState(state, "parent");
    auto& child = talas::project::ensureWorkspaceGraphSessionState(state, "child");
    child.panX = 18.0;
    child.zoom = 1.5;
    child.selectedNodeId = "osc";

    require(
        talas::project::captureWorkspaceSessionHierarchyBookmark(state, "child"),
        "current hierarchy view should be capturable as a bounded bookmark");
    const auto reopened = talas::project::parseWorkspaceSessionState(
        talas::project::serializeWorkspaceSessionState(state));
    require(reopened.ok() && reopened.state.hierarchyBookmarks.size() == 1 &&
                reopened.state.hierarchyBookmarks.front().navigationPath ==
                    std::vector<std::string>({"parent", "child"}),
        "hierarchy bookmark should survive a byte-stable WorkspaceSession v4 "
        "save/reopen");

    auto restored = reopened.state;
    restored.structureNavigationPath.clear();
    restored.activeSoundDefinitionId.reset();
    restored.paneSplit = 0.4;
    restored.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::LeftRight;
    restored.primaryPaneFirst = true;
    restored.graphViewMode = talas::project::WorkspaceGraphViewMode::Compact;
    require(
        talas::project::restoreWorkspaceSessionHierarchyBookmark(restored,
            "child") &&
            restored.viewMode == talas::project::WorkspaceViewMode::Structure &&
            restored.structureNavigationPath ==
                std::vector<std::string>({"parent", "child"}) &&
            restored.activeSoundDefinitionId ==
                std::optional<std::string>{"child"} &&
            restored.paneSplit == 0.625 &&
            restored.structureSplitOrientation ==
                talas::project::WorkspaceStructureSplitOrientation::TopBottom &&
            !restored.primaryPaneFirst &&
            restored.graphViewMode ==
                talas::project::WorkspaceGraphViewMode::Ports &&
            talas::project::findWorkspaceGraphSessionState(restored,
                std::optional<std::string_view>{"child"})
                    ->selectedNodeId == std::optional<std::string>{"osc"},
        "bookmark restore should recover hierarchy, split configuration, "
        "active scope, and cached per-scope edit context");
    require(talas::project::removeWorkspaceSessionHierarchyBookmark(restored,
                "child") &&
                restored.hierarchyBookmarks.empty() &&
                !talas::project::restoreWorkspaceSessionHierarchyBookmark(restored,
                    "child"),
        "hierarchy bookmark removal should be exact and stale restore should "
        "fail closed");
}

void testWorkspaceSessionNeverSerializesIntoOrDirtiesProject()
{
    talas::project::ProjectDocument document;
    const talas::project::ProjectEditBoundary boundary(document);
    const auto projectBytes = talas::project::serializeProjectDocument(document);

    talas::project::WorkspaceSessionState session;
    session.viewMode = talas::project::WorkspaceViewMode::Performance;
    session.paneSplit = 0.7;
    session.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::LeftRight;
    session.secondaryPaneVisible = false;
    session.primaryPaneFirst = false;
    session.graphViewMode =
        talas::project::WorkspaceGraphViewMode::Compact;
    session.graphContexts.front().panX = 480.0;
    session.graphContexts.front().zoom = 1.25;
    session.graphContexts.front().selectedNodeId = "local-selection";
    session.focusedPerformanceEventName = "Gain";
    session.hierarchyBookmarks = {bookmark("voice")};

    require(talas::project::serializeProjectDocument(document) == projectBytes &&
                !boundary.isDirty(document),
        "WorkspaceSession mutations must be structurally incapable of "
        "changing project bytes or dirty state");
    require(talas::project::serializeWorkspaceSessionState(session).find(
                "local-selection") != std::string::npos,
        "local session codec should still retain the independent mutation");
}

void testWorkspaceSessionReconcilesOnlyAgainstStableProjectIdentities()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"root", "talas.oscillator", {}, {}});
    document.graph.nodes.push_back({
        "voice-instance",
        "talas.sound",
        {},
        std::optional<std::string>{"voice"},
    });
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({"osc", "talas.oscillator", {}, {}});
    document.soundDefinitions.push_back(std::move(voice));
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.5,
        0.0,
        1.0,
        {},
    });
    talas::project::reconcileProjectPresentation(document);
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {640, 96}, true});
    talas::project::ProjectAsset asset;
    asset.id = "piano";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "piano.wav";
    asset.frameCount = 100;
    document.assets.push_back(asset);
    talas::project::ProjectSampleRegion region;
    region.regionId = "kept";
    region.assetId = "piano";
    document.sampleMapLibrary.maps.push_back({
        "keys",
        "Keys",
        {region},
    });

    talas::project::WorkspaceSessionState state;
    state.activeSoundDefinitionId = "removed";
    state.graphContexts = {
        {"removed", 8.0, 9.0, 1.0, "ghost"},
        {std::nullopt, 1.0, 2.0, 1.25, "root"},
        {"voice", 3.0, 4.0, 0.75, "osc"},
        {"voice", 99.0, 99.0, 2.0, "duplicate"},
    };
    state.focusedPerformanceEventName = "Removed Event";
    state.performance.selectedControlId = "removed-control";
    state.performance.selectedMeterId = "removed-meter";
    state.hierarchyBookmarks = {
        bookmark("removed"),
        bookmark("voice"),
        bookmark("voice"),
    };
    state.viewMode = talas::project::WorkspaceViewMode::Samples;
    state.samples.activeMapId = "removed-map";
    state.samples.selectedRegionIds = {"ghost", "kept", "kept"};
    state.analysis.traces = {
        {"root-trace", {}, {}, "root", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 0, 1.0},
        {"voice-trace", {"voice-instance"}, "voice", "osc", {},
            talas::project::WorkspaceAnalysisTraceKind::Control, true, 1, 1.0},
        {"ghost-trace", {"ghost-instance"}, "voice", "osc", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 2, 1.0},
        {"wrong-definition", {"voice-instance"}, "removed", "osc", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 3, 1.0},
        {"duplicate-target", {}, {}, "root", {},
            talas::project::WorkspaceAnalysisTraceKind::Audio, true, 4, 1.0},
    };
    state.analysis.selectedTraceId = "ghost-trace";
    state.analysis.audition =
        talas::project::WorkspaceAnalysisAuditionState{
            .traceId = "ghost-trace",
            .destinationInputPortId = "audioIn",
            .source =
                talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset,
            .frozenAssetId = "missing-audio",
        };

    talas::project::reconcileWorkspaceSessionState(state, document);
    require(!state.activeSoundDefinitionId.has_value() &&
                state.graphContexts.size() == 2 &&
                !state.graphContexts[0].soundDefinitionId.has_value() &&
                state.graphContexts[0].selectedNodeId ==
                    std::optional<std::string>{"root"} &&
                state.graphContexts[1].soundDefinitionId ==
                    std::optional<std::string>{"voice"} &&
                state.graphContexts[1].selectedNodeId ==
                    std::optional<std::string>{"osc"},
        "WorkspaceSession reconciliation should remove stale scopes, "
        "preserve stable selections, and collapse duplicates");
    require(!state.focusedPerformanceEventName.has_value()
                && !state.performance.selectedControlId.has_value()
                && !state.performance.selectedMeterId.has_value() &&
                talas::project::workspaceSessionBookmarkedDefinitionIds(state) ==
                    std::vector<std::string>{"voice"},
        "WorkspaceSession reconciliation should remove stale focus and "
        "Performance selection and bookmark identities");
    require(state.samples.activeMapId == std::optional<std::string>{"keys"} &&
                state.samples.selectedRegionIds.empty(),
        "Samples reconciliation should choose a deterministic surviving map "
        "and remove selections from the stale map");
    require(state.analysis.traces.size() == 2
            && state.analysis.traces[0].traceId == "root-trace"
            && state.analysis.traces[1].traceId == "voice-trace"
            && !state.analysis.selectedTraceId.has_value()
            && !state.analysis.audition.has_value(),
        "Analysis reconciliation should retain exact root/nested targets, "
        "drop stale or duplicate targets, and clear stale selection");

    state.samples.selectedRegionIds = {"ghost", "kept", "kept"};
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(
        state.samples.selectedRegionIds
            == std::vector<std::string>{"kept"},
        "Samples reconciliation should retain only unique stable region identities from the active map");

    state.performance.selectedControlId =
        document.presentation.performanceControls.front().controlId;
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.performance.selectedControlId
                == std::optional<std::string>{
                    document.presentation.performanceControls.front().controlId},
        "Performance reconciliation should retain a stable control selection");
    state.performance.selectedControlId.reset();
    state.performance.selectedMeterId = "master-left";
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.performance.selectedMeterId
                == std::optional<std::string>{"master-left"},
        "Performance reconciliation should retain a stable meter selection");

    auto& created = talas::project::ensureWorkspaceGraphSessionState(
        state,
        std::optional<std::string>{"bass"});
    created.panX = 12.0;
    const auto* found = talas::project::findWorkspaceGraphSessionState(
        state,
        std::optional<std::string_view>{"bass"});
    require(found != nullptr && found->panX == 12.0,
        "WorkspaceSession graph helpers should address contexts only by "
        "stable scope identity");
}

void testWorkspaceSessionReconcilesCanonicalHierarchyEdges()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition parent;
    parent.definitionId = "parent";
    parent.graph.nodes.push_back({
        "child-instance",
        "talas.sound",
        {},
        std::optional<std::string>{"child"},
    });
    talas::project::ProjectSoundDefinition child;
    child.definitionId = "child";
    talas::project::ProjectSoundDefinition orphan;
    orphan.definitionId = "orphan";
    document.soundDefinitions = {parent, child, orphan};

    talas::project::WorkspaceSessionState state;
    state.structureNavigationPath = {"parent", "child"};
    state.activeSoundDefinitionId = "child";
    auto childBookmark = bookmark("child");
    childBookmark.navigationPath = {"parent", "child"};
    state.hierarchyBookmarks = {childBookmark};
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.structureNavigationPath ==
                    std::vector<std::string>({"parent", "child"}) &&
                state.hierarchyBookmarks.front().navigationPath ==
                    std::vector<std::string>({"parent", "child"}),
        "reconciliation should preserve a real parent-to-child Sound "
        "hierarchy");

    document.soundDefinitions.front().graph.nodes.clear();
    state.structureNavigationPath = {"parent", "child"};
    state.activeSoundDefinitionId = "child";
    state.hierarchyBookmarks.front().navigationPath = {"parent", "child"};
    talas::project::reconcileWorkspaceSessionState(state, document);
    require(state.structureNavigationPath ==
                    std::vector<std::string>{"parent"} &&
                state.activeSoundDefinitionId ==
                    std::optional<std::string>{"parent"} &&
                state.hierarchyBookmarks.front().navigationPath ==
                    std::vector<std::string>{"child"} &&
                state.hierarchyBookmarks.front().activeSoundDefinitionId ==
                    std::optional<std::string>{"child"},
        "reconciliation should truncate a stale live hierarchy edge and "
        "repair a bookmark to its surviving stable definition");
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-workspace-session-tests-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void testWorkspaceSessionFilePathIsStableLocalAndProjectScoped()
{
    const TemporaryDirectory temporary;
    const auto first = talas::project::workspaceSessionFilePathForProject(
        temporary.path(),
        "/projects/Album/Voice.talas.json");
    const auto normalizedFirst =
        talas::project::workspaceSessionFilePathForProject(
            temporary.path(),
            "/projects/Album/../Album/Voice.talas.json");
    const auto second = talas::project::workspaceSessionFilePathForProject(
        temporary.path(),
        "/projects/Album/Bass.talas.json");

    require(first.has_value() && first == normalizedFirst &&
                second.has_value() && first != second,
        "WorkspaceSession storage should be stable per normalized project "
        "identity");
    require(first->parent_path() == temporary.path() &&
                first->extension() == ".json" &&
                first->filename().string().find("Voice") == std::string::npos,
        "WorkspaceSession storage should stay in the local state root "
        "without leaking project paths");
    require(
        !talas::project::workspaceSessionFilePathForProject(
             temporary.path(),
             {})
             .has_value(),
        "WorkspaceSession storage should reject an empty project identity");
}

void testWorkspaceSessionFileRoundTripsCanonicalStateAtomically()
{
    const TemporaryDirectory temporary;
    const auto path = temporary.path() / "session.json";
    talas::project::WorkspaceSessionState state;
    state.viewMode = talas::project::WorkspaceViewMode::Performance;
    state.activeSoundDefinitionId = "voice";
    state.structureNavigationPath = {"voice"};
    state.graphContexts.push_back({"voice", 16.0, -32.0, 1.25, "osc"});
    state.hierarchyBookmarks = {bookmark("voice")};
    state.performance.selectedLiveToolId = "conductor";
    state.performance.liveToolStates.push_back({
        "conductor",
        "armed",
        "sha256:" + std::string(64, 'c'),
        {{"enabled",
            talas::project::ProjectLiveToolVariableKind::Boolean,
            0.0,
            true}},
    });

    const auto saved = talas::project::saveWorkspaceSessionFile(path, state);
    require(saved.ok(), "valid WorkspaceSession should save locally");
    const auto loaded = talas::project::loadWorkspaceSessionFile(path);
    require(
        loaded.ok() && loaded.state == state,
        "saved WorkspaceSession should reopen without losing local fields");

    state.paneSplit = 0.6;
    state.graphContexts.front().panX = 48.0;
    const auto replaced =
        talas::project::saveWorkspaceSessionFile(path, state);
    const auto replacement =
        talas::project::loadWorkspaceSessionFile(path);
    require(
        replaced.ok() && replacement.ok()
            && replacement.state == state,
        "WorkspaceSession should replace an existing local record failure-atomically");

    std::ifstream input(path, std::ios::binary);
    const std::string bytes(
        std::istreambuf_iterator<char>{input},
        {});
    require(
        bytes == talas::project::serializeWorkspaceSessionState(state),
        "WorkspaceSession file should contain canonical codec bytes");
    require(
        !std::filesystem::exists(path.string() + ".tmp")
            && !std::filesystem::exists(path.string() + ".bak"),
        "successful WorkspaceSession replacement should leave no transient artifacts");
}

void testWorkspaceSessionLoadsPreservedBackupAfterInterruptedReplacement()
{
    const TemporaryDirectory temporary;
    const auto path = temporary.path() / "session.json";
    auto backupPath = path;
    backupPath += ".bak";

    talas::project::WorkspaceSessionState state;
    state.paneSplit = 0.64;
    state.graphContexts.front().panY = -72.0;
    require(
        talas::project::saveWorkspaceSessionFile(
            backupPath,
            state)
            .ok(),
        "interrupted-replacement backup fixture should save");

    const auto recovered =
        talas::project::loadWorkspaceSessionFile(path);
    require(
        recovered.ok() && recovered.state == state,
        "missing primary WorkspaceSession should recover its preserved backup");
}

void testWorkspaceSessionWriterRejectsOversizedStateBeforeReplacingPriorBytes()
{
    const TemporaryDirectory temporary;
    const auto path = temporary.path() / "session.json";
    const talas::project::WorkspaceSessionState original;
    require(
        talas::project::saveWorkspaceSessionFile(path, original).ok(),
        "oversized writer fixture should save its original state");

    auto oversized = original;
    oversized.focusedPerformanceEventName = std::string(
        talas::project::maximumWorkspaceSessionFileBytes + 1,
        'x');
    const auto rejected =
        talas::project::saveWorkspaceSessionFile(path, oversized);
    const auto preserved =
        talas::project::loadWorkspaceSessionFile(path);
    require(
        !rejected.ok() &&
            rejected.error->code ==
                talas::project::WorkspaceSessionFileErrorCode::InvalidSession &&
            preserved.ok() && preserved.state == original,
        "writer should reject a loader-oversized session before replacing "
        "the last readable state");
}

void testWorkspaceSessionFileFailureReturnsDefaultWithoutBlockingProjectState()
{
    const TemporaryDirectory temporary;
    const auto path = temporary.path() / "session.json";
    {
        std::ofstream output(path, std::ios::binary);
        output << R"json({"format":"talas.workspace-session","version":14})json";
    }

    const auto loaded = talas::project::loadWorkspaceSessionFile(path);
    require(
        !loaded.ok()
            && loaded.error.has_value()
            && loaded.error->code
                == talas::project::WorkspaceSessionFileErrorCode::InvalidSession
            && loaded.state == talas::project::WorkspaceSessionState{}
            && std::ranges::any_of(
                loaded.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.path == "$.version";
                }),
        "future local WorkspaceSession should fail closed to defaults with a bounded exact diagnostic");

    talas::project::ProjectDocument document;
    document.metadata.title = "Project remains available";
    require(
        talas::project::serializeProjectDocument(document).find(
            "Project remains available") != std::string::npos,
        "invalid local WorkspaceSession handling must remain independent of project state");

    const auto missing = talas::project::loadWorkspaceSessionFile(
        temporary.path() / "missing.json");
    require(
        missing.missing()
            && missing.state == talas::project::WorkspaceSessionState{},
        "missing local WorkspaceSession should select harmless defaults");
}
}

int main()
{
    testDefaultSessionHasCanonicalVersionedSchema();
    testSessionCanonicalizesIdentityCollectionsAndRoundTrips();
    testSessionSchemaFailsClosedWithBoundedDiagnostics();
    testVersionOneMigratesToEmptySamplesStateAndCurrentWriter();
    testVersionTwoMigratesToCurrentStructureLayoutDefaults();
    testVersionThreeMigratesLegacyBookmarksToHierarchyViews();
    testVersionFourMigratesToDefaultPerformanceCanvasState();
    testVersionFiveMigratesToDefaultAnalysisState();
    testVersionSixMigratesToDefaultAccessibilityPreferences();
    testVersionSevenMigratesToDefaultSoundLibraryState();
    testVersionEightWithoutOriginMigratesWithoutDiscardingSession();
    testVersionNineMigratesToEmptyWorkspaceVariableValues();
    testVersionTenMigratesToEmptyWorkspaceSoundVariableValues();
    testVersionTwelveMigratesToEmptyLiveToolState();
    testWorkspaceVariableValuesRoundTripWithClosedFingerprintAuthority();
    testWorkspaceVariableReconciliationRequiresExactLiveDeclaration();
    testWorkspaceLiveToolStateUsesExactDefinitionAuthority();
    testWorkspaceSoundVariableValuesUseExactCandidateAuthority();
    testSoundLibraryStateIsClosedBoundedAndReconciled();
    testAccessibilityPreferencesAreClosedAndLocalOnly();
    testAnalysisSessionSchemaRejectsAmbiguousOrUnboundedTargets();
    testAnalysisAuditionSchemaIsClosedAndExactlyBound();
    testWorkspaceSessionPreservesVisibleStructureContextsExplicitly();
    testWorkspaceSessionBuildersCannotExceedCodecOnlyCollectionLimits();
    testHierarchyBookmarksRoundTripAndRestoreCompleteWorkspaceView();
    testWorkspaceSessionNeverSerializesIntoOrDirtiesProject();
    testWorkspaceSessionReconcilesOnlyAgainstStableProjectIdentities();
    testWorkspaceSessionReconcilesCanonicalHierarchyEdges();
    testWorkspaceSessionFilePathIsStableLocalAndProjectScoped();
    testWorkspaceSessionFileRoundTripsCanonicalStateAtomically();
    testWorkspaceSessionLoadsPreservedBackupAfterInterruptedReplacement();
    testWorkspaceSessionWriterRejectsOversizedStateBeforeReplacingPriorBytes();
    testWorkspaceSessionFileFailureReturnsDefaultWithoutBlockingProjectState();
    return 0;
}
