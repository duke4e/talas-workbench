#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace talas::runtime
{
struct RealtimeRenderSessionTestAccess
{
    static bool hasOfflineTransaction(RealtimeRenderSession& session)
    {
        return bool(session.beginOfflineTransaction());
    }
};
}

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(
    std::string_view actual,
    std::string_view expected,
    const std::string& message)
{
    require(
        actual.find(expected) != std::string_view::npos,
        message + ": expected '" + std::string(expected)
            + "' in '" + std::string(actual) + "'");
}

void requireNear(
    double actual,
    double expected,
    const std::string& message)
{
    require(
        std::abs(actual - expected) <= 2.0e-4,
        message + ": expected " + std::to_string(expected)
            + ", received " + std::to_string(actual));
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-sample-map-playback-tests-"
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

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {
        std::move(id),
        std::move(typeId),
        std::move(parameters),
    };
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {
        std::move(fromNode),
        std::move(fromPort),
        std::move(toNode),
        std::move(toPort),
    };
}

struct Fixture
{
    talas::project::ProjectDocument document;
    talas::runtime::RenderSettings settings;
};

std::vector<talas::project::ProjectParameterExpression>
playerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters{
        textParameter("sampleMapId", "map_main"),
        {"frequency", "261.6255653005986 hz"},
        {"gate", "1.0"},
        {"velocity", "1.0"},
        {"scale", "1.0"},
        {"attackTime", "0 s"},
        {"releaseTime", "0 s"},
    };
    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find(
            parameters,
            overrideParameter.parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }
    return parameters;
}

Fixture makeFixture(
    const std::filesystem::path& directory,
    std::vector<talas::project::ProjectSampleRegion> regions = {
        {
            "region_root",
            "asset-root",
            60,
            60,
            60,
            1,
            127,
        },
    },
    std::vector<double> samples = {0.25, 0.5, -0.25, -0.5},
    double sourceSampleRate = 48000.0,
    std::vector<talas::project::ProjectParameterExpression> parameters =
        playerParameters())
{
    const auto path = directory / "root.wav";
    const std::vector<std::vector<double>> channels{samples};
    std::string writeError;
    require(
        talas::audio::writePcm16Wav(
            path,
            sourceSampleRate,
            channels,
            writeError),
        "fixture WAV should be written: " + writeError);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "fixture WAV should hash");

    Fixture fixture;
    fixture.document.assets.push_back({
        "asset-root",
        talas::project::ProjectAssetKind::Audio,
        path.string(),
        talas::project::ProjectAssetDisposition::Referenced,
        hash.contentHash,
        1,
        sourceSampleRate,
        samples.size(),
        "pcm16-wav",
        1,
        "test-generated",
    });
    fixture.document.sampleMapLibrary.maps.push_back({
        "map_main",
        "Main Map",
        std::move(regions),
    });
    fixture.document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    fixture.document.graph.nodes.push_back(node(
        "player",
        "talas.sampleMapPlayer",
        std::move(parameters)));
    fixture.document.graph.connections.push_back(connection(
        "player",
        "audioOut",
        "out",
        "audioIn"));

    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "asset-root";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = 1;
    snapshot.sampleRate = sourceSampleRate;
    snapshot.frameCount = samples.size();
    snapshot.encoding = "pcm16-wav";
    fixture.settings.resourceSnapshots.push_back(
        std::make_shared<const talas::core::ResourceSnapshot>(
            std::move(snapshot)));
    fixture.settings.sampleRate = sourceSampleRate;
    fixture.settings.maxBlockFrames = 512;
    return fixture;
}

void replaceFixtureAudioGeneration(
    Fixture& fixture,
    const std::vector<double>& samples,
    std::uint64_t generation)
{
    require(
        !fixture.document.assets.empty()
            && !fixture.settings.resourceSnapshots.empty(),
        "replacement fixture should own one Audio manifest and snapshot");
    auto& asset = fixture.document.assets.front();
    const auto path = std::filesystem::path(asset.path);
    std::string writeError;
    const std::vector<std::vector<double>> channels{samples};
    require(
        talas::audio::writePcm16Wav(
            path,
            asset.sampleRate,
            channels,
            writeError),
        "replacement WAV should be written: " + writeError);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "replacement WAV should hash");
    asset.contentHash = hash.contentHash;
    asset.frameCount = samples.size();
    asset.generation = generation;

    auto snapshot = std::make_shared<talas::core::ResourceSnapshot>(
        *fixture.settings.resourceSnapshots.front());
    snapshot->generation = generation;
    snapshot->contentHash = hash.contentHash;
    snapshot->frameCount = samples.size();
    snapshot->resolvedPath = path;
    fixture.settings.resourceSnapshots = {std::move(snapshot)};
}

talas::runtime::OfflineRenderResult renderFixture(
    Fixture fixture,
    std::size_t frameCount)
{
    const auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        compiled.ok(),
        "sample-map playback fixture should compile"
            + (compiled.diagnostics.empty()
                   ? std::string{}
                   : ": " + compiled.diagnostics.front().message));
    return talas::runtime::renderOffline(*compiled.graph, frameCount);
}

void requireSamples(
    const talas::runtime::OfflineRenderResult& result,
    std::initializer_list<double> expected,
    const std::string& message)
{
    require(
        result.ok(),
        message + " should render without diagnostics"
            + (result.diagnostics.empty()
                   ? std::string{}
                   : ": " + result.diagnostics.front().message));
    require(
        result.mono.size() == expected.size(),
        message + " should return the requested frame count");
    std::size_t frame = 0;
    for (const auto value : expected)
    {
        requireNear(
            result.mono[frame],
            value,
            message + " frame " + std::to_string(frame));
        ++frame;
    }
}

void connectEventControl(
    Fixture& fixture,
    std::string portId,
    std::string eventName,
    double initialValue)
{
    const auto nodeId = "control_" + portId;
    fixture.document.graph.nodes.push_back(node(
        nodeId,
        "talas.constant",
        {{"value", "!" + eventName}}));
    fixture.document.graph.connections.push_back(connection(
        nodeId,
        "value",
        "player",
        portId));
    fixture.settings.eventValues.push_back({
        std::move(eventName),
        initialValue,
        talas::expr::ValueDomain::Scalar,
    });
}

const talas::runtime::CompileStep& playerStep(
    const talas::runtime::PreparedRuntimeGraph& graph)
{
    const auto found = std::ranges::find(
        graph.plan().steps,
        std::string_view{"talas.sampleMapPlayer"},
        &talas::runtime::CompileStep::typeId);
    require(
        found != graph.plan().steps.end(),
        "compiled plan should retain the sample-map player step");
    return *found;
}

void testDescriptorContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* descriptor = registry.findNodeType("talas.sampleMapPlayer");
    require(descriptor != nullptr, "sample-map player descriptor should exist");
    require(
        descriptor->displayName == "Sample Map Player"
            && descriptor->category == "Sampling",
        "sample-map player should be a named Sampling utility");
    require(
        descriptor->inputs.size() == 6
            && descriptor->inputs[0].id == "frequency"
            && descriptor->inputs[1].id == "gate"
            && descriptor->inputs[2].id == "velocity"
            && descriptor->inputs[3].id == "scale"
            && descriptor->inputs[4].id == "attackTime"
            && descriptor->inputs[5].id == "releaseTime",
        "sample-map player should expose exact dynamic controls");
    require(
        descriptor->parameters.size() == 7
            && descriptor->parameters[0].id == "sampleMapId"
            && descriptor->parameters[0].kind
                == talas::core::ParameterKind::Text
            && descriptor->parameters[1].id == "frequency"
            && descriptor->parameters[2].id == "gate"
            && descriptor->parameters[3].id == "velocity"
            && descriptor->parameters[4].id == "scale"
            && descriptor->parameters[5].id == "attackTime"
            && descriptor->parameters[6].id == "releaseTime",
        "sample-map player should expose exact portable parameters");
    require(
        descriptor->supportLevel == talas::core::NodeSupportLevel::Utility
            && descriptor->capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful,
        "Step 5 should publish honest offline and prepared-realtime utility capability");
}

void testCompilePreparesMapRegionsAndImmutableAudio()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(directory.path());
    const auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        compiled.ok(),
        "valid sample map and immutable Audio snapshot should compile");
    const auto& step = playerStep(*compiled.graph);
    require(
        step.sampleMapPlayback.has_value()
            && step.sampleMapPlayback->mapId == "map_main"
            && step.sampleMapPlayback->regions.size() == 1
            && step.sampleMapPlayback->regions[0].regionId
                == "region_root"
            && step.sampleMapPlayback->regions[0].resourceId
                == "asset-root",
        "compile step should own the ordered prepared map projection");
    require(
        compiled.graph->preparedAudioStreamCount() == 1,
        "map-only reachable Audio should prepare one immutable stream owner");
}

void testUnknownMapFailsWithNodeAndParameterDiagnostic()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(directory.path());
    fixture.document.graph.nodes[1].parameters[0].text = "missing-map";
    const auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        !compiled.ok() && compiled.diagnostics.size() == 1,
        "unknown sample-map ID should fail preparation exactly once");
    require(
        compiled.diagnostics[0].nodeId == "player"
            && compiled.diagnostics[0].parameterId == "sampleMapId",
        "unknown map diagnostic should identify node and parameter");
    requireContains(
        compiled.diagnostics[0].message,
        "missing-map",
        "unknown map diagnostic should identify the stable map ID");
}

void testMoreThanThirtyTwoSimultaneousLayersFailsClosed()
{
    TemporaryDirectory directory;
    std::vector<talas::project::ProjectSampleRegion> regions;
    for (std::size_t index = 0; index < 33; ++index)
    {
        regions.push_back({
            "layer_" + std::to_string(index),
            "asset-root",
            60,
            60,
            60,
            64,
            64,
        });
    }
    auto fixture = makeFixture(directory.path(), std::move(regions));
    const auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        !compiled.ok() && compiled.diagnostics.size() == 1,
        "more than 32 matches at one note/velocity should fail preparation");
    requireContains(
        compiled.diagnostics[0].message,
        "32",
        "capacity diagnostic should state the prepared layer cap");
}

void testMissingOrMismatchedImmutableAudioFailsAtRegionPath()
{
    TemporaryDirectory directory;
    auto missing = makeFixture(directory.path());
    missing.settings.resourceSnapshots.clear();
    auto compiled = talas::runtime::compileProject(
        missing.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(missing.settings));
    require(
        !compiled.ok() && compiled.diagnostics.size() == 1,
        "missing immutable region Audio should fail preparation");
    requireContains(
        compiled.diagnostics[0].message,
        "asset-root",
        "missing resource diagnostic should identify asset identity");
    requireContains(
        compiled.diagnostics[0].structuralSourcePath,
        "sampleMap[map_main]/region[region_root]",
        "missing resource diagnostic should retain exact map/region path");

    auto mismatched = makeFixture(directory.path());
    auto replacement = std::make_shared<talas::core::ResourceSnapshot>(
        *mismatched.settings.resourceSnapshots.front());
    replacement->generation = 2;
    mismatched.settings.resourceSnapshots = {replacement};
    compiled = talas::runtime::compileProject(
        mismatched.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(mismatched.settings));
    require(
        !compiled.ok() && compiled.diagnostics.size() == 1,
        "mismatched immutable generation should fail preparation");
    requireContains(
        compiled.diagnostics[0].message,
        "identity or media shape",
        "mismatched resource diagnostic should explain immutable mismatch");
}

void testOfflineUsesInclusiveNoteVelocityBoundariesAndEveryLayer()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(
        directory.path(),
        {
            {"low_layer", "asset-root", 60, 59, 60, 1, 64},
            {"high_layer", "asset-root", 60, 60, 61, 64, 127},
        },
        {0.25, 0.5, -0.25, -0.5},
        48000.0,
        playerParameters({
            {"velocity", "0.5039370078740157"},
        }));
    requireSamples(
        renderFixture(std::move(fixture), 1),
        {0.5},
        "inclusive overlapping layer playback");

    fixture = makeFixture(
        directory.path(),
        {
            {"low_layer", "asset-root", 60, 59, 60, 1, 64},
            {"high_layer", "asset-root", 60, 60, 61, 64, 127},
        },
        {0.25, 0.5, -0.25, -0.5},
        48000.0,
        playerParameters({
            {"velocity", "0.4960629921259843"},
        }));
    requireSamples(
        renderFixture(std::move(fixture), 1),
        {0.25},
        "adjacent velocity boundary playback");

    fixture = makeFixture(
        directory.path(),
        {{"note_off", "asset-root", 60, 60, 60, 1, 127}},
        {0.25, 0.5, -0.25, -0.5},
        48000.0,
        playerParameters({{"velocity", "0.0"}}));
    requireSamples(
        renderFixture(std::move(fixture), 3),
        {0.0, 0.0, 0.0},
        "velocity zero note-off");
}

void testOfflineAppliesRootTranspositionAndReverseTraversal()
{
    TemporaryDirectory directory;
    auto transposed = makeFixture(
        directory.path(),
        {{"wide", "asset-root", 60, 60, 72, 1, 127}},
        {0.25, 0.5, -0.25, -0.5},
        48000.0,
        playerParameters({{"frequency", "523.2511306011972 hz"}}));
    requireSamples(
        renderFixture(std::move(transposed), 4),
        {0.25, -0.25, 0.0, 0.0},
        "root-note octave transposition");

    auto reversed = makeFixture(
        directory.path(),
        {{"reverse", "asset-root", 60, 60, 60, 1, 127, true}});
    requireSamples(
        renderFixture(std::move(reversed), 4),
        {-0.5, -0.25, 0.5, 0.25},
        "reverse traversal");
}

