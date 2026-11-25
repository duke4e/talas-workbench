#include "../support/RealtimeProbe.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectFile.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <cxxabi.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

// Renders every example project through the realtime bridge while the
// inserted probe counts allocations, locks, sleeps and file I/O on the
// rendering thread. Staging runs outside the probe; rendering, including the
// first callback that adopts the staged engine, runs inside it.
namespace
{
constexpr double defaultSampleRate = 48000.0;
constexpr std::size_t blockFrames = 128;
constexpr std::size_t probedBlocks = 64;
constexpr std::size_t inputChannels = 2;

// Examples that cannot render at the default rate, and why.
struct ExampleException
{
    std::string_view file;
    std::optional<double> sampleRate;
    std::string_view reason;
};

constexpr std::array exampleExceptions{
    ExampleException{
        "c11_disk_cache_capture.talas.json", 8000.0,
        "its captured DiskCache asset plays only at its 8000 Hz capture rate"},
    ExampleException{
        "m4_missing_component_preservation.talas.json", std::nullopt,
        "it preserves a missing component, so it never compiles"},
};

struct Probe
{
    TalasRealtimeProbeBegin begin = nullptr;
    TalasRealtimeProbeEnd end = nullptr;
};

Probe loadProbe()
{
    return {
        reinterpret_cast<TalasRealtimeProbeBegin>(
            dlsym(RTLD_DEFAULT, TALAS_REALTIME_PROBE_BEGIN_SYMBOL)),
        reinterpret_cast<TalasRealtimeProbeEnd>(
            dlsym(RTLD_DEFAULT, TALAS_REALTIME_PROBE_END_SYMBOL)),
    };
}

std::uint64_t violations(const TalasRealtimeProbeReport& report)
{
    std::uint64_t total = 0;
    for (const auto count : report.counts)
        total += count;
    return total;
}

std::string symbolName(void* frame)
{
    Dl_info info{};
    if (dladdr(frame, &info) == 0 || info.dli_sname == nullptr)
        return "?";
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> demangled(
        abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status),
        &std::free);
    return status == 0 && demangled ? demangled.get() : info.dli_sname;
}

std::string describe(const TalasRealtimeProbeReport& report)
{
    static constexpr std::array names{
        "allocations", "deallocations", "locks", "file I/O calls", "sleeps"};
    std::string text;
    for (std::size_t kind = 0; kind < names.size(); ++kind)
    {
        if (report.counts[kind] != 0)
        {
            text += std::to_string(report.counts[kind]) + " "
                + names[kind] + "; ";
        }
    }
    text += "first ";
    text += report.firstFunction != nullptr ? report.firstFunction : "?";
    text += " from:";
    for (int frame = 1; frame < report.firstFrameCount && frame < 16; ++frame)
        text += "\n    " + symbolName(report.firstFrames[frame]);
    return text;
}

