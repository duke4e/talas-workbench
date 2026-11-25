#include "talas/runtime/MtcSmpteProtocol.h"

#include <limits>
#include <type_traits>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    output = left * right;
    return true;
}

[[nodiscard]] bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    output = left + right;
    return true;
}

[[nodiscard]] bool rateFromMtcCode(
    std::uint8_t code,
    SmpteRate& output) noexcept
{
    switch (code)
    {
    case 0:
        output = SmpteRate::Fps24;
        return true;
    case 1:
        output = SmpteRate::Fps25;
        return true;
    case 2:
        output = SmpteRate::Fps2997Drop;
        return true;
    case 3:
        output = SmpteRate::Fps30;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool labelToAbsoluteFrame(
    SmpteRate rate,
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    std::uint64_t& output) noexcept
{
    const auto profile = smpteRateRational(rate);
    if (profile.numerator == 0 || hour > 23 || minute > 59
        || second > 59 || frame >= profile.nominalFramesPerSecond)
    {
        return false;
    }
    if (profile.dropFrame && minute % 10 != 0 && second == 0
        && frame < 2)
    {
        return false;
    }

    const auto totalSeconds = static_cast<std::uint64_t>(hour) * 3600
        + static_cast<std::uint64_t>(minute) * 60 + second;
    auto absolute = totalSeconds * profile.nominalFramesPerSecond + frame;
    if (profile.dropFrame)
    {
        const auto totalMinutes = static_cast<std::uint64_t>(hour) * 60
            + minute;
        const auto dropped = 2 * (totalMinutes - totalMinutes / 10);
        if (absolute < dropped)
            return false;
        absolute -= dropped;
    }
    output = absolute;
    return absolute < smpteFramesPer24Hours(rate);
}
}

SmpteRateRational smpteRateRational(SmpteRate rate) noexcept
{
    switch (rate)
    {
    case SmpteRate::Fps24:
        return {24, 1, 24, false};
    case SmpteRate::Fps25:
        return {25, 1, 25, false};
    case SmpteRate::Fps2997Drop:
        return {30'000, 1'001, 30, true};
    case SmpteRate::Fps30:
        return {30, 1, 30, false};
    default:
        return {};
    }
}

std::uint64_t smpteFramesPer24Hours(SmpteRate rate) noexcept
{
    switch (rate)
    {
    case SmpteRate::Fps24:
        return 2'073'600;
    case SmpteRate::Fps25:
        return 2'160'000;
    case SmpteRate::Fps2997Drop:
        return 2'589'408;
    case SmpteRate::Fps30:
        return 2'592'000;
    default:
        return 0;
    }
}

bool makeSmpteTimeCode(
    SmpteRate rate,
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    SmpteDirection direction,
    SmpteTimeCode& output) noexcept
{
    if (direction != SmpteDirection::Forward
        && direction != SmpteDirection::Reverse)
    {
        return false;
    }
    std::uint64_t absoluteFrame = 0;
    if (!labelToAbsoluteFrame(
            rate, hour, minute, second, frame, absoluteFrame))
    {
        return false;
    }
    const auto profile = smpteRateRational(rate);
    output = {
        .rate = rate,
        .dropFrame = profile.dropFrame,
        .hour = hour,
        .minute = minute,
        .second = second,
        .frame = frame,
        .direction = direction,
        .absoluteFrame = absoluteFrame,
    };
    return true;
}

bool isValidSmpteTimeCode(const SmpteTimeCode& value) noexcept
{
    SmpteTimeCode canonical;
    return makeSmpteTimeCode(
               value.rate,
               value.hour,
               value.minute,
               value.second,
               value.frame,
               value.direction,
               canonical)
        && canonical.dropFrame == value.dropFrame
        && canonical.absoluteFrame == value.absoluteFrame;
}

bool smpteTimeCodeFromAbsoluteFrame(
    SmpteRate rate,
    std::uint64_t absoluteFrame,
    SmpteDirection direction,
    SmpteTimeCode& output) noexcept
{
    const auto frameLimit = smpteFramesPer24Hours(rate);
    const auto profile = smpteRateRational(rate);
    if (frameLimit == 0 || absoluteFrame >= frameLimit
        || profile.nominalFramesPerSecond == 0
        || (direction != SmpteDirection::Forward
            && direction != SmpteDirection::Reverse))
    {
        return false;
    }

    auto labelFrame = absoluteFrame;
    if (profile.dropFrame)
    {
        constexpr std::uint64_t framesPerTenMinutes = 17'982;
        constexpr std::uint64_t framesPerDropMinute = 1'798;
        const auto tenMinuteBlocks = absoluteFrame / framesPerTenMinutes;
        const auto remainder = absoluteFrame % framesPerTenMinutes;
        const auto droppedLabels = 18 * tenMinuteBlocks
            + (remainder > 1
                    ? 2 * ((remainder - 2) / framesPerDropMinute)
                    : 0);
        labelFrame += droppedLabels;
    }

    const auto framesPerSecond = profile.nominalFramesPerSecond;
    const auto totalSeconds = labelFrame / framesPerSecond;
    const auto frame = static_cast<std::uint8_t>(
        labelFrame % framesPerSecond);
    const auto second = static_cast<std::uint8_t>(totalSeconds % 60);
    const auto totalMinutes = totalSeconds / 60;
    const auto minute = static_cast<std::uint8_t>(totalMinutes % 60);
    const auto hour = static_cast<std::uint8_t>((totalMinutes / 60) % 24);
    return makeSmpteTimeCode(
        rate, hour, minute, second, frame, direction, output);
}

bool addSmpteFrames(
    const SmpteTimeCode& source,
    std::uint64_t frames,
    SmpteTimeCode& output,
    bool& wrapped) noexcept
{
    if (!isValidSmpteTimeCode(source))
        return false;
    const auto frameLimit = smpteFramesPer24Hours(source.rate);
    if (frameLimit == 0)
        return false;
    const auto normalizedFrames = frames % frameLimit;
    const auto sum = source.absoluteFrame + normalizedFrames;
    const auto crossedBoundary = sum >= frameLimit;
    wrapped = frames >= frameLimit || crossedBoundary;
    const auto absolute = crossedBoundary ? sum - frameLimit : sum;
    return smpteTimeCodeFromAbsoluteFrame(
        source.rate, absolute, source.direction, output);
}

bool smpteTimeCodeToSampleFrame(
    const SmpteTimeCode& source,
    std::uint32_t sampleRate,
    std::uint64_t& output) noexcept
{
    if (!isValidSmpteTimeCode(source) || sampleRate == 0)
        return false;
    const auto profile = smpteRateRational(source.rate);
    std::uint64_t scaled = 0;
    if (!checkedMultiply(source.absoluteFrame, sampleRate, scaled)
        || !checkedMultiply(scaled, profile.denominator, scaled))
    {
        return false;
    }
    const auto rounding = profile.numerator / smpteRoundToNearestDivisor;
    if (!checkedAdd(scaled, rounding, scaled))
        return false;
    output = scaled / profile.numerator;
    return true;
}

MtcDecodeResult decodeMtcQuarterFramePiece(
    std::span<const std::uint8_t> bytes,
    MtcQuarterFramePiece& output) noexcept
{
    if (bytes.size() != 2 || bytes[0] != 0xF1 || bytes[1] > 0x7F)
        return MtcDecodeResult::InvalidShape;
    const auto type = static_cast<std::uint8_t>(
        (bytes[1] >> 4) & 0x07);
    const auto value = static_cast<std::uint8_t>(bytes[1] & 0x0F);
    const auto reservedBitsSet =
        (type == 1 && value > 0x01)
        || ((type == 3 || type == 5) && value > 0x03)
        || (type == 7 && value > 0x07);
    if (reservedBitsSet)
        return MtcDecodeResult::InvalidPiece;
    output = {.type = type, .value = value};
    return MtcDecodeResult::Accepted;
}

MtcDecodeResult decodeMtcQuarterFrameSet(
    std::span<const MtcQuarterFramePiece> pieces,
    SmpteDirection direction,
    bool compensateForwardTransport,
    SmpteTimeCode& output,
    bool& wrapped) noexcept
{
    wrapped = false;
    if (pieces.size() != mtcQuarterFramePieceCount)
        return MtcDecodeResult::InvalidShape;
    if (direction != SmpteDirection::Forward)
        return MtcDecodeResult::UnsupportedDirection;
    for (std::uint8_t index = 0; index < mtcQuarterFramePieceCount; ++index)
    {
        const auto reservedBitsSet =
            (index == 1 && pieces[index].value > 0x01)
            || ((index == 3 || index == 5)
                && pieces[index].value > 0x03)
            || (index == 7 && pieces[index].value > 0x07);
        if (pieces[index].type != index
            || pieces[index].value > 0x0F
            || reservedBitsSet)
            return MtcDecodeResult::InvalidPiece;
    }

    const auto frame = static_cast<std::uint8_t>(
        pieces[0].value | ((pieces[1].value & 0x01) << 4));
    const auto second = static_cast<std::uint8_t>(
        pieces[2].value | ((pieces[3].value & 0x03) << 4));
    const auto minute = static_cast<std::uint8_t>(
        pieces[4].value | ((pieces[5].value & 0x03) << 4));
    const auto hour = static_cast<std::uint8_t>(
        pieces[6].value | ((pieces[7].value & 0x01) << 4));
    SmpteRate rate;
    if (!rateFromMtcCode(
            static_cast<std::uint8_t>((pieces[7].value >> 1) & 0x03),
            rate))
    {
        return MtcDecodeResult::InvalidRate;
    }
    SmpteTimeCode decoded;
    if (!makeSmpteTimeCode(
            rate, hour, minute, second, frame, direction, decoded))
    {
        return MtcDecodeResult::InvalidLabel;
    }
    if (!compensateForwardTransport)
    {
        output = decoded;
        return MtcDecodeResult::Accepted;
    }
    if (!addSmpteFrames(
            decoded, mtcForwardCompensationFrames, output, wrapped))
    {
        return MtcDecodeResult::ArithmeticOverflow;
    }
    return MtcDecodeResult::Accepted;
}

MtcDecodeResult decodeMtcFullFrame(
    std::span<const std::uint8_t> bytes,
    MtcFullFrameMessage& output) noexcept
{
    if (bytes.size() != 10 || bytes[0] != 0xF0 || bytes[1] != 0x7F
        || bytes[2] > 0x7F || bytes[3] != 0x01 || bytes[4] != 0x01
        || bytes[5] > 0x7F || bytes[6] > 0x7F || bytes[7] > 0x7F
        || bytes[8] > 0x7F || bytes[9] != 0xF7)
    {
        return MtcDecodeResult::InvalidShape;
    }
    SmpteRate rate;
    if (!rateFromMtcCode(
            static_cast<std::uint8_t>((bytes[5] >> 5) & 0x03),
            rate))
    {
        return MtcDecodeResult::InvalidRate;
    }
    SmpteTimeCode decoded;
    if (!makeSmpteTimeCode(
            rate,
            static_cast<std::uint8_t>(bytes[5] & 0x1F),
            bytes[6],
            bytes[7],
            bytes[8],
            SmpteDirection::Forward,
            decoded))
    {
        return MtcDecodeResult::InvalidLabel;
    }
    output = {.deviceId = bytes[2], .timeCode = decoded};
    return MtcDecodeResult::Accepted;
}

static_assert(std::is_trivially_copyable_v<SmpteTimeCode>);
static_assert(std::is_trivially_copyable_v<MtcQuarterFramePiece>);
static_assert(std::is_trivially_copyable_v<MtcFullFrameMessage>);
}
