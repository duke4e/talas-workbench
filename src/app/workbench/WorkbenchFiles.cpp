#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/CompiledSounds.h"

#include "app/workbench/WorkbenchSampleEditor.h"
#include "talas/app/ExpressionsLibraryModel.h"
#include "talas/app/WorkbenchLaunch.h"
#include "talas/audio/SpectrumTables.h"
#include "talas/project/SosSpectrumFile.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#if JUCE_MAC
#include <CoreGraphics/CoreGraphics.h>
#endif

// The files Talas works with: the
// preferences file, the File Organizer, and files dragged into the
// Workbench.
namespace talas::app::workbench
{
namespace
{
// A spectrum written to a file of its own, in the format its extension
// names; false, with the reason, when it could not be.
bool writeSpectrumFile(
    const talas::project::PartialTrackAsset& spectrum, const std::filesystem::path& destination, std::string& error)
{
    const auto encoded = talas::project::encodeSpectrumFileFor(destination, spectrum);
    if (!encoded.ok())
    {
        error = "The selection could not be written as a spectrum: " + encoded.error->message;
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.bytes.data()), static_cast<std::streamsize>(encoded.bytes.size()));
    if (!output)
    {
        error = "The new spectrum file could not be written";
        return false;
    }
    return true;
}

// The shared preferences, workbenchPreferencesFileName, and the file
// they were kept in before it was named so.
std::filesystem::path workbenchPreferencesPath(std::string_view name = talas::app::workbenchPreferencesFileName)
{
    return filesystemPath(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                              .getChildFile("Talas")
                              .getChildFile(juce::String{std::string{name}})
                              .getFullPathName());
}

// The organizer: folders and the files Talas uses, colour-coded by kind and
// indented by depth; a double-click opens or closes a folder or opens a file,
// Enter opens the selected file, Ctrl+Space plays it, and the lower part
// shows its information.
class FileOrganizerPanelImpl final : public FileOrganizerPanel,
                                     public juce::DragAndDropContainer,
                                     private juce::ListBoxModel
{
public:
    FileOrganizerPanelImpl(
        std::function<void(const std::filesystem::path&)> open, std::function<bool(const std::filesystem::path&)> play)
        : open_(std::move(open))
        , play_(std::move(play))
    {
        list_.setModel(this);
        list_.setRowHeight(20);
        list_.setComponentID("file-organizer.list");
        list_.setTitle("File Organizer");
        info_.setMultiLine(true);
        info_.setReadOnly(true);
        info_.setComponentID("file-organizer.info");
        info_.setTitle("File information");
        addAndMakeVisible(list_);
        addAndMakeVisible(info_);
        setSize(520, 460);
    }

    void setRoots(std::vector<std::filesystem::path> roots) override
    {
        model_.setRoots(std::move(roots));
        refresh();
    }

    void refresh() override
    {
        rows_ = model_.rows();
        list_.updateContent();
        list_.repaint();
    }

    void setFoldersFirst(bool foldersFirst) override
    {
        model_.setFoldersFirst(foldersFirst);
        refresh();
    }

    [[nodiscard]] bool foldersFirst() const override
    {
        return model_.foldersFirst();
    }

    [[nodiscard]] std::vector<talas::app::FileOrganizerRow> rowsForSmoke() const override
    {
        return rows_;
    }

    bool activateRowForSmoke(std::size_t row) override
    {
        if (row >= rows_.size())
            return false;
        activate(static_cast<int>(row));
        return true;
    }

    [[nodiscard]] bool playRowForSmoke(std::size_t row) override
    {
        return playRow(static_cast<int>(row));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress{' ', juce::ModifierKeys::ctrlModifier, 0})
        {
            (void) playRow(list_.getSelectedRow());
            return true;
        }
        return false;
    }

    [[nodiscard]] int selectThenPressForSmoke(std::size_t row, const juce::KeyPress& key) override
    {
        list_.selectRow(static_cast<int>(row));
        (void) list_.keyPressed(key);
        return list_.getSelectedRow();
    }

