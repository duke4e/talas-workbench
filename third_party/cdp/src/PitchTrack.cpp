/*
 * Derived from the CDP System, Release 8: dev/repitch/repitch.c and
 * ap_repitch.c, dev/pitch/pitch.c and ap_pitch.c, dev/pv/pvoc.c,
 * dev/cdp2k/formantsg.c, parstruct.c, readfiles.c, tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/PitchTrack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double FLTERR = 0.000002;
constexpr double MINPITCH = 9.0;
constexpr double NOT_PITCH = -1.0;
constexpr double NOT_SOUND = -2.0;
constexpr double SEMITONE_INTERVAL = 1.05946309436;
constexpr double SPAN_FACTOR = 1.5;
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
// MAXFLOAT, which <math.h> also defines.
constexpr float CDP_MAXFLOAT = (float)3.40282346638528860e+38;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
constexpr int MAXIMI = 8;
constexpr int CHANSCAN = 8;
constexpr int PEAKSCAN = 2;
constexpr int MAXHARM = 8;
// dev/include/pvoc.h.
constexpr long long VERY_BIG_INT = 100000000;
constexpr double PVOC_CONSTANT_A = 8.0;
// dev/include/tkglobals.h: getpitch reads its .ana file BUF_MULTIPLIER
// windows at a time (allocate_single_buffer_plus_extra_pointer).
constexpr int BUF_MULTIPLIER = 20;
// dev/repitch/repitch.c.
constexpr double EIGHT_OVER_SEVEN = 1.142857143;
constexpr int MAXIMUM_PARTIAL = 64;
constexpr double ALMOST_TWO = 1.75;
constexpr double PEAK_LIMIT = .05;
constexpr int MIN_SMOOTH_SET = 3;
constexpr double MAX_GLISRATE = 16.0;

// CDP's round, lround.
[[nodiscard]] int cdpRound(double x) noexcept
{
    return static_cast<int>(std::lround(x));
}

// flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// pow(10.0, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

// get_totalamp.
[[nodiscard]] double get_totalamp(const float* sbuf, int wanted) noexcept
{
    double totalamp = 0.0;
    for (int vc = 0; vc < wanted; vc += 2)
        totalamp += sbuf[vc];
    return totalamp;
}

// normalise.
void normalise(float* flbufptr, int wanted, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const double normaliser = pre_totalamp / post_totalamp;
    for (int vc = 0; vc < wanted; vc += 2)
        flbufptr[vc] = (float)(flbufptr[vc] * normaliser);
}

// is_harmonic (formantsg.c): whether frq1 is a harmonic of frq2, within a
// semitone, and which. CDP fails for a fundamental under FLTERR, which
// the callers here never pass.
[[nodiscard]] bool is_harmonic(int* iratio, double frq1, double frq2) noexcept
{
    double ratio, intvl;
    ratio = frq1 / frq2;
    *iratio = cdpRound(ratio);
    if (ratio > *iratio)
        intvl = ratio / (double)(*iratio);
    else
        intvl = (double)(*iratio) / ratio;
    if (intvl > SEMITONE_INTERVAL)
        return false;
    return true;
}

struct Sizes
{
    int chans = 0;
    int m = 0;
    int d = 0;
};

// pvoc.c's window and decimation for the points and overlap, as
// PhaseVocoder sets them.
[[nodiscard]] std::optional<Sizes> sizesFor(int points, int overlap) noexcept
{
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!d.has_value())
        return std::nullopt;
    Sizes sizes;
    sizes.chans = points + (points % 2);
    switch (overlap - 1)
    {
    case 0: sizes.m = 4 * sizes.chans; break;
    case 1: sizes.m = 2 * sizes.chans; break;
    case 2: sizes.m = sizes.chans; break;
    default: sizes.m = sizes.chans / 2; break;
    }
    sizes.d = (int)(sizes.m / PVOC_CONSTANT_A);
    if (sizes.d == 0)
        sizes.d = 1;
    if (sizes.d != *d)
        return std::nullopt;
    return sizes;
}

// The frames pvoc_process reads (it stops at VERY_BIG_INT) and the windows
// it writes, counted as its main loop counts them.
struct WindowPlan
{
    std::size_t frames = 0;
    std::size_t windows = 0;
};

[[nodiscard]] std::optional<WindowPlan> windowPlan(std::size_t frames, int points, int overlap) noexcept
{
    const auto sizes = sizesFor(points, overlap);
    if (frames == 0 || !sizes.has_value())
        return std::nullopt;
    const long long D = sizes->d;
    const long long analWinLen = sizes->m / 2;
    const auto available = static_cast<long long>(std::min<std::size_t>(frames, VERY_BIG_INT + 1));
    long long nI = -(analWinLen / D) * D;
    long long Dd = analWinLen + nI + 1;
    long long endsamp = VERY_BIG_INT;
    long long read = 0;
    long long windows = 0;
    bool flag = true;
    while (nI < (endsamp + analWinLen))
    {
        const long long got = std::min(Dd, std::max(0LL, available - read));
        read += got;
        if (got < Dd)
            Dd = got;
        ++windows;
        if (flag && (nI > 0) && (Dd < D))
        {
            flag = false;
            endsamp = nI + analWinLen - (D - Dd);
        }
        nI += D;
        Dd = std::min(D, std::max(0LL, D + endsamp - nI - analWinLen));
    }
    WindowPlan plan;
    plan.frames = static_cast<std::size_t>(read);
    plan.windows = static_cast<std::size_t>(windows);
    return plan;
}

// getpitch's state over a sound: outer_pitch_loop and specpitch for each
// window, then tidy_up_pitch_data.
class PitchFinder
{
public:
    PitchFinder(int wanted, double nyquist, float frametime, std::size_t windows, const GetPitchSettings& settings)
        : wanted_(wanted)
        , clength_(wanted / 2)
        , chwidth_(nyquist / (double)(wanted / 2 - 1))
        , halfchwidth_(chwidth_ / 2.0)
        , wlength_(static_cast<int>(windows))
        , frametime_(frametime)
        , pretotamp_(windows, 0.0)
        , pitches_(windows, 0.0F)
        , block_(static_cast<std::size_t>(wanted) * BUF_MULTIPLIER, 0.0F)
    {
        // adjust_parameters_for_specpitch: semitones to a ratio, dB to a
        // gain. CDP8's build calls __exp10 for pow(10.0, ...).
        rnge_ = std::pow(SEMITONE_INTERVAL, std::fabs(settings.tuningRange));
        double sratio = settings.signalToNoise;
        sratio /= 20.0;
        sratio = cdpExp10(sratio);
        sratio_ = 1.0 / sratio;
        valid_ = settings.minimumPitchedWindows;
        match_ = settings.harmonicsToConfirm;
        lolm_ = settings.lowestPitch;
        hilm_ = settings.highestPitch.value_or(nyquist / MAXIMI);
        alternative_ = settings.alternativeMethod;
        keepZeros_ = settings.keepUnpitched;
        // setup_ring, then initialise_ring_vals's extend_ring to MAXIMI:
        // the ring's nodes in the order malloc'd, their locations unset.
        for (int n = 0; n < MAXIMI; n++)
        {
            ring_[n].next = (n + 1) % MAXIMI;
            ring_[n].last = (n + MAXIMI - 1) % MAXIMI;
        }
        ringhead_ = 0;
    }

    // outer_pitch_loop, for the next window of the .ana file.
    void window(std::span<const float> frame) noexcept
    {
        const int w = totalWindows_++;
        float* buf = block_.data() + static_cast<std::ptrdiff_t>(w % BUF_MULTIPLIER) * wanted_;
        std::copy(frame.begin(), frame.end(), buf);
        if (w == 0 && wlength_ > 1)
        {
            pitches_[0] = (float)NOT_PITCH;
            return;
        }
        pretotamp_[static_cast<std::size_t>(w)] = get_totalamp(buf, wanted_);
        specpitch(w, buf);
    }

    // tidy_up_pitch_data; false where CDP finds no valid pitch.
    [[nodiscard]] bool tidy() noexcept
    {
        anti_noise_smoothing();
        mark_zeros_in_pitchdata();
        eliminate_blips_in_pitch_data();
        if (!keepZeros_)
            return interpolate_pitch();
        return pitch_found();
    }

    [[nodiscard]] std::vector<float> pitches() && { return std::move(pitches_); }

private:
    struct Chanval
    {
        float val = 0.0F;
        int loc = 0;
        int next = 0;
        int last = 0;
    };

    void specpitch(int w, float* flbuf) noexcept;
    [[nodiscard]] bool close_to_frq_already_in_ring(int* there, double frq1, const float* flbuf) const noexcept;
    void substitute_in_ring(int vc, int here, int there, const float* flbuf) noexcept;
    void insert_in_ring(int vc, int here, const float* flbuf) noexcept;
    void put_ring_frqs_in_ascending_order(std::array<int, MAXIMI>& partials, float* minamp,
        const float* flbuf) noexcept;
    [[nodiscard]] bool found_pitch_1(const std::array<int, MAXIMI>& partials, double lo_loud_partial,
        double hi_loud_partial, float minamp, int w, const float* flbuf) noexcept;
    [[nodiscard]] bool found_pitch_2(const std::array<int, MAXIMI>& partials) noexcept;
    void smooth_spurious_octave_leaps(int pitchno, float minamp, int w) noexcept;
    [[nodiscard]] bool equivalent_pitches(double frq1, double frq2) const noexcept;
    [[nodiscard]] bool is_peak_at(double frq, int window_offset, float minamp, int w) const noexcept;
    [[nodiscard]] bool local_peak(int thiscc, double frq, const float* thisbuf) const noexcept;
    [[nodiscard]] bool enough_partials_are_harmonics(const std::array<int, MAXIMI>& partials,
        const float* flbuf) const noexcept;
    [[nodiscard]] bool is_a_harmonic(double frq1, double frq2) const noexcept;

    void anti_noise_smoothing() noexcept;
    [[nodiscard]] bool is_smooth_from_both_sides(int n, double max_pglide) const noexcept;
    [[nodiscard]] bool is_initialpitch_smooth(const std::vector<char>& smooth, double max_pglide) const noexcept;
    [[nodiscard]] bool is_finalpitch_smooth(const std::vector<char>& smooth, double max_pglide) const noexcept;
    [[nodiscard]] bool is_smooth_from_before(int n, const std::vector<char>& smooth, double max_pglide) const noexcept;
    [[nodiscard]] bool is_smooth_from_after(int n, const std::vector<char>& smooth, double max_pglide) const noexcept;
    void test_glitch_sets(std::vector<char>& smooth, double max_pglide) const noexcept;
    void test_glitch_forwards(
        int gltchstart, int gltchend, std::vector<char>& smooth, double max_pglide) const noexcept;
    void test_glitch_backwards(
        int gltchstart, int gltchend, std::vector<char>& smooth, double max_pglide) const noexcept;
    void mark_zeros_in_pitchdata() noexcept;
    void eliminate_blips_in_pitch_data() noexcept;
    [[nodiscard]] bool interpolate_pitch() noexcept;
    [[nodiscard]] bool do_interpolating(int* pitchno) noexcept;
    [[nodiscard]] bool pitch_found() const noexcept;

    int wanted_;
    int clength_;
    double chwidth_;
    double halfchwidth_;
    int wlength_;
    float frametime_;
    double rnge_ = 1.0;
    double sratio_ = 0.0;
    int valid_ = 0;
    int match_ = 0;
    double lolm_ = 0.0;
    double hilm_ = 0.0;
    bool alternative_ = false;
    bool keepZeros_ = false;
    // dz->param[PICH_PICH].
    double pich_ = 0.0;
    std::array<Chanval, MAXIMI> ring_{};
    int ringhead_ = 0;
    int totalWindows_ = 0;
    std::vector<double> pretotamp_;
    std::vector<float> pitches_;
    // dz->bigfbuf: the windows of the current read, which is_peak_at
    // reaches back into, rectified as specpitch leaves them.
    std::vector<float> block_;
};

// specpitch.
void PitchFinder::specpitch(int w, float* flbuf) noexcept
{
    std::array<int, MAXIMI> partials{};
    float minamp;
    double loudest_partial_frq, nextloudest_partial_frq, lo_loud_partial, hi_loud_partial;
    // initialise_ring_vals(MAXIMI, -1.0).
    {
        int tthis = ringhead_;
        do
        {
            ring_[tthis].val = (float)-1.0;
        } while ((tthis = ring_[tthis].next) != ringhead_);
    }
    // rectify_frqs.
    for (int vc = 0; vc < wanted_; vc += 2)
    {
        if (flbuf[vc + 1] < 0.0f)
            flbuf[vc + 1] = (float)(-(flbuf[vc + 1]));
    }
    for (int vc = 0; vc < wanted_; vc += 2)
    {
        int here = ringhead_;
        if (flbuf[vc + 1] > lolm_)
        {
            do
            {
                if (flbuf[vc] > ring_[here].val)
                {
                    int there = 0;
                    if (close_to_frq_already_in_ring(&there, (double)flbuf[vc + 1], flbuf))
                    {
                        if (flbuf[vc] > ring_[there].val)
                            substitute_in_ring(vc, here, there, flbuf);
                    }
                    else
                        insert_in_ring(vc, here, flbuf);
                    break;
                }
            } while ((here = ring_[here].next) != ringhead_);
        }
    }
    loudest_partial_frq = flbuf[ring_[ringhead_].loc + 1];
    nextloudest_partial_frq = flbuf[ring_[ring_[ringhead_].next].loc + 1];
    if (loudest_partial_frq < nextloudest_partial_frq)
    {
        lo_loud_partial = loudest_partial_frq;
        hi_loud_partial = nextloudest_partial_frq;
    }
    else
    {
        lo_loud_partial = nextloudest_partial_frq;
        hi_loud_partial = loudest_partial_frq;
    }
    put_ring_frqs_in_ascending_order(partials, &minamp, flbuf);
    // found_pitch.
    const bool found = alternative_ ? found_pitch_2(partials)
                                    : found_pitch_1(partials, lo_loud_partial, hi_loud_partial, minamp, w, flbuf);
    if (found && pich_ >= MINPITCH)
        pitches_[static_cast<std::size_t>(w)] = (float)pich_;
    else
        pitches_[static_cast<std::size_t>(w)] = (float)NOT_PITCH;
    smooth_spurious_octave_leaps(w, minamp, w);
}

// close_to_frq_already_in_ring.
bool PitchFinder::close_to_frq_already_in_ring(int* there, double frq1, const float* flbuf) const noexcept
{
    double frq2, frqratio;
    *there = ringhead_;
    do
    {
        if (ring_[*there].val > 0.0)
        {
            frq2 = flbuf[ring_[*there].loc + 1];
            if (frq1 > frq2)
                frqratio = frq1 / frq2;
            else
                frqratio = frq2 / frq1;
            if (frqratio < EIGHT_OVER_SEVEN)
                return true;
        }
    } while ((*there = ring_[*there].next) != ringhead_);
    return false;
}

// substitute_in_ring. here differs from there only when there is not the
// ring's head (CDP's "IMPOSSIBLE!" never arises).
void PitchFinder::substitute_in_ring(int vc, int here, int there, const float* flbuf) noexcept
{
    if (here != there)
    {
        const int spare = there;
        ring_[ring_[there].next].last = ring_[there].last; /* SPLICE REDUNDANT STRUCT FROM RING */
        ring_[ring_[there].last].next = ring_[there].next;
        const int previous = ring_[here].last;
        ring_[previous].next = spare; /* SPLICE ITS ADDRESS-SPACE BACK INTO RING */
        ring_[spare].last = previous; /* IMMEDIATELY BEFORE HERE */
        ring_[here].last = spare;
        ring_[spare].next = here;
        if (here == ringhead_) /* IF HERE IS RINGHEAD, MOVE RINGHEAD */
            ringhead_ = spare;
        here = spare; /* POINT TO INSERT LOCATION */
    }
    ring_[here].val = flbuf[vc];
    ring_[here].loc = vc;
}

