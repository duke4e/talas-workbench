#include "talas/app/ExpressionsLibraryModel.h"
#include "talas/app/ParameterFieldEditors.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/IntervalCollectArray.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectEditor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using talas::app::EnvelopeBreakpoint;

// An envelope breakpoint field shows a GraphicalEnvelope's
// breakpoints; shift-click adds one, Delete removes the selected one,
// dragging moves it, and the loop button marks loop points.
void testEnvelopeFieldEditing()
{
    const auto parsed = talas::app::parseEnvelopeField("{0 s} {0} {0.1 s} {1} {1 s} {0}");
    require(parsed.has_value()
            && parsed->points == std::vector<EnvelopeBreakpoint>{{0.0, 0.0}, {0.1, 1.0}, {1.0, 0.0}}
            && !parsed->loopStart.has_value(),
        "the default envelope should read as three breakpoints");
    require(talas::app::envelopeFieldText(*parsed) == "{0 s} {0} {0.1 s} {1} {1 s} {0}",
        "the envelope should write back the text it read");
    const auto looped = talas::app::parseEnvelopeField("{0 s} {0} {100 ms} {1} loopStart {0.5 s} {0.6} loopEnd {1 s} {0}");
    require(looped.has_value() && looped->loopStart == std::optional<std::size_t>{1}
            && looped->loopEnd == std::optional<std::size_t>{2} && looped->points[1].seconds == 0.1,
        "loop marks and milliseconds should be read");
    require(talas::app::envelopeFieldText(*looped)
            == "{0 s} {0} {0.1 s} {1} loopStart {0.5 s} {0.6} loopEnd {1 s} {0}",
        "the loop marks should be written after their breakpoints");
    const auto arrow = talas::app::envelopeLoopArrowColour();
    require((arrow >> 16 & 0xffu) > 0xc0 && (arrow >> 8 & 0xffu) < 0x60 && (arrow & 0xffu) < 0x60,
        "the loop should be marked by red arrows");
    for (const auto* text : {"{0 s} {!Level} {1 s} {0}", "{0 s} {0} {1 hz} {1}", "{1 s} {0} {0.5 s} {1}",
             "{0 s} {0}", "{0 s} {0} {1 s}", "{0 s} {0} loopEnd {1 s} {1} loopStart"})
    {
        require(!talas::app::parseEnvelopeField(text).has_value(),
            std::string{"a field the graphical editor cannot show should stay text: "} + text);
    }

    auto field = *parsed;
    const auto added = talas::app::addEnvelopeBreakpoint(field, 0.5, 1.5);
    require(added == 2 && field.points[2] == EnvelopeBreakpoint{0.5, 1.0},
        "shift-click should add a breakpoint in time order, its value within 0 to 1");
    talas::app::moveEnvelopeBreakpoint(field, 2, 2.0, -0.5);
    require(field.points[2] == EnvelopeBreakpoint{1.0, 0.0},
        "a dragged breakpoint should stay between its neighbours and within 0 to 1");
    talas::app::moveEnvelopeBreakpoint(field, 2, 0.3, 0.25);
    require(field.points[2] == EnvelopeBreakpoint{0.3, 0.25}, "a breakpoint should move where it is dragged");
    require(talas::app::toggleEnvelopeLoopStart(field, 1) && talas::app::toggleEnvelopeLoopEnd(field, 2)
            && talas::app::envelopeFieldText(field) == "{0 s} {0} {0.1 s} {1} loopStart {0.3 s} {0.25} loopEnd {1 s} {0}",
        "the loop button should mark the loop's start and end");
    require(!talas::app::toggleEnvelopeLoopEnd(field, 0) && !talas::app::toggleEnvelopeLoopStart(field, 3),
        "the loop's end should stay after its start");
    require(talas::app::removeEnvelopeBreakpoint(field, 1) && !field.loopStart.has_value()
            && field.loopEnd == std::optional<std::size_t>{1}
            && talas::app::envelopeFieldText(field) == "{0 s} {0} {0.3 s} {0.25} {1 s} {0}",
        "removing a marked breakpoint should drop its mark, and the text a loop with one mark");
    require(talas::app::removeEnvelopeBreakpoint(field, 0) && !talas::app::removeEnvelopeBreakpoint(field, 0),
        "an envelope should keep two breakpoints at least");
    require(talas::app::nearestEnvelopeBreakpoint(*parsed, 0.11, 0.95, 0.05, 0.1) == std::optional<std::size_t>{1}
            && !talas::app::nearestEnvelopeBreakpoint(*parsed, 0.5, 0.5, 0.05, 0.1).has_value(),
        "a click should select the breakpoint under it");
    // The grid's thin lines a tenth of a second apart and heavy
    // ones each second; at a Rate of 0.5 the heavy lines are two seconds
    // of playing apart.
    const auto grid = talas::app::envelopeGridLines(2.0, 1.0);
    require(grid.size() == 19 && std::abs(grid[0].seconds - 0.1) < 1.0e-12 && !grid[0].heavy && grid[9].seconds == 1.0
            && grid[9].heavy && grid[9].playedSeconds == 1.0,
        "the heavy lines indicate one second boundaries, the thin lines a tenth of a second apart");
    const auto slow = talas::app::envelopeGridLines(3.0, 0.5);
    require(slow.size() == 29 && slow[9].heavy && slow[9].playedSeconds == 2.0 && slow[19].heavy && slow[19].playedSeconds == 4.0,
        "at a Rate of 0.5 the heavy lines should be two seconds apart");
    require(talas::app::envelopeFieldRate(" 0.5 ") == 0.5 && talas::app::envelopeFieldRate("!Rate") == 1.0
            && talas::app::envelopeFieldRate("0") == 1.0,
        "a Rate that is not a positive plain number should label the grid as at 1");
}

