#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// Typing a hot value into a field
// completes the word.
[[nodiscard]] std::optional<std::string> WorkbenchComponent::runHotValueCompletionSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "the completion smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"out", "talas.audioOutput", {}}, {"osc", "talas.oscillator", {}}, {"lfo", "talas.oscillator", {}}};
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    // A pasted Sound is a Val expression.
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::reconcileProjectPresentation(document);
    if (!projectBridge_.editor().replaceDocument(document).ok())
        return finish("the completion smoke's project was rejected");
    documentChanged();
    selectNode("osc");
    inspectorPanel_->focusParameter("frequency");
    if (const auto typed = inspectorPanel_->typeIntoValueForSmoke("!", true);
        typed != std::pair<std::string, std::string>{"!Frequency", "Frequency"})
        return finish("a lone ! did not take the field's name: " + typed.first + " / " + typed.second);
    if (inspectorPanel_->typeIntoValueForSmoke("K") != std::pair<std::string, std::string>{"!KeyPitch", "eyPitch"})
        return finish("typing over the guess did not guess a common name");
    if (inspectorPanel_->typeIntoValueForSmoke("eyV") != std::pair<std::string, std::string>{"!KeyVelocity", "elocity"})
        return finish("!KeyV did not complete to !KeyVelocity");
    // The right arrow jumps to the end of the guess, and
    // what is typed then goes after it.
    if (inspectorPanel_->pressValueKeyForSmoke(juce::KeyPress{juce::KeyPress::rightKey})
            != std::pair<std::string, std::string>{"!KeyVelocity", ""}
        || inspectorPanel_->typeIntoValueForSmoke("2") != std::pair<std::string, std::string>{"!KeyVelocity2", ""})
        return finish("the right arrow did not take the guess and let typing go on after it");
    // The arrow keys place the insertion point, and
    // typing goes in there.
    inspectorPanel_->focusParameter("frequency");
    (void)inspectorPanel_->typeIntoValueForSmoke("0.5", true);
    (void)inspectorPanel_->pressValueKeyForSmoke(juce::KeyPress{juce::KeyPress::leftKey});
    (void)inspectorPanel_->pressValueKeyForSmoke(juce::KeyPress{juce::KeyPress::leftKey});
    if (inspectorPanel_->typeIntoValueForSmoke("1").first != "01.5")
        return finish("the left arrow did not move the insertion point back in the field");
    (void)inspectorPanel_->pressValueKeyForSmoke(juce::KeyPress{juce::KeyPress::rightKey});
    if (inspectorPanel_->typeIntoValueForSmoke("2").first != "01.25")
        return finish("the right arrow did not move the insertion point on in the field");
    // A double-click just inside punctuation
    // selects what it encloses, one after the contents all of them.
    if (inspectorPanel_->doubleClickValueForSmoke("default * (!KeyPitch + 12 nn)", 11) != "!KeyPitch + 12 nn")
        return finish("a double-click inside the parentheses did not select what they enclose");
    if (inspectorPanel_->doubleClickValueForSmoke("440 hz", 6) != "440 hz")
        return finish("a double-click after the contents did not select them all");
    // A Sound copied and pasted into the field
    // controls it; Paste special offers the texts copied.
    if (!copySelectedNodes({"lfo"}))
        return finish("the control Sound could not be copied");
    selectNode("osc");
    inspectorPanel_->focusParameter("frequency");
    if (inspectorPanel_->pasteIntoValueForSmoke("440 hz") != "lfo L")
        return finish("pasting the Sound into the field did not name it");
    const auto& nodes = projectBridge_.editor().document().graph.nodes;
    const auto osc = std::ranges::find(nodes, std::string_view{"osc"}, &talas::project::ProjectNode::instanceId);
    const auto frequency = osc == nodes.end() ? nullptr : findParameter(*osc, "frequency");
    if (frequency == nullptr || frequency->expression != std::optional<std::string>{"lfo L"})
        return finish("the pasted Sound did not become the field's value");
    if (inspectorPanel_->pasteSpecialForSmoke("!KeyPitch", "2 * ") != "2 * !KeyPitch")
        return finish("Paste special did not offer the text copied");
    // An illegal value is refused with a flash and
    // two beeps, the field's value kept; a legal one is taken.
    inspectorPanel_->focusParameter("frequency");
    const auto signalsBefore = inspectorPanel_->commitValueForSmoke("330 hz");
    const auto signalsAfter = inspectorPanel_->commitValueForSmoke("440 +");
    const auto* kept = [&]() -> const talas::project::ProjectParameterExpression* {
        const auto& now = projectBridge_.editor().document().graph.nodes;
        const auto node = std::ranges::find(now, std::string_view{"osc"}, &talas::project::ProjectNode::instanceId);
        return node == now.end() ? nullptr : findParameter(*node, "frequency");
    }();
    if (signalsAfter != signalsBefore + 1 || kept == nullptr || kept->expression != std::optional<std::string>{"330 hz"})
        return finish("an illegal value was not refused with the field flashing, or a legal one was not taken");
    return finish();
}
}
