#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/CdpProcessToSample.h"
#include "talas/audio/WavReader.h"

namespace talas::app::workbench
{
// ADR-0018 amendment (2026-10-19): the Tools menu's CDP Process to Sample
// renders a sample through CDP8's stretch time into a longer sample and
// leaves an example Sound that plays it.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runCdpProcessToolSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto sample = (smokeDirectory / "cdp-tool-voice.wav").lexically_normal();
    const auto stretched = (smokeDirectory / "cdp-tool-voice stretch time.wav").lexically_normal();
    const auto removeFiles = [&] {
        std::error_code ignored;
        for (const auto& file : {sample, stretched})
            std::filesystem::remove(file, ignored);
    };
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        removeFiles();
        if (!restored.ok() && error.empty())
            error = "CDP process tool smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowCdpProcess;
    if (!listed)
        return finish("the Tools menu does not offer CDP Process to Sample");

    removeFiles();
    std::vector<double> voice(4096);
    for (std::size_t frame = 0; frame < voice.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / 48000.0;
        voice[frame] = 0.4 * std::sin(2.0 * std::numbers::pi * 330.0 * t)
            + 0.2 * std::sin(2.0 * std::numbers::pi * 990.0 * t);
    }
    std::string writeError;
    if (!talas::audio::writeFloat32Wav(sample, 48000, voice, writeError))
        return finish("the CDP sample could not be written: " + writeError);
    if (const auto replaced = projectBridge_.editor().replaceDocument(talas::project::ProjectDocument{});
        !replaced.ok())
        return finish("the CDP process tool project was rejected");
    documentChanged();

    if (!commandManager_.invokeDirectly(commandShowCdpProcess, false)
        || cdpProcessPanel_ == nullptr || cdpProcessWindow_ != nullptr)
        return finish("CDP Process to Sample did not open (without a window off screen)");
    if (!cdpProcessPanel_->setInputsForSmoke({sample})
        || !cdpProcessPanel_->setProcessForSmoke("stretchTime", 256, {{"stretch", "2.5"}})
        || !cdpProcessPanel_->processNowForSmoke(stretched))
        return finish("CDP Process to Sample did not render stretch time");
    const auto written = talas::audio::readSampleFile(stretched);
    if (!written.ok() || written.channels.size() != 1 || written.channels.front().size() < 2 * voice.size())
        return finish("CDP Process to Sample did not write a longer sample");
    const auto& document = projectBridge_.editor().document();
    const auto definition = std::ranges::find(
        document.soundDefinitions, std::string_view{"cdpExample1"},
        &talas::project::ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end() || definition->graph.nodes.empty()
        || definition->graph.nodes.front().typeId != "talas.sample")
        return finish("CDP Process to Sample did not leave an example Sound");
    const auto& parameters = definition->graph.nodes.front().parameters;
    const auto bound = std::ranges::find(parameters, std::string_view{"sample"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (bound == parameters.end() || !bound->resource.has_value()
        || std::ranges::none_of(document.assets, [&](const talas::project::ProjectAsset& asset) {
               return asset.id == *bound->resource;
           }))
        return finish("CDP Process to Sample's example Sound does not play the processed sample");
    return finish();
}
}
