#pragma once

#include "talas/project/SmalltalkSubset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A Tool is a state machine with global and
// local variables. Each state holds triggered responses, a condition and the
// Smalltalk it sets off, whose last value, when a Symbol, names the next
// state. On each step the machine tests every condition of its state in
// order and runs the responses whose conditions held, in order, until one
// moves it to another state. The widgets that show a Tool's variables, its
// windows and the Tool editor are the environment's; this is the machine.
namespace talas::project::tool
{
// How a variable reaches the Event Value of its name.
enum class EventAccess
{
    None,
    ReadOnly,
    WriteOnly,
    ReadWrite,
};

// What a variable represents. A Number, a String and an Object,
// any Smalltalk object, hold what Initially gives; the kind tells the
// widgets how to show it.
enum class Kind
{
    Object,
    Number,
    String,
    // A Random number stream, which Initially seeds with an integer from 0
    // to 65535; Random new's when it is empty.
    RandomNumbers,
    // A text file Initially names, which the Tool reads and writes as its
    // access lets.
    TextFile,
    // A Compiled Sound Grid, its file named in Initially, which plays the
    // Sounds it holds.
    CompiledSoundGrid,
    // A View, a rectangle of the Tool's window a response draws in once it
    // sets the variable to View new.
    View,
};

// How a Tool opens a text file. Reads go on from the start of the
// file, writes at its end.
enum class FileAccess
{
    ReadOnly,
    WriteOnly,
    // Writing after what the file held.
    Append,
    ReadAppend,
    // Reading and writing a file emptied as the Tool opens it.
    ReadWrite,
};

// How a variable shows on the Tool's window:
// not at all, as text to read or to type into, a meter, a fader with its
// number or without, radio buttons, one of the four buttons, or a View.
enum class WidgetKind
{
    None,
    ReadOnlyText,
    Text,
    Meter,
    Fader,
    FaderNoNumber,
    RadioButtons,
    Button,
    TriggerButton,
    ToggleButton,
    StickButton,
    View,
};

// The upper half of the Variable Settings dialog: the widget, its label,
// its rectangle in the Tool's window and its display options; and a
// button's values (Set Value Options...).
struct Widget
{
    WidgetKind kind = WidgetKind::None;
    // The lower Visible box: the widget shows the variable's name.
    bool labelled = false;
    double left = 0.0;
    double top = 0.0;
    double width = 120.0;
    double height = 24.0;
    // Digits after the decimal point a number shows, none for all of them;
    // a fader's log taper; radio buttons' columns and strings.
    std::optional<int> digits;
    bool logTaper = false;
    int columns = 1;
    std::vector<std::string> strings;
    // Smalltalk for a button's values down and up.
    std::string downValue = "1";
    std::string upValue = "0";
};

struct Variable
{
    // A global's name begins with an uppercase letter, a local's with a
    // lowercase one.
    std::string name;
    Kind kind = Kind::Object;
    // Smalltalk giving its first value, as 0, 'BluePill' or Random
    // newForTalasWithSeed: 7, nil when empty; a Random's seed; a text file's
    // or a Compiled Sound Grid's name.
    std::string initially;
    FileAccess fileAccess = FileAccess::ReadOnly;
    // Whether a change to it steps the machine.
    bool stepWhenChanged = false;
    std::string eventValue;
    EventAccess access = EventAccess::None;
    // A number set to it is rounded to the grid, then clipped to the range
    //.
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> grid;
    Widget widget;
};

// A button's mouse: pressed inside it, released inside it, or
// held down while the mouse moves outside it.
enum class ButtonEvent
{
    Press,
    Release,
    Leave,
};

struct TriggeredResponse
{
    // onEntry, onExit, true, or Smalltalk answering true or false.
    std::string condition;
    // Smalltalk whose last value, a Symbol such as #TerminalState, is the
    // next state; any other value stays in the state.
    std::string response;
};

struct State
{
    std::string name;
    std::vector<Variable> variables;
    std::vector<TriggeredResponse> responses;
};

// Which of the responses whose conditions held a step runs. By default the
// step collects every true condition's response and runs them in order until
// one moves the machine; a Tool's choice in the editor has it run the first
// block whose condition is true and then update the state.
enum class StepRule
{
    EveryTrueResponse,
    FirstTrueResponse,
};

struct Definition
{
    // The global variables; StepIntervalInMilliseconds among them sets the
    // machine's pace.
    std::vector<Variable> variables;
    // InitialState among them; TerminalState, where the Tool stops, need
    // not be.
    std::vector<State> states;
    StepRule stepRule = StepRule::EveryTrueResponse;
};

// An Event Value the Tool wrote, as a variable that writes it changed or
// self postAllEvents sent it.
struct EventWrite
{
    std::string name;
    double value = 0.0;
    // A raw value, from 0 to 1, which the signal processor scales and
    // offsets before a Sound uses it.
    bool raw = false;
};

// A Sound a Tool asked a Compiled Sound Grid to play, or SignalProcessor
// stop.
struct SoundRequest
{
    // Stopping the Sound playing, as Ctrl+K does; pausing it, continuing
    // from the pause, or playing on from a time, as in the Timeline (p.
    // 388); playing the last Sound loaded again, as DSP > Replay.
    bool stop = false;
    bool replay = false;
    bool pause = false;
    bool resume = false;
    std::optional<double> skipToSeconds;
    // The virtual control surface opened if the Sound loaded has controls
    //.
    bool openControllerSurface = false;
    // The Compiled Sound Grid's file.
    std::string grid;
    // The Sound by its place on the grid, from 1, or by its name.
    std::size_t number = 0;
    std::string name;
    // Whether the Sound is compiled as it plays, rather than played as
    // the grid compiled it, and the values of its free variables.
    bool compile = false;
    std::vector<std::pair<std::string, smalltalk::Value>> bindings;
    // A sample file a SamplesFile asked to play at the current sample rate
    //, by the name the environment gave it.
    std::string sampleFile;
};

// One thing a response drew in a View through its graphicsContext (pp.
// 316-317), which the environment paints in order. Coordinates are the
// window's pixels, y growing downwards.
struct Drawing
{
    enum class Shape : std::uint8_t
    {
        Line,
        String,
        // A filled rectangle, and one stroked by its border.
        Rectangle,
        Outline,
        Dot,
        Arc,
        // A filled polygon, closed when its first and last points are the
        // same, and a stroked polyline.
        Polygon,
        Polyline,
        // A rectangle of the view to be drawn again.
        Invalidate,
    };

