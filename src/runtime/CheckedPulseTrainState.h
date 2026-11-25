#pragma once

#include <cmath>

namespace talas::runtime::checked_pulse_train {
// Counts whole samples since the current period began, so a period of
// whole samples repeats exactly; summing sample durations in seconds drifts.
struct State {
  double elapsedFrames = 0.0;
};

struct Controls {
  double period = 0.0;
  double dutyCycle = 0.0;
  double gate = 0.0;
  double sampleRate = 0.0;
  bool variableDutyCycle = false;
};

enum class Error {
  None,
  SampleRateNonFiniteOrNonPositive,
  SampleDurationNonFinite,
  PeriodNonFiniteOrNonPositive,
  DutyCycleNonFiniteOrOutOfRange,
  GateNonFinite,
  ElapsedTimeNonFiniteOrNegative,
  PositionNonFinite,
  DutyThresholdNonFinite,
  FoldedElapsedTimeNonFinite,
};

[[nodiscard]] inline Error validateControls(const Controls &controls) noexcept {
  if (!std::isfinite(controls.sampleRate) || controls.sampleRate <= 0.0)
    return Error::SampleRateNonFiniteOrNonPositive;
  const auto sampleDuration = 1.0 / controls.sampleRate;
  if (!std::isfinite(sampleDuration))
    return Error::SampleDurationNonFinite;
  if (!std::isfinite(controls.period) || controls.period <= 0.0)
    return Error::PeriodNonFiniteOrNonPositive;
  if (controls.variableDutyCycle &&
      (!std::isfinite(controls.dutyCycle) || controls.dutyCycle < 0.0 ||
       controls.dutyCycle > 1.0)) {
    return Error::DutyCycleNonFiniteOrOutOfRange;
  }
  if (!std::isfinite(controls.gate))
    return Error::GateNonFinite;
  return Error::None;
}

[[nodiscard]] inline Error advance(const State &state,
                                   const Controls &controls, State &candidate,
                                   double &output) noexcept {
  if (const auto error = validateControls(controls); error != Error::None)
    return error;
  if (!std::isfinite(state.elapsedFrames) || state.elapsedFrames < 0.0)
    return Error::ElapsedTimeNonFiniteOrNegative;

  if (controls.gate <= 0.0) {
    candidate = {};
    output = 0.0;
    return Error::None;
  }

  auto periodFrames = controls.period * controls.sampleRate;
  if (!std::isfinite(periodFrames) || periodFrames <= 0.0)
    return Error::PeriodNonFiniteOrNonPositive;
  // A whole number of samples given in seconds can come back a rounding
  // error over, which would put every later pulse a sample late.
  if (const auto whole = std::round(periodFrames);
      whole >= 1.0 && std::abs(periodFrames - whole) <= 1.0e-9 * whole)
    periodFrames = whole;
  const auto position = std::fmod(state.elapsedFrames, periodFrames);
  if (!std::isfinite(position))
    return Error::PositionNonFinite;

  // One sample per period is high unless the duty cycle is variable.
  auto isHigh = position < 1.0;
  if (controls.variableDutyCycle) {
    const auto dutyThreshold = periodFrames * controls.dutyCycle;
    if (!std::isfinite(dutyThreshold))
      return Error::DutyThresholdNonFinite;
    isHigh = controls.dutyCycle >= 1.0 ||
             (controls.dutyCycle > 0.0 && position < dutyThreshold);
  }

  // A finite count stays finite after one more frame.
  const auto advanced = state.elapsedFrames + 1.0;
  auto nextElapsedFrames = advanced;
  if (advanced >= periodFrames) {
    nextElapsedFrames = std::fmod(advanced, periodFrames);
    if (!std::isfinite(nextElapsedFrames))
      return Error::FoldedElapsedTimeNonFinite;
  }

  candidate = {nextElapsedFrames};
  output = isHigh ? 1.0 : 0.0;
  return Error::None;
}
} // namespace talas::runtime::checked_pulse_train
