#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef TALAS_BENCHMARK_COMMIT
#define TALAS_BENCHMARK_COMMIT "unknown"
#endif
#ifndef TALAS_BENCHMARK_COMPILER_ID
#define TALAS_BENCHMARK_COMPILER_ID "unknown"
#endif
#ifndef TALAS_BENCHMARK_PRESET_ID
#define TALAS_BENCHMARK_PRESET_ID "unknown"
#endif
#ifndef TALAS_BENCHMARK_FINGERPRINT_FILE
#define TALAS_BENCHMARK_FINGERPRINT_FILE ""
#endif
#ifndef TALAS_BENCHMARK_COMPILER_VERSION
#define TALAS_BENCHMARK_COMPILER_VERSION "unknown"
#endif
#ifndef TALAS_BENCHMARK_BUILD_TYPE
#define TALAS_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace
{
using Clock = std::chrono::steady_clock;
constexpr double sampleRate = 48000.0;
constexpr std::size_t blockFrames = 256;
constexpr std::uint64_t callbackDeadlineNs = 5'333'334;
constexpr std::uint64_t defaultSeed = 0x473550455246434fULL;

struct Options
{
    std::size_t warmups = 8;
    std::size_t samples = 1024;
    std::size_t callbackSamples = 2048;
    std::uint64_t seed = defaultSeed;
    std::optional<std::string> output;
};

struct Distribution
{
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t maximum = 0;
};

struct ScenarioResult
{
    std::string id;
    std::size_t nodeCount = 0;
    std::size_t connectionCount = 0;
    std::size_t stepCount = 0;
    std::size_t bufferSlots = 0;
    std::uint64_t estimatedPreparedBytes = 0;
    Distribution compile;
    Distribution prepare;
    Distribution callback;
    Distribution adoption;
    std::uint64_t callbackDeadlineMisses = 0;
    double checksum = 0.0;
};

[[nodiscard]] std::string makeRunId()
{
    std::random_device source;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < 4; ++index)
        output << std::setw(8) << static_cast<std::uint32_t>(source());
    return output.str();
}

[[nodiscard]] std::string readBuildFingerprint()
{
    std::ifstream input(TALAS_BENCHMARK_FINGERPRINT_FILE, std::ios::binary);
    std::string fingerprint;
    input >> fingerprint;
    char trailing = 0;
    if (!input.good() && !input.eof())
        throw std::runtime_error("cannot read benchmark build fingerprint");
    if (fingerprint.size() != 64
        || !std::all_of(
            fingerprint.begin(), fingerprint.end(), [](char value) {
                return (value >= '0' && value <= '9')
                    || (value >= 'a' && value <= 'f');
            })
        || (input >> trailing))
    {
        throw std::runtime_error("invalid benchmark build fingerprint");
    }
    return fingerprint;
}

[[nodiscard]] std::uint64_t elapsedNs(Clock::time_point start) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count());
}

