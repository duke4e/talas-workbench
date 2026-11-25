#include "talas/runtime/OscProtocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace talas::runtime
{
namespace
{
constexpr std::array<std::uint8_t, 8> bundlePrefix{
    '#', 'b', 'u', 'n', 'd', 'l', 'e', 0};

struct ParseState
{
    std::span<OscControlMessage> output;
    std::size_t messageCount = 0;
    std::size_t elementCount = 0;
};

[[nodiscard]] std::uint32_t readBig32(
    std::span<const std::uint8_t, 4> bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) << 24
        | static_cast<std::uint32_t>(bytes[1]) << 16
        | static_cast<std::uint32_t>(bytes[2]) << 8
        | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::uint64_t readBig64(
    std::span<const std::uint8_t, 8> bytes) noexcept
{
    return static_cast<std::uint64_t>(readBig32(bytes.first<4>())) << 32
        | readBig32(bytes.last<4>());
}

void writeBig64(
    std::uint64_t value,
    std::span<std::uint8_t, 8> output) noexcept
{
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        output[index] = static_cast<std::uint8_t>(
            value >> ((output.size() - 1 - index) * 8));
    }
}

struct OscString
{
    std::string_view value;
    std::size_t consumed = 0;
};

[[nodiscard]] bool readOscString(
    std::span<const std::uint8_t> source,
    OscString& output) noexcept
{
    std::size_t terminator = 0;
    while (terminator < source.size() && source[terminator] != 0)
        ++terminator;
    if (terminator == source.size())
        return false;
    const auto rawSize = terminator + 1;
    if (rawSize > std::numeric_limits<std::size_t>::max() - 3)
        return false;
    const auto paddedSize = (rawSize + 3) & ~std::size_t{3};
    if (paddedSize > source.size())
        return false;
    for (std::size_t index = terminator; index < paddedSize; ++index)
    {
        if (source[index] != 0)
            return false;
    }
    output.value = {
        reinterpret_cast<const char*>(source.data()), terminator};
    output.consumed = paddedSize;
    return true;
}

[[nodiscard]] OscPacketStatus parseMessage(
    std::span<const std::uint8_t> packet,
    std::uint64_t effectiveTimetag,
    ParseState& state) noexcept
{
    OscString address;
    if (!readOscString(packet, address))
        return OscPacketStatus::Malformed;
    if (!talas::project::isValidOscAddress(address.value))
        return OscPacketStatus::InvalidAddress;

    OscString types;
    if (!readOscString(packet.subspan(address.consumed), types))
        return OscPacketStatus::Malformed;
    if (types.value.size() != 2 || types.value.front() != ',')
        return OscPacketStatus::UnsupportedType;

    const auto payload = packet.subspan(address.consumed + types.consumed);
    double normalized = 0.0;
    switch (types.value[1])
    {
    case 'i':
        if (payload.size() != 4)
            return OscPacketStatus::Malformed;
        normalized = static_cast<double>(std::bit_cast<std::int32_t>(
            readBig32(std::span<const std::uint8_t, 4>{
                payload.data(), 4})));
        break;
    case 'f':
        if (payload.size() != 4)
            return OscPacketStatus::Malformed;
        normalized = static_cast<double>(std::bit_cast<float>(
            readBig32(std::span<const std::uint8_t, 4>{
                payload.data(), 4})));
        break;
    case 'd':
        if (payload.size() != 8)
            return OscPacketStatus::Malformed;
        normalized = std::bit_cast<double>(
            readBig64(std::span<const std::uint8_t, 8>{
                payload.data(), 8}));
        break;
    case 'T':
        if (!payload.empty())
            return OscPacketStatus::Malformed;
        normalized = 1.0;
        break;
    case 'F':
        if (!payload.empty())
            return OscPacketStatus::Malformed;
        normalized = 0.0;
        break;
    default:
        return OscPacketStatus::UnsupportedType;
    }

    if (!std::isfinite(normalized)
        || normalized < 0.0 || normalized > 1.0)
    {
        return OscPacketStatus::InvalidValue;
    }
    if (state.messageCount >= state.output.size())
        return OscPacketStatus::LimitExceeded;

    auto& target = state.output[state.messageCount];
    std::memcpy(
        target.address.data(), address.value.data(), address.value.size());
    target.address[address.value.size()] = '\0';
    target.addressSize = static_cast<std::uint16_t>(address.value.size());
    target.normalizedValue = normalized;
    target.timetag = effectiveTimetag;
    ++state.messageCount;
    return OscPacketStatus::Accepted;
}

[[nodiscard]] bool isBundle(
    std::span<const std::uint8_t> packet) noexcept
{
    return packet.size() >= bundlePrefix.size()
        && std::equal(
            bundlePrefix.begin(), bundlePrefix.end(), packet.begin());
}

[[nodiscard]] OscPacketStatus parseElement(
    std::span<const std::uint8_t> packet,
    std::size_t bundleDepth,
    bool countElement,
    std::uint64_t parentTimetag,
    ParseState& state) noexcept
{
    if (countElement && ++state.elementCount > maximumOscPacketElements)
        return OscPacketStatus::LimitExceeded;

    if (!isBundle(packet))
        return parseMessage(packet, parentTimetag, state);
    if (bundleDepth >= maximumOscBundleDepth || packet.size() < 16)
    {
        return bundleDepth >= maximumOscBundleDepth
            ? OscPacketStatus::LimitExceeded
            : OscPacketStatus::Malformed;
    }
    const auto bundleTimetag = readBig64(
        std::span<const std::uint8_t, 8>{packet.data() + 8, 8});
    const auto effectiveTimetag = bundleTimetag == oscImmediateTimetag
        ? parentTimetag
        : bundleTimetag;
    if (parentTimetag != oscImmediateTimetag
        && effectiveTimetag < parentTimetag)
        return OscPacketStatus::RegressingTimetag;

    std::size_t offset = 16;
    while (offset < packet.size())
    {
        if (packet.size() - offset < 4)
            return OscPacketStatus::Malformed;
        const auto elementSize = readBig32(
            std::span<const std::uint8_t, 4>{packet.data() + offset, 4});
        offset += 4;
        if (elementSize == 0 || elementSize > packet.size() - offset)
            return OscPacketStatus::Malformed;
        const auto status = parseElement(
            packet.subspan(offset, elementSize),
            bundleDepth + 1,
            true,
            effectiveTimetag,
            state);
        if (status != OscPacketStatus::Accepted)
            return status;
        offset += elementSize;
    }
    return offset == packet.size()
        ? OscPacketStatus::Accepted
        : OscPacketStatus::Malformed;
}
}

