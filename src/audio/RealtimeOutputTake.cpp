#include "talas/audio/RealtimeOutputTake.h"

#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectPathEncoding.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
inline constexpr std::uint8_t stopRequest = 1;
inline constexpr std::uint8_t cancelRequest = 2;
inline constexpr std::uint8_t publishingRequest = 3;
}

RealtimeOutputTake::RealtimeOutputTake(
    RealtimeOutputTakeOptions options,
    std::unique_ptr<AudioFilePublication> publication,
    std::unique_ptr<talas::runtime::RealtimeOutputRecorder> recorder)
    : options_(std::move(options))
    , publication_(std::move(publication))
    , recorder_(std::move(recorder))
{
}

std::unique_ptr<RealtimeOutputTake> RealtimeOutputTake::start(
    RealtimeOutputTakeOptions options,
    std::string& error)
{
    error.clear();
    if (!std::isfinite(options.sampleRate)
        || options.sampleRate <= 0.0
        || options.sampleRate
            > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max())
        || options.sampleRate != std::trunc(options.sampleRate))
    {
        error = "Output take requires an exact positive integer WAV sample rate";
        return nullptr;
    }
    if (options.sourceStageId == 0
        || options.sourceAuthorityGeneration == 0
        || options.sourceDocumentAuthorityGeneration == 0)
    {
        error = "Output take requires one exact nonzero source stage and authority generations";
        return nullptr;
    }
    std::unique_ptr<talas::runtime::RealtimeOutputRecorder> recorder;
    try
    {
        recorder = std::make_unique<talas::runtime::RealtimeOutputRecorder>(
            options.channelCount,
            options.maxBlockFrames,
            options.queueCapacity);
    }
    catch (const std::exception& exception)
    {
        error = "Unable to allocate the bounded output recorder: ";
        error += exception.what();
        return nullptr;
    }
    catch (...)
    {
        error = "Unable to allocate the bounded output recorder";
        return nullptr;
    }
    if (!recorder->ready())
    {
        error = "Output take device profile exceeds the closed recorder limits";
        return nullptr;
    }

    auto publication = AudioFilePublication::prepare(
        options.destination,
        options.existingPolicy,
        error);
    if (!publication)
        return nullptr;
    if (audioFileDestinationConflicts(
            publication->destination(),
            options.protectedFiles,
            options.protectedDirectories))
    {
        error = "Output take destination conflicts with protected project authority";
        return nullptr;
    }
    options.destination = publication->destination();

    auto take = std::unique_ptr<RealtimeOutputTake>{
        new RealtimeOutputTake{
            std::move(options),
            std::move(publication),
            std::move(recorder)}};
    if (!take->recorder_->startAccepting())
    {
        error = "Unable to open output-recorder admission";
        return nullptr;
    }
    try
    {
        take->worker_ = std::jthread(
            [raw = take.get()](std::stop_token token) {
                raw->run(token);
            });
    }
    catch (const std::exception& exception)
    {
        take->recorder_->stopAccepting();
        error = "Unable to start output-recorder worker: ";
        error += exception.what();
        return nullptr;
    }
    catch (...)
    {
        take->recorder_->stopAccepting();
        error = "Unable to start output-recorder worker";
        return nullptr;
    }
    return take;
}

RealtimeOutputTake::~RealtimeOutputTake()
{
    requestCancel();
    join();
}

bool RealtimeOutputTake::publishFromAudio(
    float* const* channels,
    std::size_t channelCount,
    std::size_t frameCount,
    std::uint64_t activeStageId,
    std::uint64_t activeAuthorityGeneration,
    std::uint64_t activeDocumentAuthorityGeneration) noexcept
{
    const auto stageMismatch = activeStageId == 0
        || activeStageId != options_.sourceStageId;
    const auto authorityMismatch = activeAuthorityGeneration == 0
        || activeAuthorityGeneration
            != options_.sourceAuthorityGeneration
        || activeDocumentAuthorityGeneration == 0
        || activeDocumentAuthorityGeneration
            != options_.sourceDocumentAuthorityGeneration;
    if (stageMismatch || authorityMismatch)
    {
        rejectFromAudio(stageMismatch, authorityMismatch, false);
        return false;
    }
    return recorder_->tryPublish(channels, channelCount, frameCount);
}

void RealtimeOutputTake::rejectRenderFailureFromAudio() noexcept
{
    rejectFromAudio(false, false, true);
}