[[nodiscard]] std::size_t parseSize(
    std::string_view argument,
    std::string_view prefix,
    std::size_t minimum,
    std::size_t maximum)
{
    if (!argument.starts_with(prefix))
        throw std::runtime_error("internal option parser mismatch");
    std::uint64_t value = 0;
    const auto source = argument.substr(prefix.size());
    const auto parsed = std::from_chars(
        source.data(), source.data() + source.size(), value);
    if (parsed.ec != std::errc{}
        || parsed.ptr != source.data() + source.size()
        || value < minimum || value > maximum)
    {
        throw std::runtime_error(
            "invalid benchmark option: " + std::string{argument});
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] Options parseOptions(int count, char** arguments)
{
    Options options;
    bool sawWarmups = false;
    bool sawSamples = false;
    bool sawCallbackSamples = false;
    bool sawSeed = false;
    bool sawOutput = false;
    for (int index = 1; index < count; ++index)
    {
        const auto argument = std::string_view{arguments[index]};
        if (argument.starts_with("--warmups="))
        {
            if (sawWarmups)
                throw std::runtime_error("duplicate --warmups option");
            sawWarmups = true;
            options.warmups = parseSize(argument, "--warmups=", 0, 1000);
        }
        else if (argument.starts_with("--samples="))
        {
            if (sawSamples)
                throw std::runtime_error("duplicate --samples option");
            sawSamples = true;
            options.samples = parseSize(argument, "--samples=", 1, 10000);
        }
        else if (argument.starts_with("--callback-samples="))
        {
            if (sawCallbackSamples)
                throw std::runtime_error("duplicate --callback-samples option");
            sawCallbackSamples = true;
            options.callbackSamples = parseSize(
                argument, "--callback-samples=", 1, 1'000'000);
        }
        else if (argument.starts_with("--seed="))
        {
            if (sawSeed)
                throw std::runtime_error("duplicate --seed option");
            sawSeed = true;
            options.seed = parseSize(
                argument, "--seed=", 1,
                std::numeric_limits<std::size_t>::max());
        }
        else if (argument.starts_with("--output="))
        {
            if (sawOutput)
                throw std::runtime_error("duplicate --output option");
            sawOutput = true;
            const auto value = argument.substr(std::string_view{"--output="}.size());
            if (value.empty())
                throw std::runtime_error("empty --output path");
            options.output = std::string{value};
        }
        else
        {
            throw std::runtime_error(
                "unknown benchmark option: " + std::string{argument});
        }
    }
    return options;
}

[[nodiscard]] talas::project::ProjectNode node(
    std::string id,
    std::string type,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(type), std::move(parameters)};
}

[[nodiscard]] talas::core::Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort)
{
    return {
        std::move(fromNode), std::move(fromPort),
        std::move(toNode), std::move(toPort),
    };
}

[[nodiscard]] talas::project::ProjectParameterExpression choice(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.choice = std::move(value);
    return result;
}

[[nodiscard]] talas::project::ProjectDocument oscillatorSmall(
    std::uint64_t seed)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("osc", "talas.oscillator", {
            {"frequency", std::to_string(220U + seed % 220U) + " hz"},
            {"amplitude", "0.25"},
        }),
    };
    document.graph.connections = {
        connection("osc", "audioOut", "out", "audioIn"),
    };
    return document;
}

[[nodiscard]] talas::project::ProjectDocument oscillatorChain(
    std::uint64_t seed)
{
    auto document = oscillatorSmall(seed);
    document.graph.connections.clear();
    for (std::size_t index = 0; index < 8; ++index)
    {
        document.graph.nodes.push_back(node(
            "vca" + std::to_string(index),
            "talas.vca",
            {{"gain", "0.99"}}));
        document.graph.connections.push_back(connection(
            index == 0 ? "osc" : "vca" + std::to_string(index - 1U),
            "audioOut",
            "vca" + std::to_string(index),
            "audioIn"));
    }
    document.graph.connections.push_back(
        connection("vca7", "audioOut", "out", "audioIn"));
    return document;
}

[[nodiscard]] talas::project::ProjectDocument spectralRoundTrip(
    std::uint64_t seed)
{
    auto document = oscillatorSmall(seed);
    document.graph.nodes.insert(
        document.graph.nodes.begin() + 1,
        node("inverse", "talas.inverseFftV1", {
            {"frameSize", "64"}, {"hopSize", "16"}, choice("window", "hann"),
        }));
    document.graph.nodes.insert(
        document.graph.nodes.begin() + 1,
        node("forward", "talas.forwardFftV1", {
            {"frameSize", "64"}, {"hopSize", "16"}, choice("window", "hann"),
        }));
    document.graph.connections = {
        connection("osc", "audioOut", "forward", "audioIn"),
        connection("forward", "spectrumOut", "inverse", "spectrumIn"),
        connection("inverse", "audioOut", "out", "audioIn"),
    };
    return document;
}

[[nodiscard]] talas::project::ProjectDocument eventControl(
    std::uint64_t seed)
{
    auto document = oscillatorSmall(seed);
    document.graph.nodes.insert(
        document.graph.nodes.begin() + 1,
        node("vca", "talas.vca", {{"gain", "!Gain"}}));
    document.graph.connections = {
        connection("osc", "audioOut", "vca", "audioIn"),
        connection("vca", "audioOut", "out", "audioIn"),
    };
    return document;
}

