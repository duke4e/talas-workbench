#pragma once

#include "CheckedBiquadMath.h"

#include <cmath>

namespace talas::runtime::shelving_presence
{

enum class ShelfMode
{
    High,
    Low,
};

struct Coefficients
{
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct State
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

[[nodiscard]] inline bool checkedAdd(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left + right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedSubtract(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left - right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedMultiply(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left * right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedDivide(
    double numerator, double denominator, double& result) noexcept
{
    if (!std::isfinite(numerator)
        || !std::isfinite(denominator)
        || denominator == 0.0)
    {
        return false;
    }
    const auto candidate = numerator / denominator;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool makeShelvingCoefficients(
    ShelfMode mode,
    double sampleRate,
    double cutoffFrequency,
    double boostOrCutDecibels,
    Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double sqrtTwo = 1.4142135623730950488016887242097;
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(cutoffFrequency)
        || cutoffFrequency <= 0.0
        || !std::isfinite(boostOrCutDecibels))
    {
        return false;
    }
    double nyquist = 0.0;
    if (!checkedMultiply(sampleRate, 0.5, nyquist)
        || cutoffFrequency >= nyquist)
    {
        return false;
    }

    double boostExponent = 0.0;
    if (!checkedDivide(boostOrCutDecibels, 40.0, boostExponent))
        return false;
    const auto a = std::pow(10.0, boostExponent);
    if (!std::isfinite(a) || a <= 0.0)
        return false;

    double omegaNumerator = 0.0;
    double omega = 0.0;
    if (!checkedMultiply(2.0, pi, omegaNumerator)
        || !checkedMultiply(
            omegaNumerator, cutoffFrequency, omegaNumerator)
        || !checkedDivide(omegaNumerator, sampleRate, omega))
    {
        return false;
    }
    const auto sinOmega = std::sin(omega);
    const auto cosOmega = std::cos(omega);
    if (!std::isfinite(sinOmega) || !std::isfinite(cosOmega))
        return false;

    double alpha = 0.0;
    if (!checkedMultiply(sinOmega, sqrtTwo, alpha)
        || !checkedMultiply(alpha, 0.5, alpha))
    {
        return false;
    }
    const auto sqrtA = std::sqrt(a);
    if (!std::isfinite(sqrtA))
        return false;

    double aPlusOne = 0.0;
    double aMinusOne = 0.0;
    double twoSqrtAAlpha = 0.0;
    if (!checkedAdd(a, 1.0, aPlusOne)
        || !checkedSubtract(a, 1.0, aMinusOne)
        || !checkedMultiply(2.0, sqrtA, twoSqrtAAlpha)
        || !checkedMultiply(twoSqrtAAlpha, alpha, twoSqrtAAlpha))
    {
        return false;
    }

    const auto expression = [](
                                double first,
                                double second,
                                double cosValue,
                                double alphaTerm,
                                bool subtractCos,
                                bool subtractAlpha,
                                double& result) noexcept {
        double cosTerm = 0.0;
        if (!checkedMultiply(second, cosValue, cosTerm))
            return false;
        if (subtractCos)
        {
            if (!checkedSubtract(first, cosTerm, result))
                return false;
        }
        else if (!checkedAdd(first, cosTerm, result))
        {
            return false;
        }
        return subtractAlpha
            ? checkedSubtract(result, alphaTerm, result)
            : checkedAdd(result, alphaTerm, result);
    };

    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double inner = 0.0;
    if (mode == ShelfMode::Low)
    {
        if (!expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                true, false, inner)
            || !checkedMultiply(a, inner, b0)
            || !checkedMultiply(aPlusOne, cosOmega, inner)
            || !checkedSubtract(aMinusOne, inner, inner)
            || !checkedMultiply(2.0, a, b1)
            || !checkedMultiply(b1, inner, b1)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                true, true, inner)
            || !checkedMultiply(a, inner, b2)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                false, false, a0)
            || !checkedMultiply(aPlusOne, cosOmega, inner)
            || !checkedAdd(aMinusOne, inner, inner)
            || !checkedMultiply(-2.0, inner, a1)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                false, true, a2))
        {
            return false;
        }
    }
    else
    {
        if (!expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                false, false, inner)
            || !checkedMultiply(a, inner, b0)
            || !checkedMultiply(aPlusOne, cosOmega, inner)
            || !checkedAdd(aMinusOne, inner, inner)
            || !checkedMultiply(-2.0, a, b1)
            || !checkedMultiply(b1, inner, b1)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                false, true, inner)
            || !checkedMultiply(a, inner, b2)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                true, false, a0)
            || !checkedMultiply(aPlusOne, cosOmega, inner)
            || !checkedSubtract(aMinusOne, inner, inner)
            || !checkedMultiply(2.0, inner, a1)
            || !expression(
                aPlusOne, aMinusOne, cosOmega, twoSqrtAAlpha,
                true, true, a2))
        {
            return false;
        }
    }

    Coefficients candidate;
    if (!checkedDivide(b0, a0, candidate.b0)
        || !checkedDivide(b1, a0, candidate.b1)
        || !checkedDivide(b2, a0, candidate.b2)
        || !checkedDivide(a1, a0, candidate.a1)
        || !checkedDivide(a2, a0, candidate.a2))
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

