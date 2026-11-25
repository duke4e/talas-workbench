#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/project/ProjectPackage.h"
#include "talas/project/ToolFile.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
// A new Tool in the Tool editor, a global
// variable added and its Variable Settings given a widget, highlighted in
// the Layout over its LayoutGrid, a state's triggered response, Run
// opening the Tool window at its fixed size, its trigger button counting
// presses, Debug showing every value, and the Tool saved, opened to edit
// and opened to use.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runToolEditorSmoke()
{
    showToolEditor(talas::project::tool::newTool(), std::nullopt);
    auto* editor = toolEditorForSmoke();
    if (editor == nullptr)
        return "the Tool editor did not open";
    // Add, a name, then the Variable Settings.
    if (!editor->addVariableForSmoke(true, "triggerButton") || !editor->setSettingForSmoke("widget", "Button (trigger)")
        || !editor->setSettingForSmoke("left", "20") || !editor->setSettingForSmoke("top", "40")
        || !editor->setSettingForSmoke("labelled", "true") || !editor->setSettingForSmoke("step", "true")
        || !editor->applySettingsForSmoke())
        return "a global variable was not added and given a button";
    const auto added = editor->tool().variables;
    if (added.size() != 1 || added.front().name != "TriggerButton" || added.front().widget.kind != talas::project::tool::WidgetKind::TriggerButton)
        return "a global variable's name did not begin with an uppercase letter, or kept no widget";
    // The selected variable's widget is highlighted in the Layout.
    if (editor->highlightedForSmoke() != std::optional<juce::Rectangle<int>>{juce::Rectangle<int>{20, 40, 120, 24}})
        return "the selected variable's widget was not highlighted in the Layout";
    if (!editor->addVariableForSmoke(true, "Presses") || !editor->setSettingForSmoke("initially", "0")
        || !editor->setSettingForSmoke("widget", "Read only text") || !editor->setSettingForSmoke("top", "80")
        || !editor->applySettingsForSmoke() || !editor->addVariableForSmoke(true, "Debug")
        || !editor->setSettingForSmoke("initially", "true") || !editor->applySettingsForSmoke()
        || !editor->addVariableForSmoke(true, "WindowTitle") || !editor->setSettingForSmoke("initially", "'Counter'")
        || !editor->applySettingsForSmoke())
        return "the other global variables were not added";
    // LayoutGrid draws a grid in the Layout; equal window extents
    // make a window that cannot be resized.
    if (!editor->addVariableForSmoke(true, "LayoutGrid") || !editor->setSettingForSmoke("initially", "10")
        || !editor->applySettingsForSmoke() || !editor->addVariableForSmoke(true, "MinimumWindowExtent")
        || !editor->setSettingForSmoke("initially", "240 @ 160") || !editor->applySettingsForSmoke()
        || !editor->addVariableForSmoke(true, "MaximumWindowExtent") || !editor->setSettingForSmoke("initially", "240 @ 160")
        || !editor->applySettingsForSmoke())
        return "the special global variables were not added";
    if (editor->gridForSmoke() != std::optional<std::pair<double, double>>{std::pair{10.0, 10.0}})
        return "LayoutGrid did not draw a grid in the Layout";
    // A state's triggered response, its trigger and response.
    if (!editor->selectStateForSmoke("InitialState") || !editor->addResponseForSmoke())
        return "a triggered response was not added to InitialState";
    editor->setTriggerForSmoke("TriggerButton turnedOn");
    editor->setResponseForSmoke("Presses := Presses + 1");
    if (editor->tool().states.front().responses.front().condition != "TriggerButton turnedOn")
        return "the trigger was not kept";

    // Run opens the Tool window and starts it.
    editor->runForSmoke();
    auto* running = toolRunForSmoke();
    if (running == nullptr || running->title() != "Counter")
        return "Run did not open the Tool window with its WindowTitle";
    if (!running->fixedSize() || running->getWidth() != 240 || running->getHeight() != 160)
        return "equal window extents did not open a window of that size that cannot be resized";
    const auto widgets = running->widgetsForSmoke();
    if (widgets.size() != 2 || widgets.front().second != talas::project::tool::WidgetKind::TriggerButton)
        return "the Tool window did not show the visible variables' widgets";
    if (!running->pressForSmoke("TriggerButton", talas::project::tool::ButtonEvent::Press)
        || !running->pressForSmoke("TriggerButton", talas::project::tool::ButtonEvent::Release))
        return "the trigger button could not be pressed";
    running->advanceForSmoke(50.0);
    if (running->shownForSmoke("Presses") != "1")
        return "a press of the trigger did not run the triggered response: " + running->shownForSmoke("Presses").toStdString();
    const auto debug = running->debugLinesForSmoke();
    if (std::ranges::find(debug, std::string{"Presses: 1"}) == debug.end())
        return "Debug did not show the variables' values";

    // The Tool saved and read back.
    const auto file = std::filesystem::temp_directory_path() / "talas-tool-smoke.talastool";
    {
        std::ofstream output(file, std::ios::binary);
        output << talas::project::tool::writeToolFile(editor->tool());
    }
    std::ifstream input(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    const auto read = talas::project::tool::readToolFile(text);
    if (!read.ok() || read.tool->variables.size() != 7 || read.tool->states.front().responses.size() != 1)
    {
        std::filesystem::remove(file);
        return "the Tool was not saved and read back";
    }
    // Open with Edit tool opens the editor on it, Use tool runs it.
    running->advanceForSmoke(50.0);
    const auto edited = openToolAt(file, false);
    const auto editedVariables = editor->tool().variables.size();
    const auto used = openToolAt(file, true);
    std::filesystem::remove(file);
    if (!edited || editedVariables != 7)
        return "Open with Edit tool did not open the Tool editor on the saved Tool";
    if (!used || toolRunForSmoke()->shownForSmoke("Presses") != "0" || toolRunForSmoke()->title() != "Counter")
        return "Use tool did not run the saved Tool afresh";
    // A Tool in the Tools folder is on the Tools menu.
    const auto folder = std::filesystem::temp_directory_path() / "talas-tool-smoke-folder";
    std::filesystem::create_directories(folder / "Tools");
    {
        std::ofstream output(folder / "Tools" / "Counter.talastool", std::ios::binary);
        output << talas::project::tool::writeToolFile(editor->tool());
    }
    const auto previousFolders = preferences_.frequentlyUsedFolders;
    preferences_.frequentlyUsedFolders = {folder.string()};
    (void)toolRunForSmoke()->pressForSmoke("TriggerButton", talas::project::tool::ButtonEvent::Press);
    (void)toolRunForSmoke()->pressForSmoke("TriggerButton", talas::project::tool::ButtonEvent::Release);
    toolRunForSmoke()->advanceForSmoke(50.0);
    const auto toolsMenu = getMenuForIndex(getMenuBarNames().indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || (item.getItem().itemID == savedToolMenuItemBase && item.getItem().text == "Counter");
    menuItemSelected(savedToolMenuItemBase, 0);
    const auto ranFromMenu = toolRunForSmoke()->shownForSmoke("Presses") == "0";
    preferences_.frequentlyUsedFolders = previousFolders;
    std::filesystem::remove_all(folder);
    if (!listed || !ranFromMenu)
        return "a Tool in the Tools folder was not on the Tools menu, or did not run from it";
    // File > Documentation lists the PDFs of a
    // Documentation folder.
    const auto documents = std::filesystem::temp_directory_path() / "talas-documentation-smoke";
    std::filesystem::create_directories(documents / "Documentation");
    std::ofstream{documents / "Documentation" / "Talas Guide.pdf"} << "%PDF-1.4";
    preferences_.frequentlyUsedFolders = {documents.string()};
    const auto fileMenu = getMenuForIndex(getMenuBarNames().indexOf("File"), "File");
    auto documented = false;
    for (juce::PopupMenu::MenuItemIterator item{fileMenu, true}; item.next();)
        documented = documented || (item.getItem().itemID == documentationMenuItemBase && item.getItem().text == "Talas Guide");
    preferences_.frequentlyUsedFolders = previousFolders;
    std::filesystem::remove_all(documents);
    if (!documented)
        return "File > Documentation did not list the Documentation folder's PDF";
    // File > New... with Tool as the type opens the Tool
    // editor on a new Tool.
    smokeInputChoice_ = 2;
    const auto newed = commandManager_.invokeDirectly(commandNewFile, false);
    smokeInputChoice_.reset();
    if (!newed || !toolEditorForSmoke()->tool().variables.empty())
        return "File > New... with Tool as its type did not open the Tool editor on a new Tool";
    // A text file new, or opened, in the text file
    // editor, and saved.
    smokeInputChoice_ = 5;
    const auto newText = commandManager_.invokeDirectly(commandNewFile, false);
    smokeInputChoice_.reset();
    if (!newText || textFilePanel_ == nullptr || textFilePanel_->file().has_value() || textFilePanel_->textForSmoke().isNotEmpty())
        return "File > New... with Text file as its type did not open the text file editor on a new file";
    const auto textFile = std::filesystem::temp_directory_path() / "talas-text-file-smoke.txt";
    {
        std::ofstream output(textFile, std::ios::binary);
        output << "EXPR\n\"Half\"\n!Fader * 0.5\n";
    }
    showTextFile(textFile);
    const auto opened = textFilePanel_->textForSmoke() == "EXPR\n\"Half\"\n!Fader * 0.5\n";
    textFilePanel_->setTextForSmoke("3 + 4");
    const auto saved = textFilePanel_->saveToForSmoke(textFile);
    std::ifstream reread(textFile, std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>{reread}, std::istreambuf_iterator<char>{}};
    reread.close();
    std::filesystem::remove(textFile);
    if (!opened || !saved || written != "3 + 4")
        return "the text file editor did not open a text file and save it again";
    return std::nullopt;
}

// Tools > File Archivist archives a Sound file
// found on the disk, not the project open, into one folder with a copy of
// each file it uses.
std::optional<std::string> WorkbenchComponent::runFileArchivistSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto toolsMenu = getMenuForIndex(getMenuBarNames().indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandFileArchivist;
    if (!listed)
        return "the Tools menu had no File Archivist";
    const auto folder = smokeDirectory / "archivist";
    std::filesystem::create_directories(folder / "tables");
    {
        std::ofstream table(folder / "tables" / "hit.txt", std::ios::binary);
        table << "1 0.5 -1";
    }
    auto document = projectBridge_.editor().document();
    document.metadata.title = "crispy mix";
    document.assets.push_back({.id = "Hit", .kind = talas::project::ProjectAssetKind::Table, .path = "tables/hit.txt"});
    const auto file = folder / "crispy mix.talas.json";
    if (!talas::project::saveProjectFile(file, document).ok())
        return "the File Archivist smoke could not save its Sound file";
    if (archiveProjectFile(folder / "missing.talas.json") || archivedProject_.has_value())
        return "the File Archivist archived a Sound file that is not on the disk";
    if (!archiveProjectFile(file))
        return "the File Archivist did not read the Sound file chosen on the disk";
    // The package startProjectPackage makes of the archived file, made here
    // at once; the source is taken once, the project open being next.
    auto source = takePackageSource();
    const auto next = takePackageSource();
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = folder / "crispy mix archive folder.talaspkg";
    options.sourceProjectFilePath = source.path;
    const auto packaged = talas::audio::saveProjectPackage(source.document, options);
    const auto resolved = talas::project::resolveProjectPackagePath(options.outputPackagePath);
    const auto loaded = talas::project::loadProjectFile(resolved.projectFilePath);
    const auto selfSufficient = packaged.ok() && packaged.copiedFileCount == 1 && resolved.ok() && loaded.ok()
        && loaded.document->metadata.title == "crispy mix"
        && talas::project::validateProjectPackageAssets(*loaded.document, resolved).ok();
    std::filesystem::remove_all(folder);
    if (!source.archived || source.path != std::optional{file} || next.archived || next.path != projectPath())
        return "the File Archivist's Sound file was not the next package's source, once";
    if (!selfSufficient)
        return "the File Archivist's archive did not hold the Sound file and a copy of the file it uses: " + packaged.message;
    return std::nullopt;
}

// Oscilloscope and Spectrum Analyzer stage the
// selected Sound with a display after it, which the Analysis workspace shows
// whatever is selected until the project is staged again.
std::optional<std::string> WorkbenchComponent::runInfoDisplaySmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        setPlaying(false);
        currentDocumentStaged_ = false;
        infoDisplayStage_.reset();
        analysisPanel_->clearInfoDisplay();
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "the info display smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto infoMenu = getMenuForIndex(getMenuBarNames().indexOf("Info"), "Info");
    auto listed = 0;
    for (juce::PopupMenu::MenuItemIterator item{infoMenu}; item.next();)
        listed += item.getItem().itemID == commandInfoOscilloscope || item.getItem().itemID == commandInfoSpectrumAnalyzer ? 1 : 0;
    if (listed != 2)
        return finish("the Info menu lacks Oscilloscope and Spectrum Analyzer");
    talas::project::ProjectDocument fixture;
    fixture.graph.nodes = {{"noise", "talas.noise", {}}, {"gain", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
    fixture.graph.connections = {{"noise", "audioOut", "gain", "audioIn"}, {"gain", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("the info display fixture was rejected: " + replaced.message);
    documentChanged();
    selectNode(std::string{"noise"});
    const auto shown = [&](talas::app::InfoDisplay display) {
        if (!prepareInfoDisplay(display) || !stageProject(talas::runtime::RenderSettings{8000.0, 128}, "Info display smoke")
            || !adoptPreparedOutputTakeStageForSmoke())
            return std::optional<std::string>{};
        const auto capture = projectBridge_.latestAnalysisCapture();
        const auto displayed = analysisPanel_->infoDisplayForSmoke();
        return displayed.has_value() && capture != nullptr && capture->preferredProbeIndex(*displayed).has_value()
            ? displayed
            : std::nullopt;
    };
    if (shown(talas::app::InfoDisplay::Oscilloscope) != std::optional<std::string>{"infoDisplay"})
        return finish("Oscilloscope did not stage the selected Sound with a display the Analysis workspace shows: "
            + statusLabel_.getText().toStdString());
    selectNode(std::string{"gain"});
    if (analysisPanel_->infoDisplayForSmoke() != std::optional<std::string>{"infoDisplay"})
        return finish("selecting another Sound took the oscilloscope away");
    if (!stageProject(talas::runtime::RenderSettings{8000.0, 128}, "Info display smoke") || !adoptPreparedOutputTakeStageForSmoke()
        || analysisPanel_->infoDisplayForSmoke())
        return finish("staging the project again kept the oscilloscope");
    if (!shown(talas::app::InfoDisplay::SpectrumAnalyzer).has_value())
        return finish("Spectrum Analyzer did not stage the selected Sound with a display the Analysis workspace shows: "
            + statusLabel_.getText().toStdString() + " · shown "
            + analysisPanel_->infoDisplayForSmoke().value_or("nothing"));
    // Of a stereo Sound, a mix of its left and right outputs.
    auto stereo = fixture;
    stereo.graph.nodes = {{"noise", "talas.noise", {}}, {"pan", "talas.pan", {}}, {"out", "talas.audioOutput", {}}};
    stereo.graph.connections = {{"noise", "audioOut", "pan", "audioIn"}, {"pan", "leftOut", "out", "audioIn"},
        {"pan", "rightOut", "out", "rightIn"}};
    talas::project::reconcileProjectPresentation(stereo);
    if (const auto replaced = projectBridge_.editor().replaceDocument(stereo); !replaced.ok())
        return finish("the stereo info display fixture was rejected: " + replaced.message);
    documentChanged();
    selectNode(std::string{"pan"});
    if (shown(talas::app::InfoDisplay::Oscilloscope) != std::optional<std::string>{"infoDisplay"})
        return finish("Oscilloscope did not stage a stereo Sound with a display of its mix: " + statusLabel_.getText().toStdString());
    // Ctrl+Space in the signal flow plays the selected Sound alone,
    // elsewhere every Sound.
    selectNode(std::string{"noise"});
    signalFlowFocusForSmoke_ = true;
    playTimelineMarked(false);
    signalFlowFocusForSmoke_ = false;
    const auto alone = statusLabel_.getText() == "Playing the selected Sound alone";
    playTimelineMarked(false);
    if (!alone || statusLabel_.getText() != "Playing every Sound of the Timeline")
        return finish("Ctrl+Space in the signal flow did not play the selected Sound alone");
    // A GenericSource whose Source is Ask asks for the
    // source as it plays; the answer stages a copy, the project unchanged.
    auto asking = fixture;
    talas::project::ProjectParameterExpression ask;
    ask.parameterId = "source";
    ask.text = "Ask";
    asking.graph.nodes = {{"source", "talas.genericSource", {ask}}, {"out", "talas.audioOutput", {}}};
    asking.graph.connections = {{"source", "leftOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(asking);
    if (const auto replaced = projectBridge_.editor().replaceDocument(asking); !replaced.ok())
        return finish("the asking GenericSource fixture was rejected: " + replaced.message);
    documentChanged();
    infoDisplayStage_.reset();
    smokeGenericSourceAnswer_ = talas::app::GenericSourceAnswer::Live;
    const auto waited = askForVariableValues();
    smokeGenericSourceAnswer_.reset();
    const auto staged = std::exchange(infoDisplayStage_, std::nullopt);
    const auto sourceOf = [](const talas::project::ProjectDocument& played) {
        const auto& node = played.graph.nodes.front();
        return node.parameters.empty() ? std::string{} : node.parameters.front().text.value_or("");
    };
    if (waited || !staged.has_value() || sourceOf(staged->document) != "Live"
        || sourceOf(projectBridge_.editor().document()) != "Ask")
        return finish("the GenericSource's Ask did not stage the source chosen for this play");
    return finish();
}
}
