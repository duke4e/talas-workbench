/*
 * Derived from the CDP System, Release 8: dev/stretch/stretch.c and
 * ap_stretch.c, and dev/cdp2k/readdata.c, tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "SpectralWholeFileEmulation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// `stretch time 1 infile outfile timestretch` (TSTRETCH, mode TSTR_NORMAL).
// Mode 2 (TSTR_LENGTH) only prints the output's length, so it is not here.
//
// CDP's quirks, kept:
// - The windows are read 20 to a buffer. advance_along_input_windows
//   reloads only once the next-window pointer passes the buffer's end
//   (`>` where `>=` was meant): stepping to the buffer's 20th window
//   leaves it pointing at the output buffer's first window (the latest
//   output window written there), and the reload that follows reads the
//   file's next window as the one after that. So each buffer boundary
//   crossed slips the input one window later, and the window interpolated
//   to at each boundary is an output window.
// - At the file's last window (atend) the advance stops a window short and
//   interpolates from the window it reached to itself.
// - Positions are worked out 200 at a time (POSMIN). A segment of a table
//   counts as "stretch decreasing" when its window count is not negative,
//   which calculate_number_of_output_windows (log(a / b) * T / (b - a))
//   never gives a segment with windows: every changing segment takes the
//   increasing path, and the decreasing path, whose
//   retrograde_sequence_of_time_intervals puts a block's last position at
//   its start, only ever sees empty segments (two times rounding to one
//   window). It is ported as it is.
// - A breakpoint table that ends before the sound ends the output at its
//   last time; one that runs past the end is cut at the sound's end, its
//   last factor interpolated there.
// - Interpolation, amp0 + (amp1 - amp0) * frac in double, and a position
//   dur * n + startwindow, each round once (CDP8's build fuses them).
namespace talas::cdp::wholefile
{
namespace
{
// speccon.h.
constexpr double MINTSTR = 0.0001;
constexpr double MAXTSTR = 10000.0;
// ap_stretch.c's POSMIN, TSTR_ARRAYSIZE.
constexpr int POSMIN = 200;

constexpr int FINISHED = 0;
constexpr int CONTINUE = 1;

// force_value_at_zero_time (tklib3.c).
void forceValueAtZeroTime(std::vector<double>& brk)
{
    auto brksize = static_cast<long>(brk.size() / 2);
    if (flteq(brk[0], 0.0))
    {
        brk[0] = 0.0;
        return;
    }
    if (brk[0] > 0.0)
    {
        brk.insert(brk.begin(), 2, 0.0);
        brk[0] = 0.0;
        brk[1] = brk[3];
        return;
    }
    long p = 0;
    const long pend = brksize * 2;
    while (brk[static_cast<std::size_t>(p)] < 0.0)
    {
        p += 2;
        if (p >= pend)
        {
            p -= 2;
            const double value = brk[static_cast<std::size_t>(p + 1)];
            brk = {0.0, value};
            return;
        }
    }
    if (brk[static_cast<std::size_t>(p)] > 0.0)
    {
        const double thistime = brk[static_cast<std::size_t>(p)];
        const double lasttime = brk[static_cast<std::size_t>(p - 2)];
        const double timediff = thistime - lasttime;
        const double timeratio = -(lasttime) / timediff;
        const double thisval = brk[static_cast<std::size_t>(p + 1)];
        const double lastval = brk[static_cast<std::size_t>(p - 1)];
        const double valdiff = thisval - lastval;
        // lastval + (valdiff * timeratio): fused, as CDP8's build fuses it.
        const double val = std::fma(valdiff, timeratio, lastval);
        p -= 2;
        brk[static_cast<std::size_t>(p)] = 0.0;
        brk[static_cast<std::size_t>(p + 1)] = val;
    }
    brk.erase(brk.begin(), brk.begin() + p);
}

class TimeStretch
{
public:
    TimeStretch(const AnaFile& file, double stretch, std::vector<double> brk)
        : in_(file), wanted_(file.wanted), wlength_(file.wlength), frametime_(file.frametime),
          stretch_(stretch), brk_(std::move(brk)),
          // allocate_tstretch_buffer: a window, the input buffer and the
          // output buffer, 20 windows each.
          big_(static_cast<std::size_t>(file.wanted) * BUF_MULTIPLIER * 2 + static_cast<std::size_t>(file.wanted)),
          pbuf_(POSMIN, 0.0), pbufDefined_(POSMIN, 0), qbuf_(POSMIN, 0.0)
    {
        bigfsize_ = static_cast<long>(wanted_) * BUF_MULTIPLIER;
        bigfbuf_ = Ptr{&big_, 0};
        flbufptr_[0] = bigfbuf_ + wanted_;
        flbufptr_[1] = flbufptr_[0] + bigfsize_;
        flbufptr_[2] = flbufptr_[1] + bigfsize_;
        flbufptr_[3] = flbufptr_[0];
        flbufptr_[4] = flbufptr_[0] + wanted_;
        flbufptr_[5] = flbufptr_[1];
    }

    std::vector<float> run()
    {
        // setup_internal_params_for_tstretch.
        if (!brk_.empty())
            forceValueAtZeroTime(brk_);
        totime_ = (double)wlength_ * frametime_;
        // spectstretch, TSTR_NORMAL.
        (void)in_.read(flbufptr_[0], static_cast<int>(bigfsize_));
        flbufptr_[3] = flbufptr_[0];
        flbufptr_[4] = flbufptr_[0] + wanted_;
        int thatwindow = 0;
        if (brk_.empty())
            doConstantTimestretch(&thatwindow);
        else
            doTimevariableTimestretch(&thatwindow);
        doTimestretching(&thatwindow, ptr_);
        if (flbufptr_[5] - flbufptr_[1] > 0)
            out_.write(flbufptr_[1], flbufptr_[5] - flbufptr_[1]);
        return std::move(out_.data());
    }

private:
    void doConstantTimestretch(int* thatwindow)
    {
        ptr_ = 0;
        const double thiswdur = 1.0 / stretch_;
        const double dcount = (double)wlength_ / thiswdur;
        const int count = cdpRound(std::fabs(dcount));
        const double dur = (double)wlength_ / (double)count;
        divideTimeIntoEqualIncrements(thatwindow, 0, dur, count);
    }

    int getBothValsFromBrktable(double* thistime, double* thisstretch)
    {
        if (brkptr_ >= brk_.size())
            return FINISHED;
        *thistime = brk_[brkptr_++];
        if (brkptr_ >= brk_.size())
            refuse("CDP finds an anomaly in its stretch table (get_both_vals_from_brktable).");
        *thisstretch = brk_[brkptr_++];
        return CONTINUE;
    }

    void doTimevariableTimestretch(int* thatwindow)
    {
        double dtime0 = 0.0, stretch0 = 0.0, dtime1 = 0.0, stretch1 = 0.0;
        bool atend = false;
        int startwindow = 0;
        bool timestretchCalled = false;
        ptr_ = 0;
        if (getBothValsFromBrktable(&dtime0, &stretch0) == FINISHED)
            refuse("CDP finds too little data in its stretch table (do_timevariable_timestretch).");
        int time0 = cdpRound(dtime0 / frametime_);
        double thiswdur = 1.0 / stretch0;
        while (getBothValsFromBrktable(&dtime1, &stretch1) == CONTINUE)
        {
            double nextwdur = 1.0 / stretch1;
            int time1 = cdpRound(dtime1 / frametime_);
            if (time1 > wlength_)
            {
                const int time2 = time1;
                time1 = cdpRound(totime_ / frametime_);
                double valdiff = nextwdur - thiswdur;
                valdiff *= (double)(time1 - time0) / (double)(time2 - time0);
                nextwdur = thiswdur + valdiff;
                atend = true;
            }
            const int totaldur = time1 - time0;
            timestretchCalled = true;
            timestretchThisSegment(thatwindow, startwindow, thiswdur, nextwdur, totaldur);
            startwindow += totaldur;
            thiswdur = nextwdur;
            time0 = time1;
            if (atend)
                break;
        }
        if (!timestretchCalled)
            refuse("CDP finds too little data in its stretch table (do_timevariable_timestretch).");
    }

    [[nodiscard]] double position(int index) const
    {
        if (index < 0 || index >= POSMIN || pbufDefined_[static_cast<std::size_t>(index)] == 0)
            refuse("CDP would read a position it never computed here, so its result is undefined.");
        return pbuf_[static_cast<std::size_t>(index)];
    }

    void store(int index, double value)
    {
        pbuf_[static_cast<std::size_t>(index)] = value;
        pbufDefined_[static_cast<std::size_t>(index)] = 1;
    }

    void doTimestretching(int* thatwindow, int count)
    {
        bool atend = false;
        for (int n = 0; n < count; n++)
        {
            const double here = position(n);
            const int thiswindow = truncateToInt(here);
            const int step = thiswindow - *thatwindow;
            if (thiswindow == wlength_ - 1)
                atend = true;
            if (step)
                advanceAlongInputWindows(step, atend);
            const double frac = here - (double)thiswindow;
            const Ptr a = flbufptr_[3];
            const Ptr b = flbufptr_[4];
            const Ptr o = flbufptr_[5];
            for (int vc = 0; vc < wanted_; vc += 2)
            {
                const double amp0 = a.get(vc);
                const double phas0 = a.get(vc + 1);
                const double amp1 = b.get(vc);
                const double phas1 = b.get(vc + 1);
                const bool ampDefined = a.isDefined(vc) && b.isDefined(vc);
                const bool frqDefined = a.isDefined(vc + 1) && b.isDefined(vc + 1);
                o.set(vc, (float)std::fma(amp1 - amp0, frac, amp0), ampDefined);
                o.set(vc + 1, (float)std::fma(phas1 - phas0, frac, phas0), frqDefined);
            }
            if ((flbufptr_[5] += wanted_) >= flbufptr_[2])
            {
                out_.write(flbufptr_[1], bigfsize_);
                flbufptr_[5] = flbufptr_[1];
            }
            *thatwindow = thiswindow;
        }
    }

    void divideTimeIntoEqualIncrements(int* thatwindow, int startwindow, double dur, int count)
    {
        int end = POSMIN - ptr_;
        int start = 0;
        while (count >= end)
        {
            for (int n = start; n < end; n++)
                store(ptr_++, std::fma(dur, (double)n, (double)startwindow));
            doTimestretching(thatwindow, POSMIN);
            ptr_ = 0;
            start = end;
            end += POSMIN;
        }
        if (count - start > 0)
        {
            for (int n = start; n < count; n++)
                store(ptr_++, std::fma(dur, (double)n, (double)startwindow));
        }
    }

    void timestretchThisSegment(int* thatwindow, int startwindow, double thiswdur, double nextwdur, int totaldur)
    {
        bool stretchDecreasing = true;
        const double dcount = calculateNumberOfOutputWindows(thiswdur, nextwdur, totaldur);
        if (dcount < 0.0)
            stretchDecreasing = false;
        const int count = cdpRound(std::fabs(dcount));
        if (std::fabs(nextwdur - thiswdur) <= FLTERR)
        {
            const double dur = (double)totaldur / (double)count;
            divideTimeIntoEqualIncrements(thatwindow, startwindow, dur, count);
            return;
        }
        const double param0 = nextwdur - thiswdur;
        const double param1 = param0 / (double)totaldur;
        const double param2 = thiswdur * (double)totaldur;
        if (stretchDecreasing)
            positionsForDecreasingStretch(thatwindow, startwindow, count, totaldur, param0, param1, param2);
        else
            positionsForIncreasingStretch(thatwindow, startwindow, count, param0, param1, param2);
    }

    void advanceAlongInputWindows(int wdwToAdvance, bool atend)
    {
        const int count = atend ? wdwToAdvance - 1 : wdwToAdvance;
        for (int n = 0; n < count; n++)
        {
            flbufptr_[3] = flbufptr_[4];
            if ((flbufptr_[4] += wanted_) > flbufptr_[1])
            {
                move(bigfbuf_, flbufptr_[3], wanted_);
                flbufptr_[3] = bigfbuf_;
                flbufptr_[4] = flbufptr_[0];
                const int sampsRead = in_.read(flbufptr_[4], static_cast<int>(bigfsize_));
                if (sampsRead < wanted_)
                {
                    if (n <= count - 2)
                        refuse("CDP stops: \"Program miscounted windows: anomaly 1 at EOF?\".");
                    return;
                }
            }
        }
        if (atend)
            flbufptr_[4] = flbufptr_[3];
    }

    [[nodiscard]] static double calculateNumberOfOutputWindows(double startwdur, double endwdur, int totaldur)
    {
        const double durdiff = endwdur - startwdur;
        if (flteq(durdiff, 0.0))
            return (double)totaldur / endwdur;
        double dcount = startwdur / endwdur;
        dcount = std::log(dcount);
        dcount *= (double)totaldur / durdiff;
        return dcount;
    }

    void positionsForDecreasingStretch(int* thatwindow, int startwindow, int count, int totaldur, double param0,
        double param1, double param2)
    {
        int end = count;
        const int tofill = POSMIN - ptr_;
        int start = count - tofill;
        while (start >= 0)
        {
            const int startptr = ptr_;
            for (int n = start; n < end; n++)
                store(ptr_++, calculatePosition(n, param0, param1, param2));
            retrogradeSequenceOfTimeIntervals(totaldur - start, end - start, startptr);
            for (int p = startptr; p < POSMIN; p++)
                store(p, position(p) + (double)startwindow);
            doTimestretching(thatwindow, POSMIN);
            ptr_ = 0;
            end = start;
            start -= POSMIN;
        }
        if (end)
        {
            const int startptr = ptr_;
            for (int n = 0; n < end; n++)
                store(ptr_++, calculatePosition(n, param0, param1, param2));
            retrogradeSequenceOfTimeIntervals(totaldur, end, startptr);
            for (int p = startptr; p < ptr_; p++)
                store(p, position(p) + (double)startwindow);
        }
    }

    void positionsForIncreasingStretch(
        int* thatwindow, int startwindow, int count, double param0, double param1, double param2)
    {
        int start = 0;
        int end = POSMIN - ptr_;
        while (count >= end)
        {
            for (int n = start; n < end; n++)
                store(ptr_++, calculatePosition(n, param0, param1, param2) + (double)startwindow);
            doTimestretching(thatwindow, POSMIN);
            ptr_ = 0;
            start = end;
            end += POSMIN;
        }
        if (count - start)
        {
            for (int n = start; n < count; n++)
                store(ptr_++, calculatePosition(n, param0, param1, param2) + (double)startwindow);
        }
    }

    [[nodiscard]] static double calculatePosition(int x, double param0, double param1, double param2)
    {
        double k = param1;
        k *= (double)x;
        k = std::exp(k);
        k *= param2;
        k -= param2;
        return k / param0;
    }

    void retrogradeSequenceOfTimeIntervals(int endtime, int count, int startptr)
    {
        int q = count - 1;
        double newtime = endtime;
        int p = startptr;
        for (int n = 0; n < count - 1; n++)
        {
            const double lasttime = position(p++);
            const double tsize = position(p) - lasttime;
            newtime -= tsize;
            qbuf_[static_cast<std::size_t>(q--)] = newtime;
        }
        if (q != 0)
            refuse("CDP stops: \"counting problem: retrograde_sequence_of_time_intervals()\".");
        qbuf_[0] = position(p);
        for (int n = 0; n < count; n++)
            store(startptr + n, qbuf_[static_cast<std::size_t>(n)]);
    }

    InFile in_;
    OutFile out_;
    int wanted_;
    int wlength_;
    float frametime_;
    double stretch_;
    std::vector<double> brk_;
    std::size_t brkptr_ = 0;
    double totime_ = 0.0;
    Block big_;
    Ptr bigfbuf_;
    long bigfsize_ = 0;
    Ptr flbufptr_[6];
    // TSTR_PBUF (with ptr_, the position pointer) and TSTR_QBUF.
    std::vector<double> pbuf_;
    std::vector<std::uint8_t> pbufDefined_;
    std::vector<double> qbuf_;
    int ptr_ = 0;
};

bool singleWord(std::string_view text) noexcept
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    if (begin == end)
        return false;
    for (std::size_t n = begin; n < end; ++n)
        if (std::isspace(static_cast<unsigned char>(text[n])))
            return false;
    return true;
}
}

std::vector<float> stretchTime(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const Field field{"Stretch Time", "stretch"};
    const auto& text = fieldText(fields, "stretch");
    double stretch = 1.0;
    std::vector<double> brk;
    // read_param_as_value_or_brkfile_and_check_range: a numeric word is a
    // value, anything else a breakpoint file.
    if (singleWord(text) && valueIsNumeric(text))
        stretch = readNumber(field, text, MINTSTR, MAXTSTR);
    else
    {
        brk = readBreakpointTable(field, text, MINTSTR, MAXTSTR);
        // check_for_enough_tstretch_brkpnt_vals.
        if (brk.size() / 2 < 2)
            refuse("Stretch Time's stretch table needs two or more pairs (CDP: \"Not enough data in tsretch data "
                   "file\").");
    }
    TimeStretch process(in[0], stretch, std::move(brk));
    return process.run();
}
}
