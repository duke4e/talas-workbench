#include "talas/project/SmalltalkExactNumber.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace talas::project::smalltalk
{
LargeInteger::LargeInteger(std::int64_t value)
{
    negative_ = value < 0;
    auto magnitude = negative_ ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    while (magnitude != 0)
    {
        limbs_.push_back(static_cast<std::uint32_t>(magnitude));
        magnitude >>= 32;
    }
}

std::optional<LargeInteger> LargeInteger::fromDigits(std::string_view digits, unsigned radix)
{
    if (digits.empty() || radix < 2 || radix > 36)
        return std::nullopt;
    LargeInteger result;
    for (const auto character : digits)
    {
        const auto digit = character >= '0' && character <= '9' ? static_cast<unsigned>(character - '0')
            : character >= 'A' && character <= 'Z'                ? static_cast<unsigned>(character - 'A' + 10)
            : character >= 'a' && character <= 'z'                ? static_cast<unsigned>(character - 'a' + 10)
                                                                  : 99U;
        if (digit >= radix)
            return std::nullopt;
        std::uint64_t carry = digit;
        for (auto& limb : result.limbs_)
        {
            const auto product = static_cast<std::uint64_t>(limb) * radix + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        if (carry != 0)
            result.limbs_.push_back(static_cast<std::uint32_t>(carry));
        if (result.bitLength() > maximumBits)
            return std::nullopt;
    }
    result.trim();
    return result;
}

std::optional<LargeInteger> LargeInteger::fromDouble(double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    value = std::trunc(value);
    if (std::abs(value) < 9.2e18)
        return LargeInteger{static_cast<std::int64_t>(value)};
    int exponent = 0;
    const auto fraction = std::frexp(std::abs(value), &exponent);
    const auto mantissa = static_cast<std::int64_t>(std::ldexp(fraction, 53));
    auto result = LargeInteger{mantissa}.shiftedLeft(static_cast<std::size_t>(exponent - 53));
    result.negative_ = value < 0.0;
    return result;
}

std::size_t LargeInteger::bitLength() const noexcept
{
    if (limbs_.empty())
        return 0;
    return limbs_.size() * 32 - static_cast<std::size_t>(std::countl_zero(limbs_.back()));
}

void LargeInteger::trim() noexcept
{
    while (!limbs_.empty() && limbs_.back() == 0)
        limbs_.pop_back();
    if (limbs_.empty())
        negative_ = false;
}

LargeInteger LargeInteger::negated() const
{
    auto result = *this;
    result.negative_ = !negative_ && !limbs_.empty();
    return result;
}

LargeInteger LargeInteger::abs() const
{
    auto result = *this;
    result.negative_ = false;
    return result;
}

LargeInteger LargeInteger::shiftedLeft(std::size_t bits) const
{
    if (limbs_.empty())
        return *this;
    LargeInteger result;
    result.negative_ = negative_;
    const auto whole = bits / 32;
    const auto part = static_cast<unsigned>(bits % 32);
    result.limbs_.assign(whole, 0U);
    std::uint32_t carry = 0;
    for (const auto limb : limbs_)
    {
        result.limbs_.push_back(part == 0 ? limb : (limb << part) | carry);
        carry = part == 0 ? 0U : limb >> (32 - part);
    }
    if (carry != 0)
        result.limbs_.push_back(carry);
    return result;
}

int LargeInteger::compareMagnitude(const LargeInteger& left, const LargeInteger& right) noexcept
{
    if (left.limbs_.size() != right.limbs_.size())
        return left.limbs_.size() < right.limbs_.size() ? -1 : 1;
    for (auto index = left.limbs_.size(); index-- > 0;)
    {
        if (left.limbs_[index] != right.limbs_[index])
            return left.limbs_[index] < right.limbs_[index] ? -1 : 1;
    }
    return 0;
}

int LargeInteger::compare(const LargeInteger& left, const LargeInteger& right) noexcept
{
    if (left.sign() != right.sign())
        return left.sign() < right.sign() ? -1 : 1;
    const auto magnitude = compareMagnitude(left, right);
    return left.negative_ ? -magnitude : magnitude;
}

LargeInteger LargeInteger::addMagnitudes(const LargeInteger& left, const LargeInteger& right)
{
    const auto& longer = left.limbs_.size() >= right.limbs_.size() ? left : right;
    const auto& shorter = left.limbs_.size() >= right.limbs_.size() ? right : left;
    LargeInteger result;
    result.limbs_.reserve(longer.limbs_.size() + 1);
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < longer.limbs_.size(); ++index)
    {
        const auto sum = static_cast<std::uint64_t>(longer.limbs_[index])
            + (index < shorter.limbs_.size() ? shorter.limbs_[index] : 0U) + carry;
        result.limbs_.push_back(static_cast<std::uint32_t>(sum));
        carry = sum >> 32;
    }
    if (carry != 0)
        result.limbs_.push_back(static_cast<std::uint32_t>(carry));
    return result;
}

LargeInteger LargeInteger::subtractMagnitudes(const LargeInteger& left, const LargeInteger& right)
{
    LargeInteger result;
    result.limbs_.reserve(left.limbs_.size());
    std::int64_t borrow = 0;
    for (std::size_t index = 0; index < left.limbs_.size(); ++index)
    {
        auto difference = static_cast<std::int64_t>(left.limbs_[index])
            - (index < right.limbs_.size() ? static_cast<std::int64_t>(right.limbs_[index]) : 0) - borrow;
        borrow = difference < 0 ? 1 : 0;
        if (difference < 0)
            difference += std::int64_t{1} << 32;
        result.limbs_.push_back(static_cast<std::uint32_t>(difference));
    }
    result.trim();
    return result;
}

LargeInteger operator+(const LargeInteger& left, const LargeInteger& right)
{
    if (left.negative_ == right.negative_)
    {
        auto sum = LargeInteger::addMagnitudes(left, right);
        sum.negative_ = left.negative_;
        sum.trim();
        return sum;
    }
    const auto magnitude = LargeInteger::compareMagnitude(left, right);
    if (magnitude == 0)
        return {};
    auto difference = magnitude > 0 ? LargeInteger::subtractMagnitudes(left, right)
                                    : LargeInteger::subtractMagnitudes(right, left);
    difference.negative_ = magnitude > 0 ? left.negative_ : right.negative_;
    difference.trim();
    return difference;
}

LargeInteger operator-(const LargeInteger& left, const LargeInteger& right)
{
    return left + right.negated();
}

LargeInteger operator*(const LargeInteger& left, const LargeInteger& right)
{
    if (left.isZero() || right.isZero())
        return {};
    LargeInteger result;
    result.limbs_.assign(left.limbs_.size() + right.limbs_.size(), 0U);
    for (std::size_t i = 0; i < left.limbs_.size(); ++i)
    {
        std::uint64_t carry = 0;
        const std::uint64_t factor = left.limbs_[i];
        for (std::size_t j = 0; j < right.limbs_.size(); ++j)
        {
            const auto product = factor * right.limbs_[j] + result.limbs_[i + j] + carry;
            result.limbs_[i + j] = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        result.limbs_[i + right.limbs_.size()] = static_cast<std::uint32_t>(carry);
    }
    result.negative_ = left.negative_ != right.negative_;
    result.trim();
    return result;
}

void LargeInteger::divide(const LargeInteger& left, const LargeInteger& right, LargeInteger& quotient,
    LargeInteger& remainder)
{
    if (compareMagnitude(left, right) < 0)
    {
        remainder = left;
        quotient = {};
        return;
    }
    LargeInteger q;
    LargeInteger r;
    if (right.limbs_.size() == 1)
    {
        const std::uint64_t divisor = right.limbs_.front();
        q.limbs_.assign(left.limbs_.size(), 0U);
        std::uint64_t rest = 0;
        for (auto index = left.limbs_.size(); index-- > 0;)
        {
            const auto current = (rest << 32) | left.limbs_[index];
            q.limbs_[index] = static_cast<std::uint32_t>(current / divisor);
            rest = current % divisor;
        }
        if (rest != 0)
            r.limbs_.push_back(static_cast<std::uint32_t>(rest));
    }
    else
    {
        // Knuth's Algorithm D, as Hacker's Delight writes it.
        const auto shift = static_cast<unsigned>(std::countl_zero(right.limbs_.back()));
        const auto n = right.limbs_.size();
        const auto m = left.limbs_.size() - n;
        std::vector<std::uint32_t> divisor(n);
        for (std::size_t i = n; i-- > 1;)
            divisor[i] = shift == 0 ? right.limbs_[i] : (right.limbs_[i] << shift) | (right.limbs_[i - 1] >> (32 - shift));
        divisor[0] = right.limbs_[0] << shift;
        std::vector<std::uint32_t> dividend(left.limbs_.size() + 1);
        dividend[left.limbs_.size()] = shift == 0 ? 0U : left.limbs_.back() >> (32 - shift);
        for (std::size_t i = left.limbs_.size(); i-- > 1;)
            dividend[i] = shift == 0 ? left.limbs_[i] : (left.limbs_[i] << shift) | (left.limbs_[i - 1] >> (32 - shift));
        dividend[0] = left.limbs_[0] << shift;
        constexpr std::uint64_t base = std::uint64_t{1} << 32;
        q.limbs_.assign(m + 1, 0U);
        for (std::size_t j = m + 1; j-- > 0;)
        {
            const auto numerator = (static_cast<std::uint64_t>(dividend[j + n]) << 32) | dividend[j + n - 1];
            auto estimate = numerator / divisor[n - 1];
            auto rest = numerator % divisor[n - 1];
            while (estimate >= base || estimate * divisor[n - 2] > ((rest << 32) | dividend[j + n - 2]))
            {
                --estimate;
                rest += divisor[n - 1];
                if (rest >= base)
                    break;
            }
            std::int64_t borrow = 0;
            std::int64_t difference = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto product = estimate * divisor[i];
                difference = static_cast<std::int64_t>(dividend[i + j]) - borrow
                    - static_cast<std::int64_t>(product & 0xFFFFFFFFULL);
                dividend[i + j] = static_cast<std::uint32_t>(difference);
                borrow = static_cast<std::int64_t>(product >> 32) - (difference >> 32);
            }
            difference = static_cast<std::int64_t>(dividend[j + n]) - borrow;
            dividend[j + n] = static_cast<std::uint32_t>(difference);
            if (difference < 0)
            {
                --estimate;
                std::uint64_t carry = 0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto sum = static_cast<std::uint64_t>(dividend[i + j]) + divisor[i] + carry;
                    dividend[i + j] = static_cast<std::uint32_t>(sum);
                    carry = sum >> 32;
                }
                dividend[j + n] = static_cast<std::uint32_t>(dividend[j + n] + carry);
            }
            q.limbs_[j] = static_cast<std::uint32_t>(estimate);
        }
        r.limbs_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            r.limbs_[i] = shift == 0 ? dividend[i] : (dividend[i] >> shift) | (dividend[i + 1] << (32 - shift));
    }
    q.negative_ = left.negative_ != right.negative_;
    r.negative_ = left.negative_;
    q.trim();
    r.trim();
    quotient = std::move(q);
    remainder = std::move(r);
}