    [[nodiscard]] juce::String informationForSmoke(std::size_t row) override
    {
        selectedRowsChanged(static_cast<int>(row));
        return info_.getText();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        info_.setBounds(bounds.removeFromBottom(96));
        bounds.removeFromBottom(6);
        list_.setBounds(bounds);
    }

private:
    int getNumRows() override
    {
        return static_cast<int>(rows_.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        const auto& entry = rows_[static_cast<std::size_t>(row)];
        if (selected)
            graphics.fillAll(workbenchTheme.selectedRow);
        // Open folders underlined, closed ones gray; files by their kind
        //. On the dark theme an open folder's black is
        // the theme's text.
        const auto look = talas::app::fileOrganizerFolderLook(entry.open);
        const auto colour = entry.folder ? (look.underlined ? workbenchTheme.primaryText : juce::Colour{look.colour})
                                         : juce::Colour{talas::app::talasFileKindColour(*entry.kind)};
        graphics.setColour(colour);
        graphics.setFont(WorkbenchType::label());
        const auto indent = 8 + static_cast<int>(entry.depth) * 16;
        const auto name = juceString(entry.path.filename().string().empty() ? entry.path.string() : entry.path.filename().string());
        graphics.drawText(name, indent, 0, width - indent, height, juce::Justification::centredLeft);
        if (entry.folder && look.underlined)
            graphics.drawHorizontalLine(height - 3, static_cast<float>(indent),
                static_cast<float>(indent + std::min(width - indent, static_cast<int>(name.length()) * 7)));
    }

    void selectedRowsChanged(int row) override
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        juce::String text;
        for (const auto& line : talas::app::talasFileInformation(rows_[static_cast<std::size_t>(row)].path))
            text << juceString(line) << "\n";
        info_.setText(text.trimEnd(), false);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        activate(row);
    }

    void returnKeyPressed(int row) override
    {
        activate(row);
    }

    // A file's name dragged into a parameter field.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
    {
        if (rows.size() != 1)
            return {};
        const auto row = rows[0];
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size() || rows_[static_cast<std::size_t>(row)].folder)
            return {};
        return juce::String{talas::app::talasFileDragDescription(rows_[static_cast<std::size_t>(row)].path)};
    }

    void activate(int row)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
            return;
        const auto entry = rows_[static_cast<std::size_t>(row)];
        if (entry.folder)
        {
            model_.toggle(entry.path);
            refresh();
            return;
        }
        if (open_)
            open_(entry.path);
    }

    bool playRow(int row)
    {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size() || rows_[static_cast<std::size_t>(row)].folder
            || !play_)
            return false;
        return play_(rows_[static_cast<std::size_t>(row)].path);
    }

    std::function<void(const std::filesystem::path&)> open_;
    std::function<bool(const std::filesystem::path&)> play_;
    talas::app::FileOrganizerModel model_;
    std::vector<talas::app::FileOrganizerRow> rows_;
    juce::ListBox list_;
    juce::TextEditor info_;
};
}

std::unique_ptr<FileOrganizerPanel> makeFileOrganizerPanel(
    std::function<void(const std::filesystem::path&)> open, std::function<bool(const std::filesystem::path&)> play)
{
    return std::make_unique<FileOrganizerPanelImpl>(std::move(open), std::move(play));
}

std::filesystem::path WorkbenchComponent::preferencesPath() const
{
    return preferencesFile_.empty() ? workbenchPreferencesPath() : preferencesFile_;
}

// A user's preferences file opened while the Workbench runs;
// its preferences are taken, and changes are kept there from then on.
void WorkbenchComponent::usePreferencesFile(const std::filesystem::path& file)
{
    preferencesFile_ = file;
    loadPreferences();
    applyAppearancePreferences();
    setStatus("Using the preferences in " + juceString(talas::project::pathToUtf8(file)));
}

