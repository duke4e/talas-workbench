#include "talas/plugin/PluginExport.h"

#include "project/JsonValue.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/NodeRegistry.h"
#include "talas/plugin/PluginPlatform.h"
#include "talas/plugin/PluginPlist.h"
#include "talas/plugin/PluginRuntime.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPackage.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace talas::plugin
{
namespace
{
namespace fs = std::filesystem;
namespace json = talas::project::json;

[[nodiscard]] std::optional<std::string> readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (stream.bad())
        return std::nullopt;
    return text;
}

[[nodiscard]] bool writeFile(const fs::path& path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    return !stream.fail();
}

[[nodiscard]] std::string lowerAscii(std::string_view text)
{
    std::string lower{text};
    for (auto& character : lower)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lower;
}

// The template binary's folder in a bundle's Contents and its file name.
struct BundleBinary
{
    fs::path folder;
    std::string fileName;
};

[[nodiscard]] std::string binaryArchitectureFolder(const fs::path& contents, std::string_view suffix)
{
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(contents, error))
    {
        const auto name = entry.path().filename().string();
        if (entry.is_directory(error) && name.ends_with(suffix))
            return name;
    }
    return {};
}

[[nodiscard]] std::optional<BundleBinary> findBundleBinary(
    const fs::path& bundle,
    const plist::Value* infoPlist,
    std::string& problem)
{
    const auto contents = bundle / "Contents";
    if (platform::bundlesUseInfoPlist())
    {
        const auto* executable = infoPlist != nullptr ? infoPlist->findString("CFBundleExecutable") : nullptr;
        if (executable == nullptr || executable->empty())
        {
            problem = "the template's Info.plist names no CFBundleExecutable";
            return std::nullopt;
        }
        return BundleBinary{contents / "MacOS", *executable};
    }
#if defined(_WIN32)
    const auto architecture = binaryArchitectureFolder(contents, "-win");
    const std::string extension = ".vst3";
#else
    const auto architecture = binaryArchitectureFolder(contents, "-linux");
    const std::string extension = ".so";
#endif
    if (architecture.empty())
    {
        problem = "the template has no Contents/<architecture> binary folder";
        return std::nullopt;
    }
    return BundleBinary{contents / architecture, bundle.stem().string() + extension};
}

[[nodiscard]] std::string executableFileName(std::string_view safeName)
{
#if defined(__APPLE__)
    return std::string{safeName};
#elif defined(_WIN32)
    return std::string{safeName} + ".vst3";
#else
    return std::string{safeName} + ".so";
#endif
}

[[nodiscard]] std::optional<PluginIdentity> readExportedIdentity(const fs::path& bundle)
{
    const auto text = readFile(
        bundle / "Contents" / "Resources" / fs::path{std::string{pluginIdentityFileName}});
    if (!text.has_value())
        return std::nullopt;
    return parsePluginIdentity(*text);
}

// The identities other Talas exports hold, in the output folder and the
// host folders, which a new plugin code must not repeat.
[[nodiscard]] std::vector<PluginIdentity> existingTalasExports(const fs::path& outputDirectory)
{
    std::vector<PluginIdentity> identities;
    auto folders = platform::installedPluginFolders();
    folders.insert(folders.begin(), outputDirectory);
    for (const auto& folder : folders)
    {
        std::error_code error;
        if (!fs::is_directory(folder, error))
            continue;
        std::size_t inspected = 0;
        for (const auto& entry : fs::directory_iterator(folder, error))
        {
            if (++inspected > 4096)
                break;
            const auto extension = lowerAscii(entry.path().extension().string());
            if (extension != ".vst3" && extension != ".component")
                continue;
            if (auto identity = readExportedIdentity(entry.path()))
                identities.push_back(std::move(*identity));
        }
    }
    identities.push_back(templatePluginIdentity(PluginKind::Instrument));
    identities.push_back(templatePluginIdentity(PluginKind::Effect));
    return identities;
}

