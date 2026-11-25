#include "talas/app/SoundLibraryModel.h"

#include "talas/app/DescriptorPresentationModel.h"
#include "talas/core/TerminalAudioLayout.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"
#include "project/JsonValue.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace talas::app
{
namespace
{
using JsonValue = talas::project::json::Value;

constexpr std::size_t maximumManifestBytes = 256U * 1024U;
constexpr std::size_t maximumExampleBytes = 32U * 1024U * 1024U;
constexpr std::size_t maximumIndexBytes = 4U * 1024U * 1024U;
constexpr std::size_t maximumHelpBytes = 4U * 1024U * 1024U;
constexpr std::size_t maximumExampleAssetBytes = 64U * 1024U * 1024U;
constexpr std::size_t maximumHelpTitleBytes = 160;
constexpr std::size_t maximumHelpSummaryBytes = 480;

[[nodiscard]] std::string folded(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
    {
        result.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(character))));
    }
    return result;
}

void appendTerm(std::string& value, std::string_view term)
{
    if (term.empty())
        return;
    if (!value.empty())
        value.push_back(' ');
    value.append(term);
}

struct ContainedFileSnapshot
{
    std::vector<std::uint8_t> bytes;
    std::filesystem::path diagnosticPath;
};

[[nodiscard]] bool validContainedRelativePath(
    const std::filesystem::path& root,
    const std::filesystem::path& relative)
{
    const auto portable = relative.generic_string();
    if (root.empty() || !root.is_absolute() || relative.empty()
        || relative.is_absolute() || relative.has_root_name()
        || relative.has_root_directory() || portable.empty()
        || portable.front() == '/' || portable.front() == '\\'
        || portable.find('\\') != std::string::npos
        || (portable.size() >= 2U
            && std::isalpha(static_cast<unsigned char>(portable.front())) != 0
            && portable[1] == ':'))
    {
        return false;
    }
    for (const auto& component : relative)
    {
        if (component.empty() || component == "." || component == "..")
            return false;
    }
    return true;
}

#if defined(_WIN32)
class UniqueHandle
{
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value)
    {
    }
    ~UniqueHandle()
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] std::optional<std::wstring> finalHandlePath(HANDLE handle)
{
    const auto count = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (count == 0U)
        return std::nullopt;
    std::wstring value(count, L'\0');
    const auto written = GetFinalPathNameByHandleW(
        handle,
        value.data(),
        count,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= count)
        return std::nullopt;
    value.resize(written);
    return value;
}

[[nodiscard]] bool equalWindowsPath(
    std::wstring_view left,
    std::wstring_view right)
{
    return left.size() == right.size()
        && CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE)
            == CSTR_EQUAL;
}