    Shape shape = Shape::Line;
    // A line's ends, a string's place, a rectangle's or an arc's bounding
    // rectangle's origin and corner, a dot's centre, a polygon's points.
    std::vector<std::pair<double, double>> points;
    std::string text;
    double diameter = 0.0;
    // An arc's start, in degrees from hard right, and how far it sweeps,
    // positive clockwise; and the offset that shifts it in the view.
    double startAngle = 0.0;
    double sweepAngle = 0.0;
    std::pair<double, double> offset{};
    // The paint's red, green and blue, from 0 to 1, and the line's width in
    // pixels, as the graphics context had them.
    std::array<double, 3> colour{};
    double lineWidth = 1.0;
};

// What the signal processor tells a Tool: the time within the
// Sound playing, in samples, the time code it receives, in seconds, and
// whether the Sound last loaded has finished.
struct Playback
{
    double timeInSamples = 0.0;
    double timeCodeSeconds = 0.0;
    bool atSoundEnd = false;
    // The audio channels shown, as the preferences set: 2, or 8
    // with Show Eight Channels.
    std::size_t displayableAudioChannels = 2;
};

class Machine
{
public:
    explicit Machine(Definition definition);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Sets the global variables and enters InitialState; false, with
    // error(), when a variable's value or a response fails, or a state's
    // onEntry or onExit follows another of its conditions.
    [[nodiscard]] bool start();
    // One step of the state machine.
    [[nodiscard]] bool step();
    // A variable set from outside, as a widget sets it; steps when the
    // variable steps the machine on a change.
    [[nodiscard]] bool set(std::string_view name, const smalltalk::Value& value);
    // A button's widget pressed, released or left: a Button sets
    // its variable to its down value as it is pressed and to its up value
    // as it is released or left; a trigger sends the down value and then
    // the up value as it is released; a toggle goes between 0 and 1 at each
    // press; a stick goes to 1, which only the Tool's own code resets.
    [[nodiscard]] bool press(std::string_view name, ButtonEvent event);
    // An Event Value the Tool reads, setting the variables that read it.
    [[nodiscard]] bool readEventValue(std::string_view name, double value);
    // Time passing: a positive StepIntervalInMilliseconds steps once per
    // interval, 0 once per call, and a negative one never.
    [[nodiscard]] bool advance(double milliseconds);

