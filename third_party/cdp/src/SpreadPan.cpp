/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c and
 * dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpreadPan.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; mchanpan.c's ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double ROOT2 = 1.4142136;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// readdata.c: a value within FLTERR of a range end is that end.
[[nodiscard]] bool inRange(double& value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return false;
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    return !(value > hi || value < lo);
}

// sfsys.h's min and max.
[[nodiscard]] double cdpMin(double x, double y) noexcept
{
    return x > y ? y : x;
}

[[nodiscard]] double cdpMax(double x, double y) noexcept
{
    return x > y ? x : y;
}
}

bool SpreadPan::prepare(double sampleRate, int inputChannels, int outputChannels, int centre, double spread,
    double depth, double rolloff)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (inputChannels < 1 || inputChannels > 2)
        return false;
    if (outputChannels < minimumChannels || outputChannels > maximumChannels)
        return false;
    if (centre < 0 || centre > maximumCentre || centre > outputChannels)
        return false;
    if (!inRange(spread, 0.0, maximumSpread) || !inRange(depth, 0.0, maximumDepth) || !inRange(rolloff, 0.0, 1.0))
        return false;
    inChannels_ = static_cast<std::size_t>(inputChannels);
    outChannels_ = static_cast<std::size_t>(outputChannels);
    centreParam_ = centre;
    startSpread_ = spread;
    startDepth_ = depth;
    startRolloff_ = rolloff;
    reset();
    return true;
}

bool SpreadPan::setSpread(double spread) noexcept
{
    if (!prepared() || !inRange(spread, 0.0, maximumSpread))
        return false;
    spread_ = spread;
    return true;
}

bool SpreadPan::setDepth(double depth) noexcept
{
    if (!prepared() || !inRange(depth, 0.0, maximumDepth))
        return false;
    depth_ = depth;
    return true;
}

bool SpreadPan::setRolloff(double rolloff) noexcept
{
    if (!prepared() || !inRange(rolloff, 0.0, 1.0))
        return false;
    rolloff_ = rolloff;
    return true;
}

void SpreadPan::reset() noexcept
{
    spread_ = startSpread_;
    depth_ = startDepth_;
    rolloff_ = startRolloff_;
    centre_ = 0.0;
    levels_.fill(0.0);
    frame_ = 0;
}

// spread_pan, as CDP8's build fuses it.
void SpreadPan::spreadPan() noexcept
{
    const auto outchans = static_cast<int>(outChannels_);
    const auto doutchans = static_cast<double>(outchans);
    auto centre = static_cast<double>(centreParam_);
    const auto spread = cdpMin(spread_, doutchans);
    auto halfspread = spread / 2.0;
    auto depth = cdpMin(depth_, doutchans / 2.0);
    const auto rolloff = rolloff_;
    auto* levels = levels_.data();

    if (depth <= 0.0)
        depth = FLTERR;
    if ((halfspread = spread / 2.0) == 0.0)
        halfspread = FLTERR;
    if ((centre = centre - 1.0) < 0.0)
        centre += doutchans;
    for (int ochan = 0; ochan < outchans; ochan++)
        levels[ochan] = 0.0;

    double range = 0.0;
    if (spread < 1.0)
        range = 0.0;
    else
        range = 1.0 - (1.0 / spread);
    const auto maxlevel = std::fma(-rolloff, range, 1.0);

    auto spredejLeft = centre - halfspread;
    while (spredejLeft < 0)
        spredejLeft += doutchans;
    auto spredejRight = centre + halfspread;
    while (spredejRight >= outchans)
        spredejRight -= doutchans;
    const auto spredejLeftLeftchan = static_cast<int>(std::floor(spredejLeft));
    auto spredejLeftRightchan = spredejLeftLeftchan + 1;
    if (spredejLeftRightchan >= outchans)
        spredejLeftRightchan = 0;
    const auto spredejRightLeftchan = static_cast<int>(std::floor(spredejRight));

    const auto hole = std::fma(depth, -2.0, spread);

    if (spread >= outchans)
    {
        for (int k = 0; k < outchans; k++)
            levels[k] = maxlevel;
        if (hole <= 0.0)
            return;
    }
    else if (spread > 1.0)
    {
        auto k = spredejLeftLeftchan;
        if (spredejLeftLeftchan == spredejLeft)
            levels[k] = maxlevel;
        if (++k >= outchans)
            k -= outchans;
        while (k != spredejRightLeftchan)
        {
            levels[k] = maxlevel;
            if (++k >= outchans)
                k -= outchans;
        }
        levels[spredejRightLeftchan] = maxlevel;
    }
    else
    {
        auto zleft = centre - halfspread;
        auto zright = centre + halfspread;
        if (zleft < 0.0)
        {
            zleft += outchans;
            zright += outchans;
        }
        double floorZleft = 0.0;
        double floorZright = 0.0;
        if (zleft >= 0.0)
            floorZleft = std::floor(zleft);
        else
            floorZleft = -std::ceil(std::fabs(zleft));
        if (zright >= 0.0)
            floorZright = std::floor(zright);
        else
            floorZright = -std::ceil(std::fabs(zright));
        if (floorZleft < floorZright)
            levels[spredejRightLeftchan] = maxlevel;
    }

    const auto stereoposLeft = spredejLeft - static_cast<double>(spredejLeftLeftchan);
    auto relpos = std::fabs(0.5 - stereoposLeft) * 2.0;
    auto temp = std::fma(relpos, relpos, 1.0);
    auto holecompensate = ROOT2 / std::sqrt(temp);
    const auto leftLeftchanLevel = (1 - stereoposLeft) * maxlevel * holecompensate;
    if (spredejLeftLeftchan != spredejRightLeftchan)
        levels[spredejLeftLeftchan] = leftLeftchanLevel;

    const auto stereoposRight = spredejRight - static_cast<double>(spredejRightLeftchan);
    relpos = std::fabs(0.5 - stereoposRight) * 2.0;
    temp = std::fma(relpos, relpos, 1.0);
    holecompensate = ROOT2 / std::sqrt(temp);
    const auto rightRightchanLevel = stereoposRight * maxlevel * holecompensate;
    if (spredejRight > spredejRightLeftchan)
    {
        auto spredejRightRightchan = spredejRightLeftchan + 1;
        if (spredejRightRightchan >= outchans)
            spredejRightRightchan -= outchans;
        if (spredejRightRightchan != spredejLeftRightchan)
        {
            if (spredejRightRightchan == spredejLeftLeftchan)
                levels[spredejRightRightchan] += rightRightchanLevel;
            else
                levels[spredejRightRightchan] = rightRightchanLevel;
        }
    }

    halfspread = spread / 2.0;
    if (hole > 0.0)
    {
        const auto holejRight = centre + halfspread - depth;
        const auto holejLeft = centre - halfspread + depth;
        auto k = static_cast<int>(std::ceil(holejLeft));
        const auto j = static_cast<int>(std::floor(holejRight));
        while (k <= j)
        {
            const auto kk = k - holejLeft;
            const auto jj = holejRight - k;
            const auto mingap = cdpMin(jj, kk);
            auto holing = cdpMin(mingap, 1.0);
            holing = 1.0 - holing;
            auto ochan = k;
            while (ochan < 0)
                ochan += outchans;
            while (ochan >= outchans)
                ochan -= outchans;
            levels[ochan] *= holing;
            k++;
        }
    }
    centre_ = centre;
}

