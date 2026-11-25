#include "talas/app/WorkbenchLaunch.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-workbench-launch-tests-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::error_code error;
        const auto created =
            std::filesystem::create_directory(path_, error);
        if (error || !created)
            throw std::runtime_error("temporary test directory could not be created");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string utf8String(const std::filesystem::path& path)
{
    const auto encoded = path.u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size(),
    };
}

void testEmptyArgumentsSelectNormalLaunch()
{
    const auto parsed = talas::app::parseWorkbenchLaunch({});
    require(parsed.ok(), "empty arguments should parse");
    require(parsed.options->mode == talas::app::WorkbenchLaunchMode::Normal,
            "empty arguments should select normal launch");
    require(parsed.options->smokeDirectory.empty(),
            "normal launch should not have a smoke directory");
}

void testAbsoluteExistingDirectorySelectsSmokeLaunch()
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto parsed = talas::app::parseWorkbenchLaunch(
        {"--smoke-test", directory.string()});
    require(parsed.ok(), "valid smoke arguments should parse");
    require(parsed.options->mode == talas::app::WorkbenchLaunchMode::SmokeTest,
            "smoke flag should select smoke mode");
    require(parsed.options->smokeDirectory == directory.lexically_normal(),
            "smoke directory should be retained in normalized form");
}

void testUtf8SmokeDirectoryRoundTrips()
{
    TemporaryDirectory directory;
    const auto unicodeDirectory =
        directory.path()
        / std::filesystem::path(
            std::u8string{u8"smoke-\u017e"});
    std::error_code error;
    require(
        std::filesystem::create_directory(unicodeDirectory, error) && !error,
        "UTF-8 smoke directory fixture should be created");
    const auto parsed = talas::app::parseWorkbenchLaunch(
        {"--smoke-test", utf8String(unicodeDirectory)});
    require(parsed.ok(), "UTF-8 smoke directory should parse");
    require(
        parsed.options->smokeDirectory == unicodeDirectory.lexically_normal(),
        "UTF-8 smoke directory should round-trip without locale loss");
}

void testMalformedSmokeInvocationsAreRejected()
{
    const auto missingPath = talas::app::parseWorkbenchLaunch({"--smoke-test"});
    require(!missingPath.ok(), "missing smoke directory should fail");
    require(missingPath.error.find("usage") != std::string::npos,
            "missing smoke directory should report usage");

    const auto relativePath =
        talas::app::parseWorkbenchLaunch({"--smoke-test", "relative"});
    require(!relativePath.ok(), "relative smoke directory should fail");
    require(relativePath.error.find("absolute") != std::string::npos,
            "relative smoke directory should explain the absolute-path contract");

    const auto unknown = talas::app::parseWorkbenchLaunch({"--unknown"});
    require(!unknown.ok(), "unknown arguments should fail");
    require(unknown.error.find("usage") != std::string::npos,
            "unknown arguments should report usage");
}

void testSmokeFlagAlwaysRequiresAnIndependentProcess()
{
    require(
        talas::app::requiresIndependentWorkbenchInstance(
            {"--smoke-test", std::filesystem::temp_directory_path().string()}),
        "valid smoke invocation should bypass the interactive single-instance lock");
    require(
        talas::app::requiresIndependentWorkbenchInstance({"--smoke-test"}),
        "malformed smoke invocation should still start independently and report its error");
    require(
        !talas::app::requiresIndependentWorkbenchInstance({}),
        "ordinary launch should retain single-instance behavior");
    require(
        !talas::app::requiresIndependentWorkbenchInstance({"--unknown"}),
        "unrelated arguments should not bypass single-instance behavior");
}

void testMissingOrNonDirectoryPathIsRejected()
{
    TemporaryDirectory directory;
    const auto missing = directory.path() / "missing";
    const auto missingResult =
        talas::app::parseWorkbenchLaunch({"--smoke-test", missing.string()});
    require(!missingResult.ok(), "missing smoke directory should fail");
    require(missingResult.error.find("existing directory") != std::string::npos,
            "missing smoke directory should explain the directory contract");

    const auto file = directory.path() / "file";
    {
        std::ofstream stream(file, std::ios::trunc);
        require(stream.good(), "non-directory fixture should be created");
    }
    const auto fileResult =
        talas::app::parseWorkbenchLaunch({"--smoke-test", file.string()});
    require(!fileResult.ok(), "non-directory smoke path should fail");
}

void testRepositoryRootIsRejectedAsSmokeDirectory()
{
    TemporaryDirectory directory;
    std::error_code error;
    require(
        std::filesystem::create_directory(directory.path() / ".git", error) && !error,
        "repository marker fixture should be created");
    const auto result =
        talas::app::parseWorkbenchLaunch({"--smoke-test", directory.path().string()});
    require(!result.ok(), "a repository root must not receive smoke artifacts");
    require(result.error.find("repository") != std::string::npos,
            "a rejected repository root should explain the directory contract");

    const auto nested = directory.path() / "workbench-smoke";
    require(
        std::filesystem::create_directory(nested, error) && !error,
        "nested smoke directory fixture should be created");
    require(
        talas::app::parseWorkbenchLaunch({"--smoke-test", nested.string()}).ok(),
        "a dedicated directory below a repository root should remain valid");
}
}

// The preferences are kept in talas.pre; a user
// starts the Workbench by double-clicking a copy in their own folder, which
// the system passes as the one argument, or as a file opened while it runs.
void testAUsersPreferencesFileStartsTheWorkbench()
{
    TemporaryDirectory directory;
    const auto own = directory.path() / std::string{talas::app::workbenchPreferencesFileName};
    std::ofstream{own} << "showUntitledOnStartup=1\n";
    const auto expected = std::filesystem::absolute(own).lexically_normal();
    const auto parsed = talas::app::parseWorkbenchLaunch({utf8String(own)});
    require(parsed.ok() && parsed.options->mode == talas::app::WorkbenchLaunchMode::Normal
            && parsed.options->preferencesFile == expected,
        "opening a user's talas.pre should start the Workbench with that file's preferences");
    require(talas::app::preferencesFileOpened("\"" + utf8String(own) + "\"") == std::optional{expected}
            && talas::app::isWorkbenchPreferencesFile("Talas.PRE") && !talas::app::isWorkbenchPreferencesFile("notes.txt"),
        "a preferences file opened while the Workbench runs should be recognized");
    const auto text = directory.path() / "notes.txt";
    std::ofstream{text} << "x";
    require(!talas::app::parseWorkbenchLaunch({utf8String(text)}).ok()
            && !talas::app::preferencesFileOpened(utf8String(directory.path() / "missing.pre")).has_value(),
        "another file, or a preferences file that is not there, should not be taken");
}

int main()
{
    testEmptyArgumentsSelectNormalLaunch();
    testAbsoluteExistingDirectorySelectsSmokeLaunch();
    testUtf8SmokeDirectoryRoundTrips();
    testMalformedSmokeInvocationsAreRejected();
    testSmokeFlagAlwaysRequiresAnIndependentProcess();
    testMissingOrNonDirectoryPathIsRejected();
    testRepositoryRootIsRejectedAsSmokeDirectory();
    testAUsersPreferencesFileStartsTheWorkbench();
    return 0;
}
