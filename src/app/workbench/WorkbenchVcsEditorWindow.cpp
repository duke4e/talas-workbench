#include "app/workbench/WorkbenchVcsEditorWindow.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/VcsLayout.h"
#include "talas/app/VirtualControlWidgetModel.h"

#include <utility>

namespace talas::app::workbench
{
// The editor's face: the Selected Value over a grid of keys.
class VcsEditorWindow::Content final : public juce::Component
{
public:
    explicit Content(VcsEditorActions& actions)
        : actions_(actions)
    {
        caption_.setText("Selected Value:", juce::dontSendNotification);
        caption_.setFont(WorkbenchType::label());
        caption_.setColour(juce::Label::textColourId, secondaryTextColour);
        value_.setFont(WorkbenchType::bodyStrong());
        value_.setColour(juce::Label::textColourId, textColour);
        value_.setColour(juce::Label::outlineColourId, workbenchTheme.outline);
        describeControl(value_, "Selected Value", "The selected widget; select one in the surface or from its list there.");
        addAndMakeVisible(caption_);
        addAndMakeVisible(value_);
        widget_.setTooltip("The selected widget's type, label, number, range, taper and place");
        widget_.onClick = [this] {
            if (actions_.editSelected)
                actions_.editSelected();
        };
        arrange_.onClick = [this] {
            if (actions_.arrange)
                actions_.arrange(arrange_);
        };
        options_.onClick = [this] {
            if (actions_.options)
                actions_.options(options_);
        };
        layout_.onClick = [this] {
            if (actions_.layout)
                actions_.layout(layout_);
        };
        lock_.setTooltip("Lock the surface, which closes the editor");
        lock_.onClick = [this] {
            const auto lock = actions_.lock;
            juce::MessageManager::callAsync([lock] {
                if (lock)
                    lock();
            });
        };
        for (auto* button : {&widget_, &arrange_, &options_, &layout_, &lock_})
            addAndMakeVisible(*button);
        setSize(workbenchScaledUiPixels(232), workbenchScaledUiPixels(150));
        refresh();
    }

