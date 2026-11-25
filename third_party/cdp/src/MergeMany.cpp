/*
 * Derived from the CDP System, Release 8: dev/submix/mixmerge.c and
 * dev/submix/ap_mix.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/MergeMany.h"

#include <cmath>

namespace talas::cdp
{
bool MergeMany::prepare(int inputs, int channels, double peak) noexcept
{
    if (inputs < minimumInputs || inputs > maximumInputs || channels < 1 || channels > maximumChannels
        || !std::isfinite(peak) || !(peak > 0.0))
        return false;
    inputs_ = static_cast<std::size_t>(inputs);
    channels_ = static_cast<std::size_t>(channels);
    // mixmany: maxdsamp = F_MAXSAMP / maxdsamp.
    gain_ = 1.0 / peak;
    return true;
}

bool MergeMany::process(std::span<const std::span<const double>> inputs, std::span<double> output) noexcept
{
    if (!prepared() || inputs.size() != inputs_ || output.size() % channels_ != 0)
        return false;
    for (const auto input : inputs)
    {
        if (input.size() != output.size())
            return false;
        for (const double sample : input)
        {
            if (!std::isfinite(sample))
                return false;
        }
    }
    for (std::size_t m = 0; m < output.size(); m++)
    {
        // dblbuf[m] += (double)ibuf[m], file by file.
        double sum = 0.0;
        for (const auto input : inputs)
            sum += static_cast<double>(static_cast<float>(input[m]));
        output[m] = static_cast<double>(static_cast<float>(sum * gain_));
    }
    return true;
}
}