// An array field lists values separated by spaces; numbers
// in curly braces are values too, and Event Values keep the field text.
void testArrayFieldEditing()
{
    const auto parsed = talas::app::parseArrayField("60 64 {67} 72 nn");
    require(parsed.has_value() && parsed->size() == 4 && (*parsed)[2].value == 67.0 && (*parsed)[3].unit == "nn",
        "an array of numbers should be read, braces and units too");
    auto elements = *parsed;
    elements[1].value = 65.0;
    require(talas::app::arrayFieldText(elements) == "60 65 67 72 nn", "the array should write back its values");
    require(!talas::app::parseArrayField("1 !Fader1 {2 + 3}").has_value() && !talas::app::parseArrayField("").has_value(),
        "an array with Event Values or calculations should stay text");
    const auto range = talas::app::arrayFieldRange(*parsed);
    require(range.minimum == 0.0 && range.maximum == 72.0, "the bars should span 0 to the largest value");
}

// The Expressions Library's entries drop into
// parameter fields; each must be a valid Val expression.
// An expressions file of one's own begins with EXPR
// on a line by itself; each expression's name follows a double quote, its
// comment the lines after up to a closing quote, and the expression the
// lines after that.
void testExpressionsFilesOfOnesOwn()
{
    const auto parsed = talas::app::parseExpressionsFile(
        "EXPR\r\n\"Arpeggiate keyboard pitches\r\nUse in any Frequency field to arpeggiate\nthe last pitches played.\"\n"
        "(((1 bpm: !BPM) countTriggersMod: 5) of: #(60 62 64 65 67)) nn\n\n\"Half fader\"\n!Fader * 0.5\n\"No expression\nnone\"\n",
        "sequences");
    require(parsed.has_value() && parsed->size() == 2, "an EXPR file's expressions, one without an expression left out");
    const auto& first = parsed->front();
    require(first.category == "sequences" && first.name == "Arpeggiate keyboard pitches"
            && first.comment == "Use in any Frequency field to arpeggiate the last pitches played."
            && first.expression == "(((1 bpm: !BPM) countTriggersMod: 5) of: #(60 62 64 65 67)) nn",
        "the name to the end of its line, the comment to the closing quote, the expression after");
    require((*parsed)[1].name == "Half fader" && (*parsed)[1].comment.empty() && (*parsed)[1].expression == "!Fader * 0.5",
        "a name closed on its own line has no comment");
    require(!talas::app::parseExpressionsFile("\"Not one\nno\"\n!Fader\n", "x").has_value(),
        "a file not beginning with EXPR is not an expressions file");
    const auto found = talas::app::findExpressionLibraryEntries("arpeggiate pitches", *parsed);
    require(std::ranges::any_of(found, [](const auto& entry) { return entry.name == "Arpeggiate keyboard pitches"; }),
        "one's own expressions are searched with the library's");
}

