#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/VcsLayout.h"
#include "talas/app/VcsPresets.h"
#include "talas/project/ProjectJson.h"

#include <filesystem>
#include <fstream>

namespace talas::app::workbench
{
// The Virtual Control Surface's camera, preset
// list, dice and frozen parameters, its keys, and a MIDI program change
// choosing a preset.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runVcsPresetsSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        projectBridge_.setPlaying(false);
        currentDocumentStaged_ = false;
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "VCS presets smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto expression = [](std::string id, std::string text) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(text);
        return parameter;
    };
    talas::project::ProjectDocument fixture;
    fixture.virtualControls.push_back(
        {"Cutoff", talas::expr::ValueDomain::Hertz, talas::project::VirtualControlKind::Fader, 440.0, 20.0, 20000.0});
    fixture.virtualControls.push_back(
        {"Vol", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});
    fixture.graph.nodes.push_back({"osc", "talas.oscillator", {expression("frequency", "!Cutoff"), expression("amplitude", "!Vol")}});
    fixture.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    fixture.graph.connections.push_back({"osc", "audioOut", "output", "audioIn"});
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("VCS presets fixture was rejected: " + replaced.message);
    documentChanged();
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    std::array<float, 8> output{};
    std::array<float*, 1> channels{output.data()};
    const auto render = [&] { return projectBridge_.render(channels.data(), channels.size(), output.size()); };
    const auto staged = projectBridge_.compileAndStage(settings);
    if (!staged.ok())
        return finish("VCS presets fixture did not stage: " + staged.message);
    projectBridge_.setPlaying(true);
    if (!render())
        return finish("VCS presets fixture did not render");
    const auto value = [this](std::string_view name) {
        for (const auto& [eventName, number] : talas::app::vcsControlValues(projectBridge_.performanceState().currentValues))
        {
            if (eventName == name)
                return number;
        }
        return -1.0;
    };

    virtualControlSurface_->refresh();
    if (!virtualControlSurface_->presetKeyForSmoke(juce::KeyPress{juce::KeyPress::returnKey})
        || virtualControlSurface_->presetLabelsForSmoke().size() != 2
        || virtualControlSurface_->presetLabelsForSmoke().front() != "Default [1]"
        || !virtualControlSurface_->presetLabelsForSmoke().back().endsWith("[2]"))
        return finish("Enter did not take a named preset after Default, each numbered by its program change");
    if (!virtualControlSurface_->toggleFrozenForSmoke("Vol"))
        return finish("clicking Vol's name did not freeze it");
    const auto* bank = talas::app::findVcsPresetBank(projectBridge_.editor().document());
    if (bank == nullptr || !talas::app::vcsControlFrozen(*bank, projectBridge_.editor().document().virtualControls.back()))
        return finish("the frozen Vol was not kept against the dice");
    if (!render() || !virtualControlSurface_->presetKeyForSmoke(juce::KeyPress{'r', 0, 'r'}) || !render())
        return finish("r did not roll the dice");
    if (value("Cutoff") == 440.0 || value("Vol") != 0.25)
        return finish("the dice did not move Cutoff and leave the frozen Vol");
    if (!virtualControlSurface_->selectPresetForSmoke(1) || !render() || value("Cutoff") != 440.0)
        return finish("choosing the taken preset did not set Cutoff back");
    if (!virtualControlSurface_->presetKeyForSmoke(juce::KeyPress{juce::KeyPress::upKey}) || !render())
        return finish("the up arrow did not choose the preset above");
    if (!virtualControlSurface_->selectPresetForProgramChange(2) || !render()
        || virtualControlSurface_->selectPresetForProgramChange(3))
        return finish("a MIDI program change did not choose the preset it numbers");
    if (!virtualControlSurface_->removePresetsBesidesDefaultForSmoke()
        || virtualControlSurface_->presetLabelsForSmoke().size() != 1)
        return finish("Remove All but Default did not leave Default alone");

    // Unlocked, the surface keeps its widgets
    // where they are; they move, resize, squeeze and clean up.
    const auto bounds = [this](std::string_view name) {
        return virtualControlSurface_->widgetBoundsForSmoke(name).value_or(juce::Rectangle<int>{});
    };
    if (!virtualControlSurface_->setEditModeForSmoke(true)
        || projectBridge_.editor().document().virtualControlLayout != talas::project::VirtualControlLayout::Fixed
        || !projectBridge_.editor().document().virtualControls.front().widget.bounds.has_value())
        return finish("unlocking the surface did not keep its widgets at their places");
    const auto before = bounds("Cutoff");
    if (!virtualControlSurface_->selectWidgetsForSmoke({"Cutoff"}) || !virtualControlSurface_->moveSelectionForSmoke(30, 20)
        || bounds("Cutoff").getPosition() != before.getPosition() + juce::Point<int>{30, 20})
        return finish("dragging the selected widget did not move it");
    if (!virtualControlSurface_->resizeWidgetForSmoke("Cutoff", -20, -40)
        || bounds("Cutoff").getWidth() != before.getWidth() - 20 || bounds("Cutoff").getHeight() != before.getHeight() - 40)
        return finish("dragging the corner did not resize the widget");
    if (!virtualControlSurface_->chooseSelectedValueForSmoke("Vol") || virtualControlSurface_->selectedValueForSmoke() != "Vol")
        return finish("choosing Vol from the Selected Value list did not select it");
    if (!virtualControlSurface_->selectWidgetsForSmoke({"Cutoff", "Vol"}) || virtualControlSurface_->selectedValueForSmoke() != "More than one")
        return finish("a selection of two did not show More than one");
    if (!virtualControlSurface_->squeezeForSmoke(false))
        return finish("Squeeze Horizontal did not squeeze the selection");
    if (!virtualControlSurface_->cleanUpForSmoke(1, false) || bounds("Vol").getX() != bounds("Cutoff").getX()
        || bounds("Vol").getY() <= bounds("Cutoff").getY())
        return finish("Clean up in one column did not stack the widgets");
    // Clean up on a surface no larger than its window
    // fits it to the window, as Fit does; a larger one keeps its sizes.
    {
        const auto window = virtualControlSurface_->getBounds();
        virtualControlSurface_->setSize(2400, 1600);
        const auto fitted = virtualControlSurface_->cleanUpForSmoke(1, false)
            && projectBridge_.editor().document().virtualControlLayout == talas::project::VirtualControlLayout::Fit;
        virtualControlSurface_->setSize(160, 120);
        const auto kept = virtualControlSurface_->cleanUpForSmoke(1, false)
            && projectBridge_.editor().document().virtualControlLayout == talas::project::VirtualControlLayout::Fixed;
        virtualControlSurface_->setBounds(window);
        if (!fitted || !kept)
            return finish("Clean up did not fit a surface the size of its window, or did not keep a larger one's sizes");
    }
    // Edit > Clean up with the surface in
    // front lays it out in the columns asked for, even locked.
    vcsFocusForSmoke_ = true;
    smokeCleanUpColumns_ = 2;
    const auto locked = virtualControlSurface_->setEditModeForSmoke(false);
    const auto cleaned = commandManager_.invokeDirectly(commandCleanUpGraph, false);
    vcsFocusForSmoke_ = false;
    smokeCleanUpColumns_.reset();
    if (!locked || !virtualControlSurface_->setEditModeForSmoke(true) || !cleaned
        || bounds("Vol").getY() != bounds("Cutoff").getY() || bounds("Vol").getX() == bounds("Cutoff").getX())
        return finish("Edit > Clean up did not lay the locked surface out in two columns");
    if (!virtualControlSurface_->selectWidgetsForSmoke({"Cutoff", "Vol"}))
        return finish("the widgets could not be selected again after locking");
    // A window smaller than its surface shows the box
    // that moves the view over it.
    const auto surfaceBounds = virtualControlSurface_->getBounds();
    virtualControlSurface_->setSize(std::min(surfaceBounds.getWidth(), 160), 120);
    const auto moved = virtualControlSurface_->navigatorDragForSmoke(0, 20);
    virtualControlSurface_->setBounds(surfaceBounds);
    if (!moved || moved->second <= 0)
        return finish("a surface larger than its window showed no box to move the view over it");
    // Align lines the selection up on its first
    // widget's left edge; a layout duplicated and merged, then removed.
    if (!virtualControlSurface_->alignForSmoke(static_cast<int>(talas::app::VcsAlignment::Left))
        || bounds("Vol").getX() != bounds("Cutoff").getX())
        return finish("Align did not line the selection up");
    const auto layoutsBefore = virtualControlSurface_->layoutLabelsForSmoke().size();
    if (!virtualControlSurface_->duplicateLayoutForSmoke()
        || virtualControlSurface_->layoutLabelsForSmoke().size() != layoutsBefore + 1
        || projectBridge_.editor().document().virtualControlLayoutPages.size() != 1)
        return finish("Duplicate did not make a layout of the editor's own");
    if (!virtualControlSurface_->mergeLayoutForSmoke(0) || projectBridge_.editor().document().virtualControlLayoutPages.size() != 2
        || !virtualControlSurface_->removeLayoutForSmoke() || projectBridge_.editor().document().virtualControlLayoutPages.size() != 1)
        return finish("Merge into new layout and Remove did not keep the layouts");
    if (!virtualControlSurface_->selectLayoutForSmoke(0))
        return finish("the top layout could not be chosen again");
    // Options > Add Widget > Text and Box,
    // the text moved and edited, pasted text filling the surface, and the
    // selected ones removed.
    using Decoration = talas::project::VirtualControlDecorationKind;
    if (!virtualControlSurface_->addDecorationForSmoke(Decoration::Text, "A Short Sequencer")
        || !virtualControlSurface_->addDecorationForSmoke(Decoration::Box, {})
        || projectBridge_.editor().document().virtualControlDecorations.size() != 2
        || virtualControlSurface_->decorationBoundsForSmoke().size() != 2)
        return finish("Add Widget did not put a text and a box on the surface");
    const auto text = virtualControlSurface_->decorationBoundsForSmoke().front();
    if (!virtualControlSurface_->selectWidgetsForSmoke({talas::app::vcsDecorationName(0)})
        || !virtualControlSurface_->moveSelectionForSmoke(0, 40)
        || virtualControlSurface_->decorationBoundsForSmoke().front().getY() != text.getY() + 40
        || !virtualControlSurface_->editDecorationTextForSmoke(0, "Sequencer")
        || projectBridge_.editor().document().virtualControlDecorations.front().text != "Sequencer")
        return finish("the text did not move or take its new words");
    if (!virtualControlSurface_->pasteTextForSmoke("Lyrics\nfor the performer")
        || virtualControlSurface_->decorationBoundsForSmoke().back().getX() != 0
        || virtualControlSurface_->decorationBoundsForSmoke().back().getY() != 0)
        return finish("pasted text did not fill the surface");
    if (!virtualControlSurface_->selectWidgetsForSmoke({talas::app::vcsDecorationName(1), talas::app::vcsDecorationName(2)})
        || !virtualControlSurface_->removeSelectedDecorationsForSmoke()
        || projectBridge_.editor().document().virtualControlDecorations.size() != 1)
        return finish("the selected texts and boxes were not removed");
    // Every widget selected and copied, one moved
    // and the copy pasted back at the places copied; a Sound's widgets copied
    // in the Sound Browser and pasted, the one with no control kept dormant.
    if (!virtualControlSurface_->selectAllWidgetsForSmoke() || virtualControlSurface_->selectedValueForSmoke() != "More than one"
        || !virtualControlSurface_->copyWidgetsForSmoke())
        return finish("Ctrl+A and Ctrl+C did not copy every widget");
    const auto copiedAt = bounds("Vol");
    if (!virtualControlSurface_->selectWidgetsForSmoke({"Vol"}) || !virtualControlSurface_->moveSelectionForSmoke(40, 0)
        || bounds("Vol") == copiedAt || !virtualControlSurface_->pasteWidgetsForSmoke() || bounds("Vol") != copiedAt)
        return finish("Ctrl+V did not paste the copied widgets at their places");
    auto other = fixture;
    other.virtualControls = {{"Vol", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 2.0},
        {"Pitch", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 60.0, 0.0, 127.0}};
    other.graph.nodes.front().parameters.front() = expression("frequency", "!Pitch");
    talas::project::reconcileProjectPresentation(other);
    const auto otherFile = std::filesystem::temp_directory_path() / "talas-vcs-widgets-smoke.talas.json";
    {
        std::ofstream written(otherFile, std::ios::binary | std::ios::trunc);
        written << talas::project::serializeProjectDocument(other);
    }
    const auto copiedFromBrowser = copyWidgetsFromSoundBrowser(
        {otherFile, {}, "talas-vcs-widgets-smoke", 0, talas::app::SoundBrowserKind::SoundFile}, false);
    std::filesystem::remove(otherFile);
    if (!copiedFromBrowser || !virtualControlSurface_->pasteWidgetsForSmoke())
        return finish("a Sound's widgets copied in the Sound Browser did not paste");
    const auto& pastedControls = projectBridge_.editor().document().virtualControls;
    const auto vol = std::ranges::find(pastedControls, std::string_view{"Vol"}, &talas::project::VirtualControl::eventName);
    const auto& dormant = projectBridge_.editor().document().dormantVirtualControls;
    if (vol == pastedControls.end() || vol->maximum != 2.0 || dormant.size() != 1 || dormant.front().eventName != "Pitch")
        return finish("the pasted widgets did not take their ranges, or the one with no control was not kept dormant");
    if (!virtualControlSurface_->setEditModeForSmoke(false) || !virtualControlSurface_->cleanUpForSmoke(1, true)
        || projectBridge_.editor().document().virtualControlLayout != talas::project::VirtualControlLayout::Fit)
        return finish("Fit did not fit the surface to its window");
    // Save as... first offers to remove the obsolete
    // layouts and presets, the dormant widget among them; kept, it stays,
    // removed, it goes as one edit Undo takes back, and with nothing left
    // Save as... goes on without asking.
    auto continued = 0;
    smokeInputChoice_ = 1;
    offerObsoleteMaterialRemoval([&] { ++continued; });
    const auto kept = projectBridge_.editor().document().dormantVirtualControls.size() == 1;
    smokeInputChoice_ = 0;
    offerObsoleteMaterialRemoval([&] { ++continued; });
    const auto removed = projectBridge_.editor().document().dormantVirtualControls.empty();
    smokeInputChoice_ = 99;
    offerObsoleteMaterialRemoval([&] { ++continued; });
    smokeInputChoice_.reset();
    const auto undone = projectBridge_.editor().undo().ok()
        && projectBridge_.editor().document().dormantVirtualControls.size() == 1;
    documentChanged();
    if (!kept || !removed || !undone || continued != 3)
        return finish("Save as... did not offer to remove the obsolete layouts and presets");
    // Locked again after changes, the surface asks
    // whether to save them; discarded, they are undone, saved, they stay.
    const auto unlockedAt = bounds("Vol");
    if (!virtualControlSurface_->setEditModeForSmoke(true) || !virtualControlSurface_->selectWidgetsForSmoke({"Vol"})
        || !virtualControlSurface_->moveSelectionForSmoke(30, 0) || bounds("Vol") == unlockedAt
        || !virtualControlSurface_->lockDiscardingForSmoke() || bounds("Vol") != unlockedAt)
        return finish("locking the surface did not discard the changes made since it was unlocked");
    if (!virtualControlSurface_->setEditModeForSmoke(true) || !virtualControlSurface_->selectWidgetsForSmoke({"Vol"})
        || !virtualControlSurface_->moveSelectionForSmoke(30, 0) || !virtualControlSurface_->setEditModeForSmoke(false)
        || bounds("Vol") == unlockedAt)
        return finish("locking the surface did not save the changes made since it was unlocked");
    return finish();
}
}
