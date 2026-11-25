#pragma once

#include "talas/core/SampleKeyRange.h"
#include "talas/core/SampleLoop.h"
#include "talas/core/SampleMarker.h"
#include "talas/core/SamplePitch.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/SmalltalkExactNumber.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bounded subset of Smalltalk that ParameterTransformer and Script fields
// hold: statements of unary, binary and keyword messages to numbers with
// units, strings, booleans, nil and blocks, temporaries and assignments,
// conditionals and loops. Everything runs once, when the Sound is compiled;
// the host names its own objects (snd, a Script's Inputs) and answers the
// messages sent to them.
namespace talas::project::smalltalk
{
inline constexpr std::size_t maximumSourceBytes = 65'536;
inline constexpr std::size_t maximumSteps = 1'000'000;
inline constexpr std::size_t maximumDepth = 256;

struct Value
{
    enum class Kind : std::uint8_t
    {
        Nil,
        Boolean,
        Number,
        String,
        Symbol,
        // An expression a Sound evaluates as it plays, as an Event Value
        // field holds: arithmetic on it builds its text.
        Expression,
        Block,
        // An object of the host's, by its index.
        Object,
        // An Array, as #(1 2 3), {a. b}, Array with: a with: b or an
        // Interval's values: its elements.
        Array,
    };

    Kind kind = Kind::Nil;
    bool boolean = false;
    double number = 0.0;
    talas::expr::Unit unit = talas::expr::Unit::None;
    std::string text;
    std::size_t index = 0;
    // An Expression whose value the script knows as it runs, a
    // variable's declared value: number and unit hold it.
    bool known = false;
    // An Array's elements.
    std::shared_ptr<const std::vector<Value>> elements;
    // A String's own object:
    // two strings of the same characters are not necessarily the same
    // object, which == asks; a copy of the value is the same object.
    std::shared_ptr<const int> identity;
    // A Number that is an integer or a ratio of integers, kept exactly
    //, number holding its nearest double; nothing
    // for a floating point number.
    std::shared_ptr<const ExactNumber> exact;

    [[nodiscard]] static Value nil() { return {}; }
    [[nodiscard]] static Value of(bool value)
    {
        Value result;
        result.kind = Kind::Boolean;
        result.boolean = value;
        return result;
    }
    [[nodiscard]] static Value of(double value, talas::expr::Unit unit = talas::expr::Unit::None)
    {
        Value result;
        result.kind = Kind::Number;
        result.number = value;
        result.unit = unit;
        return result;
    }
    // A number kept exactly, a ratio being reduced and an integer one whose
    // denominator is 1.
    [[nodiscard]] static Value of(ExactNumber value)
    {
        Value result;
        result.kind = Kind::Number;
        result.number = value.toDouble();
        result.exact = std::make_shared<const ExactNumber>(std::move(value));
        return result;
    }
    [[nodiscard]] static Value string(std::string value)
    {
        Value result;
        result.kind = Kind::String;
        result.text = std::move(value);
        result.identity = std::make_shared<const int>(0);
        return result;
    }
    [[nodiscard]] static Value expression(std::string value)
    {
        Value result;
        result.kind = Kind::Expression;
        result.text = std::move(value);
        return result;
    }
    [[nodiscard]] static Value object(std::size_t index)
    {
        Value result;
        result.kind = Kind::Object;
        result.index = index;
        return result;
    }
    [[nodiscard]] static Value array(std::vector<Value> values)
    {
        Value result;
        result.kind = Kind::Array;
        result.elements = std::make_shared<const std::vector<Value>>(std::move(values));
        return result;
    }
};

// The running interpreter as a host sees it: a message to one of the
// interpreter's own objects, as a Random's next or a block's value.
class Messenger
{
public:
    [[nodiscard]] virtual std::optional<Value> send(
        const Value& receiver,
        std::string_view selector,
        std::span<const Value> arguments,
        std::string& error) = 0;

protected:
    ~Messenger() = default;
};

class Program;
struct RunResult;
class Host;
[[nodiscard]] RunResult run(const Program& program, Host& host);
struct Evaluation;
[[nodiscard]] Evaluation evaluate(std::string_view source, Host& host);

// What the host knows of a sample file a script names: its frames at its own
// sample rate, and its header's loop, pitch and key range where it gives them.
struct SampleFileInfo
{
    std::string name;
    double frames = 0.0;
    double sampleRate = 0.0;
    std::optional<talas::core::SampleLoop> loop;
    std::optional<talas::core::SamplePitch> pitch;
    std::optional<talas::core::SampleKeyRange> keys;
    // Its samples from a start for a length, from -1 to 1, the file mono;
    // nothing where the host cannot read them.
    std::function<std::optional<std::vector<double>>(std::size_t start, std::size_t length)> monoSamples;
    // Its named positions, answered by markers as a Dictionary of Symbols
    // and frames.
    std::vector<talas::core::SampleMarker> markers;
};

// When a file was last read, written and changed, in seconds since 1970
//.
struct FileDates
{
    double accessed = 0.0;
    double modified = 0.0;
    double statusChanged = 0.0;

