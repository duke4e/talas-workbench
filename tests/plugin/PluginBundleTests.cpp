#include "project/JsonValue.h"
#include "talas/plugin/PluginExport.h"
#include "talas/plugin/PluginPlatform.h"
#include "talas/plugin/PluginPlist.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
using namespace talas::plugin;
namespace fs = std::filesystem;
namespace json = talas::project::json;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeFile(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    require(static_cast<bool>(stream), "cannot write " + path.string());
}

// The Info.plist JUCE's CMake writes for an AU template, abridged.
constexpr std::string_view auTemplatePlist = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>Talas Effect</string>
  <key>CFBundleIdentifier</key>
  <string>com.talas.plugin.template.effect</string>
  <key>CFBundleName</key>
  <string>Talas Effect</string>
  <key>CFBundleDisplayName</key>
  <string>Talas Effect</string>
  <key>CFBundlePackageType</key>
  <string>BNDL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0.0</string>
  <key>CFBundleVersion</key>
  <string>1.0.0</string>
  <key>NSHumanReadableCopyright</key>
  <string>Talas</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <!-- the component the AU registers -->
  <key>AudioComponents</key>
  <array>
    <dict>
      <key>name</key>
      <string>Talas: Talas Effect</string>
      <key>description</key>
      <string>A Talas Sound as a plugin &amp; more</string>
      <key>factoryFunction</key>
      <string>TalasEffectAUFactory</string>
      <key>manufacturer</key>
      <string>Tals</string>
      <key>type</key>
      <string>aufx</string>
      <key>subtype</key>
      <string>Tfxs</string>
      <key>version</key>
      <integer>65536</integer>
      <key>resourceUsage</key>
      <dict>
        <key>network.client</key>
        <true/>
        <key>temporary-exception.files.all.read-write</key>
        <true/>
      </dict>
      <key>sandboxSafe</key>
      <true/>
    </dict>
  </array>
  <key>Empty</key>
  <dict/>
</dict>
</plist>
)";

void testPlistRoundTrip()
{
    const auto parsed = plist::parse(auTemplatePlist);
    require(parsed.ok(), "a JUCE-style Info.plist parses: " + parsed.error);
    const auto& root = *parsed.root;
    require(root.findString("CFBundleExecutable") != nullptr && *root.findString("CFBundleExecutable") == "Talas Effect",
        "strings are read");
    const auto* components = root.find("AudioComponents");
    require(components != nullptr && components->items().size() == 1, "arrays are read");
    const auto& component = components->items().front();
    require(*component.findString("description") == "A Talas Sound as a plugin & more", "entities are decoded");
    require(component.find("version")->integerValue() == 65536, "integers are read");
    require(component.find("sandboxSafe")->booleanValue(), "booleans are read");
    const auto written = plist::write(root);
    const auto reparsed = plist::parse(written);
    require(reparsed.ok() && *reparsed.root == root, "writing and reading again gives the same plist");
    require(written.find("&amp; more") != std::string::npos, "text is escaped when written");
    require(written.find("<dict/>") != std::string::npos, "an empty dictionary is written as <dict/>");

    for (const auto* bad : {"", "<plist>", "<plist version=\"1.0\"><dict><key>a</key></dict></plist>",
             "<plist><dict><key>a</key><strin>x</strin></dict></plist>", "bplist00garbage",
             "<plist><string>a &bogus; b</string></plist>"})
    {
        require(!plist::parse(bad).ok(), std::string{"a malformed plist is rejected: "} + bad);
    }
}

[[nodiscard]] PluginIdentity exampleIdentity(PluginKind kind)
{
    PluginIdentity identity;
    identity.name = "Ring <Test>";
    identity.manufacturer = "Test Maker";
    identity.manufacturerCode = deriveManufacturerCode(identity.manufacturer);
    identity.pluginCode = derivePluginCode(identity.manufacturer, identity.name);
    identity.kind = kind;
    identity.version = "1.2.3";
    identity.bundleIdentifier = derivePluginBundleIdentifier(identity.manufacturer, identity.name);
    identity.description = "A test";
    return identity;
}