[[nodiscard]] bool codesTaken(
    const std::vector<PluginIdentity>& existing,
    const PluginIdentity& candidate)
{
    return std::ranges::any_of(existing, [&](const PluginIdentity& other) {
        return other.manufacturerCode == candidate.manufacturerCode && other.pluginCode == candidate.pluginCode
            && (other.manufacturer != candidate.manufacturer || other.name != candidate.name);
    });
}

void appendJsonString(std::string& out, std::string_view text)
{
    json::appendEscapedString(out, text);
}

struct EmbeddedProject
{
    std::string text;
    // Asset files to copy: where they are now, and their path under
    // Contents/Resources.
    std::vector<std::pair<fs::path, fs::path>> assetCopies;
};

[[nodiscard]] std::optional<EmbeddedProject> embeddedProjectFor(
    talas::project::ProjectDocument document,
    const std::optional<std::string>& originalText,
    const std::optional<fs::path>& sourceProjectFile,
    const talas::core::NodeRegistry& registry,
    std::vector<std::string>& warnings,
    std::string& error)
{
    EmbeddedProject embedded;
    bool rewritten = !originalText.has_value();
    std::set<std::string> usedNames;
    for (std::size_t index = 0; index < document.assets.size(); ++index)
    {
        auto& asset = document.assets[index];
        if (asset.path.empty())
            continue;
        const auto resolved = talas::project::resolveProjectAssetPath(asset, sourceProjectFile);
        std::error_code status;
        if (!fs::exists(resolved, status))
        {
            warnings.push_back("Asset '" + asset.id + "' (" + resolved.string()
                + ") is missing; the plugin plays without it if the project allows");
            continue;
        }
        auto fileName = resolved.filename().string();
        if (fileName.empty())
            fileName = "asset";
        auto unique = fileName;
        for (std::size_t suffix = 2; usedNames.contains(lowerAscii(unique)); ++suffix)
            unique = std::to_string(suffix) + "-" + fileName;
        usedNames.insert(lowerAscii(unique));
        const auto relative = fs::path{"assets"} / fs::path{unique};
        embedded.assetCopies.emplace_back(resolved, relative);
        asset.path = relative.generic_string();
        rewritten = true;
    }
    if (rewritten)
        embedded.text = talas::project::serializeProjectDocument(document, registry);
    else
        embedded.text = *originalText;
    if (embedded.text.empty())
    {
        error = "The project could not be written into the plugin";
        return std::nullopt;
    }
    return embedded;
}

[[nodiscard]] bool copyTree(const fs::path& from, const fs::path& to, std::string& error)
{
    std::error_code status;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, status);
    if (status)
    {
        error = "Copying " + from.string() + " to " + to.string() + " failed: " + status.message();
        return false;
    }
    return true;
}

void removeQuietly(const fs::path& path)
{
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

[[nodiscard]] std::string writeEmbeddedResources(
    const fs::path& resources,
    const PluginIdentity& identity,
    const EmbeddedProject& project)
{
    std::error_code status;
    fs::create_directories(resources, status);
    if (status)
        return "Creating " + resources.string() + " failed: " + status.message();
    if (!writeFile(resources / fs::path{std::string{pluginIdentityFileName}}, serializePluginIdentity(identity)))
        return "Writing talas-plugin.json failed";
    if (!writeFile(resources / fs::path{std::string{pluginProjectFileName}}, project.text))
        return "Writing project.talas.json failed";
    for (const auto& [source, relative] : project.assetCopies)
    {
        const auto target = resources / relative;
        fs::create_directories(target.parent_path(), status);
        if (status)
            return "Creating " + target.parent_path().string() + " failed: " + status.message();
        fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, status);
        if (status)
            return "Copying asset " + source.string() + " failed: " + status.message();
    }
    return {};
}
}

std::string_view pluginFormatName(PluginFormat format) noexcept
{
    return format == PluginFormat::Vst3 ? "VST3" : "AU";
}

