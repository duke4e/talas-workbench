#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchPluginExport.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/plugin/PluginExport.h"
#include "talas/plugin/PluginPlatform.h"
#include "talas/plugin/PluginRuntime.h"

#include <fstream>
#include <iterator>
#include <thread>

// ADR-0029: File > Export Plugin... exports the open project as a VST3 (and,
// on macOS, AU) instrument or effect, with the same exporter as
// `talas_cli export-plugin`.
namespace talas::app::workbench
{
namespace
{
namespace fs = std::filesystem;
using talas::plugin::PluginExportRequest;
using talas::plugin::PluginExportResult;
using talas::plugin::PluginFormat;
using talas::plugin::PluginKind;

// The templates inside the Workbench (Contents/Resources/PluginTemplates, or
// PluginTemplates beside it elsewhere), then the usual places.
[[nodiscard]] std::vector<fs::path> workbenchPluginTemplateDirectories()
{
    std::vector<fs::path> directories;
    const auto application = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    const auto name = juce::String{std::string{talas::plugin::pluginTemplateDirectoryName}};
#if JUCE_MAC
    directories.push_back(filesystemPath(
        application.getChildFile("Contents").getChildFile("Resources").getChildFile(name).getFullPathName()));
#else
    directories.push_back(filesystemPath(application.getParentDirectory().getChildFile(name).getFullPathName()));
#endif
    for (auto& directory : talas::plugin::defaultPluginTemplateDirectories())
        directories.push_back(std::move(directory));
    return directories;
}

// An instrument when a MIDI Voice plays live MIDI, an effect when the
// project reads live audio, else an instrument.
[[nodiscard]] PluginKind guessedPluginKind(const talas::project::ProjectDocument& document)
{
    if (talas::plugin::projectPlaysLiveMidi(document))
        return PluginKind::Instrument;
    const auto inputs = talas::plugin::projectAudioInputChannels(document);
    return std::ranges::any_of(inputs, [](bool channel) { return channel; }) ? PluginKind::Effect
                                                                           : PluginKind::Instrument;
}

[[nodiscard]] juce::String defaultPluginName(
    const talas::project::ProjectDocument& document,
    const std::optional<fs::path>& projectFile)
{
    if (!document.metadata.title.empty())
        return juceString(document.metadata.title);
    if (projectFile.has_value())
    {
        auto stem = projectFile->filename().string();
        for (const auto* suffix : {".talas.json", ".talaspkg", ".json"})
        {
            if (stem.ends_with(suffix))
                stem.resize(stem.size() - std::string_view{suffix}.size());
        }
        if (!stem.empty())
            return juceString(stem);
    }
    return "Talas Plugin";
}

[[nodiscard]] juce::String describeResult(const PluginExportResult& result)
{
    juce::String text;
    if (!result.ok())
    {
        text << juceString(result.error);
    }
    else
    {
        const auto& identity = *result.identity;
        text << juceString(identity.name) << " by " << juceString(identity.manufacturer) << " ("
             << juceString(std::string{talas::plugin::pluginKindName(identity.kind)}) << ", codes "
             << juceString(identity.manufacturerCode) << "/" << juceString(identity.pluginCode) << ", "
             << static_cast<int>(result.parameters.size()) << " parameters):\n";
        for (const auto& bundle : result.bundles)
            text << "\n" << juceString(std::string{talas::plugin::pluginFormatName(bundle.format)}) << ": "
                 << jucePathString(bundle.path);
        text << "\n\nCopy a bundle into your plug-in folder (on macOS ~/Library/Audio/Plug-Ins/VST3 or "
                "Components) and rescan in your DAW.";
    }
    for (const auto& warning : result.warnings)
        text << "\n\nNote: " << juceString(warning);
    return text;
}

// The dialog's content: the export's settings, and its result once run on a
// worker thread (an export copies bundles, stages the project and signs).
class PluginExportPanel final : public juce::Component
{
public:
    using Finished = std::function<void(const PluginExportResult&)>;