LargeInteger LargeInteger::gcd(LargeInteger left, LargeInteger right)
{
    left = left.abs();
    right = right.abs();
    while (!right.isZero())
    {
        LargeInteger quotient;
        LargeInteger remainder;
        divide(left, right, quotient, remainder);
        left = std::move(right);
        right = std::move(remainder);
    }
    return left;
}

std::optional<std::int64_t> LargeInteger::toInt64() const noexcept
{
    if (bitLength() > 63)
        return std::nullopt;
    std::uint64_t magnitude = 0;
    for (auto index = limbs_.size(); index-- > 0;)
        magnitude = (magnitude << 32) | limbs_[index];
    return negative_ ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
}

double LargeInteger::toDouble() const noexcept
{
    const auto bits = bitLength();
    if (bits <= 64)
    {
        std::uint64_t magnitude = 0;
        for (auto index = limbs_.size(); index-- > 0;)
            magnitude = (magnitude << 32) | limbs_[index];
        const auto value = static_cast<double>(magnitude);
        return negative_ ? -value : value;
    }
    if (bits > 1100)
        return negative_ ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    // The top 64 bits, the lowest of them kept set when any bit below is,
    // so that the conversion rounds as the whole number would.
    const auto shift = bits - 64;
    std::uint64_t top = 0;
    for (std::size_t bit = 0; bit < 64; ++bit)
    {
        const auto at = shift + bit;
        if (((limbs_[at / 32] >> (at % 32)) & 1U) != 0)
            top |= std::uint64_t{1} << bit;
    }
    auto below = false;
    for (std::size_t at = 0; at < shift && !below; ++at)
        below = ((limbs_[at / 32] >> (at % 32)) & 1U) != 0;
    if (below)
        top |= 1U;
    const auto value = std::ldexp(static_cast<double>(top), static_cast<int>(shift));
    return negative_ ? -value : value;
}

