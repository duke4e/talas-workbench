#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project::smalltalk
{
// Smalltalk's integers have infinite precision,
// as 1000 factorial shows, and a ratio of two integers is kept exactly, in
// its most reduced form. An integer of any size, its magnitude in 32-bit
// limbs, least significant first.
class LargeInteger
{
public:
    // The most bits a script's integer holds here.
    static constexpr std::size_t maximumBits = 131072;

    LargeInteger() = default;
    explicit LargeInteger(std::int64_t value);

    // The integer the digits give in the radix, 2 to 36, with no sign;
    // nothing for other characters or no digits.
    [[nodiscard]] static std::optional<LargeInteger> fromDigits(std::string_view digits, unsigned radix);
    // The integer nearest below or at a finite double, truncated toward 0.
    [[nodiscard]] static std::optional<LargeInteger> fromDouble(double value);

    [[nodiscard]] bool isZero() const noexcept { return limbs_.empty(); }
    [[nodiscard]] bool isNegative() const noexcept { return negative_; }
    [[nodiscard]] int sign() const noexcept { return isZero() ? 0 : negative_ ? -1 : 1; }
    [[nodiscard]] std::size_t bitLength() const noexcept;
    [[nodiscard]] bool isOdd() const noexcept { return !limbs_.empty() && (limbs_.front() & 1U) != 0; }

    [[nodiscard]] LargeInteger negated() const;
    [[nodiscard]] LargeInteger abs() const;
    [[nodiscard]] LargeInteger shiftedLeft(std::size_t bits) const;

    friend LargeInteger operator+(const LargeInteger& left, const LargeInteger& right);
    friend LargeInteger operator-(const LargeInteger& left, const LargeInteger& right);
    friend LargeInteger operator*(const LargeInteger& left, const LargeInteger& right);
    friend bool operator==(const LargeInteger& left, const LargeInteger& right) = default;

    // left = quotient * right + remainder, the quotient truncated toward 0;
    // right must not be 0.
    static void divide(const LargeInteger& left, const LargeInteger& right, LargeInteger& quotient,
        LargeInteger& remainder);
    [[nodiscard]] static int compare(const LargeInteger& left, const LargeInteger& right) noexcept;
    [[nodiscard]] static LargeInteger gcd(LargeInteger left, LargeInteger right);

    [[nodiscard]] std::optional<std::int64_t> toInt64() const noexcept;
    [[nodiscard]] double toDouble() const noexcept;
    [[nodiscard]] std::string toString(unsigned radix = 10) const;

private:
    void trim() noexcept;
    [[nodiscard]] static int compareMagnitude(const LargeInteger& left, const LargeInteger& right) noexcept;
    [[nodiscard]] static LargeInteger addMagnitudes(const LargeInteger& left, const LargeInteger& right);
    // left's magnitude less right's, left's being the larger.
    [[nodiscard]] static LargeInteger subtractMagnitudes(const LargeInteger& left, const LargeInteger& right);

    bool negative_ = false;
    std::vector<std::uint32_t> limbs_;
};

// An integer, or a ratio of two integers in its most reduced form with a
// positive denominator.
struct ExactNumber
{
    LargeInteger numerator;
    LargeInteger denominator{1};

    // The ratio, reduced; nothing for a denominator of 0.
    [[nodiscard]] static std::optional<ExactNumber> ratio(LargeInteger numerator, LargeInteger denominator);
    [[nodiscard]] static ExactNumber integer(LargeInteger value);

    [[nodiscard]] bool isInteger() const noexcept;
    [[nodiscard]] std::size_t bitLength() const noexcept;
    [[nodiscard]] double toDouble() const noexcept;
    // 1/3, or 7 for an integer; -1/3 for a negative ratio.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] ExactNumber negated() const;
    [[nodiscard]] ExactNumber abs() const;
    [[nodiscard]] static int compare(const ExactNumber& left, const ExactNumber& right);
    [[nodiscard]] static ExactNumber add(const ExactNumber& left, const ExactNumber& right);
    [[nodiscard]] static ExactNumber subtract(const ExactNumber& left, const ExactNumber& right);
    [[nodiscard]] static ExactNumber multiply(const ExactNumber& left, const ExactNumber& right);
    // Nothing for a divisor of 0.
    [[nodiscard]] static std::optional<ExactNumber> divide(const ExactNumber& left, const ExactNumber& right);
    // The integers nearest toward negative infinity, toward 0, toward
    // positive infinity, and nearest with halves away from 0.
    [[nodiscard]] LargeInteger floor() const;
    [[nodiscard]] LargeInteger truncated() const;
    [[nodiscard]] LargeInteger ceiling() const;
    [[nodiscard]] LargeInteger rounded() const;
};
}
