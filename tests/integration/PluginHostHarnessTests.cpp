#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A plugin-shaped host around the engine: it restores state, prepares for a
// sample rate and maximum block, reports latency, takes sample-accurate
// parameter changes and processes whatever block sizes the host delivers.
// It is not shipped; it shows the engine fits behind a plugin API.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const talas::core::NodeRegistry& registry()
{
    static const auto builtins = talas::core::makeBuiltinNodeRegistry();
    return builtins;
}

class HeadlessHost
{
public:
    HeadlessHost() : bridge_(talas::project::ProjectDocument{}, registry()) {}

    [[nodiscard]] bool setState(std::string_view state)
    {
        auto parsed = talas::project::parseProjectDocument(state, registry());
        return parsed.ok()
            && bridge_.editor().replaceDocument(std::move(parsed.document)).ok();
    }

    [[nodiscard]] std::string state() const
    {
        return talas::project::serializeProjectDocument(
            bridge_.editor().document(), registry());
    }

    [[nodiscard]] bool prepare(
        double sampleRate,
        std::size_t maximumBlockFrames,
        talas::runtime::RenderSettings resources = {})
    {
        resources.sampleRate = sampleRate;
        resources.maxBlockFrames = maximumBlockFrames;
        bridge_.setPlaying(true);
        return bridge_.compileAndStage(std::move(resources)).ok();
    }

    [[nodiscard]] std::size_t latencyFrames() const
    {
        const auto status = bridge_.preparedGraphStatus();
        return status.prepared != nullptr
            ? status.prepared->latency.terminalOutputFrames.value_or(0)
            : 0;
    }

    [[nodiscard]] bool setParameter(
        std::string_view name, double value, std::size_t frameOffset)
    {
        return bridge_.scheduleEventValue(
            name, value, talas::expr::ValueDomain::Scalar, frameOffset);
    }

    [[nodiscard]] bool process(std::span<float> mono)
    {
        auto* channel = mono.data();
        return bridge_.render(&channel, 1, mono.size());
    }

    [[nodiscard]] talas::project::ProjectEditor& editor()
    {
        return bridge_.editor();
    }

private:
    talas::runtime::ProjectRuntimeBridge bridge_;
};

talas::project::ProjectDocument toneDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"osc", "talas.oscillator", {
            Parameter{"frequency", "441 hz"}, Parameter{"amplitude", "!Level"}}},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::VirtualControl level;
    level.eventName = "Level";
    level.value = 0.5;
    document.virtualControls = {level};
    return document;
}

talas::project::ProjectDocument stretchDocument()
{
    Parameter sample;
    sample.parameterId = "sample";
    sample.resource = "Samples/Tone";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"stretch", "talas.timeFrequencyScale", {
            Parameter{"frequencyScale", "1"}, Parameter{"rate", "0.5"},
            sample}},
    };
    document.graph.connections = {{"stretch", "audioOut", "out", "audioIn"}};
    return document;
}

talas::runtime::RenderSettings toneTable(double sampleRate)
{
    std::vector<double> samples(16384);
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        samples[index] = 0.25 * std::sin(
            2.0 * std::numbers::pi * 440.0 * static_cast<double>(index)
            / sampleRate);
    }
    talas::runtime::RenderSettings settings;
    settings.tableResources = {{"Samples/Tone", std::move(samples)}};
    return settings;
}

std::string serialize(const talas::project::ProjectDocument& document)
{
    return talas::project::serializeProjectDocument(document, registry());
}

void processInBlocks(
    HeadlessHost& host, std::span<float> output, std::span<const std::size_t> sizes)
{
    for (std::size_t offset = 0, index = 0; offset < output.size(); ++index)
    {
        const auto frames = std::min(
            sizes[index % sizes.size()], output.size() - offset);
        require(host.process(output.subspan(offset, frames)),
            "the host should process every block it delivers");
        offset += frames;
    }
}

void testIrregularHostBlocksMatchRegularBlocks()
{
    HeadlessHost regular;
    HeadlessHost irregular;
    require(regular.setState(serialize(toneDocument()))
            && irregular.setState(serialize(toneDocument()))
            && regular.prepare(48000.0, 512)
            && irregular.prepare(48000.0, 512),
        "both hosts should restore and prepare the tone");
    std::vector<float> expected(4096);
    std::vector<float> actual(4096);
    constexpr std::array<std::size_t, 1> whole{512};
    constexpr std::array<std::size_t, 8> ragged{1, 17, 128, 511, 3, 64, 512, 7};
    processInBlocks(regular, expected, whole);
    processInBlocks(irregular, actual, ragged);
    require(actual == expected
            && std::ranges::any_of(expected, [](float sample) { return sample != 0.0F; }),
        "host block sizes must not change the output");
}

void testParameterChangesLandOnTheirFrame()
{
    HeadlessHost host;
    require(host.setState(serialize(toneDocument())) && host.prepare(48000.0, 256),
        "the host should prepare the tone");
    std::vector<float> block(256);
    require(host.setParameter("Level", 0.0, 100) && host.process(block),
        "the host should schedule a parameter change inside the block");
    require(std::ranges::any_of(std::span{block}.first(100),
                [](float sample) { return sample != 0.0F; })
            && std::ranges::all_of(std::span{block}.subspan(100),
                [](float sample) { return sample == 0.0F; }),
        "a parameter change should take effect exactly at its frame");
}

void testStateRestoresTheSameSound()
{
    HeadlessHost original;
    require(original.setState(serialize(toneDocument()))
            && original.editor().setVirtualControlValue("Level", 0.25).ok(),
        "the original host should take an edit");
    HeadlessHost restored;
    require(restored.setState(original.state())
            && original.prepare(44100.0, 128)
            && restored.prepare(44100.0, 128),
        "a restored host should prepare from the saved state");
    std::vector<float> expected(1024);
    std::vector<float> actual(1024);
    constexpr std::array<std::size_t, 1> blocks{128};
    processInBlocks(original, expected, blocks);
    processInBlocks(restored, actual, blocks);
    const auto peak = std::ranges::max(expected, {}, [](float sample) {
        return std::abs(sample);
    });
    require(actual == expected && std::abs(std::abs(peak) - 0.25F) < 1.0e-3F,
        "saved state should restore the same sound, including the edit");
}

void testLatencyFollowsThePreparedWindow()
{
    HeadlessHost tone;
    require(tone.setState(serialize(toneDocument()))
            && tone.prepare(48000.0, 256) && tone.latencyFrames() == 0,
        "a plain oscillator should report no latency");
    HeadlessHost stretch;
    require(stretch.setState(serialize(stretchDocument())),
        "the stretch host should restore its state");
    for (const auto profile : {std::pair{44100.0, 1279u}, std::pair{48000.0, 1279u},
            std::pair{96000.0, 2559u}})
    {
        require(stretch.prepare(profile.first, 256, toneTable(profile.first))
                && stretch.latencyFrames() == profile.second,
            "the reported latency should follow the prepared analysis window at "
                + std::to_string(profile.first) + " Hz: "
                + std::to_string(stretch.latencyFrames()));
        std::vector<float> block(256);
        require(stretch.process(block), "the re-prepared host should process");
    }
}
}

int main()
{
    try
    {
        testIrregularHostBlocksMatchRegularBlocks();
        testParameterChangesLandOnTheirFrame();
        testStateRestoresTheSameSound();
        testLatencyFollowsThePreparedWindow();
        std::cout << "Plugin host harness tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
