#include "app/workbench/WorkbenchGeneratorPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/GeneratorPreview.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace talas::app::workbench
{
namespace
{
constexpr std::size_t maximumTerminalEditorBytes =
    talas::generative::maximumContextFreeSymbols
    * (talas::generative::maximumContextFreeSymbolBytes + 1U);

template <typename Integer>
[[nodiscard]] bool exactUnsigned(
    const juce::String& text,
    Integer maximum,
    Integer& output)
{
    const auto source = text.toStdString();
    if (source.empty())
        return false;
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        source.data(), source.data() + source.size(), value);
    if (error != std::errc{} || end != source.data() + source.size()
        || value > static_cast<std::uint64_t>(maximum))
    {
        return false;
    }
    output = static_cast<Integer>(value);
    return true;
}

[[nodiscard]] std::vector<std::string> terminalLines(
    std::string_view source)
{
    std::vector<std::string> terminals;
    if (source.empty())
        return terminals;
    std::size_t begin = 0;
    while (begin <= source.size())
    {
        const auto end = source.find('\n', begin);
        const auto count = end == std::string::npos
            ? source.size() - begin
            : end - begin;
        terminals.emplace_back(source.substr(begin, count));
        if (end == std::string::npos)
            break;
        begin = end + 1U;
    }
    return terminals;
}

[[nodiscard]] bool boundedUtf8(
    const juce::String& text,
    std::size_t maximumBytes,
    std::string& output)
{
    if (static_cast<std::size_t>(text.getNumBytesAsUTF8()) > maximumBytes)
        return false;
    output = text.toStdString();
    return true;
}

[[nodiscard]] juce::String terminalText(
    const std::vector<std::string>& terminals)
{
    juce::String result;
    for (std::size_t index = 0; index < terminals.size(); ++index)
    {
        if (index != 0U)
            result += "\n";
        result += juceString(terminals[index]);
    }
    return result;
}

class GeneratorPanelImpl final : public GeneratorPanel,
                                 private juce::Timer
{
public:
    GeneratorPanelImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        const talas::core::NodeRegistry& registry,
        DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus)
        : bridge_(bridge)
        , registry_(registry)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , newButton_("New")
        , addActionButton_("Add Output")
        , removeActionButton_("Remove Output")
        , saveButton_("Save")
        , deleteButton_("Delete")
        , validateButton_("Validate")
        , generateButton_("Preview Symbols")
        , timelineButton_("Generate Timeline")
        , scoreButton_("Generate Score")
        , soundButton_("Generate Sound")
    {
        describeControl(
            *this,
            "Generator authoring",
            "Create bounded deterministic symbol generators, preview symbols, and apply exact Timeline, Score, or reusable-Sound output as one Undo step.");
        setFocusContainerType(juce::Component::FocusContainerType::none);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &programBox_, &newButton_, &generatorId_, &displayName_,
                 &terminals_, &seed_, &rewritePasses_, &outputSymbols_,
                 &seedLabel_, &rewritePassesLabel_, &outputSymbolsLabel_,
                 &grammar_, &timelineGridLabel_, &scoreGridLabel_,
                 &timelineBaseLabel_, &scoreBaseLabel_, &timelineGrid_,
                 &scoreGrid_, &timelineBase_, &scoreBase_, &namespaceLabel_,
                 &outputNamespace_, &soundIdLabel_, &soundNameLabel_,
                 &soundDefinitionId_, &soundDisplayName_,
                 &terminalPicker_, &actionKindPicker_,
                 &primaryTargetPicker_, &secondaryTargetPicker_,
                 &actionList_, &addActionButton_, &removeActionButton_,
                 &saveButton_, &deleteButton_, &validateButton_,
                 &generateButton_, &timelineButton_, &scoreButton_,
                 &soundButton_,
                 &summary_, &status_, &preview_})
        {
            addAndMakeVisible(*component);
        }

        programBox_.setComponentID("inspector.generator.program");
        newButton_.setComponentID("inspector.generator.new");
        generatorId_.setInputRestrictions(
            static_cast<int>(talas::generative::maximumContextFreeSymbolBytes));
        displayName_.setInputRestrictions(
            static_cast<int>(talas::generative::maximumContextFreeDisplayNameBytes));
        terminals_.setInputRestrictions(
            static_cast<int>(maximumTerminalEditorBytes));
        grammar_.setInputRestrictions(
            static_cast<int>(talas::generative::maximumContextFreeSourceBytes));
        seed_.setInputRestrictions(20, "0123456789");
        rewritePasses_.setInputRestrictions(3, "0123456789");
        outputSymbols_.setInputRestrictions(4, "0123456789");
        timelineGrid_.setInputRestrictions(14, "0123456789");
        scoreGrid_.setInputRestrictions(20, "0123456789");
        timelineBase_.setInputRestrictions(14, "0123456789");
        scoreBase_.setInputRestrictions(20, "0123456789");
        outputNamespace_.setInputRestrictions(
            static_cast<int>(talas::project::maximumProjectGeneratorNamespaceBytes));
        soundDefinitionId_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectGeneratorSoundDefinitionIdBytes));
        soundDisplayName_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectGeneratorSoundDisplayNameBytes));

        const auto editorChanged = [this] { updateActionState(); };
        for (auto* editor : {&generatorId_, &displayName_, &terminals_,
                 &seed_, &rewritePasses_, &outputSymbols_, &grammar_, &timelineGrid_, &scoreGrid_,
                             &timelineBase_, &scoreBase_, &outputNamespace_,
                             &soundDefinitionId_, &soundDisplayName_})
        {
            editor->onTextChange = editorChanged;
        }
        terminals_.onTextChange = [this]
        {
            refreshTerminalPicker();
            updateActionState();
        };

        for (auto* label : {&seedLabel_, &rewritePassesLabel_,
                 &outputSymbolsLabel_})
        {
            label->setColour(
                juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::microStrong());
        }
        seedLabel_.setText("Seed", juce::dontSendNotification);
        rewritePassesLabel_.setText("Passes", juce::dontSendNotification);
        outputSymbolsLabel_.setText("Output", juce::dontSendNotification);
        timelineGridLabel_.setText("Timeline grid us", juce::dontSendNotification);
        scoreGridLabel_.setText("Score grid ticks", juce::dontSendNotification);
        timelineBaseLabel_.setText("Timeline base us", juce::dontSendNotification);
        scoreBaseLabel_.setText("Score base tick", juce::dontSendNotification);
        namespaceLabel_.setText("Output namespace", juce::dontSendNotification);
        soundIdLabel_.setText("Sound ID", juce::dontSendNotification);
        soundNameLabel_.setText("Name", juce::dontSendNotification);
        for (auto* label : {&timelineGridLabel_, &scoreGridLabel_,
                 &timelineBaseLabel_, &scoreBaseLabel_, &namespaceLabel_,
                 &soundIdLabel_, &soundNameLabel_})
        {
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::microStrong());
        }

        generatorId_.setTextToShowWhenEmpty("stable generator ID", secondaryTextColour);
        displayName_.setTextToShowWhenEmpty("display name", secondaryTextColour);
        terminals_.setTextToShowWhenEmpty("one terminal per line", secondaryTextColour);
        grammar_.setTextToShowWhenEmpty(
            "start: phrase\nphrase -> note phrase | rest phrase | note",
            secondaryTextColour);
        grammar_.setMultiLine(true);
        grammar_.setReturnKeyStartsNewLine(true);
        grammar_.setScrollbarsShown(true);
        soundDefinitionId_.setTextToShowWhenEmpty(
            "newSoundId", secondaryTextColour);
        soundDisplayName_.setTextToShowWhenEmpty(
            "Generated Sound", secondaryTextColour);
        actionKindPicker_.addItem("Timeline Event Value", 1);
        actionKindPicker_.addItem("Score clip", 2);
        actionKindPicker_.addItem("Score automation point", 3);
        actionKindPicker_.addItem("Score marker", 4);
        actionKindPicker_.addItem("Graph node", 5);
        actionKindPicker_.setSelectedId(1, juce::dontSendNotification);
        actionKindPicker_.onChange = [this] { refreshTargetPickers(); };
        actionList_.onChange = [this] { updateActionState(); };
        addActionButton_.onClick = [this] { addOutputAction(); };
        removeActionButton_.onClick = [this] { removeOutputAction(); };
        terminals_.setMultiLine(true);
        terminals_.setReturnKeyStartsNewLine(true);
        terminals_.setScrollbarsShown(true);
        preview_.setMultiLine(true);
        preview_.setReadOnly(true);
        preview_.setCaretVisible(false);
        preview_.setScrollbarsShown(true);
        preview_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        preview_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        status_.setColour(juce::Label::textColourId, secondaryTextColour);
        status_.setFont(WorkbenchType::micro());
        summary_.setColour(juce::Label::textColourId, secondaryTextColour);
        summary_.setFont(WorkbenchType::microStrong());

        describeControl(
            programBox_,
            "Generator program",
            "Select one project-owned generator by stable ID.");
        describeControl(
            generatorId_,
            "Generator stable ID",
                        "Bounded ASCII identifier used for edit, focus, and "
                        "preview authority; never a row index.");
        describeControl(
            terminals_,
            "Declared terminals",
                        "One explicit terminal symbol per line, up to 128 bounded "
                        "ASCII identifiers.");
        describeControl(
            seed_,
            "Deterministic seed",
            "Exact unsigned 64-bit seed for deterministic alternative selection.");
        describeControl(
            rewritePasses_,
            "Maximum rewrite passes",
            "Fixed rewrite-pass limit from 0 through 128.");
        describeControl(
            outputSymbols_,
            "Maximum output symbols",
            "Fixed generated-symbol limit from 1 through 4096.");
        describeControl(
            grammar_,
            "Canonical context-free grammar",
                        "One start sequence and ordered rewrite rules. No files, network, "
                        "scripts, expressions, native calls, or audio actions.");
        describeControl(
            validateButton_,
            "Validate generator",
                        "Checks exact grammar ranges and all fixed limits without "
                        "changing the project or prior preview.");
        describeControl(
            generateButton_, "Preview bounded symbols",
                        "Runs off the audio callback and publishes symbols only when exact "
                        "document generation, bytes, generator ID, and checksum still match.");
        describeControl(terminalPicker_, "Output terminal",
                        "Select one exact declared terminal. Unbound terminals "
                        "remain explicit rests.");
        describeControl(actionKindPicker_, "Output action kind",
                        "Choose one closed Timeline, Score, or restricted graph-chain action.");
        describeControl(primaryTargetPicker_, "Exact output target",
                        "Lists only currently available Event Values, musical-tick "
                        "tracks, automation lanes, or approved graph node types for the selected action.");
        describeControl(secondaryTargetPicker_, "Exact Sound target",
                        "For Score clips, selects one existing Sound definition.");
        describeControl(actionList_, "Persisted output bindings",
                        "Ordered terminal actions. Select one exact row to remove "
                        "it; row order is persisted authority.");
        describeControl(timelineButton_, "Generate Timeline",
                        "Builds one bounded Timeline Event Value candidate "
                        "off-thread and applies it as one Undo after exact "
                        "generation and canonical-byte rechecks.");
        describeControl(scoreButton_, "Generate Score",
                        "Builds one bounded clip, automation, and marker candidate off-thread "
                        "and applies it as one Undo after exact authority rechecks.");
        describeControl(soundDefinitionId_, "Generated Sound stable ID",
                        "Exact bounded reusable-Sound identity. Generation rejects collisions.");
        describeControl(soundDisplayName_, "Generated Sound display name",
                        "Bounded user-facing name for the new reusable Sound.");
        describeControl(soundButton_, "Generate Sound",
                        "Builds a bounded source-to-processor chain off-thread, previews exact counts, and appends one reusable Sound as one Undo.");
        describeControl(
            preview_,
            "Last successful generator result",
                        "Retains the last-good bounded symbol sequence or graph summary when "
                        "validation, generation, cancellation, or authority admission fails.");

        programBox_.onChange = [this] { selectFromBox(); };
        newButton_.onClick = [this] { beginNew(); };
        saveButton_.onClick = [this] { save(); };
        deleteButton_.onClick = [this] { confirmDelete(); };
        validateButton_.onClick = [this] { validateOnly(); };
        generateButton_.onClick = [this] { startPreview(); };
        timelineButton_.onClick = [this] { startProjection(ProjectionKind::Timeline); };
        scoreButton_.onClick = [this] { startProjection(ProjectionKind::Score); };
        soundButton_.onClick = [this] { startProjection(ProjectionKind::Sound); };
        refresh();
        startTimerHz(30);
    }

    ~GeneratorPanelImpl() override
    {
        stopTimer();
        stopPreview();
        stopProjection();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this, juce::AccessibilityRole::group);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(4);
        auto selection = content.removeFromTop(24);
        newButton_.setBounds(selection.removeFromRight(48));
        selection.removeFromRight(4);
        programBox_.setBounds(selection);
        content.removeFromTop(4);
        generatorId_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        displayName_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        auto numericLabels = content.removeFromTop(14);
        const auto labelWidth =
            std::max(1, (numericLabels.getWidth() - 8) / 3);
        seedLabel_.setBounds(numericLabels.removeFromLeft(labelWidth));
        numericLabels.removeFromLeft(4);
        rewritePassesLabel_.setBounds(
            numericLabels.removeFromLeft(labelWidth));
        numericLabels.removeFromLeft(4);
        outputSymbolsLabel_.setBounds(numericLabels);
        auto numeric = content.removeFromTop(24);
        const auto numericWidth = std::max(1, (numeric.getWidth() - 8) / 3);
        seed_.setBounds(numeric.removeFromLeft(numericWidth));
        numeric.removeFromLeft(4);
        rewritePasses_.setBounds(numeric.removeFromLeft(numericWidth));
        numeric.removeFromLeft(4);
        outputSymbols_.setBounds(numeric);
        content.removeFromTop(3);
        terminals_.setBounds(content.removeFromTop(54));
        content.removeFromTop(3);
        grammar_.setBounds(content.removeFromTop(
            std::max(48, std::min(72, content.getHeight() / 5))));
        content.removeFromTop(3);
        auto outputLabels = content.removeFromTop(14);
        const auto outputLabelWidth = std::max(1, (outputLabels.getWidth() - 12) / 4);
        timelineGridLabel_.setBounds(outputLabels.removeFromLeft(outputLabelWidth));
        outputLabels.removeFromLeft(4);
        scoreGridLabel_.setBounds(outputLabels.removeFromLeft(outputLabelWidth));
        outputLabels.removeFromLeft(4);
        timelineBaseLabel_.setBounds(outputLabels.removeFromLeft(outputLabelWidth));
        outputLabels.removeFromLeft(4);
        scoreBaseLabel_.setBounds(outputLabels);
        auto outputValues = content.removeFromTop(24);
        const auto outputValueWidth = std::max(1, (outputValues.getWidth() - 12) / 4);
        timelineGrid_.setBounds(outputValues.removeFromLeft(outputValueWidth));
        outputValues.removeFromLeft(4);
        scoreGrid_.setBounds(outputValues.removeFromLeft(outputValueWidth));
        outputValues.removeFromLeft(4);
        timelineBase_.setBounds(outputValues.removeFromLeft(outputValueWidth));
        outputValues.removeFromLeft(4);
        scoreBase_.setBounds(outputValues);
        content.removeFromTop(3);
        auto namespaceRow = content.removeFromTop(24);
        namespaceLabel_.setBounds(namespaceRow.removeFromLeft(92));
        outputNamespace_.setBounds(namespaceRow);
        content.removeFromTop(3);
        auto soundRow = content.removeFromTop(24);
        soundIdLabel_.setBounds(soundRow.removeFromLeft(56));
        const auto soundFieldWidth = std::max(
            1,
            (soundRow.getWidth() - 48 - 4) / 2);
        soundDefinitionId_.setBounds(
            soundRow.removeFromLeft(soundFieldWidth));
        soundRow.removeFromLeft(4);
        soundNameLabel_.setBounds(soundRow.removeFromLeft(48));
        soundDisplayName_.setBounds(soundRow);
        content.removeFromTop(3);
        auto bindingRow = content.removeFromTop(24);
        const auto bindingWidth = std::max(1, (bindingRow.getWidth() - 12) / 4);
        terminalPicker_.setBounds(bindingRow.removeFromLeft(bindingWidth));
        bindingRow.removeFromLeft(4);
        actionKindPicker_.setBounds(bindingRow.removeFromLeft(bindingWidth));
        bindingRow.removeFromLeft(4);
        primaryTargetPicker_.setBounds(bindingRow.removeFromLeft(bindingWidth));
        bindingRow.removeFromLeft(4);
        secondaryTargetPicker_.setBounds(bindingRow);
        content.removeFromTop(3);
        auto bindingActions = content.removeFromTop(24);
        removeActionButton_.setBounds(bindingActions.removeFromRight(92));
        bindingActions.removeFromRight(4);
        addActionButton_.setBounds(bindingActions.removeFromRight(92));
        bindingActions.removeFromRight(4);
        actionList_.setBounds(bindingActions);
        content.removeFromTop(4);
        auto actions = content.removeFromTop(24);
        const auto actionWidth = std::max(1, (actions.getWidth() - 12) / 4);
        saveButton_.setBounds(actions.removeFromLeft(actionWidth));
        actions.removeFromLeft(4);
        deleteButton_.setBounds(actions.removeFromLeft(actionWidth));
        actions.removeFromLeft(4);
        validateButton_.setBounds(actions.removeFromLeft(actionWidth));
        actions.removeFromLeft(4);
        generateButton_.setBounds(actions);
        content.removeFromTop(3);
        auto projectionActions = content.removeFromTop(24);
        const auto projectionWidth = std::max(1, (projectionActions.getWidth() - 8) / 3);
        timelineButton_.setBounds(projectionActions.removeFromLeft(projectionWidth));
        projectionActions.removeFromLeft(4);
        scoreButton_.setBounds(projectionActions.removeFromLeft(projectionWidth));
        projectionActions.removeFromLeft(4);
        soundButton_.setBounds(projectionActions);
        content.removeFromTop(3);
        summary_.setBounds(content.removeFromTop(14));
        content.removeFromTop(3);
        status_.setBounds(content.removeFromTop(34));
        content.removeFromTop(3);
        preview_.setBounds(content);
    }

    void refresh() override
    {
        const auto initialRefresh = !hasRefreshed_;
        const auto authority =
            bridge_.editor().documentAuthorityGeneration();
        if (observedAuthority_ != 0 && observedAuthority_ != authority)
        {
            stopPreview();
            stopProjection();
            coordinator_.invalidate();
        }
        observedAuthority_ = authority;

        const auto retained = selectedGeneratorId_;
        programBox_.clear(juce::dontSendNotification);
        const auto& programs =
            bridge_.editor().document().generatorLibrary.programs;
        for (std::size_t index = 0; index < programs.size(); ++index)
        {
            programBox_.addItem(
                juceString(programs[index].displayName + " / "
                           + programs[index].generatorId),
                static_cast<int>(index + 1U));
        }
        auto selectedIndex = -1;
        for (std::size_t index = 0; index < programs.size(); ++index)
        {
            if (programs[index].generatorId == retained)
            {
                selectedIndex = static_cast<int>(index);
                break;
            }
        }
        if (selectedIndex < 0 && retained.empty()
            && initialRefresh && !programs.empty())
            selectedIndex = 0;
        if (selectedIndex >= 0)
        {
            programBox_.setSelectedItemIndex(
                selectedIndex, juce::dontSendNotification);
            load(programs[static_cast<std::size_t>(selectedIndex)]);
        }
        else
        {
            beginNew();
        }
        hasRefreshed_ = true;
        updateActionState();
    }

    void focusGenerators() override
    {
        if (programBox_.isShowing())
            programBox_.grabKeyboardFocus();
    }

    void createGeneratorForSmoke(
        talas::project::ProjectGeneratorProgram program) override
    {
        const auto result =
            bridge_.editor().upsertProjectGenerator(std::move(program));
        if (result.ok())
        {
            onDocumentChange_();
            refresh();
        }
    }

    bool selectGeneratorForSmoke(std::string_view generatorId) override
    {
        const auto& programs =
            bridge_.editor().document().generatorLibrary.programs;
        for (std::size_t index = 0; index < programs.size(); ++index)
        {
            if (programs[index].generatorId == generatorId)
            {
                programBox_.setSelectedItemIndex(
                    static_cast<int>(index), juce::sendNotificationSync);
                return selectedGeneratorId_ == generatorId;
            }
        }
        return false;
    }

    bool generateForSmoke() override
    {
        startPreview(false);
        return coordinator_.snapshot().available();
    }

    bool generateTimelineForSmoke() override
    {
        return startProjection(ProjectionKind::Timeline, true);
    }

    bool generateScoreForSmoke() override
    {
        return startProjection(ProjectionKind::Score, false);
    }

    bool generateSoundForSmoke() override
    {
        return startProjection(ProjectionKind::Sound, true);
    }

    bool projectionFinishedForSmoke() override
    {
        if (projectionCompletion_ != nullptr
            && projectionCompletion_->ready.load(
                std::memory_order_acquire))
        {
            timerCallback();
        }
        return !projectionWorker_.has_value()
            && projectionCompletion_ == nullptr;
    }

    bool configureProjectionForSmoke(std::uint64_t timelineBaseMicroseconds,
                                     std::uint64_t scoreBaseTick,
                                     std::string_view outputNamespace) override
    {
        timelineBase_.setText(juceString(std::to_string(timelineBaseMicroseconds)), false);
        scoreBase_.setText(juceString(std::to_string(scoreBaseTick)), false);
        outputNamespace_.setText(juceString(outputNamespace), false);
        updateActionState();
        return timelineBase_.getText()
                   == juceString(std::to_string(timelineBaseMicroseconds))
            && scoreBase_.getText()
                == juceString(std::to_string(scoreBaseTick))
            && outputNamespace_.getText() == juceString(outputNamespace);
    }

    bool configureSoundProjectionForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view outputNamespace) override
    {
        soundDefinitionId_.setText(juceString(definitionId), false);
        soundDisplayName_.setText(juceString(displayName), false);
        outputNamespace_.setText(juceString(outputNamespace), false);
        updateActionState();
        return soundDefinitionId_.getText() == juceString(definitionId)
            && soundDisplayName_.getText() == juceString(displayName)
            && outputNamespace_.getText() == juceString(outputNamespace);
    }

    bool projectionActionsEnabledForSmoke(
        bool timelineExpected,
        bool scoreExpected,
        bool soundExpected) override
    {
        updateActionState();
        return timelineButton_.isEnabled() == timelineExpected
            && scoreButton_.isEnabled() == scoreExpected
            && soundButton_.isEnabled() == soundExpected;
    }

    bool requestDeleteForSmoke() override
    {
        const auto expected = selectedGeneratorId_;
        confirmDelete();
        return !expected.empty()
            && deleteConfirmationId_ == expected
            && deleteConfirmationWindow_ != nullptr
            && deleteConfirmationWindow_->isVisible()
            && deleteConfirmationWindow_->getButton("Delete") != nullptr;
    }

    bool acceptDeleteForSmoke() override
    {
        if (deleteConfirmationId_.empty())
            return false;
        if (deleteConfirmationWindow_ == nullptr)
            return false;
        auto* button = deleteConfirmationWindow_->getButton("Delete");
        if (button == nullptr || !button->onClick)
            return false;
        button->onClick();
        return true;
    }

    bool surfaceReadyForSmoke(
        std::string_view generatorId,
        std::string_view expectedPreview) const override
    {
        return selectedGeneratorId_ == generatorId
            && preview_.getText() == juceString(expectedPreview)
            && generatorId_.getComponentID().contains(
                juceString(generatorId))
            && grammar_.getComponentID().contains(
                juceString(generatorId));
    }

    bool soundSurfaceReadyForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view expectedPreview) override
    {
        return soundDefinitionId_.getText() == juceString(definitionId)
            && soundDisplayName_.getText() == juceString(displayName)
            && preview_.getText() == juceString(expectedPreview)
            && soundDefinitionId_.getComponentID().contains(
                "sound-definition-id")
            && soundDisplayName_.getComponentID().contains(
                "sound-display-name")
            && soundButton_.getComponentID().contains("generate-sound")
            && soundDefinitionId_.isAccessible()
            && soundDisplayName_.isAccessible()
            && soundButton_.isAccessible()
            && !soundDefinitionId_.getTitle().isEmpty()
            && !soundDisplayName_.getTitle().isEmpty()
            && !soundButton_.getTitle().isEmpty();
    }

    bool graphNodePickerReadyForSmoke() override
    {
        actionKindPicker_.setSelectedId(5, juce::sendNotificationSync);
        auto hasOscillator = false;
        auto hasFilter = false;
        auto hasRejectedTransformer = false;
        auto hasRejectedOutput = false;
        auto hasRejectedGrammar = false;
        auto hasRejectedLime = false;
        auto hasRejectedScript = false;
        for (int index = 0; index < primaryTargetPicker_.getNumItems(); ++index)
        {
            const auto item = primaryTargetPicker_.getItemText(index);
            hasOscillator = hasOscillator || item == "talas.oscillator";
            hasFilter = hasFilter || item == "talas.filter";
            hasRejectedTransformer = hasRejectedTransformer
                || item == "talas.parameterTransformer";
            hasRejectedOutput = hasRejectedOutput
                || item == "talas.audioOutput";
            hasRejectedGrammar = hasRejectedGrammar
                || item == "talas.contextFreeGrammar";
            hasRejectedLime = hasRejectedLime
                || item == "talas.limeInterpreter";
            hasRejectedScript = hasRejectedScript
                || item == "talas.script";
        }
        return hasOscillator && hasFilter
            && !hasRejectedTransformer && !hasRejectedOutput
            && !hasRejectedGrammar && !hasRejectedLime
            && !hasRejectedScript;
    }

