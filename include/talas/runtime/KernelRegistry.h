#pragma once

#include "talas/core/BuiltinKernelRegistry.h"
#include "talas/runtime/RealtimeTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace talas::runtime
{
using KernelKind = talas::core::BuiltinKernelKind;
using KernelDescriptor = talas::core::BuiltinKernelDescriptor;

[[nodiscard]] std::span<const KernelDescriptor> builtinKernelDescriptors() noexcept;
[[nodiscard]] const KernelDescriptor* findBuiltinKernel(std::string_view typeId) noexcept;

enum class KernelProcessStatus
{
    Ok,
    NotPrepared,
    InvalidBlockLayout,
    InvalidConfiguration,
    InvalidValue,
};

struct KernelSignal
{
    double constant = 0.0;
    std::span<const double> samples;
    bool perFrame = false;

    [[nodiscard]] static KernelSignal constantValue(double value) noexcept;
    [[nodiscard]] static KernelSignal sampleValues(std::span<const double> values) noexcept;
    [[nodiscard]] bool validFor(std::size_t frameCount) const noexcept;
    [[nodiscard]] double valueAt(std::size_t frame) const noexcept;
};

struct KernelProcessContext
{
    RealtimeTransportSnapshot transport;
};

struct ClockKernelConfig
{
    double sampleRate = 0.0;
    double frequency = 0.0;
    double dutyCycle = 0.5;
    bool enabled = true;
    bool phaseContinuous = false;
};

struct ClockKernelBlock
{
    std::span<double> output;
    KernelSignal frequency;
    KernelSignal dutyCycle;
};

class ClockKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(ClockKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(
        const ClockKernelBlock& block,
        KernelProcessContext context) noexcept;

private:
    ClockKernelConfig config_;
    long double phase_ = 0.0L;
    std::uint64_t phaseEpoch_ = 0;
    std::uint64_t nextAbsoluteFrame_ = 0;
    bool phaseInitialized_ = false;
    bool prepared_ = false;
};

struct ClockDividerKernelConfig
{
    std::uint32_t division = 1;
    std::uint32_t steps = 1;
};

struct ClockDividerKernelBlock
{
    std::span<double> stepOutput;
    std::span<double> gateOutput;
    KernelSignal clock;
    KernelSignal reset;
    KernelSignal division;
    KernelSignal steps;
};

class ClockDividerKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(ClockDividerKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const ClockDividerKernelBlock& block) noexcept;

private:
    ClockDividerKernelConfig config_;
    std::uint32_t edgeCount_ = 0;
    std::uint32_t nextStep_ = 0;
    std::uint32_t heldStep_ = 0;
    bool clockWasPositive_ = false;
    bool gateOpen_ = false;
    bool prepared_ = false;
};

struct QuantizerKernelConfig
{
    double stepSize = 1.0;
    double offset = 0.0;
};

struct QuantizerKernelBlock
{
    std::span<double> output;
    KernelSignal value;
    KernelSignal stepSize;
    KernelSignal offset;
};

class QuantizerKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(QuantizerKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const QuantizerKernelBlock& block) noexcept;

private:
    QuantizerKernelConfig config_;
    bool prepared_ = false;
};

struct SchmittGateKernelConfig
{
    double lowThreshold = 0.25;
    double highThreshold = 0.75;
};

struct SchmittGateKernelBlock
{
    std::span<double> output;
    KernelSignal value;
    KernelSignal lowThreshold;
    KernelSignal highThreshold;
};

class SchmittGateKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(SchmittGateKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const SchmittGateKernelBlock& block) noexcept;

private:
    SchmittGateKernelConfig config_;
    bool gateOpen_ = false;
    bool prepared_ = false;
};

enum class LogicOperation
{
    And,
    Or,
    Xor,
    Not,
};

struct LogicKernelConfig
{
    LogicOperation operation = LogicOperation::And;
};

struct LogicKernelBlock
{
    std::span<double> output;
    KernelSignal left;
    KernelSignal right;
};

class LogicKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(LogicKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const LogicKernelBlock& block) noexcept;

private:
    LogicKernelConfig config_;
    bool prepared_ = false;
};

struct SlewKernelConfig
{
    double sampleRate = 0.0;
    double riseTime = 0.0;
    double fallTime = 0.0;
};

struct SlewKernelBlock
{
    std::span<double> output;
    KernelSignal value;
    KernelSignal riseTime;
    KernelSignal fallTime;
};

class SlewKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(SlewKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const SlewKernelBlock& block) noexcept;

private:
    SlewKernelConfig config_;
    double currentValue_ = 0.0;
    bool initialized_ = false;
    bool prepared_ = false;
};

enum class OscillatorWavetableInterpolation
{
    Linear,
    Nearest,
};

// An Oscillator reads only the first 4096
// entries of a longer wavetable.
inline constexpr std::size_t oscillatorWavetableEntries = 4096;