[[nodiscard]] talas::runtime::RenderSettings settingsFor(std::string_view id)
{
    talas::runtime::RenderSettings settings{sampleRate, blockFrames};
    if (id == "event-control")
    {
        settings.eventValues.push_back({
            "Gain", 0.5, talas::expr::ValueDomain::Scalar,
        });
    }
    return settings;
}

[[nodiscard]] Distribution summarize(std::vector<std::uint64_t> values)
{
    if (values.empty())
        throw std::runtime_error("cannot summarize empty benchmark samples");
    std::ranges::sort(values);
    const auto percentile = [&values](std::size_t numerator) {
        const auto rank = (numerator * values.size() + 99U) / 100U;
        return values[std::min(values.size() - 1U, rank - 1U)];
    };
    return {percentile(50), percentile(95), percentile(99), values.back()};
}

[[nodiscard]] std::uint64_t estimatedPreparedBytes(
    const talas::runtime::RuntimePlan& plan) noexcept
{
    std::uint64_t total = 0;
    for (const auto& buffer : plan.buffers)
    {
        if (buffer.complexFrameCapacity != 0)
        {
            total += static_cast<std::uint64_t>(buffer.complexFrameCapacity)
                * buffer.complexMaximumBinsPerFrame
                * std::max<std::size_t>(1, buffer.complexChannelCount)
                * 2U * sizeof(double);
        }
        else if (buffer.partialFrameCapacity != 0)
        {
            total += static_cast<std::uint64_t>(buffer.partialFrameCapacity)
                * buffer.partialMaximumPartialsPerFrame
                * 4U * sizeof(double);
        }
        else
        {
            total += static_cast<std::uint64_t>(buffer.frames)
                * sizeof(double);
        }
    }
    return total;
}

[[nodiscard]] talas::runtime::PreparedRuntimeGraph compileChecked(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::runtime::RenderSettings& settings)
{
    auto compiled = talas::runtime::compileProject(document, registry, settings);
    if (!compiled.ok())
        throw std::runtime_error("benchmark fixture failed to compile");
    return std::move(*compiled.graph);
}