// insert_in_ring.
void PitchFinder::insert_in_ring(int vc, int here, const float* flbuf) noexcept
{
    int spare;
    if (here == ringhead_)
    {
        ringhead_ = ring_[ringhead_].last;
        spare = ringhead_;
    }
    else
    {
        if (here == ring_[ringhead_].last)
            spare = here;
        else
        {
            spare = ring_[ringhead_].last;
            const int newend = ring_[ring_[ringhead_].last].last; /* cut ENDADR (spare) out of ring */
            ring_[ringhead_].last = newend;
            ring_[newend].next = ringhead_;
            const int previous = ring_[here].last;
            ring_[here].last = spare; /* reuse spare address at new loc by */
            ring_[spare].next = here; /* inserting it back into ring before HERE */
            ring_[previous].next = spare;
            ring_[spare].last = previous;
        }
    }
    ring_[spare].val = flbuf[vc];
    ring_[spare].loc = vc;
}

// put_ring_frqs_in_ascending_order. CDP fails for an amplitude of
// MAXFLOAT or more, which an analysis never holds.
void PitchFinder::put_ring_frqs_in_ascending_order(
    std::array<int, MAXIMI>& partials, float* minamp, const float* flbuf) noexcept
{
    int k;
    int start, ggot, here = ringhead_;
    float minpitch;
    *minamp = CDP_MAXFLOAT;
    for (k = 0; k < MAXIMI; k++)
    {
        // sfsys.h's min.
        *minamp = (flbuf[ring_[here].loc] > *minamp) ? *minamp : flbuf[ring_[here].loc];
        ring_[here].loc++; /* CHANGE RING TO POINT TO FRQS, not AMPS */
        ring_[here].val = flbuf[ring_[here].loc];
        here = ring_[here].next;
    }
    here = ringhead_;
    minpitch = flbuf[ring_[here].loc];
    for (k = 1; k < MAXIMI; k++)
    {
        start = ggot = here;
        while ((here = ring_[here].next) != start)
        { /* Find lowest frq */
            if (flbuf[ring_[here].loc] < minpitch)
            {
                minpitch = flbuf[ring_[here].loc];
                ggot = here;
            }
        }
        partials[static_cast<std::size_t>(k - 1)] = ggot; /* Save its address */
        here = ring_[ggot].next; /* Move to next ring site */
        minpitch = flbuf[ring_[here].loc]; /* Preset minfrq to val there */
        ring_[ring_[ggot].last].next = here; /* Unlink ringsite ggot */
        ring_[here].last = ring_[ggot].last;
    }
    partials[static_cast<std::size_t>(k - 1)] = here; /* Remaining ringsite is maximum */
    here = ringhead_ = partials[0]; /* Reconstruct ring */
    for (k = 1; k < MAXIMI; k++)
    {
        ring_[here].next = partials[static_cast<std::size_t>(k)];
        ring_[partials[static_cast<std::size_t>(k)]].last = here;
        here = ring_[here].next;
    }
    ring_[here].next = ringhead_; /* Close up ring */
    ring_[ringhead_].last = here;
}

