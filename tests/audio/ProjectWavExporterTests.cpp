#include "talas/audio/ProjectWavExporter.h"
#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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

talas::project::ProjectNode node(
    std::string id,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {std::move(fromNode), std::move(fromPort), std::move(toNode), std::move(toPort)};
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression choiceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectDocument timelineDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(
        node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.nodes.push_back(node("gain", "talas.constant", {{"value", "!Gain"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.25;
    document.virtualControls.push_back(gain);
    document.timeline.eventValues = {
        {0.375, "Gain", talas::expr::ValueDomain::Scalar, 0.5},
        {0.75, "Gain", talas::expr::ValueDomain::Scalar, 0.75},
    };
    return document;
}

talas::project::ProjectDocument missingComponentDocument()
{
    talas::project::ProjectDocument document;
    auto missing = node("missing1", "org.example.component");
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        {},
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    missing.preservedComponent = std::move(component);
    document.graph.nodes = {
        std::move(missing),
        node("out", "talas.audioOutput"),
        node(
            "unreachableSample",
            "talas.sample",
            {resourceParameter(
                "sample",
                "UnreachableAsset")}),
    };
    document.graph.connections.push_back(
        connection("missing1", "audioOut", "out", "audioIn"));
    document.assets.push_back({
        "UnreachableAsset",
        talas::project::ProjectAssetKind::Table,
        "/definitely/not/a/real/resource.table",
    });
    return document;
}

talas::project::ProjectDocument discrete4Document()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output4"));
    document.graph.nodes.push_back(node("matrix", "talas.matrix4", {
        textParameter("insToOut1", "1 0 0 0"),
        textParameter("insToOut2", "0 1 0 0"),
        textParameter("insToOut3", "0 0 1 0"),
        textParameter("insToOut4", "0 0 0 1"),
    }));
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        const auto id = "osc" + std::to_string(channel + 1);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {
            {"frequency", "1 hz"},
            {"amplitude", std::to_string(static_cast<double>(channel + 1) * 0.1)},
        }));
        document.graph.connections.push_back(connection(
            id, "audioOut", "matrix", "in" + std::to_string(channel + 1)));
        document.graph.connections.push_back(connection(
            "matrix", "out" + std::to_string(channel + 1),
            "out", "out" + std::to_string(channel + 1)));
    }
    return document;
}

talas::project::ProjectDocument spatialPanDiscrete4Document()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output4"));
    document.graph.nodes.push_back(node("pan", "talas.pan", {
        {"pan", "0.25"},
        {"scale", "1"},
        choiceParameter("type", "power"),
        choiceParameter("layout", "discrete4"),
    }));
    document.graph.nodes.push_back(node(
        "osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.connections.push_back(
        connection("osc", "audioOut", "pan", "audioIn"));
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.connections.push_back(
            connection("pan", port, "out", port));
    }
    return document;
}

talas::project::ProjectDocument horizontalAmbisonicDiscrete4Document()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output4"));
    document.graph.nodes.push_back(node(
        "decode",
        "talas.horizontalAmbisonicDecoder",
        {
            {"scale", "0.5"},
            choiceParameter("layout", "discrete4"),
        }));
    document.graph.nodes.push_back(node(
        "encode",
        "talas.horizontalAmbisonicEncoder",
        {{"azimuth", "0.25"}, {"scale", "1"}}));
    document.graph.nodes.push_back(node(
        "osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}));
    document.graph.connections.push_back(
        connection("osc", "audioOut", "encode", "audioIn"));
    document.graph.connections.push_back(
        connection("encode", "wOut", "decode", "wIn"));
    document.graph.connections.push_back(
        connection("encode", "xOut", "decode", "xIn"));
    document.graph.connections.push_back(
        connection("encode", "yOut", "decode", "yIn"));
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.connections.push_back(
            connection("decode", port, "out", port));
    }
    return document;
}