private:
    enum class ProjectionKind
    {
        Timeline,
        Score,
        Sound,
    };
    enum class ProjectionWorkerFailure
    {
        None,
        Allocation,
        Exception,
    };
    struct PreviewCompletion
    {
        std::uint64_t workerGeneration = 0;
        std::optional<talas::app::GeneratorPreviewRequest> request;
        talas::app::GeneratorPreviewCandidate candidate;
        std::atomic<bool> ready = false;
    };

    struct ProjectionCompletion
    {
        std::uint64_t workerGeneration = 0;
        ProjectionKind kind = ProjectionKind::Timeline;
        talas::project::ProjectGeneratorEditAuthority authority;
        talas::project::ProjectGeneratorTimelineResult timeline;
        talas::project::ProjectGeneratorScoreResult score;
        talas::project::ProjectGeneratorSoundResult sound;
        std::string message;
        ProjectionWorkerFailure workerFailure =
            ProjectionWorkerFailure::None;
        std::atomic<bool> ready = false;
    };

    [[nodiscard]] std::optional<talas::expr::ValueDomain>
    eventTargetDomain(std::string_view name) const
    {
        for (const auto &control : bridge_.editor().document().virtualControls)
        {
            if (control.eventName == name)
                return control.domain;
        }
        for (const auto &event : bridge_.editor().document().timeline.eventValues)
        {
            if (event.eventName == name)
                return event.domain;
        }
        if (name == "Pitch")
            return talas::expr::ValueDomain::NoteNumber;
        if (name == "KeyVelocity" || name == "KeyDown")
            return talas::expr::ValueDomain::Scalar;
        return std::nullopt;
    }

    [[nodiscard]] bool automationLaneUsesMusicalTicks(
        const talas::project::ScoreAutomationLane& lane) const
    {
        const auto& score = bridge_.editor().document().timeline.score;
        std::string_view trackId = lane.ownerId;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto clip = std::ranges::find(
                score.clips,
                lane.ownerId,
                &talas::project::ScoreClip::clipId);
            if (clip == score.clips.end())
                return false;
            trackId = clip->trackId;
        }
        const auto track = std::ranges::find(
            score.tracks,
            trackId,
            &talas::project::ScoreTrack::trackId);
        return track != score.tracks.end()
            && track->timeDomain
                == talas::project::ScoreTimeDomain::MusicalTicks;
    }

    void refreshTerminalPicker()
    {
        const auto retained = terminalPicker_.getText();
        terminalPicker_.clear(juce::dontSendNotification);
        const auto terminals = terminalLines(terminals_.getText().toStdString());
        for (std::size_t index = 0; index < terminals.size(); ++index)
        {
            terminalPicker_.addItem(juceString(terminals[index]), static_cast<int>(index + 1U));
        }
        const auto retainedIndex = std::ranges::find(terminals, retained.toStdString());
        if (retainedIndex != terminals.end())
        {
            terminalPicker_.setSelectedItemIndex(
                static_cast<int>(retainedIndex - terminals.begin()), juce::dontSendNotification);
        }
        else if (!terminals.empty())
        {
            terminalPicker_.setSelectedItemIndex(0, juce::dontSendNotification);
        }
    }

    void refreshTargetPickers()
    {
        const auto retainedPrimary = primaryTargetPicker_.getText();
        const auto retainedSecondary = secondaryTargetPicker_.getText();
        primaryTargetPicker_.clear(juce::dontSendNotification);
        secondaryTargetPicker_.clear(juce::dontSendNotification);
        const auto kind = actionKindPicker_.getSelectedId();
        std::vector<std::string> primary;
        std::vector<std::string> secondary;
        const auto addUnique = [](auto &values, std::string_view value)
        {
            if (!value.empty() && std::ranges::find(values, value) == values.end())
            {
                values.emplace_back(value);
            }
        };
        if (kind == 1)
        {
            addUnique(primary, "Pitch");
            addUnique(primary, "KeyVelocity");
            addUnique(primary, "KeyDown");
            for (const auto &control : bridge_.editor().document().virtualControls)
                addUnique(primary, control.eventName);
            for (const auto &event : bridge_.editor().document().timeline.eventValues)
                addUnique(primary, event.eventName);
        }
        else if (kind == 2)
        {
            for (const auto &track : bridge_.editor().document().timeline.score.tracks)
            {
                if (track.timeDomain == talas::project::ScoreTimeDomain::MusicalTicks)
                    addUnique(primary, track.trackId);
            }
            for (const auto &definition : bridge_.editor().document().soundDefinitions)
                addUnique(secondary, definition.definitionId);
        }
        else if (kind == 3)
        {
            for (const auto &lane : bridge_.editor().document().timeline.score.automation)
            {
                if (automationLaneUsesMusicalTicks(lane))
                    addUnique(primary, lane.laneId);
            }
        }
        else if (kind == 5)
        {
            for (const auto& descriptor : registry_.nodeTypes())
            {
                if (talas::project::validateProjectGeneratorGraphNodeType(
                        descriptor.typeId,
                        registry_).ok())
                {
                    addUnique(primary, descriptor.typeId);
                }
            }
            std::ranges::sort(primary);
        }
        for (std::size_t index = 0; index < primary.size(); ++index)
        {
            primaryTargetPicker_.addItem(juceString(primary[index]), static_cast<int>(index + 1U));
        }
        for (std::size_t index = 0; index < secondary.size(); ++index)
        {
            secondaryTargetPicker_.addItem(juceString(secondary[index]),
                                           static_cast<int>(index + 1U));
        }
        const auto select =
            [](juce::ComboBox &box, const auto &values, const juce::String &retained)
        {
            const auto found = std::ranges::find(values, retained.toStdString());
            if (found != values.end())
            {
                box.setSelectedItemIndex(static_cast<int>(found - values.begin()),
                                         juce::dontSendNotification);
            }
            else if (!values.empty())
            {
                box.setSelectedItemIndex(0, juce::dontSendNotification);
            }
        };
        select(primaryTargetPicker_, primary, retainedPrimary);
        select(secondaryTargetPicker_, secondary, retainedSecondary);
        primaryTargetPicker_.setEnabled(kind != 4 && !primary.empty());
        secondaryTargetPicker_.setEnabled(kind == 2 && !secondary.empty());
        updateActionState();
    }

    [[nodiscard]] juce::String
    actionText(std::string_view terminal,
               const talas::project::ProjectGeneratorOutputAction &action) const
    {
        juce::String result = juceString(terminal) + " -> ";
        std::visit(
            [&](const auto &typed)
            {
                using Action = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Action,
                                             talas::project::ProjectGeneratorTimelineEventAction>)
                    result += "Timeline " + juceString(typed.eventName);
                else if constexpr (std::is_same_v<Action,
                                                  talas::project::ProjectGeneratorScoreClipAction>)
                    result += "Clip " + juceString(typed.trackId) + " / " +
                              juceString(typed.soundDefinitionId);
                else if constexpr (std::is_same_v<
                                       Action, talas::project::ProjectGeneratorScoreAutomationAction>)
                    result += "Automation " + juceString(typed.laneId);
                else if constexpr (std::is_same_v<
                                       Action, talas::project::ProjectGeneratorScoreMarkerAction>)
                    result += "Marker " + juceString(typed.title);
                else
                    result += "Graph node " + juceString(typed.nodeTypeId);
            },
            action);
        return result;
    }

    void refreshActionList()
    {
        actionRows_.clear();
        actionList_.clear(juce::dontSendNotification);
        auto itemId = 1;
        for (std::size_t bindingIndex = 0; bindingIndex < workingOutput_.bindings.size();
             ++bindingIndex)
        {
            const auto &binding = workingOutput_.bindings[bindingIndex];
            for (std::size_t actionIndex = 0; actionIndex < binding.actions.size(); ++actionIndex)
            {
                actionRows_.emplace_back(bindingIndex, actionIndex);
                actionList_.addItem(actionText(binding.terminal, binding.actions[actionIndex]),
                                    itemId++);
            }
        }
        if (!actionRows_.empty())
            actionList_.setSelectedItemIndex(0, juce::dontSendNotification);
        updateActionState();
    }

    void addOutputAction()
    {
        const auto terminal = terminalPicker_.getText().toStdString();
        if (terminal.empty())
        {
            setStatus("Select one exact declared terminal", true);
            return;
        }
        auto candidate = workingOutput_;
        auto binding = std::ranges::find(candidate.bindings, terminal,
                                         &talas::project::ProjectGeneratorTerminalBinding::terminal);
        if (binding == candidate.bindings.end())
        {
            candidate.bindings.push_back({terminal, {}});
            binding = std::prev(candidate.bindings.end());
        }
        const auto kind = actionKindPicker_.getSelectedId();
        const auto primary = primaryTargetPicker_.getText().toStdString();
        if (kind == 1)
        {
            const auto domain = eventTargetDomain(primary);
            if (!domain.has_value())
            {
                setStatus("Select one available Event Value target", true);
                return;
            }
            binding->actions.push_back(talas::project::ProjectGeneratorTimelineEventAction{
                primary,
                *domain,
                0.0,
                talas::core::AutomationInterpolation::Step,
            });
        }
        else if (kind == 2)
        {
            std::uint64_t duration = 0;
            const auto definition = secondaryTargetPicker_.getText().toStdString();
            if (primary.empty() || definition.empty() ||
                !exactUnsigned(scoreGrid_.getText(), std::numeric_limits<std::uint64_t>::max(),
                               duration) ||
                duration == 0)
            {
                setStatus("Select a musical track, Sound, and positive Score grid", true);
                return;
            }
            binding->actions.push_back(talas::project::ProjectGeneratorScoreClipAction{
                primary, definition, duration, 0, 0, 0, 1});
        }
        else if (kind == 3)
        {
            const auto lane =
                std::ranges::find(bridge_.editor().document().timeline.score.automation, primary,
                                  &talas::project::ScoreAutomationLane::laneId);
            if (lane == bridge_.editor().document().timeline.score.automation.end())
            {
                setStatus("Select one available Score automation lane", true);
                return;
            }
            binding->actions.push_back(talas::project::ProjectGeneratorScoreAutomationAction{
                primary,
                lane->domain,
                0.0,
                talas::core::AutomationInterpolation::Step,
            });
        }
        else if (kind == 4)
        {
            binding->actions.push_back(talas::project::ProjectGeneratorScoreMarkerAction{
                "Generated " + terminal,
                std::nullopt,
            });
        }
        else if (kind == 5)
        {
            if (!talas::project::validateProjectGeneratorGraphNodeType(
                    primary,
                    registry_).ok())
            {
                setStatus("Select one approved graph node type", true);
                return;
            }
            binding->actions.push_back(
                talas::project::ProjectGeneratorGraphNodeAction{primary});
        }
        else
        {
            setStatus("Select one closed output action kind", true);
            return;
        }
        const auto terminals = terminalLines(terminals_.getText().toStdString());
        if (const auto error =
                talas::project::projectGeneratorOutputValidationError(candidate, terminals))
        {
            setStatus(juceString(error->message), true);
            return;
        }
        workingOutput_ = std::move(candidate);
        refreshActionList();
        setStatus("Output binding prepared; Save to persist it", false);
    }

    void removeOutputAction()
    {
        const auto selected = actionList_.getSelectedItemIndex();
        if (selected < 0 || static_cast<std::size_t>(selected) >= actionRows_.size())
            return;
        const auto [bindingIndex, actionIndex] = actionRows_[static_cast<std::size_t>(selected)];
        auto &actions = workingOutput_.bindings[bindingIndex].actions;
        actions.erase(actions.begin() + static_cast<std::ptrdiff_t>(actionIndex));
        if (actions.empty())
        {
            workingOutput_.bindings.erase(workingOutput_.bindings.begin() +
                                          static_cast<std::ptrdiff_t>(bindingIndex));
        }
        refreshActionList();
        setStatus("Output binding removed; Save to persist it", false);
    }

    void timerCallback() override
    {
        try
        {
            if (previewCompletion_ != nullptr
                && previewCompletion_->ready.load(
                    std::memory_order_acquire))
            {
                auto completion = std::move(previewCompletion_);
                finishPreview(
                    completion->workerGeneration,
                    std::move(completion->request),
                    std::move(completion->candidate));
            }
            if (projectionCompletion_ != nullptr
                && projectionCompletion_->ready.load(
                    std::memory_order_acquire))
            {
                auto completion = std::move(projectionCompletion_);
                static_cast<void>(finishProjection(*completion));
            }
        }
        catch (...)
        {
            stopPreview();
            stopProjection();
        }
    }

    [[nodiscard]] std::optional<talas::project::ProjectGeneratorProgram>
    editorProgram(juce::String& error) const
    {
        talas::project::ProjectGeneratorProgram program;
        std::string terminalSource;
        if (!boundedUtf8(
                generatorId_.getText(),
                talas::generative::maximumContextFreeSymbolBytes,
                program.generatorId)
            || !boundedUtf8(
                displayName_.getText(),
                talas::generative::maximumContextFreeDisplayNameBytes,
                program.displayName)
            || !boundedUtf8(
                terminals_.getText(),
                maximumTerminalEditorBytes,
                terminalSource)
            || !boundedUtf8(
                grammar_.getText(),
                talas::generative::maximumContextFreeSourceBytes,
                program.source.grammar))
        {
            error = "Generator text exceeds its UTF-8 byte limit";
            return std::nullopt;
        }
        program.source.terminals = terminalLines(terminalSource);
        if (!exactUnsigned(
                seed_.getText(),
                std::numeric_limits<std::uint64_t>::max(),
                program.source.seed))
        {
            error = "Seed must be an exact unsigned 64-bit integer";
            return std::nullopt;
        }
        if (!exactUnsigned(
                rewritePasses_.getText(),
                static_cast<std::uint16_t>(
                    talas::generative::maximumContextFreeRewritePasses),
                program.source.maximumRewritePasses))
        {
            error = "Rewrite limit must be an exact integer from 0 to 128";
            return std::nullopt;
        }
        if (!exactUnsigned(
                outputSymbols_.getText(),
                static_cast<std::uint16_t>(
                    talas::generative::maximumContextFreeOutputSymbols),
                program.source.maximumOutputSymbols)
            || program.source.maximumOutputSymbols == 0)
        {
            error = "Output limit must be an exact integer from 1 to 4096";
            return std::nullopt;
        }
        program.output = workingOutput_;
        if (!exactUnsigned(timelineGrid_.getText(),
                           talas::project::maximumProjectGeneratorTimelineMicroseconds,
                           program.output.timelineStepMicroseconds) ||
            program.output.timelineStepMicroseconds == 0)
        {
            error = "Timeline grid must be an exact positive microsecond value "
                    "within 24 hours";
            return std::nullopt;
        }
        if (!exactUnsigned(scoreGrid_.getText(), std::numeric_limits<std::uint64_t>::max(),
                           program.output.scoreStepTicks) ||
            program.output.scoreStepTicks == 0)
        {
            error = "Score grid must be an exact positive musical-tick value";
            return std::nullopt;
        }
        talas::project::ProjectGeneratorLibrary library;
        library.programs.push_back(program);
        const auto validation =
            talas::project::validateProjectGeneratorLibrary(library);
        if (!validation.ok())
        {
            error = validation.diagnostics.empty()
                ? "Generator diagnostics exceeded the fixed limit"
                : juceString(
                    validation.diagnostics.front().path + " bytes "
                    + std::to_string(
                        validation.diagnostics.front().sourceRange.begin)
                    + "-"
                    + std::to_string(
                        validation.diagnostics.front().sourceRange.end)
                    + ": "
                    + validation.diagnostics.front().message);
            return std::nullopt;
        }
        return program;
    }

    void load(const talas::project::ProjectGeneratorProgram& program)
    {
        selectedGeneratorId_ = program.generatorId;
        selectedDocumentAuthorityGeneration_ =
            bridge_.editor().documentAuthorityGeneration();
        generatorId_.setText(juceString(program.generatorId), false);
        displayName_.setText(juceString(program.displayName), false);
        terminals_.setText(terminalText(program.source.terminals), false);
        grammar_.setText(juceString(program.source.grammar), false);
        seed_.setText(juceString(std::to_string(program.source.seed)), false);
        rewritePasses_.setText(
            juceString(std::to_string(program.source.maximumRewritePasses)),
            false);
        outputSymbols_.setText(
            juceString(std::to_string(program.source.maximumOutputSymbols)),
            false);
        workingOutput_ = program.output;
        timelineGrid_.setText(juceString(std::to_string(workingOutput_.timelineStepMicroseconds)),
                              false);
        scoreGrid_.setText(juceString(std::to_string(workingOutput_.scoreStepTicks)), false);
        timelineBase_.setText("0", false);
        scoreBase_.setText("0", false);
        outputNamespace_.setText(juceString(program.generatorId), false);
        soundDefinitionId_.setText(
            juceString(program.generatorId + "Sound"), false);
        soundDisplayName_.setText(
            juceString(program.displayName + " Sound"), false);
        refreshTerminalPicker();
        refreshTargetPickers();
        refreshActionList();
        updateComponentIds(program.generatorId);
        summary_.setText(
            juceString(std::to_string(program.source.terminals.size()))
                + " terminals",
            juce::dontSendNotification);
    }

    void beginNew()
    {
        selectedGeneratorId_.clear();
        selectedDocumentAuthorityGeneration_ = 0;
        programBox_.setSelectedItemIndex(-1, juce::dontSendNotification);
        generatorId_.clear();
        displayName_.clear();
        terminals_.clear();
        grammar_.clear();
        seed_.setText("0", false);
        rewritePasses_.setText("16", false);
        outputSymbols_.setText("256", false);
        workingOutput_ = {};
        timelineGrid_.setText(juceString(std::to_string(workingOutput_.timelineStepMicroseconds)),
                              false);
        scoreGrid_.setText(juceString(std::to_string(workingOutput_.scoreStepTicks)), false);
        timelineBase_.setText("0", false);
        scoreBase_.setText("0", false);
        outputNamespace_.setText("generated", false);
        soundDefinitionId_.setText("generatedSound", false);
        soundDisplayName_.setText("Generated Sound", false);
        refreshTerminalPicker();
        refreshTargetPickers();
        refreshActionList();
        updateComponentIds("new");
        summary_.setText("New", juce::dontSendNotification);
        status_.setText(
            "Define terminals and grammar, then Validate or Save.",
            juce::dontSendNotification);
        updateActionState();
    }

    void selectFromBox()
    {
        const auto index = programBox_.getSelectedItemIndex();
        const auto& programs =
            bridge_.editor().document().generatorLibrary.programs;
        if (index < 0 || static_cast<std::size_t>(index) >= programs.size())
        {
            beginNew();
            return;
        }
        load(programs[static_cast<std::size_t>(index)]);
        updateActionState();
    }

    void save()
    {
        juce::String error;
        auto program = editorProgram(error);
        if (!program.has_value())
        {
            setStatus(error, true);
            return;
        }
        const auto newId = program->generatorId;
        const auto result = selectedGeneratorId_.empty()
            ? bridge_.editor().upsertProjectGenerator(std::move(*program))
            : bridge_.editor().replaceProjectGenerator(
                  selectedGeneratorId_,
                  selectedDocumentAuthorityGeneration_,
                  std::move(*program));
        if (!result.ok())
        {
            setStatus(
                result.message.empty()
                    ? juce::String{"Generator edit rejected"}
                    : juceString(result.message),
                true);
            return;
        }
        selectedGeneratorId_ = newId;
        onDocumentChange_();
        refresh();
        setStatus("Generator saved as one Undo step", false);
    }

    void validateOnly()
    {
        juce::String error;
        if (!editorProgram(error).has_value())
        {
            setStatus(error, true);
            return;
        }
        setStatus("Generator is valid within every fixed limit", false);
    }

    void confirmDelete()
    {
        if (selectedGeneratorId_.empty())
            return;
        const auto id = selectedGeneratorId_;
        deleteConfirmationId_ = id;
        deleteConfirmationWindow_ = std::make_unique<juce::AlertWindow>(
            "Delete generator?",
            "Delete '" + juceString(id)
                +
                "' as one Undo step? Generated previews are recomputed and are "
                "never project state.",
            juce::AlertWindow::WarningIcon);
        deleteConfirmationWindow_->addButton("Delete", 1);
        deleteConfirmationWindow_->addButton("Cancel", 0);
        const auto safe = juce::Component::SafePointer<GeneratorPanelImpl>(this);
        auto* window = deleteConfirmationWindow_.get();
        window->getButton("Delete")->onClick = [safe, id, window] {
            if (safe != nullptr)
                safe->completeDeleteConfirmation(id, 1);
            window->exitModalState(1);
            window->setVisible(false);
        };
        window->getButton("Cancel")->onClick = [safe, id, window] {
            if (safe != nullptr)
                safe->completeDeleteConfirmation(id, 0);
            window->exitModalState(0);
            window->setVisible(false);
        };
        window->centreAroundComponent(this, 470, 170);
        window->enterModalState(true, nullptr, false);
    }

    void completeDeleteConfirmation(
        std::string_view id,
        int result)
    {
        if (deleteConfirmationId_ != id)
            return;
        deleteConfirmationId_.clear();
        if (result != 0)
            removeExact(id);
    }

    void removeExact(std::string_view id)
    {
        const auto result = bridge_.editor().removeProjectGenerator(id);
        if (!result.ok())
        {
            setStatus(juceString(result.message), true);
            return;
        }
        selectedGeneratorId_.clear();
        onDocumentChange_();
        refresh();
        setStatus("Generator deleted; Undo restores exact source", false);
    }

    void startPreview(bool asynchronous = true)
    {
        if (selectedGeneratorId_.empty())
        {
            setStatus("Save the generator before previewing", true);
            return;
        }
        if (!editorMatchesPersistedProgram())
        {
            setStatus(
                "Save the exact editor source before generating a preview",
                true);
            updateActionState();
            return;
        }
        stopPreview();
        auto library = bridge_.editor().document().generatorLibrary;
        const auto generatorId = selectedGeneratorId_;
        const auto authority =
            bridge_.editor().documentAuthorityGeneration();
        std::shared_ptr<const std::string> bytes;
        try
        {
            bytes = std::make_shared<const std::string>(
                talas::project::serializeProjectDocument(
                    bridge_.editor().document()));
        }
        catch (const std::exception& error)
        {
            setStatus(juceString(error.what()), true);
            return;
        }
        generateButton_.setEnabled(false);
        setStatus("Generating bounded symbol preview...", false);
        if (!asynchronous)
        {
            const auto preparedResult =
                talas::project::prepareProjectGenerators(library);
            if (!preparedResult.ok())
            {
                setStatus("Current generator library cannot be prepared", true);
                updateActionState();
                return;
            }
            auto prepared = std::make_shared<
                const talas::project::PreparedProjectGeneratorSet>(
                preparedResult.prepared);
            auto request = coordinator_.begin(
                prepared, generatorId, authority, bytes);
            if (!request.has_value())
            {
                setStatus("Generator preview authority is unavailable", true);
                updateActionState();
                return;
            }
            auto candidate = coordinator_.execute(*request);
            finishPreviewCandidate(
                std::move(*request), std::move(candidate));
            updateActionState();
            return;
        }
        if (nextPreviewWorkerGeneration_ == 0
            || nextPreviewWorkerGeneration_
                == std::numeric_limits<std::uint64_t>::max())
        {
            setStatus("Generator worker authority is exhausted", true);
            updateActionState();
            return;
        }
        const auto workerGeneration = nextPreviewWorkerGeneration_++;
        activePreviewWorkerGeneration_ = workerGeneration;
        try
        {
            auto completion = std::make_shared<PreviewCompletion>();
            completion->workerGeneration = workerGeneration;
            previewCompletion_ = completion;
            previewWorker_.emplace(
                [this,
                 library = std::move(library),
                 generatorId,
                 authority,
                 completion = std::move(completion),
                 bytes = std::move(bytes)](
                    std::stop_token stopToken) mutable {
                std::optional<talas::app::GeneratorPreviewRequest> request;
                talas::app::GeneratorPreviewCandidate candidate;
                try
                {
                    auto preparedResult =
                        talas::project::prepareProjectGenerators(
                            library, stopToken);
                    if (!preparedResult.ok())
                    {
                        if (preparedResult.cancelled)
                        {
                            candidate.status = talas::generative::
                                ContextFreeGenerateStatus::Cancelled;
                            candidate.message = "Generator preparation cancelled";
                        }
                        else
                        {
                            candidate.message =
                                "Current generator library cannot be prepared";
                        }
                    }
                    else
                    {
                        auto prepared = std::make_shared<
                            const talas::project::PreparedProjectGeneratorSet>(
                            std::move(preparedResult.prepared));
                        request = coordinator_.begin(
                            std::move(prepared),
                            generatorId,
                            authority,
                            std::move(bytes));
                        if (request.has_value())
                            candidate = coordinator_.execute(
                                *request, stopToken);
                        else
                            candidate.message =
                                "Generator preview authority is unavailable";
                    }
                }
                catch (const std::exception& error)
                {
                    try
                    {
                        candidate.message = std::string{
                            "Generator preview failed safely: "}
                            + error.what();
                    }
                    catch (...)
                    {
                    }
                }
                catch (...)
                {
                    try
                    {
                        candidate.message =
                            "Generator preview failed safely";
                    }
                    catch (...)
                    {
                    }
                }
                completion->request = std::move(request);
                completion->candidate = std::move(candidate);
                completion->ready.store(true, std::memory_order_release);
            });
        }
        catch (const std::exception& error)
        {
            activePreviewWorkerGeneration_ = 0;
            previewCompletion_.reset();
            setStatus(
                "Generator worker could not start: "
                    + juceString(error.what()),
                true);
            updateActionState();
        }
    }

    void finishPreview(
        std::uint64_t workerGeneration,
        std::optional<talas::app::GeneratorPreviewRequest> request,
        talas::app::GeneratorPreviewCandidate candidate)
    {
        if (workerGeneration != activePreviewWorkerGeneration_)
            return;
        activePreviewWorkerGeneration_ = 0;
        previewWorker_.reset();
        if (!request.has_value())
        {
            setStatus(
                candidate.message.empty()
                    ? juce::String{"Generator preview failed safely"}
                    : juceString(candidate.message),
                true);
        }
        else
        {
            finishPreviewCandidate(
                std::move(*request), std::move(candidate));
        }
        updateActionState();
    }

    void finishPreviewCandidate(
        talas::app::GeneratorPreviewRequest request,
        talas::app::GeneratorPreviewCandidate candidate)
    {
        if (!candidate.ok())
        {
            setStatus(
                candidate.message.empty()
                    ? juce::String{"Generator preview failed"}
                    : juceString(candidate.message),
                true);
            return;
        }
        try
        {
            const auto currentBytes = talas::project::serializeProjectDocument(
                bridge_.editor().document());
            if (!coordinator_.publish(
                    request,
                    std::move(candidate),
                    bridge_.editor().documentAuthorityGeneration(),
                    currentBytes))
            {
                setStatus(
                    "Preview invalidated; last successful symbols retained",
                    true);
                return;
            }
        }
        catch (const std::exception& error)
        {
            setStatus(juceString(error.what()), true);
            return;
        }
        try
        {
            const auto snapshot = coordinator_.snapshot();
            juce::String output;
            for (std::size_t index = 0; index < snapshot.symbols.size(); ++index)
            {
                if (index != 0U)
                    output += " ";
                output += juceString(snapshot.symbols[index]);
            }
            preview_.setText(output, false);
            setStatus(
                "Generated "
                    + juceString(std::to_string(snapshot.symbols.size()))
                    + " symbols / preview only, no audio/events/Score",
                false);
        }
        catch (...)
        {
            try
            {
                setStatus(
                    "Preview published but display allocation failed safely",
                    true);
            }
            catch (...)
            {
            }
        }
    }

    void executeProjection(ProjectionCompletion &completion,
                           talas::project::ProjectGeneratorLibrary library,
                           talas::project::ProjectDocument document, std::string generatorId,
                           std::uint64_t timelineBase, std::uint64_t scoreBase,
                           std::string outputNamespace,
                           std::string soundDefinitionId,
                           std::string soundDisplayName,
                           std::stop_token stopToken = {})
    {
        try
        {
            auto prepared = talas::project::prepareProjectGenerators(library, stopToken);
            if (!prepared.ok())
            {
                completion.message = prepared.cancelled
                                         ? "Generator projection cancelled"
                                         : "Current generator library cannot be prepared";
                return;
            }
            const auto found =
                std::ranges::find(prepared.prepared.programs, generatorId,
                                  &talas::project::PreparedProjectGenerator::generatorId);
            if (found == prepared.prepared.programs.end())
            {
                completion.message = "Exact generator disappeared during preparation";
                return;
            }
            std::array<std::uint16_t, talas::generative::maximumContextFreeOutputSymbols> current{};
            std::array<std::uint16_t, talas::generative::maximumContextFreeOutputSymbols> next{};
            std::array<std::uint16_t, talas::generative::maximumContextFreeOutputSymbols> output{};
            talas::generative::ContextFreeGeneratorScratch scratch{current, next, output};
            if (completion.kind == ProjectionKind::Timeline)
            {
                completion.timeline = talas::project::generateProjectTimelineEvents(
                    found->program, found->output, scratch, {timelineBase}, stopToken);
                if (!completion.timeline.ok())
                    completion.message = completion.timeline.message;
            }
            else if (completion.kind == ProjectionKind::Score)
            {
                completion.score = talas::project::generateProjectScoreEdits(
                    found->program, found->output, scratch, document,
                    {scoreBase, std::move(outputNamespace)}, stopToken);
                if (!completion.score.ok())
                    completion.message = completion.score.message;
            }
            else
            {
                completion.sound =
                    talas::project::generateProjectSoundDefinition(
                        found->program,
                        found->output,
                        scratch,
                        document,
                        registry_,
                        {
                            std::move(soundDefinitionId),
                            std::move(soundDisplayName),
                            std::move(outputNamespace),
                        },
                        stopToken);
                if (!completion.sound.ok())
                    completion.message = completion.sound.message;
            }
        }
        catch (const std::bad_alloc&)
        {
            completion.workerFailure =
                ProjectionWorkerFailure::Allocation;
        }
        catch (...)
        {
            completion.workerFailure =
                ProjectionWorkerFailure::Exception;
        }
    }

    [[nodiscard]] bool projectionRequestInputs(
        ProjectionKind kind,
        std::uint64_t& timelineBase,
        std::uint64_t& scoreBase,
        std::string& outputNamespace,
        std::string* soundDefinitionId = nullptr,
        std::string* soundDisplayName = nullptr) const
    {
        if (kind == ProjectionKind::Timeline)
        {
            return exactUnsigned(
                timelineBase_.getText(),
                talas::project::maximumProjectGeneratorTimelineMicroseconds,
                timelineBase);
        }
        if (!boundedUtf8(
                outputNamespace_.getText(),
                talas::project::maximumProjectGeneratorNamespaceBytes,
                outputNamespace)
            || outputNamespace.empty()
            || !talas::project::isValidScoreIdentifier(outputNamespace))
        {
            return false;
        }
        if (kind == ProjectionKind::Score)
        {
            return exactUnsigned(
                scoreBase_.getText(),
                std::numeric_limits<std::uint64_t>::max(),
                scoreBase);
        }
        if (soundDefinitionId == nullptr
            || soundDisplayName == nullptr
            || !boundedUtf8(
                soundDefinitionId_.getText(),
                talas::project::maximumProjectGeneratorSoundDefinitionIdBytes,
                *soundDefinitionId)
            || !talas::project::isValidScoreIdentifier(*soundDefinitionId)
            || !boundedUtf8(
                soundDisplayName_.getText(),
                talas::project::maximumProjectGeneratorSoundDisplayNameBytes,
                *soundDisplayName)
            || soundDisplayName->empty()
            || std::ranges::any_of(
                *soundDisplayName,
                [](unsigned char character) {
                    return character < 0x20U || character == 0x7fU;
                }))
        {
            return false;
        }
        return std::ranges::none_of(
            bridge_.editor().document().soundDefinitions,
            [&](const auto& definition) {
                return definition.definitionId == *soundDefinitionId;
            });
    }

    bool startProjection(ProjectionKind kind, bool asynchronous = true)
    {
        if (!editorMatchesPersistedProgram())
        {
            setStatus("Save the exact generator and output bindings before generation", true);
            return false;
        }
        const auto &document = bridge_.editor().document();
        const auto targets = talas::project::validateProjectGeneratorOutputTargets(
            document,
            registry_);
        if (!targets.ok())
        {
            setStatus(targets.diagnostics.empty()
                          ? juce::String{"Generator target diagnostics exceeded the "
                                         "fixed limit"}
                          : juceString(targets.diagnostics.front().path + ": " +
                                       targets.diagnostics.front().message),
                      true);
            return false;
        }
        const auto program =
            std::ranges::find(document.generatorLibrary.programs, selectedGeneratorId_,
                              &talas::project::ProjectGeneratorProgram::generatorId);
        if (program == document.generatorLibrary.programs.end())
        {
            setStatus("Exact generator is unavailable", true);
            return false;
        }
        const auto hasRequestedAction = std::ranges::any_of(
            program->output.bindings,
            [kind](const auto &binding)
            {
                return std::ranges::any_of(
                    binding.actions,
                    [kind](const auto &action)
                    {
                        if (kind == ProjectionKind::Timeline)
                        {
                            return std::holds_alternative<
                                talas::project::ProjectGeneratorTimelineEventAction>(action);
                        }
                        if (kind == ProjectionKind::Sound)
                        {
                            return std::holds_alternative<
                                talas::project::ProjectGeneratorGraphNodeAction>(action);
                        }
                        return std::holds_alternative<
                                talas::project::ProjectGeneratorScoreClipAction>(action)
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorScoreAutomationAction>(action)
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorScoreMarkerAction>(action);
                    });
            });
        if (!hasRequestedAction)
        {
            setStatus(kind == ProjectionKind::Timeline
                          ? "This generator has no Timeline output actions"
                          : kind == ProjectionKind::Score
                          ? "This generator has no Score output actions"
                          : "This generator has no restricted graph-node output actions",
                      true);
            return false;
        }
        std::uint64_t timelineBase = 0;
        std::uint64_t scoreBase = 0;
        std::string outputNamespace;
        std::string soundDefinitionId;
        std::string soundDisplayName;
        if (!projectionRequestInputs(
                kind,
                timelineBase,
                scoreBase,
                outputNamespace,
                &soundDefinitionId,
                &soundDisplayName))
        {
            setStatus(
                kind == ProjectionKind::Timeline
                    ? "Timeline base must be exact and bounded"
                    : kind == ProjectionKind::Score
                    ? "Score base and namespace must be exact and bounded"
                    : "Sound ID, name, and output namespace must be exact and bounded",
                true);
            return false;
        }
        stopProjection();
        stopPreview();
        ProjectionCompletion initial;
        initial.kind = kind;
        initial.authority.documentGeneration = bridge_.editor().documentAuthorityGeneration();
        try
        {
            initial.authority.canonicalDocumentBytes =
                talas::project::serializeProjectDocument(document);
        }
        catch (const std::exception &error)
        {
            setStatus(juceString(error.what()), true);
            return false;
        }
        if (!asynchronous)
        {
            executeProjection(initial, document.generatorLibrary, document, selectedGeneratorId_,
                              timelineBase, scoreBase, std::move(outputNamespace),
                              std::move(soundDefinitionId),
                              std::move(soundDisplayName));
            return finishProjection(initial);
        }
        if (nextProjectionWorkerGeneration_ == 0 ||
            nextProjectionWorkerGeneration_ == std::numeric_limits<std::uint64_t>::max())
        {
            setStatus("Generator projection worker authority is exhausted", true);
            return false;
        }
        const auto workerGeneration = nextProjectionWorkerGeneration_++;
        activeProjectionWorkerGeneration_ = workerGeneration;
        try
        {
            auto completion = std::make_shared<ProjectionCompletion>();
            completion->workerGeneration = workerGeneration;
            completion->kind = kind;
            completion->authority = std::move(initial.authority);
            projectionCompletion_ = completion;
            projectionWorker_.emplace(
                [this, completion = std::move(completion), library = document.generatorLibrary,
                 source = document, generatorId = selectedGeneratorId_, timelineBase, scoreBase,
                 outputNamespace = std::move(outputNamespace),
                 soundDefinitionId = std::move(soundDefinitionId),
                 soundDisplayName = std::move(soundDisplayName)](std::stop_token stopToken) mutable
                {
                    executeProjection(*completion, std::move(library), std::move(source),
                                      std::move(generatorId), timelineBase, scoreBase,
                                      std::move(outputNamespace),
                                      std::move(soundDefinitionId),
                                      std::move(soundDisplayName),
                                      stopToken);
                    completion->ready.store(true, std::memory_order_release);
                });
            setStatus(kind == ProjectionKind::Timeline
                          ? "Building bounded Timeline candidate..."
                          : kind == ProjectionKind::Score
                          ? "Building bounded Score candidate..."
                          : "Building bounded reusable-Sound chain...",
                      false);
            updateActionState();
            return true;
        }
        catch (const std::exception &error)
        {
            activeProjectionWorkerGeneration_ = 0;
            projectionCompletion_.reset();
            setStatus("Generator projection worker could not start: " + juceString(error.what()),
                      true);
            updateActionState();
            return false;
        }
    }

    bool finishProjection(ProjectionCompletion &completion)
    {
        if (completion.workerGeneration != 0 &&
            completion.workerGeneration != activeProjectionWorkerGeneration_)
            return false;
        activeProjectionWorkerGeneration_ = 0;
        projectionWorker_.reset();
        if (completion.workerFailure
            != ProjectionWorkerFailure::None)
        {
            setStatus(
                completion.workerFailure
                        == ProjectionWorkerFailure::Allocation
                    ? "Generator projection allocation failed safely"
                    : "Generator projection failed safely",
                true);
            updateActionState();
            return false;
        }
        talas::project::EditResult applied;
        std::optional<std::pair<std::string, std::string>>
            appliedSoundIdentity;
        if (completion.kind == ProjectionKind::Timeline)
        {
            if (!completion.timeline.ok())
            {
                setStatus(completion.message.empty() ? juce::String{"Timeline generation failed"}
                                                     : juceString(completion.message),
                          true);
                updateActionState();
                return false;
            }
            const auto count = completion.timeline.events.size();
            applied = bridge_.editor().applyGeneratedTimelineEvents(
                std::move(completion.timeline.events), completion.authority);
            if (applied.ok())
            {
                preview_.setText(
                    "Timeline candidate applied: " + juceString(std::to_string(count)) + " events",
                    false);
            }
        }
        else if (completion.kind == ProjectionKind::Score)
        {
            if (!completion.score.ok())
            {
                setStatus(completion.message.empty() ? juce::String{"Score generation failed"}
                                                     : juceString(completion.message),
                          true);
                updateActionState();
                return false;
            }
            const auto count = completion.score.edits.clips.size() +
                               completion.score.edits.markers.size() +
                               completion.score.edits.automation.size();
            applied = bridge_.editor().applyGeneratedScoreEdits(std::move(completion.score.edits),
                                                                completion.authority);
            if (applied.ok())
            {
                preview_.setText("Score candidate applied: " + juceString(std::to_string(count)) +
                                     " entities/lanes",
                                 false);
            }
        }
        else
        {
            if (!completion.sound.ok())
            {
                setStatus(
                    completion.message.empty()
                        ? juce::String{"Sound generation failed"}
                        : juceString(completion.message),
                    true);
                updateActionState();
                return false;
            }
            const auto nodeCount =
                completion.sound.definition->graph.nodes.size();
            const auto connectionCount =
                completion.sound.definition->graph.connections.size();
            appliedSoundIdentity = std::pair{
                completion.sound.definition->definitionId,
                completion.sound.definition->displayName};
            applied = bridge_.editor().applyGeneratedSoundDefinition(
                std::move(completion.sound),
                completion.authority);
            if (applied.ok())
            {
                preview_.setText(
                    "Sound candidate applied: "
                        + juceString(std::to_string(nodeCount))
                        + " nodes / "
                        + juceString(std::to_string(connectionCount))
                        + " connections",
                    false);
            }
        }
        if (!applied.ok())
        {
            setStatus(applied.message.empty()
                          ? juce::String{"Generated candidate rejected; last-good "
                                         "preview retained"}
                          : juceString(applied.message),
                      true);
            updateActionState();
            return false;
        }
        onDocumentChange_();
        refresh();
        if (appliedSoundIdentity.has_value())
        {
            soundDefinitionId_.setText(
                juceString(appliedSoundIdentity->first), false);
            soundDisplayName_.setText(
                juceString(appliedSoundIdentity->second), false);
            updateActionState();
        }
        setStatus(completion.kind == ProjectionKind::Timeline
                      ? "Generated Timeline applied as one Undo step"
                      : completion.kind == ProjectionKind::Score
                      ? "Generated Score applied as one Undo step"
                      : "Generated reusable Sound applied as one Undo step",
                  false);
        return true;
    }

    void stopProjection() noexcept
    {
        activeProjectionWorkerGeneration_ = 0;
        if (projectionWorker_.has_value())
        {
            projectionWorker_->request_stop();
            projectionWorker_.reset();
        }
        projectionCompletion_.reset();
    }

    void stopPreview() noexcept
    {
        coordinator_.invalidate();
        activePreviewWorkerGeneration_ = 0;
        if (previewWorker_.has_value())
        {
            previewWorker_->request_stop();
            previewWorker_.reset();
        }
        previewCompletion_.reset();
    }

    void updateComponentIds(std::string_view generatorId)
    {
        const auto prefix = "inspector.generator:"
            + std::string{generatorId} + ":";
        generatorId_.setComponentID(juceString(prefix + "id"));
        displayName_.setComponentID(juceString(prefix + "display-name"));
        terminals_.setComponentID(juceString(prefix + "terminals"));
        seed_.setComponentID(juceString(prefix + "seed"));
        rewritePasses_.setComponentID(juceString(prefix + "rewrite-passes"));
        outputSymbols_.setComponentID(juceString(prefix + "output-symbols"));
        grammar_.setComponentID(juceString(prefix + "grammar"));
        timelineGrid_.setComponentID(juceString(prefix + "timeline-grid"));
        scoreGrid_.setComponentID(juceString(prefix + "score-grid"));
        timelineBase_.setComponentID(juceString(prefix + "timeline-base"));
        scoreBase_.setComponentID(juceString(prefix + "score-base"));
        outputNamespace_.setComponentID(juceString(prefix + "namespace"));
        soundDefinitionId_.setComponentID(
            juceString(prefix + "sound-definition-id"));
        soundDisplayName_.setComponentID(
            juceString(prefix + "sound-display-name"));
        terminalPicker_.setComponentID(juceString(prefix + "terminal"));
        actionKindPicker_.setComponentID(juceString(prefix + "action-kind"));
        primaryTargetPicker_.setComponentID(juceString(prefix + "target"));
        secondaryTargetPicker_.setComponentID(juceString(prefix + "sound-target"));
        actionList_.setComponentID(juceString(prefix + "actions"));
        addActionButton_.setComponentID(juceString(prefix + "add-action"));
        removeActionButton_.setComponentID(juceString(prefix + "remove-action"));
        saveButton_.setComponentID(juceString(prefix + "save"));
        deleteButton_.setComponentID(juceString(prefix + "delete"));
        validateButton_.setComponentID(juceString(prefix + "validate"));
        generateButton_.setComponentID(juceString(prefix + "generate"));
        timelineButton_.setComponentID(juceString(prefix + "generate-timeline"));
        scoreButton_.setComponentID(juceString(prefix + "generate-score"));
        soundButton_.setComponentID(juceString(prefix + "generate-sound"));
        preview_.setComponentID(juceString(prefix + "preview"));
    }

    [[nodiscard]] bool editorMatchesPersistedProgram() const
    {
        if (selectedGeneratorId_.empty())
            return false;
        const auto& programs =
            bridge_.editor().document().generatorLibrary.programs;
        const auto found = std::ranges::find(
            programs,
            selectedGeneratorId_,
            &talas::project::ProjectGeneratorProgram::generatorId);
        if (found == programs.end())
            return false;
        return generatorId_.getText() == juceString(found->generatorId)
            && displayName_.getText() == juceString(found->displayName)
            && terminals_.getText() == terminalText(found->source.terminals)
            && grammar_.getText() == juceString(found->source.grammar)
            && seed_.getText()
                == juceString(std::to_string(found->source.seed))
            && rewritePasses_.getText()
                == juceString(std::to_string(
                    found->source.maximumRewritePasses))
            && outputSymbols_.getText()
                == juceString(std::to_string(
                    found->source.maximumOutputSymbols)) &&
               timelineGrid_.getText() ==
                   juceString(std::to_string(found->output.timelineStepMicroseconds)) &&
               scoreGrid_.getText() == juceString(std::to_string(found->output.scoreStepTicks)) &&
               workingOutput_.bindings == found->output.bindings;
    }

    void updateActionState()
    {
        const auto busy = previewWorker_.has_value() || projectionWorker_.has_value();
        const auto exactPersisted = editorMatchesPersistedProgram();
        deleteButton_.setEnabled(!selectedGeneratorId_.empty() && !busy);
        generateButton_.setEnabled(
            !selectedGeneratorId_.empty() && exactPersisted && !busy);
        saveButton_.setEnabled(!busy);
        validateButton_.setEnabled(!busy);
        removeActionButton_.setEnabled(!busy && actionList_.getSelectedItemIndex() >= 0);
        const auto kind = actionKindPicker_.getSelectedId();
        addActionButton_.setEnabled(!busy && !terminalPicker_.getText().isEmpty() && kind >= 1 &&
                                    kind <= 5 &&
                                    (kind == 4 || !primaryTargetPicker_.getText().isEmpty()) &&
                                    (kind != 2 || !secondaryTargetPicker_.getText().isEmpty()));
        auto hasTimeline = false;
        auto hasScore = false;
        auto hasSound = false;
        if (!selectedGeneratorId_.empty())
        {
            const auto found = std::ranges::find(
                bridge_.editor().document().generatorLibrary.programs, selectedGeneratorId_,
                &talas::project::ProjectGeneratorProgram::generatorId);
            if (found != bridge_.editor().document().generatorLibrary.programs.end())
            {
                for (const auto &binding : found->output.bindings)
                {
                    for (const auto &action : binding.actions)
                    {
                        hasTimeline = hasTimeline
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorTimelineEventAction>(action);
                        hasSound = hasSound
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorGraphNodeAction>(action);
                        hasScore = hasScore
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorScoreClipAction>(action)
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorScoreAutomationAction>(action)
                            || std::holds_alternative<
                                talas::project::ProjectGeneratorScoreMarkerAction>(action);
                    }
                }
            }
        }
        const auto targets =
            exactPersisted
                ? talas::project::validateProjectGeneratorOutputTargets(
                    bridge_.editor().document(), registry_)
                : talas::project::ProjectGeneratorTargetValidationResult{};
        const auto targetAuthority = exactPersisted && targets.ok();
        std::uint64_t timelineBase = 0;
        std::uint64_t scoreBase = 0;
        std::string outputNamespace;
        const auto timelineRequest = projectionRequestInputs(
            ProjectionKind::Timeline,
            timelineBase,
            scoreBase,
            outputNamespace);
        outputNamespace.clear();
        const auto scoreRequest = projectionRequestInputs(
            ProjectionKind::Score,
            timelineBase,
            scoreBase,
            outputNamespace);
        outputNamespace.clear();
        std::string soundDefinitionId;
        std::string soundDisplayName;
        const auto soundRequest = projectionRequestInputs(
            ProjectionKind::Sound,
            timelineBase,
            scoreBase,
            outputNamespace,
            &soundDefinitionId,
            &soundDisplayName);
        timelineButton_.setEnabled(
            !busy && targetAuthority && hasTimeline && timelineRequest);
        scoreButton_.setEnabled(
            !busy && targetAuthority && hasScore && scoreRequest);
        soundButton_.setEnabled(
            !busy && targetAuthority && hasSound && soundRequest);
        if (!selectedGeneratorId_.empty() && !exactPersisted)
            summary_.setText("Unsaved", juce::dontSendNotification);
        else if (!selectedGeneratorId_.empty())
        {
            const auto found = std::ranges::find(
                bridge_.editor().document().generatorLibrary.programs,
                selectedGeneratorId_,
                &talas::project::ProjectGeneratorProgram::generatorId);
            if (found
                != bridge_.editor().document().generatorLibrary.programs.end())
            {
                summary_.setText(
                    juceString(std::to_string(found->source.terminals.size()))
                        + " terminals",
                    juce::dontSendNotification);
            }
        }
    }

    void setStatus(const juce::String& text, bool error)
    {
        status_.setColour(
            juce::Label::textColourId,
            error ? errorColour : secondaryTextColour);
        status_.setText(text, juce::dontSendNotification);
        if (onStatus_)
            onStatus_(text);
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    talas::app::GeneratorPreviewCoordinator coordinator_;
    std::optional<std::jthread> previewWorker_;
    std::shared_ptr<PreviewCompletion> previewCompletion_;
    std::optional<std::jthread> projectionWorker_;
    std::shared_ptr<ProjectionCompletion> projectionCompletion_;
    std::uint64_t nextPreviewWorkerGeneration_ = 1;
    std::uint64_t activePreviewWorkerGeneration_ = 0;
    std::uint64_t nextProjectionWorkerGeneration_ = 1;
    std::uint64_t activeProjectionWorkerGeneration_ = 0;
    std::uint64_t observedAuthority_ = 0;
    std::uint64_t selectedDocumentAuthorityGeneration_ = 0;
    bool hasRefreshed_ = false;
    std::string selectedGeneratorId_;
    std::string deleteConfirmationId_;
    std::unique_ptr<juce::AlertWindow> deleteConfirmationWindow_;
    talas::project::ProjectGeneratorOutputProfile workingOutput_;
    std::vector<std::pair<std::size_t, std::size_t>> actionRows_;
    juce::ComboBox programBox_;
    juce::TextButton newButton_;
    juce::TextEditor generatorId_;
    juce::TextEditor displayName_;
    juce::TextEditor terminals_;
    juce::TextEditor seed_;
    juce::TextEditor rewritePasses_;
    juce::TextEditor outputSymbols_;
    juce::Label seedLabel_;
    juce::Label rewritePassesLabel_;
    juce::Label outputSymbolsLabel_;
    juce::TextEditor grammar_;
    juce::Label timelineGridLabel_;
    juce::Label scoreGridLabel_;
    juce::Label timelineBaseLabel_;
    juce::Label scoreBaseLabel_;
    juce::TextEditor timelineGrid_;
    juce::TextEditor scoreGrid_;
    juce::TextEditor timelineBase_;
    juce::TextEditor scoreBase_;
    juce::Label namespaceLabel_;
    juce::TextEditor outputNamespace_;
    juce::Label soundIdLabel_;
    juce::Label soundNameLabel_;
    juce::TextEditor soundDefinitionId_;
    juce::TextEditor soundDisplayName_;
    juce::ComboBox terminalPicker_;
    juce::ComboBox actionKindPicker_;
    juce::ComboBox primaryTargetPicker_;
    juce::ComboBox secondaryTargetPicker_;
    juce::ComboBox actionList_;
    juce::TextButton addActionButton_;
    juce::TextButton removeActionButton_;
    juce::TextButton saveButton_;
    juce::TextButton deleteButton_;
    juce::TextButton validateButton_;
    juce::TextButton generateButton_;
    juce::TextButton timelineButton_;
    juce::TextButton scoreButton_;
    juce::TextButton soundButton_;
    juce::Label summary_;
    juce::Label status_;
    juce::TextEditor preview_;
};
} // namespace

std::unique_ptr<GeneratorPanel> makeGeneratorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    GeneratorPanel::DocumentChangeCallback onDocumentChange,
    GeneratorPanel::StatusCallback onStatus)
{
    return std::make_unique<GeneratorPanelImpl>(
        bridge,
        registry,
        std::move(onDocumentChange),
        std::move(onStatus));
}
} // namespace talas::app::workbench