void WorkbenchComponent::loadPreferences()
{
    std::error_code missing;
    const auto path = preferencesFile_.empty() && !std::filesystem::exists(preferencesPath(), missing)
        ? workbenchPreferencesPath("preferences.txt")
        : preferencesPath();
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    // The Talas folder is a frequently used folder
    // until the preferences say otherwise.
    talas::app::WorkbenchPreferences defaults;
    defaults.frequentlyUsedFolders = {
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Talas").getFullPathName().toStdString()};
#if JUCE_MAC
    const auto capsLock = (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift) != 0;
#else
    const auto capsLock = false;
#endif
    preferences_ = talas::app::startupWorkbenchPreferences(text, defaults, capsLock);
}

void WorkbenchComponent::savePreferences()
{
    if (!persistentStateEnabled_)
        return;
    const auto path = preferencesPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << talas::app::writeWorkbenchPreferences(preferences_);
}

// At startup a help window, the untitled project or else the last one
// opened, and the Sound Browser on the right of
// the screen.
void WorkbenchComponent::applyStartupPreferences()
{
    if (!preferences_.showUntitledOnStartup && !recentProjects_.entries().empty())
        loadProjectFromPath(recentProjects_.entries().front());
    if (preferences_.showSoundBrowserOnStartup && isShowing())
    {
        showSoundBrowser();
        const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        if (!soundBrowsers_.empty() && soundBrowsers_.front().window != nullptr && display != nullptr)
        {
            auto& window = *soundBrowsers_.front().window;
            window.setTopLeftPosition(display->userArea.getRight() - window.getWidth(), display->userArea.getY());
        }
    }
    if (preferences_.showHelpOnStartup && isShowing())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Getting started",
            "Choose a Sound in the prototypes, connect it to the output and press Play. Turn this window off under "
            "Edit > Preferences.",
            {}, this);
    }
}