talas::project::ProjectDocument discrete8Document()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output8"));
    std::vector<talas::project::ProjectParameterExpression> rows;
    for (std::size_t output = 0; output < 8; ++output)
    {
        std::string row;
        for (std::size_t input = 0; input < 8; ++input)
        {
            if (!row.empty())
                row += ' ';
            row += input == output ? '1' : '0';
        }
        rows.push_back(textParameter(
            "insToOut" + std::to_string(output + 1),
            std::move(row)));
    }
    document.graph.nodes.push_back(node("matrix", "talas.matrix8", std::move(rows)));
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        const auto id = "osc" + std::to_string(channel + 1);
        const auto port = "out" + std::to_string(channel + 1);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {
            {"frequency", "1 hz"},
            {"amplitude", std::to_string(static_cast<double>(channel + 1) * 0.05)},
        }));
        document.graph.connections.push_back(connection(
            id, "audioOut", "matrix", "in" + std::to_string(channel + 1)));
        document.graph.connections.push_back(connection("matrix", port, "out", port));
    }
    return document;
}

talas::project::ProjectDocument discrete6Document()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.output6"));
    for (std::size_t channel = 0; channel < 6; ++channel)
    {
        const auto id = "osc" + std::to_string(channel + 1);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {
            {"frequency", "1 hz"},
            {"amplitude", std::to_string(static_cast<double>(channel + 1) * 0.1)},
        }));
        document.graph.connections.push_back(connection(
            id, "audioOut", "out", "out" + std::to_string(channel + 1)));
    }
    return document;
}

talas::project::ProjectDocument pairedStereoOutputDocument(
    std::size_t channelCount)
{
    const auto pairPorts = channelCount == 4
        ? std::vector<std::string>{"out12", "out34"}
        : std::vector<std::string>{
            "out12", "out34", "out56", "out78"};
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node(
        "out",
        channelCount == 4
            ? "talas.stereoInOutput4"
            : "talas.stereoInOutput8"));
    for (std::size_t pair = 0; pair < pairPorts.size(); ++pair)
    {
        const auto id = "pair" + std::to_string(pair + 1);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {
            {"frequency", "1 hz"},
            {"amplitude", std::to_string(
                static_cast<double>(pair + 1) * 0.1)},
        }));
        document.graph.connections.push_back(connection(
            id, "audioOut", "out", pairPorts[pair]));
    }
    return document;
}

std::filesystem::path temporaryPath(std::string_view suffix)
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("talas-project-export-test-" + std::to_string(now) + "-"
           + std::to_string(counter++) + std::string(suffix));
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, {}};
}

void testExportsPersistedControlsAndTimelineAcrossStreamingBlocks()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    std::vector<double> progress;
    options.reportProgress = [&](double value) { progress.push_back(value); };

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(result.ok(), "Timeline project should export: " + result.error);
    require(result.frameCount == 8 && result.channelCount == 1,
            "export should report exact mono render shape");
    require(!progress.empty() && std::abs(progress.back() - 1.0) < 1.0e-12,
            "export should report terminal progress");

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok(), "exported WAV should be readable: " + wav.error);
    require(wav.channels.size() == 1 && wav.channels.front().size() == 8,
            "exported WAV should contain the requested frames");
    const std::vector<double> gains = {0.25, 0.25, 0.25, 0.5, 0.5, 0.5, 0.75, 0.75};
    for (std::size_t frame = 0; frame < gains.size(); ++frame)
    {
        const auto oscillator = std::sin(
            2.0 * std::acos(-1.0) * static_cast<double>(frame) / 8.0);
        require(std::abs(wav.channels.front()[frame] - oscillator * gains[frame]) < 4.0e-5,
                "streamed Timeline sample should match frame " + std::to_string(frame));
    }
}

void testCancellationPreservesExistingDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    options.shouldCancel = [] { return true; };

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(result.cancelled(), "cancelled export should report cancellation");
    require(readText(path) == "existing destination",
            "cancelled export should preserve the previous destination");
    std::filesystem::remove(path);
}

void testPublicationAdmissionPreservesExistingDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    int admissionChecks = 0;
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    options.admitPublication = [&] {
        ++admissionChecks;
        return false;
    };

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(
        result.cancelled() && admissionChecks == 1,
        "terminal authority rejection should cancel exactly once");
    require(
        readText(path) == "existing destination",
        "terminal authority rejection must preserve the previous WAV");
    std::filesystem::remove(path);
}

void testAssetPayloadCancellationMapsToCancelledExport()
{
    const auto source = temporaryPath("-table.txt");
    const auto destination = temporaryPath(".wav");
    {
        std::ofstream output(source, std::ios::binary);
        output << "1 0 -1";
    }
    {
        std::ofstream output(destination, std::ios::binary);
        output << "existing destination";
    }

    talas::project::ProjectDocument document;
    document.assets.push_back({
        "Shape",
        talas::project::ProjectAssetKind::Table,
        source.string(),
    });
    document.graph.nodes.push_back(
        node(
            "sample",
            "talas.sample",
            {resourceParameter("sample", "Shape")}));

    int cancellationChecks = 0;
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = destination;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    options.shouldCancel = [&] {
        return ++cancellationChecks >= 4;
    };

    const auto result = talas::audio::exportProjectToPcm16Wav(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(
        result.cancelled(),
        "resource payload cancellation should remain typed through WAV export");
    require(
        readText(destination) == "existing destination",
        "asset preparation cancellation should preserve the previous destination");
    std::filesystem::remove(source);
    std::filesystem::remove(destination);
}

void testExportRefusesToReplaceAProjectAsset()
{
    const auto source = temporaryPath("-table.txt");
    {
        std::ofstream output(source, std::ios::binary);
        output << "1 0 -1";
    }
    auto document = timelineDocument();
    document.assets.push_back({
        "Shape",
        talas::project::ProjectAssetKind::Table,
        source.string(),
    });

    talas::audio::ProjectWavExportOptions options;
    options.outputPath = source;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    const auto result = talas::audio::exportProjectToPcm16Wav(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    const auto preserved = readText(source) == "1 0 -1";
    std::filesystem::remove(source);
    require(!result.ok() && preserved,
        "an export must not overwrite an asset of the project: " + result.error);
}

void testExportsMatrix4Output4AsStableFourChannelWav()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        discrete4Document(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    require(result.ok(), "Matrix4 -> Output4 project should export: " + result.error);
    require(result.frameCount == 4 && result.channelCount == 4,
            "Discrete4 export should report four channels");

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok(), "Discrete4 WAV should be readable: " + wav.error);
    require(wav.channels.size() == 4, "Discrete4 WAV should retain four channels");
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        require(wav.channels[channel].size() == 4, "Discrete4 WAV channel should retain every frame");
        require(
            std::abs(wav.channels[channel][2]
                - static_cast<double>(channel + 1) * 0.1) < 4.0e-5,
            "Discrete4 WAV channel order should remain stable");
    }
}

void testExportsMatrix8Output8AsStableEightChannelWav()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        discrete8Document(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    require(result.ok(), "Matrix8 -> Output8 project should export: " + result.error);
    require(result.frameCount == 4 && result.channelCount == 8,
        "Discrete8 export should report eight channels");

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok() && wav.channels.size() == 8,
        "Discrete8 WAV should retain eight channels");
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        require(wav.channels[channel].size() == 4,
            "Discrete8 WAV channel should retain every frame");
        require(
            std::abs(wav.channels[channel][2]
                - static_cast<double>(channel + 1) * 0.05) < 4.0e-5,
            "Discrete8 WAV channel order should remain stable");
    }
}

