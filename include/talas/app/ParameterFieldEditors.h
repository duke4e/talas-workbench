#pragma once

#include "talas/core/NodeRegistry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The graphical parameter field editors: a GraphicalEnvelope's envelope
// breakpoint field and an array field, as models the inspector draws and
// edits; each reads its field's text and writes it back.
namespace talas::app
{
struct EnvelopeBreakpoint
{
    double seconds = 0.0;
    double value = 0.0;

    bool operator==(const EnvelopeBreakpoint&) const = default;
};

// A GraphicalEnvelope's Envelope, `{0 s} {0} {0.1 s} {1} loopStart ...`
//: breakpoints in time order and the loop
// between two of them.
struct EnvelopeField
{
    std::vector<EnvelopeBreakpoint> points;
    std::optional<std::size_t> loopStart;
    std::optional<std::size_t> loopEnd;

    bool operator==(const EnvelopeField&) const = default;
};

// The field when every time is a number of s, ms or usec (or a bare
// number of seconds) and every value a plain number, the times not going
// back; nothing for any other text, which stays a text field.
[[nodiscard]] std::optional<EnvelopeField> parseEnvelopeField(std::string_view text);
[[nodiscard]] std::string envelopeFieldText(const EnvelopeField& field);

// Shift-click adds a breakpoint: in time order, the time
// not negative and the value within 0 to 1; the index it took.
std::size_t addEnvelopeBreakpoint(EnvelopeField& field, double seconds, double value);
// Delete removes the selected breakpoint, keeping two at least; a loop mark
// on it goes with it.
bool removeEnvelopeBreakpoint(EnvelopeField& field, std::size_t index);
// Dragging moves a breakpoint, its time kept between its neighbours'.
void moveEnvelopeBreakpoint(EnvelopeField& field, std::size_t index, double seconds, double value);
// The loop button marks the selected breakpoint as the loop's start or
// end (the start before the end); marking a breakpoint that already
// holds that mark removes it. Text keeps the loop only with both marks.
bool toggleEnvelopeLoopStart(EnvelopeField& field, std::size_t index);
bool toggleEnvelopeLoopEnd(EnvelopeField& field, std::size_t index);
// The loop's beginning and end show as two little
// red arrows over their breakpoints; as ARGB.
[[nodiscard]] std::uint32_t envelopeLoopArrowColour() noexcept;
// The breakpoint nearest a point, within `reach` in each direction.
[[nodiscard]] std::optional<std::size_t> nearestEnvelopeBreakpoint(
    const EnvelopeField& field, double seconds, double value, double secondsReach, double valueReach);
// The envelope field's grid: a thin line each tenth of a
// second of the envelope as written, a heavy one each second. Each segment
// lasting the inverse of the Rate times as long, a heavy line marks 1/Rate
// seconds of playing, so at a Rate of 0.5 the heavy lines are two seconds
// apart. The lines within the seconds shown (at most 400), each with the
// time it marks as played; a Rate that is not a positive number counts as
// 1, as does one that is not a plain number (`envelopeFieldRate`).
struct EnvelopeGridLine
{
    double seconds = 0.0;
    double playedSeconds = 0.0;
    bool heavy = false;

    bool operator==(const EnvelopeGridLine&) const = default;
};
[[nodiscard]] std::vector<EnvelopeGridLine> envelopeGridLines(double visibleSeconds, double rate);
[[nodiscard]] double envelopeFieldRate(std::string_view rateText);

// An array field: values one after another, separated by
// spaces, each a number with an optional unit word (60 nn, 440 hz, 0.5 s)
// or a number in curly braces.
struct ArrayFieldElement
{
    double value = 0.0;
    std::string unit;

    bool operator==(const ArrayFieldElement&) const = default;
};

// The elements when every one is a number, at most 1024; nothing for an
// array holding Event Values or calculations, which stays a text field.
[[nodiscard]] std::optional<std::vector<ArrayFieldElement>> parseArrayField(std::string_view text);
[[nodiscard]] std::string arrayFieldText(const std::vector<ArrayFieldElement>& elements);

// The span the array editor's bars show: 0 to 1 widened to every value.
struct ArrayFieldRange
{
    double minimum = 0.0;
    double maximum = 1.0;
};
[[nodiscard]] ArrayFieldRange arrayFieldRange(const std::vector<ArrayFieldElement>& elements);

// Shortest decimal text of a number, as the editors write it.
[[nodiscard]] std::string fieldNumberText(double value);

// Typing a hot value into a field completes the word. A lone ! takes the
// field's name; letters after it, the field's name if it begins with them,
// else the first of the project's Event Values and then of the commonly used
// names that does (!V gives !Volume, !KeyP !KeyPitch), letters' case aside.
// The whole name it completes to, or none when nothing begins with the letters
// typed or they are a name already.
[[nodiscard]] std::optional<std::string> hotValueCompletion(
    std::string_view typed, std::string_view fieldName, const std::vector<std::string>& projectNames);
// A field's name as its hot value takes it: timeIndex gives TimeIndex.
[[nodiscard]] std::string fieldHotValueName(std::string_view parameterId);
// The commonly used names the completion offers after the project's.
[[nodiscard]] const std::vector<std::string>& commonHotValueNames();
// The letters of a hot value being typed just before `caret` in `text`,
// after its !, or none when the caret is not at the end of one.
[[nodiscard]] std::optional<std::string> hotValueBeingTyped(std::string_view text, std::size_t caret);

// A double-click in a field: just inside an opening or closing parenthesis,
// bracket, brace or quote it selects what they enclose; before or after
// all of the text, all of it; elsewhere the word or number there (a hot
// value with its !). The selection's start and the position after its end,
// in bytes of `text`.
[[nodiscard]] std::pair<std::size_t, std::size_t> doubleClickSelection(std::string_view text, std::size_t caret);

// A Sound pasted into a hot field: its
// name in place of the selection, reading its left channel as a control
// signal (R reads the right, M the mix).
[[nodiscard]] std::string pastedSoundText(std::string_view soundName);
// A Sound used as a control signal shows in a parameter field as its name
// in a box: the byte ranges of the field's text
// naming one of `sounds`, with the channel (L, R or M) after it; a name
// inside a string or a comment, or one an Event Value, a variable, an
// Event Source or a symbol begins with, is left.
struct FieldSoundSpan
{
    std::size_t start = 0;
    std::size_t end = 0;

