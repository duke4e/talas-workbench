#include "runtime/component_activation/ComponentActivationAudioStream.h"

#include "talas/audio/Pcm16WaveSpan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <mutex>
#include <thread>
#include <utility>

namespace talas::runtime::component_activation
{
namespace
{
constexpr std::size_t blockFrames =
    componentActivationAudioDecodeBlockFramesV1;
constexpr std::size_t maximumBlockSamples =
    (blockFrames + 1U) * 2U;
static_assert(
    maximumComponentActivationResourceSetAudioStreamsV1
    <= componentActivationAudioStreamSetSlotMaskV1);

std::atomic<std::uint64_t> nextStreamToken{4U};

void addSaturating(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t amount = 1U) noexcept
{
    auto current = counter.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max())
    {
        const auto available =
            std::numeric_limits<std::uint64_t>::max() - current;
        const auto next = current + std::min(amount, available);
        if (counter.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return;
        }
    }
}

[[nodiscard]] std::uint64_t allocateStreamToken() noexcept
{
    auto token = nextStreamToken.fetch_add(4U, std::memory_order_relaxed);
    if (token == 0U)
        token = nextStreamToken.fetch_add(4U, std::memory_order_relaxed);
    return token;
}
}

class ComponentActivationAudioStreamV1::Impl
{
public:
    struct Request
    {
        std::uint64_t generation = 0;
        std::uint64_t firstBlock = 0;
        std::uint64_t lastBlock = 0;
    };

    struct Response
    {
        std::uint64_t generation = 0;
        std::uint64_t blockIndex = 0;
        std::uint32_t frameCount = 0;
        std::array<double, maximumBlockSamples> samples{};
    };

    struct CacheBlock
    {
        bool valid = false;
        std::uint64_t blockIndex = 0;
        std::uint64_t useStamp = 0;
        std::uint32_t frameCount = 0;
        std::array<double, maximumBlockSamples> samples{};
    };

    struct OfflineState
    {
        std::array<Request, componentActivationAudioRequestCapacityV1> requests;
        std::array<Response, componentActivationAudioResponseCapacityV1> responses;
        std::array<CacheBlock, componentActivationAudioWorkerCacheCapacityV1> cache;
        std::array<std::uint64_t, 14> counters;
        std::uint64_t cacheUseStamp = 0;
        double expectedNextSourceFrame = 0;
        bool underrunActive = false;
        ComponentActivationAudioStreamWorkerStateV1 workerState{};
        ComponentActivationAudioStreamHealthV1 health;
        bool deferHealth = false;
    };

    [[nodiscard]] auto processingCounters() noexcept
    {
        return std::array{
            &requestRead_, &requestWrite_, &responseRead_, &responseWrite_,
            &seekGeneration_, &underrunEvents_, &missingFrames_, &discontinuities_,
            &requestRejections_, &responseFullEvents_, &staleBlocks_,
            &decodedBlocks_, &decodeFailures_, &endOfStreamFrames_};
    }

    [[nodiscard]] bool terminal() const noexcept
    {
        const auto state = workerState_.load(std::memory_order_acquire);
        return stopRequested_.load(std::memory_order_acquire)
            || state == ComponentActivationAudioStreamWorkerStateV1::Failed
            || state == ComponentActivationAudioStreamWorkerStateV1::Stopped;
    }

