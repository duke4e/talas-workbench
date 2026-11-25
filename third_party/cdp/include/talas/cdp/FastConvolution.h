/*
 * Derived from the CDP System, Release 8: dev/externals/fastconv/fconv.cpp,
 * genrespframe2.cpp and mxfftd.c
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
struct FastConvolutionSettings
{
    // fastconv's dry, 0 to 1: the output takes the input times
    // sin(dry × 1.570796327) and the convolution times cos of it. CDP's dry
    // of 1 ("copies input") sets the convolution's gain to 0 but leaves the
    // input's at 0 too, so its output, and this one, is silent.
    double dry = 0.0;
    // fastconv's -a, above 0: multiplies the impulse response before its
    // level is measured, so the convolution's gain grows as its 1.5th power.
    double scale = 1.0;
    // The impulse response's channels, interleaved in prepare's span as
    // CDP's impulse soundfile holds them, and the one this output convolves
    // with. The loudest channel sets every output's gain.
    std::size_t impulseChannels = 1;
    std::size_t channel = 0;
    // The input's channels, as CDP allows them: impulseChannels, or 1 (each
    // output then convolves the same mono input), or any number with a mono
    // impulse response. Only a mono input with a multichannel impulse
    // response changes the sound: CDP rounds the dry part there before
    // adding it, where elsewhere it fuses the multiply and add.
    std::size_t inputChannels = 1;
};

// CDP `fastconv` (soundfile impulse response): the input convolved with an
// impulse response of L frames by overlap-add, a block of L input frames at
// a time, through CDP's double-precision mixed-radix FFT of the power of
// two at or above 2L points, then scaled by a gain set by L and the impulse
// response's RMS level, and mixed with the dry input. One output channel:
// for CDP's multichannel cases, one processor per output channel, each
// prepared with the whole impulse response.
//
// CDP reads the input in blocks of L frames; the stream waits for each
// block, so the output comes latencyFrames (L - 1) late, and each block's
// two FFTs run in the process call that completes it. CDP writes L frames
// after its input, the convolution's tail; the stream's tail plays on as
// silence follows, then the rest of that block's overlap, the FFTs'
// rounding noise past the convolution's end (CDP stops before it), then
// silence. The output is rounded to float, as CDP writes a float input's
// output; the input and impulse response stay double, as CDP reads them.
class FastConvolution
{
public:
    // CDP's only limit is memory; this one keeps the latency under 22 s at
    // 48 kHz, and the spectrum and work buffer under 17 MB each.
    static constexpr std::size_t maximumImpulseFrames = std::size_t{1} << 20;

    // False, with no change, for a sample rate that is not finite and
    // positive or differs from the impulse response's (CDP: "files must
    // have same sample rate"), an empty impulse response, one that is not
    // whole frames of impulseChannels, has a non-finite sample, or is longer
    // than maximumImpulseFrames, a channel outside the impulse response, an
    // input channel count CDP refuses (other than 1 or impulseChannels with
    // a multichannel impulse response), a dry outside 0 to 1, or a scale
    // that is not finite and above 0.
    // CDP's text impulse file is not ported: CDP never measures its level,
    // so its gain is 0 and only the dry input sounds.
    [[nodiscard]] bool prepare(double sampleRate, std::span<const double> impulse, double impulseSampleRate,
        const FastConvolutionSettings& settings = {});

    // Silence in the block and its overlap.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return frames_ > 0; }

    // The impulse response's frames less one.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return frames_ > 0 ? frames_ - 1 : 0; }

    // fastconv's FFT size: the power of two at or above twice the impulse
    // response's frames.
    [[nodiscard]] std::size_t fftLength() const noexcept { return fftLength_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    static void transform(std::span<double> data, int isn, const std::array<int, 32>& nfac, int m, int kt,
        std::array<int, 34>& np) noexcept;
    void convolveBlock() noexcept;

    std::size_t frames_ = 0;
    std::size_t fftLength_ = 0;
    // fft_'s factors of fftLength_ / 2, from nfac[1], and its counts.
    std::array<int, 32> nfac_{};
    int factorCount_ = 0;
    int squareFactors_ = 0;
    // fftmx's permutation workspace, which fft_ allocates at each call.
    std::array<int, 34> np_{};
    double gain_ = 0.0;
    double dryGain_ = 0.0;
    double wetGain_ = 1.0;
    bool fusedDry_ = true;
    // The impulse response's spectrum, as genimpframe2 leaves it.
    std::vector<double> impulseSpectrum_;
    // main's inbuf, transformed in place.
    std::vector<double> work_;
    std::vector<double> overlap_;
    std::vector<double> inputBlock_;
    // The last block's output, rounded to float.
    std::vector<double> outputBlock_;
    std::size_t position_ = 0;
    bool started_ = false;
};
}
