#include "talas/audio/RealtimeOutputTake.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <semaphore>
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

std::filesystem::path temporaryDirectory()
{
    static int sequence = 0;
    auto path = std::filesystem::temp_directory_path()
        / ("talas-realtime-output-take-test-"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count())
           + "-" + std::to_string(sequence++));
    require(std::filesystem::create_directory(path),
        "test should create a private directory");
    return path;
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::uint16_t readU16(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes.at(offset))
        | static_cast<std::uint16_t>(bytes.at(offset + 1) << 8);
}

std::uint32_t readU32(
    const std::vector<unsigned char>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes.at(offset))
        | (static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8)
        | (static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16)
        | (static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24);
}

void testRecordsExactOutputAndPublishesAfterStop()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {
            destination,
            talas::audio::ExistingAudioFilePolicy::Reject,
            8000.0,
            2,
            4,
            4,
            11,
        },
        error);
    require(take != nullptr, "valid output take should start: " + error);
    std::array<float, 3> left{0.0F, 0.5F, -0.5F};
    std::array<float, 3> right{1.0F, -1.0F, 0.25F};
    std::array<float*, 2> channels{left.data(), right.data()};
    require(take->publishFromAudio(channels.data(), 2, 3, 11),
        "audio callback should admit exact rendered block");
    require(!std::filesystem::exists(destination),
        "destination must remain unpublished while recording");
    take->requestStop();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Completed
            && snapshot.writtenFrames == 3
            && snapshot.sampleRate == 8000.0
            && snapshot.channelCount == 2
            && !snapshot.incomplete,
        "completed take should expose exact metadata and complete state: "
            + snapshot.diagnostic);
    take->requestStop();
    take->requestCancel();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Completed,
        "terminal completion must not regress to a transient lifecycle state");
    const auto bytes = readBytes(destination);
    require(readU16(bytes, 22) == 2 && readU32(bytes, 24) == 8000
            && readU32(bytes, 40) == 12,
        "published take should contain exact channel/rate/frame metadata");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testLengthLimitPublishesTheRecordedPrefix()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "limited.wav";
    talas::audio::RealtimeOutputTakeOptions options{
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        8000.0,
        2,
        4,
        4,
        11,
    };
    options.maximumFrames = 5;
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(options, error);
    require(take != nullptr, "limited output take should start: " + error);
    std::array<float, 3> left{0.25F, 0.25F, 0.25F};
    std::array<float, 3> right{-0.25F, -0.25F, -0.25F};
    std::array<float*, 2> channels{left.data(), right.data()};
    for (int block = 0; block < 3; ++block)
        (void)take->publishFromAudio(channels.data(), 2, 3, 11);
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Completed
            && snapshot.writtenFrames == 5 && !snapshot.incomplete,
        "a take that reaches its length limit must publish what it recorded: "
            + snapshot.diagnostic);
    const auto bytes = readBytes(destination);
    require(readU32(bytes, 40) == 20,
        "the published take should hold exactly the frames within the limit");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testCancelNeverPublishesDestination()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "cancelled.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {
            destination,
            talas::audio::ExistingAudioFilePolicy::Reject,
            48000.0,
            1,
            4,
            4,
            12,
        },
        error);
    require(take != nullptr, "cancel fixture should start: " + error);
    std::array<float, 2> samples{0.1F, 0.2F};
    std::array<float*, 1> channels{samples.data()};
    require(take->publishFromAudio(channels.data(), 1, 2, 12),
        "cancel fixture should admit one block");
    take->requestCancel();
    take->join();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Cancelled,
        "cancel should publish terminal cancelled state");
    take->requestStop();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Cancelled,
        "terminal cancellation must not regress to Stopping");
    require(!std::filesystem::exists(destination),
        "cancel must not publish a destination");
    require(std::filesystem::is_empty(directory),
        "cancel should remove only its private staging artifacts");
    std::filesystem::remove(directory);
}

