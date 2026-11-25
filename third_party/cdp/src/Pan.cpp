/*
 * Derived from the CDP System, Release 8: dev/modify/pan.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Pan.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// pan.c's ROOT2.
constexpr double root2 = 1.4142136;

// pancalc, statement for statement.
void panGains(double position, double& leftGain, double& rightGain) noexcept
{
    const auto signalToLeft = position < 0.0;
    const auto relativePosition = position < 0 ? -position : position;
    if (relativePosition <= 1.0)
    {
        auto temp = 1.0 + (relativePosition * relativePosition);
        const auto relativeDistance = root2 / std::sqrt(temp);
        temp = (position + 1.0) / 2.0;
        rightGain = temp * relativeDistance;
        leftGain = (1.0 - temp) * relativeDistance;
        return;
    }
    const auto temp = (relativePosition * relativePosition) + 1.0;
    const auto relativeDistance = std::sqrt(temp) / root2;
    const auto inverseSquare = 1.0 / (relativeDistance * relativeDistance);
    leftGain = signalToLeft ? inverseSquare : 0.0;
    rightGain = signalToLeft ? 0.0 : inverseSquare;
}
}

void Pan::reset() noexcept
{
    leftCoefficient_ = 0.0;
    rightCoefficient_ = 0.0;
    framesLeftInBlock_ = 0;
}

bool Pan::process(
    std::span<const double> input,
    double position,
    double prescale,
    std::span<double> left,
    std::span<double> right) noexcept
{
    if (input.size() != left.size() || input.size() != right.size()
        || !std::isfinite(position) || std::abs(position) > maximumPosition
        || !std::isfinite(prescale) || prescale < minimumPrescale
        || prescale > maximumPrescale
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // dopan computes the coefficients once per block.
        if (framesLeftInBlock_ == 0)
        {
            double leftGain = 0.0;
            double rightGain = 0.0;
            panGains(position, leftGain, rightGain);
            leftCoefficient_ = leftGain * prescale;
            rightCoefficient_ = rightGain * prescale;
            framesLeftInBlock_ = controlBlockFrames;
        }
        --framesLeftInBlock_;
        // CDP stores each product in its float sample buffer.
        left[frame] = static_cast<float>(leftCoefficient_ * input[frame]);
        right[frame] = static_cast<float>(rightCoefficient_ * input[frame]);
    }
    return true;
}
}
