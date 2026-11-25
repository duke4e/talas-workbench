#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class AssetDirectory
{
public:
    AssetDirectory()
        : path(std::filesystem::temp_directory_path()
            / ("talas-formant-assets-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }
    ~AssetDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

talas::project::ProjectParameterExpression boolean(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression text(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectDocument assetDocument(bool oscillator)
{
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = oscillator ? "formantImpulse" : "impulseResponse";
    resource.resource = "Tables/FormantAsset";
    talas::project::ProjectDocument document;
    document.assets = {
        {"Tables/FormantAsset", talas::project::ProjectAssetKind::Table, "impulse.wav"}};
    document.graph.nodes = {
        {"spectrum", "talas.syntheticSpectrumFromArray", {
            {"nbrPartials", "1"}, {"envelope", "1"},
            boolean("logScale", false), boolean("sendBandwidths", true),
            text("amplitudes", "1"), text("frequencies", "0"), text("bandwidths", "0")}},
        {"formant", oscillator ? "talas.formantBankOscillator" : "talas.formantBank", {
            {"frequency", "2000 hz"}, {"nbrFormants", "1"},
            {oscillator ? "firstFormant" : "bankSize", "1"},
            std::move(resource), {"nbrImpulses", "2"},
            boolean(oscillator ? "allowFormantAtDC" : "allowDC", true)}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"spectrum", "spectrumOut", "formant", "spectrum"},
        {"formant", "audioOut", "out", "audioIn"},
    };
    if (oscillator)
    {
        document.graph.nodes.push_back({"cascade", "talas.oscillator", {
            {"frequency", "1000 hz"}, {"amplitude", "0"}}});
        document.graph.connections.push_back(
            {"cascade", "audioOut", "formant", "cascadeInput"});
    }
    return document;
}
}

void testFormantBankProjectAssets()
{
    AssetDirectory directory;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto projectPath = directory.path / "formant.talas.json";
    const auto wavePath = directory.path / "impulse.wav";
    for (const auto oscillator : {false, true})
    {
        // The existing Table WAV codec uses 32767 for finite PCM16 values
        // other than the minimum code. These exact integer codes preserve
        // the shape without assuming the separate audio-stream PCM scale.
        const std::array<std::vector<double>, 1> wave{{{
            24576.0 / 32767.0, 16384.0 / 32767.0, -8192.0 / 32767.0, 0.0}}};
        std::string error;
        require(talas::audio::writePcm16Wav(wavePath, 8000.0, wave, error),
            "Formant impulse WAV should save: " + error);
        const auto original = assetDocument(oscillator);
        require(talas::project::saveProjectFile(projectPath, original).ok(),
            "Formant project with a relative Table asset should save");
        const auto reopened = talas::project::loadProjectFile(projectPath);
        require(reopened.ok(), "Formant project should reopen");
        require(talas::project::serializeProjectDocument(*reopened.document)
                == talas::project::serializeProjectDocument(original),
            "Table resource, overlap count and DC option should survive save/reopen");

        const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
            *reopened.document, registry, projectPath);
        require(loaded.ok() && loaded.snapshots.size() == 1,
            "Reachable relative formant impulse should load into one snapshot");
        require(loaded.snapshots.front()->tableSamples
                    == wave.front()
                && loaded.snapshots.front()->tableSourceSampleRate == 8000,
            "Formant snapshot should retain WAV samples and native source rate");
        auto settings = talas::runtime::RenderSettings{16000.0, 7};
        settings.resourceSnapshots = loaded.snapshots;
        const auto compiled = talas::runtime::compileProject(
            *reopened.document, registry, settings);
        require(compiled.ok(), "Reopened formant asset project should compile");

        // Changing the file after compilation must not change this graph's
        // immutable resource, including when preparing a session later.
        const std::array<std::vector<double>, 1> changedWave{{{0.0, 0.0, 0.0, 0.0}}};
        require(talas::audio::writePcm16Wav(wavePath, 8000.0, changedWave, error),
            "Replacement source fixture should save");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 32);
        require(offline.ok() && offline.mono.size() == 32,
            "Immutable formant asset should render after its source file changes");
        talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
        require(realtime.ready(), "Persisted impulse asset should prepare: "
            + std::string{realtime.error()});
        std::array<double, 32> actual{};
        for (std::size_t offset = 0; offset < actual.size();)
        {
            const auto count = std::min<std::size_t>(7, actual.size() - offset);
            require(realtime.render(std::span<double>{actual}.subspan(offset, count)),
                "Persisted impulse asset should render across partitions");
            offset += count;
        }
        // Native 8 kHz playback into 16 kHz output advances half a table
        // sample per output sample; a 2 kHz fundamental restarts every eight.
        constexpr std::array<double, 8> expected{
            24576.0 / 32767.0, 20480.0 / 32767.0,
            16384.0 / 32767.0, 4096.0 / 32767.0,
            -8192.0 / 32767.0, -4096.0 / 32767.0, 0.0, 0.0};
        for (std::size_t frame = 0; frame < actual.size(); ++frame)
        {
            require(std::isfinite(offline.mono[frame]) && std::isfinite(actual[frame])
                    && std::abs(offline.mono[frame] - expected[frame % 8]) < 1.0e-13
                    && actual[frame] == offline.mono[frame],
                "Persisted formant impulse should follow the native-rate pulse oracle");
        }
    }
}
