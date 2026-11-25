#include "talas/audio/ResourceLoader.h"
#include "talas/audio/SampleFileFormats.h"
#include "talas/audio/WavReader.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
              / ("talas-sample-file-formats-"
                  + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::uint8_t> bytesOf(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "the test should write " + path.string());
}

[[nodiscard]] std::uint32_t big(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
        value = (value << 8U) | bytes[offset + index];
    return value;
}

[[nodiscard]] std::vector<std::uint8_t> hexBytes(std::string_view hex)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(index, 2)}, nullptr, 16)));
    return bytes;
}

// Samples a word of 16 bits holds exactly: whole codes over 32767.
const std::vector<std::vector<double>> stereo16{
    {16384.0 / 32767.0, 1.0, 0.0},
    {-16384.0 / 32767.0, -1.0, 1.0 / 32767.0},
};

// IRCAM/MTU files are written as well as read: an IRCAM file is written
// with the McGill description's header, a Sun's identifier, its rate as a
// big-endian float, its channels and its packing, the samples big-endian
// after 1024 bytes; it reads back exactly.
// Files written by other programs read too: libsndfile's (a MIPS
// identifier, little-endian), and a Sun's identifier reversed, which means
// little-endian.
void testIrcamFilesAreWrittenAndExchanged()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "take.sf";
    require(talas::audio::writeIrcamFile(path, 48000.0, stereo16, 16).empty(), "an IRCAM file should be written");
    const auto bytes = bytesOf(path);
    require(bytes.size() == 1024 + 3 * 2 * 2 && bytes[0] == 0x64 && bytes[1] == 0xA3 && bytes[2] == 0x02
            && bytes[3] == 0x00 && std::bit_cast<float>(big(bytes, 4, 4)) == 48000.0F && big(bytes, 8, 4) == 2
            && big(bytes, 12, 4) == 2 && big(bytes, 16, 4) == 0,
        "an IRCAM file should start with a Sun's identifier, the rate, the channels, SF_SHORT and SF_END");
    require(big(bytes, 1024, 2) == 0x4000 && big(bytes, 1026, 2) == 0xC000 && big(bytes, 1028, 2) == 0x7FFF
            && big(bytes, 1030, 2) == 0x8000,
        "an IRCAM file's samples should follow its header, interleaved and big-endian");
    const auto read = talas::audio::readSampleFile(path);
    require(read.ok() && read.sampleRate == 48000 && read.channels == stereo16,
        "the IRCAM file written should open: " + read.error);
    for (const std::uint16_t bits : {8, 24})
    {
        const auto wordPath = directory.path() / ("take " + std::to_string(bits) + ".sf");
        require(talas::audio::writeIrcamFile(wordPath, 44100.0, {{0.5, -1.0}}, bits).empty()
                && big(bytesOf(wordPath), 12, 4) == bits / 8U,
            "an IRCAM file of " + std::to_string(bits) + " bits should say so in its packing");
        const auto word = talas::audio::readSampleFile(wordPath);
        const auto half = static_cast<double>((1U << (bits - 1U)) - 1U);
        require(word.ok() && word.channels.size() == 1 && word.channels[0][1] == -1.0
                && word.channels[0][0] == std::round(0.5 * half) / half,
            "an IRCAM file of " + std::to_string(bits) + " bits should read back: " + word.error);
    }

    // libsndfile's IRCAM file of the same samples (sndfile-convert): the
    // identifier 64 A3 03 00, a MIPS's, little-endian.
    auto sndfile = hexBytes("64a30300" "00803b47" "02000000" "02000000");
    sndfile.resize(1024, 0);
    for (const auto byte : hexBytes("004000c0ff7f008000000100"))
        sndfile.push_back(byte);
    const auto sndfilePath = directory.path() / "sndfile.sf";
    writeBytes(sndfilePath, sndfile);
    const auto exchanged = talas::audio::readSampleFile(sndfilePath);
    require(exchanged.ok() && exchanged.sampleRate == 48000 && exchanged.channels == stereo16,
        "an IRCAM file another program wrote should read: " + exchanged.error);

    // A Sun's identifier reversed (00 02 A3 64) means little-endian.
    auto reversed = hexBytes("0002a364" "00803b47" "01000000" "02000000");
    reversed.resize(1024, 0);
    for (const auto byte : hexBytes("0040ff7f"))
        reversed.push_back(byte);
    const auto reversedPath = directory.path() / "reversed.sf";
    writeBytes(reversedPath, reversed);
    const auto swapped = talas::audio::readSampleFile(reversedPath);
    require(swapped.ok() && swapped.sampleRate == 48000 && swapped.channels.size() == 1
            && swapped.channels[0] == std::vector<double>{16384.0 / 32767.0, 1.0},
        "a Sun's reversed identifier should read little-endian: " + swapped.error);
}

