#pragma once

#include <charconv>

namespace talas
{
// Floating-point std::from_chars, which Apple's libc++ offers only from
// macOS 26. It reads what libc++'s std::from_chars reads and gives the same
// results, bit for bit:
//
// - The text must start with the number: no leading white space and no '+'.
//   A leading '-' negates.
// - general reads digits with an optional '.' (at least one digit in all) and
//   an optional exponent: 'e' or 'E', an optional sign and at least one digit.
//   An 'e' without such digits is not part of the number. fixed never reads an
//   exponent; scientific requires one. hex reads hexadecimal digits without a
//   "0x" prefix and an optional binary exponent ('p' or 'P', in decimal).
// - In every format, "inf" and "infinity" are infinity and "nan" or
//   "nan(chars)", chars being letters, digits and '_', is the quiet NaN with
//   the sign bit of a leading '-' and no payload; letters may be in any case.
// - ptr is the end of the longest prefix that matches. When nothing matches,
//   ec is errc::invalid_argument, ptr is first and value is left as it was.
// - The result is correctly rounded, ties to even. A finite number too large
//   for the type gives errc::result_out_of_range and writes infinity; a
//   nonzero number that rounds to zero gives errc::result_out_of_range and
//   writes zero, both signed as the text is. A subnormal result is in range.
// - One libc++ defect is not copied: in fixed format libc++ sometimes applies
//   an exponent after the digits that it did not consume.
//
// It does not change errno and allocates nothing itself. Decimal numbers are
// converted by the C library's strtod_l, given only digits and an exponent;
// on macOS that neither allocates nor locks. Hexadecimal numbers are rounded
// here.
std::from_chars_result fromChars(
    const char* first,
    const char* last,
    double& value,
    std::chars_format format = std::chars_format::general) noexcept;

std::from_chars_result fromChars(
    const char* first,
    const char* last,
    float& value,
    std::chars_format format = std::chars_format::general) noexcept;
}