    bool operator==(const FileDates&) const = default;
};

// The files a script reaches through its host, by path: a Tool's are its own,
// held in memory for the environment to load and save; a Sound's compile
// reaches none.
class FileSystem
{
public:
    virtual ~FileSystem() = default;
    // A file's contents; nothing when there is no such file.
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view path) const = 0;
    // Writes a file, making it and its directories when they are not there.
    virtual void write(std::string_view path, std::string contents) = 0;
    // Removes a file or an empty directory; false when there is neither.
    [[nodiscard]] virtual bool remove(std::string_view path) = 0;
    [[nodiscard]] virtual bool isDirectory(std::string_view path) const = 0;
    virtual void makeDirectory(std::string_view path) = 0;
    // The names of what a directory holds itself, in order.
    [[nodiscard]] virtual std::vector<std::string> list(std::string_view directory) const = 0;
    // A file's dates; none where the files keep none.
    [[nodiscard]] virtual std::optional<FileDates> dates(std::string_view path) const
    {
        (void)path;
        return std::nullopt;
    }
};

// Files in memory, a path's directories made with it; "" is the top.
class MemoryFileSystem final : public FileSystem
{
public:
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override;
    void write(std::string_view path, std::string contents) override;
    [[nodiscard]] bool remove(std::string_view path) override;
    [[nodiscard]] bool isDirectory(std::string_view path) const override;
    void makeDirectory(std::string_view path) override;
    [[nodiscard]] std::vector<std::string> list(std::string_view directory) const override;
    // A file's contents to change in place; nothing when there is no such
    // file.
    [[nodiscard]] std::string* find(std::string_view path);
    // Each file is dated when it is written, by the clock given, seconds
    // since 1970, else the system's.
    [[nodiscard]] std::optional<FileDates> dates(std::string_view path) const override;
    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

private:
    std::map<std::string, std::string, std::less<>> files_;
    std::set<std::string, std::less<>> directories_;
    std::map<std::string, FileDates, std::less<>> dates_;
    std::function<double()> clock_;
};

// A question a script asks the user, by self
// confirm: (OK and Cancel), confirmYesNo: (Yes and No), getValueFromUser:
// (a value typed) or request:initialAnswer:evaluating:ifCancel: (a string
// typed, from its initial answer); and a file to open or to save by Filename
// getFileWithPrompt:fileTypes:ifCancel: or
// putFileWithPrompt:initially:fileTypes:ifCancel:, through a file dialog
// showing the files of the extensions given.
enum class PromptKind
{
    Confirm,
    ConfirmYesNo,
    Value,
    Request,
    OpenFile,
    SaveFile,
};

struct Prompt
{
    PromptKind kind = PromptKind::Confirm;
    std::string question;
    std::string initialAnswer;
    std::vector<std::string> fileExtensions;

    bool operator==(const Prompt&) const = default;
};

// The user's answer: OK (or Yes) with the text typed, or the file chosen;
// or Cancel (or No).
struct PromptAnswer
{
    bool accepted = false;
    std::string text;

    bool operator==(const PromptAnswer&) const = default;
};

