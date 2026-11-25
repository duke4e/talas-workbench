#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <limits>
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

// One-frame blocks, as a feedback loop forces, must not rescan a long
// audio input in every block.
void testLongAudioInputRendersInLinearTime()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "out", "audioIn"},
    };
    constexpr std::size_t frames = 160'000;
    talas::runtime::RenderSettings settings{8000.0, 1};
    settings.audioInputChannels.push_back(
        {1, std::vector<double>(frames, 0.25)});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "long audio input graph should compile");
    const auto started = std::chrono::steady_clock::now();
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, frames);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(rendered.ok() && rendered.mono.size() == frames
            && rendered.mono.front() == 0.25 && rendered.mono.back() == 0.25,
        "long audio input should render exactly");
    require(elapsed < std::chrono::seconds(5),
        "one-frame blocks must not rescan the whole audio input per block: "
            + std::to_string(std::chrono::duration<double>(elapsed).count())
            + " s");
}

void testNonFiniteInputStillFailsTheRender()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"input", "talas.audioInput", {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8000.0, 4};
    std::vector<double> samples(64, 0.25);
    samples.back() = std::numeric_limits<double>::quiet_NaN();
    settings.audioInputChannels.push_back({1, std::move(samples)});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    const auto rendered = compiled.ok()
        ? talas::runtime::renderOffline(*compiled.graph, 4)
        : talas::runtime::OfflineRenderResult{};
    require(!compiled.ok() || !rendered.ok(),
        "a non-finite sample anywhere in the input must still fail the render");
}

// Sample reads a RAM table of at most 1048576 frames in both contexts; a
// longer table must say so instead of failing with a generic message.
void testSampleTableLimitIsNamedOfflineAndRealtime()
{
    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };
    const auto boolean = [](std::string id, bool value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.boolean = value;
        return parameter;
    };
    talas::project::ProjectParameterExpression sample;
    sample.parameterId = "sample";
    sample.resource = "Long";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"sample", "talas.sample", {
            expression("frequency", "440 hz"),
            expression("gate", "1.0"),
            sample,
            boolean("setLoop", false),
            expression("loopStart", "0.0"),
            expression("loopEnd", "1.0"),
            boolean("loopFade", false),
            expression("start", "0.0"),
            expression("end", "1.0"),
            boolean("fromMemoryWriter", false),
            expression("attackTime", "0 s"),
            expression("releaseTime", "0 s"),
            expression("scale", "0.75"),
        }},
    };
    document.graph.connections = {
        {"sample", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{48000.0, 64};
    settings.tableResources.push_back(
        {"Long", std::vector<double>(1'048'577, 0.25)});
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "long Sample table graph should compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 64);
    require(!offline.ok() && !offline.diagnostics.empty()
            && offline.diagnostics.front().message.find("1048576 frames")
                != std::string::npos,
        "offline Sample should name its table limit: "
            + (offline.diagnostics.empty()
                   ? std::string{}
                   : offline.diagnostics.front().message));
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(!session.ready()
            && std::string{session.error()}.find("1048576 frames")
                != std::string::npos,
        "realtime Sample should name the same table limit: "
            + std::string{session.error()});
}
}

int main()
{
    try
    {
        testLongAudioInputRendersInLinearTime();
        testNonFiniteInputStillFailsTheRender();
        testSampleTableLimitIsNamedOfflineAndRealtime();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Offline render scaling tests passed\n";
    return 0;
}