void testOfflineSustainForwardAndAlternateLoopsUseExactEndpoints()
{
    TemporaryDirectory directory;
    auto forwardRegion = talas::project::ProjectSampleRegion{
        "forward", "asset-root", 60, 60, 60, 1, 127};
    forwardRegion.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        1,
        3,
    };
    requireSamples(
        renderFixture(
            makeFixture(directory.path(), {forwardRegion}),
            7),
        {0.25, 0.5, -0.25, 0.5, -0.25, 0.5, -0.25},
        "forward sustain loop");

    auto alternateRegion = forwardRegion;
    alternateRegion.regionId = "alternate";
    alternateRegion.sustainLoop->mode =
        talas::project::ProjectSampleLoopMode::Alternate;
    alternateRegion.sustainLoop->endFrameExclusive = 4;
    requireSamples(
        renderFixture(
            makeFixture(directory.path(), {alternateRegion}),
            8),
        {0.25, 0.5, -0.25, -0.5, -0.25, 0.5, -0.25, -0.5},
        "alternate sustain loop without repeated endpoints");
}

void testOfflineReleaseLoopAndEnvelopeFollowGateTransition()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "release", "asset-root", 60, 60, 60, 1, 127};
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        1,
        4,
    };
    region.releaseLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        3,
        6,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.1, 0.2, 0.3, 0.4, 0.5, 0.6},
        4.0,
        playerParameters({{"releaseTime", "1 s"}}));
    connectEventControl(fixture, "gate", "SampleGate", 1.0);
    fixture.settings.eventValueChanges.push_back({
        4,
        "SampleGate",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    requireSamples(
        renderFixture(std::move(fixture), 9),
        {0.1, 0.2, 0.3, 0.4, 0.4, 0.375, 0.3, 0.125, 0.0},
        "alternate release loop and bounded envelope");
}

void testOfflineForwardReleaseLoopAndAttackRetriggerAreDeterministic()
{
    TemporaryDirectory directory;
    auto releaseRegion = talas::project::ProjectSampleRegion{
        "forward_release", "asset-root", 60, 60, 60, 1, 127};
    releaseRegion.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        1,
        4,
    };
    releaseRegion.releaseLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        3,
        6,
    };
    auto releaseFixture = makeFixture(
        directory.path(),
        {releaseRegion},
        {0.1, 0.2, 0.3, 0.4, 0.5, 0.6},
        4.0,
        playerParameters({{"releaseTime", "1.25 s"}}));
    connectEventControl(
        releaseFixture,
        "gate",
        "ForwardReleaseGate",
        1.0);
    releaseFixture.settings.eventValueChanges.push_back({
        4,
        "ForwardReleaseGate",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });
    requireSamples(
        renderFixture(std::move(releaseFixture), 10),
        {0.1, 0.2, 0.3, 0.4, 0.4, 0.4, 0.36, 0.16, 0.1, 0.0},
        "forward release loop");

    auto retriggerRegion = talas::project::ProjectSampleRegion{
        "retrigger", "asset-root", 60, 60, 60, 1, 127};
    retriggerRegion.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        0,
        4,
    };
    auto retriggerFixture = makeFixture(
        directory.path(),
        {retriggerRegion},
        {0.8, 0.8, 0.8, 0.8},
        4.0,
        playerParameters({
            {"attackTime", "1 s"},
            {"releaseTime", "0.5 s"},
        }));
    connectEventControl(
        retriggerFixture,
        "gate",
        "RetriggerGate",
        1.0);
    retriggerFixture.settings.eventValueChanges = {
        {2, "RetriggerGate", 0.0, talas::expr::ValueDomain::Scalar},
        {4, "RetriggerGate", 1.0, talas::expr::ValueDomain::Scalar},
    };
    requireSamples(
        renderFixture(std::move(retriggerFixture), 7),
        {0.0, 0.2, 0.4, 0.2, 0.0, 0.2, 0.4},
        "gate release and attack retrigger");
}

void testOfflineOneFrameAndReverseAlternateLoopsStayBounded()
{
    TemporaryDirectory directory;
    auto oneFrame = talas::project::ProjectSampleRegion{
        "one_frame", "asset-root", 60, 60, 60, 1, 127};
    oneFrame.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        1,
        2,
    };
    requireSamples(
        renderFixture(
            makeFixture(
                directory.path(),
                {oneFrame},
                {0.1, 0.7, 0.3}),
            6),
        {0.1, 0.7, 0.7, 0.7, 0.7, 0.7},
        "one-frame alternate sustain loop");

    auto reverseAlternate = talas::project::ProjectSampleRegion{
        "reverse_alternate",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
        true,
    };
    reverseAlternate.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        0,
        4,
    };
    requireSamples(
        renderFixture(
            makeFixture(directory.path(), {reverseAlternate}),
            7),
        {-0.5, -0.25, 0.5, 0.25, 0.5, -0.25, -0.5},
        "reverse alternate sustain loop");
}

void testFractionalAlternateLoopTraversesPreludeBeforeReflecting()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "fractional_alternate",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        1,
        4,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.0, 0.2, 0.4, 0.6},
        4.0);
    fixture.settings.sampleRate = 8.0;
    requireSamples(
        renderFixture(std::move(fixture), 9),
        {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.5, 0.4},
        "fractional alternate loop prelude and reflection");
}

void testFractionalReverseForwardLoopKeepsPreludeInterpolationAdjacent()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "fractional_reverse_forward",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
        true,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        1,
        4,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.0, 0.2, 0.4, 0.6, 0.8, 1.0},
        4.0);
    fixture.settings.sampleRate = 8.0;
    fixture.settings.maxBlockFrames = 5;

    auto offlineFixture = fixture;
    auto offlineGraph = talas::runtime::compileProject(
        offlineFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(offlineFixture.settings));
    require(offlineGraph.ok(), "fractional reverse oracle should compile");
    const auto offline = talas::runtime::renderOffline(
        *offlineGraph.graph,
        8);
    requireSamples(
        offline,
        {1.0, 0.9, 0.8, 0.4, 0.6, 0.5, 0.4, 0.3},
        "fractional reverse forward-loop prelude");

    auto realtimeGraph = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(realtimeGraph.ok(), "fractional reverse realtime graph should compile");
    talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
    require(session.ready(), std::string(session.error()));
    require(talas::runtime::RealtimeRenderSessionTestAccess::hasOfflineTransaction(session),
        "streaming Sample Map hooks must participate while retaining their original context addresses");

    std::array<double, 3> first{};
    std::array<double, 5> second{};
    require(
        session.render(first) && session.render(second),
        "fractional reverse realtime partitions should render");
    for (std::size_t frame = 0; frame < first.size(); ++frame)
    {
        requireNear(
            first[frame],
            offline.mono[frame],
            "fractional reverse realtime parity frame "
                + std::to_string(frame));
    }
    for (std::size_t frame = 0; frame < second.size(); ++frame)
    {
        const auto absoluteFrame = first.size() + frame;
        requireNear(
            second[frame],
            offline.mono[absoluteFrame],
            "fractional reverse realtime parity frame "
                + std::to_string(absoluteFrame));
    }
    require(
        session.streamHealth().totals.missingFrames == 0,
        "fractional reverse forward loop should stay fully prepared");
}