bool SpreadPan::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() % inChannels_ != 0 || output.size() != input.size() / inChannels_ * outChannels_)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto frames = input.size() / inChannels_;
    const auto outchans = static_cast<int>(outChannels_);
    const auto halfstage = outchans / 2;
    for (std::size_t i = 0; i < frames; i++)
    {
        if (frame_ % readFrames == 0)
            spreadPan();
        auto* obuf = output.data() + i * outChannels_;
        if (inChannels_ == 2)
        {
            for (std::size_t c = 0; c < outChannels_; c++)
                obuf[c] = 0.0;
            const auto left = static_cast<double>(static_cast<float>(input[2 * i]));
            const auto right = static_cast<double>(static_cast<float>(input[2 * i + 1]));
            const auto cLeft = static_cast<int>(std::floor(centre_));
            auto cRite = cLeft + 1;
            if (cRite >= outchans)
                cRite -= outchans;
            for (int k = 1; k < halfstage; k++)
            {
                auto m = cLeft - k;
                if (m < 0)
                    m += outchans;
                obuf[m] = static_cast<double>(static_cast<float>(left * levels_[static_cast<std::size_t>(m)]));
            }
            for (int k = 1; k < halfstage; k++)
            {
                auto m = cRite + k;
                if (m >= outchans)
                    m -= outchans;
                obuf[m] = static_cast<double>(static_cast<float>(right * levels_[static_cast<std::size_t>(m)]));
            }
            const auto stereoPos = centre_ - static_cast<double>(cLeft);
            const auto leftToCLeft = cdpMin(1.0, std::fma(2.0, stereoPos, 1.0) / 2.0);
            const auto riteToCLeft = cdpMax(0.0, std::fma(-2.0, stereoPos, 1.0) / 2.0);
            auto val = std::fma(left, leftToCLeft, right * riteToCLeft);
            val *= levels_[static_cast<std::size_t>(cLeft)];
            obuf[cLeft] = static_cast<double>(static_cast<float>(val));
            const auto leftToCRite = cdpMax(0.0, std::fma(2.0, stereoPos, -1.0) / 2.0);
            const auto riteToCRite = cdpMin(1.0, std::fma(-2.0, stereoPos, 3.0) / 2.0);
            val = std::fma(left, leftToCRite, right * riteToCRite);
            val *= levels_[static_cast<std::size_t>(cRite)];
            obuf[cRite] = static_cast<double>(static_cast<float>(val));
        }
        else
        {
            const auto sample = static_cast<double>(static_cast<float>(input[i]));
            for (std::size_t m = 0; m < outChannels_; m++)
                obuf[m] = static_cast<double>(static_cast<float>(sample * levels_[m]));
        }
        frame_++;
    }
    return true;
}
}
