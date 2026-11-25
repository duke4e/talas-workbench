#include "app/workbench/WorkbenchGraphCanvas.h"
#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/app/WorkbenchGraphViewportIndex.h"
#include "talas/project/ProjectValidation.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
#ifndef TALAS_PRODUCT_PROBE_COMMIT
#error "TALAS_PRODUCT_PROBE_COMMIT must bind the product probe to Git"
#endif
#ifndef TALAS_PRODUCT_PROBE_FINGERPRINT_FILE
#error "TALAS_PRODUCT_PROBE_FINGERPRINT_FILE must bind the product probe build"
#endif

constexpr std::size_t defaultWarmups = 16;
constexpr std::size_t defaultSamples = 512;
constexpr std::size_t maximumSamples = 4096;
constexpr int canvasWidth = 1440;
constexpr int canvasHeight = 900;
constexpr std::size_t fixtureNodeCount = 1000;
constexpr std::size_t fixtureConnectionCount = 2000;
constexpr std::size_t cacheRebuildSamples = 16;

bool lowercaseSha256(std::string_view value)
{
    return value.size() == 64U
        && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

std::string buildFingerprint()
{
    std::ifstream input(TALAS_PRODUCT_PROBE_FINGERPRINT_FILE, std::ios::binary);
    std::string value;
    if (!input || !std::getline(input, value) || !lowercaseSha256(value))
        throw std::runtime_error("product probe build fingerprint is unavailable");
    input >> std::ws;
    if (!input.eof())
        throw std::runtime_error("product probe build fingerprint has trailing data");
    return value;
}

std::string executableSha256()
{
    const auto executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
    if (!executable.existsAsFile())
        throw std::runtime_error("cannot resolve the running probe executable");
    const auto value = juce::SHA256(executable).toHexString().toStdString();
    if (!lowercaseSha256(value))
        throw std::runtime_error("cannot hash the running probe executable");
    return value;
}

std::string probeRunId()
{
    const auto value = juce::Uuid().toString().removeCharacters("-")
        .toLowerCase().toStdString();
    if (value.size() != 32U
        || !std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           }))
    {
        throw std::runtime_error("cannot create product probe run identity");
    }
    return value;
}

struct Options
{
    std::size_t warmups = defaultWarmups;
    std::size_t samples = defaultSamples;
    std::filesystem::path output;
    bool stdoutReport = false;
    std::uint64_t deviceDurationMs = 0;
    bool requireDevice = false;
};

enum class DeviceProbePhase : std::uint8_t
{
    Initialising,
    Observing,
};