void testCancelAfterStopRequestWinsBeforePublicationAdmission()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "stop-then-cancel.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {
            destination,
            talas::audio::ExistingAudioFilePolicy::Reject,
            48000.0,
            8,
            4096,
            32,
            13,
        },
        error);
    require(take != nullptr,
        "stop/cancel fixture should start: " + error);
    std::array<std::vector<float>, 8> storage;
    std::array<float*, 8> channels{};
    for (std::size_t channel = 0; channel < storage.size(); ++channel)
    {
        storage[channel].assign(4096, static_cast<float>(channel) / 8.0F);
        channels[channel] = storage[channel].data();
    }
    for (std::size_t block = 0; block < 32; ++block)
    {
        (void)take->publishFromAudio(
            channels.data(), channels.size(), storage.front().size(), 13);
    }
    take->requestStop();
    take->requestCancel();
    take->join();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Cancelled,
        "cancel requested before publication admission should win over stop");
    require(!std::filesystem::exists(destination),
        "stop/cancel race must not publish a destination");
    require(std::filesystem::is_empty(directory),
        "stop/cancel race should clean private staging");
    std::filesystem::remove(directory);
}

void testExistingDestinationPolicyIsExact()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "existing.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "original";
    }
    std::string error;
    auto rejected = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         48000.0,
         1,
         4,
         4,
         14},
        error);
    require(rejected == nullptr
            && error.find("explicit replacement") != std::string::npos,
        "existing destination should reject without explicit replacement");
    require(readBytes(destination).size() == 8,
        "rejected take should preserve original bytes");

    auto replacement = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Replace,
         48000.0,
         1,
         4,
         4,
         14},
        error);
    require(replacement != nullptr,
        "explicit replacement take should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(replacement->publishFromAudio(channels.data(), 1, 1, 14),
        "replacement should admit one frame");
    replacement->requestStop();
    replacement->join();
    require(replacement->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Completed,
        "explicit replacement should complete");
    require(readBytes(destination).size() == 46,
        "explicit replacement should install the finalized WAV");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testRejectsSampleRateThatCannotRoundTripExactly()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "fractional-rate.wav";
    std::string error;
    auto fractional = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         48000.5,
         1,
         4,
         4,
         15},
        error);
    require(fractional == nullptr
            && error.find("exact positive integer WAV sample rate")
                != std::string::npos,
        "fractional rate should reject before metadata can diverge");

    auto oversized = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         static_cast<double>(std::numeric_limits<std::uint32_t>::max())
             + 1.0,
         1,
         4,
         4,
         15},
        error);
    require(oversized == nullptr
            && error.find("exact positive integer WAV sample rate")
                != std::string::npos,
        "rate beyond the WAV field should reject before recorder admission");
    require(std::filesystem::is_empty(directory),
        "invalid sample rates must not create staging artifacts");
    std::filesystem::remove(directory);
}