    [[nodiscard]] bool beginOfflineCheckpoint(bool deferHealth) noexcept
    {
        if (checkpoint_ || terminal()) return false;
        try
        {
            auto state = std::make_unique<OfflineState>();
            std::lock_guard workerLock(workerMutex_);
            std::lock_guard inspectionLock(inspectionMutex_);
            if (terminal()) return false;
            state->requests = requests_;
            state->responses = responses_;
            state->cache = cache_;
            const auto counters = processingCounters();
            for (std::size_t i = 0; i < counters.size(); ++i)
                state->counters[i] = counters[i]->load(std::memory_order_relaxed);
            state->cacheUseStamp = cacheUseStamp_;
            state->expectedNextSourceFrame = expectedNextSourceFrame_;
            state->underrunActive = underrunActive_;
            state->workerState = workerState_.load(std::memory_order_relaxed);
            state->health = liveHealth();
            state->deferHealth = deferHealth;
            checkpoint_ = std::move(state);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    [[nodiscard]] bool restoreOfflineCheckpoint() noexcept
    {
        std::lock_guard workerLock(workerMutex_);
        std::lock_guard inspectionLock(inspectionMutex_);
        if (!checkpoint_) return false;
        if (terminal())
        {
            checkpoint_.reset();
            return false;
        }
        const auto& state = *checkpoint_;
        requests_ = state.requests;
        responses_ = state.responses;
        cache_ = state.cache;
        const auto counters = processingCounters();
        for (std::size_t i = 0; i < counters.size(); ++i)
            counters[i]->store(state.counters[i], std::memory_order_relaxed);
        for (std::size_t i = 0; i < responses_.size(); ++i)
            responseGenerations_[i].store(responses_[i].generation, std::memory_order_relaxed);
        cacheUseStamp_ = state.cacheUseStamp;
        expectedNextSourceFrame_ = state.expectedNextSourceFrame;
        underrunActive_ = state.underrunActive;
        workerState_.store(state.workerState, std::memory_order_release);
        checkpoint_.reset();
        return true;
    }

    [[nodiscard]] bool commitOfflineCheckpoint() noexcept
    {
        std::lock_guard inspectionLock(inspectionMutex_);
        if (!checkpoint_) return false;
        checkpoint_.reset();
        return !terminal();
    }

    Impl(
        std::span<const std::uint8_t> sourceBytes,
        talas::audio::Pcm16WaveLayout layout,
        const ComponentActivationAudioStreamGrantV1& grant,
        const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
        bool manualWorker) noexcept
        : sourceBytes_(sourceBytes)
        , layout_(layout)
        , grant_(grant)
        , prepare_(prepare)
        , expectedNextSourceFrame_(prepare.initialSourceFrame)
        , manualWorker_(manualWorker)
    {
        resourceId_.fill(0);
        std::copy_n(
            grant.resourceId.begin(),
            grant.resourceIdBytes,
            resourceId_.begin());
        view_ = {};
        view_.structSize = sizeof(view_);
        view_.apiVersion = TALAS_KERNEL_API_VERSION_V3;
        view_.id = {resourceId_.data(), grant.resourceIdBytes};
        view_.generation = grant.resourceGeneration;
        std::copy(
            grant.sourceContentDigest.begin(),
            grant.sourceContentDigest.end(),
            view_.sourceContentDigest);
        view_.channelCount = grant.sourceChannelCount;
        view_.sampleRate = grant.sourceSampleRate;
        view_.frameCount = grant.sourceFrameCount;
        view_.streamToken = allocateStreamToken();
        view_.maximumSourceFrameIncrement =
            prepare.maximumSourceFrameIncrement;
        view_.maximumReadFrames = prepare.maximumReadFrames;
        seekGeneration_.store(1U, std::memory_order_relaxed);
    }

    ~Impl()
    {
        stop();
    }

    [[nodiscard]] bool start() noexcept
    {
        if (manualWorker_)
            return true;
        try
        {
            worker_ = std::thread([this] { workerLoop(); });
            return true;
        }
        catch (...)
        {
            workerState_.store(
                ComponentActivationAudioStreamWorkerStateV1::Failed,
                std::memory_order_release);
            return false;
        }
    }

    void stop() noexcept
    {
        stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        workerState_.store(
            ComponentActivationAudioStreamWorkerStateV1::Stopped,
            std::memory_order_release);
        std::lock_guard lock(inspectionMutex_);
        checkpoint_.reset();
    }

    [[nodiscard]] std::uint64_t sourceBlockCount() const noexcept
    {
        return (layout_.frameCount + blockFrames - 1U) / blockFrames;
    }

    [[nodiscard]] bool bindResourceSetSlot(std::size_t slot) noexcept
    {
        if (checkpoint_ || terminal()
            || slot >= maximumComponentActivationResourceSetAudioStreamsV1
            || (view_.streamToken
                & componentActivationAudioStreamSetSlotMaskV1) != 0U)
        {
            return false;
        }
        view_.streamToken |= static_cast<std::uint64_t>(slot + 1U);
        return true;
    }

    [[nodiscard]] bool publishRequest(
        std::uint64_t generation,
        std::uint64_t firstBlock,
        std::uint64_t requestedLastBlock) noexcept
    {
        const auto count = sourceBlockCount();
        if (generation == 0U || count == 0U || firstBlock >= count)
            return false;
        const auto highLast = firstBlock
            + static_cast<std::uint64_t>(
                componentActivationAudioHighWatermarkBlocksV1 - 1U);
        const auto lastBlock = std::min(
            count - 1U,
            std::max(requestedLastBlock, highLast));
        const auto write = requestWrite_.load(std::memory_order_relaxed);
        const auto read = requestRead_.load(std::memory_order_acquire);
        if (write - read >= requests_.size())
        {
            addSaturating(requestRejections_);
            return false;
        }
        requests_[write % requests_.size()] = {
            generation,
            firstBlock,
            lastBlock,
        };
        requestWrite_.store(write + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool popLatestRequest(Request& request) noexcept
    {
        auto read = requestRead_.load(std::memory_order_relaxed);
        const auto write = requestWrite_.load(std::memory_order_acquire);
        if (read == write)
            return false;
        bool found = false;
        const auto currentGeneration =
            seekGeneration_.load(std::memory_order_acquire);
        while (read != write)
        {
            const auto candidate = requests_[read % requests_.size()];
            ++read;
            if (candidate.generation == currentGeneration)
            {
                request = candidate;
                found = true;
            }
        }
        requestRead_.store(read, std::memory_order_release);
        return found;
    }

    [[nodiscard]] const Response* responseForBlock(
        std::uint64_t generation,
        std::uint64_t blockIndex) const noexcept
    {
        const auto read = responseRead_.load(std::memory_order_acquire);
        const auto write = responseWrite_.load(std::memory_order_acquire);
        for (auto index = read; index < write; ++index)
        {
            const auto& response = responses_[index % responses_.size()];
            if (response.generation == generation
                && response.blockIndex == blockIndex)
            {
                return &response;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool responseAlreadyPublished(
        std::uint64_t generation,
        std::uint64_t blockIndex) const noexcept
    {
        return responseForBlock(generation, blockIndex) != nullptr;
    }

    [[nodiscard]] bool pushResponse(const Response& response) noexcept
    {
        if (response.generation
            != seekGeneration_.load(std::memory_order_acquire))
        {
            addSaturating(staleBlocks_);
            return false;
        }
        if (responseAlreadyPublished(
                response.generation, response.blockIndex))
        {
            return true;
        }
        const auto write = responseWrite_.load(std::memory_order_relaxed);
        const auto read = responseRead_.load(std::memory_order_acquire);
        if (write - read >= responses_.size())
        {
            addSaturating(responseFullEvents_);
            return false;
        }
        responses_[write % responses_.size()] = response;
        responseGenerations_[write % responses_.size()].store(
            response.generation, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (response.generation
            != seekGeneration_.load(std::memory_order_acquire))
        {
            addSaturating(staleBlocks_);
            return false;
        }
        responseWrite_.store(write + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool decodeBlock(
        std::uint64_t blockIndex,
        Response& response) noexcept
    {
        ++cacheUseStamp_;
        auto found = std::find_if(
            cache_.begin(),
            cache_.end(),
            [blockIndex](const auto& candidate) {
                return candidate.valid
                    && candidate.blockIndex == blockIndex;
            });
        if (found == cache_.end())
        {
            found = std::find_if(
                cache_.begin(), cache_.end(), [](const auto& candidate) {
                    return !candidate.valid;
                });
            if (found == cache_.end())
            {
                found = std::min_element(
                    cache_.begin(),
                    cache_.end(),
                    [](const auto& left, const auto& right) {
                        return left.useStamp < right.useStamp;
                    });
            }
            const auto firstFrame = blockIndex * blockFrames;
            if (firstFrame >= layout_.frameCount)
                return false;
            found->samples.fill(0.0);
            found->frameCount = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    blockFrames + 1U,
                    layout_.frameCount - firstFrame));
            std::array<double, 2> channels{};
            for (std::uint32_t frame = 0;
                 frame < found->frameCount;
                 ++frame)
            {
                if (!talas::audio::decodePcm16WaveFrame(
                        sourceBytes_,
                        layout_,
                        firstFrame + frame,
                        std::span<double>{
                            channels.data(), layout_.channelCount}))
                {
                    addSaturating(decodeFailures_);
                    workerState_.store(
                        ComponentActivationAudioStreamWorkerStateV1::Failed,
                        std::memory_order_release);
                    return false;
                }
                for (std::size_t channel = 0;
                     channel < layout_.channelCount;
                     ++channel)
                {
                    found->samples[
                        static_cast<std::size_t>(frame)
                            * layout_.channelCount
                        + channel] = channels[channel];
                }
            }
            found->valid = true;
            found->blockIndex = blockIndex;
            addSaturating(decodedBlocks_);
        }
        found->useStamp = cacheUseStamp_;
        response.blockIndex = blockIndex;
        response.frameCount = found->frameCount;
        response.samples = found->samples;
        return true;
    }

    [[nodiscard]] std::size_t processRequests(
        std::size_t maximumBlocks) noexcept
    {
        std::lock_guard lock(workerMutex_);
        if (terminal()) return 0;
        Request request;
        if (maximumBlocks == 0U || !popLatestRequest(request))
            return 0U;
        workerState_.store(
            ComponentActivationAudioStreamWorkerStateV1::Filling,
            std::memory_order_release);
        std::size_t published = 0U;
        for (auto block = request.firstBlock;
             block <= request.lastBlock && published < maximumBlocks;
             ++block)
        {
            if (stopRequested_.load(std::memory_order_acquire)
                || request.generation
                    != seekGeneration_.load(std::memory_order_acquire))
            {
                break;
            }
            Response response;
            response.generation = request.generation;
            if (!decodeBlock(block, response))
                break;
            if (!pushResponse(response))
                break;
            ++published;
        }
        if (workerState_.load(std::memory_order_acquire)
            != ComponentActivationAudioStreamWorkerStateV1::Failed)
        {
            workerState_.store(
                request.lastBlock + 1U >= sourceBlockCount()
                    ? ComponentActivationAudioStreamWorkerStateV1::EndOfStream
                    : ComponentActivationAudioStreamWorkerStateV1::Ready,
                std::memory_order_release);
        }
        return published;
    }

    void workerLoop() noexcept
    {
        while (!stopRequested_.load(std::memory_order_acquire))
        {
            if (processRequests(
                    componentActivationAudioResponseCapacityV1) == 0U)
            {
                std::this_thread::yield();
            }
        }
    }

    void drainResponses() noexcept
    {
        const auto write = responseWrite_.load(std::memory_order_acquire);
        const auto read = responseRead_.exchange(
            write, std::memory_order_acq_rel);
        if (write > read)
            addSaturating(staleBlocks_, write - read);
    }

    void retireResponsesBefore(
        std::uint64_t generation,
        std::uint64_t blockIndex) noexcept
    {
        auto read = responseRead_.load(std::memory_order_relaxed);
        const auto write = responseWrite_.load(std::memory_order_acquire);
        while (read < write)
        {
            const auto& response = responses_[read % responses_.size()];
            if (response.generation == generation
                && response.blockIndex >= blockIndex)
            {
                break;
            }
            if (response.generation != generation)
                addSaturating(staleBlocks_);
            ++read;
        }
        responseRead_.store(read, std::memory_order_release);
    }

    [[nodiscard]] std::size_t bufferedBlocks(
        std::uint64_t generation) const noexcept
    {
        const auto read = responseRead_.load(std::memory_order_acquire);
        const auto write = responseWrite_.load(std::memory_order_acquire);
        std::size_t count = 0U;
        for (auto index = read; index < write; ++index)
        {
            if (responses_[index % responses_.size()].generation == generation)
                ++count;
        }
        return count;
    }

    [[nodiscard]] bool prewarm(
        double sourceFrame,
        std::chrono::milliseconds timeout) noexcept
    {
        if (!std::isfinite(sourceFrame) || sourceFrame < 0.0
            || sourceFrame >= static_cast<double>(layout_.frameCount))
        {
            return false;
        }
        const auto generation = seekGeneration_.load(std::memory_order_acquire);
        const auto firstBlock = static_cast<std::uint64_t>(
            std::floor(sourceFrame)) / blockFrames;
        const auto targetLast = std::min(
            sourceBlockCount() - 1U,
            firstBlock
                + componentActivationAudioHighWatermarkBlocksV1 - 1U);
        if (!publishRequest(generation, firstBlock, targetLast))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() <= deadline)
        {
            if (manualWorker_)
                static_cast<void>(processRequests(responses_.size()));
            bool ready = true;
            for (auto block = firstBlock; block <= targetLast; ++block)
            {
                if (responseForBlock(generation, block) == nullptr)
                {
                    ready = false;
                    break;
                }
            }
            if (ready)
                return true;
            if (workerState_.load(std::memory_order_acquire)
                == ComponentActivationAudioStreamWorkerStateV1::Failed)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return false;
    }

    [[nodiscard]] bool replaceEpochAndPrewarm(
        double initialSourceFrame,
        std::chrono::milliseconds timeout) noexcept
    {
        const auto workerState = workerState_.load(std::memory_order_acquire);
        if (workerState == ComponentActivationAudioStreamWorkerStateV1::Failed
            || workerState
                == ComponentActivationAudioStreamWorkerStateV1::Stopped)
        {
            return false;
        }
        auto generation = seekGeneration_.load(std::memory_order_acquire);
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        seekGeneration_.store(generation + 1U, std::memory_order_release);
        expectedNextSourceFrame_ = initialSourceFrame;
        underrunActive_ = false;
        drainResponses();
        return prewarm(initialSourceFrame, timeout);
    }

    [[nodiscard]] TalasKernelAbiStatusV1 read(
        const TalasKernelAbiImmutableAudioStreamReadV3& request,
        TalasKernelAbiImmutableAudioStreamReadResultV3& result) noexcept
    {
        const auto workerState = workerState_.load(std::memory_order_acquire);
        if (workerState == ComponentActivationAudioStreamWorkerStateV1::Failed
            || workerState
                == ComponentActivationAudioStreamWorkerStateV1::Stopped)
        {
            return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
        }
        if (validateComponentActivationAudioStreamReadV3(request, view_)
            != ComponentActivationProtocolDiagnosticV1::Valid)
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }
        const auto discontinuity =
            request.absoluteSourceFrame != expectedNextSourceFrame_;
        auto generation = seekGeneration_.load(std::memory_order_acquire);
        if (discontinuity
            && generation == std::numeric_limits<std::uint64_t>::max())
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
        }

        const auto finalPosition = static_cast<long double>(
                request.absoluteSourceFrame)
            + static_cast<long double>(request.frameCount - 1U)
                * static_cast<long double>(request.sourceFrameIncrement);
        const auto firstFrame = static_cast<std::uint64_t>(
            std::floor(request.absoluteSourceFrame));
        const auto finalLeftFrame = static_cast<std::uint64_t>(
            std::floor(finalPosition));
        const auto firstBlock = firstFrame / blockFrames;
        const auto finalBlock = finalLeftFrame / blockFrames;

        if (discontinuity)
        {
            ++generation;
            seekGeneration_.store(generation, std::memory_order_release);
            addSaturating(discontinuities_);
            underrunActive_ = false;
            drainResponses();
            static_cast<void>(publishRequest(
                generation, firstBlock, finalBlock));
        }
        else
        {
            retireResponsesBefore(generation, firstBlock);
        }

        std::array<const Response*,
            maximumComponentActivationAudioTouchedSourceBlocksV1> blocks{};
        if (firstBlock < sourceBlockCount())
        {
            const auto availableLast = std::min(
                finalBlock, sourceBlockCount() - 1U);
            for (auto block = firstBlock; block <= availableLast; ++block)
            {
                blocks[static_cast<std::size_t>(block - firstBlock)] =
                    responseForBlock(generation, block);
            }
        }

        std::uint64_t missingFrames = 0U;
        std::uint64_t endOfStreamFrames = 0U;
        std::uint64_t underrunEvents = 0U;
        for (std::uint64_t outputFrame = 0;
             outputFrame < request.frameCount;
             ++outputFrame)
        {
            const auto sourceFrame = static_cast<double>(
                static_cast<long double>(request.absoluteSourceFrame)
                + static_cast<long double>(outputFrame)
                    * static_cast<long double>(request.sourceFrameIncrement));
            auto* output = request.interleavedOutput
                + outputFrame * request.outputChannelCount;
            if (sourceFrame >= static_cast<double>(layout_.frameCount))
            {
                std::fill_n(output, request.outputChannelCount, 0.0);
                ++missingFrames;
                ++endOfStreamFrames;
                underrunActive_ = false;
                continue;
            }
            const auto leftFrame = static_cast<std::uint64_t>(
                std::floor(sourceFrame));
            const auto sourceBlock = leftFrame / blockFrames;
            const auto* response = blocks[
                static_cast<std::size_t>(sourceBlock - firstBlock)];
            const auto leftOffset = static_cast<std::size_t>(
                leftFrame % blockFrames);
            const auto rightFrame = std::min<std::uint64_t>(
                leftFrame + 1U, layout_.frameCount - 1U);
            const auto rightOffset = leftOffset
                + static_cast<std::size_t>(rightFrame != leftFrame);
            const auto available = response != nullptr
                && leftOffset < response->frameCount
                && rightOffset < response->frameCount;
            if (!available)
            {
                std::fill_n(output, request.outputChannelCount, 0.0);
                ++missingFrames;
                if (!underrunActive_)
                {
                    ++underrunEvents;
                    underrunActive_ = true;
                }
                continue;
            }
            const auto fraction = sourceFrame
                - static_cast<double>(leftFrame);
            for (std::size_t channel = 0;
                 channel < request.outputChannelCount;
                 ++channel)
            {
                const auto left = response->samples[
                    leftOffset * layout_.channelCount + channel];
                const auto right = response->samples[
                    rightOffset * layout_.channelCount + channel];
                output[channel] = left + (right - left) * fraction;
            }
            underrunActive_ = false;
        }

        addSaturating(missingFrames_, missingFrames);
        addSaturating(endOfStreamFrames_, endOfStreamFrames);
        addSaturating(underrunEvents_, underrunEvents);
        expectedNextSourceFrame_ = request.absoluteSourceFrame
            + static_cast<double>(request.frameCount)
                * request.sourceFrameIncrement;

        if (missingFrames > endOfStreamFrames && !discontinuity)
        {
            static_cast<void>(publishRequest(
                generation, firstBlock, finalBlock));
        }
        const auto nextBlock = expectedNextSourceFrame_
                < static_cast<double>(layout_.frameCount)
            ? static_cast<std::uint64_t>(
                  std::floor(expectedNextSourceFrame_)) / blockFrames
            : sourceBlockCount();
        if (!discontinuity && nextBlock < sourceBlockCount())
        {
            retireResponsesBefore(generation, nextBlock);
            if (bufferedBlocks(generation)
                < componentActivationAudioLowWatermarkBlocksV1)
            {
                static_cast<void>(publishRequest(
                    generation, nextBlock, nextBlock));
            }
        }

        result = {};
        result.structSize = sizeof(result);
        result.outcome = endOfStreamFrames != 0U
            ? TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM
            : (missingFrames != 0U
                ? TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN
                : TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE);
        result.frameCount = request.frameCount;
        result.missingFrameCount = missingFrames;
        result.endOfStreamFrameCount = endOfStreamFrames;
        result.seekGeneration = generation;
        result.expectedNextSourceFrame = expectedNextSourceFrame_;
        return TALAS_KERNEL_ABI_STATUS_OK;
    }

    [[nodiscard]] std::size_t bufferedBlocksForHealth(std::uint64_t generation) const noexcept
    {
        const auto read = responseRead_.load(std::memory_order_acquire);
        const auto write = responseWrite_.load(std::memory_order_acquire);
        const auto available = write >= read
            ? std::min<std::uint64_t>(write - read, responseGenerations_.size()) : 0;
        std::size_t count = 0;
        for (std::uint64_t i = 0; i < available; ++i)
            if (responseGenerations_[(read + i) % responseGenerations_.size()].load(
                    std::memory_order_relaxed) == generation) ++count;
        return count;
    }

    [[nodiscard]] ComponentActivationAudioStreamHealthV1 liveHealth()
        const noexcept
    {
        const auto generation = seekGeneration_.load(std::memory_order_acquire);
        const auto requestRead = requestRead_.load(std::memory_order_acquire);
        const auto requestWrite = requestWrite_.load(std::memory_order_acquire);
        return {
            underrunEvents_.load(std::memory_order_relaxed),
            missingFrames_.load(std::memory_order_relaxed),
            discontinuities_.load(std::memory_order_relaxed),
            requestRejections_.load(std::memory_order_relaxed),
            responseFullEvents_.load(std::memory_order_relaxed),
            staleBlocks_.load(std::memory_order_relaxed),
            decodedBlocks_.load(std::memory_order_relaxed),
            decodeFailures_.load(std::memory_order_relaxed),
            endOfStreamFrames_.load(std::memory_order_relaxed),
            generation,
            bufferedBlocksForHealth(generation),
            requestWrite >= requestRead
                ? std::min<std::uint64_t>(requestWrite - requestRead, requests_.size()) : 0,
            workerState_.load(std::memory_order_acquire),
        };
    }

    [[nodiscard]] ComponentActivationAudioStreamHealthV1 health() const noexcept
    {
        std::lock_guard lock(inspectionMutex_);
        if (checkpoint_ && checkpoint_->deferHealth && !terminal())
            return checkpoint_->health;
        return liveHealth();
    }

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
    void setHealthCountersForTest(std::uint64_t value) noexcept
    {
        underrunEvents_.store(value, std::memory_order_relaxed);
        missingFrames_.store(value, std::memory_order_relaxed);
        discontinuities_.store(value, std::memory_order_relaxed);
        requestRejections_.store(value, std::memory_order_relaxed);
        responseFullEvents_.store(value, std::memory_order_relaxed);
        staleBlocks_.store(value, std::memory_order_relaxed);
        decodedBlocks_.store(value, std::memory_order_relaxed);
        decodeFailures_.store(value, std::memory_order_relaxed);
        endOfStreamFrames_.store(value, std::memory_order_relaxed);
    }

    void setDecodeFailuresForTest(std::uint64_t value) noexcept
    {
        decodeFailures_.store(value, std::memory_order_relaxed);
    }
#endif

    std::span<const std::uint8_t> sourceBytes_;
    talas::audio::Pcm16WaveLayout layout_;
    ComponentActivationAudioStreamGrantV1 grant_;
    TalasKernelAbiImmutableAudioStreamPrepareV3 prepare_{};
    std::array<char, maximumComponentActivationDescriptorIdBytesV1>
        resourceId_{};
    TalasKernelAbiImmutableAudioStreamViewV3 view_{};
    std::array<Request, componentActivationAudioRequestCapacityV1> requests_{};
    std::array<Response, componentActivationAudioResponseCapacityV1> responses_{};
    // Inspection does not borrow response payload slots: the consumer may
    // release a slot while an independent health reader is scanning it.
    std::array<std::atomic<std::uint64_t>, componentActivationAudioResponseCapacityV1>
        responseGenerations_{};
    std::array<CacheBlock, componentActivationAudioWorkerCacheCapacityV1>
        cache_{};
    std::atomic<std::uint64_t> requestRead_{0U};
    std::atomic<std::uint64_t> requestWrite_{0U};
    std::atomic<std::uint64_t> responseRead_{0U};
    std::atomic<std::uint64_t> responseWrite_{0U};
    std::atomic<std::uint64_t> seekGeneration_{0U};
    std::atomic<std::uint64_t> underrunEvents_{0U};
    std::atomic<std::uint64_t> missingFrames_{0U};
    std::atomic<std::uint64_t> discontinuities_{0U};
    std::atomic<std::uint64_t> requestRejections_{0U};
    std::atomic<std::uint64_t> responseFullEvents_{0U};
    std::atomic<std::uint64_t> staleBlocks_{0U};
    std::atomic<std::uint64_t> decodedBlocks_{0U};
    std::atomic<std::uint64_t> decodeFailures_{0U};
    std::atomic<std::uint64_t> endOfStreamFrames_{0U};
    std::atomic<ComponentActivationAudioStreamWorkerStateV1> workerState_{
        ComponentActivationAudioStreamWorkerStateV1::Waiting};
    std::atomic<bool> stopRequested_{false};
    std::uint64_t cacheUseStamp_ = 0U;
    double expectedNextSourceFrame_ = 0.0;
    bool underrunActive_ = false;
    bool manualWorker_ = false;
    std::thread worker_;
    std::mutex workerMutex_;
    mutable std::mutex inspectionMutex_;
    std::unique_ptr<OfflineState> checkpoint_;
};

ComponentActivationAudioStreamPrepareResultV1
ComponentActivationAudioStreamV1::prepareInternal(
    std::span<const std::uint8_t> sourceBytes,
    const ComponentActivationAudioStreamGrantV1& grant,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    std::chrono::milliseconds timeout,
    bool manualWorker) noexcept
{
    ComponentActivationAudioStreamPrepareResultV1 result;
    if (validateComponentActivationAudioStreamPrepareV3(prepare, grant)
        != ComponentActivationProtocolDiagnosticV1::Valid)
    {
        return result;
    }
    if (sourceBytes.size() != grant.sourceByteCount
        || calculateComponentActivationByteDigestV1(sourceBytes)
            != grant.sourceContentDigest)
    {
        result.diagnostic =
            ComponentActivationAudioStreamDiagnosticV1::SourceInvalid;
        return result;
    }
    const auto parsed = talas::audio::parseStrictPcm16Wave(
        sourceBytes, maximumComponentActivationAudioSourceSampleRateV1);
    if (!parsed.ok()
        || parsed.layout.channelCount != grant.sourceChannelCount
        || parsed.layout.sampleRate != grant.sourceSampleRate
        || parsed.layout.frameCount != grant.sourceFrameCount)
    {
        result.diagnostic =
            ComponentActivationAudioStreamDiagnosticV1::SourceInvalid;
        return result;
    }
    try
    {
        auto implementation =
            std::make_unique<ComponentActivationAudioStreamV1::Impl>(
                sourceBytes, parsed.layout, grant, prepare, manualWorker);
        if (!implementation->start())
        {
            result.diagnostic =
                ComponentActivationAudioStreamDiagnosticV1::WorkerUnavailable;
            return result;
        }
        if (!implementation->prewarm(prepare.initialSourceFrame, timeout))
        {
            result.diagnostic =
                ComponentActivationAudioStreamDiagnosticV1::PrewarmFailed;
            return result;
        }
        result.stream = std::unique_ptr<ComponentActivationAudioStreamV1>(
            new ComponentActivationAudioStreamV1(
                std::move(implementation)));
        result.diagnostic = ComponentActivationAudioStreamDiagnosticV1::Ready;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.diagnostic =
            ComponentActivationAudioStreamDiagnosticV1::AllocationFailed;
        return result;
    }
}

ComponentActivationAudioStreamV1::ComponentActivationAudioStreamV1(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation))
{
}

ComponentActivationAudioStreamV1::~ComponentActivationAudioStreamV1()
{
    shutdown();
}

const TalasKernelAbiImmutableAudioStreamViewV3&
ComponentActivationAudioStreamV1::view() const noexcept
{
    return impl_->view_;
}

TalasKernelAbiStatusV1 ComponentActivationAudioStreamV1::read(
    const TalasKernelAbiImmutableAudioStreamReadV3& request,
    TalasKernelAbiImmutableAudioStreamReadResultV3& result) noexcept
{
    return impl_ != nullptr
        ? impl_->read(request, result)
        : TALAS_KERNEL_ABI_STATUS_QUARANTINED;
}

bool ComponentActivationAudioStreamV1::bindResourceSetSlot(
    std::size_t slot) noexcept
{
    return impl_ != nullptr && impl_->bindResourceSetSlot(slot);
}

bool ComponentActivationAudioStreamV1::replaceEpochAndPrewarm(
    double initialSourceFrame,
    std::chrono::milliseconds timeout) noexcept
{
    return impl_ != nullptr
        && impl_->replaceEpochAndPrewarm(initialSourceFrame, timeout);
}

bool ComponentActivationAudioStreamV1::beginOfflineCheckpoint(bool deferHealth) noexcept
{
    return impl_ != nullptr && impl_->beginOfflineCheckpoint(deferHealth);
}

bool ComponentActivationAudioStreamV1::restoreOfflineCheckpoint() noexcept
{
    return impl_ != nullptr && impl_->restoreOfflineCheckpoint();
}

bool ComponentActivationAudioStreamV1::commitOfflineCheckpoint() noexcept
{
    return impl_ != nullptr && impl_->commitOfflineCheckpoint();
}

void ComponentActivationAudioStreamV1::shutdown() noexcept
{
    if (impl_ != nullptr)
        impl_->stop();
}

ComponentActivationAudioStreamHealthV1
ComponentActivationAudioStreamV1::health() const noexcept
{
    return impl_ != nullptr
        ? impl_->health()
        : ComponentActivationAudioStreamHealthV1{};
}

ComponentActivationAudioStreamHealthV1
ComponentActivationAudioStreamV1::processingHealth() const noexcept
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->inspectionMutex_);
    return impl_->liveHealth();
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
std::size_t ComponentActivationAudioStreamV1::pumpForTest(
    std::size_t maximumBlocks) noexcept
{
    return impl_ != nullptr && impl_->manualWorker_
        ? impl_->processRequests(maximumBlocks)
        : 0U;
}

void ComponentActivationAudioStreamV1::setHealthCountersForTest(
    std::uint64_t value) noexcept
{
    if (impl_ != nullptr)
        impl_->setHealthCountersForTest(value);
}

void ComponentActivationAudioStreamV1::setDecodeFailuresForTest(
    std::uint64_t value) noexcept
{
    if (impl_ != nullptr)
        impl_->setDecodeFailuresForTest(value);
}

void ComponentActivationAudioStreamV1::failWorkerForTest() noexcept
{
    if (impl_ != nullptr)
    {
        std::lock_guard lock(impl_->workerMutex_);
        addSaturating(impl_->decodeFailures_);
        impl_->workerState_.store(ComponentActivationAudioStreamWorkerStateV1::Failed,
            std::memory_order_release);
    }
}
#endif

ComponentActivationAudioStreamPrepareResultV1
prepareComponentActivationAudioStreamV1(
    std::span<const std::uint8_t> sourceBytes,
    const ComponentActivationAudioStreamGrantV1& grant,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    std::chrono::milliseconds prewarmTimeout) noexcept
{
    return ComponentActivationAudioStreamV1::prepareInternal(
        sourceBytes, grant, prepare, prewarmTimeout, false);
}

#if defined(TALAS_COMPONENT_ACTIVATION_TEST_SUPPORT)
ComponentActivationAudioStreamPrepareResultV1
prepareComponentActivationAudioStreamForTestV1(
    std::span<const std::uint8_t> sourceBytes,
    const ComponentActivationAudioStreamGrantV1& grant,
    const TalasKernelAbiImmutableAudioStreamPrepareV3& prepare,
    ComponentActivationAudioStreamWorkerModeV1 workerMode) noexcept
{
    return ComponentActivationAudioStreamV1::prepareInternal(
        sourceBytes,
        grant,
        prepare,
        std::chrono::milliseconds(250),
        workerMode == ComponentActivationAudioStreamWorkerModeV1::ManualTest);
}
#endif
}