// An SD-I sample file opens, is created, plays and imports:
// a Sound Designer I file is created with the 1336-byte header of the
// Digidesign Developer Documentation (April 1990, pp. 29-32) laid out as
// the Macintosh's Pascal lays out its HeaderType record: its length and
// version first, the bytes of sound at 184, the file's name at 254, the
// rate at 1020, its period at 1024, the bits at 1028, one channel of
// big-endian samples after it. It opens and plays (readSampleFile), and
// imports as a table; a header made from the documented offsets alone
// reads too. A Sound Designer I file holds one channel, so stereo is
// refused.
void testSoundDesignerIFiles()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "voice.sd1";
    require(talas::audio::writeSoundDesignerIFile(path, 44100.0, {stereo16[0]}, 16).empty(),
        "an SD-I file should be created");
    const auto bytes = bytesOf(path);
    require(bytes.size() == 1336 + 3 * 2 && big(bytes, 0, 2) == 1336 && big(bytes, 2, 2) == 32
            && big(bytes, 184, 4) == 6 && big(bytes, 1020, 4) == 44100 && big(bytes, 1024, 4) == 23
            && big(bytes, 1028, 2) == 16 && big(bytes, 752, 4) == 0xFFFFFFFFU,
        "an SD-I header should hold its length, version, bytes of sound, rate, period, bits and no loop");
    require(bytes[254] == 9 && std::string(bytes.begin() + 255, bytes.begin() + 264) == "voice.sd1",
        "an SD-I header should name its file");
    require(big(bytes, 1336, 2) == 0x4000 && big(bytes, 1338, 2) == 0x7FFF && big(bytes, 1340, 2) == 0,
        "an SD-I file's samples should follow its header, big-endian");
    const auto read = talas::audio::readSampleFile(path);
    require(read.ok() && read.sampleRate == 44100 && read.channels.size() == 1 && read.channels[0] == stereo16[0],
        "the SD-I file should open and play: " + read.error);
    const auto table = talas::audio::loadTableResource(path);
    require(table.ok() && table.sourceSampleRate == 44100 && table.samples == stereo16[0],
        "an SD-I file should import as a sample: " + table.error);
    require(!talas::audio::writeSoundDesignerIFile(directory.path() / "two.sd1", 44100.0, stereo16, 16).empty()
            && !std::filesystem::exists(directory.path() / "two.sd1"),
        "an SD-I file holds one channel, so a stereo one should be refused");
    const auto eight = directory.path() / "eight.sd1";
    require(talas::audio::writeSoundDesignerIFile(eight, 22050.0, {{-1.0, 127.0 / 127.0}}, 8).empty()
            && big(bytesOf(eight), 1028, 2) == 8 && bytesOf(eight).size() == 1338,
        "an 8-bit SD-I file should say so");
    const auto eightRead = talas::audio::readSampleFile(eight);
    require(eightRead.ok() && eightRead.sampleRate == 22050 && eightRead.channels[0] == std::vector<double>{-1.0, 1.0},
        "an 8-bit SD-I file should read back: " + eightRead.error);

    // A header holding only what the documentation's offsets say: its
    // length, the rate and 16 bits; the bytes of sound left 0, the sound
    // runs to the end of the file.
    std::vector<std::uint8_t> documented(1336, 0);
    documented[0] = 0x05;
    documented[1] = 0x38;
    documented[1020 + 2] = 0x7D; // 32000
    documented[1020 + 3] = 0x00;
    documented[1029] = 16;
    for (const auto byte : hexBytes("c0004000"))
        documented.push_back(byte);
    const auto documentedPath = directory.path() / "documented.sd1";
    writeBytes(documentedPath, documented);
    const auto documentedRead = talas::audio::readSampleFile(documentedPath);
    require(documentedRead.ok() && documentedRead.sampleRate == 32000
            && documentedRead.channels == std::vector<std::vector<double>>{{-16384.0 / 32767.0, 16384.0 / 32767.0}},
        "an SD-I file from another program should read: " + documentedRead.error);
}