void testOfflineWholeCallSampleMapRollbackPreservesLoopAndRetriggerContexts()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    for (const bool reverse : {false, true})
    for (const std::size_t prefix : {0U, 7U})
    {
        auto region = talas::project::ProjectSampleRegion{"loop", "asset-root", 60, 60, 60, 1, 127, reverse};
        region.sustainLoop = talas::project::ProjectSampleLoop{talas::project::ProjectSampleLoopMode::Forward, 0, 4};
        auto fixture = makeFixture(directory.path(), {region}, {0.25, 0.5, -0.25, -0.5}, 64.0,
            playerParameters({{"gate", "!Gate"}}));
        fixture.settings.maxBlockFrames = 5;
        fixture.settings.eventValues = {{"Gate", 1.0}, {"Divisor", 1.0}};
        fixture.document.graph.nodes.push_back(node("gain", "talas.vca"));
        fixture.document.graph.nodes.push_back(node("control", "talas.constant", {{"value", "1 / !Divisor"}}));
        fixture.document.graph.connections = {connection("player", "audioOut", "gain", "audioIn"),
            connection("control", "value", "gain", "gain"), connection("gain", "audioOut", "out", "audioIn")};
        auto compiled = compileProject(fixture.document, talas::core::makeBuiltinNodeRegistry(), fixture.settings);
        auto referenceGraph = compileProject(fixture.document, talas::core::makeBuiltinNodeRegistry(), fixture.settings);
        require(compiled.ok() && referenceGraph.ok(), "whole-call Sample Map graphs must prepare");
        OfflineRenderSession candidate(*compiled.graph), reference(*referenceGraph.graph);
        std::array<OfflineMidiVoiceEvent, 3> events{};
        for (std::size_t index = 0; index != events.size(); ++index)
        {
            events[index].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
            events[index].sequence = events[index].batchId = index + 1;
        }
        events[0].frame = 13; events[0].eventValueName = "Gate"; events[0].value = 0.0;
        events[1].frame = 17; events[1].eventValueName = "Gate"; events[1].value = 1.0;
        events[2].frame = 29; events[2].eventValueName = "Divisor"; events[2].value = 0.0;
        require(candidate.scheduleMidiVoiceEvents(events) && reference.scheduleMidiVoiceEvents(events),
            "whole-call Sample Map inputs must be admitted");
        require(candidate.render(prefix).ok() && reference.render(prefix).ok(), "Sample Map entry boundary must commit");
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            const auto failed = candidate.render(31 - prefix);
            require(!failed.ok() && failed.mono.empty() && failed.channels.empty(), "late Sample Map failure must return no partial audio");
        }
        const auto retry = candidate.render(29 - prefix);
        const auto expected = reference.render(29 - prefix);
        require(retry.ok() && expected.ok() && retry.mono == expected.mono,
            "Sample Map retry must preserve stream context identity, loop relocation and gate retrigger");
        constexpr std::array<double, 4> source{0.25, 0.5, -0.25, -0.5};
        for (std::size_t index = 0; index != retry.mono.size(); ++index)
        {
            const auto frame = prefix + index;
            const auto phase = (frame < 17 ? frame : frame - 17) % 4;
            const auto sample = frame >= 13 && frame < 17 ? 0.0 : source[reverse ? 3 - phase : phase];
            requireNear(retry.mono[index], sample, "Sample Map retry must match independent exact loop endpoints and gate timing");
        }
        require(candidate.render(0).ok() && !candidate.render(1).ok(), "Sample Map invalid event must remain at frame 29");
    }
}

void testOfflinePlaybackIsBlockAndSampleRateInvariant()
{
    TemporaryDirectory directory;
    auto singleFrameBlocks = makeFixture(
        directory.path(),
        {{"wide", "asset-root", 60, 60, 60, 1, 127}},
        {0.0, 0.5, 1.0, 0.5},
        4.0);
    singleFrameBlocks.settings.sampleRate = 8.0;
    singleFrameBlocks.settings.maxBlockFrames = 1;
    const auto partitioned = renderFixture(singleFrameBlocks, 8);

    auto wholeBlock = singleFrameBlocks;
    wholeBlock.settings.maxBlockFrames = 8;
    const auto contiguous = renderFixture(std::move(wholeBlock), 8);
    require(
        partitioned.ok() && contiguous.ok()
            && partitioned.mono.size() == contiguous.mono.size(),
        "block-invariance fixtures should render equally sized output");
    for (std::size_t frame = 0; frame < partitioned.mono.size(); ++frame)
    {
        requireNear(
            partitioned.mono[frame],
            contiguous.mono[frame],
            "callback partition invariance frame "
                + std::to_string(frame));
    }
    requireSamples(
        contiguous,
        {0.0, 0.25, 0.5, 0.75, 1.0, 0.75, 0.5, 0.0},
        "source-to-output sample-rate conversion");
}

void testPreparedSummaryPublishesEveryMapRegionStructuralPath()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(
        directory.path(),
        {
            {"first_layer", "asset-root", 60, 60, 60, 1, 127},
            {"second_layer", "asset-root", 60, 60, 60, 1, 127},
        });
    const auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(compiled.ok(), "summary fixture should compile");
    const auto summarized = talas::runtime::makePreparedGraphSummary(
        *compiled.graph);
    require(
        summarized.ok(),
        "prepared summary should accept map-only streams: "
            + summarized.error);
    require(
        summarized.summary->streams.size() == 2
            && summarized.summary->streams[0].resourceId
                == "asset-root"
            && summarized.summary->streams[1].resourceId
                == "asset-root",
        "prepared summary should retain one structural stream reference per region");
    requireContains(
        summarized.summary->streams[0].structuralSourcePath,
        "sampleMap[map_main]/region[first_layer]",
        "first prepared stream path should retain map and region identity");
    requireContains(
        summarized.summary->streams[1].structuralSourcePath,
        "sampleMap[map_main]/region[second_layer]",
        "second prepared stream path should retain ordered region identity");
}