void testInfoPlistRewrite()
{
    const auto identity = exampleIdentity(PluginKind::Effect);
    const auto rewritten = rewritePluginInfoPlist(auTemplatePlist, identity, PluginFormat::AudioUnit, "Ring -Test-");
    require(rewritten.ok(), "the AU plist is rewritten: " + rewritten.error);
    const auto root = *plist::parse(rewritten.plist).root;
    require(*root.findString("CFBundleExecutable") == "Ring -Test-", "the executable is renamed");
    require(*root.findString("CFBundleIdentifier") == identity.bundleIdentifier, "the bundle identifier is the plugin's");
    require(*root.findString("CFBundleName") == "Ring <Test>" && *root.findString("CFBundleDisplayName") == "Ring <Test>",
        "the bundle names are the plugin's");
    require(*root.findString("CFBundleShortVersionString") == "1.2.3" && *root.findString("CFBundleVersion") == "1.2.3",
        "the versions are the plugin's");
    require(*root.findString("NSHumanReadableCopyright") == "Test Maker", "the copyright names the manufacturer");
    require(*root.findString("CFBundlePackageType") == "BNDL", "other keys stay");
    const auto& component = root.find("AudioComponents")->items().front();
    require(*component.findString("name") == "Test Maker: Ring <Test>", "the AU is named Manufacturer: Name");
    require(*component.findString("manufacturer") == identity.manufacturerCode, "the AU manufacturer is the code");
    require(*component.findString("subtype") == identity.pluginCode, "the AU subtype is the plugin code");
    require(*component.findString("type") == "aufx", "an effect stays aufx");
    require(component.find("version")->integerValue() == 0x010203, "the AU version is the packed version");
    require(*component.findString("factoryFunction") == "TalasEffectAUFactory", "the factory function stays");
    require(component.find("resourceUsage") != nullptr && component.find("sandboxSafe")->booleanValue(),
        "the template's sandbox keys stay");
    require(rewritten.plist.find("Ring &lt;Test&gt;") != std::string::npos, "names are escaped");

    const auto wrongKind =
        rewritePluginInfoPlist(auTemplatePlist, exampleIdentity(PluginKind::Instrument), PluginFormat::AudioUnit, "x");
    require(!wrongKind.ok() && wrongKind.error.find("'aumu'") != std::string::npos,
        "an instrument cannot be made from an aufx template: " + wrongKind.error);
    const auto vst3 = rewritePluginInfoPlist(
        "<plist><dict><key>CFBundleExecutable</key><string>Talas Effect</string></dict></plist>",
        identity, PluginFormat::Vst3, "Ring -Test-");
    require(vst3.ok() && vst3.plist.find("AudioComponents") == std::string::npos,
        "a VST3 plist needs no AudioComponents: " + vst3.error);
    const auto noComponents = rewritePluginInfoPlist(
        "<plist><dict></dict></plist>", identity, PluginFormat::AudioUnit, "x");
    require(!noComponents.ok(), "an AU plist must have AudioComponents");
}

