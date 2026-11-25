/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/cdp2k/special.c, tklib1.c and tklib3.c, and dev/newsfsys/osbind.c
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

#include "talas/cdp/Drand48.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// `blur drunk`, `blur shuffle` and `blur weave` (blur.c's specdrunk,
// specshuffle and specweave), each with CDP's buffers.
namespace talas::cdp::wholefile
{
namespace
{
constexpr int FINISHED = 0;
constexpr int CONTINUE = 1;
constexpr double BIG_TIME = 32767.0;

// allocate_double_buffer: input, then output, 20 windows each.
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

[[nodiscard]] bool readFlag(std::string_view text)
{
    return text == "true";
}

// specdrunk. CDP's drand48 is rand() / RAND_MAX over srand(time(0))
// (initrand48 in drnk_preprocess): the seed stands for that time. Its
// buffer moves keep CDP's bugs: stepping back into an earlier buffer that
// is the file's first (sndseekEx's new position 0) is taken as stepping
// off the file's start, and the step is reflected from the pointer already
// moved into that earlier buffer, so the walk jumps a buffer ahead;
// stepping off either end reflects the step.
class Drunk
{
public:
    Drunk(const AnaFile& file, int range, double stime, double dur, bool noZeroSteps, unsigned long seed)
        : file_(file), in_(file), buffer_(file.wanted), range_(range), stime_(stime), dur_(dur),
          noZeroSteps_(noZeroSteps)
    {
        random_.seed(seed);
        twicerange_ = (range_ * 2) + 1;
    }

    std::vector<float> run()
    {
        auto& flbufptr = buffer_.flbufptr;
        const int wanted = file_.wanted;
        float time = 0.0F;
        advanceToStarttime();
        flbufptr[1] = flbufptr[2];
        for (;;)
        {
            copyWindowToOutbuf();
            if ((time = (float)(time + file_.frametime)) >= dur_)
                break;
            int step = getRandstep();
            flbufptr[0] += static_cast<long>(step) * wanted;
            if (flbufptr[0] < buffer_.bigfbuf || flbufptr[0] >= inputbufEnd_)
                adjustBuffers(&step);
        }
        if (!(flbufptr[1] == flbufptr[2]))
            out_.write(flbufptr[2], flbufptr[1] - flbufptr[2]);
        return std::move(out_.data());
    }

private:
    int readSamps(Ptr into)
    {
        ssampsread_ = in_.read(into, static_cast<int>(buffer_.buflen));
        return ssampsread_;
    }

    void advanceToStarttime()
    {
        double timeRead = 0.0;
        double previousTimeRead = 0.0;
        while (timeRead <= stime_)
        {
            readSamps(buffer_.bigfbuf);
            if (ssampsread_ == 0)
                refuse("CDP stops: \"No data found in input analysis file\" (Blur Drunk's start is at the sound's "
                       "end).");
            sampsRead_ = ssampsread_;
            drnkBufno_++;
            const int wToBuf = ssampsread_ / file_.wanted;
            previousTimeRead = timeRead;
            timeRead += ((float)wToBuf * file_.frametime);
        }
        inputbufEnd_ = buffer_.bigfbuf + ssampsread_;
        const double timeInBuf = stime_ - previousTimeRead;
        const int startwindowInBuf = cdpRound(timeInBuf / file_.frametime);
        buffer_.flbufptr[0] = buffer_.bigfbuf + static_cast<long>(startwindowInBuf) * file_.wanted;
    }

    void copyWindowToOutbuf()
    {
        auto& flbufptr = buffer_.flbufptr;
        move(flbufptr[1], flbufptr[0], file_.wanted);
        if ((flbufptr[1] += file_.wanted) >= flbufptr[3])
        {
            out_.write(flbufptr[2], buffer_.buflen);
            flbufptr[1] = flbufptr[2];
        }
    }

    int getRandstep()
    {
        int step = 0;
        do
        {
            step = static_cast<int>(random_.next() * twicerange_);
            step -= range_;
        } while (noZeroSteps_ && step == 0);
        return step;
    }

