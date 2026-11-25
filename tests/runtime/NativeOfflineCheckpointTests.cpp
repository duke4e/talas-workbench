#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace talas::runtime
{
struct OfflineRenderSessionTestAccess
{
    static void rejectAt(std::uint64_t frame)
    {
        (void)OfflineRenderSession::exchangeRejectedTransactionEndFrameForTesting(frame);
    }
    static std::size_t position(const OfflineRenderSession& session) { return session.renderedFrames_; }
    static bool hasDiffuseOwner(const OfflineRenderSession& session)
    {
        return std::ranges::any_of(session.nodeStates_, [](const auto& node) {
            return node.diffuseSpectralStretch != nullptr;
        });
    }
    static const talas::dsp::PreparedTimeFrequencyScale* timeFrequencyScale(const OfflineRenderSession& session)
    {
        for (const auto& node : session.nodeStates_)
            if (node.timeFrequencyScalePitchShifter) return node.timeFrequencyScalePitchShifter.get();
        return nullptr;
    }
    static std::uint64_t exchangeWorkFrames(std::uint64_t value)
    {
#ifndef TALAS_NATIVE_OFFLINE_BENCHMARK
        return OfflineRenderSession::exchangeNativeWorkFramesForTesting(value);
#else
        (void)value;
        return 0;
#endif
    }
};
}
namespace
{
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void testNativeWork(std::size_t blocks, bool measure)
{
    talas::project::ProjectParameterExpression coefficients;
    coefficients.parameterId = "coefficients";
    coefficients.text = "0.5 0.25";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
        {"fir", "talas.firFilter", {coefficients}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"source", "audioOut", "fir", "audioIn"},
        {"fir", "audioOut", "out", "audioIn"}};
    const auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), {64.0, 16});
    require(compiled.ok(), "native checkpoint work fixture must compile");
    talas::runtime::OfflineRenderSession session(*compiled.graph);
    talas::runtime::OfflineRenderSessionTestAccess::exchangeWorkFrames(0);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < blocks; ++block)
    {
        const auto result = session.render(16);
        require(result.ok() && result.mono.size() == 16, "native FIR blocks must render");
        for (std::size_t frame = 0; frame < 16; ++frame)
        {
            const auto n = block * 16 + frame;
            const auto sine = [](std::size_t index) {
                return std::sin(2 * std::numbers::pi_v<double> * index / 64);
            };
            const auto expected = 0.5 * sine(n) + (n ? 0.25 * sine(n - 1) : 0.0);
            require(std::abs(result.mono[frame] - expected) < 1e-12,
                "native transaction boundaries must preserve the independent FIR recurrence");
        }
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto work = talas::runtime::OfflineRenderSessionTestAccess::exchangeWorkFrames(0);
    std::cout << "blocks=" << blocks << " outputFrames=" << blocks * 16
        << " uncheckedFrames=" << work << " seconds=" << seconds << std::endl;
    if (!measure)
        require(work == blocks * 16,
            "native offline transactions must process new frames once, without replaying accepted history");
}

talas::project::ProjectParameterExpression resource(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id); result.resource = std::move(value); return result;
}