OscPacketParseResult parseOscControlPacket(
    std::span<const std::uint8_t> packet,
    std::span<OscControlMessage> output) noexcept
{
    if (packet.empty())
        return {OscPacketStatus::Empty, 0};
    if (packet.size() > maximumOscUdpPacketBytes)
        return {OscPacketStatus::PacketTooLarge, 0};

    ParseState state{output};
    const auto status = parseElement(
        packet, 0, false, oscImmediateTimetag, state);
    if (status != OscPacketStatus::Accepted || state.messageCount == 0)
    {
        for (std::size_t index = 0; index < state.messageCount; ++index)
            output[index] = {};
        return {status == OscPacketStatus::Accepted
                    ? OscPacketStatus::Malformed
                    : status,
                0};
    }
    return {OscPacketStatus::Accepted, state.messageCount};
}

OscNtpClockAnchor makeOscNtpClockAnchor(
    std::int64_t unixNanoseconds,
    std::int64_t steadyNanoseconds) noexcept
{
    if (unixNanoseconds < 0 || steadyNanoseconds <= 0)
        return {};
    constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000;
    const auto unixSeconds = static_cast<std::uint64_t>(
        unixNanoseconds / nanosecondsPerSecond);
    if (unixSeconds
        > std::numeric_limits<std::uint32_t>::max()
            - oscNtpUnixEpochOffsetSeconds)
    {
        return {};
    }
    const auto unixFraction = static_cast<std::uint64_t>(
        unixNanoseconds % nanosecondsPerSecond);
    const auto fraction = (
        unixFraction * oscNtpFractionUnitsPerSecond
        + nanosecondsPerSecond / 2)
        / nanosecondsPerSecond;
    const auto ntpSeconds = unixSeconds + oscNtpUnixEpochOffsetSeconds;
    return {
        ntpSeconds * oscNtpFractionUnitsPerSecond + fraction,
        steadyNanoseconds,
    };
}