    [[noreturn]] static void tooLarge()
    {
        refuse("Blur Drunk's walk steps off both ends of the sound (CDP: \"Step turned out ot be too large\").");
    }

    void adjustBuffers(int* step)
    {
        auto& flbufptr = buffer_.flbufptr;
        const Ptr bigfbuf = buffer_.bigfbuf;
        const long buflen = buffer_.buflen;
        const int wanted = file_.wanted;
        int bigbufsBaktrak = 0;
        bool reflectOffStart = false;
        bool reflectOffEnd = false;
        do
        {
            if (flbufptr[0] < bigfbuf)
            {
                if (drnkBufno_ == 0)
                {
                    // invert_step_back_to_step_fwd_at_start_of_file.
                    if (reflectOffEnd)
                        tooLarge();
                    *step = -(*step);
                    flbufptr[0] += static_cast<long>(*step) * 2 * wanted;
                    reflectOffStart = true;
                }
                else
                {
                    // place_sbufptr_in_bigbuf_range.
                    while (flbufptr[0] < bigfbuf)
                    {
                        flbufptr[0] += buflen;
                        bigbufsBaktrak++;
                    }
                    const long baksamps = (static_cast<long>(bigbufsBaktrak) * buflen) + sampsRead_;
                    if (in_.seek(-baksamps, 1) <= 0)
                    {
                        // invert_step_back_to_step_fwd.
                        if (reflectOffEnd)
                            tooLarge();
                        const Ptr origSbuf = flbufptr[0];
                        (void)in_.seek(static_cast<long>(drnkBufno_) * buflen, 0);
                        readSamps(bigfbuf);
                        if (ssampsread_ == 0)
                            refuse("CDP stops: \"Problem reading buffer:in invert_step_back_to_step_fwd()\".");
                        sampsRead_ = ssampsread_;
                        inputbufEnd_ = bigfbuf + ssampsread_;
                        *step = -(*step);
                        flbufptr[0] = origSbuf + static_cast<long>(*step) * 2 * wanted;
                        reflectOffStart = true;
                    }
                    else
                    {
                        // baktrak_to_correct_bigbuf.
                        drnkBufno_ -= bigbufsBaktrak;
                        readSamps(bigfbuf);
                        if (ssampsread_ == 0)
                            refuse("CDP stops: \"Problem reading buffer in baktrak_to_correct_bigbuf()\".");
                        sampsRead_ = ssampsread_;
                        inputbufEnd_ = bigfbuf + ssampsread_;
                        reflectOffStart = false;
                    }
                }
            }
            else
                reflectOffStart = false;
            const long totalSampsRead
                = std::min(static_cast<long>(file_.insams()), static_cast<long>(drnkBufno_ + 1) * buflen);
            if (flbufptr[0] >= inputbufEnd_)
            {
                if (totalSampsRead >= file_.insams())
                {
                    // invert_step_fwd_to_step_back_at_end_of_file.
                    if (reflectOffStart)
                        tooLarge();
                    *step = -(*step);
                    flbufptr[0] += static_cast<long>(*step) * 2 * wanted;
                    if (flbufptr[0] < bigfbuf && drnkBufno_ == 0)
                        tooLarge();
                    reflectOffEnd = true;
                }
                else
                {
                    // advance_to_correct_bigbuf.
                    while (flbufptr[0] >= inputbufEnd_)
                    {
                        readSamps(bigfbuf);
                        sampsRead_ = ssampsread_;
                        if (ssampsread_ == 0)
                            break;
                        inputbufEnd_ = bigfbuf + ssampsread_;
                        drnkBufno_++;
                        flbufptr[0] -= buflen;
                    }
                    if (flbufptr[0] >= inputbufEnd_)
                    {
                        // invert_step_fwd_to_step_back.
                        if (reflectOffStart)
                            tooLarge();
                        const Ptr origSbuf = flbufptr[0];
                        (void)in_.seek(static_cast<long>(drnkBufno_) * buflen, 0);
                        readSamps(bigfbuf);
                        if (ssampsread_ == 0)
                            refuse("CDP stops: \"Problem reading buffer in invert_step_fwd_to_step_back()\".");
                        sampsRead_ = ssampsread_;
                        inputbufEnd_ = bigfbuf + ssampsread_;
                        *step = -(*step);
                        flbufptr[0] = origSbuf + static_cast<long>(*step) * 2 * wanted;
                        reflectOffEnd = true;
                    }
                    else
                        reflectOffEnd = false;
                }
            }
            else
                reflectOffEnd = false;
        } while (reflectOffEnd);
    }

