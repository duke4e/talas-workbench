#include "talas/project/ProjectRecoveryFile.h"

#include "JsonValue.h"
#include "TemporaryFileSafety.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::string_view recoveryFormat = "talas.project-recovery";
constexpr double legacyRecoveryVersion = 1.0;
constexpr double historyRecoveryVersion = 2.0;
constexpr double recoveryVersion = 3.0;

[[nodiscard]] ProjectRecoveryError makeError(
    ProjectRecoveryErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    return {code, std::move(message), pathToUtf8(path)};
}

[[nodiscard]] std::filesystem::path siblingWithSuffix(
    const std::filesystem::path& path,
    std::string_view suffix)
{
    auto sibling = path;
    sibling += suffix;
    return sibling;
}

[[nodiscard]] std::filesystem::file_status recoverySymlinkStatus(
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

void removeSilently(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = recoverySymlinkStatus(path, error);
    if (error || status.type() == std::filesystem::file_type::not_found)
        return;
    if (std::filesystem::is_regular_file(status)
        || status.type() == std::filesystem::file_type::symlink)
    {
        std::filesystem::remove(path, error);
    }
}

[[nodiscard]] bool cancellationRequested(
    const ProjectRecoveryCancellationCheck& shouldCancel)
{
    return shouldCancel && shouldCancel();
}

[[nodiscard]] std::string serializeRecoveryRecord(
    const ProjectRecoveryRecord& record)
{
    std::string output;
    output.reserve(512);
    output += "{\n  \"format\": ";
    json::appendEscapedString(output, recoveryFormat);
    output += ",\n  \"version\": 3,\n  \"projectPath\": ";
    if (record.projectPath.has_value())
        json::appendEscapedString(output, pathToUtf8(*record.projectPath));
    else
        output += "null";
    output += ",\n  \"projectRevision\": ";
    if (record.projectRevision.has_value())
    {
        output += "{\n    \"exists\": ";
        output += record.projectRevision->exists
            ? "true"
            : "false";
        output += ",\n    \"contents\": ";
        json::appendEscapedString(
            output,
            record.projectRevision->contents);
        output += "\n  }";
    }
    else
    {
        output += "null";
    }
    output += ",\n  \"cleanProject\": ";
    json::appendEscapedString(
        output,
        serializeProjectDocument(record.cleanDocument));
    output += ",\n  \"versions\": [";
    for (std::size_t index = 0;
         index < record.versions.size();
         ++index)
    {
        const auto& version = record.versions[index];
        output += index == 0 ? "\n" : ",\n";
        output +=
            "    {\n"
            "      \"createdAtUnixMilliseconds\": "
            + std::to_string(version.createdAtUnixMilliseconds)
            + ",\n      \"recoveredProject\": ";
        json::appendEscapedString(
            output,
            serializeProjectDocument(
                version.recoveredDocument));
        output += "\n    }";
    }
    output += "\n  ]\n}\n";
    return output;
}

[[nodiscard]] std::optional<std::string>
validateRecoveryRecordForSave(
    const ProjectRecoveryRecord& record)
{
    if (record.versions.empty())
        return "Project recovery must contain at least one version";
    if (record.projectRevision.has_value()
        && !record.projectPath.has_value())
    {
        return "Project recovery revision requires a project path";
    }
    if (record.projectRevision.has_value()
        && !record.projectRevision->exists
        && !record.projectRevision->contents.empty())
    {
        return "Missing project recovery revision cannot contain file contents";
    }
    if (record.versions.size()
        > maximumProjectRecoveryVersions)
    {
        return "Project recovery contains too many versions";
    }
    const auto cleanValidation =
        validateProjectDocumentForPersistence(
            record.cleanDocument);
    if (!cleanValidation.ok())
    {
        const auto& diagnostic =
            cleanValidation.diagnostics.front();
        return "Clean project is not canonically persistable at "
            + diagnostic.path + ": "
            + diagnostic.message;
    }
    for (std::size_t index = 0;
         index < record.versions.size();
         ++index)
    {
        const auto recoveredValidation =
            validateProjectDocumentForPersistence(
                record.versions[index].recoveredDocument);
        if (!recoveredValidation.ok())
        {
            const auto& diagnostic =
                recoveredValidation.diagnostics.front();
            return "Recovered project version "
                + std::to_string(index)
                + " is not canonically persistable at "
                + diagnostic.path + ": "
                + diagnostic.message;
        }
    }
    for (std::size_t index = 1;
         index < record.versions.size();
         ++index)
    {
        if (record.versions[index - 1]
                .createdAtUnixMilliseconds
            >= record.versions[index]
                   .createdAtUnixMilliseconds)
        {
            return "Project recovery version timestamps must be strictly increasing";
        }
    }
    if (record.versions.back().createdAtUnixMilliseconds
        > maximumProjectRecoveryTimestamp)
    {
        return "Project recovery version timestamp is too large";
    }
    return {};
}

[[nodiscard]] std::optional<ProjectDocument>
parseRecoveredProject(
    const json::Value* value,
    const std::filesystem::path& path,
    bool usedBackup,
    ProjectRecoveryLoadResult& failure)
{
    if (value == nullptr || !value->isString())
    {
        failure = {{}, makeError(
                           ProjectRecoveryErrorCode::InvalidRecovery,
                           "Recovered project snapshot must be a string",
                           path),
                   usedBackup};
        return {};
    }
    auto recovered = parseProjectDocument(value->string());
    if (!recovered.ok())
    {
        const auto message = recovered.errors.empty()
            ? "Invalid recovered project"
            : "Invalid recovered project: "
                + recovered.errors.front().message;
        failure = {{}, makeError(
                           ProjectRecoveryErrorCode::InvalidRecovery,
                           message,
                           path),
                   usedBackup};
        return {};
    }
    return std::move(recovered.document);
}

[[nodiscard]] ProjectRecoveryLoadResult parseRecoveryRecord(
    std::string_view text,
    const std::filesystem::path& path,
    bool usedBackup)
{
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    auto parsed = json::parse(text, limits);
    if (!parsed.ok())
    {
        const auto message = parsed.errors.empty()
            ? "Invalid recovery JSON"
            : "Invalid recovery JSON: " + parsed.errors.front().message;
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        message,
                        path),
                usedBackup};
    }
    if (!parsed.value.isObject())
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Recovery root must be an object",
                        path),
                usedBackup};
    }

    const auto* format = json::findObjectMember(parsed.value, "format");
    const auto* version = json::findObjectMember(parsed.value, "version");
    const auto* projectPath = json::findObjectMember(parsed.value, "projectPath");
    const auto* projectRevision =
        json::findObjectMember(
            parsed.value,
            "projectRevision");
    const auto* cleanProject = json::findObjectMember(parsed.value, "cleanProject");
    if (format == nullptr || !format->isString()
        || format->string() != recoveryFormat)
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Unsupported recovery format",
                        path),
                usedBackup};
    }
    if (version == nullptr || !version->isNumber()
        || !std::isfinite(version->number())
        || (version->number() != legacyRecoveryVersion
            && version->number()
                != historyRecoveryVersion
            && version->number() != recoveryVersion))
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Unsupported recovery version",
                        path),
                usedBackup};
    }
    if (projectPath == nullptr
        || (!projectPath->isNull() && !projectPath->isString()))
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Recovery projectPath must be a string or null",
                        path),
                usedBackup};
    }
    if (version->number() == recoveryVersion
        && (projectRevision == nullptr
            || (!projectRevision->isNull()
                && !projectRevision->isObject())))
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Recovery projectRevision must be an object or null",
                        path),
                usedBackup};
    }
    if (cleanProject == nullptr || !cleanProject->isString())
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        "Clean recovery project snapshot must be a string",
                        path),
                usedBackup};
    }

    auto clean = parseProjectDocument(cleanProject->string());
    if (!clean.ok())
    {
        const auto message = clean.errors.empty()
            ? "Invalid clean project in recovery"
            : "Invalid clean project in recovery: " + clean.errors.front().message;
        return {{}, makeError(
                        ProjectRecoveryErrorCode::InvalidRecovery,
                        message,
                        path),
                usedBackup};
    }
    ProjectRecoveryRecord record;
    if (projectPath->isString() && !projectPath->string().empty())
    {
        try
        {
            record.projectPath = pathFromUtf8(projectPath->string());
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return {{}, makeError(
                            ProjectRecoveryErrorCode::InvalidRecovery,
                            "Recovery projectPath is not valid UTF-8",
                            path),
                    usedBackup};
        }
    }
    if (version->number() == recoveryVersion
        && projectRevision->isObject())
    {
        const auto* revisionExists =
            json::findObjectMember(
                *projectRevision,
                "exists");
        const auto* revisionContents =
            json::findObjectMember(
                *projectRevision,
                "contents");
        if (revisionExists == nullptr
            || !revisionExists->isBoolean()
            || revisionContents == nullptr
            || !revisionContents->isString()
            || (!revisionExists->boolean()
                && !revisionContents->string().empty())
            || !record.projectPath.has_value())
        {
            return {{}, makeError(
                            ProjectRecoveryErrorCode::InvalidRecovery,
                            "Recovery projectRevision is invalid",
                            path),
                    usedBackup};
        }
        record.projectRevision =
            ProjectFileRevision{
                revisionExists->boolean(),
                revisionContents->string(),
            };
    }
    record.cleanDocument = std::move(clean.document);
    if (version->number() == legacyRecoveryVersion)
    {
        ProjectRecoveryLoadResult failure;
        auto recovered = parseRecoveredProject(
            json::findObjectMember(
                parsed.value,
                "recoveredProject"),
            path,
            usedBackup,
            failure);
        if (!recovered.has_value())
            return failure;
        record.versions.push_back({
            0,
            std::move(*recovered),
        });
    }
    else
    {
        const auto* versions =
            json::findObjectMember(parsed.value, "versions");
        if (versions == nullptr || !versions->isArray()
            || versions->array().empty()
            || versions->array().size()
                > maximumProjectRecoveryVersions)
        {
            return {{}, makeError(
                            ProjectRecoveryErrorCode::InvalidRecovery,
                            "Recovery versions must be a non-empty bounded array",
                            path),
                    usedBackup};
        }

        std::uint64_t previousTimestamp = 0;
        bool hasPreviousTimestamp = false;
        for (const auto& source : versions->array())
        {
            if (!source.isObject())
            {
                return {{}, makeError(
                                ProjectRecoveryErrorCode::InvalidRecovery,
                                "Recovery version must be an object",
                                path),
                        usedBackup};
            }
            const auto* timestamp = json::findObjectMember(
                source,
                "createdAtUnixMilliseconds");
            if (timestamp == nullptr || !timestamp->isNumber()
                || !std::isfinite(timestamp->number())
                || timestamp->number() < 0.0
                || timestamp->number()
                    > static_cast<double>(
                        maximumProjectRecoveryTimestamp)
                || std::floor(timestamp->number())
                    != timestamp->number())
            {
                return {{}, makeError(
                                ProjectRecoveryErrorCode::InvalidRecovery,
                                "Recovery version timestamp must be a non-negative integer",
                                path),
                        usedBackup};
            }
            const auto parsedTimestamp =
                static_cast<std::uint64_t>(
                    timestamp->number());
            if (hasPreviousTimestamp
                && parsedTimestamp <= previousTimestamp)
            {
                return {{}, makeError(
                                ProjectRecoveryErrorCode::InvalidRecovery,
                                "Recovery version timestamps must be strictly increasing",
                                path),
                        usedBackup};
            }

            ProjectRecoveryLoadResult failure;
            auto recovered = parseRecoveredProject(
                json::findObjectMember(
                    source,
                    "recoveredProject"),
                path,
                usedBackup,
                failure);
            if (!recovered.has_value())
                return failure;
            record.versions.push_back({
                parsedTimestamp,
                std::move(*recovered),
            });
            previousTimestamp = parsedTimestamp;
            hasPreviousTimestamp = true;
        }
    }
    return {std::move(record), {}, usedBackup};
}