void RealtimeOutputTake::rejectFromAudio(
    bool stageMismatch,
    bool authorityMismatch,
    bool renderFailure) noexcept
{
    auto rejected = stageRejectedBlocks_.load(std::memory_order_relaxed);
    if (rejected != std::numeric_limits<std::uint64_t>::max())
    {
        stageRejectedBlocks_.store(
            rejected + 1, std::memory_order_relaxed);
    }
    if (stageMismatch)
        stageBindingRejected_.store(true, std::memory_order_release);
    if (authorityMismatch)
        authorityBindingRejected_.store(true, std::memory_order_release);
    if (renderFailure)
        renderFailureRejected_.store(true, std::memory_order_release);
    auto request = request_.load(std::memory_order_acquire);
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        if (request == cancelRequest || request == publishingRequest)
            break;
        if (request_.compare_exchange_strong(
                request,
                cancelRequest,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            break;
        }
    }
    recorder_->stopAccepting();
    auto recording = RealtimeOutputTakeState::Recording;
    (void)state_.compare_exchange_strong(
        recording,
        RealtimeOutputTakeState::Stopping,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

std::size_t RealtimeOutputTake::channelCount() const noexcept
{
    return options_.channelCount;
}

void RealtimeOutputTake::requestStop() noexcept
{
    std::uint8_t expected = 0;
    if (request_.compare_exchange_strong(
            expected,
            stopRequest,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        auto recording = RealtimeOutputTakeState::Recording;
        (void)state_.compare_exchange_strong(
            recording,
            RealtimeOutputTakeState::Stopping,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    recorder_->stopAccepting();
}

void RealtimeOutputTake::requestCancel() noexcept
{
    bool committed = false;
    {
        std::lock_guard lock(publicationMutex_);
        committed = publicationCommitted_;
        if (!committed)
        {
            publicationRevoked_ = true;
            auto request = request_.load(std::memory_order_acquire);
            while (request != cancelRequest)
            {
                if (request_.compare_exchange_weak(
                        request,
                        cancelRequest,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    break;
                }
            }
        }
    }
    recorder_->stopAccepting();
    if (!committed && worker_.joinable())
        worker_.request_stop();
}

void RealtimeOutputTake::requestAuthorityInvalidation() noexcept
{
    authorityBindingRejected_.store(true, std::memory_order_release);
    requestCancel();
}

void RealtimeOutputTake::join()
{
    if (worker_.joinable())
        worker_.join();
}

RealtimeOutputTakeSnapshot RealtimeOutputTake::snapshot() const
{
    const auto recorder = recorder_->snapshot();
    std::lock_guard lock(resultMutex_);
    const auto state = state_.load(std::memory_order_acquire);
    const auto stageRejected = stageRejectedBlocks_.load(
        std::memory_order_relaxed);
    const auto rejected = recorder.rejectedBlocks
            > std::numeric_limits<std::uint64_t>::max() - stageRejected
        ? std::numeric_limits<std::uint64_t>::max()
        : recorder.rejectedBlocks + stageRejected;
    return {
        state,
        options_.destination,
        options_.sampleRate,
        options_.channelCount,
        options_.sourceStageId,
        options_.sourceAuthorityGeneration,
        options_.sourceDocumentAuthorityGeneration,
        state == RealtimeOutputTakeState::Recording
            || state == RealtimeOutputTakeState::Stopping
            ? liveWrittenFrames_.load(std::memory_order_acquire)
            : writtenFrames_,
        recorder.droppedBlocks,
        recorder.droppedFrames,
        rejected,
        recorder.incomplete() || stageRejected != 0,
        diagnostic_,
    };
}

void RealtimeOutputTake::setTerminal(
    RealtimeOutputTakeState state,
    std::string diagnostic,
    std::uint64_t writtenFrames) noexcept
{
    try
    {
        std::lock_guard lock(resultMutex_);
        diagnostic_ = std::move(diagnostic);
        writtenFrames_ = writtenFrames;
        state_.store(state, std::memory_order_release);
        return;
    }
    catch (...)
    {
        // Terminal state remains truthful even if diagnostic storage fails.
    }
    state_.store(state, std::memory_order_release);
}

void RealtimeOutputTake::run(std::stop_token stopToken) noexcept
{
    try
    {
        std::string openError;
        std::unique_ptr<std::FILE, decltype(&std::fclose)> output{
            publication_->openTemporaryFile(openError),
            &std::fclose};
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
        if (options_.afterTemporaryFileOpenForTesting)
            options_.afterTemporaryFileOpenForTesting();
#endif
        auto diagnosticPath =
            talas::project::pathToUtf8(publication_->temporaryPath());
        Pcm16WavStreamWriter writer(
            output.get(),
            std::move(diagnosticPath),
            options_.sampleRate,
            options_.channelCount,
            options_.wordSize);
        (void)output.release();
        if (!writer.ready())
        {
            recorder_->stopAccepting();
            writer.abandon();
            std::string cleanupError;
            (void)publication_->discard(cleanupError);
            auto diagnostic = openError.empty()
                ? std::string(writer.error())
                : std::move(openError);
            if (!cleanupError.empty())
                diagnostic += "; " + cleanupError;
            setTerminal(
                RealtimeOutputTakeState::Failed,
                std::move(diagnostic),
                0);
            return;
        }

        std::vector<float> interleaved(
            options_.channelCount * options_.maxBlockFrames,
            0.0F);
        bool lengthLimitReached = false;
        talas::runtime::RealtimeOutputRecorderBlock block;
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
        if (options_.beforeRecorderDrainForTesting)
            options_.beforeRecorderDrainForTesting();
#endif
        while (true)
        {
            const auto request = request_.load(std::memory_order_acquire);
            if (request == cancelRequest || stopToken.stop_requested())
            {
                recorder_->stopAccepting();
                if (!recorder_->producerQuiescent())
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                    continue;
                }
                (void)recorder_->discardPending();
                writer.abandon();
                std::string cleanupError;
                (void)publication_->discard(cleanupError);
                const auto renderFailed = renderFailureRejected_.load(
                    std::memory_order_acquire);
                setTerminal(
                    renderFailed
                        ? RealtimeOutputTakeState::Failed
                        : RealtimeOutputTakeState::Cancelled,
                    stageBindingRejected_.load(std::memory_order_acquire)
                        ? (cleanupError.empty()
                            ? "Output take cancelled: active render stage changed"
                            : "Output take cancelled: active render stage changed; "
                                + cleanupError)
                        : authorityBindingRejected_.load(
                              std::memory_order_acquire)
                            ? (cleanupError.empty()
                                ? "Output take cancelled: project authority changed"
                                : "Output take cancelled: project authority changed; "
                                + cleanupError)
                        : renderFailed
                            ? (cleanupError.empty()
                                ? "Output take incomplete: source render failed; destination was not published"
                                : "Output take incomplete: source render failed; destination was not published; "
                                    + cleanupError)
                        : (cleanupError.empty()
                            ? "Output take cancelled"
                            : "Output take cancelled; " + cleanupError),
                    writer.framesWritten());
                return;
            }

            const auto consumed = recorder_->tryConsume(
                interleaved,
                block);
            if (consumed
                == talas::runtime::RealtimeOutputRecorderConsumeStatus::Accepted)
            {
                // Blocks queued before the take stopped at its length limit.
                if (lengthLimitReached)
                    continue;
                auto capacity = static_cast<std::uint64_t>(writer.remainingFrameCapacity());
                if (options_.maximumFrames.has_value())
                {
                    const auto written = static_cast<std::uint64_t>(writer.framesWritten());
                    capacity = std::min(capacity,
                        *options_.maximumFrames > written ? *options_.maximumFrames - written : 0);
                }
                const auto frames = static_cast<std::size_t>(
                    std::min<std::uint64_t>(block.frameCount, capacity));
                lengthLimitReached = frames < block.frameCount;
                const auto sampleCount =
                    static_cast<std::size_t>(block.channelCount) * frames;
                if (frames != 0
                    && !writer.writeInterleaved(
                        std::span<const float>{
                            interleaved.data(), sampleCount}))
                {
                    recorder_->stopAccepting();
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    auto diagnostic = std::string(writer.error());
                    if (!cleanupError.empty())
                        diagnostic += "; " + cleanupError;
                    setTerminal(
                        RealtimeOutputTakeState::Failed,
                        std::move(diagnostic),
                        writer.framesWritten());
                    return;
                }
                liveWrittenFrames_.store(
                    writer.framesWritten(),
                    std::memory_order_release);
                // Keep what was recorded instead of losing the whole take.
                if (lengthLimitReached)
                    requestStop();
                continue;
            }
            if (consumed
                != talas::runtime::RealtimeOutputRecorderConsumeStatus::Empty)
            {
                recorder_->stopAccepting();
                writer.abandon();
                std::string cleanupError;
                (void)publication_->discard(cleanupError);
                setTerminal(
                    RealtimeOutputTakeState::Failed,
                    "Output recorder produced invalid queued evidence"
                        + (cleanupError.empty()
                            ? std::string{}
                            : "; " + cleanupError),
                    writer.framesWritten());
                return;
            }

            if (request == stopRequest
                && recorder_->producerQuiescent())
            {
                const auto recorderSnapshot = recorder_->snapshot();
                if (recorderSnapshot.incomplete())
                {
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    auto diagnostic = std::string{
                        "Output take incomplete: destination was not published; "}
                        + std::to_string(recorderSnapshot.droppedBlocks)
                        + " dropped blocks, "
                        + std::to_string(recorderSnapshot.rejectedBlocks)
                        + " rejected blocks";
                    if (!cleanupError.empty())
                        diagnostic += "; " + cleanupError;
                    setTerminal(
                        RealtimeOutputTakeState::Failed,
                        std::move(diagnostic),
                        writer.framesWritten());
                    return;
                }
                if (writer.framesWritten() == 0)
                {
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    setTerminal(
                        RealtimeOutputTakeState::Cancelled,
                        cleanupError.empty()
                            ? "Output take contained no frames"
                            : "Output take contained no frames; "
                                + cleanupError,
                        0);
                    return;
                }
                if (!writer.finish())
                {
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    auto diagnostic = std::string(writer.error());
                    if (!cleanupError.empty())
                        diagnostic += "; " + cleanupError;
                    setTerminal(
                        RealtimeOutputTakeState::Failed,
                        std::move(diagnostic),
                        writer.framesWritten());
                    return;
                }
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
                if (options_.beforePublicationAdmissionForTesting)
                    options_.beforePublicationAdmissionForTesting();
#endif
                auto expectedRequest = stopRequest;
                if (!request_.compare_exchange_strong(
                        expectedRequest,
                        publishingRequest,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    const auto stageChanged = stageBindingRejected_.load(
                        std::memory_order_acquire);
                    const auto authorityChanged =
                        authorityBindingRejected_.load(
                            std::memory_order_acquire);
                    const auto renderFailed = renderFailureRejected_.load(
                        std::memory_order_acquire);
                    auto diagnostic = expectedRequest == cancelRequest
                        ? std::string{
                            stageChanged
                                ? "Output take cancelled: active render stage changed"
                                : authorityChanged
                                    ? "Output take cancelled: project authority changed"
                                    : renderFailed
                                        ? "Output take incomplete: source render failed; destination was not published"
                                    : "Output take cancelled"}
                        : std::string{
                            "Output take publication admission changed unexpectedly"};
                    if (!cleanupError.empty())
                        diagnostic += "; " + cleanupError;
                    setTerminal(
                        expectedRequest == cancelRequest
                            ? renderFailed
                                ? RealtimeOutputTakeState::Failed
                                : RealtimeOutputTakeState::Cancelled
                            : RealtimeOutputTakeState::Failed,
                        std::move(diagnostic),
                        writer.framesWritten());
                    return;
                }
#if defined(TALAS_REALTIME_OUTPUT_TAKE_TEST_SUPPORT)
                if (options_.afterPublicationAdmissionForTesting)
                    options_.afterPublicationAdmissionForTesting();
#endif
                std::lock_guard publicationLock(publicationMutex_);
                const auto stageChanged = stageBindingRejected_.load(
                    std::memory_order_acquire);
                const auto authorityChanged =
                    authorityBindingRejected_.load(
                        std::memory_order_acquire);
                const auto renderFailed = renderFailureRejected_.load(
                    std::memory_order_acquire);
                if (publicationRevoked_ || stageChanged
                    || authorityChanged || renderFailed)
                {
                    writer.abandon();
                    std::string cleanupError;
                    (void)publication_->discard(cleanupError);
                    auto diagnostic = std::string{
                        stageChanged
                            ? "Output take cancelled: active render stage changed"
                            : authorityChanged
                                ? "Output take cancelled: project authority changed"
                                : renderFailed
                                    ? "Output take incomplete: source render failed; destination was not published"
                                    : "Output take cancelled"};
                    if (!cleanupError.empty())
                        diagnostic += "; " + cleanupError;
                    setTerminal(
                        renderFailed
                            ? RealtimeOutputTakeState::Failed
                            : RealtimeOutputTakeState::Cancelled,
                        std::move(diagnostic),
                        writer.framesWritten());
                    return;
                }
                std::string publicationMessage;
                if (!publication_->publish(publicationMessage))
                {
                    setTerminal(
                        RealtimeOutputTakeState::Failed,
                        std::move(publicationMessage),
                        writer.framesWritten());
                    return;
                }
                publicationCommitted_ = true;
                if (lengthLimitReached)
                {
                    publicationMessage += publicationMessage.empty() ? "" : "; ";
                    publicationMessage += "recording stopped at the maximum take length";
                }
                setTerminal(
                    RealtimeOutputTakeState::Completed,
                    std::move(publicationMessage),
                    writer.framesWritten());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    catch (const std::exception&)
    {
        recorder_->stopAccepting();
        std::string cleanupError;
        (void)publication_->discard(cleanupError);
        setTerminal(
            RealtimeOutputTakeState::Failed,
            {},
            0);
    }
    catch (...)
    {
        recorder_->stopAccepting();
        std::string cleanupError;
        (void)publication_->discard(cleanupError);
        setTerminal(
            RealtimeOutputTakeState::Failed,
            {},
            0);
    }
}
}