    [[nodiscard]] std::string_view state() const;
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] std::optional<smalltalk::Value> variable(std::string_view name) const;
    [[nodiscard]] const std::vector<EventWrite>& eventWrites() const;
    // A text file as the Tool will find it, which the environment reads
    // from the disk before start.
    void setTextFile(std::string_view file, std::string contents);
    // A text file as the Tool has left it; nothing for one it never knew.
    [[nodiscard]] std::optional<std::string> textFile(std::string_view file) const;
    // The Sounds the Tool asked to play or stop, in order.
    [[nodiscard]] const std::vector<SoundRequest>& soundRequests() const;
    // A View variable's rectangle in the window, which the environment lays
    // out; the View a response sets the variable to has these bounds.
    void setViewBounds(std::string_view variable, double left, double top, double width, double height);
    // Where the cursor lies in a View variable's view, in the window's
    // pixels, as its controller's sensor answers.
    void setCursorPoint(std::string_view variable, double x, double y);
    // The fly-by help a response gave the View a variable holds.
    [[nodiscard]] std::string helpString(std::string_view variable) const;
    // The signal processor's playback, as the environment hears it.
    void setPlayback(Playback playback);
    // The sample files a script names, as the environment finds them in the
    // project.
    void setSampleFiles(std::function<std::optional<smalltalk::SampleFileInfo>(std::string_view)> files);
    // What the responses drew in the View a variable holds, since its
    // graphics context was last cleared; nothing when it holds no View.
    [[nodiscard]] std::vector<Drawing> drawings(std::string_view variable) const;
    // The Tool's size as it opens, halfway between MinimumWindowExtent and
    // MaximumWindowExtent; nothing unless it sets both.
    [[nodiscard]] std::optional<std::pair<double, double>> openingExtent();
    // A window that cannot be resized: its minimum and maximum extents the
    // same.
    [[nodiscard]] bool fixedExtent();
    // The spacing of the grid LayoutGrid sets, a number for both directions
    // or a Point; nothing unless it sets one.
    [[nodiscard]] std::optional<std::pair<double, double>> layoutGrid();
    [[nodiscard]] const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Each state is a page of its own on the screen,
// the global variables and that state's own: the variables shown in a
// state, globals first; the globals alone for a state the Tool lacks.
[[nodiscard]] std::vector<const Variable*> shownVariables(const Definition& tool, std::string_view state);
// The Tool editor's variable lists: the Global
// Variables, or, for the state selected in the States list, its Local
// Variables; none for a state the Tool lacks.
[[nodiscard]] std::vector<std::string> editorVariableNames(const Definition& tool, std::optional<std::size_t> state);
// A variable opens in the Variable Settings when its
// name is double-clicked in one of those lists, or selected there and the
// list's Edit button clicked: the name at that row of the list, nothing
// for no row selected (a row below 0) or one past the list's end.
[[nodiscard]] std::optional<std::string> variableToEdit(
    const Definition& tool, std::optional<std::size_t> state, int row);

// Set Value Options...: the files a Text file
// or Compiled Sound Grid variable is located among in the file dialog, and
// the Initially that names the file chosen; nothing for other kinds.
[[nodiscard]] std::optional<std::string_view> valueOptionsFiles(Kind kind) noexcept;
[[nodiscard]] std::string initiallyNamingFile(std::string_view path);

// The grid a Tool's LayoutGrid gives the Layout as the Tool is edited: its
// Initially alone evaluated.
[[nodiscard]] std::optional<std::pair<double, double>> layoutGridOf(const Definition& tool);

// BackgroundColor: the colour menu's choices,
// Transparent (the system's window background) first; the colour a name
// gives, as ARGB, nothing for Transparent or a name not a colour; and the
// Initially a choice sets.
[[nodiscard]] const std::vector<std::string>& colourChoices();
[[nodiscard]] std::optional<std::uint32_t> colourNamed(std::string_view name);
[[nodiscard]] std::string colourInitially(std::string_view choice);
}
