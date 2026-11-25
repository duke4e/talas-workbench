#include "talas/audio/PartialTrackResynthesis.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <type_traits>
#include <utility>
#include <span>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
std::atomic<std::size_t> allocationBytes = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
    {
        allocationCount.fetch_add(1, std::memory_order_relaxed);
        allocationBytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

void testOwnedProcessingCloneAndMoveRestore()
{
    using namespace talas::audio;
    using talas::project::PartialTrackPointInterpolation;
    static_assert(!std::is_copy_constructible_v<PreparedPartialTrackResynthesizer>);
    static_assert(std::is_nothrow_move_assignable_v<PreparedPartialTrackResynthesizer>);
    for (const auto interpolation : {PartialTrackPointInterpolation::Step, PartialTrackPointInterpolation::Linear})
        for (const std::uint64_t segment : {0, 2})
            for (const std::size_t boundary : {0, 41, 99, 140})
            {
                talas::project::PartialTrackAsset asset{64.0, 160, {
                    {1, 0, {{3, 0.5, 7.3, 0.0}}},
                    {1, 32, {{3, 0.25, 11.1, 1.0}, {9, 0.1, 13.7, 0.2}}},
                    {1, 64, {{3, 0.8, 5.4, 2.0}, {9, 0.2, 17.0, 0.0}}},
                    {2, 80, {{5, 0.6, 9.2, 1.0}}},
                    {2, 112, {{5, 0.2, 13.1, 0.0}}},
                    {3, 136, {}},
                }};
                std::vector<PartialTrackPointInterpolation> modes(7, interpolation);
                const PartialTrackResynthesisProfile profile{maximumOfflinePartialTrackVoices, 0.375, segment};
                PreparedPartialTrackResynthesizer candidate(asset, modes, profile), reference(asset, modes, profile);
                require(candidate.ready() && reference.ready(), "clone fixture must prepare all owned tracks");
                asset.frames.clear(); modes.clear(); // Neither live processing nor its clone may borrow these inputs.
                std::array<double, 180> actual{}, expected{};
                if (boundary != 0)
                    require(candidate.render(0, std::span(actual).first(boundary)) == PartialTrackResynthesisStatus::Ok
                        && reference.render(0, std::span(expected).first(boundary)) == PartialTrackResynthesisStatus::Ok,
                        "clone fixture must independently prime phase and track/segment cursors");
                auto baseline = candidate.cloneProcessingState();
                const auto tracks = candidate.trackCount();
                require(candidate.render(boundary, actual) == PartialTrackResynthesisStatus::Ok,
                    "clone fixture must overwrite continuation state across the final segment");
                allocationCount.store(0);
                countAllocations.store(true);
                candidate = std::move(*baseline);
                countAllocations.store(false);
                require(allocationCount.load() == 0 && candidate.ready() && candidate.trackCount() == tracks
                    && candidate.sampleRate() == 64.0 && candidate.sourceFrameCount() == 160,
                    "owned move restore must retain complete preparation and continuation without allocation");
                baseline.reset();
                for (std::size_t frame = boundary; frame != 180;)
                {
                    const auto count = std::min(std::size_t{13}, 180 - frame);
                    allocationCount.store(0);
                    countAllocations.store(true);
                    const auto a = candidate.render(frame, std::span(actual).first(count));
                    const auto b = reference.render(frame, std::span(expected).first(count));
                    countAllocations.store(false);
                    require(a == PartialTrackResynthesisStatus::Ok && b == PartialTrackResynthesisStatus::Ok
                        && allocationCount.load() == 0 && std::equal(actual.begin(), actual.begin() + count, expected.begin()),
                        "restored clone must preserve exact interpolation, seeded bandwidth noise, phase and segment selection");
                    frame += count;
                }
            }
    const talas::project::PartialTrackAsset tone{64.0, 128, {{1, 0, {{1, 0.8, 7.3, 0.0}}}}};
    PreparedPartialTrackResynthesizer tonal(tone, {}, {maximumOfflinePartialTrackVoices, 0.25, 0});
    std::array<double, 87> samples{};
    require(tonal.render(0, std::span(samples).first(41)) == PartialTrackResynthesisStatus::Ok,
        "analytical clone fixture must commit a nonzero phase");
    auto checkpoint = tonal.cloneProcessingState();
    tonal.reset();
    tonal = std::move(*checkpoint);
    checkpoint.reset();
    require(tonal.render(41, samples) == PartialTrackResynthesisStatus::Ok,
        "analytical clone continuation must render");
    for (std::size_t index = 0; index != samples.size(); ++index)
        require(std::abs(samples[index] - 0.2 * std::sin(2.0 * std::numbers::pi * 7.3
            * static_cast<double>(41 + index) / 64.0)) < 1.0e-12,
            "restored resynthesis phase must satisfy the independent sine oracle");
    PreparedPartialTrackResynthesizer invalid(talas::project::PartialTrackAsset{});
    const auto invalidClone = invalid.cloneProcessingState();
    require(!invalidClone->ready() && invalidClone->render(0, samples) == PartialTrackResynthesisStatus::InvalidPreparation,
        "cloning must not promote an invalid preparation");
}

void testCompactProcessingCheckpoint()
{
    using namespace talas::audio;
    using talas::project::PartialTrackPointInterpolation;
    const auto withoutAllocation = [](auto &&action)
    {
        allocationCount.store(0);
        countAllocations.store(true);
        const auto ok = action();
        countAllocations.store(false);
        require(allocationCount.load() == 0, "compact checkpoint operations must allocate nothing");
        return ok;
    };
    for (const auto interpolation :
         {PartialTrackPointInterpolation::Step, PartialTrackPointInterpolation::Linear})
        for (const std::uint64_t segment : {0, 2})
            for (const std::size_t boundary : {0, 41, 79, 80, 99, 136, 160, 177})
            {
                talas::project::PartialTrackAsset asset{
                    64.0,
                    160,
                    {{1, 0, {{3, 0.5, 7.3, 0.0}}},
                     {1, 32, {{3, 0.25, 11.1, 1.0}, {9, 0.1, 13.7, 0.2}}},
                     {1, 64, {{3, 0.8, 5.4, 2.0}, {9, 0.2, 17.0, 0.0}}},
                     {2, 80, {{5, 0.6, 9.2, 1.0}}},
                     {2, 112, {{5, 0.2, 13.1, 0.0}}},
                     {3, 136, {}}}};
                const std::vector<PartialTrackPointInterpolation> modes(7, interpolation);
                const PartialTrackResynthesisProfile profile{maximumRealtimePartialTrackVoices,
                                                             .375, segment};
                PreparedPartialTrackResynthesizer candidate(asset, modes, profile),
                    reference(asset, modes, profile), foreign(asset, modes, profile);
                auto checkpoint = candidate.createProcessingCheckpoint();
                auto foreignCheckpoint = foreign.createProcessingCheckpoint();
                require(checkpoint != nullptr && foreignCheckpoint != nullptr,
                        "compact checkpoint must prepare from a ready processor");
                PreparedPartialTrackResynthesizer::ProcessingCheckpoint unprepared;
                require(withoutAllocation(
                            [&]
                            {
                                return !candidate.captureProcessingState(unprepared) &&
                                       !candidate.restoreProcessingState(unprepared) &&
                                       !candidate.captureProcessingState(*foreignCheckpoint) &&
                                       !candidate.restoreProcessingState(*foreignCheckpoint) &&
                                       !foreign.captureProcessingState(*checkpoint) &&
                                       !foreign.restoreProcessingState(*checkpoint);
                            }),
                        "unprepared and foreign checkpoints must reject without changing either "
                        "preparation");
                std::array<double, 180> actual{}, expected{}, cloned{};
                require(withoutAllocation(
                            [&]
                            {
                                return candidate.render(0, std::span(actual).first(boundary)) ==
                                           PartialTrackResynthesisStatus::Ok &&
                                       reference.render(0, std::span(expected).first(boundary)) ==
                                           PartialTrackResynthesisStatus::Ok &&
                                       candidate.captureProcessingState(*checkpoint);
                            }),
                        "compact checkpoint must capture the actual post-prepare phase and cursor");
                require(std::equal(actual.begin(), actual.begin() + boundary, expected.begin()),
                        "compact fixture prefix must match independent continuation");
                require(withoutAllocation(
                            [&]
                            {
                                if (candidate.render(boundary, actual) !=
                                    PartialTrackResynthesisStatus::Ok)
                                    return false;
                                candidate.reset();
                                return candidate.render(0, std::span(actual).first(75)) ==
                                       PartialTrackResynthesisStatus::Ok;
                            }),
                        "compact fixture must alter phase, point cursor and segment state before "
                        "restore");
                auto relocated = std::move(candidate);
                require(withoutAllocation(
                            [&]
                            {
                                return !candidate.captureProcessingState(*checkpoint) &&
                                       relocated.restoreProcessingState(*checkpoint);
                            }),
                        "compact checkpoint identity must follow an owned move");
                auto ownedClone = relocated.cloneProcessingState();
                require(ownedClone &&
                            withoutAllocation(
                                [&] { return ownedClone->restoreProcessingState(*checkpoint); }),
                        "explicit owned clone must retain checkpoint preparation authority");
                for (std::size_t frame = boundary; frame < actual.size();)
                {
                    const auto count = std::min(std::size_t{13}, actual.size() - frame);
                    require(
                        withoutAllocation(
                            [&]
                            {
                                return relocated.render(frame, std::span(actual).first(count)) ==
                                           PartialTrackResynthesisStatus::Ok &&
                                       reference.render(frame, std::span(expected).first(count)) ==
                                           PartialTrackResynthesisStatus::Ok &&
                                       ownedClone->render(frame, std::span(cloned).first(count)) ==
                                           PartialTrackResynthesisStatus::Ok;
                            }),
                        "compact restored processors must continue without allocations");
                    require(
                        std::equal(actual.begin(), actual.begin() + count, expected.begin()) &&
                            std::equal(cloned.begin(), cloned.begin() + count, expected.begin()),
                        "compact restored phase, cursor and segment state must match exact "
                        "uninterrupted PCM");
                    frame += count;
                }
            }
    const auto segmentedAsset = [](std::size_t segments)
    {
        talas::project::PartialTrackAsset asset{64.0, segments * 4, {}};
        for (std::size_t i = 0; i < segments; ++i)
            asset.frames.push_back(
                {i + 1, i * 4, {{1, .1, 7.3, 0.0}, {2, .2, 9.1, .2}, {3, .15, 11.7, 1.0}}});
        return asset;
    };
    PreparedPartialTrackResynthesizer shortAsset(segmentedAsset(1), {}, {3, 1.0, 0});
    PreparedPartialTrackResynthesizer longAsset(segmentedAsset(1024), {}, {3, 1.0, 0});
    require(shortAsset.ready() && longAsset.ready() && shortAsset.trackCount() == 3 &&
                longAsset.trackCount() == 3072,
            "bounded checkpoint fixture must vary inactive history by 1024 times");
    const auto checkpointBytes = [&](const PreparedPartialTrackResynthesizer &processor)
    {
        allocationBytes.store(0);
        allocationCount.store(0);
        countAllocations.store(true);
        auto checkpoint = processor.createProcessingCheckpoint();
        countAllocations.store(false);
        require(checkpoint != nullptr && allocationCount.load() != 0,
                "checkpoint must own prepared storage");
        const auto bytes = allocationBytes.load();
        require(withoutAllocation([&] { return processor.captureProcessingState(*checkpoint); }),
                "long asset checkpoint capture must not allocate");
        return bytes;
    };
    const auto shortBytes = checkpointBytes(shortAsset), longBytes = checkpointBytes(longAsset);
    require(shortBytes == longBytes,
            "checkpoint storage must depend on active tracks, not inactive segments or points");
    std::cout << "compact checkpoint storage bytes: " << shortBytes << " / " << longBytes << '\n';
    PreparedPartialTrackResynthesizer invalid({});
    require(!invalid.ready() && invalid.createProcessingCheckpoint() == nullptr,
            "invalid preparation must not produce a usable checkpoint");
}

// The resynthesis uses the spectrum file's start
// and end loop markers to loop it: past the end loop marker it plays again
// from the start loop marker, on past the file's end.
void testTheResynthesisLoopsBetweenTheLoopMarkers()
{
    talas::project::PartialTrackAsset asset{48000.0, 14400,
        {{1, 0, {{1, 0.5, 440.0, 0.0}}}, {1, 4800, {{1, 0.5, 440.0, 0.0}}}, {1, 9600, {{1, 0.5, 880.0, 0.0}}}}};
    const auto rendered = [](const talas::project::PartialTrackAsset& played) {
        talas::audio::PreparedPartialTrackResynthesizer resynthesizer{played};
        std::vector<double> output(19200, 0.0);
        for (std::size_t start = 0; start < output.size(); start += 64)
        {
            require(resynthesizer.render(start, std::span{output}.subspan(start, 64))
                    == talas::audio::PartialTrackResynthesisStatus::Ok,
                "the resynthesis should render");
        }
        return output;
    };
    const auto frequency = [](const std::vector<double>& samples, std::size_t from, std::size_t to) {
        std::size_t crossings = 0;
        for (auto index = from + 1; index < to; ++index)
            crossings += samples[index - 1] < 0.0 && samples[index] >= 0.0 ? 1 : 0;
        return static_cast<double>(crossings) * 48000.0 / static_cast<double>(to - from);
    };
    const auto level = [](const std::vector<double>& samples, std::size_t from, std::size_t to) {
        double sum = 0.0;
        for (auto index = from; index < to; ++index)
            sum += samples[index] * samples[index];
        return std::sqrt(sum / static_cast<double>(to - from));
    };
    const auto once = rendered(asset);
    asset.loopStartFrame = 0;
    asset.loopEndFrame = 4800;
    const auto looped = rendered(asset);
    require(std::abs(frequency(once, 10000, 14000) - 880.0) < 15.0 && level(once, 15000, 19000) == 0.0,
        "without loop markers the file should play once to its end");
    require(std::abs(frequency(looped, 10000, 14000) - 440.0) < 15.0 && level(looped, 15000, 19000) > 0.2,
        "with loop markers the resynthesis should loop between them, past the file's end");
}

int main()
{
    testTheResynthesisLoopsBetweenTheLoopMarkers();
    testCompactProcessingCheckpoint();
    testOwnedProcessingCloneAndMoveRestore();
    talas::project::PartialTrackAsset asset{
        48000.0,
        256,
        {
            {1, 0, {{1, 0.5, 440.0, 8.0}}},
            {1, 128, {{1, 0.25, 660.0, 8.0}}},
        }};
    talas::audio::PreparedPartialTrackResynthesizer whole{asset};
    talas::audio::PreparedPartialTrackResynthesizer partitioned{asset};
    require(whole.ready() && partitioned.ready(),
            "valid partial-track asset should prepare additive synthesis");
    std::array<double, 256> expected{};
    std::array<double, 256> actual{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto wholeStatus = whole.render(0, expected);
    const auto firstStatus = partitioned.render(
        0, std::span<double>{actual}.first(73));
    const auto secondStatus = partitioned.render(
        73, std::span<double>{actual}.subspan(73));
    countAllocations.store(false, std::memory_order_release);
    require(wholeStatus == talas::audio::PartialTrackResynthesisStatus::Ok
                && firstStatus == talas::audio::PartialTrackResynthesisStatus::Ok
                && secondStatus == talas::audio::PartialTrackResynthesisStatus::Ok
                && expected == actual,
            "prepared synthesis should be callback-partition invariant");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared synthesis callback should not allocate");
    const auto offline = talas::audio::renderPartialTrackAssetOffline(
        asset, {}, {}, 31);
    require(offline.ok()
                && offline.samples.size() == expected.size()
                && std::equal(
                    offline.samples.begin(),
                    offline.samples.end(),
                    expected.begin()),
            "offline render should reuse the prepared partition-invariant engine");
    const auto resynthesisInfo =
        talas::audio::partialTrackResynthesisInfo();
    require(resynthesisInfo.version == 1
                && resynthesisInfo.interpolatesAmplitudeAndFrequency
                && resynthesisInfo.synthesizesBandwidthNoise
                && resynthesisInfo.resetsPhaseOnDiscontinuity,
            "resynthesis quality and reset behavior must be explicit");

    const talas::project::PartialTrackAsset multiSegment{
        48000.0,
        8,
        {
            {1, 0, {{1, 0.4, 6000.0, 0.0}}},
            {2, 4, {{2, 0.8, 12000.0, 12000.0}}},
        }};
    talas::audio::PreparedPartialTrackResynthesizer segmented{multiSegment};
    std::array<double, 8> segmentedOutput{};
    require(segmented.ready()
                && segmented.render(0, segmentedOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok
                && std::ranges::any_of(
                    std::span<const double>{segmentedOutput}.first(4),
                    [](double sample) { return sample != 0.0; })
                && std::ranges::any_of(
                    std::span<const double>{segmentedOutput}.last(4),
                    [](double sample) { return sample != 0.0; }),
            "default resynthesis must render every sequential segment with bandwidth-sensitive output");

    auto tonalSegment = multiSegment;
    tonalSegment.frames.back().points.front().bandwidthHz = 0.0;
    talas::audio::PreparedPartialTrackResynthesizer tonal{tonalSegment};
    std::array<double, 8> tonalOutput{};
    require(tonal.ready()
                && tonal.render(0, tonalOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok
                && tonalOutput != segmentedOutput,
            "bandwidth must materially and deterministically affect resynthesis");

    const talas::project::PartialTrackAsset emptyBoundary{
        48000.0,
        8,
        {
            {1, 0, {{1, 0.4, 6000.0, 0.0}}},
            {2, 4, {}},
        }};
    talas::audio::PreparedPartialTrackResynthesizer emptySegment{emptyBoundary};
    talas::audio::PreparedPartialTrackResynthesizer selectedFirst{
        emptyBoundary,
        {},
        {talas::audio::maximumOfflinePartialTrackVoices, 1.0, 1}};
    std::array<double, 8> emptyBoundaryOutput{};
    std::array<double, 8> selectedFirstOutput{};
    require(emptySegment.ready()
                && selectedFirst.ready()
                && emptySegment.render(0, emptyBoundaryOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok
                && selectedFirst.render(0, selectedFirstOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok
                && std::ranges::any_of(
                    std::span<const double>{emptyBoundaryOutput}.first(4),
                    [](double sample) { return sample != 0.0; })
                && std::ranges::all_of(
                    std::span<const double>{emptyBoundaryOutput}.last(4),
                    [](double sample) { return sample == 0.0; })
                && std::ranges::all_of(
                    std::span<const double>{selectedFirstOutput}.last(4),
                    [](double sample) { return sample == 0.0; }),
            "empty segment boundaries and explicit nonfinal selection must stop prior oscillators exactly");
    const talas::project::PartialTrackAsset allEmpty{
        48000.0,
        8,
        {
            {1, 0, {}},
            {2, 4, {}},
        }};
    talas::audio::PreparedPartialTrackResynthesizer silent{allEmpty};
    std::array<double, 8> silentOutput{};
    require(silent.ready()
                && silent.render(0, silentOutput)
                    == talas::audio::PartialTrackResynthesisStatus::Ok
                && std::ranges::all_of(
                    silentOutput,
                    [](double sample) { return sample == 0.0; }),
            "valid all-empty segment assets must prepare as deterministic silence");

    auto overRealtimeLimit = asset;
    overRealtimeLimit.frames.clear();
    overRealtimeLimit.frames.push_back({1, 0, {}});
    for (std::uint64_t trackId = 1;
         trackId <= talas::audio::maximumRealtimePartialTrackVoices + 1;
         ++trackId)
    {
        overRealtimeLimit.frames.front().points.push_back(
            {trackId, 0.01, 100.0 + static_cast<double>(trackId), 0.0});
    }
    talas::audio::PreparedPartialTrackResynthesizer realtimeRejected{
        overRealtimeLimit,
        {},
        {talas::audio::maximumRealtimePartialTrackVoices, 1.0, 0}};
    require(!realtimeRejected.ready(),
            "realtime profile must fail closed above its fixed 128-voice budget");

    talas::audio::PartialComplexAdapterProfile profile{
        64, 16, 48000.0, 8};
    std::array<std::complex<double>, 33> bins{};
    const std::array<talas::project::PartialTrackPoint, 1> partial{{
        {7, 0.75, 6000.0, 750.0}}};
    require(talas::audio::partialsToComplexBins(partial, profile, bins)
                == talas::audio::PartialComplexConversionStatus::Ok
                && std::abs(bins[8].real() - 0.75) < 1.0e-12,
            "explicit partial-to-complex adapter should quantize to the declared bin grid");
    std::array<talas::project::PartialTrackPoint, 8> recovered{};
    std::size_t written = 0;
    require(talas::audio::complexBinsToPartials(
                bins, profile, recovered, written)
                == talas::audio::PartialComplexConversionStatus::Ok
                && written == 1
                && recovered[0].frequencyHz == 6000.0
                && recovered[0].trackId == 0,
            "explicit complex-to-partial adapter should retain untracked identity until off-audio tracking");
    const auto info = talas::audio::partialComplexConversionInfo(profile);
    require(info.version == 1
                && info.latencyFrames == 64
                && info.losesTrackIdentity
                && info.quantizesFrequencyToBins
                && info.discardsPhase,
            "conversion must expose latency and all lossy-quality facts");

    std::cout << "Partial track resynthesis tests passed\n";
    return 0;
}
