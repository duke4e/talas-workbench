#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
inline constexpr std::uint32_t mtcQuarterFramePieceCount = 8;
inline constexpr std::uint32_t mtcForwardCompensationFrames = 2;
inline constexpr std::uint64_t mtcSignalTimeoutNanoseconds = 500'000'000ULL;
inline constexpr std::uint32_t smpteRoundToNearestDivisor = 2;

enum class SmpteRate : std::uint8_t
{
    Fps24,
    Fps25,
    Fps2997Drop,
    Fps30,
};

enum class SmpteDirection : std::uint8_t
{
    Forward,
    Reverse,
};

struct SmpteRateRational
{
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
    std::uint8_t nominalFramesPerSecond = 0;
    bool dropFrame = false;

    bool operator==(const SmpteRateRational&) const = default;
};

struct SmpteTimeCode
{
    SmpteRate rate = SmpteRate::Fps24;
    bool dropFrame = false;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    std::uint8_t frame = 0;
    SmpteDirection direction = SmpteDirection::Forward;
    std::uint64_t absoluteFrame = 0;

    bool operator==(const SmpteTimeCode&) const = default;
};

struct MtcQuarterFramePiece
{
    std::uint8_t type = 0;
    std::uint8_t value = 0;

    bool operator==(const MtcQuarterFramePiece&) const = default;
};

struct MtcFullFrameMessage
{
    std::uint8_t deviceId = 0;
    SmpteTimeCode timeCode;

    bool operator==(const MtcFullFrameMessage&) const = default;
};

enum class MtcDecodeResult : std::uint8_t
{
    Accepted,
    InvalidShape,
    InvalidPiece,
    InvalidRate,
    InvalidLabel,
    UnsupportedDirection,
    ArithmeticOverflow,
};

[[nodiscard]] SmpteRateRational smpteRateRational(SmpteRate rate) noexcept;
[[nodiscard]] std::uint64_t smpteFramesPer24Hours(SmpteRate rate) noexcept;
[[nodiscard]] bool makeSmpteTimeCode(
    SmpteRate rate,
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    SmpteDirection direction,
    SmpteTimeCode& output) noexcept;
[[nodiscard]] bool isValidSmpteTimeCode(const SmpteTimeCode& value) noexcept;
[[nodiscard]] bool smpteTimeCodeFromAbsoluteFrame(
    SmpteRate rate,
    std::uint64_t absoluteFrame,
    SmpteDirection direction,
    SmpteTimeCode& output) noexcept;
[[nodiscard]] bool addSmpteFrames(
    const SmpteTimeCode& source,
    std::uint64_t frames,
    SmpteTimeCode& output,
    bool& wrapped) noexcept;
[[nodiscard]] bool smpteTimeCodeToSampleFrame(
    const SmpteTimeCode& source,
    std::uint32_t sampleRate,
    std::uint64_t& output) noexcept;

[[nodiscard]] MtcDecodeResult decodeMtcQuarterFramePiece(
    std::span<const std::uint8_t> bytes,
    MtcQuarterFramePiece& output) noexcept;
[[nodiscard]] MtcDecodeResult decodeMtcQuarterFrameSet(
    std::span<const MtcQuarterFramePiece> pieces,
    SmpteDirection direction,
    bool compensateForwardTransport,
    SmpteTimeCode& output,
    bool& wrapped) noexcept;
[[nodiscard]] MtcDecodeResult decodeMtcFullFrame(
    std::span<const std::uint8_t> bytes,
    MtcFullFrameMessage& output) noexcept;
}