    PluginExportPanel(
        talas::project::ProjectDocument document,
        std::optional<fs::path> projectFile,
        Finished finished)
        : document_(std::move(document)), projectFile_(std::move(projectFile)), finished_(std::move(finished))
    {
        const auto addLabel = [this](juce::Label& label, const juce::String& text) {
            label.setText(text, juce::dontSendNotification);
            label.setFont(WorkbenchType::labelStrong());
            label.setColour(juce::Label::textColourId, secondaryTextColour);
            addAndMakeVisible(label);
        };
        addLabel(nameLabel_, "Name");
        addLabel(manufacturerLabel_, "Manufacturer");
        addLabel(kindLabel_, "Kind");
        addLabel(formatLabel_, "Formats");
        addLabel(folderLabel_, "Folder");
        name_.setText(defaultPluginName(document_, projectFile_), false);
        name_.setTitle("Plugin name");
        addAndMakeVisible(name_);
        manufacturer_.setText(juceString(std::string{talas::plugin::defaultPluginManufacturer}), false);
        manufacturer_.setTitle("Manufacturer");
        addAndMakeVisible(manufacturer_);
        kind_.addItem("Instrument (MIDI in, stereo out)", 1);
        kind_.addItem("Effect (stereo in and out)", 2);
        kind_.setSelectedId(guessedPluginKind(document_) == PluginKind::Instrument ? 1 : 2, juce::dontSendNotification);
        kind_.setTitle("Plugin kind");
        addAndMakeVisible(kind_);
        vst3_.setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(vst3_);
        au_.setToggleState(talas::plugin::platform::supportsAudioUnits(), juce::dontSendNotification);
        au_.setEnabled(talas::plugin::platform::supportsAudioUnits());
        addAndMakeVisible(au_);
        folder_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Talas Plugins");
        folderPath_.setText(folder_.getFullPathName(), juce::dontSendNotification);
        folderPath_.setFont(WorkbenchType::label());
        folderPath_.setMinimumHorizontalScale(0.6F);
        addAndMakeVisible(folderPath_);
        chooseFolder_.onClick = [this] { chooseFolder(); };
        addAndMakeVisible(chooseFolder_);
        licence_.setMultiLine(true);
        licence_.setReadOnly(true);
        licence_.setCaretVisible(false);
        licence_.setFont(WorkbenchType::micro());
        licence_.setText(juceString(std::string{talas::plugin::pluginExportLicenseNotice()}));
        addAndMakeVisible(licence_);
        status_.setFont(WorkbenchType::label());
        status_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(status_);
        export_.onClick = [this] { startExport(); };
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible(export_);
        addAndMakeVisible(cancel_);
        setSize(520, 380);
    }

    ~PluginExportPanel() override
    {
        if (worker_.joinable())
            worker_.join();
    }

    [[nodiscard]] PluginExportRequest request() const
    {
        PluginExportRequest request;
        request.document = document_;
        request.projectPath = projectFile_;
        request.name = name_.getText().trim().toStdString();
        request.manufacturer = manufacturer_.getText().trim().toStdString();
        request.kind = kind_.getSelectedId() == 1 ? PluginKind::Instrument : PluginKind::Effect;
        if (vst3_.getToggleState())
            request.formats.push_back(PluginFormat::Vst3);
        if (au_.getToggleState() && au_.isEnabled())
            request.formats.push_back(PluginFormat::AudioUnit);
        request.outputDirectory = filesystemPath(folder_.getFullPathName());
        request.templateDirectories = workbenchPluginTemplateDirectories();
        return request;
    }

    // The settings on a glass card over the window's backdrop, the licence
    // and the actions below it.
    void paint(juce::Graphics& graphics) override
    {
        const auto card = getLocalBounds().reduced(8).withBottom(licence_.getY() - 6).toFloat();
        WorkbenchStyle::paintPanelShadow(graphics, card, WorkbenchStyle::panelRadius());
        WorkbenchStyle::paintPanel(graphics, card, WorkbenchStyle::panelRadius());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        const auto row = [&area](juce::Label& label, juce::Component& field) {
            auto line = area.removeFromTop(28);
            label.setBounds(line.removeFromLeft(110));
            field.setBounds(line);
            area.removeFromTop(6);
        };
        row(nameLabel_, name_);
        row(manufacturerLabel_, manufacturer_);
        row(kindLabel_, kind_);
        {
            auto line = area.removeFromTop(28);
            formatLabel_.setBounds(line.removeFromLeft(110));
            vst3_.setBounds(line.removeFromLeft(90));
            au_.setBounds(line.removeFromLeft(90));
            area.removeFromTop(6);
        }
        {
            auto line = area.removeFromTop(28);
            folderLabel_.setBounds(line.removeFromLeft(110));
            chooseFolder_.setBounds(line.removeFromRight(96));
            line.removeFromRight(8);
            folderPath_.setBounds(line);
            area.removeFromTop(10);
        }
        auto buttons = area.removeFromBottom(30);
        cancel_.setBounds(buttons.removeFromRight(96));
        buttons.removeFromRight(8);
        export_.setBounds(buttons.removeFromRight(110));
        status_.setBounds(buttons);
        area.removeFromBottom(10);
        licence_.setBounds(area);
    }

private:
    void chooseFolder()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Export the plugin into", folder_);
        juce::Component::SafePointer<PluginExportPanel> safeThis(this);
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                safeThis->folder_ = chooser.getResult();
                safeThis->folderPath_.setText(safeThis->folder_.getFullPathName(), juce::dontSendNotification);
            });
    }

    void startExport()
    {
        if (worker_.joinable())
            return;
        auto request = this->request();
        export_.setEnabled(false);
        status_.setText("Exporting...", juce::dontSendNotification);
        juce::Component::SafePointer<PluginExportPanel> safeThis(this);
        worker_ = std::thread([safeThis, request = std::move(request)] {
            auto result = talas::plugin::exportPlugin(request);
            juce::MessageManager::callAsync([safeThis, result = std::move(result)] {
                if (safeThis == nullptr)
                    return;
                safeThis->worker_.join();
                safeThis->export_.setEnabled(true);
                safeThis->status_.setText(result.ok() ? "Exported" : "Export failed", juce::dontSendNotification);
                if (safeThis->finished_)
                    safeThis->finished_(result);
                if (result.ok())
                    safeThis->close();
            });
        });
    }

    void close()
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
    }

    talas::project::ProjectDocument document_;
    std::optional<fs::path> projectFile_;
    Finished finished_;
    juce::Label nameLabel_, manufacturerLabel_, kindLabel_, formatLabel_, folderLabel_;
    juce::TextEditor name_, manufacturer_;
    juce::ComboBox kind_;
    juce::ToggleButton vst3_{"VST3"}, au_{"AU"};
    juce::File folder_;
    juce::Label folderPath_;
    juce::TextButton chooseFolder_{"Choose..."};
    juce::TextEditor licence_;
    juce::Label status_;
    juce::TextButton export_{"Export"}, cancel_{"Cancel"};
    std::unique_ptr<juce::FileChooser> chooser_;
    std::thread worker_;
};
}