// found_pitch_1: endd carries over from one ratio to the next.
bool PitchFinder::found_pitch_1(const std::array<int, MAXIMI>& partials, double lo_loud_partial,
    double hi_loud_partial, float minamp, int w, const float* flbuf) noexcept
{
    int n, m, k, maximi_less_one = MAXIMUM_PARTIAL - 1, endd = 0;
    double whole_number_ratio, comparison_frq;
    for (n = 1; n < maximi_less_one; n++)
    {
        for (m = n + 1; m < MAXIMUM_PARTIAL; m++)
        { /* NOV 7 */
            whole_number_ratio = (double)m / (double)n;
            comparison_frq = lo_loud_partial * whole_number_ratio;
            if (equivalent_pitches(comparison_frq, hi_loud_partial))
                endd = (MAXIMUM_PARTIAL / m) * n; /* explanation at foot of file */
            else if (comparison_frq > hi_loud_partial)
                break;
            for (k = n; k <= endd; k += n)
            {
                pich_ = lo_loud_partial / (double)k;
                if (pich_ > hilm_)
                    continue;
                if (pich_ < lolm_)
                    break;
                if (is_peak_at(pich_, 0, minamp, w))
                {
                    if (match_ <= 2)
                        return true;
                    else if (enough_partials_are_harmonics(partials, flbuf))
                        return true;
                }
            }
        }
    }
    return false;
}

