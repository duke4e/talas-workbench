#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::dsp
{
enum class SerializedSpectrumSyncStatus
{
    Ok,
    InvalidBlock,
    CapacityExceeded,
    ConflictingTriggers,
    MissingInputFrameStart,
};

class SerializedSpectrumSyncSnapshot
{
public:
    explicit SerializedSpectrumSyncSnapshot(std::size_t frameLength);
    [[nodiscard]] std::size_t frameLength() const noexcept
    {
        return frameLength_;
    }

private:
    friend class PreparedSerializedSpectrumFrameSynchronizer;
    std::size_t frameLength_ = 0;
    std::vector<double> amplitudeHistory_;
    std::vector<double> frequencyHistory_;
    std::size_t phase_ = 0;
    std::size_t writeIndex_ = 0;
    std::size_t historyCount_ = 0;
    std::size_t delay_ = 0;
    std::size_t startupSilence_ = 0;
    std::size_t outputComponentIndex_ = 0;
    bool previousSyncOutput_ = false;
    bool previousSyncInput_ = false;
};

// Two sample streams represent one spectral component per sample: amplitude
// and frequency. The caller supplies the common spectral period origin at
// reset.
class PreparedSerializedSpectrumFrameSynchronizer
{
public:
    PreparedSerializedSpectrumFrameSynchronizer(
        std::size_t frameLength, std::size_t maximumBlockFrames);

    // SyncOutput makes the current output sample component zero by reading
    // the already received start of the input frame. SyncInput marks the
    // current input sample as component zero and delays it to the next common
    // spectral boundary. Positive edges trigger either action. Each output
    // may alias its input; the two double output spans must not overlap.
    // componentIndexOut is zero-based; frameLength() denotes startup silence.
    [[nodiscard]] SerializedSpectrumSyncStatus process(
        std::span<const double> amplitudeIn,
        std::span<const double> frequencyIn,
        std::span<const double> syncOutput,
        std::span<const double> syncInput,
        std::span<double> amplitudeOut,
        std::span<double> frequencyOut,
        std::span<std::size_t> componentIndexOut) noexcept;

    void reset(std::uint64_t absoluteFrame = 0) noexcept;
    [[nodiscard]] SerializedSpectrumSyncSnapshot prepareSnapshot() const;
    [[nodiscard]] bool capture(
        SerializedSpectrumSyncSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool restore(
        const SerializedSpectrumSyncSnapshot& snapshot) noexcept;
    [[nodiscard]] std::size_t frameLength() const noexcept { return frameLength_; }
    [[nodiscard]] std::size_t maximumBlockFrames() const noexcept
    {
        return maximumBlockFrames_;
    }

private:
    std::size_t frameLength_ = 0;
    std::size_t maximumBlockFrames_ = 0;
    std::vector<double> amplitudeHistory_;
    std::vector<double> frequencyHistory_;
    std::vector<double> amplitudeScratch_;
    std::vector<double> frequencyScratch_;
    std::vector<std::size_t> componentIndexScratch_;
    std::size_t phase_ = 0;
    std::size_t writeIndex_ = 0;
    std::size_t historyCount_ = 0;
    std::size_t delay_ = 0;
    std::size_t startupSilence_ = 0;
    std::size_t outputComponentIndex_ = 0;
    bool previousSyncOutput_ = false;
    bool previousSyncInput_ = false;
};
}
