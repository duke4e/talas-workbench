#pragma once

#include "talas/project/VirtualControlOsc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace talas::runtime
{
inline constexpr std::size_t maximumOscUdpPacketBytes = 65'507;
inline constexpr std::size_t maximumOscBundleDepth = 4;
inline constexpr std::size_t maximumOscPacketElements = 128;
inline constexpr std::size_t maximumOscControlMessageBytes =
    ((talas::project::maximumOscAddressBytes + 1 + 3) & ~std::size_t{3})
    + 4 + 8;
inline constexpr std::uint64_t oscImmediateTimetag = 1;
inline constexpr std::uint64_t oscNtpFractionUnitsPerSecond =
    std::uint64_t{1} << 32;
inline constexpr std::uint64_t oscNtpUnixEpochOffsetSeconds = 2'208'988'800;
inline constexpr std::uint64_t maximumOscFutureTimetagDelta =
    2 * oscNtpFractionUnitsPerSecond;
inline constexpr std::uint64_t maximumOscLateTimetagDelta =
    oscNtpFractionUnitsPerSecond / 10;

enum class OscPacketStatus : std::uint8_t
{
    Accepted,
    Empty,
    PacketTooLarge,
    Malformed,
    InvalidAddress,
    UnsupportedType,
    InvalidValue,
    UnsupportedTimetag,
    RegressingTimetag,
    LimitExceeded,
};

struct OscControlMessage
{
    std::array<char, talas::project::maximumOscAddressBytes + 1> address{};
    std::uint16_t addressSize = 0;
    double normalizedValue = 0.0;
    std::uint64_t timetag = oscImmediateTimetag;

    [[nodiscard]] std::string_view addressView() const noexcept
    {
        return {address.data(), addressSize};
    }
};

enum class OscNtpConversionStatus : std::uint8_t
{
    Immediate,
    Scheduled,
    Late,
    TooLate,
    TooFar,
    InvalidAnchor,
    ArithmeticOverflow,
};

struct OscNtpClockAnchor
{
    std::uint64_t timetag = 0;
    std::int64_t steadyNanoseconds = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return timetag != 0 && steadyNanoseconds > 0;
    }
};

struct OscNtpConversionResult
{
    OscNtpConversionStatus status = OscNtpConversionStatus::InvalidAnchor;
    std::int64_t steadyDeadlineNanoseconds = 0;

    [[nodiscard]] bool accepted() const noexcept
    {
        return status == OscNtpConversionStatus::Immediate
            || status == OscNtpConversionStatus::Scheduled
            || status == OscNtpConversionStatus::Late;
    }
};

struct OscPacketParseResult
{
    OscPacketStatus status = OscPacketStatus::Malformed;
    std::size_t messageCount = 0;

    [[nodiscard]] bool accepted() const noexcept
    {
        return status == OscPacketStatus::Accepted;
    }
};

enum class OscPacketEncodeStatus : std::uint8_t
{
    Encoded,
    InvalidAddress,
    InvalidValue,
    OutputTooSmall,
};

struct OscPacketEncodeResult
{
    OscPacketEncodeStatus status = OscPacketEncodeStatus::InvalidAddress;
    std::size_t packetSize = 0;

    [[nodiscard]] bool encoded() const noexcept
    {
        return status == OscPacketEncodeStatus::Encoded;
    }
};

[[nodiscard]] OscPacketParseResult parseOscControlPacket(
    std::span<const std::uint8_t> packet,
    std::span<OscControlMessage> output) noexcept;
[[nodiscard]] OscPacketEncodeResult encodeOscControlMessage(
    std::string_view address,
    double normalizedValue,
    std::span<std::uint8_t> output) noexcept;
[[nodiscard]] OscNtpClockAnchor makeOscNtpClockAnchor(
    std::int64_t unixNanoseconds,
    std::int64_t steadyNanoseconds) noexcept;
[[nodiscard]] OscNtpConversionResult convertOscNtpTimetag(
    std::uint64_t timetag,
    OscNtpClockAnchor anchor) noexcept;
}