std::size_t parseCount(std::string_view text, std::string_view label)
{
    if (text.empty())
        throw std::runtime_error(std::string{label} + " is empty");
    std::size_t value = 0;
    for (const auto character : text)
    {
        if (character < '0' || character > '9')
            throw std::runtime_error(std::string{label} + " must be decimal");
        const auto digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
            throw std::runtime_error(std::string{label} + " overflows");
        value = value * 10U + digit;
    }
    if (value == 0 || value > maximumSamples)
        throw std::runtime_error(std::string{label} + " is outside [1, 4096]");
    return value;
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    bool sawWarmups = false;
    bool sawSamples = false;
    bool sawOutput = false;
    bool sawStdout = false;
    bool sawDeviceDuration = false;
    bool sawRequireDevice = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument.starts_with("--warmups="))
        {
            if (sawWarmups)
                throw std::runtime_error("duplicate --warmups option");
            sawWarmups = true;
            options.warmups = parseCount(
                argument.substr(std::string_view{"--warmups="}.size()),
                "warmups");
        }
        else if (argument.starts_with("--samples="))
        {
            if (sawSamples)
                throw std::runtime_error("duplicate --samples option");
            sawSamples = true;
            options.samples = parseCount(
                argument.substr(std::string_view{"--samples="}.size()),
                "samples");
        }
        else if (argument.starts_with("--output="))
        {
            if (sawOutput)
                throw std::runtime_error("duplicate --output option");
            sawOutput = true;
            const auto value = argument.substr(
                std::string_view{"--output="}.size());
            if (value.empty())
                throw std::runtime_error("empty --output path");
            options.output = std::string{value};
        }
        else if (argument == "--stdout")
        {
            if (sawStdout)
                throw std::runtime_error("duplicate --stdout option");
            sawStdout = true;
            options.stdoutReport = true;
        }
        else if (argument.starts_with("--device-duration-ms="))
        {
            if (sawDeviceDuration)
                throw std::runtime_error("duplicate --device-duration-ms option");
            sawDeviceDuration = true;
            options.deviceDurationMs = parseCount(
                argument.substr(
                    std::string_view{"--device-duration-ms="}.size()),
                "device duration");
            if (options.deviceDurationMs < 100U)
                throw std::runtime_error("device duration is outside [100, 4096]");
        }
        else if (argument == "--require-device")
        {
            if (sawRequireDevice)
                throw std::runtime_error("duplicate --require-device option");
            sawRequireDevice = true;
            options.requireDevice = true;
        }
        else
        {
            throw std::runtime_error(
                "unknown product probe option: " + std::string{argument});
        }
    }
    if (options.output.empty() == !options.stdoutReport)
        throw std::runtime_error("choose exactly one of --output or --stdout");
    if (std::filesystem::exists(options.output))
        throw std::runtime_error("output report already exists");
    if (options.requireDevice && options.deviceDurationMs == 0)
        throw std::runtime_error("--require-device requires --device-duration-ms");
    return options;
}

talas::project::ProjectNode node(std::string id, std::string type)
{
    return {std::move(id), std::move(type), {}};
}

