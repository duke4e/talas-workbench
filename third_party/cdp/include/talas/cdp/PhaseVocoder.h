/*
 * Derived from the CDP System, Release 8: dev/pv/pvoc.c and mxfft.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `pvoc anal` and `pvoc synth`, CARL's phase vocoder: every D input
// frames a window of M frames is transformed into points / 2 + 1 channels of
// an amplitude and a frequency (CDP's analysis frame, as an .ana file holds
// it), and each frame is turned back into sound by weighted overlap-add. M
// is four, two, one or a half times the points for overlaps 1 to 4, and D
// an eighth of M.
//
// CDP reads its whole file; the stream waits for the frames each output
// frame needs, so the output comes latencyFrames late. A spectral process
// edits each analysis frame between the two halves, as CDP's spectral
// programs edit an .ana file between `pvoc anal` and `pvoc synth`.
class PhaseVocoder
{
public:
    static constexpr int minimumPoints = 2;
    // The most an .ana file holds.
    static constexpr int maximumPoints = 8192;

    // CDP's points, made even as CDP makes them, and overlap 1 to 4. False,
    // with no change, for a sample rate that is not positive, points or an
    // overlap outside those ranges, or points whose half CDP's FFT cannot
    // factor.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap);

    // The latency prepare gives those points and overlap; nothing for the
    // ones prepare refuses.
    [[nodiscard]] static std::optional<std::size_t> latencyFor(int points, int overlap) noexcept;

    // The analysis frames' spacing, D, for those points and overlap.
    [[nodiscard]] static std::optional<int> decimationFor(int points, int overlap) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !anal_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    // An analysis frame's floats: an amplitude and a frequency for each of
    // points / 2 + 1 channels.
    [[nodiscard]] std::size_t frameSize() const noexcept { return anal_.size(); }

    // The analysis frames' spacing, D, in frames.
    [[nodiscard]] int decimation() const noexcept { return d_; }

    // Each analysis frame goes to edit(std::span<float>) before synthesis.
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    template <typename Edit>
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<double> output, Edit&& edit) noexcept
    {
        if (!prepared() || input.size() != output.size()
            || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        {
            return false;
        }
        for (std::size_t index = 0; index < input.size(); ++index)
        {
            if (consume(static_cast<float>(input[index])))
            {
                edit(std::span<float>{anal_});
                synthesise();
            }
            output[index] = emit();
        }
        return true;
    }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept
    {
        return process(input, output, [](std::span<float>) {});
    }

    // Two files in step, as CDP's two-file spectral programs read two .ana
    // files window by window: the side stream goes through sideAnalysis, a
    // second phase vocoder prepared alike and reset with this one, which
    // only analyses it. Each pair of frames goes to edit(std::span<float>,
    // std::span<const float> sideFrame) before this frame's synthesis.
    // False, with no change, before either is prepared, for vocoders of
    // different sizes or out of step, for spans of different sizes or a
    // non-finite input.
    template <typename Edit>
    [[nodiscard]] bool process(std::span<const double> input, std::span<const double> side,
        PhaseVocoder& sideAnalysis, std::span<double> output, Edit&& edit) noexcept
    {
        const auto finite = [](double sample) { return std::isfinite(sample); };
        if (!prepared() || &sideAnalysis == this || !inStepWith(sideAnalysis) || input.size() != output.size()
            || side.size() != input.size() || !std::ranges::all_of(input, finite)
            || !std::ranges::all_of(side, finite))
        {
            return false;
        }
        for (std::size_t index = 0; index < input.size(); ++index)
        {
            // In step, both complete their frames at the same input frame.
            const bool frame = consume(static_cast<float>(input[index]));
            if (sideAnalysis.consume(static_cast<float>(side[index])))
                sideAnalysis.skipSynthesis();
            if (frame)
            {
                edit(std::span<float>{anal_}, std::span<const float>{sideAnalysis.anal_});
                synthesise();
            }
            output[index] = emit();
        }
        return true;
    }

private:
    // Prepared alike (points, overlap and sample rate) and at the same
    // input frame.
    [[nodiscard]] bool inStepWith(const PhaseVocoder& other) const noexcept
    {
        return other.prepared() && other.n_ == n_ && other.m_ == m_ && other.d_ == d_ && other.f_ == f_
            && other.nI_ == nI_ && other.dd_ == dd_ && other.read_ == read_ && other.nextIn_ == nextIn_;
    }

    // synthesise's step of pvoc_process's times, for a vocoder that only
    // analyses.
    void skipSynthesis() noexcept
    {
        nI_ += d_;
        nO_ += d_;
        dd_ = d_;
    }

    // Takes one input frame; true when it completes an analysis frame.
    [[nodiscard]] bool consume(float sample) noexcept;
    void analyse() noexcept;
    void synthesise() noexcept;
    [[nodiscard]] double emit() noexcept;
    void fft(float* a, float* b, int n, int isn) noexcept;

    int n_ = 0;
    int n2_ = 0;
    int m_ = 0;
    int d_ = 0;
    int analWinLen_ = 0;
    int synWinLen_ = 0;
    int ibuflen_ = 0;
    int obuflen_ = 0;
    float f_ = 0.0F;
    float roverTwoPi_ = 0.0F;
    float twoPioverR_ = 0.0F;
    // Each window from -winLen to winLen, centred at winLen.
    std::vector<float> analWindow_;
    std::vector<float> synWindow_;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<float> anal_;
    std::vector<float> syn_;
    std::vector<float> oldInPhase_;
    std::vector<float> oldOutPhase_;
    // mxfft.c's work space, which fft_ allocates at each call.
    std::vector<float> at_;
    std::vector<float> ck_;
    std::vector<float> bt_;
    std::vector<float> sk_;
    std::vector<int> np_;
    // pvoc_process's times and counts.
    int nI_ = 0;
    int nO_ = 0;
    int dd_ = 0;
    int iOi_ = 0;
    int nextIn_ = 0;
    int nextOut_ = 0;
    int read_ = 0;
    // The frames synthesis has shifted out, waiting for the latency.
    std::vector<float> pending_;
    std::size_t pendingRead_ = 0;
    std::size_t pendingCount_ = 0;
    std::size_t latency_ = 0;
    std::size_t frame_ = 0;
};
}