[[nodiscard]] ProjectRecoveryLoadResult loadRecoveryAt(
    const std::filesystem::path& path,
    bool usedBackup)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::ReadFailed,
                        "Unable to open project recovery file",
                        path),
                usedBackup};
    }
    const std::string text(std::istreambuf_iterator<char>{input}, {});
    if (!input.good() && !input.eof())
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::ReadFailed,
                        "Unable to read project recovery file",
                        path),
                usedBackup};
    }
    return parseRecoveryRecord(text, path, usedBackup);
}

[[nodiscard]] std::optional<ProjectRecoveryError> inspectReadableArtifact(
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = recoverySymlinkStatus(path, error);
    if (error)
    {
        return makeError(
            ProjectRecoveryErrorCode::ReadFailed,
            "Unable to inspect project recovery file: " + error.message(),
            path);
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return {};
    if (!std::filesystem::is_regular_file(status))
    {
        return makeError(
            ProjectRecoveryErrorCode::ReadFailed,
            "Project recovery path is not a regular file",
            path);
    }
    return {};
}

[[nodiscard]] std::optional<ProjectRecoveryError> removeRecoveryArtifact(
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = recoverySymlinkStatus(path, error);
    if (error)
    {
        return makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to inspect project recovery artifact: " + error.message(),
            path);
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return {};
    if (!std::filesystem::is_regular_file(status)
        && status.type() != std::filesystem::file_type::symlink)
    {
        return makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Refusing to remove a non-file recovery artifact",
            path);
    }
    if (!std::filesystem::remove(path, error) || error)
    {
        return makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to remove project recovery artifact: " + error.message(),
            path);
    }
    return {};
}
}

