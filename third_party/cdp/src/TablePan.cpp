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
#include "talas/cdp/TablePan.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; mchanpan.c's ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double ROOT2 = 1.4142136;
// After the last point CDP counts to the file's end; the stream never ends.
constexpr std::int64_t forever = std::numeric_limits<std::int64_t>::max() / 4;

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
}

bool TablePan::prepare(double sampleRate, int outputChannels, std::span<const Point> table, double focus)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (outputChannels < minimumChannels || outputChannels > maximumChannels || table.empty())
        return false;
    if (!inRange(focus, 0.0, 1.0))
        return false;
    // read_mchanpan_data.
    auto lasttime = -1.0;
    auto maxpos = -std::numeric_limits<double>::max();
    std::vector<std::int64_t> frames;
    for (std::size_t k = 0; k < table.size(); k++)
    {
        const auto& p = table[k];
        if (!std::isfinite(p.time) || p.time < 0.0 || p.time <= lasttime)
            return false;
        lasttime = p.time;
        if (!std::isfinite(p.position) || p.position < 0.0 || p.position > maximumPosition)
            return false;
        if (p.type < -1 || p.type > 1)
            return false;
        if (p.type == 0 && std::round(p.position) != p.position)
            return false;
        maxpos = p.position > maxpos ? p.position : maxpos;
        const auto f = p.time * sampleRate;
        if (!(f < 1e15))
            return false;
        frames.push_back(std::lround(f));
        if (k >= 1 && frames[k] <= (k == 1 ? 0 : frames[k - 1]))
            return false;
    }
    if (static_cast<double>(outputChannels) < std::ceil(maxpos))
        return false;
    sampleRate_ = sampleRate;
    channels_ = static_cast<std::size_t>(outputChannels);
    focus_ = focus;
    scalefact_ = (1.0 - focus) / 2.0;
    table_.assign(table.begin(), table.end());
    frames_ = std::move(frames);
    reset();
    return true;
}

void TablePan::reset() noexcept
{
    brkindex_ = 1;
    leftchan_ = 1;
    rightchan_ = 0;
    farleftchan_ = 0;
    farrightchan_ = 0;
    startbesideleft_ = 0;
    startbesideright_ = 0;
    goalbesideleft_ = 0;
    goalbesideright_ = 0;
    trueGoalchan_ = table_.empty() ? 0 : static_cast<int>(std::lround(table_[0].position));
    goalchan_ = trueGoalchan_ - 1;
    startchan_ = 0;
    pantype_ = 0;
    step_ = 0;
    block_ = 0;
    sams_ = 0;
    totalSams_ = 0;
    leftgain_ = rightgain_ = farleftgain_ = farrightgain_ = 0.0;
    lcoef_ = rcoef_ = llcoef_ = rrcoef_ = maxllcoef_ = 0.0;
    position_ = 0.0;
    posincr_ = 0.0;
}

// newposition.
std::int64_t TablePan::newposition() noexcept
{
    const auto outchans = static_cast<double>(channels_);
    std::int64_t sams = 0;
    if (brkindex_ < table_.size())
    {
        const auto& here = table_[brkindex_];
        const auto& before = table_[brkindex_ - 1];
        sams = frames_[brkindex_] - totalSams_;
        const auto steps = static_cast<double>(sams) / static_cast<double>(blockFrames);
        pantype_ = before.type;
        switch (pantype_)
        {
        case 0:
            trueGoalchan_ = static_cast<int>(std::lround(here.position));
            position_ = 0;
            posincr_ = 1.0 / steps;
            break;
        case 1:
        {
            position_ = before.position;
            const auto nextval = here.position;
            auto diff = std::fabs(nextval - position_);
            if (nextval < position_)
                diff = outchans - diff;
            posincr_ = diff / steps;
            break;
        }
        default:
        {
            position_ = before.position;
            const auto nextval = here.position;
            auto diff = std::fabs(nextval - position_);
            if (nextval > position_)
                diff = outchans - diff;
            posincr_ = -(diff / steps);
            break;
        }
        }
        brkindex_++;
    }
    else
    {
        posincr_ = 0.0;
        sams = forever;
    }
    totalSams_ += sams;
    if (pantype_ != 0)
        pantype_ = 1;
    return sams;
}

