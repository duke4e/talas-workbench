#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The system wavetables of the Wavetables folder, computed rather than read
// from files. Only the tables of a known shape or of standard functions are
// provided, 4096 entries each as the Oscillator expects:
// - Waveforms: Sine, Cosine and Triangle, one cycle each; Saw21 and
//   Square21, a rising sawtooth of the first 21 harmonics at 1/n
//   and a square of the odd ones among them, and Buzz, the first 21
//   harmonics in cosine phase and of equal amplitude (our count), each scaled
//   to peak at 1; GlottalPulse, the TwoFormantElement's pulse (a raised cosine
//   opening over 40% of the cycle, a quarter cosine closing over 16%, then
//   closed; our shape). Not provided: the Buzzes, Saws and Squares folders of
//   tables of other harmonic counts for a KeyMappedMultiSample;
// - Control: FullRamp, a straight line from -1 up to 1, Ramp
//   from 0 up to 1, and Gaussian, a bell whose standard deviation of an
//   eighth of the table is this project's choice; Exponential, rising from
//   0 to 1 over 60 dB, ExponRev, falling back, FourExpons, four ExponRevs in
//   a row, and Random, uniform from -1 to 1 from a fixed seed;
// - Waveshapers: ATAN20 and ATAN40, atan(20 x) and atan(40 x) over an input
//   x from -1 to 1, scaled to reach 1. Not provided: Pentaton;
// - Windows: LinearEnvelope rising from 0 to 1 and back, Hann, which is zero
//   at its first and last entries, Hamming and Rectangular;
// - Internal: Zero.
namespace talas::dsp
{
inline constexpr std::size_t systemWavetableLength = 4096;

[[nodiscard]] std::span<const std::string_view> systemWavetableNames() noexcept;

// The named table, or none when no such table is provided here.
[[nodiscard]] std::optional<std::vector<double>> systemWavetable(std::string_view name);

// Computed material of other shapes, the defaults of the fields that take
// an analysis, an RE table or a terrain, all computed here:
// - HarmonicAmplitudes: one frame of the amplitudes of eight harmonic
//   partials, 1/n summing to 1, the analysis a SumOfSines or GAOscillators
//   resynthesizes at the harmonics of its Frequency;
// - HarmonicSpectrum: one frame of eight harmonic partials as amplitude,
//   frequency and bandwidth triples, amplitudes 1/n summing to 1,
//   frequencies the harmonic numbers relative to the base pitch, so that
//   a Frequency of f plays them at n f, and
//   no bandwidth;
// - Resonance: an RE table (ADR-0022) of order 2 with one set of
//   reflection coefficients, -0.9 and 0.5, a stable resonance;
// - Terrain: an 8 by 8 wave terrain, sin(2 pi x / 8) cos(2 pi y / 8) row
//   by row, for a WaveTerrainOscillator's default Width and Height.
[[nodiscard]] std::span<const std::string_view> systemTableNames() noexcept;

// A system wavetable or one of the computed tables above by name.
[[nodiscard]] std::optional<std::vector<double>> systemTable(std::string_view name);
}