std::string_view pluginFormatExtension(PluginFormat format) noexcept
{
    return format == PluginFormat::Vst3 ? ".vst3" : ".component";
}

std::optional<PluginFormat> parsePluginFormat(std::string_view text) noexcept
{
    std::string lower;
    for (const auto character : text)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    if (lower == "vst3")
        return PluginFormat::Vst3;
    if (lower == "au" || lower == "auv2" || lower == "component")
        return PluginFormat::AudioUnit;
    return std::nullopt;
}

std::string_view templateBundleBaseName(PluginKind kind) noexcept
{
    return kind == PluginKind::Instrument ? "Talas Instrument" : "Talas Effect";
}

std::string auComponentName(const PluginIdentity& identity)
{
    return identity.manufacturer + ": " + identity.name;
}

std::string_view auComponentType(PluginKind kind) noexcept
{
    return kind == PluginKind::Instrument ? "aumu" : "aufx";
}

InfoPlistRewrite rewritePluginInfoPlist(
    std::string_view templatePlist,
    const PluginIdentity& identity,
    PluginFormat format,
    std::string_view executableName)
{
    InfoPlistRewrite result;
    auto parsed = plist::parse(templatePlist);
    if (!parsed.ok() || parsed.root->type() != plist::Value::Type::Dictionary)
    {
        result.error = parsed.ok() ? "the template's Info.plist is not a dictionary"
                                   : "the template's Info.plist could not be read: " + parsed.error;
        return result;
    }
    auto root = std::move(*parsed.root);
    const auto versionCode = pluginVersionCode(identity.version);
    if (!versionCode.has_value())
    {
        result.error = "the version '" + identity.version + "' is not major.minor.patch";
        return result;
    }
    root.set("CFBundleExecutable", plist::Value::string(std::string{executableName}));
    root.set("CFBundleIdentifier", plist::Value::string(identity.bundleIdentifier));
    root.set("CFBundleName", plist::Value::string(identity.name));
    if (root.find("CFBundleDisplayName") != nullptr)
        root.set("CFBundleDisplayName", plist::Value::string(identity.name));
    root.set("CFBundleShortVersionString", plist::Value::string(identity.version));
    root.set("CFBundleVersion", plist::Value::string(identity.version));
    if (root.find("NSHumanReadableCopyright") != nullptr)
        root.set("NSHumanReadableCopyright", plist::Value::string(identity.manufacturer));
    if (format == PluginFormat::AudioUnit)
    {
        auto* components = root.find("AudioComponents");
        if (components == nullptr || components->type() != plist::Value::Type::Array || components->items().empty()
            || components->items().front().type() != plist::Value::Type::Dictionary)
        {
            result.error = "the AU template's Info.plist has no AudioComponents entry";
            return result;
        }
        auto& component = components->items().front();
        const auto* templateType = component.findString("type");
        if (templateType == nullptr || *templateType != auComponentType(identity.kind))
        {
            result.error = "the AU template is a '" + (templateType != nullptr ? *templateType : std::string{"?"})
                + "' component, not the '" + std::string{auComponentType(identity.kind)} + "' a "
                + std::string{pluginKindName(identity.kind)} + " needs";
            return result;
        }
        if (component.findString("factoryFunction") == nullptr)
        {
            result.error = "the AU template's AudioComponents entry has no factoryFunction";
            return result;
        }
        component.set("name", plist::Value::string(auComponentName(identity)));
        component.set("description", plist::Value::string(identity.description.empty() ? identity.name : identity.description));
        component.set("manufacturer", plist::Value::string(identity.manufacturerCode));
        component.set("subtype", plist::Value::string(identity.pluginCode));
        component.set("version", plist::Value::integer(static_cast<std::int64_t>(*versionCode)));
        // A template holds one component; an export too.
        components->items().resize(1);
    }
    result.plist = plist::write(root);
    return result;
}