void testRejectedBlockRemainsExactIncompleteEvidence()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "rejected-block.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve rejected destination";
    }
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Replace,
         48000.0,
         1,
         4,
         4,
         16},
        error);
    require(take != nullptr,
        "rejected-block fixture should start: " + error);
    std::array<float, 1> invalid{
        std::numeric_limits<float>::quiet_NaN()};
    std::array<float*, 1> invalidChannels{invalid.data()};
    require(!take->publishFromAudio(invalidChannels.data(), 1, 1, 16),
        "non-finite callback data must reject rather than enter the file");
    std::array<float, 1> valid{0.25F};
    std::array<float*, 1> validChannels{valid.data()};
    require(take->publishFromAudio(validChannels.data(), 1, 1, 16),
        "a later exact block should remain recordable");
    take->requestStop();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Failed
            && snapshot.incomplete
            && snapshot.droppedBlocks == 0
            && snapshot.rejectedBlocks == 1
            && snapshot.writtenFrames == 1
            && snapshot.diagnostic.find("incomplete") != std::string::npos,
        "rejected callback evidence should fail the incomplete take before publication");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'r', 'e', 'j', 'e', 'c', 't', 'e', 'd', ' ',
                'd', 'e', 's', 't', 'i', 'n', 'a', 't', 'i', 'o', 'n'},
        "rejected callback evidence must preserve existing destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testDroppedBlockPreventsPublicationAndPreservesExistingDestination()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "dropped-block.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve dropped destination";
    }
    std::binary_semaphore workerReady{0};
    std::binary_semaphore releaseWorker{0};
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    options.sampleRate = 48000.0;
    options.channelCount = 1;
    options.maxBlockFrames = 4;
    options.queueCapacity = 2;
    options.sourceStageId = 17;
    options.beforeRecorderDrainForTesting = [&] {
        workerReady.release();
        releaseWorker.acquire();
    };
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take != nullptr,
        "dropped-block fixture should start: " + error);
    require(workerReady.try_acquire_for(std::chrono::seconds(5)),
        "dropped-block worker should pause before draining");
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 17)
            && take->publishFromAudio(channels.data(), 1, 1, 17),
        "bounded queue should admit its exact capacity");
    require(!take->publishFromAudio(channels.data(), 1, 1, 17),
        "first block beyond queue capacity should be dropped");
    take->requestStop();
    releaseWorker.release();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Failed
            && snapshot.incomplete
            && snapshot.droppedBlocks == 1
            && snapshot.droppedFrames == 1
            && snapshot.rejectedBlocks == 0
            && snapshot.writtenFrames == 2
            && snapshot.diagnostic.find("incomplete") != std::string::npos,
        "queue overflow should fail the incomplete take before publication");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'd', 'r', 'o', 'p', 'p', 'e', 'd', ' ',
                'd', 'e', 's', 't', 'i', 'n', 'a', 't', 'i', 'o', 'n'},
        "queue overflow must preserve existing destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testStageReplacementCancelsWithoutPublishingMixedTake()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "stage-replaced.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         48000.0,
         1,
         4,
         4,
         21},
        error);
    require(take != nullptr,
        "stage replacement fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 21),
        "source stage should admit one exact block");
    require(!take->publishFromAudio(channels.data(), 1, 1, 22),
        "replacement stage must fail closed before any mixed block is queued");
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.sourceStageId == 21
            && snapshot.rejectedBlocks == 1
            && snapshot.incomplete
            && snapshot.diagnostic.find("active render stage changed")
                != std::string::npos
            && !std::filesystem::exists(destination),
        "stage mismatch must cancel the private take with exact evidence and no publication");
    require(std::filesystem::is_empty(directory),
        "stage mismatch cancellation should remove private staging");
    std::filesystem::remove(directory);
}

void testRenderFailureRejectsZerosAndPreservesExistingDestination()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "render-failed.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve render destination";
    }
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Replace,
         48000.0,
         1,
         4,
         4,
         23},
        error);
    require(take != nullptr,
        "render-failure fixture should start: " + error);
    std::array<float, 1> source{0.25F};
    std::array<float*, 1> channels{source.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 23),
        "render-failure fixture should admit one prior exact block");
    take->rejectRenderFailureFromAudio();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Failed
            && snapshot.incomplete
            && snapshot.rejectedBlocks == 1
            && snapshot.diagnostic.find("source render failed")
                != std::string::npos,
        "source render failure should fail closed with exact incomplete evidence");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'r', 'e', 'n', 'd', 'e', 'r', ' ',
                'd', 'e', 's', 't', 'i', 'n', 'a', 't', 'i', 'o', 'n'},
        "source render failure must preserve existing destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testProjectAuthorityInvalidationPreservesExistingDestination()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "authority-invalidated.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "existing destination";
    }
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Replace,
         48000.0,
         1,
         4,
         4,
         31},
        error);
    require(take != nullptr,
        "authority invalidation fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 31),
        "authority invalidation fixture should admit one private block");
    take->requestAuthorityInvalidation();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.diagnostic.find("project authority changed")
                != std::string::npos,
        "project authority invalidation should retain one typed terminal cause");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'e', 'x', 'i', 's', 't', 'i', 'n', 'g', ' ',
                'd', 'e', 's', 't', 'i', 'n', 'a', 't', 'i', 'o', 'n'},
        "project authority invalidation must preserve existing destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testAudioAdmissionRejectsStaleProjectAuthority()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "stale-authority.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         48000.0,
         1,
         4,
         4,
         41,
         7},
        error);
    require(take != nullptr,
        "authority-bound fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(!take->publishFromAudio(
                channels.data(), 1, 1, 41, 8),
        "audio admission must reject a stale project authority generation");
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.sourceAuthorityGeneration == 7
            && snapshot.rejectedBlocks == 1
            && snapshot.diagnostic.find("project authority changed")
                != std::string::npos
            && !std::filesystem::exists(destination),
        "stale authority rejection must cancel without publishing mixed bytes");
    std::filesystem::remove(directory);
}