void testModuleInfo()
{
    for (const auto kind : {PluginKind::Instrument, PluginKind::Effect})
    {
        const auto identity = exampleIdentity(kind);
        const auto text = vst3ModuleInfoJson(identity);
        const auto parsed = json::parse(text);
        require(parsed.ok() && parsed.value.isObject(), "moduleinfo.json is JSON");
        const auto& root = parsed.value;
        require(json::findObjectMember(root, "Name")->string() == identity.name, "moduleinfo names the plugin");
        require(json::findObjectMember(root, "Version")->string() == "1.2.3", "and its version");
        const auto* factory = json::findObjectMember(root, "Factory Info");
        require(factory != nullptr && json::findObjectMember(*factory, "Vendor")->string() == "Test Maker",
            "the factory's vendor is the manufacturer");
        const auto& classes = json::findObjectMember(root, "Classes")->array();
        require(classes.size() == 2, "the component and the controller");
        const auto manufacturer = fourCharCode(identity.manufacturerCode);
        const auto plugin = fourCharCode(identity.pluginCode);
        require(json::findObjectMember(classes[0], "CID")->string() == vst3ClassIdText(manufacturer, plugin, false)
                && json::findObjectMember(classes[0], "Category")->string() == "Audio Module Class",
            "the first class is the component, with JUCE's ID");
        require(json::findObjectMember(classes[1], "CID")->string() == vst3ClassIdText(manufacturer, plugin, true)
                && json::findObjectMember(classes[1], "Category")->string() == "Component Controller Class",
            "the second is the edit controller");
        const auto& categories = json::findObjectMember(classes[0], "Sub Categories")->array();
        if (kind == PluginKind::Instrument)
        {
            require(categories.size() == 2 && categories[0].string() == "Instrument" && categories[1].string() == "Synth",
                "an instrument is Instrument|Synth");
            require(json::findObjectMember(classes[0], "Class Flags")->number() == 2, "with simple mode");
        }
        else
        {
            require(categories.size() == 1 && categories[0].string() == "Fx", "an effect is Fx");
        }
        require(json::findObjectMember(classes[0], "SDKVersion")->string() == vst3SdkVersionString, "the SDK version");
    }
}

// A template bundle with a stand-in binary, enough for export's layout work.
void writeFakeTemplate(const fs::path& directory, PluginKind kind, PluginFormat format)
{
    const auto base = std::string{templateBundleBaseName(kind)};
    const auto bundle = directory / (base + std::string{pluginFormatExtension(format)});
    writeFile(bundle / "Contents" / "PkgInfo", "BNDL????");
    writeFile(bundle / "Contents" / "Resources" / "keep.txt", "template resource");
#if defined(__APPLE__)
    writeFile(bundle / "Contents" / "MacOS" / base, "not really a binary");
    std::string plistText{auTemplatePlist};
    const auto replaceAll = [&](std::string_view from, std::string_view to) {
        for (auto at = plistText.find(from); at != std::string::npos; at = plistText.find(from, at + to.size()))
            plistText.replace(at, from.size(), to);
    };
    replaceAll("Talas Effect", base);
    if (kind == PluginKind::Instrument)
        replaceAll("aufx", "aumu");
    if (format == PluginFormat::Vst3)
    {
        const auto start = plistText.find("<!-- the component");
        const auto end = plistText.find("</array>") + std::string_view{"</array>"}.size();
        plistText.erase(start, end - start);
    }
    writeFile(bundle / "Contents" / "Info.plist", plistText);
#elif defined(_WIN32)
    writeFile(bundle / "Contents" / "x86_64-win" / (base + ".vst3"), "not really a binary");
#else
    writeFile(bundle / "Contents" / "x86_64-linux" / (base + ".so"), "not really a binary");
#endif
}

