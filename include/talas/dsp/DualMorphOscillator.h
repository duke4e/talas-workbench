#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace talas::dsp
{
struct DualMorphOscillatorFrame
{
    double primary = 0.0;
    double modulator = 0.0;
};

class DualMorphOscillator
{
public:
    [[nodiscard]] bool prepare(double sampleRate) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
            return false;
        prepared_ = true;
        sampleRate_ = sampleRate;
        reset();
        return true;
    }

    void reset() noexcept
    {
        primaryPhase_ = 0.0;
        modulatorPhase_ = 0.0;
    }

    [[nodiscard]] bool processSample(
        double frequency,
        double ratio,
        double phaseMod,
        double morph,
        double fold,
        double amplitude,
        DualMorphOscillatorFrame& output) noexcept
    {
        const auto nyquist = sampleRate_ * 0.5;
        const auto modulatorFrequency = frequency * ratio;
        if (!prepared_ || !std::isfinite(frequency) || frequency < 0.0
            || frequency >= nyquist || !std::isfinite(ratio) || ratio < 0.125
            || ratio > 16.0 || !std::isfinite(modulatorFrequency)
            || modulatorFrequency >= nyquist || !std::isfinite(phaseMod)
            || phaseMod < 0.0 || phaseMod > 2.0 || !std::isfinite(morph)
            || morph < 0.0 || morph > 1.0 || !std::isfinite(fold)
            || fold < 0.0 || fold > 1.0 || !std::isfinite(amplitude)
            || amplitude < 0.0 || amplitude > 8.0)
        {
            return false;
        }

        const auto modulatorAngle =
            2.0 * std::numbers::pi * modulatorPhase_;
        const auto modulator = std::sin(modulatorAngle);
        const auto phaseOffset = phaseMod * modulator;
        const auto modulatedPhase = wrapPhase(
            primaryPhase_ + phaseOffset);
        const auto primaryAngle =
            2.0 * std::numbers::pi * modulatedPhase;
        const auto sine = std::sin(primaryAngle);
        const auto triangle = (2.0 / std::numbers::pi)
            * std::asin(std::clamp(sine, -1.0, 1.0));
        const auto morphDelta = triangle - sine;
        const auto morphOffset = morphDelta * morph;
        const auto waveform = sine + morphOffset;
        const auto drive = 1.0 + 7.0 * fold;
        const auto foldAngle =
            0.5 * std::numbers::pi * waveform * drive;
        const auto foldedSine = std::sin(foldAngle);
        const auto folded = (2.0 / std::numbers::pi)
            * std::asin(foldedSine);
        const auto primary = folded * amplitude;
        const auto scaledModulator = modulator * amplitude;
        const auto primaryIncrement = frequency / sampleRate_;
        const auto modulatorIncrement = modulatorFrequency / sampleRate_;
        const auto nextPrimaryPhase = wrapPhase(
            primaryPhase_ + primaryIncrement);
        const auto nextModulatorPhase = wrapPhase(
            modulatorPhase_ + modulatorIncrement);
        if (!std::isfinite(modulatorAngle) || !std::isfinite(modulator)
            || !std::isfinite(phaseOffset)
            || !std::isfinite(modulatedPhase)
            || !std::isfinite(primaryAngle) || !std::isfinite(sine)
            || !std::isfinite(triangle) || !std::isfinite(morphDelta)
            || !std::isfinite(morphOffset) || !std::isfinite(waveform)
            || !std::isfinite(drive) || !std::isfinite(foldAngle)
            || !std::isfinite(foldedSine) || !std::isfinite(folded)
            || !std::isfinite(primary) || !std::isfinite(scaledModulator)
            || !std::isfinite(primaryIncrement)
            || !std::isfinite(modulatorIncrement)
            || !std::isfinite(nextPrimaryPhase)
            || !std::isfinite(nextModulatorPhase))
        {
            return false;
        }

        output.primary = primary;
        output.modulator = scaledModulator;
        primaryPhase_ = nextPrimaryPhase;
        modulatorPhase_ = nextModulatorPhase;
        return true;
    }

    [[nodiscard]] double primaryPhase() const noexcept
    {
        return primaryPhase_;
    }

    [[nodiscard]] double modulatorPhase() const noexcept
    {
        return modulatorPhase_;
    }

private:
    [[nodiscard]] static double wrapPhase(double phase) noexcept
    {
        return phase - std::floor(phase);
    }

    double sampleRate_ = 0.0;
    double primaryPhase_ = 0.0;
    double modulatorPhase_ = 0.0;
    bool prepared_ = false;
};
}
