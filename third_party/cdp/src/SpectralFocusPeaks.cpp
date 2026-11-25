/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, dev/cdp2k/formantsg.c, readdata.c, tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralFocusPeaks.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double FLTERR = 0.000002;
constexpr double PI = 3.141592654;
constexpr double ONE_OVER_LN2 = 1.442695;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
// construct_filter_envelope's channels tested beyond a band's top.
constexpr int OVERSHOOT = 8;

// flteq (tklib1.c).
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// read_param_as_value_or_brkfile_and_check_range (readdata.c), as
// get_brkpnt_data_from_file_and_test_it checks breakpoints too: a value
// within FLTERR of a limit is that limit; nothing outside them.
[[nodiscard]] std::optional<double> inRange(double value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return std::nullopt;
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    if (value > hi || value < lo)
        return std::nullopt;
    return value;
}

// setup_pitchzero (tklib3.c): a breakpoint frequency under SPEC_MINFRQ is
// raised to it, a fixed one is zero.
[[nodiscard]] double pitchzero(double frequency, bool live) noexcept
{
    if (frequency < SPEC_MINFRQ)
        return live ? SPEC_MINFRQ : 0.0;
    return frequency;
}

// rectify_window (tklib3.c), as SpectralGlis.cpp copies it.
void rectify_window(std::span<float> frame) noexcept
{
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
    {
        if (frame[vc + 1] < 0.0)
            frame[vc + 1] = -frame[vc + 1];
    }
}

// gen_amplitude_in_lo_half_filterband and gen_amplitude_in_hi_half_filterband
// (tklib3.c): a raised cosine over the octaves from the centre.
[[nodiscard]] double gen_amplitude_in_lo_half_filterband(
    double thisfrq, double filt_centre_frq, double scalefact) noexcept
{
    double val;
    val = -(std::log(thisfrq / filt_centre_frq) * ONE_OVER_LN2); /* -LOG2 */
    val *= scalefact;
    val = std::cos(val);
    val += 1.0;
    val /= 2.0;
    return val;
}

[[nodiscard]] double gen_amplitude_in_hi_half_filterband(
    double thisfrq, double filt_centre_frq, double scalefact) noexcept
{
    double val;
    val = std::log(thisfrq / filt_centre_frq) * ONE_OVER_LN2; /* LOG2 */
    val *= scalefact;
    val = std::cos(val);
    val += 1.0;
    val /= 2.0;
    return val;
}

// The channel count CDP's frames of those points have; nothing for what the
// phase vocoder refuses.
[[nodiscard]] std::optional<int> channelsFor(int points, int overlap) noexcept
{
    if (!PhaseVocoder::decimationFor(points, overlap).has_value())
        return std::nullopt;
    return (points + (points % 2)) / 2 + 1;
}
}

std::optional<int> FocusFocus::stabilityWindows(bool stable, double stability) noexcept
{
    if (!stable)
        return 0;
    const auto value = inRange(stability, minimumStability, maximumStability);
    if (!value.has_value())
        return std::nullopt;
    // get_variants_and_flags: -s only for a value other than its default.
    if (flteq(*value, defaultStability))
        return 0;
    return static_cast<int>(std::round(*value));
}

std::optional<std::size_t> FocusFocus::latencyFor(int points, int overlap, bool stable, double stability) noexcept
{
    const auto latency = PhaseVocoder::latencyFor(points, overlap);
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    const auto windows = stabilityWindows(stable, stability);
    if (!latency.has_value() || !d.has_value() || !windows.has_value())
        return std::nullopt;
    if (*windows == 0)
        return *latency;
    return *latency + static_cast<std::size_t>(*windows - 2) * static_cast<std::size_t>(*d);
}

