#include "../../src/runtime/PreparedSourceCallback.h"
#include "talas/core/BuiltinNodes.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace { std::atomic<bool> counting{false}; std::atomic<std::size_t> allocations{0}; }
void* operator new(std::size_t n)
{
    if (counting) ++allocations;
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
using namespace talas::runtime;
void require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}
PreparedRuntimeGraph oscillator(bool failing = false, bool analysis = false, bool ramp = false, std::size_t maxFrames = 8)
{
    talas::project::ProjectDocument d;
    d.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "!Gain"}}},
        {"out", "talas.audioOutput", {}}};
    d.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    if (failing)
    {
        d.graph.nodes.push_back({"device", "talas.audioInput", {}});
        d.graph.nodes.push_back({"mix", "talas.mixer", {}});
        d.graph.nodes.push_back({"gain", "talas.vca", {{"gain", "1e308"}}});
        d.graph.connections = {{"osc", "audioOut", "mix", "in1"},
            {"device", "audioOut", "mix", "in2"}, {"mix", "audioOut", "gain", "audioIn"},
            {"gain", "audioOut", "out", "audioIn"}};
    }
    RenderSettings s; s.sampleRate = 1000; s.maxBlockFrames = maxFrames;
    s.eventValues = {{"Gain", 0.125, talas::expr::ValueDomain::Scalar}};
    s.eventValueChanges = {{3, "Gain", 0.0625, talas::expr::ValueDomain::Scalar}};
    if (ramp)
        s.eventValueChanges = {{0, "Gain", 0.125, talas::expr::ValueDomain::Scalar, talas::core::AutomationInterpolation::Linear},
            {6, "Gain", 0.0625, talas::expr::ValueDomain::Scalar}};
    if (analysis)
    {
        s.enableRealtimeAnalysisCapture = true;
        s.realtimeAnalysisTargets.push_back({{}, {}, "osc", "audioOut",
            RealtimeAnalysisKind::Oscilloscope, RealtimeAnalysisPayloadKind::AudioBlock, 1, "source"});
    }
    auto compiled = compileProject(d, talas::core::makeBuiltinNodeRegistry(), s);
    require(compiled.ok(), "source compile");
    return std::move(*compiled.graph);
}
void testTentativeRollbackAndCommit()
{
    auto graph = oscillator();
    PreparedSourceCallback source(graph);
    require(source.ready(), source.error());
    std::array<double, 8> first{}, retry{}, following{};
    double* out[] = {first.data()};
    allocations = 0; counting = true;
    const auto rendered = source.renderTentative({}, out, 1, 8);
    const auto pending = source.pending();
    source.rollback();
    counting = false;
    require(rendered && pending && !source.pending() && allocations == 0, "tentative render and rollback without allocation");
    out[0] = retry.data();
    require(source.renderTentative({}, out, 1, 8), "retry tentative source");
    require(first == retry, "parent abort must restore source DSP, automation and frame cursor exactly");
    source.commit();
    require(!source.pending(), "commit retires tentative transaction");
    out[0] = following.data();
    require(source.renderTentative({}, out, 1, 8), "render after parent commit");
    source.commit();
    for (std::size_t f = 0; f < 8; ++f)
        require(std::abs(following[f] - 0.0625 * std::sin(2 * std::numbers::pi * 7 * (f + 8) / 1000)) < 1e-12,
            "committed source continues from the exact next frame");
}
void testCompositionSuspensionRetainsAutomationAndAdvancesDsp()
{
    for (const auto ramp : {false, true})
    {
        PreparedSourceCallback source(oscillator(false, false, ramp));
        require(source.ready(), source.error());
        std::size_t sample = 0, composition = 0;
        for (const auto [frames, advancing] : std::array<std::pair<std::size_t, bool>, 7>{
            {{2, true}, {3, false}, {5, false}, {1, true}, {4, false}, {3, true}, {3, true}}})
        {
            std::array<double, 8> audio{}, retry{};
            double* outputs[] = {audio.data()};
            allocations = 0; counting = true;
            const auto first = source.renderTentative({}, outputs, 1, frames, advancing);
            source.rollback();
            outputs[0] = retry.data();
            const auto second = source.renderTentative({}, outputs, 1, frames, advancing);
            source.commit();
            counting = false;
            require(first && second && allocations == 0, "independent composition clock is transactional and allocation free");
            for (std::size_t f = 0; f < frames; ++f)
            {
                const auto cursor = composition + (advancing ? f : 0);
                const auto gain = ramp ? 0.125 - 0.0625 * std::min<std::size_t>(cursor, 6) / 6
                    : cursor < 3 ? 0.125 : 0.0625;
                const auto expected = gain * std::sin(2 * std::numbers::pi * 7 * (sample + f) / 1000);
                require(audio[f] == retry[f] && std::abs(retry[f] - expected) < 1e-12,
                    "composition suspension holds automation while oscillator sample time continues");
            }
            sample += frames;
            if (advancing) composition += frames;
        }
    }
}
void testCompositionClockResetAndOfflineRollback()
{
    PreparedSourceCallback source(oscillator());
    std::array<double, 8> audio{}, reference{};
    double* outputs[] = {audio.data()};
    require(source.renderTentative({}, outputs, 1, 2), "advance composition to 2"); source.commit();
    require(source.renderTentative({}, outputs, 1, 8, false), "hold composition while advancing sample time to 10"); source.commit();
    require(source.prepareResetPoint(), "capture distinct composition and sample cursors");
    {
        auto checkpoint = source.beginOfflineTransaction();
        require(checkpoint != nullptr, "capture offline source clocks");
        require(source.renderTentative({}, outputs, 1, 4), "advance inside offline transaction"); source.commit();
        reference = audio;
    }
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        allocations = 0; counting = true;
        // First iteration relies on the offline rollback, later ones on reusable reset.
        const auto restored = attempt == 0 || source.resetToPreparedPoint();
        const auto rendered = source.renderTentative({}, outputs, 1, 4);
        source.commit();
        counting = false;
        require(restored && rendered && allocations == 0, "both source clocks restore without callback allocation");
        for (std::size_t f = 0; f < 4; ++f)
        {
            const auto expected = (f == 0 ? 0.125 : 0.0625)
                * std::sin(2 * std::numbers::pi * 7 * (10 + f) / 1000);
            require(audio[f] == reference[f] && std::abs(audio[f] - expected) < 1e-12,
                "reset and offline abort restore distinct composition and DSP positions");
        }
    }
}
void testMultiplePreparedTransportPoints()
{
    auto graph = oscillator();
    PreparedSourceCallback source(graph), other(graph);
    require(source.prepareResetPoint(), "retain initial default source reset");
    auto initial = source.capturePreparedPoint();
    require(bool(initial), "capture independent initial transport point");
    std::array<double, 8> audio{}; double* outputs[] = {audio.data()};
    require(source.renderTentative({}, outputs, 1, 2), "prepare composition 2"); source.commit();
    require(source.renderTentative({}, outputs, 1, 8, false), "prepare sample 10 while composition stays 2"); source.commit();
    auto held = source.capturePreparedPoint(); require(bool(held), "capture held transport point");
    require(source.renderTentative({}, outputs, 1, 4), "prepare composition 6/sample 14"); source.commit();
    auto later = source.capturePreparedPoint(); require(bool(later), "capture later transport point without replacing earlier points");
    for (int attempt = 0; attempt < 3; ++attempt)
        for (int point = 0; point < 3; ++point)
        {
            auto& prepared = point == 0 ? *initial : point == 1 ? *held : *later;
            allocations = 0; counting = true;
            const auto restored = source.restorePreparedPoint(prepared);
            const auto rejected = !other.restorePreparedPoint(prepared);
            const auto rendered = source.renderTentative({}, outputs, 1, 4);
            source.rollback();
            const auto retried = source.renderTentative({}, outputs, 1, 4);
            source.commit(); counting = false;
            require(restored && rejected && rendered && retried && allocations == 0,
                "prepared points are owner-bound, reusable, allocation-free and rollback-safe");
            const auto composition = point == 0 ? 0 : point == 1 ? 2 : 6;
            const auto sample = point == 0 ? 0 : point == 1 ? 10 : 14;
            for (std::size_t f = 0; f < 4; ++f)
                require(std::abs(audio[f] - (composition + f < 3 ? 0.125 : 0.0625)
                    * std::sin(2 * std::numbers::pi * 7 * (sample + f) / 1000)) < 1e-12,
                    "independent points restore both clocks, phase and automation");
        }
    require(other.renderTentative({}, outputs, 1, 2), "foreign point rejection keeps destination intact"); other.commit();
    require(audio[0] == 0 && std::abs(audio[1] - 0.125 * std::sin(2 * std::numbers::pi * 7 / 1000)) < 1e-12,
        "foreign point cannot change another source");
    require(source.resetToPreparedPoint(), "named points preserve the original default reset point");
    require(source.renderTentative({}, outputs, 1, 2), "open transaction at initial point");
    require(!source.capturePreparedPoint() && !source.restorePreparedPoint(*later) && source.pending(),
        "capture and restore reject outstanding transactions without aborting them");
    source.commit();
    require(audio[0] == 0, "default reset still starts at its original phase");
}
void testPreparedPointOutlivesOwner()
{
    auto graph = oscillator();
    std::optional<PreparedSourceCallback> source;
    source.emplace(graph);
    auto point = source->capturePreparedPoint(); require(bool(point), "capture before owner destruction");
    source.reset(); source.emplace(graph); // Reuse the exact C++ object address.
    allocations = 0; counting = true;
    const auto rejected = !source->restorePreparedPoint(*point);
    counting = false;
    require(rejected && allocations == 0, "reused object address must not inherit a retired source's transport authority");
}