void testExportLayout()
{
    const auto root = fs::temp_directory_path()
        / ("talas-plugin-bundle-tests-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto templates = root / "templates";
    std::vector<PluginFormat> formats{PluginFormat::Vst3};
    if (platform::supportsAudioUnits())
        formats.push_back(PluginFormat::AudioUnit);
    for (const auto format : formats)
        writeFakeTemplate(templates, PluginKind::Effect, format);

    const auto source = fs::path{TALAS_SOURCE_DIR} / "examples" / "demos" / "ring_modulator.talas.json";
    PluginExportRequest request;
    request.projectPath = source;
    request.name = "Ring Test";
    request.manufacturer = "Test Maker";
    request.kind = PluginKind::Effect;
    request.formats = formats;
    request.outputDirectory = root / "out";
    request.templateDirectories = {templates};
    request.version = "1.0.2";
    request.sign = false;
    const auto result = exportPlugin(request);
    require(result.ok(), "the export succeeds: " + result.error);
    require(result.bundles.size() == formats.size(), "one bundle per format");
    require(result.identity.has_value() && result.identity->manufacturerCode == "Test", "the manufacturer code is derived");
    require(result.identity->pluginCode == derivePluginCode("Test Maker", "Ring Test"), "the plugin code is derived");
    require(result.parameters.size() == 4 && result.parameters[0].eventName == "Frequency",
        "the ring modulator's four controls are its parameters");

    const auto sourceText = readFile(source);
    for (const auto& bundle : result.bundles)
    {
        const auto extension = std::string{pluginFormatExtension(bundle.format)};
        require(bundle.path == request.outputDirectory / ("Ring Test" + extension), "the bundle is named after the plugin");
        const auto resources = bundle.path / "Contents" / "Resources";
        require(readFile(resources / "keep.txt") == "template resource", "the template's resources are copied");
        require(readFile(resources / "project.talas.json") == sourceText,
            "a project without assets is embedded byte for byte");
        const auto identity = parsePluginIdentity(readFile(resources / "talas-plugin.json"));
        require(identity.has_value() && *identity == *result.identity, "talas-plugin.json holds the identity");
        require(identity->projectHash == stableHashHex(sourceText), "with the embedded project's hash");
        require(fs::exists(resources / "moduleinfo.json") == (bundle.format == PluginFormat::Vst3),
            "a VST3, and only a VST3, gets moduleinfo.json");
#if defined(__APPLE__)
        require(fs::exists(bundle.path / "Contents" / "MacOS" / "Ring Test")
                && !fs::exists(bundle.path / "Contents" / "MacOS" / "Talas Effect"),
            "the binary is renamed after the plugin");
        const auto info = plist::parse(readFile(bundle.path / "Contents" / "Info.plist"));
        require(info.ok() && *info.root->findString("CFBundleExecutable") == "Ring Test"
                && *info.root->findString("CFBundleIdentifier") == "com.talas.plugin.test-maker.ring-test",
            "the Info.plist names the renamed binary and the plugin's bundle");
        if (bundle.format == PluginFormat::AudioUnit)
        {
            const auto& component = info.root->find("AudioComponents")->items().front();
            require(*component.findString("name") == "Test Maker: Ring Test"
                    && *component.findString("subtype") == result.identity->pluginCode
                    && component.find("version")->integerValue() == 0x010002,
                "the AU's component is the plugin's");
        }
#endif
    }
    std::size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(request.outputDirectory))
        leftovers += entry.path().filename().string().starts_with(".") ? 1U : 0U;
    require(leftovers == 0, "no staging folders are left behind");

    // Exporting again replaces the earlier export.
    const auto again = exportPlugin(request);
    require(again.ok() && again.identity->pluginCode == result.identity->pluginCode,
        "re-exporting the same plugin keeps its codes and replaces it: " + again.error);

    // Another plugin whose derived code another export holds gets another.
    auto clash = request;
    clash.name = "Other Name";
    clash.pluginCode = result.identity->pluginCode;
    clash.outputDirectory = root / "clash";
    require(exportPlugin(clash).ok(), "an explicit plugin code is used as given");

    // A folder that is not a Talas export is never replaced.
    auto foreign = request;
    foreign.outputDirectory = root / "foreign";
    fs::create_directories(foreign.outputDirectory / "Ring Test.vst3");
    writeFile(foreign.outputDirectory / "Ring Test.vst3" / "mine.txt", "someone else's");
    const auto refused = exportPlugin(foreign);
    require(!refused.ok() && refused.error.find("not a Talas export") != std::string::npos,
        "an existing non-Talas bundle is not replaced: " + refused.error);
    require(readFile(foreign.outputDirectory / "Ring Test.vst3" / "mine.txt") == "someone else's", "and stays intact");

    // An instrument needs an instrument template.
    auto instrument = request;
    instrument.kind = PluginKind::Instrument;
    instrument.outputDirectory = root / "instrument";
    const auto missing = exportPlugin(instrument);
    require(!missing.ok() && missing.error.find("No VST3 instrument template") != std::string::npos,
        "a missing template is reported: " + missing.error);

    // A project that does not compile is not exported.
    const auto broken = root / "broken.talas.json";
    auto brokenText = sourceText;
    const std::string_view typeKey = "\"typeId\": \"";
    const auto at = brokenText.find(typeKey);
    require(at != std::string::npos, "the demo names node types");
    brokenText.insert(at + typeKey.size(), "talas.noSuchNode.");
    writeFile(broken, brokenText);
    auto failing = request;
    failing.projectPath = broken;
    failing.outputDirectory = root / "broken";
    const auto failed = exportPlugin(failing);
    require(!failed.ok(), "a project that does not compile fails the export");
    require(!fs::exists(failing.outputDirectory / "Ring Test.vst3"), "and leaves no bundle");

    // Assets are copied into the bundle and the project points at them.
    auto withAssets = request;
    withAssets.projectPath = fs::path{TALAS_SOURCE_DIR} / "examples" / "c33_bounded_generic_source_ram_sample.talas.json";
    withAssets.name = "Asset Test";
    withAssets.outputDirectory = root / "assets";
    const auto assets = exportPlugin(withAssets);
    require(assets.ok(), "a project with assets exports: " + assets.error);
    const auto assetResources = assets.bundles.front().path / "Contents" / "Resources";
    require(readFile(assetResources / "assets" / "c33-generic-source-sample.txt")
            == readFile(fs::path{TALAS_SOURCE_DIR} / "examples" / "resources" / "c33-generic-source-sample.txt"),
        "the asset is copied into the bundle");
    require(readFile(assetResources / "project.talas.json").find("\"assets/c33-generic-source-sample.txt\"")
            != std::string::npos,
        "the embedded project reads it from there");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}
}