// The library is kept as text files,
// among them those the tutorial opens by name, holding the expressions it
// names; each file reads back as written, each expression parses (an
// array's as a list field), and the library written into a folder leaves
// files already there as they are.
void testTheLibraryAsFiles()
{
    const auto files = talas::app::expressionsLibraryFiles();
    const auto entryIn = [&](std::string_view file, std::string_view name) -> const talas::app::ExpressionLibraryEntry* {
        const auto found = std::ranges::find(files, file, &talas::app::ExpressionsLibraryFile::name);
        if (found == files.end())
            return nullptr;
        const auto entry = std::ranges::find(found->entries, name, &talas::app::ExpressionLibraryEntry::name);
        return entry == found->entries.end() ? nullptr : &*entry;
    };
    for (const auto& [file, name] : std::vector<std::pair<std::string, std::string>>{
             {"keyboard expressions.txt", "Pitch plus an interval"}, {"fader expressions.txt", "Fader inverted (max=10)"},
             {"samples & looping.txt", "Backwards & forwards through a GenericSource or Sample"},
             {"BPM.txt", "Loop a sample at any !BPM"},
             {"control functions.txt", "(-1 -> 1) repeating linear in sample's duration"},
             {"control functions.txt", "(0 -> 1-> 0) repeating triangle in sample's duration"},
             {"fader warping.txt", "Nonlinear fader"}, {"arrays.txt", "!Amp fader array algorithmically"},
             {"arrays.txt", "Frequency (pitch) array spaced !Interval apart"},
             {"arrays.txt", "Musical scale steps using a controller"}, {"sequences.txt", "Arpeggiator array"},
             {"sequences.txt", "Arpeggiator array (variable intervals)"},
             {"sequences.txt", "Arpeggiator random access array (variable)"}})
    {
        const auto* entry = entryIn(file, name);
        require(entry != nullptr && !entry->comment.empty(), "" + file + " should hold " + name);
    }
    require(entryIn("fader expressions.txt", "Fader inverted (max=10)")->expression == "1 / (!Fader max: (1/10))"
            && entryIn("samples & looping.txt", "Backwards & forwards through a GenericSource or Sample")->expression
                == "default * !Scale",
        "the fader inverted and backwards and forwards expressions");
    // Dropped on the Frequency field, nothing selected, the
    // field reads default * !Scale.
    require(talas::app::fieldTextAfterExpressionDrop("440 hz", 0, 0,
                entryIn("samples & looping.txt", "Backwards & forwards through a GenericSource or Sample")->expression)
            == "default * !Scale",
        "the field should read default * !Scale");
    const auto seconds = [](std::string_view) -> std::optional<double> { return 4.0; };
    for (const auto& file : files)
    {
        const auto read = talas::app::parseExpressionsFile(talas::app::expressionsFileText(file.entries), file.name.substr(0, file.name.size() - 4));
        require(read.has_value() && read->size() == file.entries.size(), file.name + " should read back as written");
        for (std::size_t index = 0; index < file.entries.size(); ++index)
        {
            const auto& entry = file.entries[index];
            require((*read)[index].name == entry.name && (*read)[index].comment == entry.comment
                    && (*read)[index].expression == entry.expression,
                file.name + "'s " + entry.name + " should read back as written");
            if (entry.expression.starts_with("{"))
            {
                require(talas::core::interval_collect_array::expand(entry.expression).status
                        == talas::core::interval_collect_array::Status::Expanded,
                    entry.name + " should be a list field's collected array");
                continue;
            }
            const auto timed = talas::expr::withSampleFileDurations(entry.expression, seconds);
            const auto parsed = talas::expr::parse(timed.value_or(entry.expression), talas::expr::ExpressionLanguageVersion::ValV1);
            require(parsed.ok(), entry.name + " should parse: " + entry.expression);
        }
    }
    const auto folder = std::filesystem::temp_directory_path() / ("talas-expressions-library-" + std::to_string(std::rand()));
    std::filesystem::remove_all(folder);
    const auto written = talas::app::writeExpressionsLibrary(folder);
    require(written.size() == files.size() && std::filesystem::exists(folder / "fader expressions.txt"),
        "the library should be written as its text files");
    {
        std::ofstream own(folder / "BPM.txt", std::ios::binary | std::ios::trunc);
        own << "EXPR\n\"Mine\"\n!BPM\n";
    }
    require(talas::app::writeExpressionsLibrary(folder).empty() && talas::app::expressionsFilesIn({folder}).size() > 1
            && std::ranges::any_of(talas::app::expressionsFilesIn({folder}), [](const auto& entry) { return entry.name == "Mine"; }),
        "a file already there, one's own changes in it, should be left as it is");
    std::filesystem::remove_all(folder);
}