// What the host makes of a message to one of its objects, or of a name.
class Host
{
public:
    virtual ~Host() = default;
    // The user's answer to a question the script asks: Cancel unless the host
    // asks; nothing while the host has still to ask it, which stops the script
    // with the question pending.
    [[nodiscard]] virtual std::optional<PromptAnswer> ask(const Prompt& prompt)
    {
        (void)prompt;
        return PromptAnswer{};
    }
    // Whether debug:
    // and debugWithLabel:value: pause on each value shown, OK going on and
    // Cancel stopping the script; not unless the host asks.
    [[nodiscard]] virtual bool pausesAtDebug() { return false; }
    // Allow stack traces: an error tells the messages it was sent within as well,
    // innermost first, each with its bytes; not unless the host asks.
    [[nodiscard]] virtual bool stackTraces() { return false; }
    // The files the script reaches; none unless the host gives them.
    [[nodiscard]] virtual FileSystem* files() { return nullptr; }
    // The seed of a Random made by Random new, which gives another sequence
    // each time the program runs: the system's entropy, unless the host keeps
    // it repeatable.
    [[nodiscard]] virtual std::uint32_t freshSeed();
    // The rate a number of samples becomes a time at, as theFile
    // sampleFrames samp s; none unless the host
    // plays at one.
    [[nodiscard]] virtual double sampleRate() { return 0.0; }
    // The frames a second of SMPTE time code, as asSMPTE writes a time
    //: the project's, else 30.
    [[nodiscard]] virtual unsigned timecodeFramesPerSecond() { return 30; }
    // Whether the script's integers and ratios are exact; in projects before
    // format 121 every number was floating point.
    [[nodiscard]] virtual bool exactNumbers() { return true; }
    // How rounded and truncated round (format 156): Smalltalk's, unless the
    // project takes ManualTable, where -2.5 rounded is -2 and -2.5 truncated
    // -3.
    [[nodiscard]] virtual talas::expr::RoundingRule rounding() { return talas::expr::RoundingRule::Smalltalk; }
    // The sample file of a name, as 'bell.aif' asSampleFile finds it;
    // nothing when the host knows no such file.
    [[nodiscard]] virtual std::optional<SampleFileInfo> sampleFile(std::string_view name)
    {
        (void)name;
        return std::nullopt;
    }
    // A sample file played at the current sample rate; false where nothing can
    // play it, as in a Script compiling.
    [[nodiscard]] virtual bool playSampleFile(std::string_view name)
    {
        (void)name;
        return false;
    }
    // The value a free name stands for, as snd or time.
    [[nodiscard]] virtual std::optional<Value> variable(std::string_view name) = 0;
    // Whether the host keeps a name the script assigns without declaring
    // it, as a Script's MM.
    [[nodiscard]] virtual bool assign(std::string_view name, const Value& value)
    {
        (void)name;
        (void)value;
        return false;
    }
    // The answer to a message to an Object; nothing, with `error` set, for a
    // message the object does not understand.
    [[nodiscard]] virtual std::optional<Value> send(
        const Value& receiver,
        std::string_view selector,
        std::span<const Value> arguments,
        std::string& error) = 0;

protected:
    // The interpreter running a script on this host, while it runs; a
    // host's send may use it to draw from a Random or evaluate a block it
    // was given (nothing, with `error` set, when the message fails).
    [[nodiscard]] std::optional<Value> sendBack(
        const Value& receiver,
        std::string_view selector,
        std::span<const Value> arguments,
        std::string& error)
    {
        if (messenger_ == nullptr)
        {
            error = "No script is running";
            return std::nullopt;
        }
        return messenger_->send(receiver, selector, arguments, error);
    }

private:
    friend RunResult run(const Program& program, Host& host);
    friend Evaluation evaluate(std::string_view source, Host& host);
    friend class Session;
    Messenger* messenger_ = nullptr;
};

struct Diagnostic
{
    std::string message;
    talas::expr::SourceRange range;
};

class Program;

struct ParseResult
{
    std::shared_ptr<const Program> program;
    std::optional<Diagnostic> diagnostic;

    [[nodiscard]] bool ok() const noexcept { return program != nullptr; }
};

