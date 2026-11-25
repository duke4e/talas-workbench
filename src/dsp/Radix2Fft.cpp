#include "talas/dsp/Radix2Fft.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] std::size_t reversedBits(
    std::size_t value,
    unsigned bitCount) noexcept
{
    std::size_t result = 0;
    for (unsigned bit = 0; bit < bitCount; ++bit)
    {
        result = (result << 1U) | (value & 1U);
        value >>= 1U;
    }
    return result;
}
}

Radix2FftPlan::Radix2FftPlan(std::size_t size, Radix2FftSizeProfile profile)
    : size_(size), longAnalysis_(profile == Radix2FftSizeProfile::LongAnalysis)
{
    if (!isSupportedRadix2FftSize(size, profile))
        throw std::invalid_argument("FFT size must be a power of two within its prepared size profile");

    unsigned bitCount = 0;
    for (auto value = size; value > 1; value >>= 1U)
        ++bitCount;

    bitReversal_.resize(size);
    for (std::size_t index = 0; index < size; ++index)
        bitReversal_[index] = reversedBits(index, bitCount);

    forwardTwiddles_.resize(size / 2);
    const auto twoPi = 2.0 * std::acos(-1.0);
    for (std::size_t index = 0; index < forwardTwiddles_.size(); ++index)
    {
        const auto phase = -twoPi * static_cast<double>(index)
            / static_cast<double>(size);
        forwardTwiddles_[index] = {std::cos(phase), std::sin(phase)};
    }
}

std::size_t Radix2FftPlan::size() const noexcept
{
    return size_;
}

Radix2FftPlan::Cursor Radix2FftPlan::beginTransform(bool inverse) const noexcept
{
    Cursor cursor;
    cursor.stage_ = Cursor::Stage::Permutation;
    cursor.size_ = size_;
    cursor.inverse_ = inverse;
    cursor.longAnalysis_ = longAnalysis_;
    return cursor;
}

bool Radix2FftPlan::advanceTransform(std::span<std::complex<double>> values,
    Cursor& cursor, std::size_t& workUnits) const noexcept
{
    if (values.size() != size_ || cursor.size_ != size_ || cursor.longAnalysis_ != longAnalysis_)
        return false;
    while (workUnits != 0 && !cursor.complete())
    {
        if (cursor.stage_ == Cursor::Stage::Permutation)
        {
            const auto count = std::min(workUnits, size_ - cursor.index_);
            const auto end = cursor.index_ + count;
            for (; cursor.index_ < end; ++cursor.index_)
            {
                const auto reversed = bitReversal_[cursor.index_];
                if (reversed > cursor.index_) std::swap(values[cursor.index_], values[reversed]);
            }
            workUnits -= count;
            if (cursor.index_ == size_)
            {
                cursor.stage_ = Cursor::Stage::Butterflies;
                cursor.index_ = 0;
            }
        }
        else if (cursor.stage_ == Cursor::Stage::Butterflies)
        {
            const auto half = cursor.length_ / 2;
            const auto offset = cursor.index_ % half;
            const auto begin = 2 * (cursor.index_ - offset);
            const auto count = std::min(workUnits, half - offset);
            const auto stride = size_ / cursor.length_;
            for (std::size_t i = offset; i < offset + count; ++i)
            {
                const auto prepared = forwardTwiddles_[i * stride];
                const auto even = values[begin + i];
                const auto input = values[begin + i + half];
                if (longAnalysis_)
                {
                    const auto real = prepared.real();
                    const auto imag = cursor.inverse_ ? -prepared.imag() : prepared.imag();
                    const auto oddReal = input.real() * real - input.imag() * imag;
                    const auto oddImag = input.real() * imag + input.imag() * real;
                    values[begin + i] = {even.real() + oddReal, even.imag() + oddImag};
                    values[begin + i + half] = {even.real() - oddReal, even.imag() - oddImag};
                }
                else
                {
                    const auto odd = input * (cursor.inverse_ ? std::conj(prepared) : prepared);
                    values[begin + i] = even + odd;
                    values[begin + i + half] = even - odd;
                }
            }
            cursor.index_ += count;
            workUnits -= count;
            if (cursor.index_ == size_ / 2)
            {
                cursor.index_ = 0;
                cursor.length_ *= 2;
                if (cursor.length_ > size_)
                    cursor.stage_ = cursor.inverse_ ? Cursor::Stage::Scale : Cursor::Stage::Complete;
            }
        }
        else if (cursor.stage_ == Cursor::Stage::Scale)
        {
            const auto count = std::min(workUnits, size_ - cursor.index_);
            const auto scale = 1.0 / static_cast<double>(size_);
            const auto end = cursor.index_ + count;
            for (; cursor.index_ < end; ++cursor.index_) values[cursor.index_] *= scale;
            workUnits -= count;
            if (cursor.index_ == size_) cursor.stage_ = Cursor::Stage::Complete;
        }
    }
    return true;
}

