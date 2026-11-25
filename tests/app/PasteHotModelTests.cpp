#include "talas/app/PasteHotModel.h"

#include <algorithm>
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

// Paste hot lists commonly used
// expressions first, half the sampling rate second, then every Event Value
// name in the global map; typing part of a name jumps to it.
void testPasteHotListsExpressionsThenTheGlobalMapsNames()
{
    talas::project::ProjectDocument document;
    document.globalMap = "!Delay is: `MIDIController20.\n!Cutoff is: `MIDIController21.\n";
    const auto entries = talas::app::pasteHotEntries(document);
    require(entries.size() > 2 && entries[1].label == "Half the sampling rate"
            && entries[1].text == "SignalProcessor sampleRate * 0.5",
        "the second item should be half the sampling rate");
    const auto cutoff = std::ranges::find(entries, std::string{"!Cutoff"}, &talas::app::PasteHotEntry::text);
    const auto delay = std::ranges::find(entries, std::string{"!Delay"}, &talas::app::PasteHotEntry::text);
    require(cutoff != entries.end() && delay != entries.end() && cutoff < delay && cutoff - entries.begin() > 1,
        "the global map's Event Value names should follow the expressions, in order");
    require(talas::app::pasteHotMatch(entries, "del") == static_cast<std::size_t>(delay - entries.begin())
            && talas::app::pasteHotMatch(entries, "!Del") == static_cast<std::size_t>(delay - entries.begin())
            && talas::app::pasteHotMatch(entries, "half") == std::optional<std::size_t>{1},
        "typing part of a name should jump to it");
    require(!talas::app::pasteHotMatch(entries, "zzz").has_value() && !talas::app::pasteHotMatch(entries, "").has_value(),
        "a name not in the list should not be chosen");
    // The inverses of a frequency and of a duration are among
    // the hot expressions, before the global map's names.
    const auto period = std::ranges::find(entries, std::string{"(440 hz inverse)"}, &talas::app::PasteHotEntry::text);
    const auto rate = std::ranges::find(entries, std::string{"(2 s inverse)"}, &talas::app::PasteHotEntry::text);
    require(period != entries.end() && rate != entries.end() && period < cutoff && rate < cutoff,
        "the inverse expressions should be in the list of hot expressions");
}
}

int main()
{
    try
    {
        testPasteHotListsExpressionsThenTheGlobalMapsNames();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "paste hot model tests passed\n";
    return 0;
}
