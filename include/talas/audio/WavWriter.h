#pragma once

#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::audio
{
// A PCM WAV file's word size: 8, 16 or 24 bits.
struct WavWordSize
{
    std::uint16_t bits = 16;
};

class Pcm16WavStreamWriter
{
public:
    Pcm16WavStreamWriter(
        const std::filesystem::path& path,
        double sampleRate,
        std::size_t channelCount,
        std::size_t totalFrameCount);
    // Takes ownership of an already-open binary stream. This lets secure
    // publication code create the inode relative to a held directory handle.
    Pcm16WavStreamWriter(
        std::FILE* output,
        std::string diagnosticPath,
        double sampleRate,
        std::size_t channelCount,
        std::size_t totalFrameCount);
    // Growing mode writes a placeholder header and commits the exact frame
    // count only from finish(). This is intended for bounded realtime takes
    // whose duration is not known before recording starts.
    Pcm16WavStreamWriter(
        const std::filesystem::path& path,
        double sampleRate,
        std::size_t channelCount);
    // Growing-mode counterpart for a securely created staging inode.
    // Takes ownership of output.
    Pcm16WavStreamWriter(
        std::FILE* output,
        std::string diagnosticPath,
        double sampleRate,
        std::size_t channelCount);
    // The same, writing 8-, 16- or 24-bit words, as a DiskRecorder's
    // WordSize asks.
    Pcm16WavStreamWriter(
        std::FILE* output,
        std::string diagnosticPath,
        double sampleRate,
        std::size_t channelCount,
        WavWordSize wordSize);
    ~Pcm16WavStreamWriter();

    Pcm16WavStreamWriter(const Pcm16WavStreamWriter&) = delete;
    Pcm16WavStreamWriter& operator=(const Pcm16WavStreamWriter&) = delete;
    Pcm16WavStreamWriter(Pcm16WavStreamWriter&&) noexcept;
    Pcm16WavStreamWriter& operator=(Pcm16WavStreamWriter&&) noexcept;

    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::string_view error() const;
    [[nodiscard]] std::size_t framesWritten() const;
    // Frames that still fit within the RIFF/WAVE size limit.
    [[nodiscard]] std::size_t remainingFrameCapacity() const;
    [[nodiscard]] bool write(std::span<const std::vector<double>> channels);
    [[nodiscard]] bool writeInterleaved(std::span<const float> samples);
    [[nodiscard]] bool finish();
    void abandon() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool writePcm16Wav(
    const std::filesystem::path& path,
    double sampleRate,
    std::span<const std::vector<double>> channels,
    std::string& error);
// The same in 8-, 16- or 24-bit words (a Timeline's caches in 24 bits).
[[nodiscard]] bool writePcmWav(
    const std::filesystem::path& path,
    double sampleRate,
    std::span<const std::vector<double>> channels,
    WavWordSize wordSize,
    std::string& error);
}
