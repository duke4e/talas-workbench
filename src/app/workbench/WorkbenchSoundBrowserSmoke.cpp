#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectJson.h"

#include <cmath>
#include <fstream>

namespace talas::app::workbench
{
// The Sound Browser's list, coloured boxes,
// information, Find, replacement input, playing, Space and Ctrl+K, a
// sub-browser, and a Sound with its input dragged into the project.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSoundBrowserSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto root = smokeDirectory / "sound-browser";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / "Library");
    std::filesystem::create_directories(root / "Samples");
    talas::project::ProjectDocument library;
    talas::project::ProjectSoundDefinition reverb;
    reverb.definitionId = "reverb";
    reverb.displayName = "Effects-EUverb";
    reverb.description = "A reverb for any input";
    reverb.inputs.push_back({"in", talas::core::PortType::Audio, "gain", "audioIn"});
    reverb.outputs.push_back({"audioOut", talas::core::PortType::Audio, "gain", "leftOut"});
    reverb.graph.nodes.push_back({"gain", "talas.gain", {}});
    library.soundDefinitions.push_back(std::move(reverb));
    {
        std::ofstream output(root / "Library" / "Effects.talas.json", std::ios::binary);
        output << talas::project::serializeProjectDocument(library);
    }
    std::vector<std::vector<double>> tone(1, std::vector<double>(8000, 0.0));
    for (std::size_t frame = 0; frame < tone.front().size(); ++frame)
        tone.front()[frame] = 0.25 * std::sin(static_cast<double>(frame) * 0.1);
    std::string wavError;
    if (!talas::audio::writePcm16Wav(root / "Samples" / "bucket2.wav", 8000.0, tone, wavError))
        return "Sound Browser smoke could not write its sample: " + wavError;

    const auto previousPreferences = preferences_;
    const auto previousDocument = projectBridge_.editor().document();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSoundBrowser();
        soundBrowsers_.clear();
        soundBrowserInput_ = {};
        soundBrowserLastPlayed_.reset();
        soundBrowserUsesPreparedProfileForSmoke_ = false;
        preferences_ = previousPreferences;
        if (!projectBridge_.editor().replaceDocument(previousDocument).ok() && error.empty())
            error = "Sound Browser smoke could not restore the project";
        documentChanged();
        std::filesystem::remove_all(root, ignored);
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    preferences_.frequentlyUsedFolders = {talas::project::pathToUtf8(root)};
    soundBrowserUsesPreparedProfileForSmoke_ = true;
    showSoundBrowser();
    auto* browser = soundBrowserForSmoke(0);
    if (browser == nullptr)
        return finish("File > Sound Browser did not open a browser");
    using Kind = talas::app::SoundBrowserKind;
    if (!browser->findForSmoke("euverb") || !browser->selectedRowForSmoke().has_value()
        || browser->selectedRowForSmoke()->kind != Kind::Sound
        || !browser->informationForSmoke().contains("Sound · Effects-EUverb"))
        return finish("Find did not select the Sound and show its information");
    const auto count = [browser](Kind kind) {
        const auto rows = browser->rowsForSmoke();
        return std::ranges::count(rows, kind, &talas::app::SoundBrowserRow::kind);
    };
    browser->clickKindForSmoke(Kind::Sample, false);
    if (count(Kind::Sound) != 0)
        return finish("clicking the sample colour did not hide the Sounds");
    browser->clickKindForSmoke(std::nullopt, false);
    // The project's folder may hold this one too, listing it twice.
    if (count(Kind::Sound) < 1)
        return finish("the grey box did not show every kind again");

    if (browser->inputTextForSmoke() != "Input 1")
        return finish("the replacement input did not start as Input 1");
    browser->pressAudioInputForSmoke();
    if (browser->inputTextForSmoke() != "Input 2")
        return finish("the audio input button did not step to Input 2");
    if (!browser->findForSmoke("bucket"))
        return finish("Find did not find the sample");
    browser->pressSubstituteForSmoke();
    if (browser->inputTextForSmoke() != "bucket2.wav" || !soundBrowserInput_.connected)
        return finish("the substitute button did not make the sample the replacement input");
    browser->pressConnectionForSmoke();
    if (soundBrowserInput_.connected)
        return finish("the connection button did not disengage the replacement input");
    browser->pressConnectionForSmoke();