void testExpressionsLibrary()
{
    const auto library = talas::app::expressionsLibrary();
    require(library.size() >= 30, "the library should hold the common expressions");
    for (const auto& entry : library)
    {
        const auto parsed = talas::expr::parse(entry.expression, talas::expr::ExpressionLanguageVersion::ValV1);
        require(parsed.ok(), "the library's " + entry.name + " should parse: " + entry.expression
                + (parsed.diagnostics.empty() ? std::string{} : " (" + parsed.diagnostics.front().message + ")"));
        require(!entry.category.empty() && !entry.comment.empty(), entry.name + " should have a category and a comment");
    }
    // Arrays, tempo, time-varying expressions, arithmetic with and
    // warping of faders, MIDI events, random values, memory, reserved
    // words, sequences, spectra, time expansion and compression, triggering
    // and unit conversions.
    std::vector<std::string> categories;
    for (const auto& entry : library)
        if (std::ranges::find(categories, entry.category) == categories.end())
            categories.push_back(entry.category);
    require(categories == std::vector<std::string>{"Arrays", "Tempo", "Time-varying", "Fader arithmetic", "Fader warping",
                "MIDI", "Random", "Memory", "Reserved words", "Sequences", "Spectra",
                "Time expansion and compression", "Triggers", "Unit conversions"},
        "the library should group its expressions by these categories");
    const auto interval = talas::app::findExpressionLibraryEntries("pitch interval");
    require(interval.size() == 1 && interval.front().expression == "!KeyPitch + !Interval nn",
        "Pitch plus an interval should read !KeyPitch + !Interval nn");
    require(talas::app::findExpressionLibraryEntries("").size() == library.size(), "an empty query should find everything");
    const auto description = talas::app::expressionLibraryDragDescription(interval.front());
    require(talas::app::expressionFromLibraryDrag(description) == std::optional<std::string>{"!KeyPitch + !Interval nn"}
            && !talas::app::expressionFromLibraryDrag("talas.oscillator").has_value(),
        "a drag from the library should carry its expression, and nothing else should pass for one");
    // Dropped as if pasted, the expression takes the place of the
    // text selected in the field, or of all of it with nothing selected.
    require(talas::app::fieldTextAfterExpressionDrop("!Gain * 2", 0, 5, "!Fader inverted") == "!Fader inverted * 2"
            && talas::app::fieldTextAfterExpressionDrop("!Gain * 2", 3, 3, "!BW1") == "!BW1",
        "a dropped expression should replace the selected text, or the whole field");
}
}