bool appendProjectRecoveryVersion(
    ProjectRecoveryRecord& record,
    ProjectDocument recoveredDocument,
    std::uint64_t createdAtUnixMilliseconds)
{
    createdAtUnixMilliseconds = std::min(
        createdAtUnixMilliseconds,
        maximumProjectRecoveryTimestamp);
    if (!record.versions.empty())
    {
        const auto current =
            serializeProjectDocument(
                record.versions.back().recoveredDocument);
        const auto incoming =
            serializeProjectDocument(recoveredDocument);
        if (current == incoming)
            return false;
        const auto priorTimestamp =
            record.versions.back()
                .createdAtUnixMilliseconds;
        if (createdAtUnixMilliseconds <= priorTimestamp)
        {
            if (priorTimestamp
                >= maximumProjectRecoveryTimestamp)
            {
                record.versions.clear();
            }
            else
            {
                createdAtUnixMilliseconds =
                    priorTimestamp + 1;
            }
        }
    }
    record.versions.push_back({
        createdAtUnixMilliseconds,
        std::move(recoveredDocument),
    });
    if (record.versions.size()
        > maximumProjectRecoveryVersions)
    {
        record.versions.erase(
            record.versions.begin(),
            record.versions.begin()
                + static_cast<std::ptrdiff_t>(
                    record.versions.size()
                    - maximumProjectRecoveryVersions));
    }
    return true;
}