double FocusFocus::maximumBandwidth(double sampleRate, int points, int overlap) noexcept
{
    const auto clength = channelsFor(points, overlap);
    if (!clength.has_value() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 0.0;
    // set_param_ranges (tklib1.c).
    const double nyquist = sampleRate / 2.0;
    const double chwidth = nyquist / (double)(*clength - 1);
    const double halfchwidth = chwidth / 2.0;
    return std::log(nyquist / halfchwidth) * ONE_OVER_LN2; /* LOG2 */
}

bool FocusFocus::prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands, bool quickSearch,
    int peaks, double bandwidth, double bottomFrequency, double topFrequency, bool liveBottom, bool liveTop,
    bool stable, double stability)
{
    const auto windows = stabilityWindows(stable, stability);
    if (!windows.has_value() || peaks < 1 || peaks > maximumPeaks)
        return false;
    FocusFocus focus;
    if (!focus.analysis_.prepare(sampleRate, points, overlap))
        return false;
    if (!focus.envelope_.prepare(sampleRate, focus.analysis_.frameSize(), pitchwise, bands, quickSearch))
        return false;
    focus.maximumBandwidth_ = maximumBandwidth(sampleRate, points, overlap);
    focus.nyquist_ = sampleRate / 2.0;
    const auto bw = inRange(bandwidth, minimumBandwidth, focus.maximumBandwidth_);
    const auto lo = inRange(bottomFrequency, minimumFrequency, focus.nyquist_);
    const auto hi = inRange(topFrequency, minimumFrequency, focus.nyquist_);
    if (!bw.has_value() || !lo.has_value() || !hi.has_value()
        || flteq(pitchzero(*lo, liveBottom), pitchzero(*hi, liveTop)))
    {
        return false;
    }
    const auto frameSize = focus.analysis_.frameSize();
    focus.clength_ = static_cast<int>(frameSize / 2);
    focus.specenvcnt_ = static_cast<int>(focus.envelope_.amplitudes().size());
    focus.itemcnt_ = peaks;
    focus.startBandwidth_ = *bw;
    focus.startLow_ = *lo;
    focus.startHigh_ = *hi;
    focus.liveLow_ = liveBottom;
    focus.liveHigh_ = liveTop;
    // establish_internal_arrays_for_focus.
    const auto items = static_cast<std::size_t>(peaks) + 1;
    focus.filtpeak_.assign(items, 0.0);
    focus.fbandtop_.assign(items, 0.0);
    focus.fbandbot_.assign(items, 0.0);
    focus.peakno_.assign(items, 0);
    focus.fsampbuf_.assign(static_cast<std::size_t>(focus.clength_) + 1, 0.0F);
    // setup_stability_arrays_and_constants.
    focus.stabl_ = *windows;
    if (focus.stabl_ != 0)
    {
        if (!focus.synthesis_.prepare(sampleRate, points, overlap))
            return false;
        const auto s = static_cast<std::size_t>(focus.stabl_);
        const auto specenvcnt = static_cast<std::size_t>(focus.specenvcnt_);
        const auto d = static_cast<std::size_t>(focus.analysis_.decimation());
        focus.sl1_ = focus.stabl_ - 1;
        focus.offset_ = focus.stabl_ / 2;
        focus.spec_.assign(s * specenvcnt, 0.0F);
        focus.fpk_.assign(s * specenvcnt, 0);
        focus.totalPkcnt_.assign(s, 0);
        focus.sbuf_.assign(s * frameSize, 0.0F);
        focus.designScore_.assign(specenvcnt + 1, 0);
        focus.des_.assign(specenvcnt + 1, Design{});
        focus.delay_ = static_cast<std::size_t>(focus.stabl_ - 2) * d;
        focus.discard_.assign(d, 0.0);
        focus.silence_.assign(d, 0.0);
    }
    *this = std::move(focus);
    reset();
    return true;
}

bool FocusFocus::setBandwidth(double bandwidth) noexcept
{
    if (!prepared())
        return false;
    const auto bw = inRange(bandwidth, minimumBandwidth, maximumBandwidth_);
    if (!bw.has_value())
        return false;
    bandwidth_ = *bw;
    generateFocuFilterParams();
    return true;
}

bool FocusFocus::setFrequencies(double bottomFrequency, double topFrequency) noexcept
{
    if (!prepared())
        return false;
    const auto lo = inRange(bottomFrequency, minimumFrequency, nyquist_);
    const auto hi = inRange(topFrequency, minimumFrequency, nyquist_);
    if (!lo.has_value() || !hi.has_value())
        return false;
    const auto low = pitchzero(*lo, liveLow_);
    const auto high = pitchzero(*hi, liveHigh_);
    // specfocus (or check_consistency_of_frq_limits_for_focus for fixed
    // ones) stops at a zero-width band.
    if (flteq(low, high))
        return false;
    low_ = low;
    high_ = high;
    return true;
}