void testAudioAdmissionRejectsStaleDocumentAuthority()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "stale-document-authority.wav";
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        {destination,
         talas::audio::ExistingAudioFilePolicy::Reject,
         48000.0,
         1,
         4,
         4,
         42,
         9,
         11},
        error);
    require(take != nullptr,
        "document-authority fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(!take->publishFromAudio(
                channels.data(), 1, 1, 42, 9, 12),
        "audio admission must reject a stale document authority generation");
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.sourceAuthorityGeneration == 9
            && snapshot.sourceDocumentAuthorityGeneration == 11
            && snapshot.rejectedBlocks == 1
            && snapshot.diagnostic.find("project authority changed")
                != std::string::npos
            && !std::filesystem::exists(destination),
        "stale document authority must cancel without publishing mixed bytes");
    std::filesystem::remove(directory);
}

void testAuthorityMismatchUpgradesPendingStopToCancellation()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "stop-then-authority-change.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve";
    }
    std::string error;
    std::binary_semaphore beforeAdmission{0};
    std::binary_semaphore releaseAdmission{0};
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    options.sampleRate = 48000.0;
    options.channelCount = 8;
    options.maxBlockFrames = 4096;
    options.queueCapacity = 32;
    options.sourceStageId = 51;
    options.sourceAuthorityGeneration = 13;
    options.sourceDocumentAuthorityGeneration = 17;
    options.beforePublicationAdmissionForTesting = [&] {
        beforeAdmission.release();
        releaseAdmission.acquire();
    };
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take != nullptr,
        "stop/authority fixture should start: " + error);
    std::array<std::vector<float>, 8> storage;
    std::array<float*, 8> channels{};
    for (std::size_t channel = 0; channel < storage.size(); ++channel)
    {
        storage[channel].assign(4096, 0.125F);
        channels[channel] = storage[channel].data();
    }
    for (std::size_t block = 0; block < 32; ++block)
    {
        (void)take->publishFromAudio(
            channels.data(), channels.size(), 4096, 51, 13, 17);
    }
    take->requestStop();
    if (!beforeAdmission.try_acquire_for(std::chrono::seconds(5)))
    {
        releaseAdmission.release();
        require(false,
            "stop/authority fixture did not reach publication admission");
    }
    require(!take->publishFromAudio(
                channels.data(), channels.size(), 1, 51, 13, 18),
        "authority mismatch after Stop must be observed before publication");
    releaseAdmission.release();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.diagnostic.find("project authority changed")
                != std::string::npos
            && readBytes(destination)
                == std::vector<unsigned char>{
                    'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'},
        "authority mismatch must upgrade pending Stop and preserve destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testAuthorityInvalidationAfterPublicationAdmissionWinsBeforeCommit()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "admitted-then-revoked.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve after admission";
    }
    std::binary_semaphore admitted{0};
    std::binary_semaphore releasePublication{0};
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    options.sampleRate = 48000.0;
    options.channelCount = 1;
    options.maxBlockFrames = 4;
    options.queueCapacity = 4;
    options.sourceStageId = 61;
    options.afterPublicationAdmissionForTesting = [&] {
        admitted.release();
        releasePublication.acquire();
    };
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take != nullptr,
        "post-admission revocation fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 61),
        "post-admission revocation fixture should admit one block");
    take->requestStop();
    require(admitted.try_acquire_for(std::chrono::seconds(5)),
        "worker should pause after publication admission");
    take->requestAuthorityInvalidation();
    releasePublication.release();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Cancelled
            && snapshot.diagnostic.find("project authority changed")
                != std::string::npos,
        "revocation after admission must win before the publication commit point");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'a', 'f', 't', 'e', 'r', ' ',
                'a', 'd', 'm', 'i', 's', 's', 'i', 'o', 'n'},
        "post-admission revocation must preserve destination bytes");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testStagingLeafSwapCannotPublishReplacementBytes()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "leaf-swap.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve leaf swap";
    }
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    options.sampleRate = 48000.0;
    options.channelCount = 1;
    options.maxBlockFrames = 4;
    options.queueCapacity = 4;
    options.sourceStageId = 62;
    options.beforePublicationAdmissionForTesting = [&] {
        std::filesystem::path staged;
        for (const auto& sibling :
             std::filesystem::directory_iterator(directory))
        {
            if (!sibling.is_directory()
                || sibling.path() == destination)
            {
                continue;
            }
            for (const auto& entry :
                 std::filesystem::directory_iterator(sibling.path()))
            {
                if (entry.is_regular_file())
                {
                    staged = entry.path();
                    break;
                }
            }
        }
        require(!staged.empty(),
            "leaf-swap fixture should find the private staged file");
        std::filesystem::rename(staged, staged.string() + ".held");
        std::ofstream replacement(staged, std::ios::binary);
        replacement << "malicious replacement";
    };
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take != nullptr,
        "leaf-swap fixture should start: " + error);
    std::array<float, 1> sample{0.25F};
    std::array<float*, 1> channels{sample.data()};
    require(take->publishFromAudio(channels.data(), 1, 1, 62),
        "leaf-swap fixture should admit one exact block");
    take->requestStop();
    take->join();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Failed,
        "replaced staging pathname must fail publication");
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'l', 'e', 'a', 'f', ' ', 's', 'w', 'a', 'p'},
        "staging leaf replacement must preserve destination bytes");
    std::filesystem::remove_all(directory);
}

