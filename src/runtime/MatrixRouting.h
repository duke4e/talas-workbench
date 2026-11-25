#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
enum class MatrixRowParseError
{
    None,
    NonNumeric,
    NonFinite,
    WrongCount
};

struct MatrixRowParseResult
{
    std::vector<double> gains;
    MatrixRowParseError error = MatrixRowParseError::None;

    [[nodiscard]] bool ok() const noexcept
    {
        return error == MatrixRowParseError::None;
    }
};

// A row's levels, each a literal or a {braced expression}. A level given
// in liveGains follows Event Values, variables or time, so its value there
// replaces the expression's.
[[nodiscard]] MatrixRowParseResult parseMatrixRowGains(
    std::string_view text,
    std::size_t expectedCount,
    double sampleRate,
    double tempoBpm,
    std::span<const std::optional<double>> liveGains = {});

// The expression of each braced level, and each bare Event Value such as
// !Amp1, by its position in the row; nullopt when the row's levels cannot
// be told apart. A row wholly an Interval collected through a block gives
// the block's value at each step.
struct MatrixRowExpression
{
    std::size_t index = 0;
    std::string source;
};
[[nodiscard]] std::optional<std::vector<MatrixRowExpression>> matrixRowExpressions(
    std::string_view text);

// The compiled parameter that carries a live level: "insToOut2#3" for the
// third level of InsToOut2.
[[nodiscard]] std::string matrixLiveGainId(std::string_view rowId, std::size_t index);
}