talas::core::Connection connection(
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

talas::project::ProjectDocument largeGraphDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.reserve(fixtureNodeCount);
    document.graph.connections.reserve(fixtureConnectionCount);
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    constexpr auto mixerCount = fixtureNodeCount - 2U;
    for (std::size_t index = 0; index < mixerCount; ++index)
    {
        document.graph.nodes.push_back(
            node("mix" + std::to_string(index), "talas.overlappingMixer"));
    }
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    for (std::size_t input = 1; input <= 4; ++input)
    {
        document.graph.connections.push_back(
            connection(
                "osc", "audioOut", "mix0",
                "in" + std::to_string(input)));
    }
    for (std::size_t index = 1; index < mixerCount; ++index)
    {
        const auto previous = "mix" + std::to_string(index - 1U);
        const auto current = "mix" + std::to_string(index);
        document.graph.connections.push_back(
            connection(previous, "leftOut", current, "in1"));
        document.graph.connections.push_back(
            connection(previous, "rightOut", current, "in2"));
    }
    const auto finalMixer = "mix" + std::to_string(mixerCount - 1U);
    document.graph.connections.push_back(
        connection(finalMixer, "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(
        connection(finalMixer, "rightOut", "out", "rightIn"));
    talas::project::reconcileProjectPresentation(document);
    auto& layout = document.presentation.structureLayouts.front();
    for (std::size_t index = 0; index < layout.nodes.size(); ++index)
    {
        layout.nodes[index].position = {
            static_cast<std::int32_t>((index % 40U) * 224U),
            static_cast<std::int32_t>((index / 40U) * 128U),
        };
    }
    return document;
}

talas::project::ProjectDocument deviceProbeDocument()
{
    constexpr std::size_t deviceNodeCount = 128;
    talas::project::ProjectDocument document;
    document.graph.nodes.reserve(deviceNodeCount);
    document.graph.connections.reserve(deviceNodeCount - 1U);
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    for (std::size_t index = 1; index + 1U < deviceNodeCount; ++index)
    {
        const auto current = "vca" + std::to_string(index);
        document.graph.nodes.push_back(node(current, "talas.vca"));
        document.graph.connections.push_back(connection(
            index == 1U
                ? "osc"
                : "vca" + std::to_string(index - 1U),
            "audioOut", current, "audioIn"));
    }
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.connections.push_back(connection(
        "vca" + std::to_string(deviceNodeCount - 2U),
        "audioOut", "out", "audioIn"));
    talas::project::reconcileProjectPresentation(document);
    return document;
}

std::uint64_t nearestRank(
    const std::vector<std::uint64_t>& sorted,
    std::size_t numerator,
    std::size_t denominator)
{
    const auto rank = (sorted.size() * numerator + denominator - 1U)
        / denominator;
    return sorted[std::max<std::size_t>(1U, rank) - 1U];
}

std::string pixelDigest(const juce::Image& image)
{
    juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    std::array<std::uint64_t, 4> hashes{
        1469598103934665603ULL,
        1099511628211ULL,
        0x9e3779b97f4a7c15ULL,
        0xd6e8feb86659fd93ULL,
    };
    constexpr std::array<std::uint64_t, 4> primes{
        1099511628211ULL,
        0x100000001b3ULL,
        0x9ddfea08eb382d69ULL,
        0xc6a4a7935bd1e995ULL,
    };
    for (int y = 0; y < data.height; ++y)
    {
        const auto* row = data.getLinePointer(y);
        const auto bytes = static_cast<std::size_t>(
            data.width * data.pixelStride);
        for (std::size_t index = 0; index < bytes; ++index)
        {
            for (std::size_t lane = 0; lane < hashes.size(); ++lane)
            {
                hashes[lane] ^= static_cast<std::uint64_t>(
                    row[index] + static_cast<unsigned char>(lane * 17U));
                hashes[lane] *= primes[lane];
            }
        }
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : hashes)
        output << std::setw(16) << value;
    return output.str();
}

std::string jsonString(std::string_view value)
{
    std::ostringstream output;
    output << '"';
    for (const auto character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U)
            {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0')
                       << static_cast<unsigned int>(
                              static_cast<unsigned char>(character))
                       << std::dec;
            }
            else
            {
                output << character;
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

std::optional<talas::app::workbench::ProductDeviceProbeObservation>
observeDevice(
    const Options& options,
    std::atomic<DeviceProbePhase>& phase)
{
    if (options.deviceDurationMs == 0)
        return std::nullopt;
    auto stateDirectory = std::filesystem::temp_directory_path()
        / ("talas-product-performance-state-"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    talas::app::workbench::WorkbenchHostOptions hostOptions;
    hostOptions.enableHardware = true;
    hostOptions.enablePersistentState = false;
    hostOptions.applicationName = "Talas Product Performance Probe";
    hostOptions.isolatedStateDirectory = std::move(stateDirectory);
    auto workbench = std::make_unique<talas::app::workbench::WorkbenchComponent>(
        [](const auto&) {}, std::move(hostOptions));
    const auto document = deviceProbeDocument();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    if (!talas::project::validateProjectDocument(document, registry).ok()
        || !workbench->installProductPerformanceDocumentForProbe(document))
    {
        return std::nullopt;
    }
    workbench->setSize(canvasWidth, canvasHeight);
    workbench->beginHardwareInitialisation();
    juce::Image frame(juce::Image::ARGB, canvasWidth, canvasHeight, true);
    {
        juce::Graphics graphics(frame);
        workbench->paintEntireComponent(graphics, false);
    }
    const auto readyDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);
    while (!workbench->productPerformanceDeviceReadyForProbe()
           && std::chrono::steady_clock::now() < readyDeadline)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    if (!workbench->beginProductPerformancePlaybackForProbe())
        return std::nullopt;
    phase.store(DeviceProbePhase::Observing, std::memory_order_release);
    const auto observationDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(options.deviceDurationMs);
    while (std::chrono::steady_clock::now() < observationDeadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    auto observation = workbench->productPerformanceDeviceObservationForProbe();
    workbench->stopProductPerformancePlaybackForProbe();
    return observation;
}

void renderOnce(
    talas::app::workbench::GraphCanvas& canvas,
    juce::Image& image)
{
    image.clear(image.getBounds(), juce::Colours::transparentBlack);
    juce::Graphics graphics(image);
    canvas.paintEntireComponent(graphics, false);
    if (image.getPixelAt(0, 0).getARGB() == 0U)
        throw std::runtime_error("graph canvas did not paint a fresh frame");
}

std::string reportJson(
    const Options& options,
    const std::vector<std::uint64_t>& sorted,
    const std::vector<std::uint64_t>& sortedRebuilds,
    std::string_view digest,
    std::string_view runId,
    std::string_view probeBuildFingerprint,
    std::string_view probeExecutableSha256,
    const std::optional<talas::app::workbench::ProductDeviceProbeObservation>&
        device,
    std::string_view unavailableDeviceReason = {})
{
    std::ostringstream output;
    output << "{\n"
           << "  \"format\": \"talas.product-probe-report\",\n"
           << "  \"version\": 2,\n"
           << "  \"commit\": \"" << TALAS_PRODUCT_PROBE_COMMIT << "\",\n"
           << "  \"runId\": \"" << runId << "\",\n"
           << "  \"buildFingerprint\": \""
           << probeBuildFingerprint << "\",\n"
           << "  \"executableSha256\": \""
           << probeExecutableSha256 << "\",\n"
           << "  \"ui\": {\n"
           << "    \"availability\": \"available\",\n"
           << "    \"authority\": \"observational\",\n"
           << "    \"fixture\": \"large-graph-1000-2000\",\n"
           << "    \"width\": " << canvasWidth << ",\n"
           << "    \"height\": " << canvasHeight << ",\n"
           << "    \"scale\": 1.0,\n"
           << "    \"warmups\": " << options.warmups << ",\n"
           << "    \"samples\": " << options.samples << ",\n"
           << "    \"nodeCount\": " << fixtureNodeCount << ",\n"
           << "    \"connectionCount\": " << fixtureConnectionCount << ",\n"
           << "    \"p50Ns\": " << nearestRank(sorted, 50U, 100U) << ",\n"
           << "    \"p95Ns\": " << nearestRank(sorted, 95U, 100U) << ",\n"
           << "    \"p99Ns\": " << nearestRank(sorted, 99U, 100U) << ",\n"
           << "    \"maxNs\": " << sorted.back() << ",\n"
           << "    \"rebuildSamples\": " << sortedRebuilds.size() << ",\n"
           << "    \"rebuildP50Ns\": "
           << nearestRank(sortedRebuilds, 50U, 100U) << ",\n"
           << "    \"rebuildP95Ns\": "
           << nearestRank(sortedRebuilds, 95U, 100U) << ",\n"
           << "    \"rebuildMaxNs\": " << sortedRebuilds.back() << ",\n"
           << "    \"pixelDigest\": \"" << digest << "\"\n"
           << "  },\n"
           << "  \"device\": {\n";
    if (device.has_value())
    {
        output << "    \"availability\": \"available\",\n"
               << "    \"authority\": \"observational\",\n"
               << "    \"name\": " << jsonString(device->name) << ",\n"
               << "    \"type\": " << jsonString(device->type) << ",\n"
               << "    \"sampleRate\": " << device->sampleRate << ",\n"
               << "    \"blockFrames\": " << device->blockFrames << ",\n"
               << "    \"inputChannels\": " << device->inputChannels << ",\n"
               << "    \"outputChannels\": " << device->outputChannels << ",\n"
               << "    \"durationMs\": " << options.deviceDurationMs << ",\n"
               << "    \"callbacks\": "
               << device->metrics.deviceCallbacks << ",\n"
               << "    \"callbackPeriodNs\": "
               << device->metrics.devicePeriodNanoseconds << ",\n"
               << "    \"maximumCallbackNs\": "
               << device->metrics.deviceMaximumCallbackNanoseconds << ",\n"
               << "    \"deadlineOverruns\": "
               << device->metrics.deviceDeadlineOverruns << "\n";
    }
    else
    {
        output << "    \"availability\": \"unavailable\",\n"
               << "    \"authority\": \"observational\",\n"
               << "    \"reason\": \""
               << (unavailableDeviceReason.empty()
                       ? (options.deviceDurationMs == 0
                              ? "device-not-requested"
                              : "device-unavailable-or-stage-failed")
                       : unavailableDeviceReason)
               << "\"\n";
    }
    output << "  }\n"
           << "}\n";
    return output.str();
}

void writeReport(const std::filesystem::path& path, const std::string& text)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path{
        path.string() + ".tmp." + juce::Uuid{}.toString().toStdString()};
    try
    {
        {
            std::ofstream stream(
                temporary, std::ios::binary | std::ios::trunc);
            if (!stream || !(stream << text) || !stream.flush())
                throw std::runtime_error("cannot write product probe report");
        }
        std::filesystem::create_hard_link(temporary, path);
        std::filesystem::remove(temporary);
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void emitReport(const Options& options, const std::string& report)
{
    if (options.stdoutReport)
    {
        std::cout << report;
        std::cout.flush();
        return;
    }
    writeReport(options.output, report);
    std::cout << "Product probe report: " << options.output << '\n';
    std::cout.flush();
}
}

int main(int argc, char** argv)
{
    try
    {
        const auto options = parseOptions(argc, argv);
        juce::ScopedJuceInitialiser_GUI juce;
        const auto registry = talas::core::makeBuiltinNodeRegistry();
        const auto document = largeGraphDocument();
        if (document.graph.nodes.size() != fixtureNodeCount
            || document.graph.connections.size() != fixtureConnectionCount
            || !talas::project::validateProjectDocument(document, registry).ok())
        {
            throw std::runtime_error(
                "large graph probe fixture is not an exact product-valid graph");
        }
        talas::runtime::ProjectRuntimeBridge bridge(
            document, registry);
        auto canvas = talas::app::workbench::makeGraphCanvas(
            bridge,
            registry,
            [](auto, auto, auto) {},
            [] {},
            [](const auto&) {},
            [](auto, auto, auto) {},
            [](auto, auto) {},
            [](auto, auto, auto, auto) {},
            [](auto) {});
        canvas->setSize(canvasWidth, canvasHeight);
        canvas->setSessionViewState(0.0, 0.0, 1.0);
        if (canvas->totalNodeCountForSmoke() != fixtureNodeCount
            || canvas->visibleNodeCountForSmoke() == 0
            || canvas->visibleNodeCountForSmoke() >= fixtureNodeCount
            || canvas->graphViewportMembershipCountForSmoke()
                > (fixtureNodeCount + fixtureConnectionCount)
                    * talas::app::WorkbenchGraphViewportIndex::maximumMembershipsPerItem)
        {
            throw std::runtime_error(
                "large graph viewport cache did not preserve its bounded fixture contract");
        }
        std::vector<std::uint64_t> cacheDurations;
        cacheDurations.reserve(cacheRebuildSamples);
        for (std::size_t index = 0; index < cacheRebuildSamples; ++index)
        {
            const auto started = std::chrono::steady_clock::now();
            canvas->rebuildStructuralGraphCachesForSmoke();
            const auto finished = std::chrono::steady_clock::now();
            cacheDurations.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started).count()));
        }
        std::ranges::sort(cacheDurations);
        std::cerr
            << "Graph cache rebuild ns: p50="
            << nearestRank(cacheDurations, 50U, 100U)
            << " p95=" << nearestRank(cacheDurations, 95U, 100U)
            << " max=" << cacheDurations.back() << '\n';
        const auto structuralRebuilds =
            canvas->structuralGraphRebuildCountForSmoke();
        const auto accessibilityRebuilds =
            canvas->accessibilityGraphRebuildCountForSmoke();
        juce::Image image(
            juce::Image::ARGB, canvasWidth, canvasHeight, true);
        for (std::size_t index = 0; index < options.warmups; ++index)
            renderOnce(*canvas, image);
        std::vector<std::uint64_t> durations;
        durations.reserve(options.samples);
        for (std::size_t index = 0; index < options.samples; ++index)
        {
            image.clear(image.getBounds(), juce::Colours::transparentBlack);
            juce::Graphics graphics(image);
            const auto started = std::chrono::steady_clock::now();
            canvas->paintEntireComponent(graphics, false);
            const auto finished = std::chrono::steady_clock::now();
            if (image.getPixelAt(0, 0).getARGB() == 0U)
                throw std::runtime_error("graph canvas skipped a sampled frame");
            durations.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started).count()));
        }
        if (canvas->structuralGraphRebuildCountForSmoke()
                != structuralRebuilds
            || canvas->accessibilityGraphRebuildCountForSmoke()
                != accessibilityRebuilds)
        {
            throw std::runtime_error(
                "paint unexpectedly rebuilt structural or accessibility caches");
        }
        std::ranges::sort(durations);
        const auto digest = pixelDigest(image);
        if (digest == std::string(64U, '0'))
            throw std::runtime_error("graph canvas pixel digest is empty");
        const auto probeBuildFingerprint = buildFingerprint();
        const auto probeExecutableSha256 = executableSha256();
        const auto runId = probeRunId();
        std::mutex watchdogMutex;
        std::condition_variable watchdogCondition;
        bool deviceObservationFinished = false;
        std::atomic devicePhase{DeviceProbePhase::Initialising};
        std::jthread watchdog;
        if (options.deviceDurationMs != 0)
        {
            watchdog = std::jthread([&] {
                std::unique_lock lock(watchdogMutex);
                const auto timeout = std::chrono::seconds(20)
                    + std::chrono::milliseconds(options.deviceDurationMs);
                if (watchdogCondition.wait_for(lock, timeout, [&] {
                        return deviceObservationFinished;
                    }))
                {
                    return;
                }
                lock.unlock();
                try
                {
                    const auto timeoutReason =
                        devicePhase.load(std::memory_order_acquire)
                                == DeviceProbePhase::Initialising
                            ? "device-initialisation-timeout"
                            : "device-observation-timeout";
                    const auto timeoutReport = reportJson(
                        options,
                        durations,
                        cacheDurations,
                        digest,
                        runId,
                        probeBuildFingerprint,
                        probeExecutableSha256,
                        std::nullopt,
                        timeoutReason);
                    emitReport(options, timeoutReport);
                    std::cerr
                        << "Product probe: device observation timed out\n";
                    std::cerr.flush();
                    std::_Exit(options.requireDevice ? 2 : 0);
                }
                catch (const std::exception& error)
                {
                    std::cerr
                        << "Product probe: timeout report failed: "
                        << error.what() << '\n';
                    std::cerr.flush();
                    std::_Exit(1);
                }
            });
        }
        std::optional<talas::app::workbench::ProductDeviceProbeObservation> device;
        try
        {
            device = observeDevice(options, devicePhase);
        }
        catch (...)
        {
            {
                const std::lock_guard lock(watchdogMutex);
                deviceObservationFinished = true;
            }
            watchdogCondition.notify_one();
            if (watchdog.joinable())
                watchdog.join();
            throw;
        }
        {
            const std::lock_guard lock(watchdogMutex);
            deviceObservationFinished = true;
        }
        watchdogCondition.notify_one();
        if (watchdog.joinable())
            watchdog.join();
        const auto report = reportJson(
            options,
            durations,
            cacheDurations,
            digest,
            runId,
            probeBuildFingerprint,
            probeExecutableSha256,
            device);
        emitReport(options, report);
        return options.requireDevice && !device.has_value() ? 2 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Product probe: FAIL: " << error.what() << '\n';
        return 1;
    }
}