[[nodiscard]] ParseResult parse(std::string_view source);
// The variables a program declares and assigns a
// value to but never reads, in the order declared; the Sound compiler warns
// of each.
[[nodiscard]] std::vector<std::string> unreadTemporaries(const Program& program);
// The keywords of the events a program sends, by the
// name of the Sound each goes to: `filts start: 0 s amps: x` gives filts
// amps (start, the event's time, is not a variable).
[[nodiscard]] std::map<std::string, std::set<std::string>> eventKeywords(const Program& program);
// In an event's parameter:value pairs no space comes
// between a name and its colon (freq : 500 hz does not parse) and a space
// must come between the colon and the value. The first keyword of an event
// whose value touches its colon, as freq:500 hz, with its range; nothing
// when every event spaces its values. Smalltalk outside events keeps its
// own rule, which takes either.
[[nodiscard]] std::optional<Diagnostic> eventValueTouchingItsColon(const Program& program);

// XYGraphView openOnPoints: and openOnValues::
// the points of a graph to open in a window of its own, joined by lines;
// values are the y of points at x 1, 2, 3 and so on.
using XYGraph = std::vector<std::pair<double, double>>;

struct RunResult
{
    Value value;
    std::optional<Diagnostic> diagnostic;
    // What the script showed by self debug: and self debugWithLabel:value:
    //, a line each, at most 1024, for the host to
    // show.
    std::vector<std::string> debugLines;
    // The graphs it opened, at most 64.
    std::vector<XYGraph> graphs;

    [[nodiscard]] bool ok() const noexcept { return !diagnostic.has_value(); }
};

// Runs a parsed program against `host`, within maximumSteps message sends.
[[nodiscard]] RunResult run(const Program& program, Host& host);

// Evaluate
// (Ctrl+Y) runs the selected text as a Smalltalk program, in a text window
// or a parameter field, and puts what its value prints as just after the
// selection, as 3 + 4 gives 7. A program that does not parse or run
// answers its diagnostic instead.
struct Evaluation
{
    std::string printed;
    std::optional<Diagnostic> diagnostic;
    std::vector<std::string> debugLines;
    std::vector<XYGraph> graphs;
    // The question the script stopped at, which its host has still to ask
    //.
    std::optional<Prompt> pendingPrompt;

    [[nodiscard]] bool ok() const noexcept { return !diagnostic.has_value(); }
};

[[nodiscard]] Evaluation evaluate(std::string_view source, Host& host);

// Evaluate as a text window or a parameter field does it: the text with what
// the selection prints put just after it, after a space, and the range of that
// result, which stays selected so that Backspace takes it away. An empty
// selection is the line it lies on. A selection that fails leaves the text as
// it was and answers its diagnostic. Offsets are bytes.
struct EvaluatedText
{
    std::string text;
    std::size_t resultStart = 0;
    std::size_t resultEnd = 0;
    std::optional<Diagnostic> diagnostic;
    // What self debug: and self debugWithLabel:value: showed as it ran, in
    // order, for a dialog, and the graphs it
    // opened.
    std::vector<std::string> debugLines;
    std::vector<XYGraph> graphs;
    std::optional<Prompt> pendingPrompt;

    [[nodiscard]] bool ok() const noexcept { return !diagnostic.has_value(); }
};

[[nodiscard]] EvaluatedText evaluateSelection(
    std::string_view text, std::size_t selectionStart, std::size_t selectionEnd, Host& host);

// The host a text window's or a parameter field's Evaluate runs on: no
// Sound's objects, and the sample rate a number of samples is timed at
//.
class EvaluationHost final : public Host
{
public:
    explicit EvaluationHost(double sampleRate = 48000.0, unsigned timecodeFramesPerSecond = 30) noexcept
        : sampleRate_(sampleRate), timecodeFramesPerSecond_(timecodeFramesPerSecond)
    {
    }