std::optional<std::size_t>
projectRecoveryVersionIndexNewestFirst(
    const ProjectRecoveryRecord& record,
    std::size_t newestFirstPosition) noexcept
{
    if (newestFirstPosition >= record.versions.size())
        return {};
    return record.versions.size()
        - newestFirstPosition
        - 1;
}

bool makeProjectRecoveryAssetPathsAbsolute(
    ProjectRecoveryRecord& record,
    const std::filesystem::path& sourceProjectPath)
{
    auto converted = makeProjectAssetPathsAbsolute(
        record.cleanDocument,
        sourceProjectPath);
    for (auto& version : record.versions)
    {
        converted =
            makeProjectAssetPathsAbsolute(
                version.recoveredDocument,
                sourceProjectPath)
            || converted;
    }
    return converted;
}

bool projectRecoverySourceMatchesBaseline(
    const ProjectRecoveryRecord& record,
    const ProjectDocument& sourceDocument,
    const ProjectFileRevision& sourceRevision)
{
    return record.projectPath.has_value()
        && record.projectRevision.has_value()
        && *record.projectRevision
            == sourceRevision
        && serializeProjectDocument(
               record.cleanDocument)
            == serializeProjectDocument(
                sourceDocument);
}

bool consumeSuccessfulProjectRecoveryWrite(
    const ProjectRecoverySaveResult& result,
    std::vector<ProjectRecoveryVersion>& publishedVersions,
    std::vector<ProjectRecoveryVersion>& currentVersions) noexcept
{
    if (!result.ok())
    {
        publishedVersions.clear();
        return false;
    }
    currentVersions = std::move(publishedVersions);
    return true;
}