void FocusFocus::reset() noexcept
{
    analysis_.reset();
    synthesis_.reset();
    bandwidth_ = startBandwidth_;
    low_ = pitchzero(startLow_, liveLow_);
    high_ = pitchzero(startHigh_, liveHigh_);
    generateFocuFilterParams();
    std::ranges::fill(filtpeak_, 0.0);
    // CDP leaves the band edges unset until a frame finds a peak (a frame
    // with none builds its filter from the first band's last edges).
    std::ranges::fill(fbandtop_, 0.0);
    std::ranges::fill(fbandbot_, 0.0);
    std::ranges::fill(peakno_, 0);
    std::ranges::fill(fsampbuf_, 0.0F);
    least_ = 0;
    peakscore_ = 0;
    totalWindows_ = 0;
    std::ranges::fill(spec_, 0.0F);
    std::ranges::fill(fpk_, 0);
    std::ranges::fill(totalPkcnt_, 0);
    std::ranges::fill(sbuf_, 0.0F);
    std::ranges::fill(designScore_, 0);
    std::ranges::fill(des_, Design{});
    waited_ = 0;
    synthesised_ = 0;
}

std::size_t FocusFocus::latencyFrames() const noexcept
{
    if (stabl_ == 0)
        return analysis_.latencyFrames();
    return synthesis_.latencyFrames() + delay_;
}

bool FocusFocus::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
    {
        return false;
    }
    if (stabl_ == 0)
        return analysis_.process(input, output, [this](std::span<float> frame) { analysed(frame); });
    // As BlurAndTrace::process: at most one frame spacing at a time, so that
    // each piece completes at most one analysis frame, which is stored before
    // the synthesis vocoder takes the frame (stability - 2) windows earlier
    // at the same input frame.
    const auto spacing = discard_.size();
    const auto frameSize = analysis_.frameSize();
    for (std::size_t offset = 0; offset < input.size();)
    {
        const auto count = std::min(spacing, input.size() - offset);
        (void)analysis_.process(input.subspan(offset, count), std::span<double>{discard_.data(), count},
            [this](std::span<float> frame) { analysed(frame); });
        const auto waiting = std::min(count, delay_ - waited_);
        std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), waiting, 0.0);
        waited_ += waiting;
        if (count > waiting)
        {
            (void)synthesis_.process(std::span<const double>{silence_.data(), count - waiting},
                output.subspan(offset + waiting, count - waiting), [this, frameSize](std::span<float> frame) {
                    // CDP's first frame is its stability buffer's first
                    // place, never filled; each later one the window before
                    // it, filtered.
                    const auto window = synthesised_++;
                    if (window == 0)
                    {
                        std::ranges::fill(frame, 0.0F);
                        return;
                    }
                    const float* written = sbuf(window - 1);
                    std::copy(written, written + frameSize, frame.begin());
                });
        }
        offset += count;
    }
    return true;
}

std::size_t FocusFocus::slot(std::size_t window) const noexcept
{
    return window % static_cast<std::size_t>(stabl_);
}

float* FocusFocus::sbuf(std::size_t window) noexcept
{
    return sbuf_.data() + slot(window) * analysis_.frameSize();
}

// generate_focu_filter_params, as specfocus repeats it for a breakpoint
// bandwidth. CDP8's build calls exp2 for pow(2.0,...).
void FocusFocus::generateFocuFilterParams() noexcept
{
    const double halfbwidth = bandwidth_ / 2.0;
    scalefact_ = PI / halfbwidth;
    bratioUp_ = std::exp2(halfbwidth);
    bratioDn_ = 1.0 / bratioUp_;
}

// inner_loop, at each analysis window.
void FocusFocus::analysed(std::span<float> frame) noexcept
{
    if (totalWindows_ == 0)
    {
        // skip_or_special_operation_on_window_zero: CDP leaves the first
        // window as it is; with stability it stores its envelope and the
        // window, with no peaks.
        if (stabl_ != 0)
        {
            rectify_window(frame);
            envelope_.extract(frame);
            const auto here = slot(0);
            const auto amps = envelope_.amplitudes();
            std::ranges::copy(amps, spec_.begin() + static_cast<std::ptrdiff_t>(here * amps.size()));
            std::ranges::copy(frame, sbuf(0));
            totalPkcnt_[here] = 0;
        }
        totalWindows_++;
        return;
    }
    specfocus(frame);
    totalWindows_++;
}