// An SD-II sample file opens, is created and plays, on a Macintosh only:
// a Sound Designer II file keeps its samples, big-endian,
// in its data fork and its sample-size, sample-rate and channels in 'STR '
// resources 1000 to 1002 of its resource fork (Digidesign Developer
// Documentation, April 1990, p. 33). One is created that reads back, and
// opens one Apple's afconvert made (its data and resource forks here). Away
// from a Macintosh, writing one is refused.
void testSoundDesignerIIFiles()
{
    TemporaryDirectory directory;
    // afconvert -f Sd2f -d BEI16 of three stereo frames at 48 kHz.
    const auto afconvertFork = hexBytes(
        "000001000000011b0000001b0000006b"
        + std::string(480, '0')
        + "0000000b0a34383030302e30303030000000020132000000020132000001000000011b000000"
          "1b0000006b0000000009000000001c004a0000535452200002000a03e9000000000000010000"
          "0003e8000c0000000f0200000003ea001800000015030000000b73616d706c652d726174650b"
          "73616d706c652d73697a65086368616e6e656c73");
    require(afconvertFork.size() == 390, "the afconvert fork fixture should be whole");
    const auto parameters = talas::audio::soundDesignerIIParametersFromResourceFork(afconvertFork);
    require(parameters.has_value() && parameters->bytesPerSample == 2 && parameters->sampleRate == 48000.0
            && parameters->channels == 2,
        "the sample-size, sample-rate and channels of Apple's SD-II file should be read");
    const auto ours = talas::audio::soundDesignerIIResourceFork(44100.0, 1, 3);
    const auto oursRead = talas::audio::soundDesignerIIParametersFromResourceFork(ours);
    require(oursRead.has_value() && oursRead->bytesPerSample == 3 && oursRead->sampleRate == 44100.0
            && oursRead->channels == 1,
        "the resource fork written should hold its three parameters");
    const std::string_view rate{"\x0a" "44100.0000"};
    require(std::search(ours.begin(), ours.end(), rate.begin(), rate.end()) != ours.end(),
        "the sample rate should be written as Sound Designer II and afconvert write it, 44100.0000");

    const auto path = directory.path() / "take.sd2";
    const auto written = talas::audio::writeSoundDesignerIIFile(path, 48000.0, stereo16, 16);
    if (!talas::audio::soundDesignerIIAvailable())
    {
        require(!written.empty() && written.find("Macintosh") != std::string::npos,
            "away from a Macintosh, an SD-II file should be refused");
        return;
    }
    require(written.empty(), "an SD-II file should be created: " + written);
    require(bytesOf(path) == hexBytes("4000c0007fff800000000001"),
        "an SD-II file's data fork should hold its samples alone, interleaved and big-endian, as afconvert's");
    const auto fork = talas::audio::resourceForkOf(path);
    const auto forkRead = talas::audio::soundDesignerIIParametersFromResourceFork(fork);
    require(forkRead.has_value() && forkRead->bytesPerSample == 2 && forkRead->sampleRate == 48000.0
            && forkRead->channels == 2,
        "an SD-II file's resource fork should give its sample size, rate and channels");
    const auto read = talas::audio::readSampleFile(path);
    require(read.ok() && read.sampleRate == 48000 && read.channels == stereo16,
        "the SD-II file written should open and play: " + read.error);

    // Apple's file, its data fork and resource fork, opens alike.
    const auto apple = directory.path() / "apple.sd2";
    writeBytes(apple, hexBytes("4000c0007fff800000000001"));
    writeBytes(apple / "..namedfork" / "rsrc", afconvertFork);
    const auto appleRead = talas::audio::readSampleFile(apple);
    require(appleRead.ok() && appleRead.sampleRate == 48000 && appleRead.channels == stereo16,
        "an SD-II file afconvert made should open: " + appleRead.error);
    const auto table = talas::audio::loadTableResource(apple);
    require(table.ok() && table.sourceSampleRate == 48000 && table.samples == stereo16[0],
        "an SD-II file should play as a sample: " + table.error);

    // 24-bit mono.
    const auto mono = directory.path() / "mono.sd2";
    require(talas::audio::writeSoundDesignerIIFile(mono, 96000.0, {{0.5, -1.0}}, 24).empty()
            && bytesOf(mono).size() == 6,
        "a 24-bit SD-II file should hold three bytes a sample");
    const auto monoRead = talas::audio::readSampleFile(mono);
    require(monoRead.ok() && monoRead.sampleRate == 96000 && monoRead.channels.size() == 1
            && monoRead.channels[0][1] == -1.0 && monoRead.channels[0][0] == std::round(0.5 * 8388607.0) / 8388607.0,
        "a 24-bit SD-II file should read back: " + monoRead.error);
}