void testPreparedRealtimeMatchesOfflineAcrossPartitionsAndLayersWithoutAllocation()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(
        directory.path(),
        {
            {"first_layer", "asset-root", 60, 60, 60, 1, 127},
            {"second_layer", "asset-root", 60, 60, 60, 1, 127},
        },
        {0.0, 0.25, 0.5, 0.75},
        4.0);
    fixture.settings.maxBlockFrames = 3;

    auto offlineFixture = fixture;
    auto offlineGraph = talas::runtime::compileProject(
        offlineFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(offlineFixture.settings));
    require(
        offlineGraph.ok(),
        offlineGraph.diagnostics.empty()
            ? "realtime parity fixture should compile offline"
            : offlineGraph.diagnostics.front().message);
    const auto offline = talas::runtime::renderOffline(
        *offlineGraph.graph,
        6);
    require(offline.ok(), "realtime parity oracle should render offline");

    auto realtimeGraph = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        realtimeGraph.ok(),
        realtimeGraph.diagnostics.empty()
            ? "realtime parity fixture should compile"
            : realtimeGraph.diagnostics.front().message);
    require(
        realtimeGraph.graph->preparedAudioStreamCount() == 1,
        "two regions on one asset generation should share one graph-scoped cache owner");
    talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
    require(session.ready(), std::string(session.error()));
    realtimeGraph.graph.reset();

    std::array<double, 1> first{};
    std::array<double, 3> middle{};
    std::array<double, 2> last{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto renderedFirst = session.render(first);
    const auto renderedMiddle = session.render(middle);
    const auto renderedLast = session.render(last);
    countAllocations.store(false, std::memory_order_release);
    require(
        renderedFirst && renderedMiddle && renderedLast,
        "prepared sample-map player should render every callback partition");
    require(
        allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared sample-map callback path must not allocate");

    std::array<double, 6> realtime{};
    std::ranges::copy(first, realtime.begin());
    std::ranges::copy(middle, realtime.begin() + 1);
    std::ranges::copy(last, realtime.begin() + 4);
    require(realtime.size() == offline.mono.size(),
            "offline and realtime parity traces should have equal length");
    for (std::size_t frame = 0; frame < realtime.size(); ++frame)
    {
        requireNear(
            realtime[frame],
            offline.mono[frame],
            "prepared realtime parity frame " + std::to_string(frame));
    }

    const auto health = session.streamHealth();
    require(
        health.consumerCount == 2
            && health.consumers.size() == 2
            && health.consumers[0].resourceId == "asset-root"
            && health.consumers[1].resourceId == "asset-root"
            && health.consumers[0].structuralSourcePath.find(
                   "region[first_layer]") != std::string::npos
            && health.consumers[1].structuralSourcePath.find(
                   "region[second_layer]") != std::string::npos
            && health.totals.underrunEvents == 0
            && health.totals.missingFrames == 0,
        "each prepared region should retain an independent prewarmed consumer and structural telemetry");
}

void testPreparedRealtimeEmptyMapIsPreparedSilence()
{
    TemporaryDirectory directory;
    auto fixture = makeFixture(
        directory.path(),
        {},
        {0.25, 0.5, -0.25, -0.5},
        4.0);
    fixture.settings.maxBlockFrames = 4;
    auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(compiled.ok(), "empty sample map should compile");
    require(
        compiled.graph->preparedAudioStreamCount() == 0,
        "empty sample map should not prepare an unreachable stream owner");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 4> output{1.0, 1.0, 1.0, 1.0};
    require(session.render(output), "empty sample map should render prepared silence");
    require(
        std::ranges::all_of(output, [](double sample) {
            return sample == 0.0;
        })
            && session.streamHealth().consumerCount == 0,
        "empty sample map should stay silent without phantom stream consumers");
}

void testPreparedRealtimeCrossBlockLoopWrapsRetainPreparedStartBlocks()
{
    const auto run = [](talas::project::ProjectSampleLoopMode mode,
                        bool reverse,
                        std::string_view label) {
        TemporaryDirectory directory;
        auto region = talas::project::ProjectSampleRegion{
            "cross_block_loop",
            "asset-root",
            60,
            60,
            60,
            1,
            127,
            reverse,
        };
        region.sustainLoop = talas::project::ProjectSampleLoop{
            mode,
            2048,
            2816,
        };
        std::vector<double> samples(4096, 0.0);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
        {
            samples[frame] = 0.125
                + static_cast<double>(frame % 113U) / 256.0;
        }
        auto fixture = makeFixture(
            directory.path(),
            {region},
            std::move(samples),
            48000.0);
        fixture.settings.maxBlockFrames = 512;

        auto offlineFixture = fixture;
        auto offlineGraph = talas::runtime::compileProject(
            offlineFixture.document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(offlineFixture.settings));
        require(
            offlineGraph.ok(),
            std::string(label) + " cross-block oracle should compile");
        const auto offline = talas::runtime::renderOffline(
            *offlineGraph.graph,
            3585);
        require(
            offline.ok(),
            std::string(label) + " cross-block oracle should render");

        auto realtimeGraph = talas::runtime::compileProject(
            fixture.document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(fixture.settings));
        require(
            realtimeGraph.ok(),
            std::string(label) + " cross-block realtime graph should compile");
        talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
        require(session.ready(), std::string(session.error()));

        std::array<double, 511> fullPartition{};
        std::array<double, 8> tailPartition{};
        std::size_t absoluteFrame = 0;
        allocationCount.store(0, std::memory_order_relaxed);
        for (std::size_t partition = 0; partition < 7; ++partition)
        {
            countAllocations.store(true, std::memory_order_release);
            const auto rendered = session.render(fullPartition);
            countAllocations.store(false, std::memory_order_release);
            require(
                rendered,
                std::string(label)
                    + " cross-block loop should render full partition "
                    + std::to_string(partition));
            for (const auto sample : fullPartition)
            {
                requireNear(
                    sample,
                    offline.mono[absoluteFrame],
                    std::string(label) + " cross-block parity frame "
                        + std::to_string(absoluteFrame));
                ++absoluteFrame;
            }
        }
        countAllocations.store(true, std::memory_order_release);
        const auto renderedTail = session.render(tailPartition);
        countAllocations.store(false, std::memory_order_release);
        require(
            renderedTail,
            std::string(label)
                + " cross-block loop should render its irregular tail");
        for (const auto sample : tailPartition)
        {
            requireNear(
                sample,
                offline.mono[absoluteFrame],
                std::string(label) + " cross-block parity frame "
                    + std::to_string(absoluteFrame));
            ++absoluteFrame;
        }
        require(
            allocationCount.load(std::memory_order_relaxed) == 0,
            std::string(label)
                + " cross-block loop callback must not allocate");
        require(
            absoluteFrame == offline.mono.size(),
            std::string(label)
                + " cross-block partitions should cover the oracle");
        const auto health = session.streamHealth();
        require(
            health.totals.underrunEvents == 0
                && health.totals.missingFrames == 0,
            std::string(label)
                + " prewarmed cross-block loop should not underrun");
    };

    run(
        talas::project::ProjectSampleLoopMode::Forward,
        false,
        "forward");
    run(
        talas::project::ProjectSampleLoopMode::Alternate,
        false,
        "alternate");
    run(
        talas::project::ProjectSampleLoopMode::Forward,
        true,
        "reverse forward");
    run(
        talas::project::ProjectSampleLoopMode::Alternate,
        true,
        "reverse alternate");
}

void testPreparedRealtimeRetainsReverseAlternateLoopSemantics()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "reverse_alternate",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
        true,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        0,
        4,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.25, 0.5, -0.25, -0.5},
        4.0);
    fixture.settings.maxBlockFrames = 4;

    auto offlineFixture = fixture;
    auto offlineGraph = talas::runtime::compileProject(
        offlineFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(offlineFixture.settings));
    require(offlineGraph.ok(), "reverse loop oracle should compile");
    const auto offline = talas::runtime::renderOffline(
        *offlineGraph.graph,
        8);
    require(offline.ok(), "reverse loop oracle should render");

    auto realtimeGraph = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(realtimeGraph.ok(), "reverse loop realtime graph should compile");
    talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 3> first{};
    std::array<double, 1> second{};
    std::array<double, 4> third{};
    require(
        session.render(first)
            && session.render(second)
            && session.render(third),
        "reverse alternate loop should render across irregular partitions");
    std::array<double, 8> realtime{};
    std::ranges::copy(first, realtime.begin());
    std::ranges::copy(second, realtime.begin() + 3);
    std::ranges::copy(third, realtime.begin() + 4);
    for (std::size_t frame = 0; frame < realtime.size(); ++frame)
    {
        requireNear(
            realtime[frame],
            offline.mono[frame],
            "reverse alternate realtime parity frame "
                + std::to_string(frame));
    }
}