std::string vst3ModuleInfoJson(const PluginIdentity& identity)
{
    const auto manufacturer = fourCharCode(identity.manufacturerCode);
    const auto plugin = fourCharCode(identity.pluginCode);
    const auto instrument = identity.kind == PluginKind::Instrument;
    // JUCE gives both classes the plugin's flags and category: Instrument|Synth
    // with simple mode for an instrument, Fx for an effect.
    const auto classFlags = instrument ? "2" : "0";
    const auto subCategories = instrument ? std::vector<std::string_view>{"Instrument", "Synth"}
                                          : std::vector<std::string_view>{"Fx"};
    std::string out = "{\n  \"Name\": ";
    appendJsonString(out, identity.name);
    out += ",\n  \"Version\": ";
    appendJsonString(out, identity.version);
    out += ",\n  \"Factory Info\": {\n    \"Vendor\": ";
    appendJsonString(out, identity.manufacturer);
    out += ",\n    \"URL\": \"\",\n    \"E-Mail\": \"\",\n    \"Flags\": {\n"
           "      \"Unicode\": true,\n      \"Classes Discardable\": false,\n"
           "      \"Component Non Discardable\": false\n    }\n  },\n  \"Classes\": [\n";
    const auto writeClass = [&](bool controller) {
        out += "    {\n      \"CID\": ";
        appendJsonString(out, vst3ClassIdText(manufacturer, plugin, controller));
        out += ",\n      \"Category\": ";
        appendJsonString(out, controller ? "Component Controller Class" : "Audio Module Class");
        out += ",\n      \"Name\": ";
        appendJsonString(out, identity.name);
        out += ",\n      \"Vendor\": ";
        appendJsonString(out, identity.manufacturer);
        out += ",\n      \"Version\": ";
        appendJsonString(out, identity.version);
        out += ",\n      \"SDKVersion\": ";
        appendJsonString(out, vst3SdkVersionString);
        out += ",\n      \"Sub Categories\": [";
        for (std::size_t index = 0; index < subCategories.size(); ++index)
        {
            out += index == 0 ? "\n        " : ",\n        ";
            appendJsonString(out, subCategories[index]);
        }
        out += "\n      ],\n      \"Class Flags\": ";
        out += classFlags;
        out += ",\n      \"Cardinality\": 2147483647,\n      \"Snapshots\": []\n    }";
    };
    writeClass(false);
    out += ",\n";
    writeClass(true);
    out += "\n  ]\n}\n";
    return out;
}

std::vector<std::filesystem::path> defaultPluginTemplateDirectories()
{
    std::vector<fs::path> directories;
    if (const auto* listed = std::getenv(std::string{pluginTemplateEnvironmentVariable}.c_str());
        listed != nullptr && listed[0] != '\0')
    {
#if defined(_WIN32)
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::string_view rest{listed};
        while (!rest.empty())
        {
            const auto end = rest.find(separator);
            const auto part = rest.substr(0, end);
            if (!part.empty())
                directories.emplace_back(std::string{part});
            if (end == std::string_view::npos)
                break;
            rest.remove_prefix(end + 1);
        }
    }
    if (const auto executable = platform::currentExecutablePath())
    {
        auto beside = pluginTemplateDirectoriesFor(*executable);
        directories.insert(directories.end(), beside.begin(), beside.end());
    }
    return directories;
}