// The DiskRecorder's Format names SDI, SDII,
// AIFF, IRCAM and WAVE, each a file of its own extension; a Sound Designer
// I file holds one channel, and every format 8-, 16- or 24-bit samples.
void testFormatNamesAndExtensions()
{
    using talas::audio::SampleFileFormat;
    require(talas::audio::sampleFileFormats()
            == std::vector<SampleFileFormat>{SampleFileFormat::SoundDesignerI, SampleFileFormat::SoundDesignerII,
                SampleFileFormat::Aiff, SampleFileFormat::Ircam, SampleFileFormat::Wave},
        "the formats should be SDI, SDII, AIFF, IRCAM and WAVE");
    for (const auto format : talas::audio::sampleFileFormats())
    {
        require(talas::audio::sampleFileFormatNamed(talas::audio::sampleFileFormatName(format)) == format,
            "each Format name should name its format");
        require(talas::audio::sampleFileHasFormatExtension(
                    std::filesystem::path{"take"} += std::string{talas::audio::sampleFileFormatExtension(format)}, format),
            "each format's file should have its extension");
    }
    require(talas::audio::sampleFileFormatNamed("") == SampleFileFormat::Wave
            && talas::audio::sampleFileFormatNamed("WAV") == SampleFileFormat::Wave
            && !talas::audio::sampleFileFormatNamed("MP3").has_value(),
        "an empty Format and WAV should be WAVE, others nothing");
    require(talas::audio::sampleFileFormatExtension(SampleFileFormat::SoundDesignerI) == ".sd1"
            && talas::audio::sampleFileFormatExtension(SampleFileFormat::Ircam) == ".sf"
            && talas::audio::sampleFileHasFormatExtension("take.AIFF", SampleFileFormat::Aiff)
            && !talas::audio::sampleFileHasFormatExtension("take.wav", SampleFileFormat::Aiff),
        "SD-I files end in .sd1, IRCAM files in .sf");
    require(!talas::audio::sampleFileFormatRefusal(SampleFileFormat::SoundDesignerI, 2, 16).empty()
            && talas::audio::sampleFileFormatRefusal(SampleFileFormat::SoundDesignerI, 1, 24).empty()
            && !talas::audio::sampleFileFormatRefusal(SampleFileFormat::Ircam, 2, 32).empty()
            && talas::audio::sampleFileFormatRefusal(SampleFileFormat::SoundDesignerII, 2, 8).empty()
                == talas::audio::soundDesignerIIAvailable(),
        "SD-I should hold one channel, every format 8 to 24 bits, SD-II only on a Macintosh");
}
}

int main()
{
    testIrcamFilesAreWrittenAndExchanged();
    testSoundDesignerIFiles();
    testSoundDesignerIIFiles();
    testFormatNamesAndExtensions();
    return 0;
}