// found_pitch_2: pitchdiff and diffcount carry over from one candidate to
// the next; with -n1 no candidate is ever confirmed.
bool PitchFinder::found_pitch_2(const std::array<int, MAXIMI>& partials) noexcept
{
    int m, n, k, good_match;
    int top_of_test = MAXIMI - match_ + 1;
    double resolved_pitch, prevpitch, pitchdiff = 0.0;
    int diffcount = 0;
    for (n = 0; n < top_of_test; n++)
    {
        for (m = 1; m <= MAXHARM; m++)
        {
            pich_ = (ring_[partials[static_cast<std::size_t>(n)]].val) / (double)m;
            if (pich_ > hilm_)
                continue;
            if (pich_ < lolm_)
                break;
            good_match = 1;
            prevpitch = pich_;
            if (match_ > 1)
            {
                for (k = n + 1; k < MAXIMI; k++)
                {
                    const float val = ring_[partials[static_cast<std::size_t>(k)]].val;
                    if (is_a_harmonic((double)val, pich_))
                    {
                        pitchdiff += (double)(val - prevpitch);
                        prevpitch = (double)val;
                        diffcount++;
                        if (++good_match >= match_)
                        {
                            resolved_pitch = pitchdiff / (double)diffcount;
                            if (equivalent_pitches(resolved_pitch, pich_))
                            {
                                ring_[partials[0]].val = (float)resolved_pitch;
                                pich_ = resolved_pitch;
                            }
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

// smooth_spurious_octave_leaps: a drop of more than ALMOST_TWO rewrites
// earlier windows, testing their peaks only within the current read.
void PitchFinder::smooth_spurious_octave_leaps(int pitchno, float minamp, int w) noexcept
{
    double thispitch = pitches_[static_cast<std::size_t>(pitchno)];
    double startpitch, lastpitch;
    int k = 0;
    if (pitchno <= 0)
        return;
    lastpitch = pitches_[static_cast<std::size_t>(pitchno - 1)];
    if (lastpitch > lolm_ && thispitch > lolm_)
    { /* OCTAVE ADJ HERE */
        if (thispitch > lastpitch)
        { /* OCTAVE ADJ FORWARDS */
            startpitch = thispitch;
            while (thispitch / lastpitch > ALMOST_TWO)
                thispitch /= 2.0;
            if (thispitch != startpitch)
            {
                if (thispitch < lolm_)
                    return;
                if (is_peak_at(thispitch, 0, minamp, w))
                    pitches_[static_cast<std::size_t>(pitchno)] = (float)thispitch;
                else
                    pitches_[static_cast<std::size_t>(pitchno)] = (float)startpitch;
            }
            return;
        }
        while (pitchno >= 1)
        { /* OCTAVE ADJ BCKWARDS */
            k++;
            if ((thispitch = pitches_[static_cast<std::size_t>(pitchno--)]) < lolm_)
                return;
            if ((lastpitch = pitches_[static_cast<std::size_t>(pitchno)]) < lolm_)
                return;
            startpitch = lastpitch;
            while (lastpitch / thispitch > ALMOST_TWO)
                lastpitch /= 2.0;
            if (lastpitch != startpitch)
            {
                if (lastpitch < lolm_)
                    return;
                if (is_peak_at(lastpitch, k, minamp, w))
                    pitches_[static_cast<std::size_t>(pitchno)] = (float)lastpitch;
                else
                    pitches_[static_cast<std::size_t>(pitchno)] = (float)startpitch;
            }
        }
    }
}

// equivalent_pitches.
bool PitchFinder::equivalent_pitches(double frq1, double frq2) const noexcept
{
    double ratio;
    int iratio;
    double intvl;
    ratio = frq1 / frq2;
    iratio = cdpRound(ratio);
    if (iratio != 1)
        return false;
    if (ratio > iratio)
        intvl = ratio / (double)iratio;
    else
        intvl = (double)iratio / ratio;
    if (intvl > rnge_)
        return false;
    return true;
}

// is_peak_at: a window window_offset back is read from the current read of
// BUF_MULTIPLIER windows, and is no peak when it lies in an earlier read.
bool PitchFinder::is_peak_at(double frq, int window_offset, float minamp, int w) const noexcept
{
    const float* thisbuf;
    int cc, vc, searchtop, searchbot;
    if (window_offset)
    { /* BAKTRAK ALONG BIGBUF, IF NESS */
        const int earlier = w - window_offset;
        if (earlier < 0 || earlier / BUF_MULTIPLIER != w / BUF_MULTIPLIER)
            return false;
        thisbuf = block_.data() + static_cast<std::ptrdiff_t>(earlier % BUF_MULTIPLIER) * wanted_;
    }
    else
        thisbuf = block_.data() + static_cast<std::ptrdiff_t>(w % BUF_MULTIPLIER) * wanted_;
    cc = (int)((frq + halfchwidth_) / chwidth_); /* TRUNCATE */
    searchtop = std::min(clength_, cc + CHANSCAN + 1);
    searchbot = std::max(0, cc - CHANSCAN);
    for (cc = searchbot, vc = searchbot * 2; cc < searchtop; cc++, vc += 2)
    {
        if (!equivalent_pitches((double)thisbuf[vc + 1], frq))
            continue;
        if (thisbuf[vc] < minamp * PEAK_LIMIT)
            continue;
        if (local_peak(cc, frq, thisbuf))
            return true;
    }
    return false;
}

// local_peak.
bool PitchFinder::local_peak(int thiscc, double frq, const float* thisbuf) const noexcept
{
    int thisvc = thiscc * 2;
    int cc, vc, searchtop, searchbot;
    double frqtop = frq * SEMITONE_INTERVAL;
    double frqbot = frq / SEMITONE_INTERVAL;
    searchtop = (int)((frqtop + halfchwidth_) / chwidth_); /* TRUNCATE */
    searchtop = std::min(clength_, searchtop + PEAKSCAN + 1);
    searchbot = (int)((frqbot + halfchwidth_) / chwidth_); /* TRUNCATE */
    searchbot = std::max(0, searchbot - PEAKSCAN);
    for (cc = searchbot, vc = searchbot * 2; cc < searchtop; cc++, vc += 2)
    {
        if (thisbuf[thisvc] < thisbuf[vc])
            return false;
    }
    return true;
}

// enough_partials_are_harmonics.
bool PitchFinder::enough_partials_are_harmonics(
    const std::array<int, MAXIMI>& partials, const float* flbuf) const noexcept
{
    int n, good_match = 0;
    double thisfrq;
    for (n = 0; n < MAXIMI; n++)
    {
        if ((thisfrq = flbuf[ring_[partials[static_cast<std::size_t>(n)]].loc]) < pich_)
            continue;
        if (is_a_harmonic(thisfrq, pich_))
        {
            if (++good_match >= match_)
                return true;
        }
    }
    return false;
}

// is_a_harmonic.
bool PitchFinder::is_a_harmonic(double frq1, double frq2) const noexcept
{
    double ratio = frq1 / frq2;
    int iratio = cdpRound(ratio);
    double intvl;
    if (ratio > iratio)
        intvl = ratio / (double)iratio;
    else
        intvl = (double)iratio / ratio;
    if (intvl > rnge_)
        return false;
    return true;
}

// anti_noise_smoothing. CDP8's build calls exp2 for pow(2.0, ...).
void PitchFinder::anti_noise_smoothing() noexcept
{
    const int wlength = wlength_;
    if (wlength < MIN_SMOOTH_SET + 1)
        return;
    const double max_pglide = std::exp2(MAX_GLISRATE * frametime_);
    std::vector<char> smooth(static_cast<std::size_t>(wlength), 0);
    int n;
    for (n = 1; n < wlength - 1; n++)
        smooth[static_cast<std::size_t>(n)] = (char)is_smooth_from_both_sides(n, max_pglide);
    smooth[0] = (char)is_initialpitch_smooth(smooth, max_pglide);
    smooth[static_cast<std::size_t>(wlength - 1)] = (char)is_finalpitch_smooth(smooth, max_pglide);
    for (n = MIN_SMOOTH_SET - 1; n < wlength; n++)
    {
        if (!smooth[static_cast<std::size_t>(n)])
            smooth[static_cast<std::size_t>(n)] = (char)is_smooth_from_before(n, smooth, max_pglide);
    }
    for (n = 0; n <= wlength - MIN_SMOOTH_SET; n++)
    {
        if (!smooth[static_cast<std::size_t>(n)])
            smooth[static_cast<std::size_t>(n)] = (char)is_smooth_from_after(n, smooth, max_pglide);
    }
    test_glitch_sets(smooth, max_pglide);
    // remove_unsmooth_pitches.
    for (n = 0; n < wlength; n++)
    {
        if (!smooth[static_cast<std::size_t>(n)])
            pitches_[static_cast<std::size_t>(n)] = (float)NOT_PITCH;
    }
}

// is_smooth_from_both_sides: CDP divides the float pitches in float.
bool PitchFinder::is_smooth_from_both_sides(int n, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    float thispitch, pitch_before, pitch_after;
    double pre_interval, post_interval;
    if ((thispitch = pitches[n]) < FLTERR)
        return false;
    if ((pitch_before = pitches[n - 1]) < FLTERR)
        return false;
    if ((pitch_after = pitches[n + 1]) < FLTERR)
        return false;
    pre_interval = pitch_before / thispitch;
    if (pre_interval < 1.0)
        pre_interval = 1.0 / pre_interval;
    post_interval = pitch_after / thispitch;
    if (post_interval < 1.0)
        post_interval = 1.0 / post_interval;
    if (pre_interval > max_pglide || post_interval > max_pglide)
        return false;
    return true;
}

// is_initialpitch_smooth.
bool PitchFinder::is_initialpitch_smooth(const std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    float thispitch;
    int n;
    double post_interval;
    if ((thispitch = pitches[0]) < FLTERR)
        return false;
    for (n = 1; n < MIN_SMOOTH_SET; n++)
    {
        if (smooth[static_cast<std::size_t>(n)])
        {
            post_interval = pitches[n] / pitches[0];
            if (post_interval < 1.0)
                post_interval = 1.0 / post_interval;
            if (post_interval <= std::pow(max_pglide, (double)n))
                return true;
        }
    }
    return false;
}

// is_finalpitch_smooth.
bool PitchFinder::is_finalpitch_smooth(const std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    float thispitch;
    double pre_interval;
    int n;
    int last = wlength_ - 1;
    if ((thispitch = pitches[last]) < FLTERR)
        return false;
    for (n = 1; n < MIN_SMOOTH_SET; n++)
    {
        if (smooth[static_cast<std::size_t>(last - n)])
        {
            pre_interval = pitches[last - n] / pitches[last];
            if (pre_interval < 1.0)
                pre_interval = 1.0 / pre_interval;
            if (pre_interval <= std::pow(max_pglide, (double)n))
                return true;
        }
    }
    return false;
}

// is_smooth_from_before.
bool PitchFinder::is_smooth_from_before(int n, const std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    float thispitch, pitch_before;
    double pre_interval;
    int m;
    if ((thispitch = pitches[n]) < FLTERR)
        return false;
    for (m = 1; m < MIN_SMOOTH_SET; m++)
    { /* If there are (MIN_SMOOTH_SET-1) smooth pitches before */
        if (!smooth[static_cast<std::size_t>(n - m)])
            return false;
    }
    pitch_before = pitches[n - 1]; /* Test the interval with the previous pitch */
    pre_interval = pitch_before / thispitch;
    if (pre_interval < 1.0)
        pre_interval = 1.0 / pre_interval;
    if (pre_interval > max_pglide)
        return false; /* And if it's acceptably smooth */
    return true; /* mark this pitch as smooth also */
}

// is_smooth_from_after.
bool PitchFinder::is_smooth_from_after(int n, const std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    float thispitch, pitch_after;
    double post_interval;
    int m;
    if ((thispitch = pitches[n]) < FLTERR)
        return false;
    for (m = 1; m < MIN_SMOOTH_SET; m++)
    { /* If there are (MIN_SMOOTH_SET-1) smooth pitches after */
        if (!smooth[static_cast<std::size_t>(n + m)])
            return false;
    }
    pitch_after = pitches[n + 1]; /* Test the interval with the next pitch */
    post_interval = pitch_after / thispitch;
    if (post_interval < 1.0)
        post_interval = 1.0 / post_interval;
    if (post_interval > max_pglide)
        return false; /* And if it's acceptably smooth */
    return true; /* mark this pitch as smooth also */
}

// test_glitch_sets: a run of unverified windows is searched from its start
// and, unless it runs to the end of the data, from its end, however far
// ahead that lies.
void PitchFinder::test_glitch_sets(std::vector<char>& smooth, double max_pglide) const noexcept
{
    bool gotglitch = false;
    int n, gltchend, gltchstart = 0;
    for (n = 0; n < wlength_; n++)
    {
        if (gotglitch)
        { /* if inside a glitch */
            if (smooth[static_cast<std::size_t>(n)])
            { /* if reached its end, mark the end, then process the glitch */
                gltchend = n;
                test_glitch_forwards(gltchstart, gltchend, smooth, max_pglide);
                test_glitch_backwards(gltchstart, gltchend, smooth, max_pglide);
                gotglitch = false;
            }
        }
        else
        { /* look for a glitch and mark its start */
            if (!smooth[static_cast<std::size_t>(n)])
            {
                gotglitch = true;
                gltchstart = n;
            }
        }
    }
    if (gotglitch)
    { /* if inside a glitch at end of data, process glitch */
        gltchend = n;
        test_glitch_forwards(gltchstart, gltchend, smooth, max_pglide);
    }
}

// test_glitch_forwards. The window before a glitch is verified, so holds a
// pitch (CDP's "Error in previous smoothing logic" never arises).
void PitchFinder::test_glitch_forwards(
    int gltchstart, int gltchend, std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    int n, glcnt;
    int last_smooth, previous;
    double pre_interval;
    if ((previous = gltchstart - 1) < 0)
        return;
    if (pitches[previous] < FLTERR)
        return;
    last_smooth = previous;
    n = gltchstart + 1; /* setup params for local search of glitch */
    glcnt = 1;
    while (n < gltchend)
    { /* look through the glitch */
        if (pitches[n] > FLTERR)
        { /* if glitch location holds a true pitch */
            pre_interval = pitches[n] / pitches[previous];
            if (pre_interval < 1.0)
                pre_interval = 1.0 / pre_interval; /* compare against previous verified pitch */
            if (pre_interval <= std::pow(max_pglide, (double)(n - previous)))
            {
                smooth[static_cast<std::size_t>(n)] = 1; /* if comparable: mark this pitch as verified */
                last_smooth = n;
            }
        }
        n++; /* Once more than a max-glitch-set has been scanned */
        /* or the end of the entire glitch is reached */
        if (++glcnt >= MIN_SMOOTH_SET || n >= gltchend)
        {
            if (last_smooth == previous)
                break; /* If no new verifiable pitch found, give up */
            previous = last_smooth;
            n = last_smooth + 1; /* Otherwise start a new local search from newly verified pitch */
            glcnt = 1;
        }
    }
}

// test_glitch_backwards.
void PitchFinder::test_glitch_backwards(
    int gltchstart, int gltchend, std::vector<char>& smooth, double max_pglide) const noexcept
{
    const float* pitches = pitches_.data();
    int n, glcnt, next, next_smooth;
    double post_interval;
    if ((next = gltchend) >= wlength_)
        return;
    if (pitches[next] < FLTERR)
        return;
    next_smooth = next;
    n = gltchend - 2; /* setup params for local search of glitch */
    glcnt = 1;
    while (n >= gltchstart)
    { /* look through the glitch */
        if (pitches[n] > FLTERR)
        { /* if glitch location holds a true pitch */
            post_interval = pitches[n] / pitches[next];
            if (post_interval < 1.0)
                post_interval = 1.0 / post_interval; /* compare against previous verified pitch */
            if (post_interval <= std::pow(max_pglide, (double)(next - n)))
            {
                smooth[static_cast<std::size_t>(n)] = 1; /* if comparable: mark this pitch as verified */
                next_smooth = n;
            }
        }
        n--; /* Once more than a max-glitch-set has been scanned */
        /* or the start of the entire glitch is reached */
        if (++glcnt >= MIN_SMOOTH_SET || n < gltchstart)
        {
            if (next_smooth == next)
                break; /* If no new verifiable pitch found, give up */
            next = next_smooth;
            n = next_smooth - 1;
            glcnt = 1; /* Otherwise start a new local search */
        }
    }
}

// mark_zeros_in_pitchdata: the whole sound's loudest window.
void PitchFinder::mark_zeros_in_pitchdata() noexcept
{
    double maxlevel = 0.0, minlevel;
    for (int n = 0; n < wlength_; n++)
    {
        if (pretotamp_[static_cast<std::size_t>(n)] > maxlevel)
            maxlevel = pretotamp_[static_cast<std::size_t>(n)];
    }
    minlevel = maxlevel * sratio_;
    for (int n = 0; n < wlength_; n++)
    {
        if (pretotamp_[static_cast<std::size_t>(n)] < minlevel)
            pitches_[static_cast<std::size_t>(n)] = (float)NOT_SOUND;
    }
}

// eliminate_blips_in_pitch_data: the continue goes on with the next k from
// the moved n, as CDP's does; the tail tests one window only.
void PitchFinder::eliminate_blips_in_pitch_data() noexcept
{
    float* pitches = pitches_.data();
    int n, m, k, wlength_less_bliplen;
    bool OK = true;
    if (valid_ <= 0)
        return;
    wlength_less_bliplen = wlength_ - valid_;
    for (n = 1; n < wlength_less_bliplen; n++)
    {
        if (pitches[n] > 0.0)
        {
            if (pitches[n - 1] < 0.0)
            {
                for (k = 1; k <= valid_; k++)
                {
                    if (pitches[n + k] < 0.0)
                    {
                        for (m = 0; m < k; m++)
                            pitches[n + m] = (float)NOT_PITCH;
                        n += k;
                        continue;
                    }
                }
            }
        }
    }
    n = wlength_less_bliplen;
    // With as many windows as the sound has, CDP reads pitches[-1] here
    // only if the first window were pitched, which it never is.
    if (n >= 1 && (pitches[n] > 0.0) && (pitches[n - 1] < 0.0))
    {
        /* UNREACHABLE at level4 ??? */
        for (k = 1; k < valid_; k++)
        {
            if (pitches[n + k] < 0.0)
                OK = false;
            break;
        }
    }
    if (!OK)
    {
        for (n = wlength_less_bliplen; n < wlength_; n++)
            pitches[n] = (float)NOT_PITCH;
    }
}

// interpolate_pitch(pitches, skip_silence = 1).
bool PitchFinder::interpolate_pitch() noexcept
{
    float* floatbuf = pitches_.data();
    int pitchno;
    for (pitchno = 0; pitchno < wlength_; pitchno++)
    {
        if (floatbuf[pitchno] < MINPITCH)
        {
            if (flteq((double)floatbuf[pitchno], NOT_SOUND))
                continue;
            if (!do_interpolating(&pitchno))
                return false;
        }
    }
    return true;
}

// do_interpolating (skip_silence): an unpitched run takes pitches
// interpolated in log frequency from the window BEFORE the last pitched
// window ahead of it, which it overwrites too; a run at the start takes
// the first pitch after it, one at the end the last before it. Silent
// windows keep their code.
bool PitchFinder::do_interpolating(int* pitchno) noexcept
{
    enum
    {
        MID_PITCH,
        FIRST_PITCH,
        END_PITCH,
        NO_PITCH
    };
    float* floatbuf = pitches_.data();
    int act_type = MID_PITCH;
    int start = *pitchno, m;
    double startpitch, endpitch, thispitch, lastpitch, pstep;
    if (*pitchno == 0)
        act_type = FIRST_PITCH;
    while (floatbuf[*pitchno] < MINPITCH)
    {
        if (++(*pitchno) >= wlength_)
        {
            if (act_type == FIRST_PITCH)
                act_type = NO_PITCH;
            else
                act_type = END_PITCH;
            break;
        }
    }
    if (act_type == MID_PITCH)
    {
        m = start - 1;
        while (floatbuf[m] < MINPITCH)
        {
            if (--m <= 0)
            {
                act_type = FIRST_PITCH;
                break;
            }
        }
        start = m;
    }
    switch (act_type)
    {
    case MID_PITCH:
        startpitch = std::log((double)floatbuf[start - 1]);
        endpitch = std::log((double)floatbuf[*pitchno]);
        pstep = (endpitch - startpitch) / (double)((*pitchno) - (start - 1));
        lastpitch = startpitch;
        for (m = start; m < *pitchno; m++)
        { /* INTERP PITCH ACROSS UNPITCHED SEG */
            thispitch = lastpitch + pstep;
            if (!flteq(floatbuf[m], NOT_SOUND))
                floatbuf[m] = (float)std::exp(thispitch);
            lastpitch = thispitch;
        }
        break;
    case FIRST_PITCH:
        for (m = 0; m < *pitchno; m++)
        { /* EXTEND FIRST CLEAR PITCH BACK TO START */
            if (!flteq(floatbuf[m], NOT_SOUND))
                floatbuf[m] = floatbuf[*pitchno];
        }
        break;
    case END_PITCH: /* EXTEND LAST CLEAR PITCH ON TO END */
        for (m = start; m < *pitchno; m++)
        {
            if (!flteq(floatbuf[m], NOT_SOUND))
                floatbuf[m] = floatbuf[start - 1];
        }
        break;
    case NO_PITCH:
        return false; // "No valid pitch found."
    }
    (*pitchno)--;
    return true;
}

// pitch_found.
bool PitchFinder::pitch_found() const noexcept
{
    for (int n = 0; n < wlength_; n++)
    {
        if (pitches_[static_cast<std::size_t>(n)] > NOT_PITCH)
            return true;
    }
    return false;
}

[[nodiscard]] bool validPitches(std::span<const float> pitches) noexcept
{
    return std::ranges::none_of(pitches, [](float pitch) {
        return std::isinf(pitch) || (pitch > 0.0F && pitch < MINPITCH);
    });
}
}

std::optional<std::size_t> analysisWindowCount(std::size_t frames, int points, int overlap) noexcept
{
    const auto plan = windowPlan(frames, points, overlap);
    if (!plan.has_value())
        return std::nullopt;
    return plan->windows;
}

std::optional<std::vector<float>> getPitch(std::span<const double> samples, double sampleRate, int points,
    int overlap, const GetPitchSettings& settings)
{
    if (samples.empty() || !std::ranges::all_of(samples, [](double sample) { return std::isfinite(sample); }))
        return std::nullopt;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return std::nullopt;
    const auto plan = windowPlan(samples.size(), points, overlap);
    if (!plan.has_value())
        return std::nullopt;
    const auto windows = plan->windows;
    // tklib1.c's ranges (getpitch's options are all fixed values), then
    // check_consistency_of_pitch_params.
    const double nyquist = sampleRate / 2.0;
    const double maxpitch = nyquist / MAXIMI;
    const double hilm = settings.highestPitch.value_or(maxpitch);
    const auto within = [](double value, double lo, double hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    if (!within(settings.tuningRange, 0.0, GetPitchSettings::maximumTuningRange)
        || settings.minimumPitchedWindows < 0 || static_cast<std::size_t>(settings.minimumPitchedWindows) > windows
        || !within(settings.signalToNoise, 0.0, GetPitchSettings::maximumSignalToNoise)
        || settings.harmonicsToConfirm < 1 || settings.harmonicsToConfirm > GetPitchSettings::maximumHarmonicsToConfirm
        || !within(settings.lowestPitch, SPEC_MINFRQ, maxpitch) || !within(hilm, SPEC_MINFRQ, maxpitch)
        || hilm <= settings.lowestPitch)
    {
        return std::nullopt;
    }
    // pvoc.c's arate and cdparse.c's frametime.
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    const auto frametime = (float)(1.0 / arate);
    PitchFinder finder(static_cast<int>(vocoder.frameSize()), nyquist, frametime, windows, settings);
    // pvoc anal: the samples CDP reads, then zeros, until every window the
    // .ana file holds has been analysed.
    std::size_t analysed = 0;
    const auto edit = [&](std::span<float> frame) {
        if (analysed < windows)
            finder.window(frame);
        ++analysed;
    };
    constexpr std::size_t chunk = 4096;
    std::vector<double> input(chunk, 0.0);
    std::vector<double> output(chunk, 0.0);
    std::size_t fed = 0;
    while (analysed < windows)
    {
        const std::size_t take = fed < plan->frames ? std::min(chunk, plan->frames - fed) : chunk;
        for (std::size_t index = 0; index < take; ++index)
            input[index] = fed + index < plan->frames ? samples[fed + index] : 0.0;
        if (!vocoder.process(std::span{input}.first(take), std::span{output}.first(take), edit))
            return std::nullopt;
        fed += take;
    }
    if (!finder.tidy())
        return std::nullopt;
    return std::move(finder).pitches();
}

bool analyseWholeSound(std::span<const double> samples, double sampleRate, int points, int overlap,
    const std::function<void(std::span<const float>)>& visit)
{
    if (samples.empty() || !std::ranges::all_of(samples, [](double sample) { return std::isfinite(sample); }))
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto plan = windowPlan(samples.size(), points, overlap);
    if (!plan.has_value())
        return false;
    // As getPitch: the samples CDP reads, then zeros, until every window the
    // .ana file holds has been analysed.
    const auto windows = plan->windows;
    std::size_t analysed = 0;
    const auto edit = [&](std::span<float> frame) {
        if (analysed < windows)
            visit(std::span<const float>{frame});
        ++analysed;
    };
    constexpr std::size_t chunk = 4096;
    std::vector<double> input(chunk, 0.0);
    std::vector<double> output(chunk, 0.0);
    std::size_t fed = 0;
    while (analysed < windows)
    {
        const std::size_t take = fed < plan->frames ? std::min(chunk, plan->frames - fed) : chunk;
        for (std::size_t index = 0; index < take; ++index)
            input[index] = fed + index < plan->frames ? samples[fed + index] : 0.0;
        if (!vocoder.process(std::span{input}.first(take), std::span{output}.first(take), edit))
            return false;
        fed += take;
    }
    return true;
}

bool PitchAltHarmonics::prepare(double sampleRate, int points, int overlap, std::span<const float> pitches,
    Mode mode, bool lessBody)
{
    if (mode < Mode::DeleteOdd || mode > Mode::DeleteEven || !validPitches(pitches))
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    pitches_.assign(pitches.begin(), pitches.end());
    mode_ = mode;
    lessBody_ = lessBody;
    nyquist_ = sampleRate / 2.0;
    reset();
    return true;
}

void PitchAltHarmonics::reset() noexcept
{
    vocoder_.reset();
    frames_ = 0;
}

bool PitchAltHarmonics::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // CDP's pitchcnt: frame k reads pitches[k]; past the array, no pitch.
        const auto k = frames_++;
        const double pitch = k < pitches_.size() ? (double)pitches_[k] : NOT_PITCH;
        altFrame(frame, pitch);
    });
}

// specalt, after specbare.
void PitchAltHarmonics::altFrame(std::span<float> frame, double pitch) const noexcept
{
    float* flbufptr = frame.data();
    const int wanted = static_cast<int>(frame.size());
    // specbare.
    {
        const double error_range = SPAN_FACTOR * SEMITONE_INTERVAL; /* +or- error in finding pitch of partial */
        double thisfrq, frq_close_above_next_partial, frq_close_below_next_partial;
        int n = 0;
        int vc = 0;
        const double one_over_error_range = 1.0 / error_range;
        double upper_freqdiff, lower_freqdiff;
        if (pitch < 0.0) /*   UNPITCHED WINDOW : marked in specpich */
            return;
        // CDP8's build fuses both: pitch * error_range - pitch, and pitch
        // plus pitch times minus one over the error range.
        upper_freqdiff = std::fma(pitch, error_range, -pitch);
        lower_freqdiff = std::fma(pitch, -one_over_error_range, pitch);
        while (vc < wanted && (thisfrq = pitch * (double)++n) < nyquist_)
        {
            frq_close_above_next_partial = thisfrq + upper_freqdiff;
            frq_close_below_next_partial = thisfrq - lower_freqdiff;
            // zero_channels_between_partials.
            while (flbufptr[vc + 1] < frq_close_below_next_partial)
            {
                flbufptr[vc] = 0.0f;
                if ((vc += 2) >= wanted)
                    break;
            }
            if (vc >= wanted)
                break;
            if (flbufptr[vc + 1] > frq_close_above_next_partial)
                continue;
            else if (lessBody_)
            {
                // eliminate_all_but_loudest_channel_near_this_partial_frq.
                double maxamp = flbufptr[vc]; /* SET MAXAMP TO 1ST IN-RANGE CH */
                int k, maxloc = vc; /* NOTE NO. OF THE CH THUS MARKD */
                int firstloc = vc; /* MARK FIRST CH IN SET          */
                while ((vc += 2) < wanted)
                {
                    if (flbufptr[vc + 1] > frq_close_above_next_partial) /* IF BEYOND CURRENT RANGE, STOP */
                        break;
                    if (flbufptr[vc] > maxamp)
                    { /* IF LOUDER THAN MAX,RESET MAX  */
                        maxamp = flbufptr[vc]; /* AND RESET MAXAMP CHANNEL NO.  */
                        maxloc = vc;
                    }
                }
                for (k = firstloc; k < vc; k += 2)
                { /* FOR THE WHOLE SET THUS DONE  */
                    if (k != maxloc) /* ZERO ALL CHS OTHER THAN MAX  */
                        flbufptr[k] = 0.0f;
                }
            }
            else
            {
                // keep_all_channels_near_partial_frq.
                while ((vc += 2) < wanted)
                {
                    if (flbufptr[vc + 1] > frq_close_above_next_partial) /* IF BEYOND CURRENT RANGE, STOP */
                        break;
                }
            }
        }
    }
    int n = 1;
    if (pitch < 0.0) /*   UNPITCHED WINDOW : IGNORE */
        return;
    for (int vc = 0; vc < wanted; vc += 2)
    {
        if (flbufptr[vc] > 0.0)
        {
            const bool odd = (n & 1) != 0;
            if (mode_ == Mode::DeleteOdd ? odd : !odd)
                flbufptr[vc] = 0.0F;
            n++;
        }
    }
}

bool PitchOctaveMove::prepare(double sampleRate, int points, int overlap, std::span<const float> pitches,
    Mode mode, int transposition, double bassBoost)
{
    if (mode < Mode::Up || mode > Mode::DownBassBoost || transposition < 2 || transposition > maximumTransposition
        || !std::isfinite(bassBoost) || bassBoost < 0.0 || bassBoost > maximumBassBoost || !validPitches(pitches))
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    pitches_.assign(pitches.begin(), pitches.end());
    mode_ = mode;
    transposition_ = transposition;
    startBassBoost_ = bassBoost;
    // set_chunklens_and_establish_windowbufs.
    const int clength = static_cast<int>(vocoder_.frameSize() / 2);
    nyquist_ = sampleRate / 2.0;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
    // initialise_specenv's arrays of clength + 1, and one more for the
    // Nyquist point CDP writes past them when every channel is a distinct
    // harmonic.
    specenvfrq_.assign(static_cast<std::size_t>(clength) + 2, 0.0F);
    specenvpch_.assign(specenvfrq_.size(), 0.0F);
    specenvamp_.assign(specenvfrq_.size(), 0.0F);
    reset();
    return true;
}

bool PitchOctaveMove::setBassBoost(double bassBoost) noexcept
{
    if (!prepared() || !std::isfinite(bassBoost) || bassBoost < 0.0 || bassBoost > maximumBassBoost)
        return false;
    bassBoost_ = bassBoost;
    return true;
}

void PitchOctaveMove::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(specenvfrq_, 0.0F);
    std::ranges::fill(specenvpch_, 0.0F);
    std::ranges::fill(specenvamp_, 0.0F);
    bassBoost_ = startBassBoost_;
    frames_ = 0;
}

bool PitchOctaveMove::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        const auto k = frames_++;
        // skip_or_special_operation_on_window_zero.
        if (k == 0)
            return;
        const double pitch = k < pitches_.size() ? (double)pitches_[k] : NOT_PITCH;
        octFrame(frame, pitch);
    });
}

