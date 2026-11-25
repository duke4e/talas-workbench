#pragma once

#include "talas/app/ExpressionsLibraryModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
// The Expressions Library: commonly used
// expressions, found by words, each shown with its comment, to drag into a
// parameter field of the inspector.
class ExpressionsLibraryPanel : public juce::Component,
                                public juce::DragAndDropContainer
{
public:
    ~ExpressionsLibraryPanel() override = default;

    virtual void setQueryForSmoke(juce::String query) = 0;
    [[nodiscard]] virtual int visibleEntryCountForSmoke() const = 0;
    // What dragging the entry of that name carries; empty when not listed.
    [[nodiscard]] virtual juce::String dragDescriptionForSmoke(std::string_view name) const = 0;
};

// `ownFiles` gives the entries of one's own expressions files, read again
// as the library is searched.
[[nodiscard]] std::unique_ptr<ExpressionsLibraryPanel> makeExpressionsLibraryPanel(
    std::function<std::vector<talas::app::ExpressionLibraryEntry>()> ownFiles = {});
}
