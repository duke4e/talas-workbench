#include "talas/audio/WaveformOverview.h"
#include "talas/audio/WavWriter.h"

#include <chrono>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
std::atomic<std::size_t> maximumSingleAllocation{
    std::numeric_limits<std::size_t>::max()};
}

void* operator new(std::size_t size)
{
    if (size > maximumSingleAllocation.load(std::memory_order_relaxed))
        throw std::bad_alloc{};
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    ::operator delete(memory);
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::filesystem::path temporaryWavPath()
{
    static int counter = 0;
    return std::filesystem::temp_directory_path()
        / ("talas-waveform-overview-"
           + std::to_string(
               std::chrono::steady_clock::now()
                   .time_since_epoch()
                   .count())
           + "-" + std::to_string(counter++) + ".wav");
}

class ScopedSingleAllocationLimit
{
public:
    explicit ScopedSingleAllocationLimit(std::size_t limit)
        : previous_(maximumSingleAllocation.exchange(
              limit,
              std::memory_order_relaxed))
    {
    }

    ~ScopedSingleAllocationLimit()
    {
        maximumSingleAllocation.store(
            previous_,
            std::memory_order_relaxed);
    }

private:
    std::size_t previous_;
};

void writeSparseStereoPcm16Wav(
    const std::filesystem::path& path,
    std::uint32_t frameCount)
{
    constexpr std::uint16_t channelCount = 2;
    constexpr std::uint16_t blockAlign = channelCount * 2;
    const auto dataSize = frameCount * static_cast<std::uint32_t>(blockAlign);
    std::array<unsigned char, 44> header{};
    const auto ascii = [&](std::size_t offset, std::string_view value) {
        for (std::size_t index = 0; index < value.size(); ++index)
            header[offset + index] = static_cast<unsigned char>(value[index]);
    };
    const auto u16 = [&](std::size_t offset, std::uint16_t value) {
        header[offset] = static_cast<unsigned char>(value & 0xffU);
        header[offset + 1] = static_cast<unsigned char>(value >> 8U);
    };
    const auto u32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            header[offset + byte] = static_cast<unsigned char>(
                value >> (byte * 8U));
        }
    };
    ascii(0, "RIFF");
    u32(4, dataSize + 36U);
    ascii(8, "WAVE");
    ascii(12, "fmt ");
    u32(16, 16U);
    u16(20, 1U);
    u16(22, channelCount);
    u32(24, 48000U);
    u32(28, 48000U * blockAlign);
    u16(32, blockAlign);
    u16(34, 16U);
    ascii(36, "data");
    u32(40, dataSize);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    output.seekp(
        static_cast<std::streamoff>(header.size())
            + static_cast<std::streamoff>(dataSize) - 1);
    output.put('\0');
    require(output.good(), "large sparse PCM16 WAV should be writable");
}

void testOverviewBuildsExactBoundedMinMaxBuckets()
{
    const std::vector<std::vector<double>> channels{
        {0.5, -0.5, 0.25, 0.75, -0.25, 1.0, -1.0, 0.0},
        {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7},
    };
    const talas::audio::WaveformOverviewRequest request{
        "asset-a",
        7,
        3,
    };
    const auto result = talas::audio::makeWaveformOverview(
        request,
        48000,
        channels);
    require(result.ok(), "valid immutable channels should build an overview");
    static_assert(std::is_same_v<
        decltype(result.overview),
        std::shared_ptr<const talas::audio::WaveformOverview>>);
    require(
        result.overview->assetId == "asset-a"
            && result.overview->generation == 7
            && result.overview->sampleRate == 48000
            && result.overview->channelCount == 2
            && result.overview->frameCount == 8
            && result.overview->buckets.size() == 3,
        "overview should retain the immutable generation identity and bounded shape");

    const auto& first = result.overview->buckets[0];
    const auto& middle = result.overview->buckets[1];
    const auto& last = result.overview->buckets[2];
    require(
        first.firstFrame == 0 && first.endFrameExclusive == 2
            && first.channels[0].minimum == -0.5
            && first.channels[0].maximum == 0.5,
        "first bucket should cover exact source frames with exact extrema");
    require(
        middle.firstFrame == 2 && middle.endFrameExclusive == 5
            && middle.channels[0].minimum == -0.25
            && middle.channels[0].maximum == 0.75,
        "remainder frames should be distributed deterministically");
    require(
        last.firstFrame == 5 && last.endFrameExclusive == 8
            && last.channels[0].minimum == -1.0
            && last.channels[0].maximum == 1.0
            && last.channels[1].minimum == 0.5
            && last.channels[1].maximum == 0.7,
        "last bucket should include the exact immutable generation tail");
}

