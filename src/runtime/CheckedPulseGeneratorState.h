#pragma once

#include <cmath>

namespace talas::runtime::checked_pulse_generator {
struct State {
  double phase = 0.0;
};

struct Controls {
  double frequency = 0.0;
  double maxMI = 0.0;
  double envelope = 0.0;
  double dutyCycle = 0.0;
  double reset = 0.0;
  double modulator = 0.0;
  double sampleRate = 0.0;
  bool applyFrequencyModulation = false;
  bool linearInterpolation = true;
  bool holdPhaseAtDutyBoundary = false;
};

enum class Error {
  None,
  SampleRateNonFiniteOrNonPositive,
  NyquistNonFinite,
  FrequencyNonFiniteOrOutOfRange,
  MaxMiNonFiniteOrNegative,
  EnvelopeNonFinite,
  DutyCycleNonFiniteOrOutOfRange,
  ResetNonFinite,
  ModulatorNonFinite,
  ModulationArithmeticNonFinite,
  EffectiveFrequencyNonFiniteOrOutOfRange,
  PhaseNonFiniteOrOutOfRange,
  PhaseIncrementNonFinite,
  PulseLevelNonFinite,
  PolyBlepArithmeticNonFinite,
  FallingPhaseNonFinite,
  PulseNonFinite,
  OutputNonFinite,
  AdvancedPhaseNonFinite,
  WrappedPhaseNonFiniteOrOutOfRange,
};

[[nodiscard]] inline Error selectedFrequency(
    const Controls &controls, double &effectiveFrequency) noexcept {
  if (!std::isfinite(controls.sampleRate) || controls.sampleRate <= 0.0)
    return Error::SampleRateNonFiniteOrNonPositive;
  const auto nyquist = controls.sampleRate * 0.5;
  if (!std::isfinite(nyquist))
    return Error::NyquistNonFinite;
  if (!std::isfinite(controls.frequency) || controls.frequency < 0.0 ||
      controls.frequency >= nyquist) {
    return Error::FrequencyNonFiniteOrOutOfRange;
  }
  if (!std::isfinite(controls.maxMI) || controls.maxMI < 0.0)
    return Error::MaxMiNonFiniteOrNegative;

  auto selected = controls.frequency;
  if (controls.applyFrequencyModulation) {
    if (!std::isfinite(controls.modulator))
      return Error::ModulatorNonFinite;
    const auto modulationAmount = controls.modulator * controls.maxMI;
    const auto modulationFactor = 1.0 + modulationAmount;
    if (!std::isfinite(modulationAmount) ||
        !std::isfinite(modulationFactor)) {
      return Error::ModulationArithmeticNonFinite;
    }
    selected = controls.frequency * modulationFactor;
  }
  if (!std::isfinite(selected) || selected < 0.0 || selected >= nyquist)
    return Error::EffectiveFrequencyNonFiniteOrOutOfRange;
  effectiveFrequency = selected;
  return Error::None;
}

[[nodiscard]] inline Error validateControls(
    const Controls &controls, double &effectiveFrequency) noexcept {
  if (const auto error = selectedFrequency(controls, effectiveFrequency);
      error != Error::None) {
    return error;
  }
  if (!std::isfinite(controls.envelope))
    return Error::EnvelopeNonFinite;
  if (!std::isfinite(controls.dutyCycle) || controls.dutyCycle < 0.0 ||
      controls.dutyCycle > 1.0) {
    return Error::DutyCycleNonFiniteOrOutOfRange;
  }
  if (!std::isfinite(controls.reset))
    return Error::ResetNonFinite;
  return Error::None;
}

[[nodiscard]] inline Error polyBlep(
    double phase, double phaseIncrement, double &candidate) noexcept {
  auto result = 0.0;
  if (phaseIncrement <= 0.0) {
    candidate = result;
    return Error::None;
  }

  if (phase < phaseIncrement) {
    const auto t = phase / phaseIncrement;
    const auto doubled = t + t;
    const auto squared = t * t;
    const auto withoutOffset = doubled - squared;
    result = withoutOffset - 1.0;
    if (!std::isfinite(t) || !std::isfinite(doubled) ||
        !std::isfinite(squared) || !std::isfinite(withoutOffset) ||
        !std::isfinite(result)) {
      return Error::PolyBlepArithmeticNonFinite;
    }
    candidate = result;
    return Error::None;
  }

  const auto finalEdge = 1.0 - phaseIncrement;
  if (!std::isfinite(finalEdge))
    return Error::PolyBlepArithmeticNonFinite;
  if (phase > finalEdge) {
    const auto numerator = phase - 1.0;
    const auto t = numerator / phaseIncrement;
    const auto squared = t * t;
    const auto plusFirst = squared + t;
    const auto plusSecond = plusFirst + t;
    result = plusSecond + 1.0;
    if (!std::isfinite(numerator) || !std::isfinite(t) ||
        !std::isfinite(squared) || !std::isfinite(plusFirst) ||
        !std::isfinite(plusSecond) || !std::isfinite(result)) {
      return Error::PolyBlepArithmeticNonFinite;
    }
  }
  candidate = result;
  return Error::None;
}

[[nodiscard]] inline Error advance(const State &state,
                                   const Controls &controls, State &candidate,
                                   double &output) noexcept {
  if (!std::isfinite(state.phase) || state.phase < 0.0 || state.phase >= 1.0)
    return Error::PhaseNonFiniteOrOutOfRange;

  auto effectiveFrequency = 0.0;
  if (const auto error = validateControls(controls, effectiveFrequency);
      error != Error::None) {
    return error;
  }

  auto selectedState = state;
  if (controls.reset != 0.0)
    selectedState = {};

  if (effectiveFrequency == 0.0 ||
      (controls.holdPhaseAtDutyBoundary &&
       (controls.dutyCycle == 0.0 || controls.dutyCycle == 1.0))) {
    candidate = selectedState;
    output = 0.0;
    return Error::None;
  }

  const auto phaseIncrement = effectiveFrequency / controls.sampleRate;
  if (!std::isfinite(phaseIncrement) || phaseIncrement <= 0.0)
    return Error::PhaseIncrementNonFinite;
  const auto high = 2.0 * (1.0 - controls.dutyCycle);
  const auto low = -2.0 * controls.dutyCycle;
  if (!std::isfinite(high) || !std::isfinite(low))
    return Error::PulseLevelNonFinite;

  auto pulse = selectedState.phase < controls.dutyCycle ? high : low;
  if (controls.linearInterpolation) {
    auto rising = 0.0;
    if (const auto error = polyBlep(
            selectedState.phase, phaseIncrement, rising);
        error != Error::None) {
      return error;
    }
    const auto withRising = pulse + rising;
    if (!std::isfinite(withRising))
      return Error::PulseNonFinite;
    pulse = withRising;

    auto fallingPhase = selectedState.phase - controls.dutyCycle;
    if (!std::isfinite(fallingPhase))
      return Error::FallingPhaseNonFinite;
    if (fallingPhase < 0.0)
      fallingPhase += 1.0;
    if (!std::isfinite(fallingPhase))
      return Error::FallingPhaseNonFinite;
    auto falling = 0.0;
    if (const auto error = polyBlep(
            fallingPhase, phaseIncrement, falling);
        error != Error::None) {
      return error;
    }
    const auto withFalling = pulse - falling;
    if (!std::isfinite(withFalling))
      return Error::PulseNonFinite;
    pulse = withFalling;
  }
  if (!std::isfinite(pulse))
    return Error::PulseNonFinite;

  const auto candidateOutput = pulse * controls.envelope;
  if (!std::isfinite(candidateOutput))
    return Error::OutputNonFinite;

  const auto advancedPhase = selectedState.phase + phaseIncrement;
  if (!std::isfinite(advancedPhase))
    return Error::AdvancedPhaseNonFinite;
  auto nextPhase = advancedPhase;
  if (advancedPhase >= 1.0) {
    const auto wholeCycles = std::floor(advancedPhase);
    if (!std::isfinite(wholeCycles))
      return Error::WrappedPhaseNonFiniteOrOutOfRange;
    nextPhase = advancedPhase - wholeCycles;
  }
  if (!std::isfinite(nextPhase) || nextPhase < 0.0 || nextPhase >= 1.0)
    return Error::WrappedPhaseNonFiniteOrOutOfRange;

  candidate = {nextPhase};
  output = candidateOutput;
  return Error::None;
}
} // namespace talas::runtime::checked_pulse_generator