void testSegmentedTransactionRetainsClocksAndAnalysis()
{
    auto graph = oscillator(false, true, false, 16);
    auto capture = graph.analysisCapture();
    PreparedSourceCallback source(graph);
    require(source.ready() && capture && capture->bindStage(81), "prepare segmented analysis source");
    std::array<double, 16> reference{}, copied{};
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::array<double, 16> audio{};
        double* outputs[] = {audio.data()};
        allocations = 0; counting = true;
        const auto first = source.renderTentative({}, outputs, 1, 2);
        outputs[0] += 2;
        const auto hold = source.renderNextSegment({}, outputs, 1, 8, false);
        outputs[0] += 8;
        const auto resumed = source.renderNextSegment({}, outputs, 1, 4);
        const auto pending = source.pending();
        counting = false;
        require(first && hold && resumed && pending && allocations == 0,
            "hold and resume append to one allocation-free tentative source callback");
        require(!capture->copyLatestRecord(0, copied), "segments do not publish before parent commit");
        for (std::size_t f = 0; f < 14; ++f)
        {
            const auto expected = (f < 11 ? 0.125 : 0.0625)
                * std::sin(2 * std::numbers::pi * 7 * f / 1000);
            require(std::abs(audio[f] - expected) < 1e-12,
                "segmented transaction retains sample phase and holds composition automation");
        }
        if (attempt != 0) require(audio == reference, "whole-transaction rollback restores every appended segment");
        reference = audio;
        allocations = 0; counting = true;
        if (attempt < 2) source.rollback(); else source.commit();
        counting = false;
        require(allocations == 0 && !source.pending(), "segmented completion without allocation");
    }
    const auto record = capture->copyLatestRecord(0, copied);
    require(record && record->stageId == 81 && record->absoluteSamplePosition == 0
        && record->valueCount == 14 && copied == reference,
        "one parent commit publishes all segments with their original sample timestamp");
}
void testFailedAppendRestoresEntireSource()
{
    PreparedSourceCallback source(oscillator(true, false, false, 16));
    std::array<float, 8> input{}; input.fill(1); input[4] = 10;
    const float* inputs[] = {input.data()};
    std::array<double, 8> first{}, retry{}, tail{};
    double* outputs[] = {first.data()};
    require(source.renderTentative({inputs, 1}, outputs, 1, 2), "first source segment succeeds");
    inputs[0] += 2; outputs[0] = tail.data();
    allocations = 0; counting = true;
    const auto failed = !source.renderNextSegment({inputs, 1}, outputs, 1, 3, false);
    counting = false;
    require(failed && !source.pending() && allocations == 0,
        "late append failure aborts the entire source transaction");
    input[4] = 1; inputs[0] = input.data(); outputs[0] = retry.data();
    require(source.renderTentative({inputs, 1}, outputs, 1, 2), "restart failed multi-segment source");
    require(first[0] == retry[0] && first[1] == retry[1], "append failure restores the first segment's DSP and controls");
    inputs[0] += 2; outputs[0] = tail.data();
    require(source.renderNextSegment({inputs, 1}, outputs, 1, 3, false), "retry corrected tail");
    source.commit();
    for (std::size_t f = 0; f < 3; ++f)
    {
        const auto expected = (1 + 0.125 * std::sin(2 * std::numbers::pi * 7 * (2 + f) / 1000)) * 1e308;
        require(std::abs(tail[f] / expected - 1) < 1e-12, "retried tail has exact held automation and phase");
    }
}
void testLaterSourceFailureRestoresEarlierSource()
{
    PreparedSourceCallback first(oscillator()), second(oscillator(true));
    require(first.ready() && second.ready(), "prepare both parent sources");
    std::array<double, 8> a{}, b{}, retryA{}, retryB{};
    double* ao[] = {a.data()}; double* bo[] = {b.data()};
    std::array<float, 8> samples{}; samples.fill(1); samples.back() = 10;
    const float* inputs[] = {samples.data()};
    allocations = 0; counting = true;
    const auto firstOk = first.renderTentative({}, ao, 1, 8);
    const auto secondOk = second.renderTentative({inputs, 1}, bo, 1, 8);
    second.rollback(); first.rollback();
    counting = false;
    require(firstOk && !secondOk && allocations == 0, "late parent source failure without allocation");
    samples.back() = 1;
    ao[0] = retryA.data(); bo[0] = retryB.data();
    require(first.renderTentative({}, ao, 1, 8) && second.renderTentative({inputs, 1}, bo, 1, 8), "retry all parent sources");
    first.commit(); second.commit();
    require(a == retryA, "later source failure must not consume the earlier source");
    for (std::size_t f = 0; f < 8; ++f)
    {
        const auto expected = (1 + (f < 3 ? 0.125 : 0.0625) * std::sin(2 * std::numbers::pi * 7 * f / 1000)) * 1e308;
        require(std::abs(retryB[f] / expected - 1) < 1e-12, "failed source also restarts from its old phase and controls");
    }
}
void testPendingCallbackCannotBeOverwritten()
{
    PreparedSourceCallback source(oscillator());
    std::array<double, 8> a{}, b{}; double* out[] = {a.data()};
    require(source.renderTentative({}, out, 1, 8), "first tentative callback");
    out[0] = b.data();
    require(!source.renderTentative({}, out, 1, 8) && source.pending(), "second callback must preserve outstanding rollback authority");
    source.rollback(); source.rollback(); source.commit();
    require(source.renderTentative({}, out, 1, 8), "source reusable after explicit rollback");
    require(a == b, "rejected overlap does not lose rollback state");
    source.commit();
}
void testAnalysisWaitsForParentCommit()
{
    auto graph = oscillator(false, true);
    auto capture = graph.analysisCapture();
    PreparedSourceCallback source(graph);
    require(source.ready() && capture && capture->bindStage(77), "prepare source analysis");
    std::array<double, 8> audio{}, copied{}; double* outputs[] = {audio.data()};
    require(source.renderTentative({}, outputs, 1, 8), "tentative analysis source");
    require(!capture->copyLatestRecord(0, copied), "tentative source must not publish analysis");
    source.rollback();
    require(!capture->copyLatestRecord(0, copied), "aborted source must not publish analysis");
    allocations = 0; counting = true;
    const auto rendered = source.renderTentative({}, outputs, 1, 8);
    source.commit();
    counting = false;
    require(rendered && allocations == 0, "analysis source commit without allocation");
    auto record = capture->copyLatestRecord(0, copied);
    require(record && record->stageId == 77 && record->absoluteSamplePosition == 0
        && record->valueCount == 8 && copied == audio, "parent commit publishes exact retried source evidence");
    require(source.renderTentative({}, outputs, 1, 3), "short source continuation");
    source.commit();
    record = capture->copyLatestRecord(0, copied);
    require(record && record->absoluteSamplePosition == 8 && record->valueCount == 3,
        "analysis follows committed source frame cursor");
}
PreparedRuntimeGraph filterAndFeedbackSource(std::size_t maxFrames)
{
    talas::project::ProjectDocument d;
    talas::project::ProjectParameterExpression coefficients;
    coefficients.parameterId = "coefficients"; coefficients.text = "0.5 0.25";
    d.graph.nodes = {{"device", "talas.audioInput", {}},
        {"fir", "talas.firFilter", {coefficients}},
        {"delay", "talas.delayWithFeedback", {{"delay", "2 ms"}, {"feedback", "0.25"}, {"mix", "1"}}},
        {"out", "talas.audioOutput", {}}};
    d.graph.connections = {{"device", "audioOut", "fir", "audioIn"},
        {"fir", "audioOut", "delay", "audioIn"}, {"delay", "audioOut", "out", "audioIn"}};
    RenderSettings settings{1000, maxFrames};
    auto graph = compileProject(d, talas::core::makeBuiltinNodeRegistry(), settings);
    require(graph.ok(), "compile stateful history source");
    return std::move(*graph.graph);
}
void testFilterAndFeedbackHistoryRestored()
{
    PreparedSourceCallback source(filterAndFeedbackSource(8));
    require(source.ready(), source.error());
    std::array<float, 24> input{};
    std::array<double, 24> oracle{};
    for (std::size_t f = 0; f < input.size(); ++f) input[f] = static_cast<float>((f * 7) % 11) / 8;
    for (std::size_t f = 2; f < input.size(); ++f)
        oracle[f] = 0.5 * input[f - 2] + (f > 2 ? 0.25 * input[f - 3] : 0) + 0.25 * oracle[f - 2];
    std::size_t position = 0;
    for (const std::size_t frames : {3, 8, 5, 8})
    {
        std::array<double, 8> discarded{}, retry{};
        const float* inputs[] = {input.data() + position};
        double* outputs[] = {discarded.data()};
        allocations = 0; counting = true;
        const auto first = source.renderTentative({inputs, 1}, outputs, 1, frames, position % 2 == 0);
        source.rollback();
        outputs[0] = retry.data();
        const auto second = source.renderTentative({inputs, 1}, outputs, 1, frames, position % 2 == 0);
        source.commit();
        counting = false;
        require(first && second && allocations == 0, "stateful history transaction without allocation");
        for (std::size_t f = 0; f < frames; ++f)
            require(discarded[f] == retry[f] && std::abs(retry[f] - oracle[position + f]) < 1e-12,
                "rollback restores FIR and feedback ring history across partial callbacks");
        position += frames;
    }
}
void testReusableResetRetainsSeparateDspHistory()
{
    using Parameters = std::vector<talas::project::ProjectParameterExpression>;
    talas::project::ProjectParameterExpression mode, wavetable, prezero, pole1, pole2;
    mode.parameterId = "type"; mode.choice = "compressor";
    wavetable.parameterId = "wavetable"; wavetable.resource = "Private";
    prezero.parameterId = "prezero"; prezero.boolean = true;
    pole1.parameterId = "pole1"; pole1.text = "0.5 % 0.25";
    pole2.parameterId = "pole2"; pole2.text = "0.25 % -0.5";
    const std::vector<std::pair<std::string, Parameters>> cases{
        {"talas.harmonicResonator", {{"decayTime", "0.5 s"}, {"brightness", "2"},
            {"frequency", "250 hz"}, wavetable, prezero, {"scale", "0.75"}}},
        {"talas.dynamicRangeController", {mode, {"ratio", "2"}, {"threshold", "0.05"},
            {"attackTime", "1 ms"}, {"releaseTime", "2 ms"}, {"delay", "3 ms"}, {"gain", "0.75"}}},
        {"talas.vcf", {{"amplitude", "0.75"}, {"cutoff", "200 hz"}, {"cutoffModulator", "0.25"},
            {"cutoffModRange", "10 hz"}, {"resonance", "0.25"}}},
        {"talas.presenceFilter", {{"centerFreq", "200 hz"}, {"bandwidth", "80 hz"},
            {"boostOrCut", "6"}, {"scale", "0.75"}}},
        {"talas.twoFormantElement", {{"formant1", "100 hz"}, {"bandwidth1", "50 hz"}, {"scale1", "0.75"},
            {"formant2", "200 hz"}, {"bandwidth2", "75 hz"}, {"scale2", "-0.25"}}},
        {"talas.dualParallelTwoPoleFilter", {pole1, {"scale1", "0.75"}, pole2, {"scale2", "-0.25"}}},
        {"talas.graphicEq", {{"cf250Hz", "1"}, {"cf500Hz", "1.5"}, {"cf1000Hz", "0.75"},
            {"cf2000Hz", "1"}, {"cf4000Hz", "1"}, {"cf8000Hz", "1"}, {"cf16000Hz", "1"}}},
        {"talas.twoFormantVoiceElement", {{"frequency", "100 hz"}, {"formant1", "100 hz"},
            {"bandwidth1", "50 hz"}, {"scale1", "0.75"}, {"formant2", "200 hz"},
            {"bandwidth2", "75 hz"}, {"scale2", "-0.25"}, {"seed", "17"}}},
        {"talas.analysisFilter", {{"fundamental", "100 hz"}, {"harmonic", "2"}}}
    };
    for (const auto& [type, parameters] : cases)
    {
        talas::project::ProjectDocument d;
        d.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "71 hz"}, {"amplitude", "0.75"}}},
            {"filter", type, parameters}, {"out", "talas.audioOutput", {}}};
        if (type != "talas.twoFormantVoiceElement")
            d.graph.connections.push_back({"osc", "audioOut", "filter", "audioIn"});
        if (type == "talas.dynamicRangeController")
            d.graph.connections.push_back({"osc", "audioOut", "filter", "sideChain"});
        const auto analysis = type == "talas.analysisFilter";
        d.graph.connections.push_back({"filter", analysis ? "leftOut" : "audioOut", "out", "audioIn"});
        if (analysis) d.graph.connections.push_back({"filter", "rightOut", "out", "rightIn"});
        RenderSettings settings{type == "talas.graphicEq" ? 48000.0 : 1000.0, 64};
        auto graph = compileProject(d, talas::core::makeBuiltinNodeRegistry(), settings);
        require(graph.ok(), type + " reset fixture compiles");
        PreparedSourceCallback source(*graph.graph), uninterrupted(*graph.graph);
        require(source.ready() && uninterrupted.ready(), type + " source prepares");
        const auto channels = analysis ? 2u : 1u;
        std::array<double, 64> left{}, right{}, referenceLeft{}, referenceRight{};
        double* outputs[] = {left.data(), right.data()};
        double* referenceOutputs[] = {referenceLeft.data(), referenceRight.data()};
        require(source.renderTentative({}, outputs, channels, 37)
            && uninterrupted.renderTentative({}, referenceOutputs, channels, 37), type + " preroll");
        source.commit(); uninterrupted.commit();
        require(source.prepareResetPoint(), type + " captures settled history");
        require(uninterrupted.renderTentative({}, referenceOutputs, channels, 64), type + " uninterrupted reference");
        uninterrupted.commit();
        require(std::ranges::any_of(referenceLeft, [](double value) { return value != 0; }), type + " reference is audible");
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            allocations = 0; counting = true;
            const auto reset = attempt == 2 || source.resetToPreparedPoint();
            const auto first = source.renderTentative({}, outputs, channels, 19);
            if (attempt == 0) source.commit();
            double* tail[] = {left.data() + 19, right.data() + 19};
            const auto second = attempt == 0 ? source.renderTentative({}, tail, channels, 45)
                : source.renderNextSegment({}, tail, channels, 45);
            if (attempt == 1) source.rollback(); else source.commit();
            counting = false;
            require(reset && first && second && allocations == 0, type + " repeated reset renders without allocation");
            require(left == referenceLeft && (!analysis || right == referenceRight),
                type + " reset preserves preexisting DSP history across callback partitions");
        }
    }
}
PreparedRuntimeGraph midiFileSource(std::size_t maxFrames = 64)
{
    talas::project::ProjectDocument d;
    talas::project::ProjectParameterExpression sourceType, resource, script, shared;
    sourceType.parameterId = "source"; sourceType.text = "MIDI File";
    resource.parameterId = "midiFile"; resource.resource = "notes";
    script.parameterId = "script"; script.text = "";
    shared.parameterId = "shared"; shared.boolean = false;
    d.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "!Pitch"}, {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
        {"voice", "talas.midiVoice", {sourceType, resource, script, shared,
            {"channel", "1"}, {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
            {"polyphony", "1"}, {"left", "1"}, {"right", "0.5"}}},
        {"out", "talas.audioOutput", {}}};
    d.graph.connections = {{"osc", "audioOut", "voice", "audioIn"},
        {"voice", "leftOut", "out", "audioIn"}, {"voice", "rightOut", "out", "rightIn"}};
    RenderSettings settings{1000, maxFrames};
    settings.eventValues = {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0}, {"KeyVelocity", 0}};
    settings.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
        'M','T','r','k',0,0,0,20,
        0,0x90,60,100, 20,0x80,60,0, 10,0x90,64,80, 20,0x90,64,0, 0,0xff,0x2f,0}}};
    auto graph = compileProject(d, talas::core::makeBuiltinNodeRegistry(), settings);
    require(graph.ok(), graph.diagnostics.empty() ? "compile MIDI file source transaction" : graph.diagnostics.front().message);
    return std::move(*graph.graph);
}
void testPreparedMidiTransportPoints()
{
    PreparedSourceCallback source(midiFileSource());
    std::array<double, 64> expected{}; double phase = 0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto pitch = frame >= 35 ? 64 : 60;
        const auto gain = frame >= 5 && frame < 20 ? 100.0 / 127.0 : frame >= 35 && frame < 50 ? 80.0 / 127.0 : 0;
        expected[frame] = gain * std::sin(2 * std::numbers::pi * phase);
        phase += 440 * std::pow(2.0, (pitch - 69) / 12.0) / 1000; phase -= std::floor(phase);
    }
    std::array<std::unique_ptr<PreparedSourceCallback::PreparedPoint>, 3> points;
    const std::array<std::size_t, 3> positions{3, 17, 33};
    std::array<double, 64> left{}, right{}; double* outputs[] = {left.data(), right.data()};
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < positions.size(); ++index)
    {
        require(source.renderTentative({}, outputs, 2, positions[index] - cursor), "advance MIDI source to prepared point");
        source.commit(); cursor = positions[index];
        points[index] = source.capturePreparedPoint(); require(bool(points[index]), "capture MIDI transport point");
    }
    for (int attempt = 0; attempt < 3; ++attempt)
        for (std::size_t index = 0; index < positions.size(); ++index)
        {
            allocations = 0; counting = true;
            const auto restored = source.restorePreparedPoint(*points[index]);
            const auto rendered = source.renderTentative({}, outputs, 2, 24);
            source.rollback();
            const auto retried = source.renderTentative({}, outputs, 2, 24); source.commit();
            counting = false;
            require(restored && rendered && retried && allocations == 0, "MIDI point restoration and rollback are allocation-free");
            for (std::size_t f = 0; f < 24; ++f)
                require(std::abs(left[f] - expected[positions[index] + f]) < 1e-11
                    && std::abs(right[f] - 0.5 * expected[positions[index] + f]) < 1e-11,
                    "prepared MIDI points restore active voices, future file events, attack deadlines and phase");
        }
}
void testMidiFileReaderAndVoiceRollback()
{
    PreparedSourceCallback source(midiFileSource());
    require(source.ready(), source.error());
    std::array<double, 64> expected{};
    double phase = 0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto pitch = frame >= 35 ? 64 : 60;
        const auto gain = frame >= 5 && frame < 20 ? 100.0 / 127.0
            : frame >= 35 && frame < 50 ? 80.0 / 127.0 : 0;
        expected[frame] = gain * std::sin(2 * std::numbers::pi * phase);
        phase += 440 * std::pow(2.0, (pitch - 69) / 12.0) / 1000;
        phase -= std::floor(phase);
    }
    std::size_t position = 0;
    for (const std::size_t frames : {17, 24, 23})
    {
        std::array<double, 64> left{}, right{}, retryLeft{}, retryRight{};
        double* outputs[] = {left.data(), right.data()};
        allocations = 0; counting = true;
        const auto first = source.renderTentative({}, outputs, 2, frames);
        source.rollback();
        outputs[0] = retryLeft.data(); outputs[1] = retryRight.data();
        const auto second = source.renderTentative({}, outputs, 2, frames);
        source.commit();
        counting = false;
        require(first && second && allocations == 0, "MIDI reader rollback and commit without allocation");
        for (std::size_t f = 0; f < frames; ++f)
            require(left[f] == retryLeft[f] && right[f] == retryRight[f]
                && std::abs(retryLeft[f] - expected[position + f]) < 1e-11
                && std::abs(retryRight[f] - expected[position + f] * 0.5) < 1e-11,
                "MIDI reader, pending attack, voice and oscillator restore together");
        position += frames;
        if (position == 17) require(source.prepareResetPoint(), "capture an active MIDI voice reset boundary");
    }
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::array<double, 24> left{}, right{};
        double* outputs[] = {left.data(), right.data()};
        allocations = 0; counting = true;
        const auto reset = source.resetToPreparedPoint();
        const auto rendered = source.renderTentative({}, outputs, 2, left.size());
        source.commit();
        counting = false;
        require(reset && rendered && allocations == 0, "restore active MIDI voice and render without allocation");
        for (std::size_t f = 0; f < left.size(); ++f)
            require(std::abs(left[f] - expected[17 + f]) < 1e-11
                && std::abs(right[f] - expected[17 + f] * 0.5) < 1e-11,
                "reusable MIDI reset repeats note release, delayed next attack and oscillator phase exactly");
    }
}
void testCompositionSuspensionSeparatesMidiReaderAndAttackClocks()
{
    const auto graph = midiFileSource();
    for (const std::size_t initialAdvance : {0, 2})
    for (const std::size_t partition : {1, 7, 64})
    {
        PreparedSourceCallback source(graph);
        require(source.ready(), source.error());
        std::size_t sample = 0;
        double phase = 0;
        for (const auto [frames, advancing] : std::array<std::pair<std::size_t, bool>, 5>{
            {{initialAdvance, true}, {24, false}, {31 - initialAdvance, true}, {8, false}, {32, true}}})
        {
            for (std::size_t rendered = 0; rendered < frames;)
            {
                const auto count = std::min(partition, frames - rendered);
                std::array<double, 64> left{}, right{}, retryLeft{}, retryRight{};
                double* outputs[] = {left.data(), right.data()};
                allocations = 0; counting = true;
                const auto first = source.renderTentative({}, outputs, 2, count, advancing);
                source.rollback();
                outputs[0] = retryLeft.data(); outputs[1] = retryRight.data();
                const auto second = source.renderTentative({}, outputs, 2, count, advancing);
                source.commit();
                counting = false;
                require(first && second && allocations == 0, "suspended MIDI source is partition independent and transactional");
                for (std::size_t f = 0; f < count; ++f, ++sample)
                {
                    // File events at composition 0/20/30/50. The cursor holds at
                    // 0 or 2 for 24 samples, then at 31 for 8 samples. Both five-sample
                    // attacks occur during a hold; neither note-off is consumed early.
                    const auto pitch = sample >= 59 ? 64 : 60;
                    const auto gain = sample >= 5 && sample < 44 ? 100.0 / 127.0
                        : sample >= 59 && sample < 82 ? 80.0 / 127.0 : 0;
                    const auto expected = gain * std::sin(2 * std::numbers::pi * phase);
                    require(left[f] == retryLeft[f] && right[f] == retryRight[f]
                        && std::abs(retryLeft[f] - expected) < 1e-11
                        && std::abs(retryRight[f] - expected * 0.5) < 1e-11,
                        "MIDI file note-offs wait while delayed attacks and oscillator phase continue");
                    phase += 440 * std::pow(2.0, (pitch - 69) / 12.0) / 1000;
                    phase -= std::floor(phase);
                }
                rendered += count;
            }
        }
    }
}