// A release unpacks bin/talas_cli beside Talas Workbench.app, whose
// Resources hold the templates; the CLI finds them there by itself.
void testTemplatesBesideTheReleaseCli()
{
    const auto root = fs::temp_directory_path()
        / ("talas-release-layout-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto templates = root / "Talas-0.1.0" / std::string{workbenchAppBundleName} / "Contents" / "Resources"
        / std::string{pluginTemplateDirectoryName};
    fs::create_directories(templates / "Talas Instrument.vst3" / "Contents");
    fs::create_directories(templates / "Talas Effect.component" / "Contents");
    const auto cli = root / "Talas-0.1.0" / "bin" / "talas_cli";
    const auto directories = pluginTemplateDirectoriesFor(cli);
    const auto instrument = findPluginTemplate(directories, PluginKind::Instrument, PluginFormat::Vst3);
    const auto effect = findPluginTemplate(directories, PluginKind::Effect, PluginFormat::AudioUnit);
    require(instrument.has_value() && *instrument == templates / "Talas Instrument.vst3",
        "the release CLI should find the instrument template inside the Workbench app beside it");
    require(effect.has_value() && *effect == templates / "Talas Effect.component",
        "the release CLI should find the effect template inside the Workbench app beside it");
    // The app's own executable still finds them in its Resources first.
    const auto appExecutable = templates.parent_path().parent_path() / "MacOS" / "Talas Workbench";
    const auto fromApp = pluginTemplateDirectoriesFor(appExecutable);
    require(!fromApp.empty() && fromApp[1] == templates,
        "the Workbench should search its own Resources before other places");
    fs::remove_all(root);
}

int main()
{
    try
    {
        testPlistRoundTrip();
        testInfoPlistRewrite();
        testModuleInfo();
        testExportLayout();
        testTemplatesBesideTheReleaseCli();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "talas_plugin_bundle_tests passed\n";
    return 0;
}
