#include "talas/cli/Cli.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditBoundary.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/project/WorkspaceSessionState.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef TALAS_SOURCE_DIR
#error "TALAS_SOURCE_DIR must identify the repository root"
#endif

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(input.good(), "M4 fixture should be readable");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

int runCli(
    std::vector<std::string> arguments,
    std::string& output,
    std::string& error)
{
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);
    std::ostringstream standardOutput;
    std::ostringstream standardError;
    const auto result = talas::cli::runCli(
        views,
        standardOutput,
        standardError);
    output = standardOutput.str();
    error = standardError.str();
    return result;
}

void testOriginalFormatThirteenStateLayerFixture()
{
    const auto path = std::filesystem::path{TALAS_SOURCE_DIR}
        / "examples"
        / "m4_state_layers.talas.json";
    const auto source = readText(path);
    const auto loaded = talas::project::parseProjectDocument(source);
    require(loaded.ok(), "M4 format-13 fixture should parse");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion
            && loaded.document.graph.nodes.size() == 2
            && loaded.document.soundDefinitions.size() == 1,
        "M4 fixture should retain independent executable topology");
    require(
        loaded.document.presentation.structureLayouts.size() == 2
            && loaded.document.presentation.performanceControls.size()
                == 1
            && loaded.document.presentation.structureLayouts[1]
                   .soundDefinitionId
                == std::optional<std::string>{
                    "stateVoiceDefinition"},
        "M4 fixture should retain root, nested, and Performance presentation identities");
    require(
        loaded.document.snapshotLibrary.schemaVersion == 1,
        "M4 fixture should retain the empty version-1 snapshot boundary");
    require(
        source.find("\"banks\": []") != std::string::npos
            && source.find("\"position\"") == std::string::npos
            && source.find("\"paneSplit\"")
                == std::string::npos
            && source.find("\"graphContexts\"")
                == std::string::npos,
        "M4 project bytes should contain portable presentation but no legacy or local workspace fields");
    const auto canonicalSource =
        talas::project::serializeProjectDocument(loaded.document);
    require(
        canonicalSource.find(R"json("version": 156)json")
                != std::string::npos
            && canonicalSource != source,
        "M4 format-thirteen fixture should migrate to canonical format-32 bytes");

    const talas::project::ProjectEditBoundary editBoundary(
        loaded.document);
    auto session = talas::project::WorkspaceSessionState{};
    session.paneSplit = 0.68;
    session.activeSoundDefinitionId =
        "stateVoiceDefinition";
    session.graphContexts.push_back({
        "stateVoiceDefinition",
        -48.0,
        96.0,
        1.4,
        "oscillator",
    });
    session.focusedPerformanceEventName = "StateGain";
    session.structureNavigationPath = {"stateVoiceDefinition"};
    (void)talas::project::captureWorkspaceSessionHierarchyBookmark(session,
        "stateVoiceDefinition");
    talas::project::reconcileWorkspaceSessionState(session, loaded.document);
    const auto sessionBytes =
        talas::project::serializeWorkspaceSessionState(session);
    require(
        sessionBytes.find("\"paneSplit\": 0.68")
                != std::string::npos
            && sessionBytes.find("stateVoiceDefinition")
                != std::string::npos
            && talas::project::serializeProjectDocument(
                   loaded.document)
                == canonicalSource
            && !editBoundary.isDirty(loaded.document),
        "local WorkspaceSession projection must not enter project bytes or dirty state");

    std::string cliOutput;
    std::string cliError;
    require(
        runCli(
            {"validate", "--project", path.string()},
            cliOutput,
            cliError)
                == 0
            && cliOutput == "OK\n"
            && cliError.empty(),
        "M4 state-layer fixture should pass ordinary CLI validation");
}