// A hot value is completed as it is
// typed into a field.
void testHotValuesAreCompleted()
{
    const std::vector<std::string> project{"Morph", "VolumeOfDrum"};
    require(talas::app::hotValueCompletion("", "timeIndex", project) == std::optional<std::string>{"TimeIndex"},
        "a lone ! takes the field's name");
    require(talas::app::hotValueCompletion("Sc", "scale", project) == std::optional<std::string>{"Scale"},
        "the first guess is the field's name");
    require(talas::app::hotValueCompletion("V", "scale", {}) == std::optional<std::string>{"Volume"},
        "then a commonly used name, Volume for !V");
    require(talas::app::hotValueCompletion("v", "scale", project) == std::optional<std::string>{"VolumeOfDrum"},
        "the project's Event Values come before the common names, letters' case aside");
    require(talas::app::hotValueCompletion("KeyP", "formant1", {}) == std::optional<std::string>{"KeyPitch"},
        "!KeyP completes to !KeyPitch");
    require(!talas::app::hotValueCompletion("Qx", "scale", {}) && !talas::app::hotValueCompletion("Volume", "scale", {})
            && !talas::app::hotValueCompletion("a b", "scale", {}),
        "nothing to complete when no name begins with the letters, or they are one already");
    require(talas::app::hotValueCompletion("K", "frequency", {}) == std::optional<std::string>{"KeyPitch"}
            && talas::app::hotValueCompletion("KeyV", "frequency", {}) == std::optional<std::string>{"KeyVelocity"}
            && !talas::app::hotValueCompletion("KeyVelocity2", "frequency", {}),
        "typing on guesses again from the letters typed, and a name typed on past a guess keeps them");
    // Completion adds no unit, and GAOscillators' Morph is
    // a field without units.
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* ga = registry.findNodeType("talas.gaOscillators");
    const auto morph = ga == nullptr ? nullptr
                                     : &*std::ranges::find(ga->parameters, std::string_view{"morph"}, &talas::core::ParameterDescriptor::id);
    require(talas::app::hotValueCompletion("KeyN", "morph", {}) == std::optional<std::string>{"KeyNumber"} && morph != nullptr
            && morph->domain == talas::expr::ValueDomain::Scalar,
        "!KeyN should complete with no unit, Morph expecting none");
    require(talas::app::fieldHotValueName("dbMorph") == "DbMorph", "a field's hot value name capitalizes its first letter");
    require(talas::app::hotValueBeingTyped("1 + !Ke", 7) == std::optional<std::string>{"Ke"}
            && talas::app::hotValueBeingTyped("!", 1) == std::optional<std::string>{""}
            && !talas::app::hotValueBeingTyped("1 + !Ke", 5) && !talas::app::hotValueBeingTyped("Ke", 2),
        "the letters of the hot value typed just before the caret");
}


// A double-click just inside punctuation selects what it encloses, before
// or after the text all of it, elsewhere the word or number.
void testDoubleClickSelection()
{
    using Selection = std::pair<std::size_t, std::size_t>;
    const std::string expression = "default * (!KeyPitch + 12 nn)";
    const auto open = expression.find('(') + 1;
    const auto close = expression.find(')');
    require(talas::app::doubleClickSelection(expression, open) == Selection{open, close}
            && talas::app::doubleClickSelection(expression, close) == Selection{open, close},
        "inside the parentheses, what they enclose");
    const std::string named = "'Celthrp2a' sampleFile";
    require(talas::app::doubleClickSelection(named, 1) == Selection{1, 10} && talas::app::doubleClickSelection(named, 10) == Selection{1, 10},
        "inside the quotes, the name they enclose");
    require(talas::app::doubleClickSelection("440 hz", 0) == Selection{0, 6} && talas::app::doubleClickSelection("440 hz", 6) == Selection{0, 6}
            && talas::app::doubleClickSelection("  440 hz", 1) == Selection{0, 8},
        "before or after the contents, all of them");
    require(talas::app::doubleClickSelection("440 hz", 1) == Selection{0, 3}, "elsewhere the word");
    const std::string hot = "!KeyPitch + 0.5";
    require(talas::app::doubleClickSelection(hot, 3) == Selection{0, 9} && talas::app::doubleClickSelection(hot, 13) == Selection{12, 15},
        "a hot value with its !, a number with its point");
    require(talas::app::doubleClickSelection("{256 samp inverse}", 1) == Selection{1, 17}
            && talas::app::doubleClickSelection("x (a (b) c) y", 3) == Selection{3, 10},
        "braces, and parentheses within parentheses");
}