void testSegmentedMidiTransaction()
{
    PreparedSourceCallback source(midiFileSource(128));
    require(source.ready(), source.error());
    std::array<double, 128> referenceLeft{}, referenceRight{};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::array<double, 128> left{}, right{};
        std::size_t sample = 0;
        bool ok = true;
        allocations = 0; counting = true;
        for (const auto [frames, advancing] : std::array<std::pair<std::size_t, bool>, 5>{
            {{2, true}, {24, false}, {29, true}, {8, false}, {32, true}}})
        {
            double* outputs[] = {left.data() + sample, right.data() + sample};
            ok = ok && (sample == 0 ? source.renderTentative({}, outputs, 2, frames, advancing)
                : source.renderNextSegment({}, outputs, 2, frames, advancing));
            sample += frames;
        }
        if (attempt == 0) source.rollback(); else source.commit();
        counting = false;
        require(ok && !source.pending() && allocations == 0, "MIDI segments share one allocation-free transaction");
        double phase = 0;
        for (std::size_t f = 0; f < sample; ++f)
        {
            const auto pitch = f >= 59 ? 64 : 60;
            const auto gain = f >= 5 && f < 44 ? 100.0 / 127.0
                : f >= 59 && f < 82 ? 80.0 / 127.0 : 0;
            const auto expected = gain * std::sin(2 * std::numbers::pi * phase);
            require(std::abs(left[f] - expected) < 1e-11 && std::abs(right[f] - expected * 0.5) < 1e-11,
                "MIDI release and delayed attacks keep separate clocks across appended segments");
            phase += 440 * std::pow(2.0, (pitch - 69) / 12.0) / 1000;
            phase -= std::floor(phase);
        }
        if (attempt == 0) { referenceLeft = left; referenceRight = right; }
        else require(left == referenceLeft && right == referenceRight, "whole MIDI transaction repeats after rollback");
    }
}
void testSegmentedCapacityFailureRetiresTransaction()
{
    PreparedSourceCallback source(oscillator(false, false, false, 16));
    std::array<double, 16> first{}, tail{}, retry{};
    double* outputs[] = {first.data()};
    for (const std::size_t invalidFrames : {0, 9})
    {
        require(source.renderTentative({}, outputs, 1, 8), "open bounded source transaction");
        outputs[0] = tail.data(); tail.fill(19);
        allocations = 0; counting = true;
        const auto rejected = !source.renderNextSegment({}, outputs, 1, invalidFrames);
        counting = false;
        require(rejected && !source.pending() && allocations == 0, "invalid append aborts without advancing either clock");
        if (invalidFrames) require(std::all_of(tail.begin(), tail.begin() + invalidFrames,
            [](double value) { return value == 0; }), "failed append clears its destination");
        outputs[0] = retry.data();
        require(source.renderTentative({}, outputs, 1, 8), "restart after invalid append");
        require(std::equal(first.begin(), first.begin() + 8, retry.begin()), "capacity rejection restores initial source samples");
        source.rollback(); outputs[0] = first.data();
    }
}