    // The sample plays, looping since it is short; Ctrl+K stops it.
    if (!browser->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey, juce::ModifierKeys::commandModifier, 0})
        || sampleAuditionOwned_ == nullptr || !sampleAuditionOwned_->loop)
        return finish("Ctrl+Space did not play the short sample looping");
    if (!browser->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey}) || !sampleAuditionPaused_.load())
        return finish("Space did not pause the sample");
    if (!browser->keyForSmoke(juce::KeyPress{'k', juce::ModifierKeys::commandModifier, 'k'}) || sampleAuditionOwned_ != nullptr)
        return finish("Ctrl+K did not stop the sample");
    // Play clicked with Control held plays it once.
    browser->pressPlayForSmoke(true);
    if (sampleAuditionOwned_ == nullptr || sampleAuditionOwned_->loop)
        return finish("Play with Control held did not play the short sample once");
    browser->pressPlayForSmoke(false);
    if (sampleAuditionOwned_ == nullptr || !sampleAuditionOwned_->loop)
        return finish("Play did not play the short sample looping");
    stopSoundBrowser();
    // A double-click on a sample opens it in
    // the Sample Editor.
    const auto found = browser->rowsForSmoke();
    const auto bucket = std::ranges::find(found, std::string{"bucket2.wav"}, &talas::app::SoundBrowserRow::name);
    if (bucket == found.end())
        return finish("the browser did not list the sample");
    browser->activateRowForSmoke(static_cast<std::size_t>(bucket - found.begin()));
    const auto opened = sampleEditorPanel_ != nullptr && sampleEditorPanel_->model().frames() > 0;
    if (sampleEditorWindow_ != nullptr)
        sampleEditorWindow_->setVisible(false);
    if (!opened)
        return finish("a double-click on a sample did not open it in the Sample Editor");

    // The Sound plays through the sample; Space pauses it, Ctrl+R plays it
    // again.
    if (!browser->findForSmoke("euverb"))
        return finish("Find did not find the Sound again");
    if (!browser->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey, juce::ModifierKeys::commandModifier, 0})
        || soundBrowserAudition_ == nullptr)
        return finish("Ctrl+Space did not play the Sound through the replacement input");
    if (!browser->keyForSmoke(juce::KeyPress{juce::KeyPress::spaceKey}) || soundBrowserAudition_->playing)
        return finish("Space did not pause the Sound");
    if (!browser->keyForSmoke(juce::KeyPress{'k', juce::ModifierKeys::commandModifier, 'k'}) || soundBrowserAudition_ != nullptr)
        return finish("Ctrl+K did not stop the Sound");
    if (!browser->keyForSmoke(juce::KeyPress{'r', juce::ModifierKeys::commandModifier, 'r'}) || soundBrowserAudition_ == nullptr)
        return finish("Ctrl+R did not play the last Sound again");
    stopSoundBrowser();

    // Dragged into the project with its input, the Sound becomes a new one.
    const auto rows = browser->rowsForSmoke();
    const auto sound = std::ranges::find(rows, Kind::Sound, &talas::app::SoundBrowserRow::kind);
    // The row dragged out carries the Sound.
    if (sound == rows.end()
        || browser->dragDescriptionForSmoke(static_cast<std::size_t>(sound - rows.begin())).toStdString()
            != talas::app::soundBrowserDragDescription(*sound))
        return finish("the browser did not drag its Sound out");
    browser->activateRowForSmoke(static_cast<std::size_t>(sound - rows.begin()));
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    const auto brought = std::ranges::find(definitions, std::string{"Effects-EUverb (bucket2.wav)"},
        &talas::project::ProjectSoundDefinition::displayName);
    if (brought == definitions.end())
        return finish("the Sound with its replacement input did not come into the project");
    // And opens in an editor.
    const auto edited = activeGraphContext_ == std::optional<std::string>{brought->definitionId};
    showRootGraph();
    if (!edited)
        return finish("a double-click on a Sound did not open an editor on it");

    // File > Play... plays a sample or a Sound
    // file's Sound without an editor; the File and Edit menus offer it with
    // Open any..., Choose window... and Large window....
    if (!playFileAtPath(root / "Samples" / "bucket2.wav") || sampleAuditionOwned_ == nullptr)
        return finish("Play... did not play the sample");
    if (!playFileAtPath(root / "Library" / "Effects.talas.json") || soundBrowserAudition_ == nullptr)
        return finish("Play... did not play the Sound file's Sound");
    stopSoundBrowser();
    const auto names = getMenuBarNames();
    const auto listed = [&](const juce::String& menuName, juce::CommandID id) {
        const auto menu = getMenuForIndex(names.indexOf(menuName), menuName);
        for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
        {
            if (item.getItem().itemID == id)
                return true;
        }
        return false;
    };
    if (!listed("File", commandOpenAny) || !listed("File", commandPlayFile) || !listed("File", commandChooseWindow)
        || !listed("Edit", commandLargeWindow))
        return finish("the File and Edit menus did not offer Open any, Play, Choose window and Large window");

    const auto folder = std::ranges::find_if(rows, [](const talas::app::SoundBrowserRow& row) {
        return row.kind == Kind::Folder && row.name == "Samples";
    });
    if (folder == rows.end())
        return finish("the browser did not list the Samples folder");
    browser->activateRowForSmoke(static_cast<std::size_t>(folder - rows.begin()));
    if (soundBrowsers_.size() != 2 || soundBrowsers_.back().folder != folder->path)
        return finish("double-clicking a folder did not spawn a browser of it");
    // The replacement input is global, the spawned browser's
    // the original's.
    if (soundBrowsers_.back().panel->inputTextForSmoke() != browser->inputTextForSmoke())
        return finish("the spawned browser did not share the replacement input");
    return finish();
}
}