// specfocus.
void FocusFocus::specfocus(std::span<float> frame) noexcept
{
    double lofrq_limit = low_;
    double hifrq_limit = high_;
    if (lofrq_limit > hifrq_limit)
        std::swap(lofrq_limit, hifrq_limit);
    rectify_window(frame);
    envelope_.extract(frame);
    if (stabl_ != 0)
    {
        doStablePeaksFilter(frame, lofrq_limit, hifrq_limit);
        return;
    }
    int pkcnt_here = 0;
    extractFormantPeaks(pkcnt_here, lofrq_limit, hifrq_limit);
    establishBottomAndTopOfFilterBands(pkcnt_here);
    constructFilterEnvelope(pkcnt_here, frame.data());
    doFocuFilter(frame.data());
}

// do_stable_peaks_filter. CDP shuffles its stores back a place at each
// window; the rings hold window w at w modulo the stability, so that at
// window w CDP's spec[k], fpk[k] and total_pkcnt[k] are window
// w - (stability - 1) + k, and its sbuf[k] window w - stability + k (the
// window stored at the end).
void FocusFocus::doStablePeaksFilter(std::span<const float> frame, double lofrq_limit, double hifrq_limit) noexcept
{
    const auto w = totalWindows_;
    const auto here = slot(w);
    const auto specenvcnt = static_cast<std::size_t>(specenvcnt_);
    const auto amps = envelope_.amplitudes();
    std::ranges::copy(amps, spec_.begin() + static_cast<std::ptrdiff_t>(here * specenvcnt));
    totalPkcnt_[here] = extractFormantPeaks2(fpk_.data() + here * specenvcnt, lofrq_limit, hifrq_limit);
    const auto stabl = static_cast<std::size_t>(stabl_);
    const auto sl1 = static_cast<std::size_t>(sl1_);
    const auto offset = static_cast<std::size_t>(offset_);
    if (w >= sl1)
    {
        // Before this CDP cannot design a filter. Its sbuf[offset] is window
        // w - stability + offset.
        float* middle = sbuf(w + offset - stabl);
        designFilter(middle);
        if (w == sl1)
        {
            // The first filter: CDP writes sbuf[0] (window -1, never
            // filled, which synthesis plays as silence), then filters
            // sbuf[1] to sbuf[offset], windows 0 to offset - 1.
            for (std::size_t n = 1; n <= offset; n++)
                doFocuFilter(sbuf(n - 1));
        }
        else
            doFocuFilter(middle);
    }
    std::ranges::copy(frame, sbuf(w));
}

// extract_formant_peaks, with outer_focu_loop's least kept from window to
// window.
void FocusFocus::extractFormantPeaks(int& pkcnt_here, double lofrq_limit, double hifrq_limit) noexcept
{
    const float* specenvfrq = envelope_.frequencies().data();
    const float* specenvamp = envelope_.amplitudes().data();
    int n = 0;
    int falling = 0, last_channel_was_in_range = 1;
    pkcnt_here = 0;
    for (n = 0; n < itemcnt_; n++)
        filtpeak_[static_cast<std::size_t>(n)] = 0.0;
    n = 1;
    while (n < specenvcnt_)
    {
        if (specenvfrq[n] < lofrq_limit)
        {
            last_channel_was_in_range = 0;
            n++;
            continue;
        }
        if (specenvamp[n] > specenvamp[n - 1])
            falling = 0;
        else
        {
            if (!falling)
            {
                if (last_channel_was_in_range)
                    insertNewPeakInPeaklist(n - 1);
                pkcnt_here++;
                falling = 1;
            }
        }
        last_channel_was_in_range = 1;
        if (specenvfrq[n] > hifrq_limit)
            break;
        n++;
    }
    pkcnt_here = std::min(pkcnt_here, itemcnt_);
    sortFocuPeaksByFrq(pkcnt_here);
}