[[nodiscard]] ScenarioResult benchmarkScenario(
    std::string id,
    const talas::project::ProjectDocument& document,
    const Options& options)
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto settings = settingsFor(id);
    for (std::size_t index = 0; index < options.warmups; ++index)
        (void)compileChecked(document, registry, settings);

    std::vector<std::uint64_t> compileSamples;
    compileSamples.reserve(options.samples);
    for (std::size_t index = 0; index < options.samples; ++index)
    {
        const auto start = Clock::now();
        auto graph = compileChecked(document, registry, settings);
        compileSamples.push_back(elapsedNs(start));
    }
    auto graph = compileChecked(document, registry, settings);

    for (std::size_t index = 0; index < options.warmups; ++index)
    {
        talas::runtime::RealtimeAudioEngine engine(graph);
        if (!engine.ready())
            throw std::runtime_error("benchmark engine warmup failed");
    }
    std::vector<std::uint64_t> prepareSamples;
    prepareSamples.reserve(options.samples);
    for (std::size_t index = 0; index < options.samples; ++index)
    {
        const auto start = Clock::now();
        talas::runtime::RealtimeAudioEngine engine(graph);
        prepareSamples.push_back(elapsedNs(start));
        if (!engine.ready())
            throw std::runtime_error("benchmark engine preparation failed");
    }

    talas::runtime::RealtimeAudioEngine engine(graph);
    if (!engine.ready())
        throw std::runtime_error("benchmark render engine failed");
    engine.setPlaying(true);
    std::array<float, blockFrames> left{};
    std::array<float, blockFrames> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    for (std::size_t index = 0; index < options.warmups; ++index)
    {
        if (!engine.render(channels.data(), channels.size(), blockFrames))
            throw std::runtime_error("benchmark callback warmup failed");
    }
    std::vector<std::uint64_t> callbackSamples;
    callbackSamples.reserve(options.callbackSamples);
    std::uint64_t deadlineMisses = 0;
    double checksum = 0.0;
    for (std::size_t index = 0; index < options.callbackSamples; ++index)
    {
        if (id == "event-control"
            && !engine.scheduleEventValue(
                "Gain",
                index % 2U == 0 ? 0.25 : 0.75,
                talas::expr::ValueDomain::Scalar,
                0))
        {
            throw std::runtime_error(
                "benchmark Event Value update was rejected");
        }
        left.fill(std::numeric_limits<float>::quiet_NaN());
        right.fill(std::numeric_limits<float>::quiet_NaN());
        const auto start = Clock::now();
        const auto rendered = engine.render(
            channels.data(), channels.size(), blockFrames);
        const auto duration = elapsedNs(start);
        if (!rendered)
            throw std::runtime_error("benchmark callback failed");
        callbackSamples.push_back(duration);
        deadlineMisses += duration > callbackDeadlineNs ? 1U : 0U;
        if (!std::all_of(left.begin(), left.end(), [](float sample) {
                return std::isfinite(sample);
            })
            || !std::all_of(right.begin(), right.end(), [](float sample) {
                return std::isfinite(sample);
            }))
        {
            throw std::runtime_error("benchmark callback output is non-finite");
        }
        checksum += left[index % blockFrames]
            + right[(index * 17U) % blockFrames];
    }
    if (!std::isfinite(checksum))
        throw std::runtime_error("benchmark output checksum is non-finite");
    if (std::abs(checksum) < 1.0e-9)
        throw std::runtime_error("benchmark output checksum proves no work");

    talas::runtime::RealtimeGraphHost host;
    host.setPlaying(true);
    std::vector<std::uint64_t> adoptionSamples;
    adoptionSamples.reserve(options.samples);
    double adoptionMagnitude = 0.0;
    for (std::size_t index = 0; index < options.samples; ++index)
    {
        (void)host.collectRetired();
        auto staged = std::make_unique<talas::runtime::RealtimeAudioEngine>(graph);
        if (!staged->ready())
            throw std::runtime_error("benchmark staged engine failed");
        left.fill(std::numeric_limits<float>::quiet_NaN());
        right.fill(std::numeric_limits<float>::quiet_NaN());
        const auto start = Clock::now();
        if (host.tryStage(std::move(staged))
            != talas::runtime::RealtimeGraphStageResult::Accepted)
        {
            throw std::runtime_error("benchmark graph stage rejected");
        }
        if (!host.render(channels.data(), channels.size(), blockFrames))
            throw std::runtime_error("benchmark graph adoption render failed");
        adoptionSamples.push_back(elapsedNs(start));
        for (std::size_t frame = 0; frame < blockFrames; ++frame)
        {
            if (!std::isfinite(left[frame]) || !std::isfinite(right[frame]))
                throw std::runtime_error(
                    "benchmark graph adoption output is non-finite");
            adoptionMagnitude += std::abs(left[frame]) + std::abs(right[frame]);
        }
    }
    host.clearAfterAudioStopped();
    if (!std::isfinite(adoptionMagnitude) || adoptionMagnitude < 1.0e-9)
        throw std::runtime_error(
            "benchmark graph adoption output proves no work");
    checksum += adoptionMagnitude;

    const auto& plan = graph.plan();
    return {
        std::move(id),
        document.graph.nodes.size(),
        document.graph.connections.size(),
        plan.steps.size(),
        plan.buffers.size(),
        estimatedPreparedBytes(plan),
        summarize(std::move(compileSamples)),
        summarize(std::move(prepareSamples)),
        summarize(std::move(callbackSamples)),
        summarize(std::move(adoptionSamples)),
        deadlineMisses,
        checksum,
    };
}

void appendDistribution(
    std::string& output,
    std::string_view name,
    const Distribution& value,
    bool comma)
{
    output += "      \"" + std::string{name} + "\": {"
        + "\"p50_ns\": " + std::to_string(value.p50)
        + ", \"p95_ns\": " + std::to_string(value.p95)
        + ", \"p99_ns\": " + std::to_string(value.p99)
        + ", \"max_ns\": " + std::to_string(value.maximum)
        + "}" + (comma ? ",\n" : "\n");
}