// Edit > Preferences.
void WorkbenchComponent::showPreferencesDialog()
{
    auto* window = new juce::AlertWindow("Preferences", "Saved for you alone, and read each time Talas starts",
        juce::MessageBoxIconType::NoIcon, this);
    const juce::StringArray yesNo{"Yes", "No"};
    const auto choice = [&](juce::String id, juce::String label, bool value) {
        window->addComboBox(id, yesNo, label);
        window->getComboBoxComponent(id)->setSelectedItemIndex(value ? 0 : 1, juce::dontSendNotification);
    };
    choice("vcs", "Open the virtual control surface on play", preferences_.openVirtualControlsOnPlay);
    window->addComboBox("pad", juce::StringArray{"0 dB", "-6 dB"}, "System output level pad");
    window->getComboBoxComponent("pad")->setSelectedItemIndex(preferences_.padSystemOutput ? 1 : 0, juce::dontSendNotification);
    choice("help", "Show help on startup", preferences_.showHelpOnStartup);
    choice("untitled", "Show an untitled project on startup", preferences_.showUntitledOnStartup);
    choice("browser", "Show the Sound Browser on startup", preferences_.showSoundBrowserOnStartup);
    choice("diskDisplay", "Update display during disk operations", preferences_.updateDisplayDuringDiskOperations);
    choice("paths", "Show full path names", preferences_.showFullPathNames);
    choice("digits", "Show all digits in double floating point numbers", preferences_.showAllDigits);
    window->addComboBox("fieldIcons", juce::StringArray{"Small", "Large"}, "Icon size in Sound editor fields");
    window->addComboBox("analyzerLength", juce::StringArray{"64", "128", "256"}, "Spectrum analyzer length");
    window->getComboBoxComponent("analyzerLength")->setSelectedItemIndex(
        preferences_.spectrumAnalyzer.length <= 64 ? 0 : preferences_.spectrumAnalyzer.length <= 128 ? 1 : 2,
        juce::dontSendNotification);
    // LinearEnvelope, Hamming and Long
    // Kaiser after Rectangular, Hann and Blackman.
    const auto& analyzerWindows = talas::app::spectrumAnalyzerWindows();
    juce::StringArray analyzerWindowLabels;
    for (const auto analyzerWindow : analyzerWindows)
        analyzerWindowLabels.add(juce::String{std::string{talas::app::spectrumAnalyzerWindowLabel(analyzerWindow)}});
    window->addComboBox("analyzerWindow", analyzerWindowLabels, "Spectrum analyzer window");
    const auto chosenWindow = std::ranges::find(analyzerWindows, preferences_.spectrumAnalyzer.window);
    window->getComboBoxComponent("analyzerWindow")->setSelectedItemIndex(
        chosenWindow == analyzerWindows.end() ? 1 : static_cast<int>(chosenWindow - analyzerWindows.begin()),
        juce::dontSendNotification);
    window->getComboBoxComponent("fieldIcons")->setSelectedItemIndex(preferences_.largeFieldIcons ? 1 : 0, juce::dontSendNotification);
    choice("eight", "Show eight channels in DSP Status", preferences_.showEightChannels);
    choice("stackTraces", "Allow stack traces", preferences_.allowStackTraces);
    choice("learnPitch", "Escape and one key types !Pitch, not !KeyPitch", preferences_.learnOneKeyAsPitch);
    choice("graySelection", "Selected Sounds in gray, not blue", preferences_.graySelection);
    window->addComboBox("warnings", juce::StringArray{"Help window only", "Notify immediately"}, "Sound compiler warnings");
    window->getComboBoxComponent("warnings")->setSelectedItemIndex(preferences_.notifyCompilerWarnings ? 1 : 0, juce::dontSendNotification);
    for (const auto kind : {talas::app::TalasFileKind::Sample, talas::app::TalasFileKind::Spectrum, talas::app::TalasFileKind::Midi})
    {
        const auto found = preferences_.fileEditors.find(kind);
        // A kind checked with no application asks for it the
        // next time one of its files is edited.
        choice("external" + juceString(talas::app::talasFileKindColourName(kind)),
            juce::String{"Edit "} + (kind == talas::app::TalasFileKind::Sample ? "samples"
                    : kind == talas::app::TalasFileKind::Spectrum            ? "spectra"
                                                                         : "MIDI files")
                + " in an external editor",
            found != preferences_.fileEditors.end());
        window->addTextEditor(juceString(talas::app::talasFileKindColourName(kind)),
            found == preferences_.fileEditors.end() ? juce::String{} : juceString(found->second),
            juce::String{"External editor for "} + (kind == talas::app::TalasFileKind::Sample ? "samples"
                    : kind == talas::app::TalasFileKind::Spectrum                            ? "spectra"
                                                                                         : "MIDI files"));
    }
    // The release number and date at the lower left.
    auto* application = juce::JUCEApplicationBase::getInstance();
    const auto version = application == nullptr ? juce::String{"0"} : application->getApplicationVersion();
    window->addTextBlock(juceString(talas::app::preferencesReleaseLine(version.toStdString(), __DATE__)));
    window->addButton("Save", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Frequently used folders...", 2);
    window->addButton("Speaker placement...", 3);
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, window](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            auto next = safeThis->preferences_;
            const auto yes = [&](const char* id) { return window->getComboBoxComponent(id)->getSelectedItemIndex() == 0; };
            next.openVirtualControlsOnPlay = yes("vcs");
            next.padSystemOutput = window->getComboBoxComponent("pad")->getSelectedItemIndex() == 1;
            next.showHelpOnStartup = yes("help");
            next.showUntitledOnStartup = yes("untitled");
            next.showSoundBrowserOnStartup = yes("browser");
            next.updateDisplayDuringDiskOperations = yes("diskDisplay");
            next.showFullPathNames = yes("paths");
            next.showAllDigits = yes("digits");
            next.largeFieldIcons = window->getComboBoxComponent("fieldIcons")->getSelectedItemIndex() == 1;
            next.spectrumAnalyzer.length = 64U << std::clamp(window->getComboBoxComponent("analyzerLength")->getSelectedItemIndex(), 0, 2);
            const auto& windows = talas::app::spectrumAnalyzerWindows();
            next.spectrumAnalyzer.window = windows[static_cast<std::size_t>(std::clamp(
                window->getComboBoxComponent("analyzerWindow")->getSelectedItemIndex(), 0, static_cast<int>(windows.size()) - 1))];
            next.showEightChannels = yes("eight");
            next.allowStackTraces = yes("stackTraces");
            next.learnOneKeyAsPitch = yes("learnPitch");
            next.graySelection = yes("graySelection");
            next.notifyCompilerWarnings = window->getComboBoxComponent("warnings")->getSelectedItemIndex() == 1;
            for (const auto kind : {talas::app::TalasFileKind::Sample, talas::app::TalasFileKind::Spectrum, talas::app::TalasFileKind::Midi})
            {
                const auto application = window->getTextEditorContents(juceString(talas::app::talasFileKindColourName(kind))).trim();
                const auto checked = yes(("external" + juceString(talas::app::talasFileKindColourName(kind))).toRawUTF8());
                if (!checked && application.isEmpty())
                    next.fileEditors.erase(kind);
                else
                    next.fileEditors[kind] = application.toStdString();
            }
            safeThis->setPreferences(std::move(next));
            if (result == 2)
                safeThis->showFrequentlyUsedFolders();
            else if (result == 3)
                safeThis->showSpeakerPlacement();
        }),
        true);
}

