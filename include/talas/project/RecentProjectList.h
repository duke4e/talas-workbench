#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t defaultRecentProjectCapacity = 10;

class RecentProjectList
{
public:
    explicit RecentProjectList(
        std::size_t capacity = defaultRecentProjectCapacity);

    [[nodiscard]] const std::vector<std::filesystem::path>& entries() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

    [[nodiscard]] bool note(const std::filesystem::path& projectPath);
    [[nodiscard]] bool remove(const std::filesystem::path& projectPath);

private:
    std::size_t capacity_;
    std::vector<std::filesystem::path> entries_;
};

enum class RecentProjectListErrorCode
{
    NotFound,
    ReadFailed,
    WriteFailed,
    InvalidState,
    UnsupportedState,
};

struct RecentProjectListError
{
    RecentProjectListErrorCode code;
    std::string message;
    std::string path;
};

struct RecentProjectListSaveResult
{
    std::optional<RecentProjectListError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

struct RecentProjectListLoadResult
{
    std::optional<RecentProjectList> list;
    std::optional<RecentProjectListError> error;
    bool usedBackup = false;

    [[nodiscard]] bool ok() const
    {
        return list.has_value() && !error.has_value();
    }

    [[nodiscard]] bool missing() const
    {
        return error.has_value()
            && error->code == RecentProjectListErrorCode::NotFound;
    }
};

[[nodiscard]] RecentProjectListSaveResult saveRecentProjectList(
    const std::filesystem::path& path,
    const RecentProjectList& list);
[[nodiscard]] RecentProjectListLoadResult loadRecentProjectList(
    const std::filesystem::path& path,
    std::size_t capacity = defaultRecentProjectCapacity);
}