// mchan_pancalc, as CDP8's build fuses it.
void TablePan::pancalc() noexcept
{
    leftchan_ = static_cast<int>(std::floor(position_));
    const auto stereoposition = position_ - static_cast<double>(leftchan_);
    const auto zerocentredposition = std::fma(stereoposition, 2.0, -1.0);
    const auto relpos = zerocentredposition < 0 ? -zerocentredposition : zerocentredposition;
    const auto temp = std::fma(relpos, relpos, 1.0);
    const auto holecompensate = ROOT2 / std::sqrt(temp);
    rightgain_ = stereoposition * holecompensate;
    leftgain_ = (1.0 - stereoposition) * holecompensate;
    rightgain_ = std::fma(rightgain_, focus_, scalefact_);
    leftgain_ = std::fma(leftgain_, focus_, scalefact_);
    farleftgain_ = scalefact_ * (1.0 - stereoposition);
    farrightgain_ = scalefact_ * stereoposition;
    if (pantype_ == 1 && leftchan_ == 0)
        leftchan_ = static_cast<int>(channels_);
}

bool TablePan::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() * channels_)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto outchans = static_cast<int>(channels_);
    const auto outchansd = static_cast<double>(outchans);
    for (std::size_t i = 0; i < input.size(); i++)
    {
        auto gotNewposition = false;
        if (sams_-- <= 0)
        {
            startchan_ = goalchan_;
            sams_ = newposition();
            gotNewposition = true;
            goalchan_ = trueGoalchan_ - 1;
            if (pantype_ == 0)
            {
                startbesideleft_ = startchan_ - 1;
                if (startbesideleft_ < 0)
                    startbesideleft_ = outchans - 1;
                startbesideright_ = startchan_ + 1;
                if (startbesideright_ >= outchans)
                    startbesideright_ = 0;
                goalbesideleft_ = goalchan_ - 1;
                if (goalbesideleft_ < 0)
                    goalbesideleft_ = outchans - 1;
                goalbesideright_ = goalchan_ + 1;
                if (goalbesideright_ >= outchans)
                    goalbesideright_ = 0;
            }
        }
        const auto due = block_-- <= 0;
        if (due || gotNewposition)
        {
            pancalc();
            if (pantype_ == 1)
            {
                leftchan_--;
                farleftchan_ = (leftchan_ + outchans - 1) % outchans;
                rightchan_ = (leftchan_ + 1) % outchans;
                farrightchan_ = (rightchan_ + 1) % outchans;
            }
            lcoef_ = leftgain_;
            rcoef_ = rightgain_;
            llcoef_ = farleftgain_;
            rrcoef_ = farrightgain_;
            if (pantype_ == 0)
            {
                if (gotNewposition)
                {
                    maxllcoef_ = llcoef_;
                    step_ = std::abs(goalchan_ - startchan_);
                }
                if (step_ > 1 && step_ != outchans - 1)
                {
                    if (maxllcoef_ > 0)
                    {
                        const auto factor = llcoef_ / maxllcoef_;
                        lcoef_ *= factor;
                        rcoef_ *= (1.0 - factor);
                    }
                }
            }
            block_ += blockFrames;
            position_ += posincr_;
            if (pantype_ == 0)
            {
                if (position_ > 1.0)
                    position_ = 1.0;
                else if (position_ < 0.0)
                    position_ = 0.0;
            }
            else
            {
                if (position_ > outchansd)
                    position_ -= outchansd;
                else if (position_ < 0.0)
                    position_ += outchansd;
            }
        }
        const auto sample = static_cast<double>(static_cast<float>(input[i]));
        auto* frame = output.data() + i * channels_;
        for (int chan = 0; chan < outchans; chan++)
        {
            double chanMultiplier = 0.0;
            if (pantype_ == 0)
            {
                if (chan == startchan_)
                    chanMultiplier += lcoef_;
                if (chan == startbesideleft_ && startbesideleft_ != goalchan_)
                    chanMultiplier += llcoef_;
                if (chan == startbesideright_ && startbesideright_ != goalchan_)
                    chanMultiplier += llcoef_;
                if (chan == goalchan_)
                    chanMultiplier += rcoef_;
                if (chan == goalbesideleft_ && goalbesideleft_ != startchan_)
                    chanMultiplier += rrcoef_;
                if (chan == goalbesideright_ && goalbesideright_ != startchan_)
                    chanMultiplier += rrcoef_;
            }
            else
            {
                if (chan == farleftchan_)
                    chanMultiplier += llcoef_;
                if (chan == leftchan_)
                    chanMultiplier += lcoef_;
                if (chan == rightchan_)
                    chanMultiplier += rcoef_;
                if (chan == farrightchan_)
                    chanMultiplier += rrcoef_;
            }
            // *bufptr += (float)(chan_multiplier * inbuf[i]), into a zeroed buffer.
            const auto value = static_cast<float>(chanMultiplier * sample);
            frame[chan] = static_cast<double>(0.0F + value);
        }
    }
    return true;
}
}