std::vector<std::filesystem::path> pluginTemplateDirectoriesFor(const std::filesystem::path& executable)
{
    std::vector<fs::path> directories;
    const auto folder = executable.parent_path();
    const auto name = fs::path{std::string{pluginTemplateDirectoryName}};
    const auto inApp = fs::path{std::string{workbenchAppBundleName}} / "Contents" / "Resources" / name;
    directories.push_back(folder / name);
    directories.push_back(folder.parent_path() / "Resources" / name);
    directories.push_back(folder.parent_path() / "share" / "talas" / name);
    directories.push_back(folder.parent_path() / inApp);
#if defined(__APPLE__)
    directories.push_back(fs::path{"/Applications"} / inApp);
    if (const auto* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        directories.push_back(fs::path{home} / "Applications" / inApp);
#endif
    return directories;
}

std::optional<std::filesystem::path> findPluginTemplate(
    std::span<const std::filesystem::path> directories,
    PluginKind kind,
    PluginFormat format)
{
    const auto bundleName = std::string{templateBundleBaseName(kind)} + std::string{pluginFormatExtension(format)};
    for (const auto& directory : directories)
    {
        const auto candidate = directory / bundleName;
        std::error_code error;
        if (fs::is_directory(candidate / "Contents", error))
            return candidate;
    }
    return std::nullopt;
}

PluginExportResult exportPlugin(const PluginExportRequest& request)
{
    PluginExportResult result;
    const auto fail = [&](std::string message) {
        result.error = std::move(message);
        return result;
    };
    if (request.formats.empty())
        return fail("Choose at least one plugin format (VST3 or AU)");
    for (const auto format : request.formats)
    {
        if (format == PluginFormat::AudioUnit && !platform::supportsAudioUnits())
            return fail("AU plugins can only be exported on macOS");
    }
    if (request.outputDirectory.empty())
        return fail("Choose an output folder");

    // The identity, before anything is read or written.
    PluginIdentity identity;
    identity.name = request.name;
    identity.manufacturer = request.manufacturer.empty() ? std::string{defaultPluginManufacturer} : request.manufacturer;
    identity.kind = request.kind;
    identity.version = request.version.empty() ? std::string{defaultPluginVersion} : request.version;
    identity.manufacturerCode =
        request.manufacturerCode.empty() ? deriveManufacturerCode(identity.manufacturer) : request.manufacturerCode;
    identity.bundleIdentifier = derivePluginBundleIdentifier(identity.manufacturer, identity.name);
    identity.description = std::string{"A Talas Sound "}
        + (identity.kind == PluginKind::Instrument ? "played as an instrument" : "used as an effect");
    identity.pluginCode =
        request.pluginCode.empty() ? derivePluginCode(identity.manufacturer, identity.name) : request.pluginCode;
    identity.projectHash = "pending";
    if (auto problem = validatePluginIdentity(identity); !problem.empty())
        return fail(problem);
    std::error_code status;
    fs::create_directories(request.outputDirectory, status);
    if (status)
        return fail("The output folder " + request.outputDirectory.string() + " could not be created: " + status.message());
    if (request.pluginCode.empty())
    {
        // Another plugin already holding the derived code gets the next one.
        const auto existing = existingTalasExports(request.outputDirectory);
        for (std::uint32_t salt = 0; salt < 256 && codesTaken(existing, identity); ++salt)
            identity.pluginCode = derivePluginCode(identity.manufacturer, identity.name, salt + 1);
    }

    // The project, and the text the bundle embeds.
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::optional<fs::path> sourceProjectFile;
    std::optional<std::string> originalText;
    std::optional<talas::project::ProjectDocument> document = request.document;
    if (request.projectPath.has_value())
    {
        const auto resolved = talas::project::resolveProjectPackagePath(*request.projectPath);
        if (!resolved.ok())
            return fail("The project could not be opened: " + resolved.error);
        sourceProjectFile = resolved.projectFilePath;
        if (!document.has_value())
        {
            originalText = readFile(resolved.projectFilePath);
            if (!originalText.has_value())
                return fail("The project " + resolved.projectFilePath.string() + " could not be read");
            auto parsed = parsePluginProject(*originalText, registry, sourceProjectFile);
            if (!parsed.ok())
                return fail(parsed.error);
            document = std::move(parsed.document);
        }
    }
    if (!document.has_value())
        return fail("Choose a project to export");
    std::string embedError;
    auto embedded = embeddedProjectFor(*document, originalText, sourceProjectFile, registry, result.warnings, embedError);
    if (!embedded.has_value())
        return fail(embedError);
    identity.projectHash = stableHashHex(embedded->text);

    // Each format's bundle, built beside its destination and moved in last.
    const auto templateDirectories = request.templateDirectories.empty() ? defaultPluginTemplateDirectories()
                                                                         : request.templateDirectories;
    const auto safeName = pluginFileSystemName(identity.name);
    struct Staged
    {
        PluginFormat format;
        fs::path staging;
        fs::path destination;
    };
    std::vector<Staged> staged;
    const auto cleanUp = [&] {
        for (const auto& bundle : staged)
            removeQuietly(bundle.staging);
    };
    std::set<PluginFormat> seenFormats;
    for (const auto format : request.formats)
    {
        if (!seenFormats.insert(format).second)
            continue;
        const auto templateBundle = findPluginTemplate(templateDirectories, identity.kind, format);
        if (!templateBundle.has_value())
        {
            std::string searched;
            for (const auto& directory : templateDirectories)
                searched += "\n  " + directory.string();
            cleanUp();
            return fail("No " + std::string{pluginFormatName(format)} + " " + std::string{pluginKindName(identity.kind)}
                + " template (" + std::string{templateBundleBaseName(identity.kind)}
                + std::string{pluginFormatExtension(format)} + ") was found; searched:" + searched
                + "\nBuild the plugin templates (TALAS_BUILD_PLUGINS) or pass --template-dir");
        }
        const auto destination = request.outputDirectory / (safeName + std::string{pluginFormatExtension(format)});
        const auto staging = request.outputDirectory / ("." + safeName + std::string{pluginFormatExtension(format)} + ".export");
        removeQuietly(staging);
        staged.push_back({format, staging, destination});
        std::string copyError;
        if (!copyTree(*templateBundle, staging, copyError))
        {
            cleanUp();
            return fail(copyError);
        }
        // Earlier exports' leftovers in the template, if any, go.
        const auto resources = staging / "Contents" / "Resources";
        removeQuietly(resources / fs::path{std::string{pluginIdentityFileName}});
        removeQuietly(resources / fs::path{std::string{pluginProjectFileName}});
        removeQuietly(resources / "assets");
        removeQuietly(resources / "moduleinfo.json");

        std::optional<plist::Value> infoPlist;
        const auto infoPlistPath = staging / "Contents" / "Info.plist";
        std::optional<std::string> infoPlistText;
        if (platform::bundlesUseInfoPlist())
        {
            infoPlistText = readFile(infoPlistPath);
            if (!infoPlistText.has_value())
            {
                cleanUp();
                return fail("The template " + templateBundle->string() + " has no Info.plist");
            }
            auto parsed = plist::parse(*infoPlistText);
            if (!parsed.ok())
            {
                cleanUp();
                return fail("The template's Info.plist could not be read: " + parsed.error);
            }
            infoPlist = std::move(parsed.root);
        }
        std::string binaryProblem;
        const auto binary = findBundleBinary(staging, infoPlist ? &*infoPlist : nullptr, binaryProblem);
        if (!binary.has_value())
        {
            cleanUp();
            return fail("The template " + templateBundle->string() + " is incomplete: " + binaryProblem);
        }
        const auto executableName = executableFileName(safeName);
        if (binary->fileName != executableName)
        {
            fs::rename(binary->folder / binary->fileName, binary->folder / executableName, status);
            if (status)
            {
                cleanUp();
                return fail("Renaming the plugin binary failed: " + status.message());
            }
        }
        if (auto problem = writeEmbeddedResources(resources, identity, *embedded); !problem.empty())
        {
            cleanUp();
            return fail(problem);
        }
        if (infoPlistText.has_value())
        {
            const auto rewritten = rewritePluginInfoPlist(*infoPlistText, identity, format, executableName);
            if (!rewritten.ok())
            {
                cleanUp();
                return fail("The bundle's Info.plist could not be rewritten: " + rewritten.error);
            }
            if (!writeFile(infoPlistPath, rewritten.plist))
            {
                cleanUp();
                return fail("Writing the bundle's Info.plist failed");
            }
        }
        if (format == PluginFormat::Vst3 && !writeFile(resources / "moduleinfo.json", vst3ModuleInfoJson(identity)))
        {
            cleanUp();
            return fail("Writing moduleinfo.json failed");
        }
    }

    // The project as the plugin will load it, from inside the first bundle.
    {
        const auto resources = staged.front().staging / "Contents" / "Resources";
        auto load = loadPluginProject(resources, registry, identity.kind);
        if (!load.ok())
        {
            cleanUp();
            return fail(load.error);
        }
        if (load.projectHash != identity.projectHash)
        {
            cleanUp();
            return fail("The embedded project changed while it was written");
        }
        auto mapping = mapProjectParameters(*load.document, registry, identity.kind);
        result.parameters = std::move(mapping.parameters);
        result.warnings.insert(result.warnings.end(), mapping.warnings.begin(), mapping.warnings.end());
        const auto inputs = projectAudioInputChannels(*load.document);
        const auto readsInput = std::ranges::any_of(inputs, [](bool channel) { return channel; });
        if (identity.kind == PluginKind::Effect && !readsInput)
            result.warnings.push_back("The project reads no live audio input, so the effect ignores its track's audio");
        if (std::any_of(inputs.begin() + 2, inputs.end(), [](bool channel) { return channel; }))
            result.warnings.push_back("The project reads input channels above 2, which the plugin leaves silent");
        if (identity.kind == PluginKind::Instrument && !projectPlaysLiveMidi(*load.document))
            result.warnings.push_back(
                "No MIDI Voice or MIDI Mapper in the project plays Live MIDI; host notes reach only the "
                "!KeyPitch, !KeyDown and !KeyVelocity it reads directly");
        talas::runtime::ProjectRuntimeBridge bridge(*load.document, registry);
        PluginStageSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 512;
        for (const auto& spec : result.parameters)
            settings.eventValues.push_back({spec.eventName, spec.defaultValue, spec.domain});
        const auto stagedProject = stagePluginProject(bridge, registry, load.projectFile, settings);
        bridge.clearAfterAudioStopped();
        if (!stagedProject.ok())
        {
            cleanUp();
            return fail(stagedProject.error);
        }
        result.latencyFrames = stagedProject.latencyFrames;
    }

    // Signed, then moved in place of an earlier export of the same name.
    for (const auto& bundle : staged)
    {
        if (request.sign)
        {
            if (auto problem = platform::adHocSignBundle(bundle.staging); !problem.empty())
            {
                cleanUp();
                return fail(problem);
            }
        }
        if (fs::exists(bundle.destination, status))
        {
            if (!readExportedIdentity(bundle.destination).has_value())
            {
                cleanUp();
                return fail(bundle.destination.string()
                    + " already exists and is not a Talas export; remove it or choose another folder");
            }
            fs::remove_all(bundle.destination, status);
            if (status)
            {
                cleanUp();
                return fail("Replacing " + bundle.destination.string() + " failed: " + status.message());
            }
        }
    }
    for (const auto& bundle : staged)
    {
        fs::rename(bundle.staging, bundle.destination, status);
        if (status)
        {
            cleanUp();
            return fail("Moving the plugin to " + bundle.destination.string() + " failed: " + status.message());
        }
        result.bundles.push_back({bundle.format, bundle.destination});
    }
    result.identity = identity;
    return result;
}

std::string_view pluginExportLicenseNotice() noexcept
{
    return "An exported plugin is a copy of a Talas template, which links JUCE 8 and the CDP library in "
           "third_party/cdp. JUCE is licensed under the AGPLv3 unless you hold a commercial JUCE licence, and "
           "the CDP code under the LGPL-2.1, so sharing or selling a plugin means meeting those licences (for "
           "the AGPLv3, offering the complete source). The original Talas code is Apache-2.0.";
}
}