    const AnaFile& file_;
    InFile in_;
    OutFile out_;
    DoubleBuffer buffer_;
    Drand48 random_;
    int range_;
    int twicerange_ = 0;
    double stime_;
    double dur_;
    bool noZeroSteps_;
    int ssampsread_ = 0;
    int sampsRead_ = 0;
    int drnkBufno_ = -1;
    Ptr inputbufEnd_;
};
}

// Ranges (tklib1.c): the range 1 to wlength / 2 (integer division), the
// start 0 to wlength * frametime, the duration frametime to 32767 seconds.
std::vector<float> blurDrunk(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    const int range = readInteger(
        Field{"Blur Drunk", "range"}, fieldText(fields, "range"), 1.0, (double)(file.wlength / 2));
    const double stime = readNumber(Field{"Blur Drunk", "start"}, fieldText(fields, "start"), 0.0,
        (double)((float)file.wlength * file.frametime));
    const double dur = readNumber(
        Field{"Blur Drunk", "duration"}, fieldText(fields, "duration"), (double)file.frametime, BIG_TIME);
    const bool noZeroSteps = readFlag(fieldText(fields, "noZeroSteps"));
    const auto& seedText = fieldText(fields, "seed");
    double seed = 0.0;
    {
        // The seed is not a CDP parameter: a whole number as time() gives.
        std::size_t begin = 0;
        while (begin < seedText.size() && std::isspace(static_cast<unsigned char>(seedText[begin])))
            ++begin;
        const std::string word = seedText.substr(begin);
        char* end = nullptr;
        errno = 0;
        const auto value = std::strtoull(word.c_str(), &end, 10);
        while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
            ++end;
        if (word.empty() || !std::isdigit(static_cast<unsigned char>(word[0])) || end == nullptr || *end != '\0'
            || errno != 0 || value > 4294967295ULL)
        {
            refuse("Blur Drunk's seed must be a whole number from 0 to 4294967295.");
        }
        seed = static_cast<double>(value);
    }
    if (dur / file.frametime * file.wanted > static_cast<double>(maximumOutputFloats))
        refuse("The output analysis file would be longer than the port renders (2^27 floats).");
    Drunk process(file, range, stime, dur, noZeroSteps, static_cast<unsigned long>(seed));
    return process.run();
}

// specshuffle, with read_shuffle_data. Window 0 is copied first, as it is;
// then each run of domain-count groups of windows (from window 1) is
// written in the image's order, until a run cannot be completed. CDP's
// copy_domain_to_domainbuf tests the pointer, not the flag, for the end of
// the file, so its data end is always the last read's (which is where the
// buffer ends anyway). A run that cannot be read whole is dropped; CDP
// refuses a sound too short for one run.
std::vector<float> blurShuffle(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    // get_domain_and_image, consistent_domain_and_image and make_map, over
    // the command-line word (surrounding spaces dropped).
    std::string word{fieldText(fields, "domainImage")};
    while (!word.empty() && std::isspace(static_cast<unsigned char>(word.back())))
        word.pop_back();
    while (!word.empty() && std::isspace(static_cast<unsigned char>(word.front())))
        word.erase(word.begin());
    std::size_t p = 0;
    while (p < word.size() && word[p] != '-' && word[p] != ':')
    {
        if (!std::isalpha(static_cast<unsigned char>(word[p])))
            refuse(std::string{"Blur Shuffle's domain-image holds an invalid character ('"} + word[p] + "').");
        p++;
    }
    if (p >= word.size())
        refuse("Blur Shuffle's domain-image has no '-' between the domain and the image.");
    if (p == 0)
        refuse("Blur Shuffle's domain-image has no domain.");
    const std::string domain = word.substr(0, p);
    const std::string image = word.substr(p + 1);
    for (const char c : image)
        if (!std::isalpha(static_cast<unsigned char>(c)))
            refuse(std::string{"Blur Shuffle's domain-image holds an invalid character ('"} + c + "').");
    if (image.empty())
        refuse("Blur Shuffle's domain-image has no image.");
    for (std::size_t n = 0; n + 1 < domain.size(); n++)
        for (std::size_t m = n + 1; m < domain.size(); m++)
            if (domain[n] == domain[m])
                refuse(std::string{"Blur Shuffle's domain repeats the letter "} + domain[n] + ".");
    std::vector<int> map;
    for (const char c : image)
    {
        const auto at = domain.find(c);
        if (at == std::string::npos)
            refuse(std::string{"Blur Shuffle's image letter "} + c + " is not in the domain.");
        map.push_back(static_cast<int>(at));
    }
    const int grpsize = readInteger(Field{"Blur Shuffle", "groupSize"}, fieldText(fields, "groupSize"), 1.0, 32767.0);
    const int dmncnt = static_cast<int>(domain.size());
    const int imgcnt = static_cast<int>(image.size());
    const int wanted = file.wanted;
    InFile input(file);
    OutFile out;
    DoubleBuffer buffer(wanted);
    auto& flbufptr = buffer.flbufptr;
    const Ptr bigfbuf = buffer.bigfbuf;
    const long buflen = buffer.buflen;
    int permcnt = 0;
    int ssampsread = 0;
    if (static_cast<std::int64_t>(dmncnt) * grpsize * wanted > static_cast<std::int64_t>(maximumOutputFloats))
        refuse("Blur Shuffle's domain would be longer than the port holds.");
    const long shufDWindows = static_cast<long>(dmncnt) * grpsize;
    const int bufwlen = static_cast<int>(buflen / wanted);
    bool reachedEndOfInfile = false;
    const int minWindowsToGet = static_cast<int>(std::min<long>(shufDWindows + 1, bufwlen));
    flbufptr[0] = bigfbuf;
    flbufptr[1] = flbufptr[2];
    Block domainblock(static_cast<std::size_t>(shufDWindows) * static_cast<std::size_t>(wanted));
    const Ptr domainbuf{&domainblock, 0};
    const Ptr dbufend = domainbuf + shufDWindows * wanted;
    int dzSsampsread = 0;
    const auto readDomainSamps = [&](int windowsWanted) {
        dzSsampsread = input.read(bigfbuf, static_cast<int>(buflen));
        if (dzSsampsread < windowsWanted * wanted)
            return FINISHED;
        ssampsread = dzSsampsread;
        if (dzSsampsread < buflen)
            reachedEndOfInfile = true;
        return CONTINUE;
    };
    const auto insufficient = []() {
        refuse("Blur Shuffle's sound is too short for one shuffle (CDP: \"Insufficient data in soundfile to do "
               "this shuffle\").");
    };
    if (readDomainSamps(minWindowsToGet) == FINISHED)
        insufficient();
    // copy_1st_window.
    move(flbufptr[1], flbufptr[0], wanted);
    flbufptr[0] += wanted;
    flbufptr[1] += wanted;
    std::size_t written = 1;
    for (;;)
    {
        Ptr dbufptr = domainbuf;
        // copy_domain_to_domainbuf.
        Ptr dataEnd = bigfbuf + ssampsread;
        bool finished = false;
        for (long n = 0; n < shufDWindows; n++)
        {
            if (dbufptr >= dbufend)
                refuse("CDP stops: \"Error in buffer arithmetic: specshuffle()\".");
            if (flbufptr[0] >= dataEnd)
            {
                int exitStatus = FINISHED;
                if (!reachedEndOfInfile)
                {
                    const int windowsNeeded = static_cast<int>(std::min<long>(shufDWindows - n, bufwlen));
                    exitStatus = readDomainSamps(windowsNeeded);
                    if (reachedEndOfInfile)
                        dataEnd = bigfbuf + ssampsread;
                    flbufptr[0] = bigfbuf;
                }
                if (exitStatus == FINISHED)
                {
                    if (permcnt == 0)
                        insufficient();
                    finished = true;
                    break;
                }
            }
            move(dbufptr, flbufptr[0], wanted);
            dbufptr += wanted;
            flbufptr[0] += wanted;
        }
        if (finished)
            break;
        // copywindows_from_domain_to_image.
        for (int n = 0; n < imgcnt; n++)
        {
            Ptr srcptr = domainbuf + static_cast<long>(map[static_cast<std::size_t>(n)]) * grpsize * wanted;
            for (int m = 0; m < grpsize; m++)
            {
                if (srcptr >= dbufend)
                    refuse("CDP stops: \"Error in buffer arithmetic: copywindows_from_domain_to_image()\".");
                if (flbufptr[1] >= flbufptr[3])
                {
                    out.write(flbufptr[2], buflen);
                    flbufptr[1] = flbufptr[2];
                }
                if (++written * static_cast<std::size_t>(wanted) > maximumOutputFloats)
                    refuse("The output analysis file would be longer than the port renders (2^27 floats).");
                move(flbufptr[1], srcptr, wanted);
                srcptr += wanted;
                flbufptr[1] += wanted;
            }
        }
        permcnt++;
    }
    if (flbufptr[1] > flbufptr[2])
        out.write(flbufptr[2], flbufptr[1] - flbufptr[2]);
    return std::move(out.data());
}

// specweave, with read_specweave_data and check_for_viable_weave. The steps
// repeat from the first window until a step passes the file's end (the
// window reached before it is the last written). check_for_viable_weave
// works out the backtrack the buffer keeps with a bug (it sums the n-th
// step, not the m-th, from each start), so its backtrack can fall short and
// CDP stop with "Backtracking failure", which the port keeps.
std::vector<float> blurWeave(std::span<const AnaFile> in, const FieldTexts& fields)
{
    const AnaFile& file = in[0];
    // special.c: steps from -(wlength - 2) to wlength - 1.
    const long minSpecial = -(file.wlength - 2);
    const long maxSpecial = file.wlength - 1;
    std::vector<int> weav;
    for (const auto& piece : fgetsPieces(fieldText(fields, "weave")))
    {
        const char* base = piece.c_str();
        const char* p = base;
        for (;;)
        {
            // read_numeric_value_from_weavefile_text.
            while (std::isspace(static_cast<unsigned char>(*p)))
                p++;
            const char* q = p;
            if (*p == '\0')
                break;
            if (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '-')
                refuse(std::string{"Blur Weave's weave holds an invalid character ('"} + *p + "').");
            if (*p == '-')
                p++;
            const char* digitstart = p;
            while (std::isdigit(static_cast<unsigned char>(*p)))
                p++;
            if (p - digitstart <= 0)
                refuse("Blur Weave's weave holds an invalid character sequence.");
            if (!std::isspace(static_cast<unsigned char>(*p)) && *p != '\0')
                refuse("Blur Weave's weave holds an invalid character.");
            errno = 0;
            const long numero = std::strtol(q, nullptr, 10);
            if (errno != 0 || numero > maxSpecial || numero < minSpecial)
            {
                refuse("Blur Weave's step " + std::to_string(weav.size() + 1) + " is outside CDP's range, "
                    + std::to_string(minSpecial) + " to " + std::to_string(maxSpecial) + ".");
            }
            weav.push_back(static_cast<int>(numero));
        }
    }
    if (weav.empty())
        refuse("Blur Weave's weave holds no steps (CDP: \"No data in weave file\").");
    const int itemcnt = static_cast<int>(weav.size());
    // check_for_viable_weave.
    long weavhere = 0;
    for (int n = 0; n < itemcnt; n++)
    {
        if ((weavhere += weav[static_cast<std::size_t>(n)]) < 0)
            refuse("Blur Weave's weave steps to before its start (CDP: \"You cannot weave to before weave-start\").");
    }
    if (weavhere <= 0)
        refuse("Blur Weave's weave must end ahead of its start (CDP: \"Weave must progress aint file\").");
    long maxbaktrak = 0;
    for (int n = 1; n < itemcnt; n++)
    {
        long posRelativeToThisStart = 0;
        long maxbaktrakFromThisStart = 0;
        for (int m = n; m < itemcnt; m++)
        {
            // CDP adds weav[n] where weav[m] was meant.
            if ((posRelativeToThisStart += weav[static_cast<std::size_t>(n)]) < 0)
                maxbaktrakFromThisStart = std::min(posRelativeToThisStart, maxbaktrakFromThisStart);
        }
        maxbaktrak = std::min(maxbaktrak, maxbaktrakFromThisStart);
    }
    const long baktrak = -maxbaktrak;
    if (static_cast<std::int64_t>(baktrak) * file.wanted > static_cast<std::int64_t>(maximumOutputFloats))
        refuse("Blur Weave's backtrack would be longer than the port holds.");
    // allocate_weave_buffer: the backtrack, the input buffer, the output
    // buffer.
    const int wanted = file.wanted;
    const long buflen = static_cast<long>(wanted) * BUF_MULTIPLIER;
    Block block(static_cast<std::size_t>(buflen * 2 + baktrak * wanted));
    const Ptr bigfbuf{&block, 0};
    Ptr flbufptr[5];
    flbufptr[0] = bigfbuf + baktrak * wanted;
    flbufptr[2] = flbufptr[0] + buflen;
    flbufptr[3] = flbufptr[2] + buflen;
    flbufptr[4] = flbufptr[2] - baktrak * wanted;
    InFile input(file);
    OutFile out;
    Ptr insbuf = flbufptr[0];
    Ptr outsbuf = flbufptr[2];
    const long wrapsamps = baktrak * wanted;
    // read_first_inbuf.
    flbufptr[1] = flbufptr[0] + input.read(flbufptr[0], static_cast<int>(buflen));
    int n = 0;
    std::size_t written = 0;
    for (;;)
    {
        if (++written * static_cast<std::size_t>(wanted) > maximumOutputFloats)
            refuse("The output analysis file would be longer than the port renders (2^27 floats).");
        move(outsbuf, insbuf, wanted);
        insbuf += static_cast<long>(weav[static_cast<std::size_t>(n)]) * wanted;
        if (insbuf < bigfbuf)
            refuse("Blur Weave's weave steps back further than CDP keeps (CDP: \"Backtracking failure in "
                   "specweave()\").");
        if (insbuf >= flbufptr[1])
        {
            // wrap_samps.
            move(bigfbuf, flbufptr[4], wrapsamps);
            const int ssampsread = input.read(flbufptr[0], static_cast<int>(buflen));
            bool finished = ssampsread == 0;
            if (!finished)
            {
                flbufptr[1] = flbufptr[0] + ssampsread;
                if ((insbuf -= buflen) >= flbufptr[1])
                    finished = true;
            }
            if (finished)
            {
                outsbuf += wanted;
                break;
            }
        }
        if (++n >= itemcnt)
            n = 0;
        if ((outsbuf += wanted) >= flbufptr[3])
        {
            out.write(flbufptr[2], buflen);
            outsbuf = flbufptr[2];
        }
    }
    if (!(outsbuf == flbufptr[2]))
        out.write(flbufptr[2], outsbuf - flbufptr[2]);
    return std::move(out.data());
}
}