// A Sound pasted into a field reads as its name;
// Paste special offers the last texts cut or copied.
void testPastingSoundsAndPasteSpecial()
{
    require(talas::app::pastedSoundText("oscillator1") == "oscillator1 L", "a pasted Sound reads its left channel");
    // A Sound used as a control signal shows as its name in a
    // box, its channel with it.
    using Span = talas::app::FieldSoundSpan;
    const std::vector<std::string> sounds{"oscillator1", "lfo"};
    require(talas::app::fieldSoundSpans("oscillator1 L * 0.5 + (lfo R)", sounds) == std::vector<Span>{{0, 13}, {23, 28}},
        "each Sound named in the field should be boxed with its channel");
    require(talas::app::fieldSoundSpans("'lfo' \"lfo\" !lfo ?lfo #lfo lfos 2lfo", sounds).empty()
            && talas::app::fieldSoundSpans("lfo", sounds) == std::vector<Span>{{0, 3}},
        "a name in a string or a comment, an Event Value or another word is not a Sound");
    talas::app::PasteHistory history;
    history.remember("!KeyPitch");
    history.remember("440 hz");
    history.remember("");
    history.remember("!KeyPitch");
    require(history.texts() == std::vector<std::string>{"!KeyPitch", "440 hz"}, "the latest first, each once");
    for (int index = 0; index < 20; ++index)
        history.remember(std::to_string(index));
    require(history.texts().size() == talas::app::PasteHistory::limit && history.texts().front() == "19",
        "the last several, at most ten");
}

// In a field Event Values are red, variables green and Event Sources
// blue; a string's or a comment's text is plain.
void testFieldTextIsColouredByKind()
{
    using Kind = talas::app::FieldTextKind;
    using Span = talas::app::FieldTextSpan;
    require(talas::app::fieldTextSpans("!KeyPitch + ?Interval nn * `MIDIController7")
            == std::vector<Span>{{"!KeyPitch", Kind::EventValue}, {" + ", Kind::Plain}, {"?Interval", Kind::Variable},
                {" nn * ", Kind::Plain}, {"`MIDIController7", Kind::EventSource}},
        "an Event Value red, a variable green, an Event Source blue");
    require(talas::app::fieldTextSpans("'!notAName' \"?nor this\" 1 ! 2")
            == std::vector<Span>{{"'!notAName' \"?nor this\" 1 ! 2", Kind::Plain}},
        "a string's or a comment's names, and a lone mark, are plain");
    require(talas::app::fieldTextColour(Kind::EventValue) != talas::app::fieldTextColour(Kind::Variable)
            && ((talas::app::fieldTextColour(Kind::EventValue) >> 16) & 0xffu) > 0xc0u
            && ((talas::app::fieldTextColour(Kind::Variable) >> 8) & 0xffu) > 0xa0u
            && (talas::app::fieldTextColour(Kind::EventSource) & 0xffu) > 0xc0u,
        "red, green and blue");
}

