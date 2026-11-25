#pragma once

#include "EnvelopeSegmentShape.h"

#include <algorithm>
#include <cmath>

namespace talas::runtime::checked_ar_envelope {
struct State {
  double elapsedTime = 0.0;
  bool triggered = false;
  double currentValue = 0.0;
  bool gateWasPositive = false;
  double releaseElapsedTime = 0.0;
  double releaseStartValue = 0.0;
};

struct Controls {
  double attackTime = 0.0;
  double releaseTime = 0.0;
  double scale = 0.0;
  double gate = 0.0;
  double sampleRate = 0.0;
  // Exponential segments need a Scale of at least 0.
  bool exponential = false;
  // A negative Gate decays rapidly to zero (format 88).
  bool negativeGateDecays = false;
};

enum class Error {
  None,
  SampleRateNonFiniteOrNonPositive,
  SampleDurationNonFinite,
  AttackTimeNonFiniteOrNonPositive,
  ReleaseTimeNonFiniteOrNonPositive,
  ScaleNonFinite,
  GateNonFinite,
  ElapsedTimeNonFinite,
  CurrentValueNonFinite,
  ReleaseElapsedTimeNonFinite,
  ReleaseStartValueNonFinite,
  RetriggerRatioNonFinite,
  RetriggerElapsedTimeNonFinite,
  AttackRatioNonFinite,
  AttackOutputNonFinite,
  AdvancedElapsedTimeNonFinite,
  ReleaseRatioNonFinite,
  ReleaseAmountNonFinite,
  ReleaseOutputNonFinite,
  AdvancedReleaseElapsedTimeNonFinite,
  ExponentialScaleNegative,
};

[[nodiscard]] inline Error advance(const State &state, const Controls &controls,
                                   State &candidate, double &output) noexcept {
  if (!std::isfinite(controls.sampleRate) || controls.sampleRate <= 0.0) {
    return Error::SampleRateNonFiniteOrNonPositive;
  }
  const auto sampleDuration = 1.0 / controls.sampleRate;
  if (!std::isfinite(sampleDuration))
    return Error::SampleDurationNonFinite;
  if (!std::isfinite(controls.attackTime) || controls.attackTime <= 0.0) {
    return Error::AttackTimeNonFiniteOrNonPositive;
  }
  if (!std::isfinite(controls.releaseTime) || controls.releaseTime <= 0.0) {
    return Error::ReleaseTimeNonFiniteOrNonPositive;
  }
  if (!std::isfinite(controls.scale))
    return Error::ScaleNonFinite;
  if (controls.exponential && controls.scale < 0.0)
    return Error::ExponentialScaleNegative;
  if (!std::isfinite(controls.gate))
    return Error::GateNonFinite;
  if (!std::isfinite(state.elapsedTime))
    return Error::ElapsedTimeNonFinite;
  if (!std::isfinite(state.currentValue))
    return Error::CurrentValueNonFinite;
  if (!std::isfinite(state.releaseElapsedTime))
    return Error::ReleaseElapsedTimeNonFinite;
  if (!std::isfinite(state.releaseStartValue))
    return Error::ReleaseStartValueNonFinite;

  auto next = state;
  auto nextOutput = 0.0;
  if (controls.gate > 0.0) {
    if (!next.gateWasPositive) {
      if (controls.scale == 0.0) {
        next.elapsedTime = 0.0;
      } else {
        const auto ratio = next.currentValue / controls.scale;
        if (!std::isfinite(ratio))
          return Error::RetriggerRatioNonFinite;
        const auto elapsed =
            envelope_segment::ratioFor(ratio, controls.exponential) *
            controls.attackTime;
        if (!std::isfinite(elapsed))
          return Error::RetriggerElapsedTimeNonFinite;
        next.elapsedTime = elapsed;
      }
      next.triggered = true;
    }

    next.gateWasPositive = true;
    next.releaseElapsedTime = 0.0;
    next.releaseStartValue = next.currentValue;
    const auto ratio = next.elapsedTime / controls.attackTime;
    if (!std::isfinite(ratio))
      return Error::AttackRatioNonFinite;
    const auto amount = envelope_segment::covered(ratio, controls.exponential);
    const auto value = controls.scale * amount;
    if (!std::isfinite(value))
      return Error::AttackOutputNonFinite;
    next.currentValue = value;
    nextOutput = value;

    const auto advanced = next.elapsedTime + sampleDuration;
    if (!std::isfinite(advanced))
      return Error::AdvancedElapsedTimeNonFinite;
    next.elapsedTime =
        advanced > controls.attackTime ? controls.attackTime : advanced;
  } else if (!next.gateWasPositive && next.currentValue == 0.0) {
    // Preserve the established idle fast-path state without bypassing
    // selected-control and committed-state validation.
    next = {};
    next.releaseElapsedTime = controls.releaseTime;
    nextOutput = 0.0;
  } else {
    if (next.gateWasPositive) {
      next.releaseElapsedTime = 0.0;
      next.releaseStartValue = next.currentValue;
    }
    next.gateWasPositive = false;
    next.triggered = false;
    next.elapsedTime = 0.0;

    const auto ratio = next.releaseElapsedTime / controls.releaseTime;
    if (!std::isfinite(ratio))
      return Error::ReleaseRatioNonFinite;
    const auto oneMinusRatio = 1.0 - ratio;
    if (!std::isfinite(oneMinusRatio))
      return Error::ReleaseAmountNonFinite;
    const auto amount =
        controls.exponential ? 1.0 - envelope_segment::covered(ratio, true)
                             : std::clamp(oneMinusRatio, 0.0, 1.0);
    const auto value = next.releaseStartValue * amount;
    if (!std::isfinite(value))
      return Error::ReleaseOutputNonFinite;
    next.currentValue = value;
    nextOutput = value;

    const auto advanced = next.releaseElapsedTime
        + sampleDuration * envelope_segment::releaseSpeed(
              controls.releaseTime, controls.gate, controls.negativeGateDecays);
    if (!std::isfinite(advanced))
      return Error::AdvancedReleaseElapsedTimeNonFinite;
    next.releaseElapsedTime = advanced;
    if (next.releaseElapsedTime >= controls.releaseTime) {
      next.releaseElapsedTime = controls.releaseTime;
      next.releaseStartValue = 0.0;
      next.currentValue = 0.0;
    }
  }

  candidate = next;
  output = nextOutput;
  return Error::None;
}
} // namespace talas::runtime::checked_ar_envelope
