#pragma once

// Splits a test binary's full case list over several ctest entries. With
// "--shard=K/N" a binary runs only the cases whose position in its list,
// counted from 0, leaves K when divided by N, in their usual order; with no
// argument it runs them all. Every case belongs to exactly one shard, and
// report() prints how many ran so the shards' counts can be summed.

#include <charconv>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace talas::test
{
class ShardRunner
{
public:
    // Accepts no arguments (every case) or a single "--shard=K/N", 0 <= K < N.
    ShardRunner(int argc, char** argv)
    {
        if (argc == 1)
            return;
        constexpr std::string_view prefix = "--shard=";
        const std::string_view argument = argc == 2 ? argv[1] : "";
        const auto slash = argument.find('/');
        if (!argument.starts_with(prefix) || slash == std::string_view::npos
            || !parse(argument.substr(prefix.size(), slash - prefix.size()), index_)
            || !parse(argument.substr(slash + 1), count_) || count_ == 0
            || index_ >= count_)
        {
            throw std::invalid_argument(
                "expected no arguments or --shard=K/N with 0 <= K < N, got '"
                + std::string{argument} + "'");
        }
    }

    // Runs the next case in the list if it belongs to this shard.
    template <typename Case>
    void operator()(Case&& testCase)
    {
        if (position_++ % count_ == index_)
        {
            testCase();
            ++ran_;
        }
    }

    void report(std::string_view suite) const
    {
        std::cout << suite << " shard " << index_ << '/' << count_ << " ran "
                  << ran_ << " of " << position_ << " cases\n";
    }

private:
    static bool parse(std::string_view text, std::size_t& value)
    {
        const auto* end = text.data() + text.size();
        const auto [last, error] = std::from_chars(text.data(), end, value);
        return !text.empty() && error == std::errc{} && last == end;
    }

    std::size_t index_ = 0;
    std::size_t count_ = 1;
    std::size_t position_ = 0;
    std::size_t ran_ = 0;
};
} // namespace talas::test
