#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace talas::project::detail
{
[[nodiscard]] inline std::filesystem::file_status symlinkStatus(
    const std::filesystem::path& path,
    std::error_code& error)
{
    auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        error.clear();
        status = std::filesystem::file_status{
            std::filesystem::file_type::not_found};
    }
    return status;
}

[[nodiscard]] inline std::optional<std::string> removeTemporaryArtifact(
    const std::filesystem::path& path,
    std::string_view description)
{
    std::error_code error;
    const auto status = symlinkStatus(path, error);
    if (error)
    {
        return "Unable to inspect " + std::string(description) + ": "
            + error.message();
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return {};
    if (!std::filesystem::is_regular_file(status)
        && status.type() != std::filesystem::file_type::symlink)
    {
        return std::string(description) + " is not a removable file";
    }
    if (!std::filesystem::remove(path, error) || error)
    {
        return "Unable to remove " + std::string(description)
            + (error ? ": " + error.message() : std::string{});
    }

    const auto afterRemoval = symlinkStatus(path, error);
    if (error)
    {
        return "Unable to verify " + std::string(description)
            + " removal: " + error.message();
    }
    if (afterRemoval.type() != std::filesystem::file_type::not_found)
        return std::string(description) + " still exists after removal";
    return {};
}
}