[[nodiscard]] inline bool makePresenceCoefficients(
    double sampleRate,
    double centerFrequency,
    double bandwidth,
    double boostOrCutDecibels,
    Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(centerFrequency)
        || centerFrequency <= 0.0
        || !std::isfinite(bandwidth)
        || bandwidth <= 0.0
        || !std::isfinite(boostOrCutDecibels))
    {
        return false;
    }
    double nyquist = 0.0;
    if (!checkedMultiply(sampleRate, 0.5, nyquist)
        || centerFrequency >= nyquist)
    {
        return false;
    }

    double q = 0.0;
    double boostExponent = 0.0;
    if (!checkedDivide(centerFrequency, bandwidth, q)
        || q <= 0.0
        || !checkedDivide(boostOrCutDecibels, 40.0, boostExponent))
    {
        return false;
    }
    const auto a = std::pow(10.0, boostExponent);
    if (!std::isfinite(a) || a <= 0.0)
        return false;

    double omegaNumerator = 0.0;
    double omega = 0.0;
    if (!checkedMultiply(2.0, pi, omegaNumerator)
        || !checkedMultiply(
            omegaNumerator, centerFrequency, omegaNumerator)
        || !checkedDivide(omegaNumerator, sampleRate, omega))
    {
        return false;
    }
    const auto sinOmega = std::sin(omega);
    const auto cosOmega = std::cos(omega);
    if (!std::isfinite(sinOmega) || !std::isfinite(cosOmega))
        return false;

    double twoQ = 0.0;
    double alpha = 0.0;
    if (!checkedMultiply(2.0, q, twoQ)
        || !checkedDivide(sinOmega, twoQ, alpha))
    {
        return false;
    }

    double alphaTimesA = 0.0;
    double alphaOverA = 0.0;
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    if (!checkedMultiply(alpha, a, alphaTimesA)
        || !checkedDivide(alpha, a, alphaOverA)
        || !checkedAdd(1.0, alphaTimesA, b0)
        || !checkedMultiply(-2.0, cosOmega, b1)
        || !checkedSubtract(1.0, alphaTimesA, b2)
        || !checkedAdd(1.0, alphaOverA, a0)
        || !checkedMultiply(-2.0, cosOmega, a1)
        || !checkedSubtract(1.0, alphaOverA, a2))
    {
        return false;
    }

    Coefficients candidate;
    if (!checkedDivide(b0, a0, candidate.b0)
        || !checkedDivide(b1, a0, candidate.b1)
        || !checkedDivide(b2, a0, candidate.b2)
        || !checkedDivide(a1, a0, candidate.a1)
        || !checkedDivide(a2, a0, candidate.a2))
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

template <typename StateType>
[[nodiscard]] inline bool processSample(
    const Coefficients& coefficients,
    StateType& state,
    double input,
    double scale,
    double& output) noexcept
{
    double scaledInput = 0.0;
    if (!checkedMultiply(input, scale, scaledInput))
    {
        return false;
    }
    return checked_biquad::processSample(
        coefficients, state, scaledInput, output);
}

} // namespace talas::runtime::shelving_presence