struct OscillatorKernelConfig
{
    double sampleRate = 0.0;
    bool frequencyModulation = false;
    double maxModulationIndex = 0.0;
    std::span<const double> wavetable;
    OscillatorWavetableInterpolation interpolation =
        OscillatorWavetableInterpolation::Linear;
};

struct OscillatorKernelBlock
{
    std::span<double> output;
    KernelSignal frequency;
    KernelSignal amplitude = KernelSignal::constantValue(1.0);
    KernelSignal envelope = KernelSignal::constantValue(1.0);
    KernelSignal pitchBend;
    KernelSignal reset;
    KernelSignal modulator;
};

class OscillatorKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(OscillatorKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] KernelProcessStatus process(const OscillatorKernelBlock& block) noexcept;

private:
    OscillatorKernelConfig config_;
    double phaseRadians_ = 0.0;
    bool prepared_ = false;
};

enum class FilterKernelMode
{
    LowPass,
    HighPass,
    AllPass,
};

struct FilterKernelConfig
{
    double sampleRate = 0.0;
    FilterKernelMode mode = FilterKernelMode::LowPass;
    std::size_t stageCount = 1;
    // As the Filter prototype: stageCount is its Order of poles, Scale is on
    // the input and Feedback goes around the cascade, subtracted for a
    // LowPass. Otherwise each stage is a two-pole section with Q, Scale is on
    // the output and Feedback a sample late.
    bool orderPoles = false;
    // A negative Feedback is the positive one 180 degrees out of phase
    // (format 92); older projects turn the loop's sign over.
    bool invertsNegativeFeedback = false;
    // With orderPoles, a LowPass or HighPass is a Butterworth filter of
    // stageCount poles, flat in its passband,
    // instead of identical one-pole sections.
    bool butterworth = false;
};

struct FilterKernelBlock
{
    std::span<const double> input;
    std::span<double> output;
    KernelSignal frequency;
    KernelSignal q = KernelSignal::constantValue(1.0);
    KernelSignal scale = KernelSignal::constantValue(1.0);
    KernelSignal feedback;
};

struct FilterKernelState
{
    std::array<double, 16> integratorOne{};
    std::array<double, 16> integratorTwo{};
    double feedbackSample = 0.0;
};

class FilterKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(FilterKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] FilterKernelState captureState() const noexcept;
    [[nodiscard]] KernelProcessStatus restoreState(
        const FilterKernelState& state) noexcept;
    [[nodiscard]] KernelProcessStatus process(const FilterKernelBlock& block) noexcept;

private:
    struct Stage
    {
        double ic1eq = 0.0;
        double ic2eq = 0.0;
    };

    FilterKernelConfig config_;
    std::array<Stage, 16> stages_{};
    double feedbackSample_ = 0.0;
    bool prepared_ = false;
};

struct AdsrKernelConfig
{
    double sampleRate = 0.0;
    // Exponential segments, which need a Scale
    // of at least 0.
    bool exponential = false;
    // A negative Gate decays rapidly to zero.
    bool negativeGateDecays = false;
    // A positive Gate falls this many seconds after
    // the envelope starts, so that its release ends with its Sound; never
    // when infinite.
    double constantGateFalls = std::numeric_limits<double>::infinity();
};

struct AdsrGateEvent
{
    std::uint32_t targetIndex = 0;
    std::size_t frameOffset = 0;
    std::uint64_t sequence = 0;
    bool high = false;
};

struct AdsrKernelBlock
{
    std::span<double> output;
    KernelSignal attackTime = KernelSignal::constantValue(0.01);
    KernelSignal decayTime = KernelSignal::constantValue(0.1);
    KernelSignal sustainLevel = KernelSignal::constantValue(0.7);
    KernelSignal releaseTime = KernelSignal::constantValue(0.25);
    KernelSignal scale = KernelSignal::constantValue(1.0);
    KernelSignal gate = KernelSignal::constantValue(1.0);
    KernelSignal legato;
    std::span<const AdsrGateEvent> typedGateEvents;
    std::uint32_t typedGateTargetIndex = 0;
    bool useTypedGateEvents = false;
};

struct AdsrKernelState
{
    double elapsedTime = 0.0;
    double currentValue = 0.0;
    bool gateWasPositive = false;
    double releaseElapsedTime = 0.0;
    double releaseStartValue = 0.0;
    // Seconds since the envelope started, for constantGateFalls.
    double localTime = 0.0;
};

class AdsrKernel
{
public:
    [[nodiscard]] KernelProcessStatus prepare(AdsrKernelConfig config) noexcept;
    void reset() noexcept;
    [[nodiscard]] AdsrKernelState captureState() const noexcept;
    [[nodiscard]] KernelProcessStatus restoreState(
        AdsrKernelState state) noexcept;
    [[nodiscard]] KernelProcessStatus applyGateEdge(
        bool high,
        double attackTime,
        double scale,
        double legato) noexcept;
    [[nodiscard]] KernelProcessStatus process(const AdsrKernelBlock& block) noexcept;

private:
    AdsrKernelConfig config_;
    AdsrKernelState state_;
    bool prepared_ = false;
};
}