void testSegmentedLiveInputHistoryRollback()
{
    PreparedSourceCallback source(filterAndFeedbackSource(32));
    require(source.ready(), source.error());
    std::array<float, 24> input{};
    std::array<double, 24> expected{};
    for (std::size_t f = 0; f < input.size(); ++f) input[f] = static_cast<float>((f * 7) % 11) / 8;
    for (std::size_t f = 2; f < input.size(); ++f)
        expected[f] = 0.5 * input[f - 2] + (f > 2 ? 0.25 * input[f - 3] : 0) + 0.25 * expected[f - 2];
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::array<double, 24> audio{};
        std::size_t position = 0;
        bool ok = true;
        allocations = 0; counting = true;
        for (const std::size_t frames : {3, 8, 5, 8})
        {
            const float* inputs[] = {input.data() + position};
            double* outputs[] = {audio.data() + position};
            ok = ok && (position == 0 ? source.renderTentative({inputs, 1}, outputs, 1, frames)
                : source.renderNextSegment({inputs, 1}, outputs, 1, frames, position % 2 == 0));
            position += frames;
        }
        if (attempt < 2) source.rollback(); else source.commit();
        counting = false;
        require(ok && allocations == 0, "live-input segments share fixed FIR/feedback rollback storage");
        for (std::size_t f = 0; f < audio.size(); ++f)
            require(std::abs(audio[f] - expected[f]) < 1e-12,
                "appended live input retains exact FIR and feedback history through whole-call rollback");
    }
}

}
int main()
{
    bool failed = false;
    for (const auto& [name, test] : std::array{
        std::pair{"tentative-rollback-commit", &testTentativeRollbackAndCommit},
        std::pair{"composition-suspension", &testCompositionSuspensionRetainsAutomationAndAdvancesDsp},
        std::pair{"composition-reset-offline", &testCompositionClockResetAndOfflineRollback},
        std::pair{"multiple-transport-points", &testMultiplePreparedTransportPoints},
        std::pair{"retired-transport-owner", &testPreparedPointOutlivesOwner},
        std::pair{"prepared-midi-transport-points", &testPreparedMidiTransportPoints},
        std::pair{"composition-midi-clocks", &testCompositionSuspensionSeparatesMidiReaderAndAttackClocks},
        std::pair{"segmented-clocks-analysis", &testSegmentedTransactionRetainsClocksAndAnalysis},
        std::pair{"segmented-live-history", &testSegmentedLiveInputHistoryRollback},
        std::pair{"segmented-midi", &testSegmentedMidiTransaction},
        std::pair{"segmented-capacity", &testSegmentedCapacityFailureRetiresTransaction},
        std::pair{"segmented-failure", &testFailedAppendRestoresEntireSource},
        std::pair{"later-source-failure", &testLaterSourceFailureRestoresEarlierSource},
        std::pair{"pending-callback-guard", &testPendingCallbackCannotBeOverwritten},
        std::pair{"analysis-parent-commit", &testAnalysisWaitsForParentCommit},
        std::pair{"filter-feedback-history", &testFilterAndFeedbackHistoryRestored},
        std::pair{"reusable-dsp-reset", &testReusableResetRetainsSeparateDspHistory},
        std::pair{"midi-file-reader-voice", &testMidiFileReaderAndVoiceRollback}})
    {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { failed = true; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
