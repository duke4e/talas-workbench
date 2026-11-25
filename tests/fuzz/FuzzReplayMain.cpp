#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size);

namespace
{
struct ReplayOptions
{
    std::filesystem::path corpus;
    std::size_t runs = 4096;
    std::size_t maximumLength = 64U * 1024U;
    std::uint64_t seed = 0x4b796d61436c6f6eULL;
};

[[nodiscard]] bool parseUnsigned(
    std::string_view argument,
    std::string_view prefix,
    std::uint64_t& output) noexcept
{
    if (!argument.starts_with(prefix))
        return false;
    const auto value = argument.substr(prefix.size());
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] ReplayOptions optionsFrom(
    int argumentCount,
    char** arguments)
{
    ReplayOptions options;
    for (int index = 1; index < argumentCount; ++index)
    {
        const auto argument = std::string_view{arguments[index]};
        std::uint64_t parsed = 0;
        if (argument.starts_with("-runs="))
        {
            if (!parseUnsigned(argument, "-runs=", parsed))
                throw std::runtime_error("Invalid -runs value");
            options.runs = static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument.starts_with("-max_len="))
        {
            if (!parseUnsigned(argument, "-max_len=", parsed))
                throw std::runtime_error("Invalid -max_len value");
            options.maximumLength = static_cast<std::size_t>(parsed);
            continue;
        }
        if (argument.starts_with("-seed="))
        {
            if (!parseUnsigned(argument, "-seed=", parsed))
                throw std::runtime_error("Invalid -seed value");
            options.seed = parsed;
            continue;
        }
        if (argument.starts_with('-'))
            throw std::runtime_error("Unknown fuzz replay option: "
                                     + std::string{argument});
        if (options.corpus.empty())
        {
            options.corpus = argument;
            continue;
        }
        throw std::runtime_error("Only one fuzz corpus directory is allowed");
    }
    return options;
}

[[nodiscard]] std::vector<std::uint8_t> readSeed(
    const std::filesystem::path& path,
    std::size_t maximumLength)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open fuzz seed: " + path.string());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(std::min<std::size_t>(
        maximumLength,
        static_cast<std::size_t>(std::filesystem::file_size(path))));
    for (std::istreambuf_iterator<char> iterator{input}, end;
         iterator != end && bytes.size() < maximumLength;
         ++iterator)
    {
        bytes.push_back(static_cast<std::uint8_t>(*iterator));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::vector<std::uint8_t>> loadCorpus(
    const ReplayOptions& options)
{
    if (options.corpus.empty()
        || !std::filesystem::is_directory(options.corpus))
    {
        throw std::runtime_error("A fuzz corpus directory is required");
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator{
             options.corpus})
    {
        if (entry.is_regular_file())
            paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    std::vector<std::vector<std::uint8_t>> corpus;
    corpus.reserve(paths.size() + 1);
    corpus.emplace_back();
    for (const auto& path : paths)
        corpus.push_back(readSeed(path, options.maximumLength));
    return corpus;
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

void mutate(
    std::vector<std::uint8_t>& bytes,
    std::size_t maximumLength,
    std::uint64_t& randomState)
{
    const auto random = nextRandom(randomState);
    const auto operation = random % 6U;
    if (bytes.empty())
    {
        if (maximumLength != 0)
            bytes.push_back(static_cast<std::uint8_t>(random >> 8U));
        return;
    }

    const auto index = static_cast<std::size_t>(
        nextRandom(randomState) % bytes.size());
    if (operation == 0)
    {
        bytes[index] ^= static_cast<std::uint8_t>(
            1U << (nextRandom(randomState) % 8U));
    }
    else if (operation == 1 && bytes.size() < maximumLength)
    {
        bytes.insert(
            bytes.begin() + static_cast<std::ptrdiff_t>(index),
            static_cast<std::uint8_t>(nextRandom(randomState)));
    }
    else if (operation == 2)
    {
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(index));
    }
    else if (operation == 3 && bytes.size() < maximumLength)
    {
        bytes.push_back(static_cast<std::uint8_t>(nextRandom(randomState)));
    }
    else if (operation == 4)
    {
        bytes.resize(index);
    }
    else
    {
        const auto length = std::min<std::size_t>(
            bytes.size() - index,
            1U + static_cast<std::size_t>(
                nextRandom(randomState) % 16U));
        for (std::size_t offset = 0; offset < length; ++offset)
        {
            bytes[index + offset] = static_cast<std::uint8_t>(
                nextRandom(randomState));
        }
    }
}
}

int main(int argumentCount, char** arguments)
{
    ReplayOptions options;
    std::vector<std::vector<std::uint8_t>> corpus;
    try
    {
        options = optionsFrom(argumentCount, arguments);
        corpus = loadCorpus(options);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fuzz replay setup failed: " << error.what() << '\n';
        return 2;
    }

    for (const auto& seed : corpus)
        (void)LLVMFuzzerTestOneInput(seed.data(), seed.size());

    auto randomState = options.seed == 0
        ? 0x4b796d61436c6f6eULL
        : options.seed;
    for (std::size_t run = 0; run < options.runs; ++run)
    {
        auto bytes = corpus[static_cast<std::size_t>(
            nextRandom(randomState) % corpus.size())];
        const auto mutationCount = 1U + static_cast<std::size_t>(
            nextRandom(randomState) % 8U);
        for (std::size_t mutation = 0;
             mutation < mutationCount;
             ++mutation)
        {
            mutate(bytes, options.maximumLength, randomState);
        }
        (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    std::cout << "Fuzz replay PASS: " << options.runs
              << " generated inputs from " << corpus.size() - 1
              << " checked-in seeds\n";
    return 0;
}
