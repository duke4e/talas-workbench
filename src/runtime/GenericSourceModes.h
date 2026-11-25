#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace talas::runtime::generic_source
{
// GenericSource's Source field names the live input, an audio track on
// disk or a sample in RAM, compared without
// case or surrounding spaces.
[[nodiscard]] inline std::string normalizedSource(std::string_view source)
{
    const auto space = [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    };
    while (!source.empty() && space(source.front()))
        source.remove_prefix(1);
    while (!source.empty() && space(source.back()))
        source.remove_suffix(1);
    std::string normalized;
    normalized.reserve(source.size());
    for (const auto character : source)
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return normalized;
}

// An empty Source is the live input.
[[nodiscard]] inline bool isLiveInput(std::string_view source)
{
    const auto normalized = normalizedSource(source);
    return normalized.empty() || normalized == "live" || normalized == "live input"
        || normalized == "input" || normalized == "audio input";
}

// A track on disk plays the file as a sample in RAM does: the two differ
// only in where the samples wait.
[[nodiscard]] inline bool isRamSample(std::string_view source)
{
    const auto normalized = normalizedSource(source);
    // Ask unanswered plays the default source,
    // the recording on disk.
    return normalized == "sample" || normalized == "ram sample"
        || normalized == "sample in ram" || normalized == "disk"
        || normalized == "disk track" || normalized == "audio track on disk" || normalized == "ask";
}
}