// extract_formant_peaks2 (formantsg.c): every peak, into fppk; their count.
int FocusFocus::extractFormantPeaks2(int* fppk, double lofrq_limit, double hifrq_limit) const noexcept
{
    const float* specenvfrq = envelope_.frequencies().data();
    const float* specenvamp = envelope_.amplitudes().data();
    int thispkcnt = 0;
    int n;
    int falling = 0, last_channel_was_in_range = 1;
    n = 1;
    while (n < specenvcnt_)
    {
        if (specenvfrq[n] < lofrq_limit)
        {
            last_channel_was_in_range = 0;
            n++;
            continue;
        }
        if (specenvamp[n] > specenvamp[n - 1])
            falling = 0;
        else
        {
            if (!falling)
            {
                if (last_channel_was_in_range)
                    fppk[thispkcnt++] = n - 1;
                falling = 1;
            }
        }
        last_channel_was_in_range = 1;
        if (specenvfrq[n] > hifrq_limit)
            break;
        n++;
    }
    return thispkcnt;
}

// insert_new_peak_in_peaklist.
void FocusFocus::insertNewPeakInPeaklist(int position_in_formant) noexcept
{
    const float* specenvamp = envelope_.amplitudes().data();
    double* filtpeak = filtpeak_.data();
    int n;
    double smallest = DBL_MAX;
    if (specenvamp[position_in_formant] > filtpeak[least_])
    {
        filtpeak[least_] = specenvamp[position_in_formant];
        peakno_[static_cast<std::size_t>(least_)] = position_in_formant;
    }
    for (n = 0; n < itemcnt_; n++)
    {
        if (filtpeak[n] < smallest)
        {
            least_ = n;
            smallest = filtpeak[n];
        }
    }
}

// sort_focu_peaks_by_frq.
void FocusFocus::sortFocuPeaksByFrq(int pkcnt_here) noexcept
{
    const float* specenvfrq = envelope_.frequencies().data();
    int* peakno = peakno_.data();
    int n, m, k;
    for (n = 0; n < pkcnt_here - 1; n++)
    {
        for (m = n + 1; m < pkcnt_here; m++)
        {
            if (specenvfrq[peakno[n]] > specenvfrq[peakno[m]])
            {
                k = peakno[m];
                peakno[m] = peakno[n];
                peakno[n] = k;
            }
        }
    }
}

// establish_bottom_and_top_of_filter_bands.
void FocusFocus::establishBottomAndTopOfFilterBands(int pkcnt_here) noexcept
{
    const float* specenvfrq = envelope_.frequencies().data();
    for (int n = 0; n < pkcnt_here; n++)
    {
        const auto i = static_cast<std::size_t>(n);
        fbandbot_[i] = specenvfrq[peakno_[i]] * bratioDn_;
        const double top = specenvfrq[peakno_[i]] * bratioUp_;
        fbandtop_[i] = (top > nyquist_) ? nyquist_ : top; /* min(...,dz->nyquist) */
    }
}

