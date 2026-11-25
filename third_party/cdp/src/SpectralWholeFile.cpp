/*
 * Derived from the CDP System, Release 8: dev/pv/pvoc.c, dev/stretch/
 * stretch.c and ap_stretch.c, dev/spec/simple.c and ap_simple.c, dev/focus/
 * focus.c and ap_focus.c, dev/combine/combine.c and ap_combine.c, dev/blur/
 * blur.c and ap_blur.c, and dev/cdp2k/mainfuncs.c, parstruct.c, readdata.c,
 * special.c, tklib1.c, tklib3.c and writedata.c, dev/cdparse/cdparse.c and
 * dev/newsfsys/snd.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralWholeFile.h"

#include "SpectralWholeFileEmulation.h"
#include "talas/cdp/PhaseVocoder.h"
#include "talas/cdp/PitchTrack.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace talas::cdp
{
namespace wholefile
{
void refuse(std::string message)
{
    throw Refusal{std::move(message)};
}

namespace
{
[[noreturn]] void outside()
{
    refuse("CDP would read or write outside its buffer here, so its result is undefined.");
}
}

float Ptr::get(long offset) const
{
    const auto at = index + offset;
    if (block == nullptr || at < 0 || at >= block->size())
        outside();
    return block->values[static_cast<std::size_t>(at)];
}

bool Ptr::isDefined(long offset) const
{
    const auto at = index + offset;
    if (block == nullptr || at < 0 || at >= block->size())
        outside();
    return block->defined[static_cast<std::size_t>(at)] != 0;
}

void Ptr::set(long offset, float value, bool isDefinedValue) const
{
    const auto at = index + offset;
    if (block == nullptr || at < 0 || at >= block->size())
        outside();
    block->values[static_cast<std::size_t>(at)] = value;
    block->defined[static_cast<std::size_t>(at)] = isDefinedValue ? 1 : 0;
}

void move(Ptr dst, Ptr src, long count)
{
    if (count < 0)
        refuse("CDP would move a negative number of floats here (memmove of a huge size), so it fails.");
    if (count == 0)
        return;
    if (dst.block == nullptr || src.block == nullptr || dst.index < 0 || src.index < 0
        || dst.index + count > dst.block->size() || src.index + count > src.block->size())
    {
        refuse("CDP would read or write outside its buffer here, so its result is undefined.");
    }
    // memmove: as if through a temporary.
    std::vector<float> values(src.block->values.begin() + src.index, src.block->values.begin() + src.index + count);
    std::vector<std::uint8_t> defined(
        src.block->defined.begin() + src.index, src.block->defined.begin() + src.index + count);
    std::copy(values.begin(), values.end(), dst.block->values.begin() + dst.index);
    std::copy(defined.begin(), defined.end(), dst.block->defined.begin() + dst.index);
}

int InFile::read(Ptr dst, int count)
{
    const auto available = static_cast<long>(file_->data.size()) - position_;
    const auto got = static_cast<long>(std::max(0L, std::min<long>(count, available)));
    if (got > 0)
    {
        if (dst.block == nullptr || dst.index < 0 || dst.index + got > dst.block->size())
            refuse("CDP would read or write outside its buffer here, so its result is undefined.");
        for (long n = 0; n < got; ++n)
            dst.set(n, file_->data[static_cast<std::size_t>(position_ + n)]);
    }
    position_ += got;
    return static_cast<int>(got);
}

int InFile::seek(long distance, int whence)
{
    long newpos = whence == 0 ? distance : position_ + distance;
    if (newpos < 0)
        newpos = 0;
    if (newpos > static_cast<long>(file_->data.size()))
        refuse("CDP would seek past the end of its analysis file here, which fails.");
    position_ = newpos;
    return static_cast<int>(newpos);
}

void OutFile::write(Ptr src, long count)
{
    if (count <= 0)
        refuse("CDP would write no samples here, and stops: \"Can't write to output soundfile\".");
    if (data_.size() + static_cast<std::size_t>(count) > maximumOutputFloats)
        refuse("The output analysis file would be longer than the port renders (2^27 floats).");
    for (long n = 0; n < count; ++n)
    {
        if (!src.isDefined(n))
            refuse("CDP would write memory it never initialised here, so its result is undefined.");
        data_.push_back(src.get(n));
    }
}

bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

int cdpRound(double value)
{
    if (!std::isfinite(value) || std::fabs(value) >= 2147483647.0)
        refuse("CDP would round a value outside the integers here, so its result is undefined.");
    return static_cast<int>(std::lround(value));
}

int truncateToInt(double value)
{
    if (!std::isfinite(value) || std::fabs(value) >= 2147483647.0)
        refuse("CDP would convert a value outside the integers here, so its result is undefined.");
    return static_cast<int>(value);
}

std::string Field::name() const
{
    std::string label{process};
    label += "'s ";
    label += id;
    return label;
}

bool valueIsNumeric(std::string_view text) noexcept
{
    // value_is_numeric, over a NUL-terminated copy.
    const std::string copy{text};
    const char* str = copy.c_str();
    const char* p = str;
    int point = 0;
    while (std::isspace(static_cast<unsigned char>(*p)))
        p++;
    const char* q = p;
    if (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '.' && *p != '-')
        return false;
    if (*p == '.' || *p == '-')
    {
        if (*p == '-')
            p++;
        else
        {
            point++;
            p++;
        }
    }
    for (;;)
    {
        if (*p == '.')
        {
            if (point)
                return false;
            point++;
            p++;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(*p)))
        {
            p++;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(*p)) && *p != '\0')
            return false;
        const char* end = p;
        int valid = 0;
        for (p = q; p != end; p++)
            if (std::isdigit(static_cast<unsigned char>(*p)))
                valid++;
        return valid != 0;
    }
}

namespace
{
std::string formatNumber(double value)
{
    char text[64];
    std::snprintf(text, sizeof text, "%.10g", value);
    return text;
}

std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}
}

double readNumber(const Field& field, std::string_view text, double lo, double hi)
{
    // The command line gives CDP one word: anything after a space is not
    // part of it, so a text holding more than one word is refused.
    const auto word = trimmed(text);
    if (word.empty() || !valueIsNumeric(word)
        || std::ranges::any_of(word, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }))
    {
        refuse(field.name() + " must be a number as CDP reads one (digits, one point, a leading minus).");
    }
    // sscanf's %lf over the word value_is_numeric passed: strtod.
    const std::string copy{word};
    char* end = nullptr;
    double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str())
        refuse(field.name() + " must be a number as CDP reads one (digits, one point, a leading minus).");
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    if (value > hi || value < lo)
    {
        refuse(field.name() + " (" + formatNumber(value) + ") is outside CDP's range, " + formatNumber(lo)
            + " to " + formatNumber(hi) + ".");
    }
    return value;
}

int readInteger(const Field& field, std::string_view text, double lo, double hi)
{
    return cdpRound(readNumber(field, text, lo, hi));
}

std::optional<double> getFloatFromWithinString(std::string_view text, std::size_t& position)
{
    // get_float_from_within_string, over a NUL-terminated copy.
    const std::string copy{text};
    const char* base = copy.c_str();
    const char* p = base + std::min(position, copy.size());
    int decimal_point_cnt = 0;
    bool has_digits = false;
    while (std::isspace(static_cast<unsigned char>(*p)))
        p++;
    const char* valstart = p;
    switch (*p)
    {
    case '-': break;
    case '.': decimal_point_cnt = 1; break;
    default:
        if (!std::isdigit(static_cast<unsigned char>(*p)))
            return std::nullopt;
        has_digits = true;
        break;
    }
    p++;
    while (!std::isspace(static_cast<unsigned char>(*p)) && *p != '\n' && *p != '\0')
    {
        if (std::isdigit(static_cast<unsigned char>(*p)))
            has_digits = true;
        else if (*p == '.')
        {
            if (++decimal_point_cnt > 1)
                return std::nullopt;
        }
        else
            return std::nullopt;
        p++;
    }
    // sscanf's %lf over the token checked: strtod.
    if (!has_digits)
        return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(valstart, &end);
    if (end == valstart)
        return std::nullopt;
    position = static_cast<std::size_t>(p - base);
    return value;
}

std::vector<std::string> fgetsPieces(std::string_view text)
{
    // A data file holds the text and a final newline (as it does when CDP
    // is given it); fgets(temp, 200, fp) reads up to 199 characters, or up
    // to and including a newline.
    std::string file{text};
    if (file.empty() || file.back() != '\n')
        file += '\n';
    std::vector<std::string> pieces;
    std::size_t at = 0;
    while (at < file.size())
    {
        std::size_t length = 0;
        while (at + length < file.size() && length < 199)
        {
            ++length;
            if (file[at + length - 1] == '\n')
                break;
        }
        pieces.push_back(file.substr(at, length));
        at += length;
    }
    return pieces;
}

std::vector<double> readBreakpointTable(const Field& field, std::string_view text, double lo, double hi)
{
    // get_brkpnt_data_from_file_and_test_it.
    std::vector<double> brk;
    double lasttime = 0.0;
    bool istime = true;
    for (const auto& piece : fgetsPieces(text))
    {
        std::size_t q = 0;
        while (auto value = getFloatFromWithinString(piece, q))
        {
            auto p = *value;
            if (istime)
            {
                if (!brk.empty() && p <= lasttime)
                {
                    refuse("The times (" + formatNumber(lasttime) + " and " + formatNumber(p) + ") in "
                        + field.name() + "'s breakpoint table are not in increasing order.");
                }
                lasttime = p;
            }
            else
            {
                if (flteq(p, lo))
                    p = lo;
                else if (flteq(p, hi))
                    p = hi;
                if (p < lo || p > hi)
                {
                    refuse("A value (" + formatNumber(p) + ") in " + field.name()
                        + "'s breakpoint table is outside CDP's range, " + formatNumber(lo) + " to "
                        + formatNumber(hi) + ".");
                }
            }
            istime = !istime;
            brk.push_back(p);
        }
    }
    if (brk.empty())
        refuse(field.name() + "'s breakpoint table holds no data.");
    if (brk.size() % 2 != 0)
        refuse(field.name() + "'s breakpoint table is not paired correctly.");
    return brk;
}

const std::string& fieldText(const FieldTexts& fields, std::string_view id)
{
    static const std::string empty;
    const auto found = fields.find(id);
    return found == fields.end() ? empty : found->second;
}
}

namespace
{
using Kind = OfflineSpectralField::Kind;
using Program = std::vector<float> (*)(std::span<const wholefile::AnaFile>, const wholefile::FieldTexts&);

struct Entry
{
    std::string_view id;
    Program program;
};

std::vector<OfflineSpectralProcess> makeProcesses()
{
    std::vector<OfflineSpectralProcess> processes;
    processes.push_back({"stretchTime", "stretch time", "Stretch Time", 1, 1,
        {{"stretch", "Stretch", Kind::Breakpoints, "1", {},
            "The time-stretch factor, 0.0001 to 10000, or a table of times and factors (the factor at time 0 is "
            "the first; the output ends at the table's last time when that comes before the sound's end)."}}});
    processes.push_back({"specCut", "spec cut", "Spec Cut", 1, 1,
        {{"start", "Start", Kind::Number, "0", {},
             "The time in seconds where the cut starts, 0 to the last window's time (truncated to a window)."},
            {"end", "End", Kind::Number, "", {},
                "The time in seconds where the cut ends, one window to the sound's duration (empty: the "
                "duration), after the start (truncated to a window, which is left out)."}}});
    processes.push_back({"specGrab", "spec grab", "Spec Grab", 1, 1,
        {{"time", "Time", Kind::Number, "", {},
            "The time in seconds of the one window kept, 0 to the duration plus one second (empty: half the "
            "duration; a time past the end takes the last window)."}}});
    processes.push_back({"specMagnify", "spec magnify", "Spec Magnify", 1, 1,
        {{"time", "Time", Kind::Number, "", {},
             "The time in seconds of the window magnified, 0 to the duration (empty: half the duration)."},
            {"duration", "Duration", Kind::Number, "2", {},
                "The output's duration in seconds, two windows to 32767 (the window repeated, its first copy "
                "silent)."}}});
    processes.push_back({"focusHold", "focus hold", "Focus Hold", 1, 1,
        {{"holds", "Holds", Kind::Text, "0.05 0.5", {},
            "CDP's data file: pairs of a time in seconds (within the sound) whose window is held and the hold's "
            "duration in seconds (0 or more), added to the sound's length."}}});
    processes.push_back({"combineInterleave", "combine interleave", "Combine Interleave", 2, 64,
        {{"leafSize", "Leaf Size", Kind::Number, "1", {},
            "Windows taken from each input in turn, 1 to the first input's window count (CDP stops once the "
            "leaves written reach the shortest input's window count)."}}});
    processes.push_back({"blurDrunk", "blur drunk", "Blur Drunk", 1, 1,
        {{"range", "Range", Kind::Number, "32", {},
             "The drunken walk's largest step in windows, 1 to half the window count (rounded)."},
            {"start", "Start", Kind::Number, "0", {},
                "The time in seconds where the walk starts, 0 to the sound's duration."},
            {"duration", "Duration", Kind::Number, "2", {},
                "The output's duration in seconds, one window to 32767."},
            {"noZeroSteps", "No Zero Steps", Kind::Flag, "false", {},
                "CDP's -z: no step of 0 windows (no window repeated)."},
            {"seed", "Seed", Kind::Number, "0", {},
                "CDP seeds its walk with srand(time(0)): the Unix time CDP started at, 0 to 4294967295, gives "
                "that run's walk."}}});
    processes.push_back({"blurShuffle", "blur shuffle", "Blur Shuffle", 1, 1,
        {{"domainImage", "Domain-Image", Kind::Text, "abcd-dcba", {},
             "Two strings of letters separated by '-' (or ':'): the domain names consecutive groups of "
             "windows, the image their new order (letters may repeat or be left out)."},
            {"groupSize", "Group Size", Kind::Number, "1", {},
                "Windows in each group a letter names, 1 to 32767 (rounded)."}}});
    processes.push_back({"blurWeave", "blur weave", "Blur Weave", 1, 1,
        {{"weave", "Weave", Kind::Text, "3 -1", {},
            "CDP's weave file: whole-number steps in windows, from 2 less than minus the window count to one "
            "less than it, repeated through the sound; never before the start and ending ahead of it."}}});
    return processes;
}

const std::vector<Entry>& programs()
{
    static const std::vector<Entry> entries{
        {"stretchTime", &wholefile::stretchTime},
        {"specCut", &wholefile::specCut},
        {"specGrab", &wholefile::specGrab},
        {"specMagnify", &wholefile::specMagnify},
        {"focusHold", &wholefile::focusHold},
        {"combineInterleave", &wholefile::combineInterleave},
        {"blurDrunk", &wholefile::blurDrunk},
        {"blurShuffle", &wholefile::blurShuffle},
        {"blurWeave", &wholefile::blurWeave},
    };
    return entries;
}

// pvoc.c's window length M for these points (made even) and overlap.
[[nodiscard]] int windowLengthFor(int points, int overlap) noexcept
{
    const int chans = points + (points % 2);
    switch (overlap)
    {
    case 1: return 4 * chans;
    case 2: return 2 * chans;
    case 3: return chans;
    default: return chans / 2;
    }
}
}

const std::vector<OfflineSpectralProcess>& offlineSpectralProcesses()
{
    static const std::vector<OfflineSpectralProcess> processes = makeProcesses();
    return processes;
}

const OfflineSpectralProcess* findOfflineSpectralProcess(std::string_view id)
{
    const auto& processes = offlineSpectralProcesses();
    const auto found = std::ranges::find(processes, id, &OfflineSpectralProcess::id);
    return found == processes.end() ? nullptr : &*found;
}

std::vector<double> synthesiseAnalysisFile(std::span<const float> windows, int points, int overlap, double sampleRate)
{
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!d.has_value() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return {};
    const auto wanted = static_cast<std::size_t>(points + (points % 2) + 2);
    if (windows.empty() || windows.size() % wanted != 0)
        return {};
    const auto windowCount = windows.size() / wanted;
    // pvoc anal writes arate = R / D in float; pvoc synth takes R = D * arate.
    const auto arate = (float)((float)sampleRate / (float)*d);
    const auto rate = (float)((float)*d * arate);
    PhaseVocoder vocoder;
    if (!vocoder.prepare(static_cast<double>(rate), points, overlap))
        return {};
    const auto latency = vocoder.latencyFrames();
    const auto D = *d;
    // pvoc_process's times: nO from nI's start, and IOi, the samples each
    // window's synthesis shifts out (the block made ready by the window
    // before), as pvoc_process steps them.
    const int M = windowLengthFor(points, overlap);
    const int synWinLen = M / 2;
    const int analWinLen = M / 2;
    const int nO0 = -(analWinLen / D) * D;
    const int obuflen = 4 * M;
    // The stream gives the shifted-out samples, in order, latency frames
    // late; frames past the file's end are the zero frames pvoc synth reads
    // there (its memset buffer).
    std::size_t frame = 0;
    std::vector<double> shifted;
    std::size_t fed = 0;
    std::vector<double> input(static_cast<std::size_t>(D), 0.0);
    std::vector<double> output(input.size(), 0.0);
    const auto edit = [&](std::span<float> analysis) {
        if (frame < windowCount)
        {
            const auto* from = windows.data() + frame * wanted;
            std::copy(from, from + wanted, analysis.begin());
        }
        else
            std::fill(analysis.begin(), analysis.end(), 0.0F);
        ++frame;
    };
    const auto ensure = [&](std::size_t count) {
        while (shifted.size() < count)
        {
            if (!vocoder.process(std::span<const double>{input}, std::span<double>{output}, edit))
                return false;
            for (std::size_t n = 0; n < output.size(); ++n)
                if (fed + n >= latency)
                    shifted.push_back(output[n]);
            fed += output.size();
        }
        return true;
    };
    std::vector<double> written;
    // NEW 2014 in pvoc_process: after the last window (a short read), it
    // stops at the next read once the last block outfloats saw was all
    // zeros, and outfloats writes no all-zero block once input has ended.
    long long outCount = 0;
    float localMax = 0.0F;
    const std::size_t limit = windowCount + static_cast<std::size_t>(obuflen / D) + 16;
    for (std::size_t t = 0; t < limit; ++t)
    {
        const bool finished = t >= windowCount;
        if (finished && localMax == 0.0F)
            return written;
        const long long nO = static_cast<long long>(nO0) + static_cast<long long>(t) * D;
        int IOi = 0;
        if (t > 0)
        {
            if (nO > (synWinLen + D))
                IOi = D;
            else if (nO > synWinLen)
                IOi = static_cast<int>(nO - synWinLen);
        }
        for (int i = 0; i < IOi;)
        {
            const int todo = std::min<int>(IOi - i, obuflen - static_cast<int>(outCount % obuflen));
            if (!ensure(static_cast<std::size_t>(outCount + todo)))
                return {};
            // outfloats' local_maxsample: the largest value, or the
            // negation of the least, of this block (clipped as written).
            float local_maxsample = 0.0F;
            float local_minsample = 0.0F;
            for (int n = 0; n < todo; ++n)
            {
                const auto val = static_cast<float>(shifted[static_cast<std::size_t>(outCount + n)]);
                if (val > local_maxsample)
                    local_maxsample = val;
                if (val < local_minsample)
                    local_minsample = val;
            }
            local_maxsample = std::max(-local_minsample, local_maxsample);
            if (!(finished && local_maxsample == 0.0F))
            {
                written.insert(written.end(), shifted.begin() + outCount, shifted.begin() + outCount + todo);
            }
            localMax = local_maxsample;
            i += todo;
            outCount += todo;
        }
    }
    return {};
}

OfflineSpectralAnalysis renderOfflineSpectralAnalysis(
    const OfflineSpectralRequest& request, std::span<const std::vector<double>> inputs, double sampleRate)
{
    OfflineSpectralAnalysis result;
    const auto fail = [&result](std::string message) {
        result.windows.clear();
        result.error = std::move(message);
        return result;
    };
    const auto* process = findOfflineSpectralProcess(request.process);
    const auto entry = std::ranges::find(programs(), request.process, &Entry::id);
    if (process == nullptr || entry == programs().end())
        return fail("There is no offline spectral process \"" + request.process + "\".");
    wholefile::FieldTexts texts;
    for (const auto& field : process->fields)
        texts.emplace(std::string{field.id}, std::string{field.defaultValue});
    for (const auto& [id, text] : request.fields)
    {
        const auto known = std::ranges::find(process->fields, std::string_view{id}, &OfflineSpectralField::id);
        if (known == process->fields.end())
            return fail(std::string{process->label} + " has no field \"" + id + "\".");
        if (known->kind == Kind::Flag && text != "true" && text != "false")
            return fail(std::string{process->label} + "'s " + id + " must be true or false.");
        if (known->kind == Kind::Choice
            && std::ranges::find(known->choices, std::string_view{text}) == known->choices.end())
        {
            return fail(std::string{process->label} + "'s " + id + " is not one of its choices.");
        }
        texts[id] = text;
    }
    if (inputs.size() < process->minimumInputs || inputs.size() > process->maximumInputs)
    {
        return fail(std::string{process->label} + " takes " + std::to_string(process->minimumInputs)
            + (process->minimumInputs == process->maximumInputs
                    ? std::string{}
                    : " to " + std::to_string(process->maximumInputs))
            + " input" + (process->maximumInputs == 1 ? "" : "s") + ".");
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return fail("The sample rate must be a positive number.");
    const auto d = PhaseVocoder::decimationFor(request.points, request.overlap);
    if (!d.has_value())
    {
        return fail("pvoc anal takes points from 2 to 8192 whose half its FFT can factor, and an overlap from 1 "
                    "to 4.");
    }
    // pvoc anal of each input, and the header cdparse reads from its .ana.
    std::vector<wholefile::AnaFile> files;
    for (const auto& samples : inputs)
    {
        if (samples.empty())
            return fail("An input holds no samples.");
        if (!std::ranges::all_of(samples, [](double sample) { return std::isfinite(sample); }))
            return fail("An input holds a sample that is not finite.");
        wholefile::AnaFile file;
        const bool analysed = analyseWholeSound(samples, sampleRate, request.points, request.overlap,
            [&file](std::span<const float> frame) { file.data.insert(file.data.end(), frame.begin(), frame.end()); });
        if (!analysed)
            return fail("pvoc anal refuses an input.");
        file.wanted = request.points + (request.points % 2) + 2;
        file.wlength = static_cast<int>(file.data.size() / static_cast<std::size_t>(file.wanted));
        // pvoc.c's arate, (float)(R / D); cdparse.c's frametime and
        // duration (wlength * frametime, in float).
        file.arate = (float)((float)sampleRate / (float)*d);
        file.frametime = (float)(1.0 / file.arate);
        file.duration = (float)((float)file.wlength * file.frametime);
        file.nyquist = static_cast<double>(static_cast<int>(sampleRate)) / 2.0;
        files.push_back(std::move(file));
    }
    try
    {
        result.windows = entry->program(files, texts);
    }
    catch (const wholefile::Refusal& refusal)
    {
        return fail(refusal.message);
    }
    if (result.windows.empty())
        return fail("CDP's output analysis file holds no windows, and pvoc synth refuses a file of zero duration.");
    result.frameSize = static_cast<std::size_t>(files.front().wanted);
    return result;
}

OfflineSpectralResult renderOfflineSpectralProcess(
    const OfflineSpectralRequest& request, std::span<const std::vector<double>> inputs, double sampleRate)
{
    OfflineSpectralResult result;
    auto analysis = renderOfflineSpectralAnalysis(request, inputs, sampleRate);
    if (!analysis.ok())
    {
        result.error = std::move(analysis.error);
        return result;
    }
    result.samples = synthesiseAnalysisFile(analysis.windows, request.points, request.overlap, sampleRate);
    return result;
}
}
