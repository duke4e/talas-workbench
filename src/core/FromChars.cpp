#include "talas/core/FromChars.h"

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <locale.h>
#include <stdlib.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#include <xlocale.h>
#else
#include <locale.h>
#include <stdlib.h>
#endif

namespace talas
{
namespace
{
template <typename Float>
struct FloatTraits;

template <>
struct FloatTraits<double>
{
    using Bits = std::uint64_t;
    static constexpr int significandBits = 53;
    static constexpr int minExponent = -1022;
    static constexpr int maxExponent = 1023;
};

template <>
struct FloatTraits<float>
{
    using Bits = std::uint32_t;
    static constexpr int significandBits = 24;
    static constexpr int minExponent = -126;
    static constexpr int maxExponent = 127;
};

template <typename Float>
constexpr typename FloatTraits<Float>::Bits signBit =
    typename FloatTraits<Float>::Bits{1} << (sizeof(Float) * 8 - 1);

template <typename Float>
constexpr typename FloatTraits<Float>::Bits infinityBits =
    std::bit_cast<typename FloatTraits<Float>::Bits>(std::numeric_limits<Float>::infinity());

template <typename Float>
Float withSign(typename FloatTraits<Float>::Bits magnitude, bool negative) noexcept
{
    return std::bit_cast<Float>(negative ? magnitude | signBit<Float> : magnitude);
}

constexpr bool isDigit(char character) noexcept
{
    return character >= '0' && character <= '9';
}

constexpr int hexDigitValue(char character) noexcept
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

constexpr bool isNanCharacter(char character) noexcept
{
    return isDigit(character) || (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z') || character == '_';
}

// Whether [first, last) starts with lowerCaseWord, in any case.
bool startsWithWord(const char* first, const char* last, std::string_view lowerCaseWord) noexcept
{
    if (last - first < static_cast<std::ptrdiff_t>(lowerCaseWord.size()))
        return false;
    for (const char expected : lowerCaseWord)
    {
        auto character = *first++;
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
        if (character != expected)
            return false;
    }
    return true;
}

// Exponents saturate here. A number's digits move its exponent by at most
// their count, so any text that fits in memory keeps its value.
constexpr std::int64_t exponentLimit = 1'000'000'000'000'000;

// An exponent's optional sign and its digits, starting after its 'e' or 'p'.
// Returns the end of the digits, or nullptr when no digit follows.
const char* scanExponent(const char* position, const char* last, std::int64_t& exponent) noexcept
{
    auto negative = false;
    if (position != last && (*position == '+' || *position == '-'))
    {
        negative = *position == '-';
        ++position;
    }
    if (position == last || !isDigit(*position))
        return nullptr;
    std::int64_t magnitude = 0;
    for (; position != last && isDigit(*position); ++position)
    {
        if (magnitude < exponentLimit)
            magnitude = magnitude * 10 + (*position - '0');
    }
    exponent = negative ? -magnitude : magnitude;
    return position;
}

// The C library's strtod_l, correctly rounded, converts decimal digits. The
// text it gets holds only digits, an 'e' and a '-', so no locale can change
// it. macOS's strtod_l locks only to read a decimal point or a hexadecimal
// number, neither of which this text has, and allocates nothing.
#if defined(_WIN32)
_locale_t cLocale() noexcept
{
    static const _locale_t locale = _create_locale(LC_ALL, "C");
    return locale;
}

double convert(const char* text, char** end, double) noexcept
{
    return _strtod_l(text, end, cLocale());
}

float convert(const char* text, char** end, float) noexcept
{
    return _strtof_l(text, end, cLocale());
}
#elif defined(__APPLE__)
// xlocale(3): the _l functions read a null locale_t as the C locale.
double convert(const char* text, char** end, double) noexcept
{
    return strtod_l(text, end, nullptr);
}

float convert(const char* text, char** end, float) noexcept
{
    return strtof_l(text, end, nullptr);
}
#else
locale_t cLocale() noexcept
{
    static const locale_t locale = newlocale(LC_ALL_MASK, "C", locale_t{});
    return locale;
}

double convert(const char* text, char** end, double) noexcept
{
    const auto locale = cLocale();
    return locale != locale_t{} ? strtod_l(text, end, locale) : strtod(text, end);
}

float convert(const char* text, char** end, float) noexcept
{
    const auto locale = cLocale();
    return locale != locale_t{} ? strtof_l(text, end, locale) : strtof(text, end);
}
#endif

template <typename Float>
std::from_chars_result finish(
    Float result, bool negative, const char* end, Float& value) noexcept
{
    value = negative ? -result : result;
    return {end, std::errc{}};
}

template <typename Float>
std::from_chars_result outOfRange(
    bool overflow, bool negative, const char* end, Float& value) noexcept
{
    value = withSign<Float>(overflow ? infinityBits<Float> : 0, negative);
    return {end, std::errc::result_out_of_range};
}

// The most significant decimal digits the conversion sees. A double halfway
// between two neighbours has at most 767 significant digits, so a number cut
// to 800 digits, with a nonzero digit standing for any nonzero digits cut
// off, rounds as the whole number does.
constexpr std::size_t maxSignificantDigits = 800;
// Beyond this decimal exponent every number of up to 801 digits is out of
// range, so clamping to it changes no result.
constexpr std::int64_t maxScale = 99'999;

template <typename Float>
std::from_chars_result parseDecimal(
    const char* first,
    const char* last,
    const char* digitsBegin,
    bool negative,
    std::chars_format format,
    Float& value) noexcept
{
    const auto scientific =
        (static_cast<unsigned>(format) & static_cast<unsigned>(std::chars_format::scientific)) != 0;
    const auto fixed =
        (static_cast<unsigned>(format) & static_cast<unsigned>(std::chars_format::fixed)) != 0;

    const auto* position = digitsBegin;
    while (position != last && isDigit(*position))
        ++position;
    const auto* const integerEnd = position;
    const auto* fractionBegin = integerEnd;
    const auto* fractionEnd = integerEnd;
    if (position != last && *position == '.')
    {
        fractionBegin = position + 1;
        fractionEnd = fractionBegin;
        while (fractionEnd != last && isDigit(*fractionEnd))
            ++fractionEnd;
    }
    if (integerEnd == digitsBegin && fractionEnd == fractionBegin)
        return {first, std::errc::invalid_argument};
    const auto* end = fractionEnd;

    std::int64_t exponent = 0;
    if (scientific && end != last && (*end == 'e' || *end == 'E'))
    {
        if (const auto* exponentEnd = scanExponent(end + 1, last, exponent))
            end = exponentEnd;
        else if (!fixed)
            return {first, std::errc::invalid_argument};
    }
    else if (scientific && !fixed)
    {
        return {first, std::errc::invalid_argument};
    }

    // value = digits * 10^scale, digits being the significant digits as an
    // integer.
    std::array<char, maxSignificantDigits + 16> text{};
    std::size_t length = 0;
    std::int64_t index = 0;
    std::int64_t lastKept = 0;
    auto started = false;
    auto cutNonzero = false;
    const auto take = [&](char digit) noexcept
    {
        ++index;
        if (!started && digit == '0')
            return;
        started = true;
        if (length < maxSignificantDigits)
        {
            text[length++] = digit;
            lastKept = index;
        }
        else if (digit != '0')
        {
            cutNonzero = true;
        }
    };
    for (const auto* digit = digitsBegin; digit != integerEnd; ++digit)
        take(*digit);
    for (const auto* digit = fractionBegin; digit != fractionEnd; ++digit)
        take(*digit);
    if (!started)
        return finish(Float{0}, negative, end, value);

    auto scale = exponent + static_cast<std::int64_t>(integerEnd - digitsBegin) - lastKept;
    if (cutNonzero)
    {
        text[length++] = '1';
        --scale;
    }
    else
    {
        while (text[length - 1] == '0')
        {
            --length;
            ++scale;
        }
    }
    scale = scale < -maxScale ? -maxScale : scale > maxScale ? maxScale : scale;

    text[length++] = 'e';
    if (scale < 0)
    {
        text[length++] = '-';
        scale = -scale;
    }
    std::array<char, 8> exponentDigits{};
    std::size_t exponentLength = 0;
    do
    {
        exponentDigits[exponentLength++] = static_cast<char>('0' + scale % 10);
        scale /= 10;
    } while (scale != 0);
    while (exponentLength != 0)
        text[length++] = exponentDigits[--exponentLength];
    text[length] = '\0';

    const auto savedErrno = errno;
    char* convertedEnd = nullptr;
    const auto result = convert(text.data(), &convertedEnd, Float{});
    errno = savedErrno;
    if (convertedEnd != text.data() + length)
        return {first, std::errc::invalid_argument};

    if (result == std::numeric_limits<Float>::infinity())
        return outOfRange(true, negative, end, value);
    if (result == Float{0})
        return outOfRange(false, negative, end, value);
    return finish(result, negative, end, value);
}

// significand * 2^exponent, with sticky set when nonzero bits below the
// significand were dropped, rounded to Float, ties to even.
template <typename Float>
std::from_chars_result assembleBinary(
    std::uint64_t significand,
    std::int64_t exponent,
    bool sticky,
    bool negative,
    const char* end,
    Float& value) noexcept
{
    using Traits = FloatTraits<Float>;
    const auto leadingZeros = std::countl_zero(significand);
    significand <<= leadingZeros;
    // The number now lies in [2^top, 2^(top + 1)).
    const auto top = exponent - leadingZeros + 63;
    if (top > Traits::maxExponent)
        return outOfRange(true, negative, end, value);

    std::int64_t shift = 64 - Traits::significandBits;
    if (top < Traits::minExponent)
        shift += Traits::minExponent - top;
    std::uint64_t kept = 0;
    if (shift == 64)
    {
        // Only the rounding bit is left: the number is at least half the
        // smallest subnormal.
        constexpr auto half = std::uint64_t{1} << 63;
        kept = significand > half || (significand == half && sticky) ? 1 : 0;
    }
    else if (shift < 64)
    {
        kept = significand >> shift;
        const auto remainder = significand & ((std::uint64_t{1} << shift) - 1);
        const auto half = std::uint64_t{1} << (shift - 1);
        if (remainder > half || (remainder == half && (sticky || (kept & 1) != 0)))
            ++kept;
    }
    // A shift past 64 leaves less than half the smallest subnormal: zero.

    // kept carries the implicit bit, so adding it to the exponent field
    // also carries a rounded-up significand into the exponent.
    auto bits = kept;
    if (top >= Traits::minExponent)
        bits += static_cast<std::uint64_t>(top - Traits::minExponent) << (Traits::significandBits - 1);
    if (bits >= infinityBits<Float>)
        return outOfRange(true, negative, end, value);
    if (bits == 0)
        return outOfRange(false, negative, end, value);
    value = withSign<Float>(static_cast<typename Traits::Bits>(bits), negative);
    return {end, std::errc{}};
}

template <typename Float>
std::from_chars_result parseHex(
    const char* first,
    const char* last,
    const char* digitsBegin,
    bool negative,
    Float& value) noexcept
{
    const auto* position = digitsBegin;
    while (position != last && hexDigitValue(*position) >= 0)
        ++position;
    const auto* const integerEnd = position;
    const auto* fractionBegin = integerEnd;
    const auto* fractionEnd = integerEnd;
    if (position != last && *position == '.')
    {
        fractionBegin = position + 1;
        fractionEnd = fractionBegin;
        while (fractionEnd != last && hexDigitValue(*fractionEnd) >= 0)
            ++fractionEnd;
    }
    if (integerEnd == digitsBegin && fractionEnd == fractionBegin)
        return {first, std::errc::invalid_argument};
    const auto* end = fractionEnd;

    std::int64_t exponent = 0;
    if (end != last && (*end == 'p' || *end == 'P'))
    {
        if (const auto* exponentEnd = scanExponent(end + 1, last, exponent))
            end = exponentEnd;
    }

    // Sixteen significant hexadecimal digits hold at least 61 significant
    // bits, more than rounding a double needs.
    std::uint64_t significand = 0;
    int keptDigits = 0;
    std::int64_t index = 0;
    std::int64_t lastKept = 0;
    auto sticky = false;
    const auto take = [&](char digit) noexcept
    {
        ++index;
        const auto digitValue = static_cast<std::uint64_t>(hexDigitValue(digit));
        if (keptDigits == 0 && digitValue == 0)
            return;
        if (keptDigits < 16)
        {
            significand = significand << 4 | digitValue;
            ++keptDigits;
            lastKept = index;
        }
        else if (digitValue != 0)
        {
            sticky = true;
        }
    };
    for (const auto* digit = digitsBegin; digit != integerEnd; ++digit)
        take(*digit);
    for (const auto* digit = fractionBegin; digit != fractionEnd; ++digit)
        take(*digit);
    if (keptDigits == 0)
        return finish(Float{0}, negative, end, value);

    const auto binaryExponent =
        exponent + 4 * (static_cast<std::int64_t>(integerEnd - digitsBegin) - lastKept);
    return assembleBinary(significand, binaryExponent, sticky, negative, end, value);
}

template <typename Float>
std::from_chars_result parse(
    const char* first, const char* last, Float& value, std::chars_format format) noexcept
{
    if (first == nullptr || last == nullptr || first >= last)
        return {first, std::errc::invalid_argument};
    auto position = first;
    const auto negative = *position == '-';
    if (negative)
        ++position;

    if (startsWithWord(position, last, "inf"))
    {
        const auto* end = startsWithWord(position, last, "infinity") ? position + 8 : position + 3;
        value = withSign<Float>(infinityBits<Float>, negative);
        return {end, std::errc{}};
    }
    if (startsWithWord(position, last, "nan"))
    {
        const auto* end = position + 3;
        if (end != last && *end == '(')
        {
            for (const auto* character = end + 1; character != last; ++character)
            {
                if (*character == ')')
                {
                    end = character + 1;
                    break;
                }
                if (!isNanCharacter(*character))
                    break;
            }
        }
        // The quiet NaN: the exponent's bits and the significand's top bit.
        using Bits = typename FloatTraits<Float>::Bits;
        constexpr auto quietBit = Bits{1} << (FloatTraits<Float>::significandBits - 2);
        value = withSign<Float>(infinityBits<Float> | quietBit, negative);
        return {end, std::errc{}};
    }

    if (format == std::chars_format::hex)
        return parseHex(first, last, position, negative, value);
    return parseDecimal(first, last, position, negative, format, value);
}
}

std::from_chars_result fromChars(
    const char* first, const char* last, double& value, std::chars_format format) noexcept
{
    return parse(first, last, value, format);
}

std::from_chars_result fromChars(
    const char* first, const char* last, float& value, std::chars_format format) noexcept
{
    return parse(first, last, value, format);
}
}