void testExportsOutput6AsStableSixChannelWav()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        discrete6Document(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    require(result.ok() && result.frameCount == 4
            && result.channelCount == 6,
        "Output 6 should export exactly six channels: " + result.error);

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok() && wav.channels.size() == 6,
        "Output 6 WAV should retain six ordered channels");
    for (std::size_t channel = 0; channel < 6; ++channel)
        require(wav.channels[channel].size() == 4
                && std::abs(wav.channels[channel][2]
                    - static_cast<double>(channel + 1) * 0.1) < 4.0e-5,
            "Output 6 WAV should preserve channel order and samples");
}

void testExportsPairedStereoTerminalsWithoutDeduplicatingChannels()
{
    for (const auto channelCount : {std::size_t{4}, std::size_t{8}})
    {
        const auto path = temporaryPath(".wav");
        talas::audio::ProjectWavExportOptions options;
        options.outputPath = path;
        options.sampleRate = 8.0;
        options.durationSeconds = 0.5;
        options.maxBlockFrames = 2;

        const auto result = talas::audio::exportProjectToPcm16Wav(
            pairedStereoOutputDocument(channelCount),
            talas::core::makeBuiltinNodeRegistry(),
            std::move(options));
        require(
            result.ok()
                && result.frameCount == 4
                && result.channelCount == channelCount,
            "paired stereo terminal should export every logical channel: "
                + result.error);

        const auto wav = talas::audio::readPcm16Wav(path);
        std::filesystem::remove(path);
        require(
            wav.ok() && wav.channels.size() == channelCount,
            "paired stereo WAV should retain exact channel count");
        for (std::size_t pair = 0; pair < channelCount / 2; ++pair)
        {
            require(
                wav.channels[pair * 2]
                        == wav.channels[(pair * 2) + 1]
                    && std::abs(
                        wav.channels[pair * 2][2]
                            - static_cast<double>(pair + 1) * 0.1)
                        < 4.0e-5,
                "paired stereo WAV should preserve duplicated pair ordinals");
        }
    }
}

void testExportsSpatialPanAsStableDiscrete4Wav()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        spatialPanDiscrete4Document(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    require(result.ok(), "spatial Pan project should export: " + result.error);
    require(result.frameCount == 4 && result.channelCount == 4,
            "spatial Pan export should report four channels");

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok() && wav.channels.size() == 4,
            "spatial Pan WAV should retain four channels");
    require(std::abs(wav.channels[1][2] - 1.0) < 4.0e-5,
            "spatial Pan WAV should route the exact speaker-two impulse");
    for (const auto channel : {std::size_t{0}, std::size_t{2}, std::size_t{3}})
        require(std::abs(wav.channels[channel][2]) < 4.0e-5,
                "spatial Pan WAV should keep other speaker ordinals silent");
}

void testExportsHorizontalAmbisonicsAsStableDiscrete4Wav()
{
    const auto path = temporaryPath(".wav");
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        horizontalAmbisonicDiscrete4Document(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));
    require(result.ok(),
            "horizontal Ambisonic project should export: " + result.error);
    require(result.frameCount == 4 && result.channelCount == 4,
            "horizontal Ambisonic export should report four channels");

    const auto wav = talas::audio::readPcm16Wav(path);
    std::filesystem::remove(path);
    require(wav.ok() && wav.channels.size() == 4,
            "horizontal Ambisonic WAV should retain four channels");
    double energy = 0.0;
    for (const auto& channel : wav.channels)
    {
        require(channel.size() == 4,
                "horizontal Ambisonic WAV should retain every frame");
        energy += channel[2] * channel[2];
    }
    require(std::abs(energy - 0.25) < 2.0e-4,
            "horizontal Ambisonic WAV should preserve scaled decoded ring energy");
}

void testCancellationAfterPartialWritePreservesExistingDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    bool renderedFirstBlock = false;
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;
    options.shouldCancel = [&] { return renderedFirstBlock; };
    options.reportProgress = [&](double) { renderedFirstBlock = true; };

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(result.cancelled(), "partially written export should report cancellation");
    require(readText(path) == "existing destination",
            "partial cancellation should preserve the previous destination");
    std::filesystem::remove(path);
}