// After Expand the red Event Values of the
// expanded Sound's fields have turned into blue Event Sources, the system
// names of the MIDI controllers the global map (here the Default map) maps
// them to: !Volume shows as `MIDIController07 and !PitchBend as
// `MIDIPitchBend, while a name the map does not hold, the Virtual Control
// Surface's alone, stays a red Event Value.
void testExpandTurnsRedEventValuesIntoBlueEventSources()
{
    using Kind = talas::app::FieldTextKind;
    talas::project::ProjectSoundDefinition res;
    res.definitionId = "res";
    res.displayName = "res";
    res.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "440 hz"}, {"amplitude", "(!Volume * 0.5) + (!PitchBend * 0.1) + (!Private * 0.1)"}}}};
    res.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    talas::project::ProjectSoundDefinition reverb;
    reverb.definitionId = "diy";
    reverb.displayName = "DIY reverb";
    talas::project::ProjectNode inner;
    inner.instanceId = "res1";
    inner.soundDefinitionId = "res";
    reverb.graph.nodes = {inner};
    reverb.outputs = {{"audioOut", talas::core::PortType::Audio, "res1", "audioOut"}};
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.soundDefinitions = {res, reverb};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(document, registry);
    const auto expanded = editor.expandSoundDefinition("diy", "diyExpanded", "DIY reverb expanded");
    require(expanded.ok(), "DIY reverb should expand: " + expanded.message);
    const auto colours = [](const talas::project::ProjectSoundDefinition& sound, std::string_view nodeId) {
        const auto node = std::ranges::find(sound.graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
        require(node != sound.graph.nodes.end() && node->parameters.size() == 2,
            "the Sound should hold its oscillator " + std::string{nodeId});
        return talas::app::fieldTextSpans(*node->parameters[1].expression);
    };
    const auto shows = [](const std::vector<talas::app::FieldTextSpan>& spans, std::string_view text, Kind kind) {
        return std::ranges::any_of(spans, [&](const auto& span) { return span.text == text && span.kind == kind; });
    };
    const auto before = colours(editor.document().soundDefinitions[0], "osc");
    require(shows(before, "!Volume", Kind::EventValue) && shows(before, "!PitchBend", Kind::EventValue),
        "before Expand the field should show red Event Values");
    const auto after = colours(editor.document().soundDefinitions.back(), "res1_osc");
    require(shows(after, "`MIDIController07", Kind::EventSource) && shows(after, "`MIDIPitchBend", Kind::EventSource),
        "after Expand the field should show the blue Event Sources the map names");
    require(!shows(after, "!Volume", Kind::EventValue) && !shows(after, "!PitchBend", Kind::EventValue),
        "the red Event Values the map holds should be gone after Expand");
    require(shows(after, "!Private", Kind::EventValue),
        "a name the map does not hold, the Virtual Control Surface's alone, should stay red");
}

// Tab
// cycles through the Sound's fields one by one, Shift+Tab back; past the
// last field, the signal flow diagram is next.
void testTabCyclesThroughTheFields()
{
    require(talas::app::tabbedParameterField(4, 0, true) == std::optional<std::size_t>{1}
            && talas::app::tabbedParameterField(4, 2, true) == std::optional<std::size_t>{3},
        "Tab should go on to the next field");
    require(talas::app::tabbedParameterField(4, 3, false) == std::optional<std::size_t>{2},
        "Shift+Tab should go back to the field before");
    require(!talas::app::tabbedParameterField(4, 3, true) && !talas::app::tabbedParameterField(4, 0, false),
        "past the last field or before the first, the signal flow diagram takes the key");
    require(!talas::app::tabbedParameterField(0, 0, true) && !talas::app::tabbedParameterField(2, 5, true),
        "no field, no next field");
    // Clicked or tabbed into, a field's
    // contents are selected, a script's too, so typing replaces them; an
    // input field's list of Sounds is not.
    require(talas::app::fieldSelectsAllWhenFocused(false) && !talas::app::fieldSelectsAllWhenFocused(true),
        "a field's contents should be selected when it is clicked or tabbed into");
}

// A Sound dragged into a parameter field gives the
// field its own value for the parameter of that name; a Sound without one
// is refused, the field unchanged.
void testASoundDraggedIntoAFieldGivesItsValue()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    const auto* constant = registry.findNodeType("talas.constant");
    require(oscillator != nullptr && constant != nullptr, "the prototypes are registered");
    const auto frequency = std::ranges::find(oscillator->parameters, std::string_view{"frequency"}, &talas::core::ParameterDescriptor::id);
    require(frequency != oscillator->parameters.end()
            && talas::app::parameterValueOfSound(*oscillator, "frequency") == std::optional<std::string>{frequency->defaultExpression},
        "the field should take the dragged Sound's value for its parameter of that name");
    require(!talas::app::parameterValueOfSound(*constant, "frequency").has_value(),
        "a dragged Sound without a parameter of that name should be refused");
}