void testOriginalPopulatedSnapshotPerformanceFixture()
{
    const auto path = std::filesystem::path{TALAS_SOURCE_DIR}
        / "examples"
        / "m4_snapshot_performance.talas.json";
    const auto source = readText(path);
    const auto loaded = talas::project::parseProjectDocument(source);
    require(loaded.ok(), "M4 populated snapshot fixture should parse");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion &&
            loaded.document.snapshotLibrary.schemaVersion == 1 &&
            loaded.document.snapshotLibrary.banks.size() == 1 &&
            loaded.document.snapshotLibrary.banks.front().snapshots.size() ==
                2 &&
            loaded.document.snapshotLibrary.banks.front()
                    .parameterPolicies.size() == 1,
        "M4 populated fixture should retain one complete bank, two "
        "snapshots, and one policy");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(
        talas::project::snapshotLibraryDiagnostics(
            loaded.document,
            registry)
            .empty(),
        "M4 populated fixture should resolve every stable snapshot target");
    const auto canonical =
        talas::project::serializeProjectDocument(loaded.document);
    require(
        canonical.find(R"json("version": 156)json")
                != std::string::npos
            && canonical != source,
        "M4 populated format-thirteen fixture should migrate canonically; canonical form:\n"
            + canonical);

    talas::runtime::ProjectRuntimeBridge bridge(
        loaded.document,
        registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 8;
    const auto staged = bridge.compileAndStage(std::move(settings));
    require(
        staged.ok(),
        "M4 populated fixture should stage its prepared baseline: "
            + staged.message);
    bridge.setPlaying(true);
    std::array<float, 8> output{};
    auto* outputPointer = output.data();
    require(
        bridge.render(&outputPointer, 1, output.size()),
        "M4 populated fixture should activate its prepared baseline");

    const auto authoringBytes =
        talas::project::serializeProjectDocument(
            bridge.editor().document());
    const auto recalled = bridge.recallSnapshot(
        "voice-pitch",
        "full");
    const auto snapshotPitch = [](const auto& state) {
        const auto found = std::ranges::find_if(
            state.currentValues,
            [](const auto& entry) {
                return entry.target.kind
                        == talas::project::SnapshotTargetKind::SoundParameter
                    && entry.target.instancePath
                        == std::vector<std::string>{"snapshotVoice"}
                    && entry.target.definitionId
                        == "snapshotVoiceDefinition"
                    && entry.target.parameterId == "frequency";
            });
        return found == state.currentValues.end()
            ? static_cast<const talas::project::SnapshotValueEntry*>(nullptr)
            : std::addressof(*found);
    };
    const auto* recalledPitch = snapshotPitch(
        bridge.performanceState());
    require(
        recalled.outcome
                == talas::runtime::SnapshotPerformanceActionOutcome::
                    AppliedDirect
            && bridge.performanceState().activeSnapshotId == "full"
            && recalledPitch != nullptr
            && std::abs(recalledPitch->value.number - 440.0)
                < 1.0e-12
            && talas::project::serializeProjectDocument(
                   bridge.editor().document()) == authoringBytes,
        "M4 populated fixture recall should use one live direct transaction without editing project bytes: outcome="
            + std::to_string(static_cast<int>(recalled.outcome))
            + "; message=" + recalled.message
            + "; active="
            + bridge.performanceState().activeSnapshotId
            + "; values="
            + std::to_string(
                bridge.performanceState().currentValues.size()));
    require(
        bridge.beginSnapshotMorph(
            "voice-pitch",
            "soft",
            "full")
                .ok()
            && bridge.updateSnapshotMorph(0.5).ok()
            && snapshotPitch(bridge.performanceState()) != nullptr
            && std::abs(
                   snapshotPitch(bridge.performanceState())
                           ->value.number
                       - 330.0)
                < 1.0e-12,
        "M4 populated fixture should morph continuously through the exact midpoint");
    bridge.endSnapshotMorph();
    require(
        bridge.randomizeSnapshotBank(
            "voice-pitch",
            0x2a)
                .ok()
            && bridge.performanceState().lastRandomSeed == 0x2a
            && snapshotPitch(bridge.performanceState()) != nullptr
            && snapshotPitch(bridge.performanceState())
                       ->value.number
                >= 110.0
            && snapshotPitch(bridge.performanceState())
                       ->value.number
                <= 880.0
            && talas::project::serializeProjectDocument(
                   bridge.editor().document()) == authoringBytes,
        "M4 populated fixture should randomize reproducibly inside its explicit range without editing project bytes");

    std::string cliOutput;
    std::string cliError;
    require(
        runCli(
            {"validate", "--project", path.string()},
            cliOutput,
            cliError)
                == 0
            && cliOutput == "OK\n"
            && cliError.empty(),
        "M4 populated snapshot fixture should pass ordinary CLI validation");
}
}

int main()
{
    try
    {
        testOriginalFormatThirteenStateLayerFixture();
        testOriginalPopulatedSnapshotPerformanceFixture();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
