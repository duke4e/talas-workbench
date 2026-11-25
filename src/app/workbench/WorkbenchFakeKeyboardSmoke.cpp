#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// The Fake Keyboard in the Tools menu sends
// MIDI note events to the playing Sound.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runFakeKeyboardSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        if (fakeKeyboardPanel_ != nullptr)
            fakeKeyboardPanel_->releaseAllKeys();
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{1000.0, 64};
            if (!stageProject(std::move(restoreSettings),
                    "Fake Keyboard smoke restore")
                && error.empty())
                error = "Fake Keyboard smoke could not restore the prior stage";
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "Fake Keyboard smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };

    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowFakeKeyboard;
    if (!listed)
        return finish("the Tools menu does not offer the Fake Keyboard");

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"osc", "talas.oscillator", {
            expression("frequency", "!KeyPitch"),
            expression("amplitude", "!KeyDown"),
        }},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(document);
    (void)talas::project::synchronizeProjectEventValueControls(document, registry_);
    if (const auto replaced = projectBridge_.editor().replaceDocument(document);
        !replaced.ok())
    {
        return finish("Fake Keyboard project was rejected: " + replaced.message);
    }
    documentChanged();
    if (!commandManager_.invokeDirectly(commandShowFakeKeyboard, false)
        || fakeKeyboardPanel_ == nullptr || fakeKeyboardWindow_ != nullptr)
        return finish("the Fake Keyboard command did not make the keys (without a window off screen)");
    if (!stageProject(talas::runtime::RenderSettings{1000.0, 64}, "Fake Keyboard smoke"))
        return finish("Fake Keyboard Sound could not stage");
    projectBridge_.setPlaying(true);
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    auto callbackTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto render = [&] {
        callbackTime = std::max(
            callbackTime + 0.064,
            juce::Time::getMillisecondCounterHiRes() * 0.001 + 0.01);
        return projectBridge_.render({}, callbackTime,
            channels.data(), channels.size(), output.size());
    };
    const auto silent = [&] {
        return std::ranges::all_of(output, [](float sample) {
            return std::abs(sample) < 1.0e-6F;
        });
    };
    const auto audible = [&] {
        return std::ranges::any_of(output, [](float sample) {
            return std::abs(sample) > 0.1F;
        });
    };
    if (!render() || !silent())
        return finish("the Fake Keyboard Sound was not silent before a key");
    fakeKeyboardPanel_->pressKeyForSmoke(57);
    if (fakeKeyboardPanel_->model().heldNotes() != std::vector<int>{57}
        || !render() || !render() || !audible())
        return finish("a Fake Keyboard key did not play the live Sound");
    fakeKeyboardPanel_->releaseKeyForSmoke(57);
    if (!render() || !render() || !silent())
        return finish("releasing a Fake Keyboard key did not end the note");
    fakeKeyboardPanel_->pressKeyForSmoke(60);
    fakeKeyboardPanel_->setChannelForSmoke(2);
    if (!fakeKeyboardPanel_->model().heldNotes().empty()
        || fakeKeyboardPanel_->model().channel() != 2
        || !render() || !render() || !silent())
        return finish("changing the Fake Keyboard's channel left a note hanging");
    (void)projectBridge_.collectRetired();
    return finish();
}
}