std::string LargeInteger::toString(unsigned radix) const
{
    if (limbs_.empty())
        return "0";
    radix = std::clamp(radix, 2U, 36U);
    std::string digits;
    auto rest = limbs_;
    while (!rest.empty())
    {
        std::uint64_t remainder = 0;
        for (auto index = rest.size(); index-- > 0;)
        {
            const auto current = (remainder << 32) | rest[index];
            rest[index] = static_cast<std::uint32_t>(current / radix);
            remainder = current % radix;
        }
        while (!rest.empty() && rest.back() == 0)
            rest.pop_back();
        digits.push_back(static_cast<char>(remainder < 10 ? '0' + remainder : 'A' + remainder - 10));
    }
    if (negative_)
        digits.push_back('-');
    std::ranges::reverse(digits);
    return digits;
}

std::optional<ExactNumber> ExactNumber::ratio(LargeInteger numerator, LargeInteger denominator)
{
    if (denominator.isZero())
        return std::nullopt;
    if (denominator.isNegative())
    {
        numerator = numerator.negated();
        denominator = denominator.negated();
    }
    const auto divisor = LargeInteger::gcd(numerator, denominator);
    if (!(divisor == LargeInteger{1}))
    {
        LargeInteger rest;
        LargeInteger::divide(numerator, divisor, numerator, rest);
        LargeInteger::divide(denominator, divisor, denominator, rest);
    }
    return ExactNumber{std::move(numerator), std::move(denominator)};
}