// Speaker Placement: the first menu, Placement
// preset, fills in the speakers of a predefined setup; or each speaker is
// typed, its radius and angle, or sub, one an output channel in order.
void WorkbenchComponent::showSpeakerPlacement()
{
    if (!isShowing())
        return;
    auto* window = new juce::AlertWindow("Speaker Placement",
        "Hard left is 0 degrees, straight ahead 90, hard right 180, negative angles behind. "
        "Each speaker, one an output channel: its radius and angle, or sub; a semicolon between them.",
        juce::MessageBoxIconType::NoIcon, this);
    juce::StringArray names{"None (left and right)"};
    for (const auto& preset : talas::project::speakerPlacementPresets())
        names.add(juceString(preset.name));
    window->addComboBox("preset", names, "Placement preset");
    const auto shown = [](const std::vector<talas::project::SpeakerPosition>& speakers) {
        return juceString(talas::project::speakerPlacementText(speakers)).replace("\n", "; ");
    };
    window->addTextEditor("speakers", shown(preferences_.speakerPlacement), "Speakers");
    auto* menu = window->getComboBoxComponent("preset");
    const auto current = talas::project::speakerPlacementPresetName(preferences_.speakerPlacement);
    for (int index = 1; current.has_value() && index < names.size(); ++index)
    {
        if (names[index] == juceString(*current))
            menu->setSelectedItemIndex(index, juce::dontSendNotification);
    }
    if (preferences_.speakerPlacement.empty())
        menu->setSelectedItemIndex(0, juce::dontSendNotification);
    menu->onChange = [window, menu, shown] {
        const auto index = menu->getSelectedItemIndex();
        const auto& presets = talas::project::speakerPlacementPresets();
        window->getTextEditor("speakers")->setText(
            index >= 1 && static_cast<std::size_t>(index) <= presets.size()
                ? shown(presets[static_cast<std::size_t>(index) - 1].speakers)
                : juce::String{});
    };
    window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
    window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    window->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, window](int result) {
            if (safeThis == nullptr || result != 1)
                return;
            const auto typed = window->getTextEditorContents("speakers").replace(";", "\n").trim();
            auto next = safeThis->preferences_;
            if (typed.isEmpty())
                next.speakerPlacement.clear();
            else if (auto placement = talas::project::parseSpeakerPlacement(typed.toStdString()))
                next.speakerPlacement = std::move(*placement);
            else
            {
                safeThis->setStatus("Speaker Placement: one to eight speakers, each a radius and an angle, or sub");
                return;
            }
            safeThis->setPreferences(std::move(next));
        }),
        true);
}

void WorkbenchComponent::showFrequentlyUsedFolders()
{
    foldersWindow_.reset();
    const auto start = preferences_.frequentlyUsedFolders.empty()
        ? filesystemPath(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName())
        : talas::project::pathFromUtf8(preferences_.frequentlyUsedFolders.front());
    foldersPanel_ = std::make_unique<FrequentlyUsedFoldersPanel>(preferences_, start, [this](talas::app::WorkbenchPreferences chosen) {
        auto next = preferences_;
        next.frequentlyUsedFolders = std::move(chosen.frequentlyUsedFolders);
        setPreferences(std::move(next));
    });
    if (isShowing())
    {
        foldersWindow_ = makeToolWindow("Frequently Used Folders", *foldersPanel_, {});
        foldersWindow_->setVisible(true);
        foldersWindow_->toFront(true);
    }
    setStatus("Open a folder, then Add it to the frequently used folders, or Remove one");
}

void WorkbenchComponent::setPreferences(talas::app::WorkbenchPreferences preferences)
{
    preferences_ = std::move(preferences);
    savePreferences();
    if (fileOrganizerPanel_ != nullptr)
        fileOrganizerPanel_->setRoots(fileOrganizerRoots());
    applyAppearancePreferences();
    setStatus("Preferences saved");
}

// The Appearance preferences: file fields show their files' whole paths or names alone, and
// the surface's numbers twelve digits or six; and the outputs' pad.
void WorkbenchComponent::applyAppearancePreferences()
{
    systemOutputGain_.store(talas::app::systemOutputGain(preferences_), std::memory_order_relaxed);
    WorkbenchType::setGraySelection(preferences_.graySelection);
    repaint();
    virtualControlSurface_->setShowAllDigits(preferences_.showAllDigits);
    inspectorPanel_->setLargeFieldIcons(preferences_.largeFieldIcons);
    analysisPanel_->setInfoSpectrumSettings(preferences_.spectrumAnalyzer);
    inspectorPanel_->setFileNameDisplay([this](const std::string& id) -> std::optional<std::string> {
        const auto& assets = projectBridge_.editor().document().assets;
        const auto asset = std::ranges::find(assets, id, &talas::project::ProjectAsset::id);
        if (asset == assets.end())
            return std::nullopt;
        return talas::app::displayedFileName(
            talas::project::pathToUtf8(talas::audio::resolveProjectAssetPath(*asset, projectPath())), preferences_);
    });
}

