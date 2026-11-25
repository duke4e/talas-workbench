#pragma once

#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/dsp/LiveSpectralAnalysis.h"
#include "talas/dsp/PhaseVocoderPitchShifter.h"
#include "talas/dsp/PreparedTimeFrequencyScale.h"
#include "talas/dsp/SerializedSpectrumFrameAssembler.h"
#include <array>
#include <atomic>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace talas::runtime
{
// Allocates only while capturing an exclusive offline boundary. Once armed,
// destruction restores in reverse order without allocation or exceptions.
class OfflineStateJournal
{
    struct Undo
    {
        virtual ~Undo() = default;
        virtual void restore() noexcept = 0;
    };
    template <class... T> struct OwnerPresenceUndo final : Undo
    {
        explicit OwnerPresenceUndo(std::unique_ptr<T>&... values)
            : targets(&values...), existed{static_cast<bool>(values)...} {}
        template <std::size_t... I> void restoreOwners(std::index_sequence<I...>) noexcept
        {
            ((existed[I] ? void() : std::get<I>(targets)->reset()), ...);
        }
        void restore() noexcept override { restoreOwners(std::index_sequence_for<T...>{}); }
        std::tuple<std::unique_ptr<T>*...> targets;
        std::array<bool, sizeof...(T)> existed;
    };
    template <class T> struct ValueUndo final : Undo
    {
        explicit ValueUndo(T& value) : target(value), baseline(value) {}
        void restore() noexcept override { using std::swap; swap(target, baseline); }
        T& target;
        T baseline;
    };
    template <class T> struct ProcessingStateUndo final : Undo
    {
        explicit ProcessingStateUndo(T& value) : target(value), baseline(value.cloneProcessingState()) {}
        void restore() noexcept override
        {
            // Same live object and immutable configuration used for capture.
            if (!target.copyProcessingStateFrom(*baseline)) std::terminate();
        }
        T& target;
        std::unique_ptr<T> baseline;
    };
    struct PartialTrackUndo final : Undo
    {
        using Processor = talas::audio::PreparedPartialTrackResynthesizer;
        static_assert(std::is_nothrow_move_assignable_v<Processor>);
        explicit PartialTrackUndo(Processor& value) : target(value), baseline(value.cloneProcessingState()) {}
        // This participant owns every point/track/segment; it exposes no borrowed
        // views. Move the complete snapshot into the same live processor object.
        void restore() noexcept override { target = std::move(*baseline); }
        Processor& target;
        std::unique_ptr<Processor> baseline;
    };
    struct ComplexSequenceUndo final : Undo
    {
        using Sequence = talas::dsp::PreparedComplexSpectrumSequence;
        explicit ComplexSequenceUndo(Sequence& value)
            : target(value), baseline(value.capacity(), value.maximumBinsPerFrame(), value.channelCount())
        {
            copyFrames(baseline, target);
        }
        // Restore into the original owner: frame views elsewhere must never
        // borrow the journal's storage. Both sequences have identical capacity;
        // every source frame has already passed append's validation.
        static void copyFrames(Sequence& destination, const Sequence& source) noexcept
        {
            destination.reset();
            for (const auto& frame : source.frames())
                if (destination.append(frame.analysisTimestamp, frame.sourceSequence,
                        frame.frameSize, frame.hopSize, frame.window, frame.channelIndex, frame.bins)
                    != talas::dsp::SpectralFrameStatus::Ok)
                    std::terminate(); // Broken internal sequence/capacity invariant.
        }
        void restore() noexcept override { copyFrames(target, baseline); }
        Sequence& target;
        Sequence baseline;
    };
    struct PartialSequenceUndo final : Undo
    {
        using Sequence = talas::dsp::PreparedPartialSpectrumSequence;
        explicit PartialSequenceUndo(Sequence& value)
            : target(value), baseline(value.capacity(), value.maximumPartialsPerFrame())
        {
            copyFrames(baseline, target);
        }
        // Copy payloads, never span views. Restore into the original owner so
        // frame and partial storage identities survive the offline transaction.
        static void copyFrames(Sequence& destination, const Sequence& source) noexcept
        {
            destination.reset();
            for (const auto& frame : source.frames())
                if (destination.append(frame.sourceTimestamp, frame.sourceSequence, frame.partials)
                    != talas::dsp::SpectralFrameStatus::Ok)
                    std::terminate(); // Identical capacity and previously validated source frames.
        }
        void restore() noexcept override { copyFrames(target, baseline); }
        Sequence& target;
        Sequence baseline;
    };
    struct SerializedAssemblyUndo final : Undo
    {
        using Assembler = talas::dsp::PreparedSerializedSpectrumFrameAssembler;
        explicit SerializedAssemblyUndo(Assembler& value)
            : target(value), baseline(value.prepareSnapshot())
        {
            if (!target.capture(baseline)) std::terminate();
        }
        void restore() noexcept override
        {
            if (!target.restore(baseline)) std::terminate();
        }
        Assembler& target;
        talas::dsp::SerializedSpectrumAssemblySnapshot baseline;
    };
    template <class T> struct AtomicUndo final : Undo
    {
        explicit AtomicUndo(std::atomic<T>& value)
            : target(value), baseline(value.load(std::memory_order_acquire)) {}
        void restore() noexcept override { target.store(baseline, std::memory_order_release); }
        std::atomic<T>& target;
        T baseline;
    };
public:
    ~OfflineStateJournal()
    {
        if (armed)
            for (auto index = undo.size(); index != 0; --index) undo[index - 1]->restore();
    }
    template <class T> void capture(T& value)
    {
        static_assert(std::is_copy_constructible_v<T> && std::is_nothrow_swappable_v<T>);
        undo.push_back(std::make_unique<ValueUndo<T>>(value));
    }
    void capture(talas::dsp::PreparedComplexSpectrumSequence& value)
    {
        undo.push_back(std::make_unique<ComplexSequenceUndo>(value));
    }
    void capture(talas::dsp::PreparedPartialSpectrumSequence& value)
    {
        undo.push_back(std::make_unique<PartialSequenceUndo>(value));
    }
    template <class T> void capture(std::atomic<T>& value)
    {
        undo.push_back(std::make_unique<AtomicUndo<T>>(value));
    }
    template <class T> static constexpr bool ownsCopyableState =
        std::is_copy_constructible_v<T> && std::is_nothrow_swappable_v<T>;
    template <class T> static constexpr bool ownsSemanticProcessingState =
        std::is_same_v<T, talas::dsp::PreparedPhaseVocoderPitchShifter>
        || std::is_same_v<T, talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>
        || std::is_same_v<T, talas::dsp::PreparedTimeFrequencyScale>
        || std::is_same_v<T, talas::dsp::PreparedLiveSpectralAnalysis>;
    template <class T> static constexpr bool ownsPartialTrackState =
        std::is_same_v<T, talas::audio::PreparedPartialTrackResynthesizer>;
    template <class T> static constexpr bool ownsSerializedAssemblyState =
        std::is_same_v<T, talas::dsp::PreparedSerializedSpectrumFrameAssembler>;
    template <class T> void captureOwned(const std::unique_ptr<T>& value)
    {
        // An unknown native owner must fail compilation, never silently skip
        // its state or disable the whole transaction for an admitted graph.
        static_assert(ownsCopyableState<T> || ownsSemanticProcessingState<T>
            || ownsPartialTrackState<T> || ownsSerializedAssemblyState<T>,
            "Every owned offline participant requires a checkpoint strategy");
        if constexpr (ownsSemanticProcessingState<T>)
        {
            if (value) undo.push_back(std::make_unique<ProcessingStateUndo<T>>(*value));
        }
        else if constexpr (ownsPartialTrackState<T>)
        {
            if (value) undo.push_back(std::make_unique<PartialTrackUndo>(*value));
        }
        else if constexpr (ownsSerializedAssemblyState<T>)
        {
            if (value) undo.push_back(std::make_unique<SerializedAssemblyUndo>(*value));
        }
        else if constexpr (ownsCopyableState<T>)
            if (value) capture(*value);
    }
    // Native adapters may construct a processor lazily during the transaction.
    // Capture presence once per node; rollback discards only newly created owners.
    template <class... T> void captureOwnerPresence(std::unique_ptr<T>&... values)
    {
        undo.push_back(std::make_unique<OwnerPresenceUndo<T...>>(values...));
    }
    bool armed = false;
private:
    std::vector<std::unique_ptr<Undo>> undo;
};

}