// specoct.
void PitchOctaveMove::octFrame(std::span<float> frame, double pitch) noexcept
{
    float* flbufptr = frame.data();
    const int wanted = static_cast<int>(frame.size());
    double pre_totalamp, post_totalamp, thisfrq, basefrq, thisamp;
    int n, vc, truecc, truevc;
    // A NaN pitch leaves the frame as CDP leaves it (every channel a
    // harmonic going up; no harmonic under the Nyquist frequency going
    // down).
    if (!((thisfrq = pitch) > 0.0))
        return;
    pre_totalamp = get_totalamp(flbufptr, wanted);
    if (mode_ == Mode::Up)
    {
        /* UPWARDS: DELETING ALTERNATE HARMONICS */
        thisfrq *= (double)transposition_;
        for (vc = 0; vc < wanted; vc += 2)
        {
            if (!is_harmonic(&n, (double)flbufptr[vc + 1], thisfrq))
                flbufptr[vc] = 0.0f;
        }
    }
    else
    {
        /* DNWARDS: INSERT INTERVENING HARMONICS */
        extractSpecenvOverPartials(frame, thisfrq);
        basefrq = thisfrq / (double)transposition_;
        // getspecenvamp(&baseamp, basefrq, 0, dz), whose value specoct
        // never reads.
        thisfrq = basefrq;
        n = 1;
        while (thisfrq < nyquist_)
        {
            // get_channel_corresponding_to_frq.
            truecc = (int)((std::fabs(thisfrq) + halfchwidth_) / chwidth_); /* TRUNCATE */
            truevc = truecc * 2;
            if (mode_ == Mode::DownBassBoost && (thisfrq < basefrq))
            {
                // CDP8's build fuses the multiply and subtract.
                flbufptr[truevc] = (float)std::fma(
                    -(bassBoost_ - 1.0), ((double)n / (double)transposition_), bassBoost_);
            }
            else
            {
                thisamp = getspecenvamp(thisfrq);
                flbufptr[truevc] = (float)thisamp;
            }
            truevc++;
            flbufptr[truevc] = (float)thisfrq;
            do
            {
                n++;
            } while (n % transposition_ == 0);
            thisfrq = basefrq * (double)n;
        }
    }
    /* NORMALISE */
    post_totalamp = get_totalamp(flbufptr, wanted);
    normalise(flbufptr, wanted, pre_totalamp, post_totalamp);
}