ProjectRecoverySaveResult saveProjectRecoveryFile(
    const std::filesystem::path& path,
    const ProjectRecoveryRecord& record,
    ProjectRecoveryCancellationCheck shouldCancel)
{
    if (path.empty())
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Project recovery path cannot be empty",
            path)};
    }
    if (const auto invalid =
            validateRecoveryRecordForSave(record);
        invalid.has_value())
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            *invalid,
            path)};
    }
    if (cancellationRequested(shouldCancel))
    {
        return {makeError(
            ProjectRecoveryErrorCode::Cancelled,
            "Project recovery write was cancelled",
            path)};
    }

    std::error_code error;
    const auto targetStatus = recoverySymlinkStatus(path, error);
    if (error)
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to inspect project recovery path: " + error.message(),
            path)};
    }
    const auto targetExists =
        targetStatus.type() != std::filesystem::file_type::not_found;
    if (targetExists && !std::filesystem::is_regular_file(targetStatus))
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Project recovery destination is not a regular file",
            path)};
    }

    const auto parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return {makeError(
                ProjectRecoveryErrorCode::WriteFailed,
                "Unable to create project recovery directory: " + error.message(),
                path)};
        }
    }

    const auto temporary = siblingWithSuffix(path, ".tmp");
    const auto backup = siblingWithSuffix(path, ".bak");
    if (auto removalError = detail::removeTemporaryArtifact(
            temporary,
            "temporary project recovery file");
        removalError.has_value())
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            std::move(*removalError),
            temporary)};
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return {makeError(
                ProjectRecoveryErrorCode::WriteFailed,
                "Unable to create temporary project recovery file",
                temporary)};
        }
        output << serializeRecoveryRecord(record);
        output.flush();
        if (!output.good())
        {
            output.close();
            removeSilently(temporary);
            return {makeError(
                ProjectRecoveryErrorCode::WriteFailed,
                "Unable to write temporary project recovery file",
                temporary)};
        }
    }

    if (cancellationRequested(shouldCancel))
    {
        removeSilently(temporary);
        return {makeError(
            ProjectRecoveryErrorCode::Cancelled,
            "Project recovery write was cancelled",
            path)};
    }

    std::filesystem::rename(temporary, path, error);
    if (!error)
    {
        removeSilently(backup);
        return {};
    }
    if (!targetExists)
    {
        removeSilently(temporary);
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to publish project recovery file: " + error.message(),
            path)};
    }

    error.clear();
    const auto backupStatus = recoverySymlinkStatus(backup, error);
    if (error
        || backupStatus.type() != std::filesystem::file_type::not_found)
    {
        removeSilently(temporary);
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            error
                ? "Unable to inspect project recovery backup: " + error.message()
                : "Project recovery backup already exists",
            backup)};
    }

    std::filesystem::rename(path, backup, error);
    if (error)
    {
        removeSilently(temporary);
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to preserve prior project recovery: " + error.message(),
            path)};
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::error_code restoreError;
        std::filesystem::rename(backup, path, restoreError);
        removeSilently(temporary);
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Unable to replace project recovery file: " + error.message(),
            path)};
    }
    removeSilently(backup);
    return {};
}

ProjectRecoveryLoadResult loadProjectRecoveryFile(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {{}, makeError(
                        ProjectRecoveryErrorCode::NotFound,
                        "Project recovery path is empty",
                        path),
                false};
    }

    const auto primaryInspection = inspectReadableArtifact(path);
    if (primaryInspection.has_value())
        return {{}, std::move(primaryInspection), false};

    std::error_code error;
    const auto primaryStatus = recoverySymlinkStatus(path, error);
    if (primaryStatus.type() != std::filesystem::file_type::not_found)
        return loadRecoveryAt(path, false);

    const auto backup = siblingWithSuffix(path, ".bak");
    const auto backupInspection = inspectReadableArtifact(backup);
    if (backupInspection.has_value())
        return {{}, std::move(backupInspection), true};
    const auto backupStatus = recoverySymlinkStatus(backup, error);
    if (backupStatus.type() != std::filesystem::file_type::not_found)
        return loadRecoveryAt(backup, true);

    return {{}, makeError(
                    ProjectRecoveryErrorCode::NotFound,
                    "No project recovery file exists",
                    path),
            false};
}

ProjectRecoveryDiscardResult discardProjectRecoveryFile(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {makeError(
            ProjectRecoveryErrorCode::WriteFailed,
            "Project recovery path cannot be empty",
            path)};
    }
    for (const auto& artifact : {
             path,
             siblingWithSuffix(path, ".tmp"),
             siblingWithSuffix(path, ".bak"),
         })
    {
        if (auto error = removeRecoveryArtifact(artifact); error.has_value())
            return {std::move(error)};
    }
    return {};
}
}
