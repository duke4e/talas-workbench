/*
 * Derived from the CDP System, Release 8: dev/modify/gain.c and
 * dev/standalone/phase.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/PhaseInvert.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
bool PhaseInvert::prepare(Mode mode) noexcept
{
    if (mode < Mode::Loudness || mode > Mode::Phase)
        return false;
    prepared_ = true;
    mode_ = mode;
    return true;
}

void PhaseInvert::reset() noexcept
{
}

bool PhaseInvert::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const auto sample = static_cast<double>(static_cast<float>(input[i]));
        double samplong = 0.0;
        if (mode_ == Mode::Loudness)
        {
            // do_phase_invert: F_MAXSAMP is 1.
            samplong = -sample;
            samplong = std::min(samplong, 1.0);
        }
        else
        {
            // phase, mode 1.
            samplong = sample * -1.0;
        }
        output[i] = static_cast<double>(static_cast<float>(samplong));
    }
    return true;
}
}