void testPreparedRealtimeMatchesFractionalForwardAndReleaseLoops()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "fractional_release",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        1,
        4,
    };
    region.releaseLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        3,
        6,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.0, 0.2, 0.4, 0.6, 0.8, 1.0},
        4.0,
        playerParameters({{"releaseTime", "1 s"}}));
    fixture.settings.sampleRate = 8.0;
    fixture.settings.maxBlockFrames = 7;
    connectEventControl(fixture, "gate", "LoopGate", 1.0);
    fixture.settings.eventValueChanges.push_back({
        8,
        "LoopGate",
        0.0,
        talas::expr::ValueDomain::Scalar,
    });

    auto offlineFixture = fixture;
    auto offlineGraph = talas::runtime::compileProject(
        offlineFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(offlineFixture.settings));
    require(offlineGraph.ok(), "fractional loop oracle should compile");
    const auto offline = talas::runtime::renderOffline(
        *offlineGraph.graph,
        16);
    require(offline.ok(), "fractional loop oracle should render");

    auto realtimeGraph = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(realtimeGraph.ok(), "fractional loop realtime graph should compile");
    talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 3> first{};
    std::array<double, 1> second{};
    std::array<double, 5> third{};
    std::array<double, 7> fourth{};
    require(
        session.render(first)
            && session.render(second)
            && session.render(third)
            && session.render(fourth),
        "fractional sustain/release loop should render every partition");
    std::array<double, 16> realtime{};
    std::ranges::copy(first, realtime.begin());
    std::ranges::copy(second, realtime.begin() + 3);
    std::ranges::copy(third, realtime.begin() + 4);
    std::ranges::copy(fourth, realtime.begin() + 9);
    for (std::size_t frame = 0; frame < realtime.size(); ++frame)
    {
        requireNear(
            realtime[frame],
            offline.mono[frame],
            "fractional sustain/release realtime parity frame "
                + std::to_string(frame));
    }
    require(
        session.streamHealth().totals.missingFrames == 0,
        "fully prewarmed fractional loops should not report an underrun");
}

