#include "talas/app/TextSearch.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Find and Find again search the field after the
// current selection and select the next occurrence; nothing found beeps.
void testFindSelectsTheNextOccurrence()
{
    const std::string text = "!Pitch + !Pitch smooth: !Rate";
    const auto first = talas::app::findNextInText(text, "!Pitch", 0);
    require(first == talas::app::TextMatch{0, 6}, "Find should select the first occurrence after the caret");
    const auto again = talas::app::findNextInText(text, "!Pitch", first->end);
    require(again == talas::app::TextMatch{9, 15}, "Find again should start after the current selection");
    require(!talas::app::findNextInText(text, "!Pitch", again->end).has_value()
            && !talas::app::findNextInText(text, "!Volume", 0).has_value() && !talas::app::findNextInText(text, "", 0).has_value(),
        "a text that is not there should not be found");
}

// Replace replaces the next occurrence and Replace
// all every one; a text not found replaces nothing.
void testReplaceAndReplaceAll()
{
    const std::string text = "!Pitch + !Pitch smooth: !Rate";
    const auto one = talas::app::replaceInText(text, "!Pitch", "!KeyPitch", 1, false);
    require(one.count == 1 && one.text == "!Pitch + !KeyPitch smooth: !Rate" && one.replaced == talas::app::TextMatch{9, 18},
        "Replace should replace the next occurrence after the selection");
    const auto all = talas::app::replaceInText(text, "!Pitch", "!KeyPitch", 0, true);
    require(all.count == 2 && all.text == "!KeyPitch + !KeyPitch smooth: !Rate",
        "Replace all should replace every occurrence");
    const auto none = talas::app::replaceInText(text, "!Volume", "!Gain", 0, true);
    require(none.count == 0 && none.text == text, "a text not found should replace nothing");
}
}

int main()
{
    try
    {
        testFindSelectsTheNextOccurrence();
        testReplaceAndReplaceAll();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "text search tests passed\n";
    return 0;
}