std::vector<std::filesystem::path> WorkbenchComponent::fileOrganizerRoots() const
{
    std::vector<std::filesystem::path> roots;
    for (const auto& folder : preferences_.frequentlyUsedFolders)
        roots.push_back(talas::project::pathFromUtf8(folder));
    if (projectPath().has_value())
        roots.push_back(projectPath()->parent_path());
    if (roots.empty())
        roots.push_back(filesystemPath(juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName()));
    // The Expressions Library's text files,
    // written once where Talas keeps its data; one's own changes to
    // them stay.
    const auto library = filesystemPath(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                            .getChildFile("Talas")
                                            .getChildFile("Expressions Library")
                                            .getFullPathName());
    (void) talas::app::writeExpressionsLibrary(library);
    std::error_code error;
    if (std::filesystem::is_directory(library, error))
        roots.push_back(library);
    return roots;
}

// File > Documentation: the documents in a
// Documentation folder of the frequently used folders or of Talas's own
// data, each opened in the computer's viewer.
juce::PopupMenu WorkbenchComponent::documentationMenu()
{
    auto folders = fileOrganizerRoots();
    folders.push_back(filesystemPath(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Talas").getFullPathName()));
    documentationFiles_ = talas::app::documentationInDocumentationFolders(folders);
    juce::PopupMenu menu;
    for (std::size_t index = 0; index < documentationFiles_.size() && index < 256; ++index)
        menu.addItem(documentationMenuItemBase + static_cast<int>(index), jucePathString(documentationFiles_[index].stem()));
    if (documentationFiles_.empty())
        menu.addItem(documentationMenuItemBase + 256, "No PDFs in a Documentation folder", false);
    return menu;
}

bool WorkbenchComponent::selectDocumentation(int menuItemId)
{
    const auto index = menuItemId - documentationMenuItemBase;
    if (index < 0 || index >= 256 || static_cast<std::size_t>(index) >= documentationFiles_.size())
        return false;
    const auto& document = documentationFiles_[static_cast<std::size_t>(index)];
    setStatus(juce::File{jucePathString(document)}.startAsProcess() ? "Opened " + jucePathString(document.filename())
                                                                     : juce::String{"The document could not be opened"});
    return true;
}

// File > File Organizer.
void WorkbenchComponent::showFileOrganizer()
{
    if (fileOrganizerPanel_ == nullptr)
        fileOrganizerPanel_ = makeFileOrganizerPanel([this](const std::filesystem::path& path) { (void) openTalasFile(path); },
            [this](const std::filesystem::path& path) { return playOrganizerFile(path); });
    fileOrganizerPanel_->setRoots(fileOrganizerRoots());
    if (isShowing())
    {
        if (fileOrganizerWindow_ == nullptr)
            fileOrganizerWindow_ = makeToolWindow("File Organizer", *fileOrganizerPanel_);
        fileOrganizerWindow_->setVisible(true);
        fileOrganizerWindow_->toFront(true);
    }
    setStatus("File Organizer: double-click a folder to open it, a file to edit it");
}

// A file opens in the external editor the preferences name for its kind, else
// in Talas's own: a sample in the Sample Editor, a spectrum as a Sound to
// play and edit, a MIDI file on a Timeline track.
talas::app::TalasFileOpening WorkbenchComponent::openTalasFile(const std::filesystem::path& path, bool fromOpenCommand)
{
    const auto kind = talas::app::talasFileKindOf(path);
    if (!kind.has_value())
    {
        setStatus("Talas does not use files of this kind");
        return talas::app::TalasFileOpening::None;
    }
    const auto opening = talas::app::talasFileOpening(*kind, preferences_, fromOpenCommand);
    if (smokeRecordsFileOpenings_)
        return opening;
    switch (opening)
    {
    case talas::app::TalasFileOpening::AskForExternalEditor:
    {
        // The first edit of a kind checked asks for the
        // application, remembered from then on.
        auto chooser = std::make_shared<juce::FileChooser>("Find the application to edit this file with",
            juce::File{"/Applications"}, "*.app");
        juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::canSelectDirectories,
            [safeThis, chooser, path, kind = *kind](const juce::FileChooser& chosen) {
                if (safeThis == nullptr || chosen.getResult() == juce::File{})
                    return;
                auto next = safeThis->preferences_;
                next.fileEditors[kind] = chosen.getResult().getFullPathName().toStdString();
                safeThis->setPreferences(std::move(next));
                (void) safeThis->openTalasFile(path, false);
            });
        break;
    }
    case talas::app::TalasFileOpening::ExternalEditor:
    {
        juce::ChildProcess process;
        if (!process.start(juce::StringArray{"/usr/bin/open", "-a", juceString(preferences_.fileEditors.at(*kind)),
                jucePathString(path)}))
            setStatus("The external editor could not be started");
        break;
    }
    case talas::app::TalasFileOpening::SampleEditor:
        showSampleEditor();
        if (sampleEditorPanel_ != nullptr)
            (void) sampleEditorPanel_->openFile(path);
        break;
    case talas::app::TalasFileOpening::SpectrumEditor:
        (void) openSpectrumEditor(path);
        break;
    case talas::app::TalasFileOpening::MidiTimeline:
        (void) importMidiFileToTimeline(path);
        break;
    case talas::app::TalasFileOpening::None:
        setStatus("Talas has no editor for this kind of file");
        break;
    }
    return opening;
}

// Ctrl+Space in the File Organizer: a sample or a
// Sound file plays as Play... plays it; an analysis plays through the
// Sound that resynthesises it and a MIDI file through a minimal orchestra,
// each a Sound file of its own written beside the Workbench's temporary
// files, the project left as it is.
bool WorkbenchComponent::playOrganizerFile(const std::filesystem::path& path)
{
    if (path.extension() == talas::app::compiledSoundExtension)
        return playCompiledSoundFile(path);
    const auto kind = talas::app::talasFileKindOf(path);
    if (!kind.has_value() || *kind == talas::app::TalasFileKind::Sample)
        return playFileAtPath(path);
    const auto player = talas::app::talasFilePlayer(path);
    if (!player.has_value())
    {
        setStatus("Talas plays no Sound from this file");
        return false;
    }
    const auto folder = filesystemPath(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName())
        / "talas-organizer";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto soundFile = folder / (talas::project::pathToUtf8(path.stem()) + ".talas.json");
    {
        std::ofstream output(soundFile, std::ios::binary | std::ios::trunc);
        output << talas::project::serializeProjectDocument(player->soundFile);
        if (!output)
        {
            setStatus("The file's Sound could not be written to play it");
            return false;
        }
    }
    return playFromSoundBrowser(
        {soundFile, player->definitionId, player->name, 1, talas::app::SoundBrowserKind::Sound, false, false});
}

bool WorkbenchComponent::copySpectrumRegion(
    const talas::project::PartialTrackAsset& region, const std::filesystem::path& destination)
{
    std::string error;
    if (!writeSpectrumFile(region, destination, error))
    {
        setStatus(juceString(error));
        return false;
    }
    return addSoundForTalasFile(destination).has_value();
}

bool WorkbenchComponent::copySpectrumRegionAsSumOfSines(
    const talas::project::PartialTrackAsset& region, const std::filesystem::path& destination)
{
    std::string error;
    if (!writeSpectrumFile(region, destination, error))
    {
        setStatus(juceString(error));
        return false;
    }
    const auto table = talas::audio::spectrumInRamTable(region);
    auto sines = talas::app::spectrumSumOfSinesSound(
        projectBridge_.editor().document(), destination, table.frames, table.partials, table.durationSeconds);
    const auto definitionId = sines.definition.definitionId;
    const auto displayName = sines.definition.displayName;
    const auto hadControl = [&](const std::string& name) {
        return std::ranges::any_of(projectBridge_.editor().document().virtualControls,
            [&](const auto& existing) { return existing.eventName == name; });
    };
    std::vector<bool> existedBefore;
    for (const auto& control : sines.controls)
        existedBefore.push_back(hadControl(control.eventName));
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(sines.definition)); !added.ok())
    {
        setStatus("The region's SumOfSines was refused: " + juceString(added.message));
        return false;
    }
    // Its !Stretch and !FreqScale start at 1, unless the project already had
    // controls of those names; the new Sound has just made them, at 0.
    for (std::size_t index = 0; index < sines.controls.size(); ++index)
    {
        auto& control = sines.controls[index];
        if (existedBefore[index])
            continue;
        if (hadControl(control.eventName))
        {
            (void) projectBridge_.editor().setVirtualControlRange(control.eventName, control.minimum, control.maximum);
            (void) projectBridge_.editor().setVirtualControlValue(control.eventName, control.value);
        }
        else
        {
            (void) projectBridge_.editor().addVirtualControl(std::move(control));
        }
    }
    documentChanged();
    for (const auto& resource : sines.resources)
    {
        if (!bindSpectrumFileAsTable(definitionId, resource.nodeId, resource.parameterId, resource.file))
            bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    }
    setStatus("Copied the region as " + juceString(displayName) + ", !Stretch and !FreqScale stretching and scaling it");
    return true;
}