// construct_filter_envelope (ap_focus.c), with filter_band_test's checks
// left out (its contour stays within 0 to 1). Each band from the lowest
// peak's: channels in its lower half rise and in its upper half fall
// over a raised cosine, each channel keeping the most any band gives it;
// past the band's top (or at the last channel) the scan goes back to the
// first channel that reached the next band.
void FocusFocus::constructFilterEnvelope(int pkcnt_here, const float* fbuf) noexcept
{
    const float* specenvfrq = envelope_.frequencies().data();
    float* fsampbuf = fsampbuf_.data();
    const double* fbandbot = fbandbot_.data();
    const double* fbandtop = fbandtop_.data();
    double thisfrq, thisamp, filt_centre_frq;
    int n, m;
    int baktrak = -1, activate_baktrak = 0, checklogic = 0;
    int beyond_top, band_incremented;
    int cc, vc;
    for (cc = 0; cc < clength_; cc++) /* PRESET FILTER ENVELOPE TO ZERO */
        fsampbuf[cc] = 0.0f;
    n = 0; /* COUNTER FOR CURRENT PEAK */
    m = 1; /* COUNTER FOR NEXT PEAK */
    cc = 0;
    vc = 0;
    beyond_top = 0;
    while (cc < clength_)
    {
        filt_centre_frq = specenvfrq[peakno_[static_cast<std::size_t>(n)]];
        band_incremented = 0;
        thisfrq = std::fabs(fbuf[vc + 1]);
        if (baktrak < 0 && (m < pkcnt_here) && (thisfrq > fbandbot[m]))
            baktrak = cc;
        if (thisfrq < fbandbot[n])
        {
            if (beyond_top)
                beyond_top++;
        }
        else if (thisfrq < filt_centre_frq)
        {
            thisamp = gen_amplitude_in_lo_half_filterband(thisfrq, filt_centre_frq, scalefact_);
            fsampbuf[cc] = (float)((fsampbuf[cc] > thisamp) ? fsampbuf[cc] : thisamp); /* max */
            if (beyond_top)
                beyond_top++;
        }
        else if (thisfrq < fbandtop[n])
        {
            thisamp = gen_amplitude_in_hi_half_filterband(thisfrq, filt_centre_frq, scalefact_);
            fsampbuf[cc] = (float)((fsampbuf[cc] > thisamp) ? fsampbuf[cc] : thisamp); /* max */
            if (beyond_top)
                beyond_top++;
        }
        else
        {
            if (!beyond_top)
            {
                if (baktrak < 0)
                    baktrak = cc;
            }
            if (++beyond_top > OVERSHOOT)
            {
                beyond_top = 0;
                if (++n >= pkcnt_here)
                    break;
                m++;
                band_incremented = 1;
                activate_baktrak = 1;
            }
        }
        if (cc == clength_ - 1)
        {
            checklogic = 1;
            if (!band_incremented)
            {
                beyond_top = 0;
                if (++n >= pkcnt_here)
                    break;
                m++;
                activate_baktrak = 1;
            }
        }
        if (activate_baktrak && baktrak >= 0)
        {
            if (checklogic)
                checklogic++;
            cc = baktrak;
            vc = cc * 2;
            baktrak = -1;
            activate_baktrak = 0;
            continue;
        }
        // CDP stops with "Problem with peak-scanning logic" here; a stream
        // keeps the contour built so far.
        if (checklogic == 1)
            break;
        checklogic = 0;
        cc++;
        vc += 2;
    }
}

// do_focu_filter.
void FocusFocus::doFocuFilter(float* thisbuf) const noexcept
{
    int cc, vc;
    for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        thisbuf[vc] = (float)(thisbuf[vc] * fsampbuf_[static_cast<std::size_t>(cc)]);
}