void testFailureAfterSecureOpenClosesAndCleansStagingAuthority()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "open-then-fail.wav";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "preserve open failure";
    }
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    options.sampleRate = 48000.0;
    options.channelCount = 1;
    options.maxBlockFrames = 4;
    options.queueCapacity = 4;
    options.sourceStageId = 64;
    options.afterTemporaryFileOpenForTesting = [] {
        throw std::bad_alloc{};
    };
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take != nullptr,
        "post-open failure fixture should start: " + error);
    take->join();
    require(take->snapshot().state
            == talas::audio::RealtimeOutputTakeState::Failed,
        "post-open allocation failure must fail the take");
    take.reset();
    require(readBytes(destination)
            == std::vector<unsigned char>{
                'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', ' ',
                'o', 'p', 'e', 'n', ' ', 'f', 'a', 'i', 'l', 'u', 'r', 'e'},
        "post-open failure must preserve destination bytes");
    require(std::distance(
                std::filesystem::directory_iterator(directory),
                std::filesystem::directory_iterator{}) == 1,
        "post-open failure must close and remove private staging authority");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

#if !defined(_WIN32)
void testResolvedDestinationCannotMoveIntoProtectedDirectory()
{
    const auto directory = temporaryDirectory();
    const auto safe = directory / "safe";
    const auto protectedDirectory = directory / "protected";
    const auto selectedParent = directory / "selected";
    require(std::filesystem::create_directory(safe)
            && std::filesystem::create_directory(protectedDirectory),
        "protected-path fixture should create both parents");
    std::filesystem::create_directory_symlink(safe, selectedParent);
    const auto destination = selectedParent / "take.wav";
    require(!talas::audio::audioFileDestinationConflicts(
                destination,
                std::span<const std::filesystem::path>{},
                std::span<const std::filesystem::path>{
                    &protectedDirectory, 1}),
        "initial chooser-time path should be outside protected authority");
    std::filesystem::remove(selectedParent);
    std::filesystem::create_directory_symlink(
        protectedDirectory, selectedParent);

    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Reject;
    options.sampleRate = 48000.0;
    options.channelCount = 1;
    options.maxBlockFrames = 4;
    options.queueCapacity = 4;
    options.sourceStageId = 63;
    options.protectedDirectories = {protectedDirectory};
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(
        std::move(options), error);
    require(take == nullptr
            && error.find("protected project authority")
                != std::string::npos,
        "secure post-prepare validation must reject a symlink flip into protected authority");
    require(std::filesystem::is_empty(protectedDirectory),
        "protected destination must remain unchanged");
    std::filesystem::remove_all(directory);
}
#endif
}

