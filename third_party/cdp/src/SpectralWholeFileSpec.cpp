/*
 * Derived from the CDP System, Release 8: dev/spec/simple.c and
 * ap_simple.c, dev/focus/focus.c and ap_focus.c, dev/combine/combine.c and
 * ap_combine.c, and dev/cdp2k/mainfuncs.c, tklib1.c and tklib3.c
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
#include <cstdio>

// `spec cut`, `spec grab`, `spec magnify` (simple.c), `focus hold`
// (FREEZE2, focus.c's specfreeze2) and `combine interleave` (LEAF,
// combine.c's specleaf), each with CDP's buffers.
namespace talas::cdp::wholefile
{
namespace
{
constexpr int FINISHED = 0;
constexpr int CONTINUE = 1;
// globcon.h's BIG_TIME and speccon.h's DEFAULT_DURATION is the default.
constexpr double BIG_TIME = 32767.0;

// An empty text takes CDP's default for the field.
[[nodiscard]] bool isEmpty(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

// tklib1.c computes these limits from a float frametime: an int times it
// is a float product.
[[nodiscard]] double windowsTimes(int windows, float frametime) noexcept
{
    return (double)((float)windows * frametime);
}

// The buffers of allocate_double_buffer: input, then output, 20 windows
// each.
struct DoubleBuffer
{
    Block block;
    long buflen;
    Ptr bigfbuf;
    Ptr flbufptr[4];
    explicit DoubleBuffer(int wanted)
        : block(static_cast<std::size_t>(wanted) * BUF_MULTIPLIER * 2),
          buflen(static_cast<long>(wanted) * BUF_MULTIPLIER), bigfbuf{&block, 0}
    {
        flbufptr[2] = bigfbuf + buflen;
        flbufptr[3] = flbufptr[2] + buflen;
    }
};

// allocate_single_buffer: 20 windows and a safety float set to 0.
struct SingleBuffer
{
    Block block;
    long buflen;
    Ptr bigfbuf;
    explicit SingleBuffer(long length) : block(static_cast<std::size_t>(length) + 1), buflen(length), bigfbuf{&block, 0}
    {
        bigfbuf.set(buflen, 0.0F);
    }
};

std::string formatNumber(double value)
{
    char text[64];
    std::snprintf(text, sizeof text, "%lf", value);
    return text;
}
}

// speccut: windows startwindow to endwindow (left out), both truncated from
// their times, the end limited to the file. Range checks as tklib1.c's:
// the start from 0 to (wlength - 1) * frametime, the end from frametime to
// wlength * frametime; check_compatibility_of_cut_params refuses an end
// window not after the start window.
std::vector<float> specCut(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    const double stime = readNumber(
        Field{"Spec Cut", "start"}, fieldText(fields, "start"), 0.0, windowsTimes(file.wlength - 1, file.frametime));
    const auto& endText = fieldText(fields, "end");
    // CDP's default end is the duration.
    const double etime = isEmpty(endText) ? file.duration
                                          : readNumber(Field{"Spec Cut", "end"}, endText, (double)file.frametime,
                                                windowsTimes(file.wlength, file.frametime));
    if (truncateToInt(etime / file.frametime) <= truncateToInt(stime / file.frametime))
        refuse("Spec Cut's end must come a window or more after its start (CDP: \"Incompatible start and end times "
               "for cut\").");
    InFile input(file);
    OutFile out;
    const int wanted = file.wanted;
    DoubleBuffer buffer(wanted);
    auto& flbufptr = buffer.flbufptr;
    const Ptr bigfbuf = buffer.bigfbuf;
    const long bigfsize = buffer.buflen;
    int startwindow = truncateToInt(stime / file.frametime);
    int endwindow = truncateToInt(etime / file.frametime);
    const int windowsPerBuf = static_cast<int>(bigfsize / wanted);
    int wToSkip = 0;
    endwindow = std::min(endwindow, file.wlength);
    int bufsToSkip = startwindow / windowsPerBuf;
    if (bufsToSkip > 0)
    {
        wToSkip = bufsToSkip * windowsPerBuf;
        (void)input.seek(static_cast<long>(wToSkip) * wanted, 0);
    }
    startwindow -= wToSkip;
    endwindow -= wToSkip;
    (void)input.read(bigfbuf, static_cast<int>(bigfsize));
    flbufptr[0] = bigfbuf + static_cast<long>(startwindow) * wanted;
    const int endchunksize = windowsPerBuf - startwindow;
    flbufptr[1] = flbufptr[2];
    // copy_cut_samps.
    const auto copyCutSamps = [&]() {
        if (endwindow >= windowsPerBuf)
        {
            move(flbufptr[1], flbufptr[0], static_cast<long>(endchunksize) * wanted);
            flbufptr[1] += static_cast<long>(endchunksize) * wanted;
            flbufptr[0] = bigfbuf;
            (void)input.read(bigfbuf, static_cast<int>(bigfsize));
            endwindow -= windowsPerBuf;
        }
        else
        {
            move(flbufptr[1], flbufptr[0], static_cast<long>(endwindow - startwindow) * wanted);
            flbufptr[1] += static_cast<long>(endwindow - startwindow) * wanted;
            return FINISHED;
        }
        if (endwindow >= startwindow)
        {
            if (startwindow > 0)
                move(flbufptr[1], flbufptr[0], static_cast<long>(startwindow) * wanted);
            out.write(flbufptr[2], bigfsize);
            flbufptr[1] = flbufptr[2];
            flbufptr[0] += static_cast<long>(startwindow) * wanted;
        }
        else
        {
            move(flbufptr[1], flbufptr[0], static_cast<long>(endwindow) * wanted);
            flbufptr[1] += static_cast<long>(endwindow) * wanted;
            return FINISHED;
        }
        return CONTINUE;
    };
    if (endwindow > 0)
    {
        while (copyCutSamps() == CONTINUE)
        {
        }
    }
    const long sampsRemain = flbufptr[1] - flbufptr[2];
    if (sampsRemain > 0)
        out.write(flbufptr[2], sampsRemain);
    return std::move(out.data());
}

namespace
{
// specgrab_or_magnify up to the window found: the buffer holding it read,
// the pointer to it in flbufptr0.
void grabWindow(const AnaFile& file, InFile& input, double frztime, SingleBuffer& buffer, Ptr& flbufptr0)
{
    const int wanted = file.wanted;
    const long bigfsize = buffer.buflen;
    const int bigWsize = static_cast<int>(bigfsize / wanted);
    int frzwindow = truncateToInt(frztime / file.frametime);
    frzwindow = std::min(frzwindow, file.wlength - 1);
    const int bufsToSeek = static_cast<int>((static_cast<long>(frzwindow) * wanted) / bigfsize);
    (void)input.seek(static_cast<long>(bufsToSeek) * bigfsize, 0);
    if (input.read(buffer.bigfbuf, static_cast<int>(bigfsize)) < wanted)
        refuse("CDP stops: \"Problem reading infile data in specgrab_or_magnify()\".");
    frzwindow -= bufsToSeek * bigWsize;
    flbufptr0 = buffer.bigfbuf + static_cast<long>(frzwindow) * wanted;
}
}

// specgrab: one window, the one at the time (truncated), or the last.
// tklib1.c's range: 0 to the duration plus a second; CDP's default is half
// the duration.
std::vector<float> specGrab(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    const auto& text = fieldText(fields, "time");
    const double time = isEmpty(text) ? file.duration / 2
                                      : readNumber(Field{"Spec Grab", "time"}, text, 0.0, file.duration + 1.0);
    InFile input(file);
    OutFile out;
    SingleBuffer buffer(static_cast<long>(file.wanted) * BUF_MULTIPLIER);
    Ptr flbufptr0;
    grabWindow(file, input, time, buffer, flbufptr0);
    out.write(flbufptr0, file.wanted);
    return std::move(out.data());
}

// specmagnify: the window at the time repeated for round(dur / frametime)
// windows (generate_magnified_outfile), the first copy's amplitudes zeroed
// (as CDP zeroes window one of the first buffer it writes). Ranges: the
// time 0 to the duration (default half of it), the duration two windows to
// 32767 seconds (default 2).
std::vector<float> specMagnify(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    const auto& timeText = fieldText(fields, "time");
    const double time = isEmpty(timeText) ? file.duration / 2
                                          : readNumber(Field{"Spec Magnify", "time"}, timeText, 0.0, file.duration);
    const double dur = readNumber(
        Field{"Spec Magnify", "duration"}, fieldText(fields, "duration"), file.frametime * 2.0, BIG_TIME);
    InFile input(file);
    OutFile out;
    const int wanted = file.wanted;
    SingleBuffer buffer(static_cast<long>(wanted) * BUF_MULTIPLIER);
    Block windowbuf(static_cast<std::size_t>(wanted));
    Ptr flbufptr0;
    grabWindow(file, input, time, buffer, flbufptr0);
    const int totalWindows = cdpRound(dur / file.frametime);
    if (static_cast<std::size_t>(totalWindows) * static_cast<std::size_t>(wanted) > maximumOutputFloats)
        refuse("The output analysis file would be longer than the port renders (2^27 floats).");
    const Ptr bigfbuf = buffer.bigfbuf;
    const long bigfsize = buffer.buflen;
    const int wToBuf = static_cast<int>(bigfsize / wanted);
    const Ptr bigbufend = bigfbuf + bigfsize;
    bool writtenWholeBuffer = false;
    move(Ptr{&windowbuf, 0}, flbufptr0, wanted);
    flbufptr0 = bigfbuf;
    for (int wc = 0; wc < totalWindows; wc++)
    {
        move(flbufptr0, Ptr{&windowbuf, 0}, wanted);
        if ((flbufptr0 += wanted) >= bigbufend)
        {
            if (!writtenWholeBuffer)
            {
                for (int vc = 0; vc < wanted; vc += 2)
                    bigfbuf.set(vc, 0.0F);
            }
            out.write(bigfbuf, bigfsize);
            if (!writtenWholeBuffer)
                move(bigfbuf, Ptr{&windowbuf, 0}, wanted);
            writtenWholeBuffer = true;
            flbufptr0 = bigfbuf;
        }
    }
    if (!writtenWholeBuffer)
    {
        for (int vc = 0; vc < wanted; vc += 2)
            bigfbuf.set(vc, 0.0F);
        if (flbufptr0 - bigfbuf > 0)
            out.write(bigfbuf, flbufptr0 - bigfbuf);
    }
    else
    {
        const int windowsLeft = totalWindows % wToBuf;
        if (windowsLeft > 0)
            out.write(bigfbuf, static_cast<long>(windowsLeft) * wanted);
    }
    return std::move(out.data());
}

// specfreeze2, with read_specfreeze2_data: each (time, duration) pair holds
// the window at round(time / frametime) for round(duration / frametime)
// windows more. The pairs are not sorted (CDP does not check their order):
// a time already passed holds the window reached. CDP refuses a location
// outside the file, a negative duration and an unpaired value, and takes a
// data file of no pairs (the output is the input).
std::vector<float> focusHold(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    std::vector<int> frztime;
    std::vector<int> segtime;
    bool isLocation = true;
    for (const auto& piece : fgetsPieces(fieldText(fields, "holds")))
    {
        std::size_t p = 0;
        while (p < piece.size() && std::isspace(static_cast<unsigned char>(piece[p])))
            p++;
        if (p >= piece.size())
            continue;
        while (auto value = getFloatFromWithinString(piece, p))
        {
            double dtime = *value;
            dtime /= file.frametime;
            if (isLocation)
            {
                const int location = cdpRound(dtime);
                if (location < 0 || location >= file.wlength)
                    refuse("Focus Hold's hold time " + formatNumber(*value)
                        + " is not within the sound (CDP: \"Invalid freeze location\").");
                frztime.push_back(location);
            }
            else
            {
                const int duration = cdpRound(dtime);
                if (duration < 0)
                    refuse("Focus Hold's hold duration " + formatNumber(*value)
                        + " is negative (CDP: \"Invalid freeze duration\").");
                segtime.push_back(duration);
            }
            isLocation = !isLocation;
        }
    }
    if (!isLocation)
        refuse("Focus Hold's holds are not paired (CDP: \"Freeze times not paired correctly\").");
    InFile input(file);
    OutFile out;
    const int wanted = file.wanted;
    DoubleBuffer buffer(wanted);
    auto& flbufptr = buffer.flbufptr;
    const long buflen = buffer.buflen;
    const int bufwinlen = static_cast<int>(buflen / wanted);
    int windowsInBuf = input.read(buffer.bigfbuf, static_cast<int>(buflen)) / wanted;
    int inwindowcnt = 0;
    int outbufwincnt = 0;
    int inbufwincnt = 0;
    bool finished = false;
    std::size_t total = 0;
    const auto countOut = [&]() {
        if (++total * static_cast<std::size_t>(wanted) > maximumOutputFloats)
            refuse("The output analysis file would be longer than the port renders (2^27 floats).");
    };
    flbufptr[0] = buffer.bigfbuf;
    flbufptr[1] = flbufptr[2];
    const auto nextOutWindow = [&]() {
        countOut();
        if (++outbufwincnt >= bufwinlen)
        {
            out.write(flbufptr[2], buflen);
            flbufptr[1] = flbufptr[2];
            outbufwincnt = 0;
        }
        else
            flbufptr[1] += wanted;
    };
    for (std::size_t n = 0; n < frztime.size(); ++n)
    {
        while (inwindowcnt < frztime[n])
        {
            move(flbufptr[1], flbufptr[0], wanted);
            if (++inwindowcnt >= file.wlength)
            {
                finished = true;
                break;
            }
            else if (++inbufwincnt >= windowsInBuf)
            {
                windowsInBuf = input.read(buffer.bigfbuf, static_cast<int>(buflen)) / wanted;
                flbufptr[0] = buffer.bigfbuf;
                inbufwincnt = 0;
            }
            else
                flbufptr[0] += wanted;
            nextOutWindow();
        }
        if (finished)
            break;
        for (int m = 0; m < segtime[n]; m++)
        {
            move(flbufptr[1], flbufptr[0], wanted);
            nextOutWindow();
        }
    }
    while (inwindowcnt < file.wlength)
    {
        move(flbufptr[1], flbufptr[0], wanted);
        nextOutWindow();
        inwindowcnt++;
        if (++inbufwincnt >= windowsInBuf)
        {
            windowsInBuf = input.read(buffer.bigfbuf, static_cast<int>(buflen)) / wanted;
            flbufptr[0] = buffer.bigfbuf;
            inbufwincnt = 0;
        }
        else
            flbufptr[0] += wanted;
    }
    if (!(flbufptr[1] == flbufptr[2]))
        out.write(flbufptr[2], flbufptr[1] - flbufptr[2]);
    return std::move(out.data());
}

// specleaf: the files in turn give a leaf of leafsize windows each, read
// from where each left off. CDP adds the leaf size to its window count once
// per leaf written (of any file) and stops once that reaches the shortest
// file's window count: so each file gives only about its share of the
// windows, and the output is that count rounded up to whole leaves. Each
// leaf is written whole even when the read is short (the rest of the buffer
// as the last read left it). The leaf size ranges 1 to the first file's
// window count (tklib1.c reads it before the other files are opened).
std::vector<float> combineInterleave(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const int leafsize = readInteger(Field{"Combine Interleave", "leafSize"}, fieldText(fields, "leafSize"), 1.0,
        static_cast<double>(in[0].wlength));
    const int wanted = in[0].wanted;
    std::vector<InFile> files;
    for (const auto& file : in)
        files.emplace_back(file);
    OutFile out;
    // allocate_specleaf_buffer.
    SingleBuffer buffer(static_cast<long>(wanted) * leafsize);
    int minsams = in[0].insams();
    for (std::size_t n = 1; n < in.size(); n++)
        minsams = std::min(minsams, in[n].insams());
    const int wlength = minsams / wanted;
    int totalWindows = 0;
    while (totalWindows < wlength)
    {
        for (std::size_t n = 0; n < files.size(); n++)
        {
            if (files[n].read(buffer.bigfbuf, static_cast<int>(buffer.buflen)) <= 0)
                refuse("CDP stops: \"Accounting problem: specleaf()\".");
            out.write(buffer.bigfbuf, buffer.buflen);
            if ((totalWindows += leafsize) >= wlength)
                break;
        }
    }
    return std::move(out.data());
}
}