void testOwnedProcessorRecovery(bool diffuse)
{
    using namespace talas::runtime;
    using Access = OfflineRenderSessionTestAccess;
    RenderSettings settings{48000.0, 256};
    talas::project::ProjectDocument document;
    if (diffuse)
    {
        settings.sampleRate = 8000;
        std::vector<double> sample(4096);
        for (std::size_t frame = 0; frame < sample.size(); ++frame)
            sample[frame] = 0.5 * std::sin(2 * std::numbers::pi_v<double> * 220 * frame / settings.sampleRate);
        settings.tableResources.push_back({"Tables/NativeCheckpoint", std::move(sample)});
        document.graph.nodes = {{"state", "talas.diffuseSpectralStretch",
            {resource("sample", "Tables/NativeCheckpoint"), {"stretch", "2"}, {"smear", "0.3"},
                {"diffusion", "0.2"}, {"seed", "17"}, {"gate", "1"}, {"reset", "0"}, {"level", "1"}}},
            {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"state", "audioOut", "out", "audioIn"}};
    }
    else
    {
        talas::project::ProjectParameterExpression coefficients;
        coefficients.parameterId = "coefficients"; coefficients.text = "0.5 0.25 -0.125";
        settings.eventValues = {{"NativeGain", 1.0, talas::expr::ValueDomain::Scalar}};
        settings.eventValueChanges = {{1500, "NativeGain", 0.5, talas::expr::ValueDomain::Scalar}};
        document.graph.nodes = {{"source", "talas.oscillator", {{"frequency", "220 hz"}, {"amplitude", "0.75"}}},
            {"tracker", "talas.constant", {{"value", "220 / 24000"}}},
            {"state", "talas.frequencyScale", {{"frequencyScale", "1.5"}, {"maxScale", "2"},
                resource("window", "Private"), {"delay", "0.001"}}},
            {"fir", "talas.firFilter", {coefficients}},
            {"gain", "talas.constant", {{"value", "!NativeGain"}}},
            {"vca", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
        document.graph.connections = {{"source", "audioOut", "state", "audioIn"},
            {"tracker", "value", "state", "freqTracker"}, {"state", "audioOut", "fir", "audioIn"},
            {"fir", "audioOut", "vca", "audioIn"}, {"gain", "value", "vca", "gain"},
            {"vca", "audioOut", "out", "audioIn"}};
    }
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "owned native checkpoint graph must compile");
    OfflineRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    struct ClearFailure { ~ClearFailure() { Access::rejectAt(0); } } clearFailure;
    bool audible = false;
    const auto render = [&](std::size_t frames) {
        const auto expected = reference.render(frames);
        Access::exchangeWorkFrames(0);
        const auto actual = candidate.render(frames);
        const auto work = Access::exchangeWorkFrames(0);
        require(actual.ok() && expected.ok() && actual.mono == expected.mono,
            "repaired native owned DSP, FIR and control history must exactly match uninterrupted continuation");
        require(work == frames, "owned native processors must not replay earlier public transactions");
        audible |= std::ranges::any_of(actual.mono, [](double value) { return std::abs(value) > 1e-7; });
    };
    const auto fail = [&](std::size_t frames, bool cold) {
        const auto position = Access::position(candidate);
        Access::rejectAt(position + frames);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto rejected = candidate.render(frames);
            require(!rejected.ok() && rejected.mono.empty() && rejected.right.empty()
                && rejected.channels.empty() && rejected.midiEvents.empty() && rejected.globalControllerEvents.empty()
                && Access::position(candidate) == position,
                "repeated late native failure must publish no payload and restore its public cursor");
            if (cold) require(!Access::hasDiffuseOwner(candidate),
                "failed lazy native preparation must restore an absent processor owner");
        }
        Access::rejectAt(0);
        render(frames);
    };
    if (diffuse)
    {
        require(!Access::hasDiffuseOwner(candidate), "diffuse native processor must begin with lazy ownership");
        fail(17, true);
    }
    for (int block = 0; block < 5; ++block) render(256);
    fail(257, false);
    for (int block = 0; block < 36; ++block) render(block % 2 ? 137 : 251);
    require(audible, "native rollback oracle must pass through audible populated spectral history");
    candidate.reset(); reference.reset();
    if (diffuse) require(!Access::hasDiffuseOwner(candidate), "native reset must release its lazy processor");
    for (int block = 0; block < 16; ++block) render(256);
}

void testTimeFrequencyScaleModelRecovery()
{
    using namespace talas::runtime;
    using Access = OfflineRenderSessionTestAccess;
    RenderSettings settings{48000, 128};
    std::vector<double> samples(9 * 48000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = .1 + .25 * std::sin(2 * std::numbers::pi * 20 * i / 48000 + .73)
            + .08 * std::sin(2 * std::numbers::pi * 28 * i / 48000 - 1.1);
    settings.tableResources.push_back({"Tables/ModelCheckpoint", std::move(samples)});
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"state", "talas.timeFrequencyScale",
        {resource("sample", "Tables/ModelCheckpoint"), {"frequencyScale", ".0625"},
         {"rate", ".25"}, {"gate", "1"}, {"minInputFreq", "20 hz"}, {"maxInputFreq", "2000 hz"},
         {"maxFreqScale", "2"}, {"detectors", "10"}}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"state", "audioOut", "out", "audioIn"}};
    const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "native component-model recovery graph compiles");
    OfflineRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    struct ClearFailure { ~ClearFailure() { Access::rejectAt(0); } } clearFailure;
    const auto render = [&](std::size_t frames) {
        const auto expected = reference.render(frames), actual = candidate.render(frames);
        require(actual.ok() && expected.ok() && actual.mono == expected.mono,
            "native component-model recovery preserves bit-exact continuation");
    };
    for (const auto cut : {25000U, 60000U, 144013U})
    {
        while (Access::position(candidate) < cut)
            render(std::min<std::size_t>(128, cut - Access::position(candidate)));
        const auto* processor = Access::timeFrequencyScale(candidate);
        require(processor != nullptr, "native source processor is owned before checkpoint");
        if (cut == 25000) require(processor->componentAnalysisPending(), "native rollback covers pending estimation");
        if (cut == 144013) require(processor->componentModelMix() == 1, "native rollback covers active model synthesis");
        Access::rejectAt(cut + 257);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto rejected = candidate.render(257);
            require(!rejected.ok() && rejected.mono.empty() && rejected.right.empty()
                && rejected.channels.empty() && Access::position(candidate) == cut,
                "late native component failure publishes nothing and restores its cursor");
        }
        Access::rejectAt(0);
        render(257);
    }
    render(3 * 8192);
    candidate.reset(); reference.reset();
    render(25000);
    std::cout << "native_component_recovery checkpoints=3 retries=6 reset=1\n";
}
}
int main(int argc, char** argv)
{
    const bool measure = argc == 2 && std::string_view(argv[1]) == "--measure";
    require(argc == 1 || measure, "unknown native checkpoint test selection");
    if (measure)
    {
        for (int repeat = 0; repeat < 3; ++repeat)
            for (std::size_t blocks : {8, 16, 32, 64}) testNativeWork(blocks, true);
    }
    else
    {
        testNativeWork(8, false);
        testOwnedProcessorRecovery(false);
        testOwnedProcessorRecovery(true);
        testTimeFrequencyScaleModelRecovery();
    }
}
