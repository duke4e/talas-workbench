/*
 * Derived from the CDP System, Release 8: dev/modify/gain.c and
 * dev/standalone/phase.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP's two phase inversions, each sample negated: Loudness is `modify
// loudness` mode 6, INVERT PHASE, which holds an inverted sample above 1
// at 1 but leaves one below -1 as it is; Phase is `phase phase` mode 1,
// which holds nothing. Both turn 0 into -0 and -0 into 0.
class PhaseInvert
{
public:
    enum class Mode
    {
        Loudness,
        Phase,
    };

    // False, with no change, for a mode that is neither.
    [[nodiscard]] bool prepare(Mode mode) noexcept;

    // Nothing to clear: each sample is inverted on its own.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const PhaseInvert&) const = default;

private:
    bool prepared_ = false;
    Mode mode_ = Mode::Loudness;
};
}