ExactNumber ExactNumber::integer(LargeInteger value)
{
    return ExactNumber{std::move(value), LargeInteger{1}};
}

bool ExactNumber::isInteger() const noexcept
{
    return denominator == LargeInteger{1};
}

std::size_t ExactNumber::bitLength() const noexcept
{
    return std::max(numerator.bitLength(), denominator.bitLength());
}

double ExactNumber::toDouble() const noexcept
{
    if (isInteger())
        return numerator.toDouble();
    if (numerator.bitLength() <= 53 && denominator.bitLength() <= 53)
        return numerator.toDouble() / denominator.toDouble();
    // A quotient of at least 65 bits, a bit set below it when the division
    // leaves a remainder, scaled back.
    const auto scale = 66 + static_cast<long>(denominator.bitLength()) - static_cast<long>(numerator.bitLength());
    auto dividend = numerator.abs();
    auto divisor = denominator;
    if (scale > 0)
        dividend = dividend.shiftedLeft(static_cast<std::size_t>(scale));
    else
        divisor = divisor.shiftedLeft(static_cast<std::size_t>(-scale));
    LargeInteger quotient;
    LargeInteger remainder;
    LargeInteger::divide(dividend, divisor, quotient, remainder);
    auto exponent = -scale;
    if (!remainder.isZero())
    {
        quotient = quotient.shiftedLeft(1) + LargeInteger{1};
        --exponent;
    }
    const auto value = std::ldexp(quotient.toDouble(), static_cast<int>(exponent));
    return numerator.isNegative() ? -value : value;
}

std::string ExactNumber::toString() const
{
    return isInteger() ? numerator.toString() : numerator.toString() + "/" + denominator.toString();
}

ExactNumber ExactNumber::negated() const
{
    return ExactNumber{numerator.negated(), denominator};
}

ExactNumber ExactNumber::abs() const
{
    return ExactNumber{numerator.abs(), denominator};
}

int ExactNumber::compare(const ExactNumber& left, const ExactNumber& right)
{
    if (left.isInteger() && right.isInteger())
        return LargeInteger::compare(left.numerator, right.numerator);
    return LargeInteger::compare(left.numerator * right.denominator, right.numerator * left.denominator);
}

ExactNumber ExactNumber::add(const ExactNumber& left, const ExactNumber& right)
{
    if (left.isInteger() && right.isInteger())
        return integer(left.numerator + right.numerator);
    return *ratio(left.numerator * right.denominator + right.numerator * left.denominator,
        left.denominator * right.denominator);
}

ExactNumber ExactNumber::subtract(const ExactNumber& left, const ExactNumber& right)
{
    return add(left, right.negated());
}

ExactNumber ExactNumber::multiply(const ExactNumber& left, const ExactNumber& right)
{
    if (left.isInteger() && right.isInteger())
        return integer(left.numerator * right.numerator);
    return *ratio(left.numerator * right.numerator, left.denominator * right.denominator);
}

std::optional<ExactNumber> ExactNumber::divide(const ExactNumber& left, const ExactNumber& right)
{
    if (right.numerator.isZero())
        return std::nullopt;
    return ratio(left.numerator * right.denominator, left.denominator * right.numerator);
}

LargeInteger ExactNumber::truncated() const
{
    LargeInteger quotient;
    LargeInteger remainder;
    LargeInteger::divide(numerator, denominator, quotient, remainder);
    return quotient;
}

LargeInteger ExactNumber::floor() const
{
    LargeInteger quotient;
    LargeInteger remainder;
    LargeInteger::divide(numerator, denominator, quotient, remainder);
    return numerator.isNegative() && !remainder.isZero() ? quotient - LargeInteger{1} : quotient;
}

LargeInteger ExactNumber::ceiling() const
{
    LargeInteger quotient;
    LargeInteger remainder;
    LargeInteger::divide(numerator, denominator, quotient, remainder);
    return !numerator.isNegative() && !remainder.isZero() ? quotient + LargeInteger{1} : quotient;
}

LargeInteger ExactNumber::rounded() const
{
    // (2 |n| + d) // 2d, with the sign of n.
    LargeInteger quotient;
    LargeInteger remainder;
    LargeInteger::divide(numerator.abs().shiftedLeft(1) + denominator, denominator.shiftedLeft(1), quotient, remainder);
    return numerator.isNegative() ? quotient.negated() : quotient;
}
}
