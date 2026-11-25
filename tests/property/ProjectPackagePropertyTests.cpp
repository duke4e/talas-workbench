#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPackage.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
constexpr std::uint64_t propertySeed = 0x5041434b50524f50ULL;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

[[nodiscard]] std::string generatedToken(
    std::uint64_t& state,
    std::size_t caseIndex)
{
    constexpr auto characters = std::string_view{"abcdefghijklmnopqrstuvwxyz"};
    std::string result = "p" + std::to_string(caseIndex) + "-";
    const auto length = 1U + static_cast<std::size_t>(
        nextRandom(state) % 12U);
    for (std::size_t index = 0; index < length; ++index)
    {
        result.push_back(characters[static_cast<std::size_t>(
            nextRandom(state) % characters.size())]);
    }
    return result;
}

[[nodiscard]] std::string caseContext(std::size_t caseIndex)
{
    return " seed=" + std::to_string(propertySeed)
        + " case=" + std::to_string(caseIndex);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-package-property-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(
        output.good(),
        "package property fixture write failed seed="
            + std::to_string(propertySeed));
}
}

int main()
{
    auto random = propertySeed;
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "Property.talaspkg";
    const auto assets = package
        / talas::project::projectPackageAssetsDirectoryName;
    std::filesystem::create_directories(assets / "nested");
    writeText(
        package / talas::project::projectPackageManifestFileName,
        talas::project::projectPackageManifestContents);
    require(
        talas::project::saveProjectFile(
            package / talas::project::projectPackageDocumentFileName,
            talas::project::ProjectDocument{})
            .ok(),
        "package property project fixture should save seed="
            + std::to_string(propertySeed));
    const auto resolved = talas::project::resolveProjectPackagePath(package);
    require(
        resolved.ok(),
        "package property fixture should resolve seed="
            + std::to_string(propertySeed));

    for (std::size_t caseIndex = 0; caseIndex < 128; ++caseIndex)
    {
        auto relative = std::filesystem::path{
            talas::project::projectPackageAssetsDirectoryName};
        const auto depth = 1U + static_cast<std::size_t>(
            nextRandom(random) % 4U);
        for (std::size_t level = 0; level < depth; ++level)
            relative /= generatedToken(random, caseIndex * 8U + level);
        relative += ".dat";
        writeText(package / relative, "property");
        talas::project::ProjectDocument document;
        document.assets.push_back({
            "asset" + std::to_string(caseIndex),
            talas::project::ProjectAssetKind::Table,
            relative.generic_string(),
        });
        require(
            talas::project::validateProjectPackageAssets(
                document, resolved)
                .ok(),
            "contained package path rejected" + caseContext(caseIndex));
    }

    const auto outside = temporary.path() / "outside.txt";
    writeText(outside, "outside");
    for (std::size_t caseIndex = 0; caseIndex < 128; ++caseIndex)
    {
        std::filesystem::path rejected;
        if (caseIndex % 4U == 0)
        {
            rejected = "../outside.txt";
        }
        else if (caseIndex % 4U == 1)
        {
            rejected = outside;
        }
        else
        {
            rejected = talas::project::projectPackageAssetsDirectoryName;
            const auto depth = 1U + static_cast<std::size_t>(
                nextRandom(random) % 4U);
            for (std::size_t level = 0; level < depth; ++level)
                rejected /= generatedToken(random, caseIndex * 8U + level);
            for (std::size_t level = 0; level < depth + 2U; ++level)
                rejected /= "..";
            rejected /= "outside.txt";
        }
        talas::project::ProjectDocument document;
        document.assets.push_back({
            "escape" + std::to_string(caseIndex),
            talas::project::ProjectAssetKind::Table,
            rejected.generic_string(),
        });
        require(
            !talas::project::validateProjectPackageAssets(
                 document, resolved)
                 .ok(),
            "escaping package path accepted" + caseContext(caseIndex));
    }

    std::filesystem::create_symlink(outside, assets / "escape.txt");
    talas::project::ProjectDocument symlinkDocument;
    symlinkDocument.assets.push_back({
        "symlinkEscape",
        talas::project::ProjectAssetKind::Table,
        "Assets/escape.txt",
    });
    require(
        !talas::project::validateProjectPackageAssets(
             symlinkDocument, resolved)
             .ok(),
        "package property must reject a symlink escape seed="
            + std::to_string(propertySeed));
}