std::optional<std::string> probeExample(
    const Probe& probe,
    const std::filesystem::path& path,
    double sampleRate,
    const talas::core::NodeRegistry& registry)
{
    auto loaded = talas::project::loadProjectFile(path);
    if (!loaded.ok())
    {
        return "does not load: "
            + (loaded.errors.empty() ? std::string{}
                                     : loaded.errors.front().message);
    }
    auto resources = talas::audio::loadReferencedProjectResourceSnapshots(
        *loaded.document, registry, path);
    if (!resources.ok())
    {
        return "resources do not load: "
            + (resources.failure.has_value()
                   ? talas::audio::describeProjectAssetLoadFailure(
                       *resources.failure)
                   : std::string{"cancelled"});
    }
    talas::runtime::RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = blockFrames;
    settings.resourceSnapshots = std::move(resources.snapshots);

    // Open the project the way the Workbench does, which also adds the
    // Event Value controls its expressions reference.
    talas::runtime::ProjectRuntimeBridge bridge(
        talas::project::ProjectDocument{}, registry);
    if (const auto opened = bridge.editor().replaceDocument(*loaded.document);
        !opened.ok())
    {
        return "does not open: " + opened.message;
    }
    bridge.setPlaying(true);
    const auto staged = bridge.compileAndStage(std::move(settings));
    if (!staged.ok())
        return "does not stage: " + staged.message;
    (void)bridge.enqueueMidiNoteOn(1, 60, 100, 0.0);
    (void)bridge.enqueueMidiNoteOff(
        1, 60, static_cast<double>(probedBlocks / 2 * blockFrames) / sampleRate);

    std::vector<std::vector<float>> input(
        inputChannels, std::vector<float>(blockFrames));
    for (auto& channel : input)
    {
        for (std::size_t frame = 0; frame < blockFrames; ++frame)
        {
            channel[frame] = 0.25F * static_cast<float>(std::sin(
                2.0 * std::numbers::pi * 441.0 * static_cast<double>(frame)
                / sampleRate));
        }
    }
    std::vector<const float*> inputPointers;
    for (const auto& channel : input)
        inputPointers.push_back(channel.data());
    const talas::runtime::RealtimeAudioInputBlock inputBlock{
        inputPointers.data(), inputPointers.size()};
    std::vector<std::vector<float>> output(
        std::max<std::size_t>(staged.outputChannelCount, 1),
        std::vector<float>(blockFrames));
    std::vector<float*> outputPointers;
    for (auto& channel : output)
        outputPointers.push_back(channel.data());
    std::array<talas::runtime::RealtimeMidiOutputEvent, 64> midiEvents{};
    std::array<talas::runtime::RealtimeGlobalControllerOutputEvent, 64>
        controllerEvents{};
    std::array<char, 256> controllerName{};

    TalasRealtimeProbeReport report{};
    std::size_t failedBlocks = 0;
    std::size_t firstFailedBlock = 0;
    probe.begin();
    for (std::size_t block = 0; block < probedBlocks; ++block)
    {
        const auto time =
            static_cast<double>(block * blockFrames) / sampleRate;
        bool rendered = false;
        if (staged.outputChannelCount == 0)
        {
            talas::runtime::RealtimeTerminalOutputBlock terminal{midiEvents};
            terminal.globalControllerEvents = controllerEvents;
            terminal.globalControllerName = controllerName;
            rendered = bridge.renderTerminalEvents(time, blockFrames, terminal)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered;
        }
        else
        {
            rendered = bridge.render(
                inputBlock,
                time,
                outputPointers.data(),
                outputPointers.size(),
                blockFrames);
        }
        if (!rendered && failedBlocks++ == 0)
            firstFailedBlock = block;
    }
    probe.end(&report);
    if (violations(report) != 0)
        return "realtime-unsafe calls while rendering: " + describe(report);
    if (failedBlocks != 0)
    {
        return std::to_string(failedBlocks) + " of "
            + std::to_string(probedBlocks)
            + " callbacks did not render, first callback "
            + std::to_string(firstFailedBlock);
    }
    return std::nullopt;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: talas_realtime_safety_tests <examples directory>\n";
        return 2;
    }
    const auto probe = loadProbe();
    if (probe.begin == nullptr || probe.end == nullptr)
    {
        std::cerr << "the realtime probe is not loaded; run with "
                     "DYLD_INSERT_LIBRARIES naming talas_realtime_probe\n";
        return 1;
    }
    // The probe must see what it is meant to catch.
    static void* (*volatile allocate)(std::size_t) = &std::malloc;
    static void (*volatile release)(void*) = &std::free;
    TalasRealtimeProbeReport selfCheck{};
    probe.begin();
    release(allocate(16));
    probe.end(&selfCheck);
    if (selfCheck.counts[TalasRealtimeProbeAllocation] == 0
        || selfCheck.counts[TalasRealtimeProbeDeallocation] == 0)
    {
        std::cerr << "the realtime probe did not see an allocation\n";
        return 1;
    }

    std::vector<std::filesystem::path> examples;
    for (const auto& entry :
         std::filesystem::directory_iterator{argv[1]})
    {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.ends_with(".talas.json"))
            examples.push_back(entry.path());
    }
    std::ranges::sort(examples);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const auto& example : examples)
    {
        const auto exception = std::ranges::find(
            exampleExceptions,
            example.filename().string(),
            [](const ExampleException& candidate) {
                return std::string{candidate.file};
            });
        auto sampleRate = defaultSampleRate;
        if (exception != exampleExceptions.end())
        {
            if (!exception->sampleRate.has_value())
            {
                ++skipped;
                std::cout << "skipped " << exception->file << ": "
                          << exception->reason << '\n';
                continue;
            }
            sampleRate = *exception->sampleRate;
        }
        if (const auto problem =
                probeExample(probe, example, sampleRate, registry))
        {
            ++failed;
            std::cerr << example.filename().string() << ": " << *problem
                      << '\n';
        }
    }
    std::cout << examples.size() - skipped << " examples rendered through "
              << "the realtime bridge; " << failed << " failed, "
              << skipped << " skipped\n";
    return examples.empty() || failed != 0 ? 1 : 0;
}
