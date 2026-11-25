#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

// Sum-of-Sines spectrum files (envelopes, not samples, so an extension of
// their own, .spc): an AIFF FORM with COMM and SSND chunks, mono and 24 bits a
// sample, each frame its partials in increasing order, each partial one 24-bit
// word, its top byte the amplitude and its low 16 bits the frequency, both
// logarithmic; an APPL chunk 'SOSe' gives the partials a frame and the frame's
// duration in microseconds. Read, a file is a PartialTrackAsset, as a
// .talaspartial is (decodePartialTrackAsset reads either), its partial k track
// k + 1 and a silent partial no point; written, the tracks are laid into as
// few partials as never sound at once, the lowest first, their count a power
// of two of at least 32 as Loris (Kelly Fitz's SpcFile) writes them. The
// frequencies a file stores are logarithmic, those Talas's spectra carry
// linear, in hertz.
namespace talas::project
{
inline constexpr std::string_view sosSpectrumFileExtension = ".spc";
inline constexpr std::string_view partialTrackFileExtension = ".talaspartial";
// The patterns a file dialog offers for a spectrum file: Talas's first,
// so that a name typed without an extension is still a .talaspartial, then
// .spc.
inline constexpr std::string_view spectrumFilePatterns = "*.talaspartial;*.spc";

// One partial's 24-bit word: log2(amplitude) * 127 / 15 + 127 in the top
// byte, clipped to 0..127 and 0 for silence; log2(2 * frequency / rate) *
// 65536 / 15 + 65536 in the low word, up to half the sample rate.
[[nodiscard]] std::uint32_t encodeSosSpectrumPartial(
    double amplitude, double frequencyHz, double sampleRate) noexcept;
[[nodiscard]] double sosSpectrumPartialAmplitude(std::uint32_t partial) noexcept;
[[nodiscard]] double sosSpectrumPartialFrequency(std::uint32_t partial, double sampleRate) noexcept;

// Whether the bytes are a spectrum file: an AIFF holding an APPL 'SOSe'
// chunk.
[[nodiscard]] bool isSosSpectrumFile(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] PartialTrackAssetDecodeResult decodeSosSpectrumFile(
    std::span<const std::uint8_t> bytes,
    std::function<bool()> shouldCancel = {});

// The frames are written a frame duration apart, the asset's most common
// spacing, each track's amplitude and frequency taken between its frames;
// its markers go in a MARK chunk at their nearest frames and its loop in
// an INST chunk's sustain loop. Bandwidths are not kept.
[[nodiscard]] PartialTrackAssetEncodeResult encodeSosSpectrumFile(
    const PartialTrackAsset& asset);

// A spectrum file's name: .spc, or Talas's .talaspartial (and .talaspartials),
// in any case.
[[nodiscard]] bool isSpectrumFilePath(const std::filesystem::path& path);
[[nodiscard]] bool isSosSpectrumFilePath(const std::filesystem::path& path);

// The spectrum as the file's name asks for it: a Sum-of-Sines spectrum file
// for an .spc, Talas's otherwise.
[[nodiscard]] PartialTrackAssetEncodeResult encodeSpectrumFileFor(
    const std::filesystem::path& path,
    const PartialTrackAsset& asset);
}