std::unique_ptr<juce::Component> makePluginExportPanelForScreenshots(
    const talas::project::ProjectDocument& document,
    const std::optional<std::filesystem::path>& projectFile)
{
    return std::make_unique<PluginExportPanel>(document, projectFile, PluginExportPanel::Finished{});
}

void WorkbenchComponent::showExportPluginDialog()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    auto panel = std::make_unique<PluginExportPanel>(
        projectBridge_.editor().document(),
        projectPath(),
        [safeThis](const PluginExportResult& result) {
            if (safeThis == nullptr)
                return;
            safeThis->setStatus(result.ok() ? "Plugin exported: " + juceString(result.identity->name)
                                            : "Plugin export failed: " + juceString(result.error));
            juce::AlertWindow::showMessageBoxAsync(
                result.ok() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                result.ok() ? "Plugin exported" : "Plugin export failed",
                describeResult(result),
                {},
                safeThis.getComponent());
        });
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(panel.release());
    options.dialogTitle = "Export Plugin";
    options.dialogBackgroundColour = panelColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.componentToCentreAround = this;
    options.launchAsync();
}

// The command and the dialog's defaults, then, where the templates are
// built, a real export of the raw effect's project.
std::optional<std::string> WorkbenchComponent::runPluginExportSmoke(const std::filesystem::path& smokeDirectory)
{
    juce::ApplicationCommandInfo command(commandExportPlugin);
    getCommandInfo(commandExportPlugin, command);
    if (command.shortName != "Export Plugin..." || command.categoryName != "File")
        return std::string{"Export Plugin... should be a File command"};
    const auto menu = getMenuForIndex(static_cast<int>(talas::app::TalasMenu::File), {});
    bool listed = false;
    for (juce::PopupMenu::MenuItemIterator item(menu); item.next();)
        listed = listed || (item.getItem().commandManager != nullptr && item.getItem().itemID == commandExportPlugin);
    if (!listed)
        return std::string{"the File menu should list Export Plugin..."};

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto effect = talas::plugin::parsePluginProject(
        talas::plugin::defaultPluginProjectJson(PluginKind::Effect), registry, {});
    auto instrument = talas::plugin::parsePluginProject(
        talas::plugin::defaultPluginProjectJson(PluginKind::Instrument), registry, {});
    if (!effect.ok() || !instrument.ok())
        return std::string{"the raw templates' projects should parse"};
    if (guessedPluginKind(*effect.document) != PluginKind::Effect
        || guessedPluginKind(*instrument.document) != PluginKind::Instrument)
        return std::string{"Export Plugin should guess an effect from live input and an instrument from live MIDI"};
    PluginExportPanel panel(*effect.document, std::nullopt, {});
    auto request = panel.request();
    if (request.name != "Pass Through" || request.manufacturer != "Talas" || request.kind != PluginKind::Effect
        || request.formats.empty() || request.formats.front() != PluginFormat::Vst3)
        return std::string{"the Export Plugin dialog's defaults are wrong"};

    request.outputDirectory = smokeDirectory / "plugin-export";
    request.formats = {PluginFormat::Vst3};
    const auto result = talas::plugin::exportPlugin(request);
#if TALAS_WORKBENCH_PLUGIN_TEMPLATES
    if (!result.ok())
        return "the Workbench's plugin export should succeed: " + result.error;
    const auto identityFile = request.outputDirectory / "Pass Through.vst3" / "Contents" / "Resources"
        / std::string{talas::plugin::pluginIdentityFileName};
    std::ifstream stream(identityFile, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    const auto identity = talas::plugin::parsePluginIdentity(text);
    if (!identity.has_value() || identity->name != "Pass Through" || identity->kind != PluginKind::Effect)
        return std::string{"the exported plugin should carry its identity"};
    std::error_code ignored;
    std::filesystem::remove_all(request.outputDirectory, ignored);
#else
    if (result.ok() || result.error.find("template") == std::string::npos)
        return std::string{"without plugin templates, Export Plugin should say they are missing"};
#endif
    return std::nullopt;
}
}
