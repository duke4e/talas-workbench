#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>

namespace
{
std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};
} // namespace
void *operator new(std::size_t size)
{
    if (counting)
        ++allocations;
    if (auto *p = std::malloc(size))
        return p;
    throw std::bad_alloc{};
}
void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void *p) noexcept
{
    std::free(p);
}
void operator delete[](void *p) noexcept
{
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}
namespace
{
using namespace talas::runtime;
using P = talas::project::ProjectParameterExpression;
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
P choice(std::string id, std::string value)
{
    P p;
    p.parameterId = id;
    p.choice = value;
    return p;
}
P boolean(std::string id, bool value)
{
    P p;
    p.parameterId = id;
    p.boolean = value;
    return p;
}
P text(std::string id, std::string value)
{
    P p;
    p.parameterId = id;
    p.text = value;
    return p;
}
constexpr std::size_t maximumFrames = 65;
talas::project::ProjectDocument document(int family, bool input, bool firBefore, bool failure)
{
    talas::project::ProjectDocument d;
    d.graph.nodes = {
        {"source", input ? "talas.audioInput" : "talas.oscillator",
         input ? std::vector<P>{} : std::vector<P>{{"frequency", "8 hz"}, {"amplitude", "0.75"}}},
        {"fir", "talas.firFilter", {text("coefficients", "0.5 0.25 -0.125")}},
        {"out", "talas.audioOutput", {}}};
    std::string last, port = "audioOut";
    if (family == 0)
    {
        std::vector<P> p = {{"frameSize", "16"}, {"hopSize", "4"}, choice("window", "hann")};
        d.graph.nodes.push_back({"forward", "talas.forwardFftV1", p});
        d.graph.nodes.push_back({"inverse", "talas.inverseFftV1", p});
        d.graph.connections.push_back({"forward", "spectrumOut", "inverse", "spectrumIn"});
        last = "inverse";
    }
    else if (family == 1 || family == 3)
    {
        // The stereo FFT carries its spectrum on both channels; family 3
        // keeps the packed layout of projects before format 62.
        const auto packing = choice("packing", family == 1 ? "talasStereo" : "legacyPacked");
        d.graph.nodes.push_back(
            {"forward", "talas.fft", {{"length", "16"}, boolean("inverse", false), packing}});
        d.graph.nodes.push_back(
            {"inverse", "talas.fft", {{"length", "16"}, boolean("inverse", true), packing}});
        d.graph.connections.push_back({"forward", "audioOut", "inverse", "audioIn"});
        if (family == 1)
            d.graph.connections.push_back({"forward", "rightOut", "inverse", "rightIn"});
        last = "inverse";
    }
    else
    {
        d.graph.nodes.push_back({"forward",
                                 "talas.liveSpectralAnalysis",
                                 {{"lowestAnalyzedFreq", "1 hz"},
                                  {"ampScale", "1"},
                                  {"freqScale", "1"},
                                  text("response", ""),
                                  boolean("harmonic", false),
                                  {"unpitchedOnly", "0"},
                                  {"unpitchedThreshold", "0"},
                                  {"trackedHarmonic", "1"},
                                  {"lowFreq", "1 hz"},
                                  {"highFreq", "32 hz"},
                                  {"initFreq", "8 hz"},
                                  boolean("fundamentalOnly", true),
                                  choice("tracks", "legacyPeaks")}});
        d.graph.nodes.push_back({"bridge", "talas.partialSpectrumLegacyHoldV1", {}});
        d.graph.nodes.push_back({"selector",
                                 "talas.spectrumTrackSelector",
                                 {{"track", "1"}, boolean("amp", true), boolean("freq", true)}});
        d.graph.connections.push_back({"forward", "spectrumOut", "bridge", "spectrumIn"});
        d.graph.connections.push_back({"bridge", "spectrumOut", "selector", "spectrumIn"});
        last = "selector";
        port = "leftOut";
    }
    if (firBefore)
    {
        d.graph.connections.push_back({"source", "audioOut", "fir", "audioIn"});
        d.graph.connections.push_back({"fir", "audioOut", "forward", "audioIn"});
    }
    else
        d.graph.connections.push_back({"source", "audioOut", "forward", "audioIn"});
    if (!firBefore)
    {
        d.graph.connections.push_back({last, port, "fir", "audioIn"});
        last = "fir";
        port = "audioOut";
    }
    if (failure)
    {
        d.graph.nodes.push_back(
            {"control", "talas.constant", {{"value", "!SpectralControl + !UnrelatedControl"}}});
        if (family == 2)
            d.graph.connections.push_back({"control", "value", "forward", "freqScale"});
        else
        {
            d.graph.nodes.push_back({"formants",
                                     "talas.twoFormantElement",
                                     {{"formant1", "8 hz"},
                                      {"bandwidth1", "2 hz"},
                                      {"scale1", "0.75"},
                                      {"formant2", "12 hz"},
                                      {"bandwidth2", "3 hz"},
                                      {"scale2", "-0.25"}}});
            d.graph.connections.push_back({"control", "value", "formants", "formant1"});
            d.graph.connections.push_back({last, port, "formants", "audioIn"});
            last = "formants";
            port = "audioOut";
        }
    }
    d.graph.connections.push_back({last, port, "out", "audioIn"});
    return d;
}
bool render(RealtimeRenderSession &session, std::span<double> output, const float *input,
            std::span<double> right = {})
{
    const std::array<const float *, 1> channels{input};
    allocations = 0;
    counting = true;
    const auto ok =
        input ? session.render({channels.data(), 1}, output, right) : session.render(output, right);
    counting = false;
    require(allocations == 0, "spectral callback success/failure/restore must not allocate");
    return ok;
}
bool scheduleControl(RealtimeRenderSession &session, double value, std::size_t offset, bool typed,
                     std::uint64_t &sequence)
{
    if (!typed)
        return session.scheduleEventValue("SpectralControl", value, talas::expr::ValueDomain::Scalar,
                                          offset);
    const auto index =
        session.eventValueTargetIndex("SpectralControl", talas::expr::ValueDomain::Scalar);
    if (!index)
        return false;
    TypedEventPayload payload{};
    payload.eventValue = {value, talas::expr::ValueDomain::Scalar};
    const std::array events{PreparedTypedEvent{TypedEventKind::EventValueAssignment,
                                               {TypedEventTargetKind::EventValue, *index, 1, 1},
                                               offset,
                                               sequence++,
                                               payload}};
    std::array<RealtimeTypedEventDispatchResult, 1> results{};
    return session.scheduleTypedEvents(events, results) &&
           results[0] == RealtimeTypedEventDispatchResult::Handled;
}
void testComposition(int family, bool input, bool firBefore, bool failure)
{
    RenderSettings settings{64.0, maximumFrames};
    const double initialControl = family == 2 ? 1.0 : 8.0;
    double validControl = initialControl;
    settings.eventValues = {{"SpectralControl", validControl, talas::expr::ValueDomain::Scalar},
                            {"UnrelatedControl", 0.0, talas::expr::ValueDomain::Scalar}};
    std::vector<float> samples(8192);
    for (std::size_t n = 0; n < samples.size(); ++n)
        samples[n] = static_cast<float>(
            0.75 *
            (family == 2 ? 1.0 + 0.25 * std::sin(2 * std::numbers::pi_v<double> * n / 511) : 1.0) *
            std::sin(2 * std::numbers::pi_v<double> * 8 * n / 64));
    if (input)
        settings.audioInputChannels.push_back({1, {samples.begin(), samples.end()}});
    const auto compiled = compileProject(document(family, input, firBefore, failure),
                                         talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "spectral checked composition must compile");
    RealtimeRenderSession candidate(*compiled.graph), reference(*compiled.graph);
    if (!candidate.ready())
        throw std::runtime_error(std::string(candidate.error()));
    require(reference.ready(), "uninterrupted spectral reference must prepare");
    auto nativeSettings = settings;
    if (failure)
        nativeSettings.eventValueChanges = {
            {290, "UnrelatedControl", 0.25, talas::expr::ValueDomain::Scalar},
            {371, "SpectralControl", family == 2 ? 1.5 : 10.0, talas::expr::ValueDomain::Scalar}};
    const auto nativeCompiled = compileProject(document(family, input, firBefore, failure),
                                               talas::core::makeBuiltinNodeRegistry(), nativeSettings);
    require(nativeCompiled.ok(), "native schedule counterpart must compile");
    OfflineRenderSession offline(*nativeCompiled.graph);
    std::array<double, maximumFrames> actual{}, expected{};
    std::size_t position = 0;
    std::uint64_t candidateSequence = 1, referenceSequence = 1;
    bool audible = false;
    const auto compareBlock = [&](std::size_t frames)
    {
        const auto in = input ? samples.data() + position : nullptr;
        const auto candidateOk = render(candidate, std::span(actual).first(frames), in);
        const auto referenceOk = render(reference, std::span(expected).first(frames), in);
        if (!candidateOk || !referenceOk)
            std::cerr << "render failure family=" << family << " input=" << input
                      << " frames=" << frames << " position=" << position
                      << " candidate=" << candidateOk << " reference=" << referenceOk << std::endl;
        require(candidateOk && referenceOk, "spectral checkpoint must continue after failure");
        const auto native = offline.render(frames);
        require(native.ok(), "native spectral checked composition must render");
        for (std::size_t n = 0; n < frames; ++n)
        {
            require(actual[n] == expected[n],
                    "spectral processor, FIR and publication history must recover exactly");
            if (std::abs(actual[n] - native.mono[n]) >= 1e-8)
                std::cerr << "native mismatch family=" << family << " input=" << input
                          << " before=" << firBefore << " failure=" << failure
                          << " position=" << position << " n=" << n << " actual=" << actual[n]
                          << " native=" << native.mono[n] << std::endl;
            require(std::abs(actual[n] - native.mono[n]) < 1e-8,
                    "realtime spectral composition must agree with native offline");
            audible |= std::abs(actual[n]) > 1e-6;
            if (!failure && family != 2)
            {
                const auto delay = family == 0 ? 15 : 30;
                const auto source = [&](std::int64_t frame)
                {
                    return frame < 0 ? 0.0
                           : input
                               ? static_cast<double>(samples[frame])
                               : 0.75 * std::sin(2 * std::numbers::pi_v<double> * 8 * frame / 64);
                };
                const auto t = static_cast<std::int64_t>(position + n) - delay;
                require(std::abs(actual[n] - (.5 * source(t) + .25 * source(t - 1) -
                                              .125 * source(t - 2))) < 1e-9,
                        "spectral/FIR round trip must match independent delayed-input recurrence");
            }
        }
        if (family == 2)
        {
            const auto observed = candidate.inspectPartialSpectrum("forward", "spectrumOut");
            const auto uninterrupted = reference.inspectPartialSpectrum("forward", "spectrumOut");
            require(observed.size() == uninterrupted.size(),
                    "repaired partial publication count must agree");
            for (std::size_t i = 0; i < observed.size(); ++i)
            {
                const auto &left = observed[i];
                const auto &right = uninterrupted[i];
                require(left.formatVersion == right.formatVersion &&
                            left.sourceTimestamp == right.sourceTimestamp &&
                            left.sourceSequence == right.sourceSequence &&
                            left.partials.size() == right.partials.size(),
                        "repaired partial publication timestamps and sequences must agree");
                for (std::size_t j = 0; j < left.partials.size(); ++j)
                {
                    const auto &x = left.partials[j];
                    const auto &y = right.partials[j];
                    require(x.hasTrackId == y.hasTrackId && x.trackId == y.trackId &&
                                x.amplitude == y.amplitude && x.frequencyHz == y.frequencyHz &&
                                x.bandwidthHz == y.bandwidthHz,
                            "repaired partial payloads must agree exactly");
                }
            }
        }
        position += frames;
    };
    for (int block = 0; block < 32; ++block)
    {
        auto frames = block % 3 == 0   ? maximumFrames
                      : block % 3 == 1 ? std::size_t{17}
                                       : std::size_t{31};
        const auto in = input ? samples.data() + position : nullptr;
        if (failure && block == 9)
        {
            require(position == 339,
                    "newer control fixture must retain its absolute schedule boundary");
            validControl = family == 2 ? 1.5 : 10.0;
            require(scheduleControl(candidate, validControl, 32, !input, candidateSequence) &&
                        scheduleControl(reference, validControl, 32, !input, referenceSequence),
                    "newer control publication must remain effective after recovery");
        }
        if (failure && block >= 6 && block % 6 == 0)
        {
            if (block == 6)
            {
                require(candidate.scheduleEventValue("UnrelatedControl", 0.25,
                                                     talas::expr::ValueDomain::Scalar, 64) &&
                            reference.scheduleEventValue("UnrelatedControl", 0.25,
                                                         talas::expr::ValueDomain::Scalar, 64),
                        "recovery must preserve another binding's previously queued change");
            }
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                require(scheduleControl(candidate, family == 2 ? -1000000.0 : 32.0, 64, input,
                                        candidateSequence),
                        "late invalid spectral control must schedule");
                actual.fill(42);
                require(!render(candidate, std::span(actual).first(frames), in) &&
                            std::ranges::all_of(std::span(actual).first(frames),
                                                [](double x) { return x == 0; }),
                        "late spectral failure must silence the complete callback");
                if (family == 2)
                    require(candidate.inspectPartialSpectrum("forward", "spectrumOut").empty(),
                            "failed spectral callback must publish no inspection frames");
                require(candidate.updateEventValue("SpectralControl", validControl,
                                                   talas::expr::ValueDomain::Scalar),
                        "spectral control must recover");
            }
            compareBlock(1);
            if (family == 2)
                require(candidate.inspectPartialSpectrum("forward", "spectrumOut").empty(),
                        "hold recovery oracle must precede the next partial frame");
            --frames;
        }
        compareBlock(frames);
    }
    require(audible, "spectral history oracle must contain audible output");
    if (failure)
        require(candidate.updateEventValue("SpectralControl", initialControl,
                                           talas::expr::ValueDomain::Scalar),
                "reset fixture must restore its selected control value");
    if (failure)
        require(candidate.updateEventValue("UnrelatedControl", 0.0, talas::expr::ValueDomain::Scalar),
                "reset fixture must restore its other selected control");
    candidate.resetTransport();
    RealtimeRenderSession fresh(*compiled.graph);
    for (int block = 0; block < 8; ++block)
    {
        const auto in = input ? samples.data() + block * maximumFrames : nullptr;
        require(render(candidate, actual, in) && render(fresh, expected, in) && actual == expected,
                "reset must clear spectral and adjacent filter histories");
    }
    std::cout << "family=" << family << " input=" << input << " firBefore=" << firBefore
              << " failure=" << failure << " passed\n";
}
void testControlBoundaries(bool linear)
{
    RenderSettings nativeSettings{64.0, maximumFrames};
    nativeSettings.eventValues = {{"SpectralControl", 1.0, talas::expr::ValueDomain::Scalar},
                                  {"UnrelatedControl", 0.0, talas::expr::ValueDomain::Scalar}};
    nativeSettings.eventValueChanges =
        linear ? std::vector<EventValueChange>{{16, "SpectralControl", 1.0,
                                                talas::expr::ValueDomain::Scalar,
                                                talas::core::AutomationInterpolation::Linear},
                                               {32, "SpectralControl", 2.0,
                                                talas::expr::ValueDomain::Scalar},
                                               {96, "SpectralControl", 0.75,
                                                talas::expr::ValueDomain::Scalar}}
               : std::vector<EventValueChange>{
                     {20, "SpectralControl", 1.5, talas::expr::ValueDomain::Scalar},
                     {34, "SpectralControl", 0.5, talas::expr::ValueDomain::Scalar},
                     {92, "SpectralControl", 1.25, talas::expr::ValueDomain::Scalar}};
    auto realtimeSettings = nativeSettings;
    realtimeSettings.eventValueChanges.clear();
    auto graph = document(2, false, true, true);
    graph.graph.connections.push_back({"selector", "rightOut", "out", "rightIn"});
    const auto nativeGraph =
        compileProject(graph, talas::core::makeBuiltinNodeRegistry(), nativeSettings);
    const auto realtimeGraph =
        compileProject(graph, talas::core::makeBuiltinNodeRegistry(), realtimeSettings);
    require(nativeGraph.ok() && realtimeGraph.ok(), "control-boundary graphs must compile");
    OfflineRenderSession native(*nativeGraph.graph);
    RealtimeRenderSession realtime(*realtimeGraph.graph);
    require(realtime.ready(), "control-boundary realtime graph must prepare");
    std::array<double, maximumFrames> output{}, frequency{};
    std::size_t position = 0;
    for (std::size_t count : {65, 17, 31, 65, 65, 17})
    {
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto frame = position + offset;
            for (const auto &event : nativeSettings.eventValueChanges)
            {
                if (event.frame == frame && !(linear && frame == 16))
                    require(realtime.scheduleEventValue("SpectralControl", event.value,
                                                        talas::expr::ValueDomain::Scalar, offset),
                            "step event must schedule");
            }
            if (linear && frame >= 16 && frame < 32)
                require(realtime.scheduleEventValue("SpectralControl", 1.0 + (frame - 16) / 16.0,
                                                    talas::expr::ValueDomain::Scalar, offset),
                        "linear ramp sample must schedule");
        }
        const auto expected = native.render(count);
        require(expected.ok() && render(realtime, std::span(output).first(count), nullptr,
                                        std::span(frequency).first(count)),
                "event boundary and linear ramp render must succeed");
        for (std::size_t n = 0; n < count; ++n)
            require(std::abs(output[n] - expected.mono[n]) < 1e-9 &&
                        std::abs(frequency[n] - expected.right[n]) < 1e-9,
                    "native process intervals must match real step boundaries and linear sample "
                    "clocks");
        for (const auto &partialFrame : realtime.inspectPartialSpectrum("forward", "spectrumOut"))
        {
            const auto start = partialFrame.sourceTimestamp.absoluteFrame;
            double scale = 1.0;
            if (linear)
                scale = start < 16   ? 1.0
                        : start < 32 ? 1.0 + (start - 16) / 16.0
                        : start < 96 ? 2.0
                                     : 0.75;
            else
                scale = start < 20 ? 1.0 : start < 34 ? 1.5 : start < 92 ? 0.5 : 1.25;
            require(!partialFrame.partials.empty() &&
                        std::abs(partialFrame.partials.front().frequencyHz - 8.0 * scale) < 1e-5,
                    "spectral frequencies must follow independently computed frame-start controls");
        }
        position += count;
    }
}
} // namespace
int main()
{
    testControlBoundaries(false);
    testControlBoundaries(true);
    for (int family = 0; family < 4; ++family)
        for (bool input : {false, true})
            for (bool before : {false, true})
            {
                // Partial-spectrum selection has no exact causal output latency. Its FIR
                // predecessor is valid; retain the separate downstream FIR admission guard.
                if (family == 2 && !before)
                    continue;
                if (family != 2)
                    testComposition(family, input, before, false);
                testComposition(family, input, before, true);
            }
}