// An illegal value makes the field flash and
// the computer beep twice, the field back in its own colour at the end.
void testAnIllegalValueFlashesAndBeepsTwice()
{
    const auto signal = talas::app::illegalFieldSignal();
    require(signal.beepMilliseconds.size() == 2 && signal.beepMilliseconds[0] < signal.beepMilliseconds[1],
        "the computer should beep twice");
    require(signal.flashMilliseconds.size() >= 2 && signal.flashMilliseconds.size() % 2 == 0
            && std::ranges::is_sorted(signal.flashMilliseconds)
            && std::ranges::adjacent_find(signal.flashMilliseconds) == signal.flashMilliseconds.end(),
        "the field should flash, ending in its own colour");
    require(!talas::app::parametersMayMoveTo(true, false) && talas::app::parametersMayMoveTo(true, true)
            && talas::app::parametersMayMoveTo(false, false),
        "while a field holds an illegal value, another Sound's parameters should not be edited");
}

// What is typed into a field is tested only when a
// different Sound is double-clicked or an Action or Info menu operation is
// chosen; typing, the same Sound clicked again and the other menus'
// operations leave it untested, as do Revert and Evaluate, which work on
// the field as it is.
void testTheValueIsTestedOnAnotherSoundOrAnOperation()
{
    using talas::app::FieldTestMoment;
    require(!talas::app::typedFieldValueTested(FieldTestMoment::Typing),
        "a value should not be tested while it is typed");
    require(talas::app::typedFieldValueTested(FieldTestMoment::DifferentSoundClicked)
            && !talas::app::typedFieldValueTested(FieldTestMoment::SameSoundClicked),
        "the value should be tested when a different Sound is double-clicked");
    require(talas::app::typedFieldValueTested(FieldTestMoment::ActionMenuOperation)
            && talas::app::typedFieldValueTested(FieldTestMoment::InfoMenuOperation)
            && !talas::app::typedFieldValueTested(FieldTestMoment::OtherMenuOperation),
        "the value should be tested when an Action or Info menu operation is chosen, not another menu's");
    require(!talas::app::typedFieldValueTested(FieldTestMoment::Revert),
        "Revert drops the changes rather than testing them");
    // Once tested, an illegal value keeps the parameters on its Sound.
    require(!talas::app::parametersMayMoveTo(true, false),
        "a value found illegal keeps another Sound's parameters from being edited");
}

// A Sound dropped into an input that holds Sounds
// asks whether to add it to them or replace one; an empty input, or one of
// a single Sound, asks nothing.
void testDroppingIntoAnInputAsksToAddOrReplace()
{
    require(talas::app::inputDropChoices({"disk", "sample"}, true)
                == std::vector<std::string>{"Add to the Sounds", "Replace disk", "Replace sample"}
            && talas::app::inputDropChoices({}, true).empty() && talas::app::inputDropChoices({"disk"}, false).empty(),
        "the drop should offer to add the Sound or replace one of those there");
}

int main()
{
    try
    {
        testAnIllegalValueFlashesAndBeepsTwice();
        testTheValueIsTestedOnAnotherSoundOrAnOperation();
        testDroppingIntoAnInputAsksToAddOrReplace();
        testEnvelopeFieldEditing();
        testArrayFieldEditing();
        testExpressionsLibrary();
        testTheLibraryAsFiles();
        testExpressionsFilesOfOnesOwn();
        testHotValuesAreCompleted();
        testDoubleClickSelection();
        testPastingSoundsAndPasteSpecial();
        testTabCyclesThroughTheFields();
        testASoundDraggedIntoAFieldGivesItsValue();
        testFieldTextIsColouredByKind();
        testExpandTurnsRedEventValuesIntoBlueEventSources();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "parameter field editors tests passed\n";
    return 0;
}