template <typename Value>
bool Radix2FftPlan::transform(
    std::span<std::complex<Value>> values,
    bool inverseTransform) const noexcept
{
    if (values.size() != size_)
        return false;

    for (std::size_t index = 0; index < size_; ++index)
    {
        const auto reversed = bitReversal_[index];
        if (reversed > index)
            std::swap(values[index], values[reversed]);
    }

    for (std::size_t length = 2; length <= size_; length <<= 1U)
    {
        const auto halfLength = length / 2;
        const auto twiddleStride = size_ / length;
        for (std::size_t begin = 0; begin < size_; begin += length)
        {
            if (longAnalysis_)
            {
                // Explicit real arithmetic permits vectorization without
                // fast-math. Long-analysis consumers validate finite spectra;
                // the standard FFT keeps its complex non-finite semantics.
                const auto sign = inverseTransform ? Value{1} : Value{-1};
                for (std::size_t offset = 0; offset < halfLength; ++offset)
                {
                    const auto prepared = forwardTwiddles_[offset * twiddleStride];
                    const auto real = static_cast<Value>(prepared.real());
                    const auto imag = -sign * static_cast<Value>(prepared.imag());
                    const auto even = values[begin + offset];
                    const auto input = values[begin + offset + halfLength];
                    const auto oddReal = input.real() * real - input.imag() * imag;
                    const auto oddImag = input.real() * imag + input.imag() * real;
                    values[begin + offset] = {even.real() + oddReal, even.imag() + oddImag};
                    values[begin + offset + halfLength] =
                        {even.real() - oddReal, even.imag() - oddImag};
                }
                continue;
            }
            for (std::size_t offset = 0; offset < halfLength; ++offset)
            {
                const auto prepared =
                    forwardTwiddles_[offset * twiddleStride];
                auto twiddle = std::complex<Value>{
                    static_cast<Value>(prepared.real()),
                    static_cast<Value>(prepared.imag()),
                };
                if (inverseTransform)
                    twiddle = std::conj(twiddle);
                const auto even = values[begin + offset];
                const auto odd = values[begin + offset + halfLength]
                    * twiddle;
                values[begin + offset] = even + odd;
                values[begin + offset + halfLength] = even - odd;
            }
        }
    }

    if (inverseTransform)
    {
        const auto scale = static_cast<Value>(1)
            / static_cast<Value>(size_);
        for (auto& value : values)
            value *= scale;
    }
    return true;
}

template <typename Value>
bool Radix2FftPlan::transform(
    std::span<const std::complex<Value>> input,
    std::span<std::complex<Value>> output,
    bool inverseTransform) const noexcept
{
    if (input.size() != size_ || output.size() != size_)
        return false;
    if (input.data() != output.data())
    {
        const std::less<const void*> precedes;
        const auto* inputBegin = input.data();
        const auto* inputEnd = input.data() + input.size();
        const auto* outputBegin = output.data();
        const auto* outputEnd = output.data() + output.size();
        const auto overlaps = precedes(inputBegin, outputEnd)
            && precedes(outputBegin, inputEnd);
        if (overlaps)
            return false;
        std::ranges::copy(input, output.begin());
    }
    return transform(output, inverseTransform);
}

bool Radix2FftPlan::forward(
    std::span<std::complex<double>> values) const noexcept
{
    return transform(values, false);
}

bool Radix2FftPlan::inverse(
    std::span<std::complex<double>> values) const noexcept
{
    return transform(values, true);
}

bool Radix2FftPlan::forward(
    std::span<const std::complex<double>> input,
    std::span<std::complex<double>> output) const noexcept
{
    return transform(input, output, false);
}

bool Radix2FftPlan::inverse(
    std::span<const std::complex<double>> input,
    std::span<std::complex<double>> output) const noexcept
{
    return transform(input, output, true);
}

bool Radix2FftPlan::forward(
    std::span<std::complex<float>> values) const noexcept
{
    return transform(values, false);
}

bool Radix2FftPlan::inverse(
    std::span<std::complex<float>> values) const noexcept
{
    return transform(values, true);
}

bool Radix2FftPlan::forward(
    std::span<const std::complex<float>> input,
    std::span<std::complex<float>> output) const noexcept
{
    return transform(input, output, false);
}

bool Radix2FftPlan::inverse(
    std::span<const std::complex<float>> input,
    std::span<std::complex<float>> output) const noexcept
{
    return transform(input, output, true);
}
}