    bool operator==(const FieldSoundSpan&) const = default;
};
[[nodiscard]] std::vector<FieldSoundSpan> fieldSoundSpans(std::string_view text, const std::vector<std::string>& sounds);

// How a field's text is coloured: an Event Value, a name after an exclamation
// point, red; a variable, after a question mark, green; an Event Source, after
// a backquote, blue; each in bold, the rest plain. Names inside a quoted
// string or a comment are plain.
enum class FieldTextKind
{
    Plain,
    EventValue,
    Variable,
    EventSource,
};

struct FieldTextSpan
{
    std::string text;
    FieldTextKind kind = FieldTextKind::Plain;

    bool operator==(const FieldTextSpan&) const = default;
};

[[nodiscard]] std::vector<FieldTextSpan> fieldTextSpans(std::string_view text);
[[nodiscard]] std::uint32_t fieldTextColour(FieldTextKind kind) noexcept;

// While a field holds an illegal value, the
// parameters of another Sound cannot be edited; the value is fixed, or
// Revert used, first. Whether a Sound clicked may take the parameters.
[[nodiscard]] bool parametersMayMoveTo(bool illegalValueHeld, bool sameSound) noexcept;

// A value typed into a field is not tested as it
// is typed; it is tested, and taken when legal, once a different Sound is
// double-clicked (clicked here, where a click opens its parameters) or an
// operation of the Action or Info menu is chosen, the click or operation
// refused while the value is illegal. Revert, which drops the changes,
// and Evaluate, which reads the field's own text, leave it untested, as do
// the other menus' operations; Enter still takes the value as before.
enum class FieldTestMoment : std::uint8_t
{
    Typing,
    SameSoundClicked,
    DifferentSoundClicked,
    ActionMenuOperation,
    InfoMenuOperation,
    Revert,
    Evaluate,
    OtherMenuOperation,
};
[[nodiscard]] bool typedFieldValueTested(FieldTestMoment moment) noexcept;

// A Sound dropped into an input that already holds
// Sounds, and takes more than one, asks whether to add it to them or use it
// to replace one of them: "Add to the Sounds" first, then "Replace" and
// each Sound's name. Nothing to ask (an empty list) for an empty input or
// one that takes a single Sound.
[[nodiscard]] std::vector<std::string> inputDropChoices(const std::vector<std::string>& sounds, bool acceptsMany);

// Clicked or tabbed into, a field's contents are selected, so that what is
// typed replaces them,
// a script's as a value's; an input field's list of Sounds is left as it
// is, to place the cursor on one of them.
[[nodiscard]] bool fieldSelectsAllWhenFocused(bool inputField) noexcept;

// Tab and Shift+Tab in a parameter field: the next or the previous of the Sound's
// fields; none past the last or before the first, where the keys go on to
// the signal flow diagram.
[[nodiscard]] std::optional<std::size_t> tabbedParameterField(
    std::size_t count, std::size_t current, bool forward) noexcept;

// Paste special...: the last several texts cut or
// copied, the latest first, each once, at most ten.
class PasteHistory
{
public:
    static constexpr std::size_t limit = 10;
    void remember(std::string text);
    [[nodiscard]] const std::vector<std::string>& texts() const noexcept { return texts_; }

private:
    std::vector<std::string> texts_;
};

// A Sound dragged into a parameter field: the field
// takes the value that Sound gives its own parameter of that name, an
// expression's text; nothing when it has no expression parameter so named,
// and the field is left as it was.
[[nodiscard]] std::optional<std::string> parameterValueOfSound(
    const talas::core::NodeTypeDescriptor& sound, std::string_view parameterId);
// An illegal value in a field: the field
// flashes and the computer beeps twice. The times, in milliseconds from the
// value's rejection, of each beep and of each change of the field's colour,
// to the error colour and back, the last one back to its own.
struct IllegalFieldSignal
{
    std::vector<int> beepMilliseconds;
    std::vector<int> flashMilliseconds;
};

[[nodiscard]] IllegalFieldSignal illegalFieldSignal();
}