    [[nodiscard]] double sampleRate() override { return sampleRate_; }
    [[nodiscard]] unsigned timecodeFramesPerSecond() override { return timecodeFramesPerSecond_; }
    [[nodiscard]] std::optional<Value> variable(std::string_view name) override
    {
        (void)name;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Value> send(
        const Value& receiver, std::string_view selector, std::span<const Value> arguments, std::string& error) override
    {
        (void)receiver;
        (void)arguments;
        error = "Nothing here understands " + std::string{selector};
        return std::nullopt;
    }

    // The user's answers so far, in order, and the
    // seeds handed out before them; a question past the answers stops the
    // evaluation, to be asked and the evaluation run again with its answer,
    // drawing the same random numbers.
    void answerFrom(std::vector<PromptAnswer> answers, std::vector<std::uint32_t> seeds);
    [[nodiscard]] const std::vector<std::uint32_t>& seeds() const noexcept { return seeds_; }
    [[nodiscard]] std::optional<PromptAnswer> ask(const Prompt& prompt) override;
    [[nodiscard]] bool pausesAtDebug() override { return answering_; }
    [[nodiscard]] std::uint32_t freshSeed() override;
    void setStackTraces(bool allowed) noexcept { stackTraces_ = allowed; }
    [[nodiscard]] bool stackTraces() override { return stackTraces_; }
    // The project's rounded and truncated.
    void setRounding(talas::expr::RoundingRule rule) noexcept { rounding_ = rule; }
    [[nodiscard]] talas::expr::RoundingRule rounding() override { return rounding_; }

private:
    bool stackTraces_ = false;
    talas::expr::RoundingRule rounding_ = talas::expr::RoundingRule::Smalltalk;
    double sampleRate_;
    unsigned timecodeFramesPerSecond_;
    bool answering_ = false;
    std::vector<PromptAnswer> answers_;
    std::size_t asked_ = 0;
    std::vector<std::uint32_t> seeds_;
    std::size_t seeded_ = 0;
};

// A run of programs that keep their objects and blocks from one run to the
// next, as a Tool's responses do from step to step:
// a Random kept in a variable draws on, a block kept in one runs later.
class Session
{
public:
    explicit Session(Host& host);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] RunResult run(std::shared_ptr<const Program> program);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// A variable written ?name in a script, as the variable library declares it:
// the expression @name, which a field resolves as it plays, knowing the
// declared value for the script itself, as a value is asked for when the
// Script loads. Nothing when the library declares no such variable; the script
// then holds @name without a value of its own.
[[nodiscard]] std::optional<Value> declaredVariable(
    const ProjectVariableLibrary& library, std::string_view name, std::string_view definitionId = {});

// A value as a number where the script knows it: a known Expression's
// number, anything else unchanged.
[[nodiscard]] Value numberOf(const Value& value);

// The text of a value written into a parameter field: a number with its
// unit, as 440 hz or 0.25 s, an expression, a string, or an Array of plain
// numbers, as #(1 1 1 4); nothing for nil, booleans, blocks and other
// Arrays.
[[nodiscard]] std::optional<std::string> fieldText(const Value& value);

// A list field written as a braced Smalltalk program, as {| p | p := 36.
// (1 to: 15) collect: [:i | p := p + (i mod: 2) + 1]}: the numbers of the
// Array it gives, each with its unit, evaluated as the Sound compiles. Nothing
// when the field is no such program or it gives no Array of numbers.
// exactNumbers false: a project before format 121, every number floating
// point. rounding: the project's rounded and truncated.
[[nodiscard]] std::optional<std::string> listFieldNumbers(std::string_view text, bool exactNumbers = true,
    talas::expr::RoundingRule rounding = talas::expr::RoundingRule::Smalltalk);

// A field written as a Smalltalk program Val's own field programs do
// not cover, as the Fibonacci triggers' | pattern ... | pattern := ... .
// ((1 bpm: !BPM * 2) countTriggersMod: pattern size) of: pattern: the Val
// expression or number it gives, run as the Sound compiles. Nothing for any
// other field.
[[nodiscard]] std::optional<std::string> fieldProgramText(std::string_view text, bool exactNumbers = true,
    talas::expr::RoundingRule rounding = talas::expr::RoundingRule::Smalltalk);

// A field of a project before format 115, where Random new was the same
// stream on every run, with each Random new written as (Random
// newForTalasWithSeed: 0), that stream; since 115 Random new gives another
// each time the program runs.
[[nodiscard]] std::string withRandomNewRepeatable(std::string_view text);
}