// design_filter, with score_peaks, collect_scores, sort_design and
// unscore_peaks (tklib3.c): the peaks found most often over the stored
// windows, the filter built on the middle window's frequencies.
void FocusFocus::designFilter(const float* middle) noexcept
{
    const auto w = totalWindows_;
    const auto sl1 = static_cast<std::size_t>(sl1_);
    const auto specenvcnt = static_cast<std::size_t>(specenvcnt_);
    int this_pkcnt;
    int n = 0;
    int pkcnt_here;
    if (w <= sl1)
        std::fill_n(designScore_.begin(), specenvcnt, 0);
    // score_peaks.
    if (w > sl1)
    {
        /* Add in the peaks in the last buf calcd */
        const auto here = slot(w);
        const int* fppk = fpk_.data() + here * specenvcnt;
        for (n = 0; n < totalPkcnt_[here]; n++)
        {
            designScore_[static_cast<std::size_t>(fppk[n])]++;
            peakscore_++;
        }
    }
    else
    {
        for (int k = 0; k < stabl_; k++)
        {
            const auto store = slot(w - sl1 + static_cast<std::size_t>(k));
            const int* fppk = fpk_.data() + store * specenvcnt;
            for (n = 0; n < totalPkcnt_[store]; n++)
            {
                designScore_[static_cast<std::size_t>(fppk[n])]++;
                peakscore_++;
            }
        }
    }
    if (peakscore_ == 0)
    {
        /* zero filter: CDP's memset of wanted floats runs past fsampbuf */
        std::ranges::fill(fsampbuf_, 0.0F);
        return;
    }
    // collect_scores.
    this_pkcnt = 0;
    for (n = 0; n < specenvcnt_; n++)
    {
        const auto score = designScore_[static_cast<std::size_t>(n)];
        if (score)
        {
            auto& des = des_[static_cast<std::size_t>(this_pkcnt)];
            des.chan = n;
            des.score = score;
            des.amp = 0.0;
            this_pkcnt++;
        }
    }
    // sort_design: descending order of abundance.
    for (n = 1; n < this_pkcnt; n++)
    {
        const int a = des_[static_cast<std::size_t>(n)].chan;
        const int b = des_[static_cast<std::size_t>(n)].score;
        int m = n - 1;
        while (m >= 0 && des_[static_cast<std::size_t>(m)].score < b)
        {
            des_[static_cast<std::size_t>(m) + 1].chan = des_[static_cast<std::size_t>(m)].chan;
            des_[static_cast<std::size_t>(m) + 1].score = des_[static_cast<std::size_t>(m)].score;
            m--;
        }
        des_[static_cast<std::size_t>(m + 1)].chan = a;
        des_[static_cast<std::size_t>(m + 1)].score = b;
    }
    const auto itemcnt = static_cast<std::size_t>(itemcnt_);
    if (this_pkcnt <= itemcnt_ || (des_[itemcnt - 1].score > des_[itemcnt].score))
    {
        pkcnt_here = std::min(this_pkcnt, itemcnt_);
        for (n = 0; n < pkcnt_here; n++)
            peakno_[static_cast<std::size_t>(n)] = des_[static_cast<std::size_t>(n)].chan;
    }
    else
    {
        sortEquivalentScores(this_pkcnt);
        pkcnt_here = itemcnt_;
    }
    sortFocuPeaksByFrq(pkcnt_here);
    establishBottomAndTopOfFilterBands(pkcnt_here);
    constructFilterEnvelope(pkcnt_here, middle);
    if (w > sl1)
    {
        // unscore_peaks: CDP's fpk[0], window w - (stability - 1).
        const auto oldest = slot(w - sl1);
        const int* fppk = fpk_.data() + oldest * specenvcnt;
        for (n = 0; n < totalPkcnt_[oldest]; n++)
        {
            designScore_[static_cast<std::size_t>(fppk[n])]--;
            peakscore_--;
        }
    }
}

// sort_equivalent_scores and do_amplitude_poll (tklib3.c): the peaks
// scoring as the last kept one does are chosen by their amplitudes summed
// over the stored windows, the loudest first.
void FocusFocus::sortEquivalentScores(int this_pkcnt) noexcept
{
    const auto w = totalWindows_;
    const auto sl1 = static_cast<std::size_t>(sl1_);
    const auto specenvcnt = static_cast<std::size_t>(specenvcnt_);
    const auto des = [this](int index) -> Design& { return des_[static_cast<std::size_t>(index)]; };
    int bot, top, n;
    n = itemcnt_ - 1;
    while (n >= 0)
    {
        if (des(n).score == des(itemcnt_).score)
            n--;
        else
            break;
    }
    bot = n + 1;
    n = itemcnt_ + 1;
    while (n < this_pkcnt)
    {
        if (des(n).score == des(itemcnt_).score)
            n++;
        else
            break;
    }
    top = n;
    for (n = 0; n < bot; n++)
        peakno_[static_cast<std::size_t>(n)] = des(n).chan; /* keep peaks that have definitely higher scores */
    // do_amplitude_poll.
    int m, here = 0, thischan;
    int nextpeak = bot;
    double thisamp, ampmax;
    for (n = bot; n < top; n++)
    {
        thischan = des(n).chan;
        for (m = 0; m < stabl_; m++)
        {
            const auto store = slot(w - sl1 + static_cast<std::size_t>(m));
            des(n).amp += spec_[store * specenvcnt + static_cast<std::size_t>(thischan)];
        }
    }
    while (nextpeak < itemcnt_)
    {
        ampmax = 0.0;
        for (n = bot; n < top; n++)
        {
            if ((thisamp = des(n).amp) > ampmax)
            {
                ampmax = thisamp;
                here = n;
            }
        }
        peakno_[static_cast<std::size_t>(nextpeak++)] = des(here).chan;
        des(here).amp = 0.0;
    }
}
}