[[nodiscard]] std::string platformName()
{
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string architectureName()
{
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string makeReport(
    const Options& options,
    const std::vector<ScenarioResult>& scenarios,
    std::string_view runId,
    std::string_view buildFingerprint)
{
    std::string output;
    output += "{\n  \"format\": \"talas.performance-report\",\n";
    output += "  \"version\": 1,\n";
    output += "  \"runId\": \"" + std::string{runId} + "\",\n";
    output += "  \"environment\": {\"platform\": \"" + platformName()
        + "\", \"architecture\": \"" + architectureName()
        + "\", \"compilerId\": \"" TALAS_BENCHMARK_COMPILER_ID
          "\", \"compilerVersion\": \"" TALAS_BENCHMARK_COMPILER_VERSION
          "\", \"buildType\": \"" TALAS_BENCHMARK_BUILD_TYPE
          "\", \"commit\": \"" TALAS_BENCHMARK_COMMIT
          "\", \"preset\": \"" TALAS_BENCHMARK_PRESET_ID
          "\", \"buildFingerprint\": \"" + std::string{buildFingerprint}
        + "\"},\n";
    output += "  \"campaign\": {\"warmups\": "
        + std::to_string(options.warmups)
        + ", \"samples\": " + std::to_string(options.samples)
        + ", \"callbackSamples\": "
        + std::to_string(options.callbackSamples)
        + ", \"sampleRate\": 48000, \"blockFrames\": 256"
        + ", \"callbackDeadlineNs\": "
        + std::to_string(callbackDeadlineNs)
        + ", \"seed\": " + std::to_string(options.seed) + "},\n";
    output += "  \"scenarios\": [\n";
    for (std::size_t index = 0; index < scenarios.size(); ++index)
    {
        const auto& scenario = scenarios[index];
        output += "    {\n      \"id\": \"" + scenario.id + "\",\n";
        output += "      \"nodeCount\": " + std::to_string(scenario.nodeCount)
            + ", \"connectionCount\": "
            + std::to_string(scenario.connectionCount) + ",\n";
        output += "      \"metrics\": {\n";
        appendDistribution(output, "compile", scenario.compile, true);
        appendDistribution(output, "prepare", scenario.prepare, true);
        appendDistribution(output, "callback", scenario.callback, true);
        appendDistribution(output, "graphAdoption", scenario.adoption, true);
        output += "      \"callbackDeadlineMissCount\": "
            + std::to_string(scenario.callbackDeadlineMisses) + ",\n";
        output += "      \"planStepCount\": "
            + std::to_string(scenario.stepCount) + ",\n";
        output += "      \"planBufferSlots\": "
            + std::to_string(scenario.bufferSlots) + ",\n";
        output += "      \"estimatedPreparedBytes\": "
            + std::to_string(scenario.estimatedPreparedBytes) + ",\n";
        output += "      \"checksum\": " + std::to_string(scenario.checksum)
            + "\n      }\n    }"
            + (index + 1U == scenarios.size() ? "\n" : ",\n");
    }
    output += "  ]\n}\n";
    return output;
}
}

int main(int count, char** arguments)
{
    try
    {
        const auto options = parseOptions(count, arguments);
        std::vector<ScenarioResult> scenarios;
        scenarios.push_back(benchmarkScenario(
            "oscillator-small", oscillatorSmall(options.seed), options));
        scenarios.push_back(benchmarkScenario(
            "oscillator-chain", oscillatorChain(options.seed), options));
        scenarios.push_back(benchmarkScenario(
            "spectral-roundtrip", spectralRoundTrip(options.seed), options));
        scenarios.push_back(benchmarkScenario(
            "event-control", eventControl(options.seed), options));
        const auto report = makeReport(
            options, scenarios, makeRunId(), readBuildFingerprint());
        if (options.output.has_value())
        {
            std::ofstream output(*options.output, std::ios::binary | std::ios::trunc);
            output << report;
            if (!output.good())
                throw std::runtime_error("cannot write benchmark report");
        }
        else
        {
            std::cout << report;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Core performance benchmark failed: "
                  << error.what() << '\n';
        return 2;
    }
}