[[nodiscard]] std::optional<ContainedFileSnapshot> readBoundedContainedFile(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::size_t maximumBytes)
{
    if (!validContainedRelativePath(root, relative))
        return std::nullopt;
    UniqueHandle rootHandle(CreateFileW(
        root.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!rootHandle)
        return std::nullopt;
    FILE_ATTRIBUTE_TAG_INFO rootAttributes{};
    if (!GetFileInformationByHandleEx(
            rootHandle.get(),
            FileAttributeTagInfo,
            &rootAttributes,
            sizeof(rootAttributes))
        || (rootAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
        || (rootAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return std::nullopt;
    }
    const auto rootFinal = finalHandlePath(rootHandle.get());
    if (!rootFinal.has_value())
        return std::nullopt;

    const auto diagnosticPath = root / relative;
    UniqueHandle fileHandle(CreateFileW(
        diagnosticPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!fileHandle)
        return std::nullopt;
    FILE_ATTRIBUTE_TAG_INFO fileAttributes{};
    if (!GetFileInformationByHandleEx(
            fileHandle.get(),
            FileAttributeTagInfo,
            &fileAttributes,
            sizeof(fileAttributes))
        || (fileAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
        || (fileAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return std::nullopt;
    }
    const auto fileFinal = finalHandlePath(fileHandle.get());
    const auto expectedFinal =
        (std::filesystem::path{*rootFinal} / relative).lexically_normal().native();
    if (!fileFinal.has_value()
        || !equalWindowsPath(*fileFinal, expectedFinal))
    {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(fileHandle.get(), &size) || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
    {
        return std::nullopt;
    }
    ContainedFileSnapshot result;
    result.diagnosticPath = diagnosticPath;
    result.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < result.bytes.size())
    {
        DWORD read = 0;
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(
            result.bytes.size() - offset,
            std::numeric_limits<DWORD>::max()));
        if (!ReadFile(
                fileHandle.get(),
                result.bytes.data() + offset,
                requested,
                &read,
                nullptr)
            || read == 0U)
        {
            return std::nullopt;
        }
        offset += read;
    }
    std::uint8_t extra = 0;
    DWORD extraRead = 0;
    if (!ReadFile(fileHandle.get(), &extra, 1U, &extraRead, nullptr)
        || extraRead != 0U)
    {
        return std::nullopt;
    }
    LARGE_INTEGER finalSize{};
    if (!GetFileSizeEx(fileHandle.get(), &finalSize)
        || finalSize.QuadPart != size.QuadPart)
    {
        return std::nullopt;
    }
    return result;
}
#else
class UniqueFileDescriptor
{
public:
    explicit UniqueFileDescriptor(int value = -1) noexcept : value_(value) {}
    ~UniqueFileDescriptor()
    {
        if (value_ >= 0)
            ::close(value_);
    }
    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1))
    {
    }
    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            if (value_ >= 0)
                ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }

private:
    int value_ = -1;
};

[[nodiscard]] std::optional<ContainedFileSnapshot> readBoundedContainedFile(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::size_t maximumBytes)
{
    if (!validContainedRelativePath(root, relative))
        return std::nullopt;
    UniqueFileDescriptor current(::open(
        root.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!current)
        return std::nullopt;

    auto iterator = relative.begin();
    while (iterator != relative.end())
    {
        const auto next = std::next(iterator);
        const auto flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
            | (next == relative.end() ? 0 : O_DIRECTORY);
        UniqueFileDescriptor candidate(::openat(
            current.get(), iterator->c_str(), flags));
        if (!candidate)
            return std::nullopt;
        struct stat status{};
        if (::fstat(candidate.get(), &status) != 0
            || (next == relative.end()
                    ? !S_ISREG(status.st_mode)
                    : !S_ISDIR(status.st_mode)))
        {
            return std::nullopt;
        }
        current = std::move(candidate);
        iterator = next;
    }

    struct stat before{};
    if (::fstat(current.get(), &before) != 0 || before.st_size < 0
        || static_cast<std::uint64_t>(before.st_size) > maximumBytes)
    {
        return std::nullopt;
    }
    ContainedFileSnapshot result;
    result.diagnosticPath = root / relative;
    result.bytes.resize(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < result.bytes.size())
    {
        const auto count = ::read(
            current.get(),
            result.bytes.data() + offset,
            result.bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::nullopt;
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra = 0;
    ssize_t extraCount = 0;
    do
    {
        extraCount = ::read(current.get(), &extra, 1U);
    } while (extraCount < 0 && errno == EINTR);
    if (extraCount != 0)
        return std::nullopt;
    struct stat after{};
    if (::fstat(current.get(), &after) != 0
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_size != after.st_size)
    {
        return std::nullopt;
    }
    return result;
}
#endif

[[nodiscard]] std::optional<std::string> readBoundedFile(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::size_t maximumBytes)
{
    auto snapshot = readBoundedContainedFile(root, relative, maximumBytes);
    if (!snapshot.has_value())
        return std::nullopt;
    if (snapshot->bytes.empty())
        return std::string{};
    return std::string{
        reinterpret_cast<const char*>(snapshot->bytes.data()),
        snapshot->bytes.size()};
}

[[nodiscard]] bool hasExactMembers(
    const JsonValue& value,
    std::span<const std::string_view> expected)
{
    if (!value.isObject() || value.object().size() != expected.size())
        return false;
    std::unordered_set<std::string_view> seen;
    seen.reserve(value.object().size());
    for (const auto& [name, _] : value.object())
    {
        if (!seen.emplace(name).second
            || std::ranges::find(expected, name) == expected.end())
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const JsonValue* member(
    const JsonValue& value,
    std::string_view name)
{
    return talas::project::json::findObjectMember(value, name);
}

[[nodiscard]] bool validBoundedText(
    const JsonValue* value,
    bool allowEmpty = false)
{
    return value != nullptr && value->isString()
        && (allowEmpty || !value->string().empty())
        && value->string().size() <= maximumSoundLibraryStringBytes
        && talas::project::json::isValidUtf8(value->string())
        && std::ranges::none_of(
            value->string(),
            [](unsigned char character) {
                return character < 0x20U || character == 0x7fU;
            });
}

[[nodiscard]] bool validIdentifier(std::string_view value)
{
    return !value.empty() && value.size() <= 128
        && std::ranges::all_of(value, [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-';
           });
}

[[nodiscard]] bool isDemoProjectPath(std::string_view value) noexcept
{
    return value.starts_with(soundLibraryDemoFolder);
}

// A project at the top of examples/ or in examples/demos/; resources/,
// DiskCache/ and deeper folders never hold library projects.
[[nodiscard]] bool validProjectPath(std::string_view value)
{
    if (!value.starts_with("examples/")
        || !value.ends_with(".talas.json")
        || value.find('\\') != std::string_view::npos
        || value.find("..") != std::string_view::npos)
    {
        return false;
    }
    const auto suffix = value.substr(
        (isDemoProjectPath(value) ? soundLibraryDemoFolder
                                  : std::string_view{"examples/"})
            .size());
    return !suffix.empty() && suffix.find('/') == std::string_view::npos;
}

[[nodiscard]] std::optional<SoundLibraryExpectedOutcome> parseOutcome(
    std::string_view value)
{
    if (value == "renderable")
        return SoundLibraryExpectedOutcome::Renderable;
    if (value == "validation-only")
        return SoundLibraryExpectedOutcome::ValidationOnly;
    if (value == "preserved-unavailable")
        return SoundLibraryExpectedOutcome::PreservedUnavailable;
    return std::nullopt;
}

[[nodiscard]] std::optional<SoundLibraryAction> parseAction(
    std::string_view value)
{
    if (value == "openExample")
        return SoundLibraryAction::OpenExample;
    if (value == "openHelp")
        return SoundLibraryAction::OpenHelp;
    if (value == "showDefinition")
        return SoundLibraryAction::ShowDefinition;
    if (value == "useInProject")
        return SoundLibraryAction::UseInProject;
    if (value == "audition")
        return SoundLibraryAction::Audition;
    return std::nullopt;
}

[[nodiscard]] bool parseManifestEntry(
    const JsonValue& value,
    SoundLibraryManifestEntry& result)
{
    constexpr std::array expected{
        std::string_view{"id"},
        std::string_view{"projectPath"},
        std::string_view{"title"},
        std::string_view{"summary"},
        std::string_view{"category"},
        std::string_view{"tags"},
        std::string_view{"author"},
        std::string_view{"origin"},
        std::string_view{"helpAnchor"},
        std::string_view{"expectedOutcome"},
        std::string_view{"definitionId"},
        std::string_view{"actions"},
    };
    if (!hasExactMembers(value, expected))
        return false;
    const auto* id = member(value, "id");
    const auto* path = member(value, "projectPath");
    const auto* title = member(value, "title");
    const auto* summary = member(value, "summary");
    const auto* category = member(value, "category");
    const auto* tags = member(value, "tags");
    const auto* author = member(value, "author");
    const auto* origin = member(value, "origin");
    const auto* helpAnchor = member(value, "helpAnchor");
    const auto* outcome = member(value, "expectedOutcome");
    const auto* definitionId = member(value, "definitionId");
    const auto* actions = member(value, "actions");
    if (!validBoundedText(id) || !validIdentifier(id->string())
        || !validBoundedText(path) || !validProjectPath(path->string())
        || !validBoundedText(title)
        || title->string().size() > maximumHelpTitleBytes
        || !validBoundedText(summary)
        || summary->string().size() > maximumHelpSummaryBytes
        || !validBoundedText(category) || !validBoundedText(author)
        || !validBoundedText(origin) || origin->string() != "repository-original"
        || !validBoundedText(helpAnchor)
        || !helpAnchor->string().starts_with('#')
        || !validBoundedText(outcome) || !tags->isArray()
        || tags->array().empty()
        || tags->array().size() > maximumSoundLibraryTags
        || !actions->isArray() || actions->array().empty())
    {
        return false;
    }
    const auto parsedOutcome = parseOutcome(outcome->string());
    if (!parsedOutcome.has_value())
        return false;

    std::unordered_set<std::string> uniqueTags;
    std::vector<std::string> parsedTags;
    parsedTags.reserve(tags->array().size());
    for (const auto& tag : tags->array())
    {
        if (!validBoundedText(&tag)
            || !uniqueTags.emplace(tag.string()).second)
        {
            return false;
        }
        parsedTags.push_back(tag.string());
    }
    const auto hasTag = [&](std::string_view tag) {
        return uniqueTags.contains(std::string{tag});
    };
    auto demoKind = SoundLibraryDemoKind::None;
    if (isDemoProjectPath(path->string()))
    {
        if (category->string() != soundLibraryDemoCategory
            || !hasTag("demo") || hasTag("effect") == hasTag("instrument"))
        {
            return false;
        }
        demoKind = hasTag("effect") ? SoundLibraryDemoKind::Effect
                                    : SoundLibraryDemoKind::Instrument;
    }
    else if (category->string() == soundLibraryDemoCategory || hasTag("demo"))
    {
        return false;
    }

    SoundLibraryActionMask actionMask = 0;
    for (const auto& action : actions->array())
    {
        if (!validBoundedText(&action))
            return false;
        const auto parsed = parseAction(action.string());
        if (!parsed.has_value())
            return false;
        const auto bit = soundLibraryActionMask(*parsed);
        if ((actionMask & bit) != 0)
            return false;
        actionMask |= bit;
    }
    if (!soundLibraryHasAction(actionMask, SoundLibraryAction::OpenExample)
        || !soundLibraryHasAction(actionMask, SoundLibraryAction::OpenHelp)
        || (*parsedOutcome != SoundLibraryExpectedOutcome::Renderable
            && soundLibraryHasAction(actionMask, SoundLibraryAction::Audition))
        || (*parsedOutcome == SoundLibraryExpectedOutcome::PreservedUnavailable
            && soundLibraryHasAction(actionMask, SoundLibraryAction::UseInProject))
        || (demoKind == SoundLibraryDemoKind::Effect
            && soundLibraryHasAction(actionMask, SoundLibraryAction::Audition)))
    {
        return false;
    }

    std::optional<std::string> parsedDefinitionId;
    if (definitionId->isString())
    {
        if (!validBoundedText(definitionId)
            || !validIdentifier(definitionId->string()))
        {
            return false;
        }
        parsedDefinitionId = definitionId->string();
    }
    else if (!definitionId->isNull())
    {
        return false;
    }
    if (soundLibraryHasAction(actionMask, SoundLibraryAction::UseInProject)
        != parsedDefinitionId.has_value())
    {
        return false;
    }

    result = {
        id->string(),
        path->string(),
        title->string(),
        summary->string(),
        category->string(),
        std::move(parsedTags),
        author->string(),
        origin->string(),
        helpAnchor->string(),
        *parsedOutcome,
        std::move(parsedDefinitionId),
        actionMask,
        demoKind,
    };
    return true;
}

struct IndexedExampleEvidence
{
    SoundLibraryExpectedOutcome outcome = SoundLibraryExpectedOutcome::Renderable;
    std::unordered_set<std::string> registeredTypeIds;
    std::unordered_set<std::string> preservedTypeIds;
};

[[nodiscard]] std::optional<std::unordered_set<std::string>> stringSet(
    const JsonValue* value)
{
    if (value == nullptr || !value->isArray())
        return std::nullopt;
    std::unordered_set<std::string> result;
    result.reserve(value->array().size());
    for (const auto& item : value->array())
    {
        if (!validBoundedText(&item)
            || !result.emplace(item.string()).second)
        {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] std::optional<
    std::unordered_map<std::string, IndexedExampleEvidence>> indexEvidence(
    const std::filesystem::path& root)
{
    const auto text = readBoundedFile(
        root, "docs/help/node-example-index.json", maximumIndexBytes);
    if (!text.has_value())
        return std::nullopt;
    talas::project::json::ParseLimits limits;
    limits.maximumInputBytes = maximumIndexBytes;
    limits.maximumNestingDepth = 16;
    limits.maximumValues = 65'536;
    limits.maximumDecodedStringBytes = maximumIndexBytes;
    limits.maximumContainerElements = 16'384;
    limits.requireValidUtf8 = true;
    const auto parsed = talas::project::json::parse(*text, limits);
    constexpr std::array rootMembers{
        std::string_view{"schemaVersion"},
        std::string_view{"generatedBy"},
        std::string_view{"authority"},
        std::string_view{"nonClaims"},
        std::string_view{"catalogPath"},
        std::string_view{"examples"},
        std::string_view{"nodes"},
    };
    if (!parsed.ok() || !hasExactMembers(parsed.value, rootMembers))
        return std::nullopt;
    const auto* schema = member(parsed.value, "schemaVersion");
    const auto* generatedBy = member(parsed.value, "generatedBy");
    const auto* authority = member(parsed.value, "authority");
    const auto* nonClaims = member(parsed.value, "nonClaims");
    const auto* catalogPath = member(parsed.value, "catalogPath");
    const auto* examples = member(parsed.value, "examples");
    const auto* nodes = member(parsed.value, "nodes");
    const auto claims = stringSet(nonClaims);
    if (schema == nullptr || !schema->isNumber() || schema->number() != 1.0
        || !validBoundedText(generatedBy)
        || generatedBy->string() != "tools/report_node_help.py"
        || !validBoundedText(authority)
        || authority->string()
            != "NodeRegistry catalog snapshot plus repository-original examples"
        || !validBoundedText(catalogPath)
        || catalogPath->string()
            != "docs/coverage/builtin-node-catalog-v2.json"
        || !claims.has_value()
        || *claims != std::unordered_set<std::string>{
            "semantic parity",
            "tutorial completeness",
            "hosted or embedded help availability",
        }
        || examples == nullptr || !examples->isArray()
        || nodes == nullptr || !nodes->isArray() || nodes->array().empty())
    {
        return std::nullopt;
    }
    std::unordered_map<std::string, IndexedExampleEvidence> result;
    result.reserve(examples->array().size());
    for (const auto& example : examples->array())
    {
        constexpr std::array exampleMembers{
            std::string_view{"path"},
            std::string_view{"title"},
            std::string_view{"expectedOutcome"},
            std::string_view{"nodeTypeIds"},
            std::string_view{"preservedUnregisteredTypeIds"},
        };
        if (!hasExactMembers(example, exampleMembers))
            return std::nullopt;
        const auto* path = member(example, "path");
        const auto* title = member(example, "title");
        const auto* outcome = member(example, "expectedOutcome");
        const auto registered = stringSet(member(example, "nodeTypeIds"));
        const auto preserved = stringSet(
            member(example, "preservedUnregisteredTypeIds"));
        const auto parsedOutcome = validBoundedText(outcome)
            ? parseOutcome(outcome->string())
            : std::nullopt;
        if (!validBoundedText(path) || !validProjectPath(path->string())
            || !validBoundedText(title) || !parsedOutcome.has_value()
            || !registered.has_value() || !preserved.has_value()
            || !result.emplace(
                    path->string(),
                    IndexedExampleEvidence{
                        *parsedOutcome, *registered, *preserved})
                    .second)
        {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] std::string markdownAnchor(std::string_view heading)
{
    std::string result;
    result.reserve(heading.size());
    bool pendingHyphen = false;
    for (const auto character : heading)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '-')
        {
            if (pendingHyphen && !result.empty() && result.back() != '-')
                result.push_back('-');
            pendingHyphen = false;
            result.push_back(static_cast<char>(std::tolower(byte)));
        }
        else if (std::isspace(byte) != 0)
        {
            pendingHyphen = true;
        }
    }
    return result;
}

// The anchors of a Markdown file's headings; a "#" line inside a fenced
// code block is a shell comment, not a heading.
[[nodiscard]] std::unordered_set<std::string> headingAnchors(
    std::string_view markdown)
{
    std::unordered_set<std::string> anchors;
    std::size_t offset = 0;
    bool fenced = false;
    while (offset < markdown.size())
    {
        const auto end = markdown.find('\n', offset);
        const auto line = markdown.substr(
            offset,
            (end == std::string_view::npos ? markdown.size() : end) - offset);
        if (line.starts_with("```"))
            fenced = !fenced;
        const auto firstText = line.find_first_not_of('#');
        if (!fenced && line.starts_with('#')
            && firstText != std::string_view::npos)
        {
            const auto heading = line.substr(firstText);
            const auto firstCharacter = heading.find_first_not_of(' ');
            if (firstCharacter != std::string_view::npos)
            {
                auto anchor = markdownAnchor(heading.substr(firstCharacter));
                if (!anchor.empty())
                    anchors.emplace(std::move(anchor));
            }
        }
        if (end == std::string_view::npos)
            break;
        offset = end + 1;
    }
    return anchors;
}

[[nodiscard]] std::optional<std::unordered_set<std::string>> helpAnchors(
    const std::filesystem::path& root,
    const talas::core::NodeRegistry& registry)
{
    const auto readme = readBoundedFile(
        root, "examples/README.md", maximumHelpBytes);
    const auto reference = readBoundedFile(
        root, "docs/help/builtin-node-reference.md", maximumHelpBytes);
    if (!readme.has_value() || !reference.has_value()
        || readme->find(
            "`Open Help` projects the exact checked heading\n"
            "and expected outcome into local Context Help; it does not open a hosted page,\n"
            "copy third-party material, or claim tutorial completeness.") == std::string::npos
        || reference->find(
            "This is a descriptor reference, not a parity claim, semantic tutorial,\n"
            "hosted help service, or guarantee that every node has an example.")
            == std::string::npos
        || reference->find("<!-- Generated by tools/report_node_help.py; do not edit by hand. -->")
            == std::string::npos)
    {
        return std::nullopt;
    }
    for (const auto& descriptor : registry.nodeTypes())
    {
        if (descriptor.provenance.origin != talas::core::NodeOrigin::BuiltIn)
            continue;
        if (reference->find("<code>" + descriptor.typeId + "</code>")
            == std::string::npos)
        {
            return std::nullopt;
        }
    }
    return headingAnchors(*readme);
}

// The demo README's headings: a demo's help anchor names its table there.
[[nodiscard]] std::optional<std::unordered_set<std::string>> demoHelpAnchors(
    const std::filesystem::path& root)
{
    const auto readme = readBoundedFile(
        root, "examples/demos/README.md", maximumHelpBytes);
    if (!readme.has_value())
        return std::nullopt;
    return headingAnchors(*readme);
}

[[nodiscard]] bool readsLiveAudioInput(
    const talas::project::ProjectDocument& document)
{
    const auto reads = [](const talas::project::ProjectGraph& graph) {
        return std::ranges::any_of(graph.nodes, [](const auto& node) {
            return node.typeId == "talas.audioInput";
        });
    };
    return reads(document.graph)
        || std::ranges::any_of(
            document.soundDefinitions,
            [&](const auto& definition) { return reads(definition.graph); });
}

[[nodiscard]] const talas::project::ProjectSoundDefinition* findDefinition(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] bool declaredExampleAssetsMatch(
    const std::filesystem::path& root,
    std::string_view projectPath,
    talas::project::ProjectDocument& document)
{
    const auto projectRelative = std::filesystem::path{projectPath}.parent_path();
    const auto projectDirectory = (root / projectRelative);
    for (auto& asset : document.assets)
    {
        if (!talas::project::isCanonicalProjectAssetContentHash(
                asset.contentHash))
        {
            return false;
        }
        const std::filesystem::path relative{asset.path};
        if (!validContainedRelativePath(root, relative))
            return false;
        auto snapshot = readBoundedContainedFile(
            root,
            projectRelative / relative,
            maximumExampleAssetBytes);
        if (!snapshot.has_value())
            return false;
        auto inspected = talas::project::inspectProjectAsset(
            asset,
            snapshot->bytes,
            snapshot->diagnosticPath);
        if (!inspected.ok())
            return false;
        asset = std::move(inspected.asset);
    }
    return true;
}

[[nodiscard]] bool indexedTypesMatch(
    const IndexedExampleEvidence& evidence,
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    std::unordered_set<std::string> registered;
    std::unordered_set<std::string> preserved;
    const auto addGraph = [&](const talas::project::ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            if (node.soundDefinitionId.has_value())
                continue;
            (registry.findNodeType(node.typeId) == nullptr
                    ? preserved
                    : registered)
                .emplace(node.typeId);
        }
    };
    addGraph(document.graph);
    for (const auto& definition : document.soundDefinitions)
        addGraph(definition.graph);
    return registered == evidence.registeredTypeIds
        && preserved == evidence.preservedTypeIds;
}

[[nodiscard]] std::string contentHash(std::string_view text)
{
    return talas::project::calculateProjectAssetSha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size()});
}

[[nodiscard]] SoundLibraryExampleSnapshotResult validateExampleSnapshot(
    const std::filesystem::path& root,
    const SoundLibraryManifestEntry& entry,
    std::string text,
    const talas::core::NodeRegistry& registry)
{
    const auto digest = contentHash(text);
    if (entry.verifiedProjectContentHash.empty()
        || digest != entry.verifiedProjectContentHash)
    {
        return {{}, "Sound library example bytes changed after verification"};
    }
    auto project = talas::project::parseProjectDocument(text, registry);
    if (!project.ok())
        return {{}, "Sound library example does not parse canonically"};
    if (!declaredExampleAssetsMatch(root, entry.projectPath, project.document))
        return {{}, "Sound library example asset bytes are missing, unsafe, or changed"};

    const auto structural = talas::project::validateProjectDocument(
        project.document, registry);
    const auto availability =
        talas::project::validateProjectComponentAvailabilityForExecution(
            project.document, registry);
    if (entry.expectedOutcome == SoundLibraryExpectedOutcome::Renderable
        && (!structural.ok() || !availability.ok()))
    {
        return {{}, "Renderable Sound library example is not executable"};
    }
    if (entry.expectedOutcome == SoundLibraryExpectedOutcome::ValidationOnly
        && !structural.ok())
    {
        return {{}, "Validation-only Sound library example is structurally invalid"};
    }
    if (entry.expectedOutcome
            == SoundLibraryExpectedOutcome::PreservedUnavailable
        && availability.ok())
    {
        return {{}, "Preserved-unavailable example unexpectedly became executable"};
    }
    if (entry.definitionId.has_value()
        && findDefinition(project.document, *entry.definitionId) == nullptr)
    {
        return {{}, "Sound library example definition target is missing"};
    }
    return {SoundLibraryExampleSnapshot{
                std::move(project.document), std::move(digest)},
        {}};
}

struct DefinitionClassification
{
    SoundLibraryCompatibility compatibility = SoundLibraryCompatibility::Ready;
    SoundLibraryHealth assetHealth = SoundLibraryHealth::Healthy;
};

[[nodiscard]] DefinitionClassification classifyDefinition(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectSoundDefinition& definition,
    const talas::core::NodeRegistry& registry,
    std::unordered_set<std::string>& visiting)
{
    if (!visiting.emplace(definition.definitionId).second)
        return {SoundLibraryCompatibility::Malformed, SoundLibraryHealth::Invalid};
    DefinitionClassification result;
    const auto merge = [&](DefinitionClassification nested) {
        if (nested.compatibility != SoundLibraryCompatibility::Ready
            && result.compatibility == SoundLibraryCompatibility::Ready)
        {
            result.compatibility = nested.compatibility;
        }
        if (nested.assetHealth != SoundLibraryHealth::Healthy)
            result.assetHealth = nested.assetHealth;
    };
    for (const auto& node : definition.graph.nodes)
    {
        if (node.soundDefinitionId.has_value())
        {
            const auto* nested = findDefinition(document, *node.soundDefinitionId);
            if (nested == nullptr)
            {
                result.compatibility = SoundLibraryCompatibility::MissingDependency;
                continue;
            }
            merge(classifyDefinition(document, *nested, registry, visiting));
            continue;
        }
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr || node.preservedComponent.has_value())
        {
            result.compatibility = SoundLibraryCompatibility::MissingDependency;
            continue;
        }
        if (result.compatibility == SoundLibraryCompatibility::Ready
            && descriptor->capabilities.preparedRealtime
            == talas::core::ExecutionSupport::Unsupported)
        {
            result.compatibility = SoundLibraryCompatibility::UnsupportedRuntime;
        }
        for (const auto& parameter : node.parameters)
        {
            if (!parameter.resource.has_value())
                continue;
            const auto asset = std::ranges::find(
                document.assets,
                *parameter.resource,
                &talas::project::ProjectAsset::id);
            if (asset == document.assets.end())
            {
                result.assetHealth = SoundLibraryHealth::Unavailable;
                if (result.compatibility == SoundLibraryCompatibility::Ready)
                    result.compatibility = SoundLibraryCompatibility::MissingAsset;
            }
        }
    }
    visiting.erase(definition.definitionId);
    return result;
}

[[nodiscard]] SoundLibraryHealth healthFor(
    SoundLibraryCompatibility compatibility) noexcept
{
    switch (compatibility)
    {
    case SoundLibraryCompatibility::Ready:
        return SoundLibraryHealth::Healthy;
    case SoundLibraryCompatibility::ValidationOnly:
        return SoundLibraryHealth::Warning;
    case SoundLibraryCompatibility::MissingDependency:
    case SoundLibraryCompatibility::MissingAsset:
    case SoundLibraryCompatibility::UnsupportedRuntime:
    case SoundLibraryCompatibility::PreservedUnavailable:
        return SoundLibraryHealth::Unavailable;
    case SoundLibraryCompatibility::Malformed:
        return SoundLibraryHealth::Invalid;
    }
    return SoundLibraryHealth::Invalid;
}

void finishSearchIndex(SoundLibraryEntry& entry)
{
    appendTerm(entry.searchIndex, entry.stableId);
    appendTerm(entry.searchIndex, entry.sourceIdentity);
    appendTerm(entry.searchIndex, entry.displayName);
    appendTerm(entry.searchIndex, entry.summary);
    appendTerm(entry.searchIndex, entry.category);
    appendTerm(entry.searchIndex, entry.author);
    appendTerm(entry.searchIndex, entry.origin);
    for (const auto& tag : entry.tags)
        appendTerm(entry.searchIndex, tag);
    entry.searchIndex = folded(entry.searchIndex);
}
}

std::string_view soundLibraryDemoKindLabel(
    SoundLibraryDemoKind kind) noexcept
{
    switch (kind)
    {
    case SoundLibraryDemoKind::Effect:
        return "Effect";
    case SoundLibraryDemoKind::Instrument:
        return "Instrument";
    case SoundLibraryDemoKind::None:
        break;
    }
    return {};
}

SoundLibraryDefinitionAuditionProfile
soundLibraryDefinitionAuditionProfile(
    const talas::project::ProjectSoundDefinition& definition)
{
    SoundLibraryDefinitionAuditionProfile result;
    if (definition.inputs.size() > maximumSoundLibraryAuditionInputs)
    {
        result.diagnostic =
            "Reusable Sound exceeds the closed audition input bound";
        return result;
    }
    result.inputs = definition.inputs;
    for (const auto& input : definition.inputs)
    {
        if (input.type != talas::core::PortType::Audio
            && input.type != talas::core::PortType::Control)
        {
            result.diagnostic =
                "Event, NoteStream, resource, and spectral audition inputs are unsupported";
            return result;
        }
    }
    result.outputChannelCount = static_cast<std::size_t>(
        std::ranges::count(
            definition.outputs,
            talas::core::PortType::Audio,
            &talas::project::ProjectSoundPort::type));
    if (result.outputChannelCount == 0)
    {
        result.diagnostic =
            "Reusable Sound has no Audio output to audition";
        return result;
    }
    if (result.outputChannelCount > maximumSoundLibraryAuditionOutputs)
    {
        result.outputChannelCount = 0;
        result.diagnostic =
            "Reusable Sound exceeds the closed audition output bound";
        return result;
    }
    return result;
}

std::size_t soundLibraryProjectOutputChannelCount(
    const talas::project::ProjectDocument& document) noexcept
{
    std::size_t result = 0;
    for (const auto& node : document.graph.nodes)
    {
        std::size_t channels = 0;
        for (const auto input : talas::core::terminalAudioInputs(node.typeId))
        {
            if (!input.optional || !input.fallbackPortId.empty() || std::ranges::any_of(
                    document.graph.connections, [&](const auto& connection) {
                        return connection.toNode == node.instanceId
                            && connection.toPort == input.portId;
                    }))
                ++channels;
        }
        if (node.typeId == "talas.diskRecorder")
        {
            const auto parameter = [&](std::string_view id) {
                return std::ranges::find(
                    node.parameters,
                    id,
                    &talas::project::ProjectParameterExpression::parameterId);
            };
            const auto format = parameter("format");
            const auto wordSize = parameter("wordSize");
            const auto layout = parameter("channels");
            const auto supportedText = [&](const auto candidate,
                                           std::string_view expected) {
                return candidate == node.parameters.end()
                    || (candidate->text.has_value()
                        && (candidate->text->empty()
                            || *candidate->text == expected));
            };
            const auto mono = layout == node.parameters.end()
                || (layout->choice.has_value()
                    && *layout->choice == "mono");
            const auto stereo = layout != node.parameters.end()
                && layout->choice.has_value()
                && *layout->choice == "stereo";
            // Mono or stereo, in 8-, 16- and 24-bit words.
            if (!supportedText(format, "WAV")
                || !(supportedText(wordSize, "16") || supportedText(wordSize, "8")
                    || supportedText(wordSize, "24"))
                || !(mono || stereo))
            {
                channels = 0;
            }
            else if (stereo)
            {
                channels = 2;
            }
        }
        if (channels > maximumSoundLibraryAuditionOutputs - result)
            return maximumSoundLibraryAuditionOutputs + 1;
        result += channels;
    }
    return result;
}

[[nodiscard]] bool soundLibraryProjectHasBoundedTerminalEvents(
    const talas::project::ProjectDocument& document) noexcept
{
    std::size_t terminalCount = 0;
    bool hasBoundedEventTerminal = false;
    for (const auto& node : document.graph.nodes)
    {
        if (!talas::project::isTerminalNodeType(node.typeId))
            continue;
        ++terminalCount;
        hasBoundedEventTerminal = hasBoundedEventTerminal
            || node.typeId == "talas.soundToGlobalController"
            || node.typeId == "talas.triggeredSoundToGlobalController"
            || node.typeId == "talas.midiFileEcho"
            || node.typeId == "talas.midiOutputEventInBytes"
            || node.typeId == "talas.midiOutputController"
            || node.typeId == "talas.midiOutputEvent"
            || node.typeId == "talas.midiOutputTimingClock"
            || node.typeId == "talas.midiOutputStartStopMasterClock";
    }
    return terminalCount == 1 && hasBoundedEventTerminal;
}

SoundLibraryManifestLoadResult loadSoundLibraryManifest(
    const std::filesystem::path& root,
    const talas::core::NodeRegistry& registry)
{
    if (root.empty() || !root.is_absolute())
        return {{}, "Sound library root must be absolute"};
    const auto text = readBoundedFile(
        root, "examples/sound-library-manifest.json", maximumManifestBytes);
    if (!text.has_value())
        return {{}, "Sound library manifest is missing or exceeds its bound"};

    auto loaded = parseSoundLibraryManifest(*text);
    if (!loaded.ok())
        return loaded;
    auto manifest = std::move(*loaded.manifest);
    const auto generatedEvidence = indexEvidence(root);
    const auto verifiedHelpAnchors = helpAnchors(root, registry);
    const auto hasDemos = std::ranges::any_of(
        manifest.entries, [](const auto& entry) {
            return entry.demoKind != SoundLibraryDemoKind::None;
        });
    const auto verifiedDemoHelpAnchors = hasDemos
        ? demoHelpAnchors(root)
        : std::optional<std::unordered_set<std::string>>{
            std::unordered_set<std::string>{}};
    if (!generatedEvidence.has_value() || !verifiedHelpAnchors.has_value()
        || !verifiedDemoHelpAnchors.has_value())
    {
        return {{}, "Generated node-example index or local help is missing or invalid"};
    }
    std::unordered_set<std::string> paths;
    paths.reserve(manifest.entries.size());
    for (auto& entry : manifest.entries)
    {
        const auto indexed = generatedEvidence->find(entry.projectPath);
        const auto& anchors =
            entry.demoKind == SoundLibraryDemoKind::None
            ? *verifiedHelpAnchors
            : *verifiedDemoHelpAnchors;
        if (!paths.emplace(entry.projectPath).second
            || indexed == generatedEvidence->end()
            || indexed->second.outcome != entry.expectedOutcome
            || entry.helpAnchor.size() < 2
            || !anchors.contains(entry.helpAnchor.substr(1)))
        {
            return {{}, "Sound library entry is duplicate, unindexed, or has no exact local help anchor"};
        }
        const auto projectText = readBoundedFile(
            root, entry.projectPath, maximumExampleBytes);
        if (!projectText.has_value())
            return {{}, "Sound library example is missing or exceeds its bound"};
        entry.verifiedProjectContentHash = contentHash(*projectText);
        const auto snapshot = validateExampleSnapshot(
            root, entry, std::move(*projectText), registry);
        if (!snapshot.ok())
            return {{}, snapshot.diagnostic};
        if (!indexedTypesMatch(
                indexed->second, snapshot.snapshot->document, registry))
        {
            return {{}, "Generated node-example index type evidence differs from the exact project"};
        }
        if (entry.demoKind != SoundLibraryDemoKind::None
            && (entry.demoKind == SoundLibraryDemoKind::Effect)
                != readsLiveAudioInput(snapshot.snapshot->document))
        {
            return {{}, "Sound Library demo kind differs from the project's live audio input"};
        }
        if (entry.definitionId.has_value())
        {
            const auto* definition = findDefinition(
                snapshot.snapshot->document,
                *entry.definitionId);
            if (definition == nullptr)
                return {{}, "Sound library example definition target is missing"};
            const auto profile =
                soundLibraryDefinitionAuditionProfile(*definition);
            if (soundLibraryHasAction(
                    entry.actions,
                    SoundLibraryAction::Audition)
                && !profile.ok())
            {
                return {{}, profile.diagnostic};
            }
            entry.auditionInputs = profile.inputs;
        }
        else if (soundLibraryHasAction(
                     entry.actions,
                     SoundLibraryAction::Audition))
        {
            const auto outputCount =
                soundLibraryProjectOutputChannelCount(
                    snapshot.snapshot->document);
            if ((outputCount == 0
                    && !soundLibraryProjectHasBoundedTerminalEvents(
                        snapshot.snapshot->document))
                || outputCount > maximumSoundLibraryAuditionOutputs)
            {
                return {{},
                    "Whole-example audition has no closed Audio output profile"};
            }
        }
        for (const auto& asset : snapshot.snapshot->document.assets)
        {
            if (asset.kind == talas::project::ProjectAssetKind::Audio
                && talas::project::isCanonicalProjectAssetContentHash(
                    asset.contentHash))
            {
                entry.auditionAudioAssetIds.push_back(asset.id);
            }
        }
        std::ranges::sort(entry.auditionAudioAssetIds);
    }
    if (paths.size() != generatedEvidence->size())
        return {{}, "Sound library manifest and generated example index differ"};
    return {std::move(manifest), {}};
}

SoundLibraryExampleSnapshotResult loadSoundLibraryExampleSnapshot(
    const std::filesystem::path& root,
    const SoundLibraryManifestEntry& entry,
    const talas::core::NodeRegistry& registry)
{
    if (root.empty() || !root.is_absolute()
        || !validProjectPath(entry.projectPath)
        || !talas::project::isCanonicalProjectAssetContentHash(
            entry.verifiedProjectContentHash))
    {
        return {{}, "Sound library example snapshot authority is invalid"};
    }
    auto text = readBoundedFile(
        root, entry.projectPath, maximumExampleBytes);
    if (!text.has_value())
        return {{}, "Sound library example is missing or exceeds its bound"};
    return validateExampleSnapshot(
        root, entry, std::move(*text), registry);
}

SoundLibraryManifestLoadResult parseSoundLibraryManifest(
    std::string_view text)
{
    talas::project::json::ParseLimits limits;
    limits.maximumInputBytes = maximumManifestBytes;
    limits.maximumNestingDepth = 10;
    limits.maximumValues = 8192;
    limits.maximumDecodedStringBytes = maximumManifestBytes;
    limits.maximumContainerElements = maximumSoundLibraryEntries;
    limits.requireValidUtf8 = true;
    const auto parsed = talas::project::json::parse(text, limits);
    constexpr std::array rootMembers{
        std::string_view{"schemaVersion"},
        std::string_view{"authority"},
        std::string_view{"entries"},
    };
    if (!parsed.ok() || !hasExactMembers(parsed.value, rootMembers))
        return {{}, "Sound library manifest root is invalid or not closed"};
    const auto* schemaVersion = member(parsed.value, "schemaVersion");
    const auto* authority = member(parsed.value, "authority");
    const auto* entries = member(parsed.value, "entries");
    if (schemaVersion == nullptr || !schemaVersion->isNumber()
        || schemaVersion->numberToken() != "1"
        || !validBoundedText(authority) || entries == nullptr
        || !entries->isArray() || entries->array().empty()
        || entries->array().size() > maximumSoundLibraryEntries)
    {
        return {{}, "Sound library manifest header is invalid"};
    }
    SoundLibraryManifest manifest;
    manifest.schemaVersion = 1;
    manifest.authority = authority->string();
    manifest.entries.reserve(entries->array().size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> paths;
    ids.reserve(entries->array().size());
    paths.reserve(entries->array().size());
    for (const auto& value : entries->array())
    {
        SoundLibraryManifestEntry entry;
        if (!parseManifestEntry(value, entry)
            || !ids.emplace(entry.id).second
            || !paths.emplace(entry.projectPath).second)
        {
            return {{}, "Sound library entry is invalid or duplicate"};
        }
        manifest.entries.push_back(std::move(entry));
    }
    return {std::move(manifest), {}};
}

std::vector<SoundLibraryEntry> buildSoundLibraryEntries(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SoundLibraryManifest& manifest)
{
    std::vector<SoundLibraryEntry> result;
    const auto descriptors = registry.nodeTypes();
    result.reserve(document.soundDefinitions.size()
        + descriptors.size() + manifest.entries.size());

    for (const auto& definition : document.soundDefinitions)
    {
        SoundLibraryEntry entry;
        entry.stableId = "project:" + definition.definitionId;
        entry.sourceKind = SoundLibrarySourceKind::ProjectDefinition;
        entry.sourceIdentity = definition.definitionId;
        entry.displayName = definition.displayName;
        entry.summary = "Project-local reusable Sound";
        entry.category = "Project Sounds";
        entry.tags = {"project", "reusable-sound"};
        entry.author = "Current Project";
        entry.origin = "project";
        std::unordered_set<std::string> visiting;
        const auto classification = classifyDefinition(
            document, definition, registry, visiting);
        entry.compatibility = classification.compatibility;
        entry.dependencyHealth = healthFor(entry.compatibility);
        entry.assetHealth = classification.assetHealth;
        entry.actions = soundLibraryActionMask(SoundLibraryAction::ShowDefinition)
            | soundLibraryActionMask(SoundLibraryAction::UseInProject);
        const auto auditionProfile =
            soundLibraryDefinitionAuditionProfile(definition);
        if (entry.compatibility == SoundLibraryCompatibility::Ready
            && auditionProfile.ok())
        {
            entry.actions |= soundLibraryActionMask(SoundLibraryAction::Audition);
        }
        else if (entry.compatibility != SoundLibraryCompatibility::Ready)
        {
            entry.actionDiagnostic = "Resolve dependencies or runtime support before audition";
        }
        else
        {
            entry.actionDiagnostic = auditionProfile.diagnostic;
        }
        entry.definitionId = definition.definitionId;
        entry.auditionInputs = auditionProfile.inputs;
        for (const auto& asset : document.assets)
        {
            if (asset.kind == talas::project::ProjectAssetKind::Audio
                && talas::project::isCanonicalProjectAssetContentHash(
                    asset.contentHash))
            {
                entry.auditionAudioAssetIds.push_back(asset.id);
            }
        }
        std::ranges::sort(entry.auditionAudioAssetIds);
        finishSearchIndex(entry);
        result.push_back(std::move(entry));
    }

    for (const auto& descriptor : descriptors)
    {
        if (descriptor.provenance.origin == talas::core::NodeOrigin::BuiltIn)
            continue;
        SoundLibraryEntry entry;
        entry.stableId = "component:" + descriptor.typeId;
        entry.sourceKind = SoundLibrarySourceKind::Component;
        entry.sourceIdentity = descriptor.typeId;
        entry.displayName = descriptor.displayName;
        entry.summary = descriptor.supportSummary;
        entry.category = descriptor.category;
        entry.tags = {"component", std::string{
            talas::core::nodeSupportLevelName(descriptor.supportLevel)}};
        entry.author = descriptor.provenance.vendor;
        entry.origin = std::string{
            talas::core::nodeOriginName(descriptor.provenance.origin)};
        entry.compatibility = descriptor.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            ? SoundLibraryCompatibility::Ready
            : descriptor.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Subset
                ? SoundLibraryCompatibility::ValidationOnly
                : SoundLibraryCompatibility::UnsupportedRuntime;
        entry.dependencyHealth = healthFor(entry.compatibility);
        entry.assetHealth = SoundLibraryHealth::Healthy;
        entry.actions = soundLibraryActionMask(SoundLibraryAction::UseInProject);
        if (entry.compatibility == SoundLibraryCompatibility::Ready)
            entry.actionDiagnostic =
                "Component audition awaits an authorized private component host";
        entry.descriptorTypeId = descriptor.typeId;
        appendTerm(entry.searchIndex, descriptorNodeCapabilitySummary(descriptor));
        for (const auto& limitation : descriptor.capabilities.limitations)
            appendTerm(entry.searchIndex, limitation);
        finishSearchIndex(entry);
        result.push_back(std::move(entry));
    }

    for (const auto& source : manifest.entries)
    {
        SoundLibraryEntry entry;
        entry.stableId = "example:" + source.id;
        entry.sourceKind = SoundLibrarySourceKind::OriginalExample;
        entry.sourceIdentity = source.id;
        entry.displayName = source.title;
        entry.summary = source.summary;
        entry.category = source.category;
        entry.tags = source.tags;
        entry.author = source.author;
        entry.origin = source.origin;
        entry.demoKind = source.demoKind;
        entry.actions = source.actions;
        if (source.demoKind == SoundLibraryDemoKind::Effect)
        {
            entry.actionDiagnostic =
                "Effects play live audio input 1: open the demo and play "
                "into input 1 (an audition here would be silent)";
        }
        entry.projectPath = source.projectPath;
        entry.definitionId = source.definitionId;
        entry.helpAnchor = source.helpAnchor;
        entry.auditionInputs = source.auditionInputs;
        entry.auditionAudioAssetIds = source.auditionAudioAssetIds;
        if (!source.verifiedProjectContentHash.empty())
        {
            entry.verifiedProjectContentHash =
                source.verifiedProjectContentHash;
        }
        switch (source.expectedOutcome)
        {
        case SoundLibraryExpectedOutcome::Renderable:
            entry.compatibility = SoundLibraryCompatibility::Ready;
            break;
        case SoundLibraryExpectedOutcome::ValidationOnly:
            entry.compatibility = SoundLibraryCompatibility::ValidationOnly;
            break;
        case SoundLibraryExpectedOutcome::PreservedUnavailable:
            entry.compatibility = SoundLibraryCompatibility::PreservedUnavailable;
            break;
        }
        entry.dependencyHealth = healthFor(entry.compatibility);
        entry.assetHealth = entry.compatibility == SoundLibraryCompatibility::Ready
            ? SoundLibraryHealth::Healthy
            : entry.dependencyHealth;
        finishSearchIndex(entry);
        result.push_back(std::move(entry));
    }

    // Demos lead the original examples, effects before instruments.
    const auto demoRank = [](SoundLibraryDemoKind kind) {
        switch (kind)
        {
        case SoundLibraryDemoKind::Effect:
            return 0;
        case SoundLibraryDemoKind::Instrument:
            return 1;
        case SoundLibraryDemoKind::None:
            break;
        }
        return 2;
    };
    std::ranges::stable_sort(result, [&](const auto& left, const auto& right) {
        if (left.sourceKind != right.sourceKind)
            return left.sourceKind < right.sourceKind;
        if (demoRank(left.demoKind) != demoRank(right.demoKind))
            return demoRank(left.demoKind) < demoRank(right.demoKind);
        if (left.displayName != right.displayName)
            return left.displayName < right.displayName;
        return left.stableId < right.stableId;
    });
    return result;
}

bool soundLibraryEntryMatches(
    const SoundLibraryEntry& entry,
    std::string_view query)
{
    const auto normalized = folded(query);
    std::size_t offset = 0;
    while (offset < normalized.size())
    {
        while (offset < normalized.size()
               && std::isspace(static_cast<unsigned char>(normalized[offset])))
        {
            ++offset;
        }
        const auto start = offset;
        while (offset < normalized.size()
               && !std::isspace(static_cast<unsigned char>(normalized[offset])))
        {
            ++offset;
        }
        if (start != offset
            && entry.searchIndex.find(normalized.substr(start, offset - start))
                == std::string::npos)
        {
            return false;
        }
    }
    return true;
}
}