void testPreparedRealtimeRetriggerUsesPreparedWindowWithoutUnderrun()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "retrigger",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        0,
        4,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.25, 0.5, -0.25, -0.5},
        4.0);
    fixture.settings.maxBlockFrames = 2;
    connectEventControl(fixture, "gate", "SampleGate", 1.0);
    auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(compiled.ok(), "retrigger realtime graph should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));

    std::array<double, 2> initial{};
    require(session.render(initial), "initial prepared trigger should render");
    const auto before = session.streamHealth();
    require(
        before.consumers.size() == 1
            && before.consumers[0].seekGeneration > 0,
        "prepared region should expose its initial consumer generation");
    require(
        session.updateEventValue(
            "SampleGate",
            0.0,
            talas::expr::ValueDomain::Scalar),
        "gate close should publish on the control thread");
    std::array<double, 1> release{};
    require(session.render(release), "zero-time release should render");
    require(
        session.streamHealth().consumers[0].seekGeneration
            == before.consumers[0].seekGeneration,
        "note-off should not replace or seek the immutable stream generation");

    require(
        session.updateEventValue(
            "SampleGate",
            1.0,
            talas::expr::ValueDomain::Scalar),
        "gate retrigger should publish on the control thread");
    std::array<double, 2> retrigger{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto rendered = session.render(retrigger);
    countAllocations.store(false, std::memory_order_release);
    const auto after = session.streamHealth();
    require(rendered, "prepared retrigger callback should render");
    require(
        allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared retrigger and bounded refill publication must not allocate");
    require(
        after.consumers.size() == 1
            && after.consumers[0].seekGeneration
                == before.consumers[0].seekGeneration,
        "prepared retrigger should not publish an ordinary asynchronous seek generation");
    requireNear(
        retrigger[0],
        0.25,
        "prepared retrigger should emit the exact start sample immediately");
    requireNear(
        retrigger[1],
        0.5,
        "prepared retrigger should continue through its retained start window");
    require(
        after.totals.missingFrames == 0
            && after.totals.underrunEvents == 0,
        "prepared retrigger should not race an asynchronous refill");
}

void testPreparedRealtimeReleaseLoopRepositionsItsRefillEpoch()
{
    const auto run = [](bool reverse, std::string_view label) {
        TemporaryDirectory directory;
        auto region = talas::project::ProjectSampleRegion{
            "release_relocation",
            "asset-root",
            60,
            60,
            60,
            1,
            127,
            reverse,
        };
        region.releaseLoop = talas::project::ProjectSampleLoop{
            talas::project::ProjectSampleLoopMode::Forward,
            256,
            4096,
        };
        std::vector<double> samples(8192, 0.0);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
        {
            samples[frame] = 0.125
                + static_cast<double>(frame % 113U) / 256.0;
        }
        auto fixture = makeFixture(
            directory.path(),
            {region},
            std::move(samples),
            48000.0,
            playerParameters({{"releaseTime", "1 s"}}));
        fixture.settings.maxBlockFrames = 512;
        connectEventControl(fixture, "gate", "ReleaseGate", 1.0);

        auto offlineFixture = fixture;
        offlineFixture.settings.eventValueChanges.push_back({
            5120,
            "ReleaseGate",
            0.0,
            talas::expr::ValueDomain::Scalar,
        });
        auto offlineGraph = talas::runtime::compileProject(
            offlineFixture.document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(offlineFixture.settings));
        require(
            offlineGraph.ok(),
            std::string(label) + " release relocation oracle should compile");
        const auto offline = talas::runtime::renderOffline(
            *offlineGraph.graph,
            7168);
        require(
            offline.ok(),
            std::string(label) + " release relocation oracle should render");

        auto realtimeGraph = talas::runtime::compileProject(
            fixture.document,
            talas::core::makeBuiltinNodeRegistry(),
            std::move(fixture.settings));
        require(
            realtimeGraph.ok(),
            std::string(label) + " release relocation graph should compile");
        talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
        require(session.ready(), std::string(session.error()));
        std::array<double, 512> partition{};
        for (std::size_t callback = 0; callback < 10; ++callback)
        {
            require(
                session.render(partition),
                std::string(label) + " later sustain block should render");
        }
        const auto beforeRelease = session.streamHealth();
        require(
            session.updateEventValue(
                "ReleaseGate",
                0.0,
                talas::expr::ValueDomain::Scalar),
            std::string(label) + " release gate should publish");

        allocationCount.store(0, std::memory_order_relaxed);
        for (std::size_t callback = 0; callback < 4; ++callback)
        {
            countAllocations.store(true, std::memory_order_release);
            const auto rendered = session.render(partition);
            countAllocations.store(false, std::memory_order_release);
            require(
                rendered,
                std::string(label) + " release callback should render");
            for (std::size_t frame = 0; frame < partition.size(); ++frame)
            {
                const auto absoluteFrame = 5120
                    + callback * partition.size() + frame;
                requireNear(
                    partition[frame],
                    offline.mono[absoluteFrame],
                    std::string(label) + " release relocation parity frame "
                        + std::to_string(absoluteFrame));
            }
        }
        require(
            allocationCount.load(std::memory_order_relaxed) == 0,
            std::string(label) + " release relocation must not allocate");
        const auto health = session.streamHealth();
        require(
            health.totals.missingFrames
                    == beforeRelease.totals.missingFrames
                && health.totals.underrunEvents
                    == beforeRelease.totals.underrunEvents,
            std::string(label)
                + " release relocation should replace the stale sustain queue");
    };

    run(false, "forward");
    run(true, "reverse");
}

void testPreparedRealtimeHighPitchInitialAndRetriggerCoverOneCallback()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "high_pitch",
        "asset-root",
        60,
        60,
        96,
        1,
        127,
    };
    std::vector<double> samples(8192, 0.0);
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
    {
        samples[frame] = 0.125
            + static_cast<double>(frame % 97U) / 256.0;
    }
    auto fixture = makeFixture(
        directory.path(),
        {region},
        std::move(samples),
        48000.0,
        playerParameters({
            {"frequency", "2093.004522404789 hz"},
        }));
    fixture.settings.maxBlockFrames = 512;
    connectEventControl(fixture, "gate", "HighPitchGate", 1.0);

    auto offlineFixture = fixture;
    auto offlineGraph = talas::runtime::compileProject(
        offlineFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(offlineFixture.settings));
    require(
        offlineGraph.ok(),
        "high-pitch oracle should compile"
            + (offlineGraph.diagnostics.empty()
                   ? std::string{}
                   : ": " + offlineGraph.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(
        *offlineGraph.graph,
        512);
    require(offline.ok(), "high-pitch oracle should render");

    auto realtimeGraph = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        realtimeGraph.ok(),
        "high-pitch realtime graph should compile"
            + (realtimeGraph.diagnostics.empty()
                   ? std::string{}
                   : ": " + realtimeGraph.diagnostics.front().message));
    talas::runtime::RealtimeRenderSession session(*realtimeGraph.graph);
    require(session.ready(), std::string(session.error()));
    std::array<double, 512> initial{};
    require(session.render(initial), "high-pitch initial callback should render");
    for (std::size_t frame = 0; frame < initial.size(); ++frame)
    {
        requireNear(
            initial[frame],
            offline.mono[frame],
            "high-pitch initial parity frame " + std::to_string(frame));
    }
    const auto generation =
        session.streamHealth().consumers.front().seekGeneration;

    require(
        session.updateEventValue(
            "HighPitchGate",
            0.0,
            talas::expr::ValueDomain::Scalar),
        "high-pitch note-off should publish");
    std::array<double, 1> released{};
    require(session.render(released), "high-pitch release should render");
    require(
        session.updateEventValue(
            "HighPitchGate",
            1.0,
            talas::expr::ValueDomain::Scalar),
        "high-pitch retrigger should publish");
    std::array<double, 512> retrigger{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto rendered = session.render(retrigger);
    countAllocations.store(false, std::memory_order_release);
    require(rendered, "high-pitch retrigger callback should render");
    for (std::size_t frame = 0; frame < retrigger.size(); ++frame)
    {
        requireNear(
            retrigger[frame],
            offline.mono[frame],
            "high-pitch retrigger parity frame " + std::to_string(frame));
    }
    const auto health = session.streamHealth();
    require(
        allocationCount.load(std::memory_order_relaxed) == 0
            && health.consumers.front().seekGeneration == generation
            && health.totals.missingFrames == 0
            && health.totals.underrunEvents == 0,
        "high-pitch initial/retrigger callbacks should stay prepared, allocation-free, and worker-independent");
}

void testPreparedRealtimeAggregateWindowBudgetFailsClosed()
{
    TemporaryDirectory directory;
    std::vector<talas::project::ProjectSampleRegion> regions;
    regions.reserve(talas::project::maximumPreparedSampleMapLayers);
    for (std::size_t index = 0;
         index < talas::project::maximumPreparedSampleMapLayers;
         ++index)
    {
        talas::project::ProjectSampleRegion region{
            "budget_" + std::to_string(index),
            "asset-root",
            60,
            60,
            127,
            1,
            127,
        };
        region.sustainLoop = talas::project::ProjectSampleLoop{
            talas::project::ProjectSampleLoopMode::Forward,
            100000,
            300000,
        };
        regions.push_back(std::move(region));
    }
    std::vector<double> samples(400000, 0.25);
    auto fixture = makeFixture(
        directory.path(),
        std::move(regions),
        std::move(samples),
        48000.0);
    fixture.settings.maxBlockFrames = 4096;
    auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(
        compiled.ok(),
        "aggregate prepared-window budget fixture should remain portable and compilable");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(
        !session.ready()
            && std::string(session.error()).find(
                   "aggregate block/byte budget")
                != std::string::npos,
        "excessive aggregate prepared windows should fail session preparation before publication");
}

void testPreparedRealtimeGraphSwapPinsActiveGenerationAndRejectsChangedCandidate()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "generation",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        0,
        4,
    };
    auto firstFixture = makeFixture(
        directory.path(),
        {region},
        {0.25, 0.25, 0.25, 0.25},
        4.0);
    firstFixture.settings.maxBlockFrames = 1;
    auto firstCompiled = talas::runtime::compileProject(
        firstFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        firstFixture.settings);
    require(firstCompiled.ok(), "generation-one sample map should compile");
    auto firstEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        *firstCompiled.graph);
    require(firstEngine->ready(), std::string(firstEngine->error()));
    firstEngine->setStageId(1);

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    require(
        host.tryStage(std::move(firstEngine))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "generation-one engine should stage");
    std::array<float, 1> output{};
    std::array<float*, 1> channels{output.data()};
    std::size_t allocations = 0;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto firstRendered = host.render(
        channels.data(),
        channels.size(),
        output.size());
    countAllocations.store(false, std::memory_order_release);
    allocations = allocationCount.load(std::memory_order_relaxed);
    require(firstRendered && allocations == 0,
            "generation-one activation should render without allocation");
    requireNear(output[0], 0.25, "generation-one active output");
    const auto firstStage = host.activeStageId();

    auto replacementFixture = firstFixture;
    replaceFixtureAudioGeneration(
        replacementFixture,
        {-0.5, -0.5, -0.5, -0.5},
        2);
    auto staleCandidate = talas::runtime::compileProject(
        firstFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        firstFixture.settings);
    require(
        !staleCandidate.ok() && !staleCandidate.graph.has_value(),
        "changed bytes with generation-one identity must fail before graph publication");
    output[0] = 0.0F;
    require(
        host.render(channels.data(), channels.size(), output.size()),
        "failed replacement must leave generation one active");
    requireNear(
        output[0],
        0.25,
        "failed replacement should preserve generation-one private bytes");
    require(
        host.activeStageId() == firstStage,
        "failed replacement must not alter the callback-visible stage");

    auto secondCompiled = talas::runtime::compileProject(
        replacementFixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        replacementFixture.settings);
    require(secondCompiled.ok(), "generation-two sample map should compile");
    auto secondEngine = std::make_unique<talas::runtime::RealtimeAudioEngine>(
        *secondCompiled.graph);
    require(secondEngine->ready(), std::string(secondEngine->error()));
    secondEngine->setStageId(2);
    require(
        host.tryStage(std::move(secondEngine))
            == talas::runtime::RealtimeGraphStageResult::Accepted,
        "fully prepared generation two should stage");
    require(
        host.activeStageId() == firstStage,
        "staged generation two must not publish before the callback boundary");
    output[0] = 0.0F;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto replacementRendered = host.render(
        channels.data(),
        channels.size(),
        output.size());
    countAllocations.store(false, std::memory_order_release);
    require(
        replacementRendered
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "generation-two boundary swap should render without allocation");
    requireNear(output[0], -0.5, "generation-two active output");
    require(
        host.activeStageId() != firstStage,
        "callback boundary should publish generation two as a new stage");

    auto retired = host.collectRetired();
    require(retired != nullptr, "generation one should be collectible after swap");
    retired->setPlaying(true);
    output[0] = 0.0F;
    require(
        retired->render(channels.data(), channels.size(), output.size()),
        "retired generation one should remain independently renderable");
    requireNear(
        output[0],
        0.25,
        "retired engine should retain generation-one immutable bytes");

    host.setPlaying(false);
    host.clearAfterAudioStopped();
}

