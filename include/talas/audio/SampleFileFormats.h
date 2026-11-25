#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The sample file formats Talas reads and writes: WAVE, AIFF, IRCAM/MTU
// (BICSF), Sound Designer I and Sound Designer II. The readers are
// readSampleFile's (talas/audio/WavReader.h); WAVE and AIFF are written by the
// WAV writer and writeAiffFile, the other three here.
namespace talas::audio
{
enum class SampleFileFormat : std::uint8_t
{
    Wave,
    Aiff,
    Ircam,
    SoundDesignerI,
    SoundDesignerII,
};

// Every format, in the order the DiskRecorder's Format lists them:
// SDI, SDII, AIFF, IRCAM, WAVE.
[[nodiscard]] std::vector<SampleFileFormat> sampleFileFormats();

// The DiskRecorder's names for them: SDI, SDII, AIFF, IRCAM and
// WAVE; WAV is WAVE too, and so is an empty Format. Nothing for others.
[[nodiscard]] std::optional<SampleFileFormat> sampleFileFormatNamed(std::string_view name) noexcept;
[[nodiscard]] std::string_view sampleFileFormatName(SampleFileFormat format) noexcept;

// The extension a new file of the format takes: .wav, .aif, .sf, .sd1 or .sd2.
[[nodiscard]] std::string_view sampleFileFormatExtension(SampleFileFormat format) noexcept;
// Whether a file's name ends in one of the format's extensions, any case:
// .wav or .wave, .aif, .aiff or .aifc, .sf, .sd1, .sd2.
[[nodiscard]] bool sampleFileHasFormatExtension(const std::filesystem::path& path, SampleFileFormat format);

// Sound Designer II keeps its parameters in a file's resource fork, so it
// is read and written on a Macintosh only.
[[nodiscard]] bool soundDesignerIIAvailable() noexcept;

// Why the format cannot hold `channels` channels of `bits`-bit samples
// here; empty when it can. Each holds 8-, 16- or 24-bit samples;
// a Sound Designer I file holds one channel, its header naming none
// (Digidesign Developer Documentation, April 1990, p. 29), and Sound
// Designer II needs a Macintosh.
[[nodiscard]] std::string sampleFileFormatRefusal(SampleFileFormat format, std::size_t channels, std::uint16_t bits);

// An IRCAM sound file (BICSF; the McGill AFsp description): a 1024-byte
// header, the Sun's identifier (bytes 64 A3 02 00, big-endian), the rate
// as a float, the channels and the packing (1, 2 or 3 bytes a sample),
// an SF_END code, then the samples, interleaved and big-endian.
[[nodiscard]] std::string writeIrcamFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits);

// A Sound Designer I file (Digidesign Developer Documentation, April 1990,
// pp. 29-32): its 1336-byte header, the HeaderType record laid out as the
// Macintosh's Pascal lays it out, big-endian, with its defaults, the
// number of bytes of sound (offset 184), the file's name (254), the
// sample rate (1020), its period in microseconds (1024) and the bits in a
// sample (1028), then one channel of big-endian samples.
[[nodiscard]] std::string writeSoundDesignerIFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits);

// A Sound Designer II file (the same documentation, p. 33): the samples,
// interleaved and big-endian, in the data fork, and in the resource fork
// the three 'STR ' resources Sound Designer II needs: 1000 sample-size,
// the bytes in a sample; 1001 sample-rate, as "44100.0000"; 1002
// channels. Refused where there are no resource forks.
[[nodiscard]] std::string writeSoundDesignerIIFile(const std::filesystem::path& destination, double sampleRate,
    const std::vector<std::vector<double>>& channels, std::uint16_t bits);

// A Sound Designer II file's parameters, from its resource fork.
struct SoundDesignerIIParameters
{
    std::uint16_t bytesPerSample = 0;
    double sampleRate = 0.0;
    std::uint16_t channels = 0;
};

// The resource fork holding them, as written above.
[[nodiscard]] std::vector<std::uint8_t> soundDesignerIIResourceFork(double sampleRate, std::size_t channels,
    std::uint16_t bytesPerSample);
// Read from a Macintosh resource fork's bytes: its 'STR ' resources 1000,
// 1001 and 1002; nothing when one is missing or makes no sense.
[[nodiscard]] std::optional<SoundDesignerIIParameters> soundDesignerIIParametersFromResourceFork(
    std::span<const std::uint8_t> resourceFork);
// A file's resource fork (<path>/..namedfork/rsrc on a Macintosh); empty
// where it has none or there are no resource forks.
[[nodiscard]] std::vector<std::uint8_t> resourceForkOf(const std::filesystem::path& path);
}
