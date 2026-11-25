#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace talas::project
{
[[nodiscard]] inline std::filesystem::path pathFromUtf8(
    std::string_view encoded)
{
    std::u8string utf8;
    utf8.reserve(encoded.size());
    for (const auto byte : encoded)
        utf8.push_back(static_cast<char8_t>(
            static_cast<unsigned char>(byte)));
    return std::filesystem::path(utf8);
}

[[nodiscard]] inline std::string pathToUtf8(
    const std::filesystem::path& path)
{
    const auto utf8 = path.generic_u8string();
    std::string encoded;
    encoded.reserve(utf8.size());
    for (const auto byte : utf8)
        encoded.push_back(static_cast<char>(byte));
    return encoded;
}
}