// Open... or a double-click on a spectrum file: the spectrum editor on it,
// the file joining
// the project's Sounds as a Sound that plays it if it is not there yet.
bool WorkbenchComponent::openSpectrumEditor(const std::filesystem::path& path)
{
    const auto opened = [&]() -> std::optional<std::string> {
        std::error_code error;
        for (const auto& asset : projectBridge_.editor().document().assets)
        {
            if (asset.kind == talas::project::ProjectAssetKind::PartialSpectrum
                && std::filesystem::equivalent(talas::audio::resolveProjectAssetPath(asset, projectPath()), path, error))
                return asset.id;
        }
        return std::nullopt;
    };
    auto assetId = opened();
    // A file the project does not have yet becomes a Sound of its own; the
    // asset the new Sound reads is the one its copy in the project is.
    if (const auto added = assetId.has_value() ? std::nullopt : addSoundForTalasFile(path); added.has_value())
    {
        const auto& document = projectBridge_.editor().document();
        const auto definition = std::ranges::find(document.soundDefinitions, *added, &talas::project::ProjectSoundDefinition::definitionId);
        for (const auto& node : definition == document.soundDefinitions.end() ? std::vector<talas::project::ProjectNode>{} : definition->graph.nodes)
        {
            for (const auto& parameter : node.parameters)
            {
                const auto asset = parameter.resource.has_value()
                    ? std::ranges::find(document.assets, *parameter.resource, &talas::project::ProjectAsset::id)
                    : document.assets.end();
                if (asset != document.assets.end()
                    && asset->kind == talas::project::ProjectAssetKind::PartialSpectrum)
                    assetId = asset->id;
            }
        }
    }
    if (!assetId.has_value())
    {
        setStatus("The spectrum file could not be opened in the spectrum editor");
        return false;
    }
    spectrumEditorAssetId_ = assetId;
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
    (void) compileAndLoadCurrentProject();
    setStatus("The spectrum editor shows " + jucePathString(path.filename()));
    return true;
}