void testOverviewRejectsInvalidShapeAndCancelsWithoutPublication()
{
    const talas::audio::WaveformOverviewRequest request{"asset", 1, 128};
    const auto mismatch = talas::audio::makeWaveformOverview(
        request,
        48000,
        {{0.0, 1.0}, {0.0}});
    require(
        !mismatch.ok() && !mismatch.cancelled,
        "mismatched channel lengths should fail closed");

    int checks = 0;
    const auto cancelled = talas::audio::makeWaveformOverview(
        request,
        48000,
        {std::vector<double>(10000, 0.25)},
        [&] { return ++checks == 2; });
    require(
        !cancelled.ok() && cancelled.cancelled
            && !cancelled.overview,
        "cancellation should publish no partial overview");
}

void testFileOverviewCarriesGenerationTagAndDiscardsStalePublication()
{
    const auto path = temporaryWavPath();
    const std::vector<std::vector<double>> channels{
        {-1.0, -0.25, 0.25, 1.0},
    };
    std::string error;
    require(
        talas::audio::writePcm16Wav(path, 44100.0, channels, error),
        "test should write an original PCM16 overview fixture: " + error);

    const talas::audio::WaveformOverviewRequest request{
        "asset-file",
        12,
        2,
    };
    const auto result = talas::audio::loadPcm16WaveformOverview(path, request);
    auto mismatchedRequest = request;
    mismatchedRequest.contentHash = std::string(64, '0');
    const auto mismatched = talas::audio::loadPcm16WaveformOverview(
        path,
        mismatchedRequest);
    std::filesystem::remove(path);
    require(
        result.ok()
            && result.overview->sampleRate == 44100
            && result.overview->frameCount == 4
            && !result.overview->contentHash.empty(),
        "file overview should decode off-thread input into an inert immutable projection");
    require(
        !mismatched.ok() && !mismatched.cancelled,
        "waveform loading should reject changed bytes instead of mis-tagging a stale immutable generation");
    require(
        talas::audio::waveformOverviewMatches(
            *result.overview,
            "asset-file",
            12)
            && !talas::audio::waveformOverviewMatches(
                *result.overview,
                "asset-file",
                13),
        "generation-tagged publication should reject a stale completed load");
}

void testLargeSparseFileUsesBoundedStreamingMemory()
{
    const auto path = temporaryWavPath();
    constexpr std::uint32_t frameCount = 4U * 1024U * 1024U;
    writeSparseStereoPcm16Wav(path, frameCount);
    const talas::audio::WaveformOverviewRequest request{
        "large-sparse",
        3,
        64,
    };
    auto cancellationChecks = 0;
    const auto cancelled = talas::audio::loadPcm16WaveformOverview(
        path,
        request,
        [&] { return ++cancellationChecks == 3; });
    require(
        !cancelled.ok()
            && cancelled.cancelled
            && !cancelled.overview,
        "large valid WAV cancellation should publish no partial overview");
    talas::audio::WaveformOverviewResult result;
    {
        ScopedSingleAllocationLimit boundedAllocation{512U * 1024U};
        result = talas::audio::loadPcm16WaveformOverview(path, request);
    }
    std::filesystem::remove(path);
    require(
        result.ok()
            && result.overview->frameCount == frameCount
            && result.overview->channelCount == 2
            && result.overview->buckets.size() == 64,
        "large valid WAV overview should stream through bounded memory into bounded buckets");
}
}

int main()
{
    testOverviewBuildsExactBoundedMinMaxBuckets();
    testOverviewRejectsInvalidShapeAndCancelsWithoutPublication();
    testFileOverviewCarriesGenerationTagAndDiscardsStalePublication();
    testLargeSparseFileUsesBoundedStreamingMemory();
    return 0;
}