OscNtpConversionResult convertOscNtpTimetag(
    std::uint64_t timetag,
    OscNtpClockAnchor anchor) noexcept
{
    if (!anchor.valid())
        return {OscNtpConversionStatus::InvalidAnchor, 0};
    if (timetag == oscImmediateTimetag)
    {
        return {
            OscNtpConversionStatus::Immediate,
            anchor.steadyNanoseconds,
        };
    }

    const auto future = timetag >= anchor.timetag;
    const auto magnitude = future
        ? timetag - anchor.timetag
        : anchor.timetag - timetag;
    if (future && magnitude > maximumOscFutureTimetagDelta)
        return {OscNtpConversionStatus::TooFar, 0};
    if (!future && magnitude > maximumOscLateTimetagDelta)
        return {OscNtpConversionStatus::TooLate, 0};

    constexpr std::uint64_t nanosecondsPerSecond = 1'000'000'000;
    const auto seconds = magnitude >> 32;
    const auto fraction = magnitude & 0xffff'ffffULL;
    const auto fractionNanoseconds =
        (fraction * nanosecondsPerSecond
         + oscNtpFractionUnitsPerSecond / 2)
        / oscNtpFractionUnitsPerSecond;
    if (seconds > std::numeric_limits<std::uint64_t>::max()
            / nanosecondsPerSecond)
    {
        return {OscNtpConversionStatus::ArithmeticOverflow, 0};
    }
    const auto delta = seconds * nanosecondsPerSecond
        + fractionNanoseconds;
    if (delta > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
    {
        return {OscNtpConversionStatus::ArithmeticOverflow, 0};
    }

    const auto signedDelta = static_cast<std::int64_t>(delta);
    if ((future
         && anchor.steadyNanoseconds
             > std::numeric_limits<std::int64_t>::max() - signedDelta)
        || (!future && anchor.steadyNanoseconds < signedDelta))
    {
        return {OscNtpConversionStatus::ArithmeticOverflow, 0};
    }
    return {
        future
            ? OscNtpConversionStatus::Scheduled
            : OscNtpConversionStatus::Late,
        future
            ? anchor.steadyNanoseconds + signedDelta
            : anchor.steadyNanoseconds - signedDelta,
    };
}

OscPacketEncodeResult encodeOscControlMessage(
    std::string_view address,
    double normalizedValue,
    std::span<std::uint8_t> output) noexcept
{
    if (!talas::project::isValidOscAddress(address))
        return {OscPacketEncodeStatus::InvalidAddress, 0};
    if (!std::isfinite(normalizedValue)
        || normalizedValue < 0.0 || normalizedValue > 1.0)
    {
        return {OscPacketEncodeStatus::InvalidValue, 0};
    }

    const auto addressBytes = address.size() + 1;
    const auto paddedAddressBytes =
        (addressBytes + 3) & ~std::size_t{3};
    constexpr std::size_t typeBytes = 4;
    constexpr std::size_t payloadBytes = 8;
    const auto packetBytes = paddedAddressBytes + typeBytes + payloadBytes;
    if (output.size() < packetBytes)
        return {OscPacketEncodeStatus::OutputTooSmall, 0};

    std::fill_n(output.begin(), packetBytes, std::uint8_t{0});
    std::memcpy(output.data(), address.data(), address.size());
    output[paddedAddressBytes] = ',';
    output[paddedAddressBytes + 1] = 'd';
    writeBig64(
        std::bit_cast<std::uint64_t>(normalizedValue),
        std::span<std::uint8_t, 8>{
            output.data() + paddedAddressBytes + typeBytes,
            payloadBytes});
    return {OscPacketEncodeStatus::Encoded, packetBytes};
}
}