    void refresh()
    {
        value_.setText(actions_.selectedValue ? actions_.selectedValue() : juce::String{}, juce::dontSendNotification);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        const auto gap = workbenchScaledUiPixels(4);
        auto bounds = getLocalBounds().reduced(workbenchScaledUiPixels(8));
        caption_.setBounds(bounds.removeFromTop(workbenchScaledUiPixels(16)));
        value_.setBounds(bounds.removeFromTop(workbenchScaledUiPixels(22)));
        bounds.removeFromTop(gap);
        const auto rowHeight = (bounds.getHeight() - 2 * gap) / 3;
        const auto pair = [&](juce::Component& left, juce::Component& right) {
            auto row = bounds.removeFromTop(rowHeight);
            left.setBounds(row.removeFromLeft((row.getWidth() - gap) / 2));
            row.removeFromLeft(gap);
            right.setBounds(row);
            bounds.removeFromTop(gap);
        };
        pair(widget_, arrange_);
        pair(options_, layout_);
        lock_.setBounds(bounds.removeFromTop(rowHeight));
    }

private:
    VcsEditorActions& actions_;
    juce::Label caption_;
    juce::Label value_;
    juce::TextButton widget_{"Widget..."};
    juce::TextButton arrange_{"Arrange..."};
    juce::TextButton options_{"Options..."};
    juce::TextButton layout_{"Layout..."};
    juce::TextButton lock_{"Lock"};
};

VcsEditorWindow::VcsEditorWindow(VcsEditorActions actions)
    : juce::DocumentWindow("VCS Editor", backgroundColour, juce::DocumentWindow::closeButton)
    , actions_(std::move(actions))
{
    setUsingNativeTitleBar(true);
    content_ = new Content(actions_);
    setContentOwned(content_, true);
    setResizable(false, false);
    // Floating over the Workbench, wherever it is put.
    setAlwaysOnTop(true);
}

VcsEditorWindow::~VcsEditorWindow()
{
    content_ = nullptr;
    clearContentComponent();
}

void VcsEditorWindow::refresh()
{
    if (content_ != nullptr)
        content_->refresh();
}

// Closing the editor locks the surface (which may first ask to save its
// changes, and stay unlocked, the editor open, on Cancel).
void VcsEditorWindow::closeButtonPressed()
{
    const auto lock = actions_.lock;
    juce::MessageManager::callAsync([lock] {
        if (lock)
            lock();
    });
}

void VcsEditorWindow::moved()
{
    juce::DocumentWindow::moved();
    if (actions_.moved && isVisible())
        actions_.moved({getX(), getY(), getWidth(), getHeight()});
}

std::unique_ptr<VcsEditorWindow> openVcsEditorWindow(
    juce::Component& surface, VcsEditorActions actions, const std::optional<talas::app::VcsRect>& last)
{
    if (!surface.isShowing())
        return nullptr;
    auto window = std::make_unique<VcsEditorWindow>(std::move(actions));
    const auto surfaceBounds = surface.getScreenBounds();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(surfaceBounds);
    const auto area = display != nullptr ? display->userBounds.toNearestInt() : surfaceBounds;
    const auto place = talas::app::vcsEditorWindowPlace({surfaceBounds.getX(), surfaceBounds.getY(), surfaceBounds.getWidth(),
                                                         surfaceBounds.getHeight()},
        {area.getX(), area.getY(), area.getWidth(), area.getHeight()}, window->getWidth(), window->getHeight(), last);
    window->setTopLeftPosition(place.left, place.top);
    window->setVisible(true);
    window->toFront(false);
    return window;
}

void showVcsWidgetDialog(juce::Component& owner, const talas::project::VirtualControl& control, VcsWidgetApply apply,
    std::function<void(const juce::String&)> status, std::function<void()> closed)
{
    const auto& widget = control.widget;
    auto* dialog = new juce::AlertWindow(
        "Widget: " + juceString(control.eventName), "Virtual Control Surface Editor", juce::MessageBoxIconType::NoIcon, &owner);
    juce::StringArray styles{"default", "fader", "horizontalFader", "smallFader", "potentiometer", "buttonFill"};
    dialog->addComboBox("style", styles, "Widget type");
    dialog->getComboBoxComponent("style")->setText(
        juceString(std::string{talas::app::virtualControlWidgetStyleName(widget.style)}), juce::dontSendNotification);
    dialog->addComboBox("number", {"Number", "No Nbr"}, "Show Nbr");
    dialog->getComboBoxComponent("number")->setSelectedItemIndex(widget.showNumber ? 0 : 1);
    dialog->addTextEditor("label", juceString(widget.label), "Widget Label");
    dialog->addTextEditor("on", juceString(talas::app::virtualControlColourText(widget.onColour)), "On Color (#rrggbb)");
    dialog->addTextEditor("off", juceString(talas::app::virtualControlColourText(widget.offColour)), "Off Color (#rrggbb)");
    dialog->addTextEditor("minimum", juce::String(control.minimum), "Min");
    dialog->addTextEditor("maximum", juce::String(control.maximum), "Max");
    dialog->addTextEditor("grid", juce::String(widget.grid), "Grid");
    dialog->addComboBox("taper", {"linear", "log"}, "Taper");
    dialog->getComboBoxComponent("taper")->setSelectedItemIndex(widget.taper == talas::project::VirtualControlTaper::Log ? 1 : 0);
    dialog->addTextEditor("tab", juceString(widget.tab), "Tab");
    // Left / Top / Wd / Ht, the widget's place to the pixel.
    if (widget.bounds.has_value())
        dialog->addTextEditor("place",
            juce::String(widget.bounds->left) + " " + juce::String(widget.bounds->top) + " "
                + juce::String(widget.bounds->width) + " " + juce::String(widget.bounds->height),
            "Left / Top / Wd / Ht");
    dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([dialog, name = control.eventName, bounds = widget.bounds, apply = std::move(apply),
                                                status = std::move(status), closed = std::move(closed)](int result) {
            talas::project::VirtualControlWidget edited;
            edited.bounds = bounds;
            if (bounds.has_value())
            {
                juce::StringArray numbers;
                numbers.addTokens(dialog->getTextEditorContents("place"), " ,", {});
                numbers.removeEmptyStrings();
                const auto place = numbers.size() == 4
                    ? talas::app::vcsRectFromSize(numbers[0].getIntValue(), numbers[1].getIntValue(), numbers[2].getIntValue(),
                          numbers[3].getIntValue())
                    : std::nullopt;
                if (place.has_value())
                    edited.bounds = *place;
            }
            const auto style = talas::app::parseVirtualControlWidgetStyle(dialog->getComboBoxComponent("style")->getText().toStdString());
            edited.style = style.value_or(talas::project::VirtualControlWidgetStyle::Default);
            edited.showNumber = dialog->getComboBoxComponent("number")->getSelectedItemIndex() != 1;
            edited.label = dialog->getTextEditorContents("label").toStdString();
            const auto on = talas::app::parseVirtualControlColour(dialog->getTextEditorContents("on").toStdString());
            const auto off = talas::app::parseVirtualControlColour(dialog->getTextEditorContents("off").toStdString());
            edited.grid = dialog->getTextEditorContents("grid").getDoubleValue();
            edited.taper = dialog->getComboBoxComponent("taper")->getSelectedItemIndex() == 1 ? talas::project::VirtualControlTaper::Log
                                                                                               : talas::project::VirtualControlTaper::Linear;
            edited.tab = dialog->getTextEditorContents("tab").toStdString();
            const auto minimum = dialog->getTextEditorContents("minimum").getDoubleValue();
            const auto maximum = dialog->getTextEditorContents("maximum").getDoubleValue();
            if (closed)
                closed();
            if (result != 1)
                return;
            if (!on.has_value() || !off.has_value())
            {
                if (status)
                    status("Colours are #rrggbb, or empty for the theme's");
                return;
            }
            edited.onColour = *on;
            edited.offColour = *off;
            if (apply)
                apply(name, edited, minimum, maximum);
        }),
        true);
}
}