// extract_specenv_over_partials: the loudest channel near each harmonic.
void PitchOctaveMove::extractSpecenvOverPartials(std::span<const float> frame, double thispitch) noexcept
{
    const float* flbufptr = frame.data();
    const int clength = static_cast<int>(frame.size() / 2);
    float* specenvamp = specenvamp_.data();
    float* specenvfrq = specenvfrq_.data();
    float* specenvpch = specenvpch_.data();
    const int last = static_cast<int>(specenvamp_.size()) - 1;
    int n = 0, k, cc, vc, lastk = 0;
    for (n = 0; n <= clength; n++)
        specenvamp[n] = 0.0F;
    n = 0;
    specenvfrq[n] = 1.0F;
    specenvpch[n++] = 0.0F;
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        if (!is_harmonic(&k, flbufptr[vc + 1], thispitch))
            continue;
        if (k == lastk)
        {
            if (flbufptr[vc] > specenvamp[n])
            {
                specenvamp[n] = flbufptr[vc]; /* GET LOUDEST PARTIAL */
                // sfsys.h's max.
                specenvfrq[n] = (1.0f > flbufptr[vc + 1]) ? 1.0f : flbufptr[vc + 1]; /*    NEAR HARMONIC    */
                specenvpch[n] = (float)std::log10(specenvfrq[n]);
            }
        }
        else
        {
            n = std::min(n + 1, last - 1);
            specenvamp[n] = flbufptr[vc]; /* KEEP AMP & FRQ OF PARTIAL */
            specenvfrq[n] = (1.0f > flbufptr[vc + 1]) ? 1.0f : flbufptr[vc + 1];
            specenvpch[n] = (float)std::log10(specenvfrq[n]);
            lastk = k;
        }
    }
    n++;
    specenvamp[n] = 0.0F;
    specenvfrq[n] = (float)nyquist_;
    specenvpch[n] = (float)std::log10(specenvfrq[n]);
}

// getspecenvamp, from store 0. octmove's specenvcnt (an analysis file's,
// none) never stops the search, which may go on into points of earlier
// frames, as far as the arrays reach.
double PitchOctaveMove::getspecenvamp(double thisfrq) const noexcept
{
    double pp, ratio, ampdiff, thisamp;
    int z = 1;
    const float* specenvpch = specenvpch_.data();
    const float* ampstore = specenvamp_.data();
    const int last = static_cast<int>(specenvpch_.size()) - 1;
    if (thisfrq < 0.0) /* NOT SURE THIS IS CORRECT */
        return 0.0;
    if (thisfrq <= 1.0)
        pp = 0.0;
    else
        pp = std::log10(thisfrq);
    while (specenvpch[z] < pp)
    {
        if (z == last)
            break;
        z++;
    }
    ratio = (pp - specenvpch[z - 1]) / (specenvpch[z] - specenvpch[z - 1]);
    ampdiff = ampstore[z] - ampstore[z - 1];
    // CDP8's build fuses the multiply and add.
    thisamp = std::fma(ampdiff, ratio, (double)ampstore[z - 1]);
    thisamp = (0.0 > thisamp) ? 0.0 : thisamp; /* max(0.0,*thisamp) */
    return thisamp;
}
}