void testPreparedRealtimeHealthIsRaceFreeDuringCallbackStress()
{
    TemporaryDirectory directory;
    auto region = talas::project::ProjectSampleRegion{
        "health",
        "asset-root",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        0,
        4,
    };
    auto fixture = makeFixture(
        directory.path(),
        {region},
        {0.25, 0.5, -0.25, -0.5},
        4.0);
    fixture.settings.maxBlockFrames = 1;
    auto compiled = talas::runtime::compileProject(
        fixture.document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(fixture.settings));
    require(compiled.ok(), "stream-health stress fixture should compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));

    std::atomic<bool> start = false;
    std::atomic<bool> finished = false;
    std::atomic<bool> renderSucceeded = true;
    std::thread callbackThread([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::array<double, 1> output{};
        for (std::size_t callback = 0; callback < 10000; ++callback)
        {
            if (!session.render(output))
            {
                renderSucceeded.store(false, std::memory_order_release);
                break;
            }
        }
        finished.store(true, std::memory_order_release);
    });
    std::size_t snapshots = 0;
    start.store(true, std::memory_order_release);
    while (!finished.load(std::memory_order_acquire))
    {
        const auto health = session.streamHealth();
        require(
            health.consumers.size() == 1
                && health.consumers[0].seekGeneration > 0,
            "concurrent control-thread health snapshot should remain coherent");
        ++snapshots;
        std::this_thread::yield();
    }
    callbackThread.join();
    require(
        renderSucceeded.load(std::memory_order_acquire)
            && snapshots > 0,
        "callback/health stress should complete with overlapping observations");
}
}

int main()
{
    testDescriptorContract();
    testCompilePreparesMapRegionsAndImmutableAudio();
    testUnknownMapFailsWithNodeAndParameterDiagnostic();
    testMoreThanThirtyTwoSimultaneousLayersFailsClosed();
    testMissingOrMismatchedImmutableAudioFailsAtRegionPath();
    testOfflineUsesInclusiveNoteVelocityBoundariesAndEveryLayer();
    testOfflineAppliesRootTranspositionAndReverseTraversal();
    testOfflineSustainForwardAndAlternateLoopsUseExactEndpoints();
    testOfflineReleaseLoopAndEnvelopeFollowGateTransition();
    testOfflineForwardReleaseLoopAndAttackRetriggerAreDeterministic();
    testOfflineOneFrameAndReverseAlternateLoopsStayBounded();
    testFractionalAlternateLoopTraversesPreludeBeforeReflecting();
    testFractionalReverseForwardLoopKeepsPreludeInterpolationAdjacent();
    testOfflineWholeCallSampleMapRollbackPreservesLoopAndRetriggerContexts();
    testOfflinePlaybackIsBlockAndSampleRateInvariant();
    testPreparedSummaryPublishesEveryMapRegionStructuralPath();
    testPreparedRealtimeMatchesOfflineAcrossPartitionsAndLayersWithoutAllocation();
    testPreparedRealtimeEmptyMapIsPreparedSilence();
    testPreparedRealtimeCrossBlockLoopWrapsRetainPreparedStartBlocks();
    testPreparedRealtimeRetainsReverseAlternateLoopSemantics();
    testPreparedRealtimeMatchesFractionalForwardAndReleaseLoops();
    testPreparedRealtimeRetriggerUsesPreparedWindowWithoutUnderrun();
    testPreparedRealtimeReleaseLoopRepositionsItsRefillEpoch();
    testPreparedRealtimeHighPitchInitialAndRetriggerCoverOneCallback();
    testPreparedRealtimeAggregateWindowBudgetFailsClosed();
    testPreparedRealtimeGraphSwapPinsActiveGenerationAndRejectsChangedCandidate();
    testPreparedRealtimeHealthIsRaceFreeDuringCallbackStress();
    return 0;
}