// Record to disk records stereo or monaural, at 8,
// 16 or 24 bits; in monaural only the selected Sound's left channel goes to
// the file.
void testRecordToDiskTakesItsChannelsAndWordSize()
{
    for (const auto bits : {std::uint16_t{8}, std::uint16_t{16}, std::uint16_t{24}})
    {
        for (const std::size_t channelsKept : {std::size_t{1}, std::size_t{2}})
        {
            const auto directory = temporaryDirectory();
            const auto destination = directory / "take.wav";
            std::string error;
            talas::audio::RealtimeOutputTakeOptions options{destination, talas::audio::ExistingAudioFilePolicy::Reject, 8000.0,
                channelsKept, 4, 4, 11};
            options.wordSize = talas::audio::WavWordSize{bits};
            auto take = talas::audio::RealtimeOutputTake::start(std::move(options), error);
            require(take != nullptr, "the take should start: " + error);
            std::array<float, 3> left{0.0F, 0.5F, -0.5F};
            std::array<float, 3> right{1.0F, -1.0F, 0.25F};
            std::array<float*, 2> channels{left.data(), right.data()};
            require(take->publishFromAudio(channels.data(), channelsKept, 3, 11), "the take should admit the block");
            take->requestStop();
            take->join();
            require(take->snapshot().state == talas::audio::RealtimeOutputTakeState::Completed, "the take should complete");
            const auto bytes = readBytes(destination);
            const auto width = bits / 8U;
            require(readU16(bytes, 22) == channelsKept && readU16(bytes, 34) == bits
                    && readU32(bytes, 40) == 3 * width * channelsKept,
                "the file should hold the channels and bits asked for");
            if (channelsKept == 1 && bits == 16)
            {
                require(static_cast<std::int16_t>(readU16(bytes, 46)) == 16384 && static_cast<std::int16_t>(readU16(bytes, 48)) == -16384,
                    "a monaural take should hold the left channel");
            }
            std::filesystem::remove(destination);
            std::filesystem::remove(directory);
        }
    }
}

int main()
{
    testRecordsExactOutputAndPublishesAfterStop();
    testRecordToDiskTakesItsChannelsAndWordSize();
    testLengthLimitPublishesTheRecordedPrefix();
    testCancelNeverPublishesDestination();
    testCancelAfterStopRequestWinsBeforePublicationAdmission();
    testExistingDestinationPolicyIsExact();
    testRejectsSampleRateThatCannotRoundTripExactly();
    testRejectedBlockRemainsExactIncompleteEvidence();
    testDroppedBlockPreventsPublicationAndPreservesExistingDestination();
    testStageReplacementCancelsWithoutPublishingMixedTake();
    testRenderFailureRejectsZerosAndPreservesExistingDestination();
    testProjectAuthorityInvalidationPreservesExistingDestination();
    testAudioAdmissionRejectsStaleProjectAuthority();
    testAudioAdmissionRejectsStaleDocumentAuthority();
    testAuthorityMismatchUpgradesPendingStopToCancellation();
    testAuthorityInvalidationAfterPublicationAdmissionWinsBeforeCommit();
    testStagingLeafSwapCannotPublishReplacementBytes();
    testFailureAfterSecureOpenClosesAndCleansStagingAuthority();
#if !defined(_WIN32)
    testResolvedDestinationCannotMoveIntoProtectedDirectory();
#endif
    return 0;
}