void testSuccessfulExportAtomicallyReplacesExistingDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(result.ok(), "successful export should replace an existing destination");
    const auto wav = talas::audio::readPcm16Wav(path);
    require(wav.ok() && wav.channels.front().size() == 4,
            "replacement destination should be the complete exported WAV");
    std::filesystem::remove(path);
}

void testInvalidOptionsFailBeforeReplacingDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.durationSeconds = 0.0;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(!result.ok() && !result.cancelled(),
            "invalid duration should fail export");
    require(result.error.find("duration") != std::string::npos,
            "invalid duration failure should explain the option");
    require(readText(path) == "existing destination",
            "invalid export should preserve the previous destination");
    std::filesystem::remove(path);
}

void testRejectsNoAudioTerminalInsteadOfExportingSilentPreview()
{
    const auto path = temporaryPath(".wav");
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(
        node("terminal", "talas.midiOutputEventInBytes", {
            textParameter("bytes", "16r90 64 16r7f"),
        }));
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 1.0;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(!result.ok() && result.error.find("no audio channels") != std::string::npos,
            "no-audio terminal should not become a successful silent WAV");
    require(!std::filesystem::exists(path),
            "no-audio terminal rejection should not publish a WAV");
}

void testRejectsNonRegularExistingDestination()
{
    const auto path = temporaryPath("-directory");
    std::filesystem::create_directory(path);
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        timelineDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(!result.ok() && result.error.find("regular file") != std::string::npos,
            "directory destination should fail without replacement");
    require(std::filesystem::is_directory(path),
            "failed directory destination export must preserve the directory");
    std::filesystem::remove(path);
}

void testMissingComponentFailsBeforeAssetLoadingAndPreservesDestination()
{
    const auto path = temporaryPath(".wav");
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    talas::audio::ProjectWavExportOptions options;
    options.outputPath = path;
    options.sampleRate = 8.0;
    options.durationSeconds = 0.5;
    options.maxBlockFrames = 2;

    const auto result = talas::audio::exportProjectToPcm16Wav(
        missingComponentDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(options));

    require(
        !result.ok()
            && result.error == "Project compilation failed"
            && !result.diagnostics.empty()
            && result.diagnostics.front().nodeId == "missing1"
            && result.diagnostics.front().message.find(
                   "org.example.component")
                != std::string::npos,
        "WAV export should reject a missing component before resolving unrelated assets");
    require(
        readText(path) == "existing destination",
        "missing-component export failure should preserve the previous destination byte-for-byte");
    std::filesystem::remove(path);
}
}

int main()
{
    testExportsPersistedControlsAndTimelineAcrossStreamingBlocks();
    testExportsMatrix4Output4AsStableFourChannelWav();
    testExportsMatrix8Output8AsStableEightChannelWav();
    testExportsOutput6AsStableSixChannelWav();
    testExportsPairedStereoTerminalsWithoutDeduplicatingChannels();
    testExportsSpatialPanAsStableDiscrete4Wav();
    testExportsHorizontalAmbisonicsAsStableDiscrete4Wav();
    testCancellationPreservesExistingDestination();
    testPublicationAdmissionPreservesExistingDestination();
    testAssetPayloadCancellationMapsToCancelledExport();
    testExportRefusesToReplaceAProjectAsset();
    testCancellationAfterPartialWritePreservesExistingDestination();
    testSuccessfulExportAtomicallyReplacesExistingDestination();
    testInvalidOptionsFailBeforeReplacingDestination();
    testRejectsNoAudioTerminalInsteadOfExportingSilentPreview();
    testRejectsNonRegularExistingDestination();
    testMissingComponentFailsBeforeAssetLoadingAndPreservesDestination();
    return 0;
}