// A file dragged in becomes a Sound in the project's Sound list that plays it
//.
std::optional<std::string> WorkbenchComponent::addSoundForTalasFile(const std::filesystem::path& path)
{
    auto sound = talas::app::soundForTalasFile(projectBridge_.editor().document(), path);
    if (!sound.has_value())
    {
        setStatus("Talas makes no Sound from this file");
        return std::nullopt;
    }
    const auto definitionId = sound->definition.definitionId;
    const auto displayName = sound->definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(sound->definition)); !added.ok())
    {
        setStatus("The file's Sound was refused: " + juceString(added.message));
        return std::nullopt;
    }
    documentChanged();
    for (const auto& resource : sound->resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    setStatus("Added the Sound " + juceString(displayName) + " that plays " + jucePathString(path.filename()));
    return definitionId;
}

bool WorkbenchComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return std::ranges::any_of(files, [](const juce::String& file) {
        return talas::app::talasFileKindOf(filesystemPath(file)).has_value();
    });
}

void WorkbenchComponent::filesDropped(const juce::StringArray& files, int, int)
{
    for (const auto& file : files)
    {
        if (talas::app::talasFileKindOf(filesystemPath(file)).has_value())
            (void) addSoundForTalasFile(filesystemPath(file));
    }
}
}
