#include "talas/project/Tool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <span>
#include <utility>

namespace talas::project::tool
{
namespace
{
using smalltalk::Value;

// The Tool itself, as its code names it self, the signal
// processor, and the text files and Compiled Sound Grids its variables hold.
constexpr std::size_t toolSelf = 0;
constexpr std::size_t signalProcessor = 1;
constexpr std::size_t firstTextFile = std::size_t{1} << 20;
constexpr std::size_t firstSoundGrid = std::size_t{1} << 21;
// Views and their graphics contexts, and the classes and values of a
// View's drawing.
constexpr std::size_t firstView = std::size_t{1} << 22;
constexpr std::size_t firstGraphicsContext = std::size_t{1} << 23;
constexpr std::size_t firstController = std::size_t{1} << 24;
constexpr std::size_t firstSensor = std::size_t{1} << 25;
constexpr std::size_t maximumViews = 1024;
constexpr std::size_t maximumDrawings = 65'536;
constexpr std::size_t viewClass = 2;
constexpr std::size_t colorValueClass = 3;
constexpr std::size_t polylineClass = 4;
constexpr std::size_t colourTag = 5;
constexpr std::size_t polylineTag = 6;
// What one text file may hold, and one character more than a Random's
// largest seed.
constexpr std::size_t maximumTextFileBytes = std::size_t{64} << 20;
constexpr std::size_t seeds = 65536;
// Steps one advance may take, and entries one step may make, before the
// machine is taken to be running away.
constexpr std::size_t maximumStepsPerAdvance = 1'000'000;
constexpr std::size_t maximumEntriesPerStep = 1024;

[[nodiscard]] bool isGlobal(std::string_view name) noexcept
{
    return !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0;
}

constexpr std::array<std::string_view, 5> variableTests{"hasChanged", "isOff", "isOn", "turnedOff", "turnedOn"};

[[nodiscard]] bool isNameCharacter(char character) noexcept
{
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

// A Tool variable sent hasChanged, isOn, isOff, turnedOn or turnedOff
// is asked of the Tool, which knows its value at the last
// step: Exit turnedOn reads (self turnedOn: #Exit). Strings and comments
// stay as they are.
[[nodiscard]] std::string withVariableTests(std::string_view source, const std::function<bool(std::string_view)>& declared)
{
    std::string result;
    for (std::size_t at = 0; at < source.size();)
    {
        const auto character = source[at];
        if (character == '\'' || character == '"')
        {
            const auto close = source.find(character, at + 1);
            const auto end = close == std::string_view::npos ? source.size() : close + 1;
            result.append(source.substr(at, end - at));
            at = end;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(character)) == 0 || (at > 0 && isNameCharacter(source[at - 1])))
        {
            result += character;
            ++at;
            continue;
        }
        auto end = at;
        while (end < source.size() && isNameCharacter(source[end]))
            ++end;
        const auto name = source.substr(at, end - at);
        auto test = end;
        while (test < source.size() && std::isspace(static_cast<unsigned char>(source[test])) != 0)
            ++test;
        auto testEnd = test;
        while (testEnd < source.size() && isNameCharacter(source[testEnd]))
            ++testEnd;
        const auto selector = source.substr(test, testEnd - test);
        const auto isTest = std::ranges::find(variableTests, selector) != variableTests.end()
            && (testEnd == source.size() || source[testEnd] != ':');
        if (test > end && isTest && declared(name))
        {
            result += "(self " + std::string{selector} + ": #" + std::string{name} + ")";
            at = testEnd;
            continue;
        }
        result.append(name);
        at = end;
    }
    return result;
}
}

struct Machine::Impl final : smalltalk::Host
{
    explicit Impl(Definition definition)
        : definition(std::move(definition))
        , session(*this)
    {
    }

    Definition definition;
    smalltalk::Session session;
    std::map<std::string, Value, std::less<>> globals;
    std::map<std::string, Value, std::less<>> locals;
    const State* current = nullptr;
    std::string stateName;
    bool started = false;
    bool stopped = false;
    std::vector<EventWrite> writes;
    // The variables' values when the last step ended.
    std::map<std::string, Value, std::less<>> lastStep;
    std::string error;
    double elapsed = 0.0;
    std::size_t entries = 0;
    // A response that sets a View variable marked
    // Step When Changed to a new View steps the machine, the rest of the
    // response skipped. `interrupted` stops the response at the assignment;
    // `stepDue` asks for the step once the run in hand has ended.
    bool interrupted = false;
    bool stepDue = false;
    // Whether the machine is running a response, a condition or a
    // variable's Initially, rather than being set from outside.
    bool running = false;
    // A text file a variable opened, reading on from its own place.
    struct TextFile
    {
        std::string file;
        FileAccess access = FileAccess::ReadOnly;
        std::size_t read = 0;
    };
    std::vector<TextFile> textFiles;
    std::vector<std::string> soundGrids;
    // Each variable's own Random, text file or Compiled Sound Grid, made
    // once however often its state is entered.
    std::map<const Variable*, Value> opened;
    // The Tool's files, its Text file variables' and its scripts'
    // Filenames'.
    smalltalk::MemoryFileSystem fileSystem;
    std::vector<SoundRequest> sounds;
    struct View
    {
        std::array<double, 4> bounds{};
        std::vector<Drawing> drawings;
        std::array<double, 3> colour{};
        double lineWidth = 1.0;
        std::pair<double, double> cursor{};
        std::string help;
    };
    Playback playback;
    std::vector<View> views;
    // Each View variable's rectangle in the window, and the cursor in it,
    // by the variable's name.
    std::map<std::string, std::array<double, 4>, std::less<>> viewBounds;
    std::map<std::string, std::pair<double, double>, std::less<>> viewCursors;
    // Every program parsed, by its text, run again rather than parsed again.
    std::map<std::string, std::shared_ptr<const smalltalk::Program>, std::less<>> programs;

    [[nodiscard]] const Variable* declared(std::string_view name) const
    {
        const auto& variables = isGlobal(name) || current == nullptr ? definition.variables : current->variables;
        const auto found = std::ranges::find(variables, name, &Variable::name);
        return found == variables.end() ? nullptr : &*found;
    }

    smalltalk::FileSystem* files() override
    {
        return &fileSystem;
    }

    // The project's sample files, and one played.
    std::function<std::optional<smalltalk::SampleFileInfo>(std::string_view)> sampleFiles;

    std::optional<smalltalk::SampleFileInfo> sampleFile(std::string_view name) override
    {
        return sampleFiles ? sampleFiles(name) : std::nullopt;
    }

    bool playSampleFile(std::string_view name) override
    {
        SoundRequest request;
        request.sampleFile = std::string{name};
        sounds.push_back(std::move(request));
        return true;
    }

    std::optional<Value> variable(std::string_view name) override
    {
        if (name == "self")
            return Value::object(toolSelf);
        if (const auto found = locals.find(name); found != locals.end())
            return found->second;
        if (const auto found = globals.find(name); found != globals.end())
            return found->second;
        if (name == "SignalProcessor")
            return Value::object(signalProcessor);
        if (name == "View" || name == "ColorValue" || name == "Polyline")
            return Value::object(name == "View" ? viewClass : name == "ColorValue" ? colorValueClass : polylineClass);
        return std::nullopt;
    }

    // A response's assignment to a variable of the Tool; one it does not
    // declare is refused.
    bool assign(std::string_view name, const Value& value) override
    {
        const auto* declaration = declared(name);
        const auto before = variable(name);
        if (!store(name, value))
            return false;
        // Initializing a View variable that steps the machine when it
        // changes steps it at once, so the code after the assignment is
        // skipped; the refusal ends the response there.
        if (running && declaration != nullptr && declaration->kind == Kind::View && declaration->stepWhenChanged
            && isView(value) && !(before.has_value() && same(*before, value)))
        {
            interrupted = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool isView(const Value& value) const noexcept
    {
        return value.kind == Value::Kind::Object && value.index >= firstView && value.index - firstView < views.size();
    }

    std::optional<Value> send(const Value& receiver, std::string_view selector, std::span<const Value> arguments,
        std::string& failure) override
    {
        if (receiver.kind == Value::Kind::Object && receiver.index == toolSelf && selector == "postAllEvents"
            && arguments.empty())
        {
            postAllEvents();
            return receiver;
        }
        // A value posted as an Event Value, as the signal
        // processor uses it or raw, from 0 to 1.
        if (receiver.kind == Value::Kind::Object && receiver.index == toolSelf && arguments.size() == 2
            && (selector == "postEventValueNamed:dspValue:" || selector == "postEventValueNamed:rawValue:"))
        {
            if ((arguments[0].kind != Value::Kind::String && arguments[0].kind != Value::Kind::Symbol)
                || arguments[1].kind != Value::Kind::Number)
            {
                failure = std::string{selector} + " takes an Event Value's name and a number";
                return std::nullopt;
            }
            auto name = arguments[0].text;
            if (name.starts_with('!'))
                name.erase(0, 1);
            writes.push_back({name, arguments[1].number, selector == "postEventValueNamed:rawValue:"});
            return receiver;
        }
        if (receiver.kind == Value::Kind::Object && receiver.index == toolSelf && arguments.size() == 1
            && arguments[0].kind == Value::Kind::Symbol && selector.ends_with(':')
            && std::ranges::find(variableTests, selector.substr(0, selector.size() - 1)) != variableTests.end())
        {
            return variableTest(selector.substr(0, selector.size() - 1), arguments[0].text, failure);
        }
        if (receiver.kind == Value::Kind::Object && receiver.index >= firstTextFile
            && receiver.index - firstTextFile < textFiles.size())
            return sendTextFile(textFiles[receiver.index - firstTextFile], receiver, selector, arguments, failure);
        if (receiver.kind == Value::Kind::Object && receiver.index >= firstSoundGrid
            && receiver.index - firstSoundGrid < soundGrids.size())
            return sendSoundGrid(soundGrids[receiver.index - firstSoundGrid], selector, arguments, failure);
        if (receiver.kind == Value::Kind::Object
            && (receiver.index == viewClass || receiver.index == colorValueClass || receiver.index == polylineClass
                || receiver.index == colourTag || receiver.index == polylineTag
                || (receiver.index >= firstView && receiver.index - firstView < views.size())
                || (receiver.index >= firstGraphicsContext && receiver.index - firstGraphicsContext < views.size())
                || (receiver.index >= firstController && receiver.index - firstController < views.size())
                || (receiver.index >= firstSensor && receiver.index - firstSensor < views.size())))
            return sendDrawing(receiver, selector, arguments, failure);
        // A Rectangle drawn by its border, or filled.
        if ((selector == "displayStrokedOn:" || selector == "displayFilledOn:") && arguments.size() == 1)
        {
            auto* view = graphicsContextOf(arguments[0]);
            const auto rectangle = view != nullptr ? rectangleOf(receiver, failure) : std::nullopt;
            if (view == nullptr || !rectangle.has_value())
            {
                failure = std::string{selector} + " draws a Rectangle on a View's graphicsContext";
                return std::nullopt;
            }
            Drawing drawing;
            drawing.shape = selector == "displayStrokedOn:" ? Drawing::Shape::Outline : Drawing::Shape::Rectangle;
            drawing.points = {{(*rectangle)[0], (*rectangle)[1]}, {(*rectangle)[2], (*rectangle)[3]}};
            return draw(*view, std::move(drawing), failure) ? std::optional{receiver} : std::nullopt;
        }
        // DspStop stops the Sound as DSP > Stop
        // does, dspReplay plays the last one loaded again.
        if (receiver.kind == Value::Kind::Object && receiver.index == signalProcessor
            && (selector == "stop" || selector == "stopSound" || selector == "dspStop") && arguments.empty())
        {
            sounds.push_back({.stop = true});
            return receiver;
        }
        // The audio channels shown, as the
        // preferences set.
        if (receiver.kind == Value::Kind::Object && receiver.index == signalProcessor
            && selector == "displayableAudioChannels" && arguments.empty())
            return Value::of(static_cast<double>(playback.displayableAudioChannels));
        // The largest amplitude a signal reaches, as
        // an Integer: ours run from -1 to 1, as a SamplesFile's samples do.
        if (receiver.kind == Value::Kind::Object && receiver.index == signalProcessor && selector == "maximumAmplitude"
            && arguments.empty())
            return Value::of(1.0);
        if (receiver.kind == Value::Kind::Object && receiver.index == signalProcessor && selector == "dspReplay"
            && arguments.empty())
        {
            sounds.push_back({.replay = true});
            return receiver;
        }
        // What the signal processor plays, asked of
        // the Tool, and its playback paused, continued or moved on.
        if (receiver.kind == Value::Kind::Object && receiver.index == toolSelf && arguments.empty())
        {
            if (selector == "currentTimeCodeInSeconds")
                return Value::of(playback.timeCodeSeconds);
            if (selector == "currentTimeInSamples")
                return Value::of(playback.timeInSamples);
            if (selector == "atSoundEnd")
                return Value::of(playback.atSoundEnd);
            if (selector == "pause" || selector == "continue")
            {
                sounds.push_back(selector == "pause" ? SoundRequest{.pause = true} : SoundRequest{.resume = true});
                return receiver;
            }
            if (selector == "openControllerSurfaceIfNeeded")
            {
                sounds.push_back({.openControllerSurface = true});
                return receiver;
            }
        }
        if (receiver.kind == Value::Kind::Object && receiver.index == toolSelf && selector == "skipToTime:"
            && arguments.size() == 1)
        {
            const auto& time = arguments[0];
            const auto factor = time.unit == talas::expr::Unit::None || time.unit == talas::expr::Unit::Seconds ? 1.0
                : time.unit == talas::expr::Unit::Milliseconds                                         ? 0.001
                : time.unit == talas::expr::Unit::Minutes                                              ? 60.0
                                                                                                    : -1.0;
            if (time.kind != Value::Kind::Number || factor < 0.0 || !(time.number * factor >= 0.0))
            {
                failure = "skipToTime: takes a time that is not negative";
                return std::nullopt;
            }
            sounds.push_back({.skipToSeconds = time.number * factor});
            return receiver;
        }
        if (receiver.kind == Value::Kind::Object && selector == "printString" && arguments.empty()
            && (receiver.index == toolSelf || receiver.index == signalProcessor))
            return Value::string(receiver.index == toolSelf ? "a Tool" : "SignalProcessor");
        failure = (receiver.index == toolSelf ? "A Tool" : "The object") + std::string{" does not understand "}
            + std::string{selector};
        return std::nullopt;
    }

    // A text file read a character at a time or up
    // to one, and written a string, a character or a space at a time.
    std::optional<Value> sendTextFile(TextFile& handle, const Value& receiver, std::string_view selector,
        std::span<const Value> arguments, std::string& failure)
    {
        if (fileSystem.find(handle.file) == nullptr)
            fileSystem.write(handle.file, std::string{});
        auto& contents = *fileSystem.find(handle.file);
        const auto fail = [&](std::string message) -> std::optional<Value> {
            failure = std::move(message);
            return std::nullopt;
        };
        const auto character = [&](const Value& value) -> std::optional<char> {
            if ((value.kind != Value::Kind::String && value.kind != Value::Kind::Symbol) || value.text.size() != 1)
                return std::nullopt;
            return value.text.front();
        };
        const auto count = [&](const Value& value) -> std::optional<std::size_t> {
            if (value.kind != Value::Kind::Number || value.unit != talas::expr::Unit::None || value.number < 0.0
                || value.number != std::floor(value.number) || value.number > static_cast<double>(maximumTextFileBytes))
                return std::nullopt;
            return static_cast<std::size_t>(value.number);
        };
        const auto name = std::string{selector};
        if (name == "close" || name == "flush")
            return receiver;
        if (name == "printString" && arguments.empty())
            return Value::string("a TextFile('" + handle.file + "')");
        constexpr std::array<std::string_view, 11> reading{"contents", "next", "next:", "skipThrough:", "through:",
            "upTo:", "upToEnd", "atEnd", "do:", "peek", "reset"};
        constexpr std::array<std::string_view, 8> writing{"nextPut:", "nextPutAll:", "cr", "space", "space:", "tab",
            "tab:", "print:"};
        if (std::ranges::find(reading, selector) != reading.end())
        {
            if (handle.access == FileAccess::WriteOnly || handle.access == FileAccess::Append)
                return fail(handle.file + " is open for writing only");
            handle.read = std::min(handle.read, contents.size());
            const auto left = contents.size() - handle.read;
            const auto take = [&](std::size_t length, std::size_t skip) {
                auto text = contents.substr(handle.read, length);
                handle.read = std::min(contents.size(), handle.read + length + skip);
                return Value::string(std::move(text));
            };
            if (name == "contents" && arguments.empty())
                return Value::string(contents);
            if (name == "atEnd" && arguments.empty())
                return Value::of(left == 0);
            if (name == "reset" && arguments.empty())
            {
                handle.read = 0;
                return receiver;
            }
            if ((name == "next" || name == "peek") && arguments.empty())
            {
                if (left == 0)
                    return Value::nil();
                return name == "next" ? take(1, 0) : Value::string(contents.substr(handle.read, 1));
            }
            if (name == "upToEnd" && arguments.empty())
                return take(left, 0);
            if (name == "next:" && arguments.size() == 1)
            {
                const auto wanted = count(arguments[0]);
                if (!wanted.has_value())
                    return fail("next: takes a count of characters");
                if (*wanted > left)
                    return fail("next: reads past the end of " + handle.file);
                return take(*wanted, 0);
            }
            if (name == "do:" && arguments.size() == 1)
            {
                while (handle.read < contents.size())
                {
                    const std::array each{take(1, 0)};
                    if (!sendBack(arguments[0], "value:", each, failure).has_value())
                        return std::nullopt;
                }
                return receiver;
            }
            if (arguments.size() == 1)
            {
                const auto wanted = character(arguments[0]);
                if (!wanted.has_value())
                    return fail(name + " takes a character, as $,");
                const auto found = contents.find(*wanted, handle.read);
                if (name == "skipThrough:")
                {
                    if (found == std::string::npos)
                    {
                        handle.read = contents.size();
                        return Value::nil();
                    }
                    handle.read = found + 1;
                    return receiver;
                }
                if (found == std::string::npos)
                    return take(left, 0);
                return take(found - handle.read + (name == "through:" ? 1 : 0), name == "through:" ? 0 : 1);
            }
        }
        if (std::ranges::find(writing, selector) != writing.end())
        {
            if (handle.access == FileAccess::ReadOnly)
                return fail(handle.file + " is open for reading only");
            std::string written;
            if ((name == "cr" || name == "space" || name == "tab") && arguments.empty())
                written = name == "cr" ? "\n" : name == "space" ? " " : "\t";
            else if ((name == "space:" || name == "tab:") && arguments.size() == 1)
            {
                const auto times = count(arguments[0]);
                if (!times.has_value())
                    return fail(name + " takes a count");
                written.assign(*times, name == "space:" ? ' ' : '\t');
            }
            else if (name == "nextPut:" && arguments.size() == 1)
            {
                const auto put = character(arguments[0]);
                if (!put.has_value())
                    return fail("nextPut: writes a character, as $e");
                written.assign(1, *put);
            }
            else if (name == "nextPutAll:" && arguments.size() == 1)
            {
                if (arguments[0].kind != Value::Kind::String && arguments[0].kind != Value::Kind::Symbol)
                    return fail("nextPutAll: writes a string");
                written = arguments[0].text;
            }
            else if (name == "print:" && arguments.size() == 1)
            {
                const auto shown = sendBack(arguments[0], "printString", {}, failure);
                if (!shown.has_value())
                    return std::nullopt;
                written = shown->text;
            }
            else
                return fail(name + " takes another number of arguments");
            if (contents.size() + written.size() > maximumTextFileBytes)
                return fail(handle.file + " would hold more than 64 MiB");
            contents += written;
            return name == "nextPut:" ? arguments[0] : receiver;
        }
        return fail("A text file does not understand " + name);
    }

    // A Point or a Rectangle of the interpreter's, by its coordinates.
    std::optional<std::pair<double, double>> pointOf(const Value& value, std::string& failure)
    {
        std::array<double, 2> coordinates{};
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            const auto coordinate = sendBack(value, axis == 0 ? "x" : "y", {}, failure);
            if (!coordinate.has_value() || coordinate->kind != Value::Kind::Number)
                return std::nullopt;
            coordinates[axis] = coordinate->number;
        }
        return std::pair{coordinates[0], coordinates[1]};
    }

    std::optional<std::array<double, 4>> rectangleOf(const Value& value, std::string& failure)
    {
        std::array<double, 4> sides{};
        constexpr std::array<std::string_view, 4> names{"left", "top", "right", "bottom"};
        for (std::size_t side = 0; side < 4; ++side)
        {
            const auto found = sendBack(value, names[side], {}, failure);
            if (!found.has_value() || found->kind != Value::Kind::Number)
                return std::nullopt;
            sides[side] = found->number;
        }
        return sides;
    }

    View* graphicsContextOf(const Value& value)
    {
        if (value.kind != Value::Kind::Object || value.index < firstGraphicsContext
            || value.index - firstGraphicsContext >= views.size())
            return nullptr;
        return &views[value.index - firstGraphicsContext];
    }

    bool draw(View& view, Drawing drawing, std::string& failure)
    {
        if (view.drawings.size() >= maximumDrawings)
        {
            failure = "A View holds at most 65536 drawings until it is cleared";
            return false;
        }
        drawing.colour = view.colour;
        drawing.lineWidth = view.lineWidth;
        view.drawings.push_back(std::move(drawing));
        return true;
    }

    [[nodiscard]] static Value colour(double red, double green, double blue)
    {
        auto value = Value::array({Value::of(red), Value::of(green), Value::of(blue)});
        value.kind = Value::Kind::Object;
        value.index = colourTag;
        return value;
    }

    // View new, its bounds and graphicsContext,
    // which clears, paints with a ColorValue, sets its line's width and
    // draws lines, strings, rectangles, dots, arcs and polygons; a Polyline
    // of vertices strokes itself on one.
    std::optional<Value> sendDrawing(const Value& receiver, std::string_view selector, std::span<const Value> arguments,
        std::string& failure)
    {
        const auto fail = [&](std::string message) -> std::optional<Value> {
            failure = std::move(message);
            return std::nullopt;
        };
        const auto name = std::string{selector};
        const auto index = receiver.index;
        if (index == viewClass)
        {
            if (name == "printString")
                return Value::string("View");
            if (name != "new")
                return fail("View understands new");
            if (views.size() >= maximumViews)
                return fail("A Tool makes at most 1024 Views");
            views.emplace_back();
            return Value::object(firstView + views.size() - 1);
        }
        if (index == colorValueClass)
        {
            // The named colours as CSS names them.
            static const std::map<std::string, std::array<double, 3>, std::less<>> named{{"black", {0, 0, 0}},
                {"white", {1, 1, 1}}, {"red", {1, 0, 0}}, {"green", {0, 0.5, 0}}, {"blue", {0, 0, 1}},
                {"yellow", {1, 1, 0}}, {"cyan", {0, 1, 1}}, {"magenta", {1, 0, 1}},
                {"purple", {0.5, 0, 0.5}}, {"orange", {1, 0.647, 0}}, {"gray", {0.5, 0.5, 0.5}},
                {"lightGray", {0.827, 0.827, 0.827}}, {"darkGray", {0.663, 0.663, 0.663}},
                {"brown", {0.647, 0.165, 0.165}}, {"pink", {1, 0.753, 0.796}}};
            if (arguments.empty())
            {
                if (name == "printString")
                    return Value::string("ColorValue");
                const auto found = named.find(name);
                if (found == named.end())
                    return fail("ColorValue does not name the colour " + name);
                return colour(found->second[0], found->second[1], found->second[2]);
            }
            std::vector<double> levels;
            for (const auto& argument : arguments)
            {
                if (argument.kind != Value::Kind::Number || argument.number < 0.0 || argument.number > 1.0)
                    return fail(name + " takes levels from 0 to 1");
                levels.push_back(argument.number);
            }
            if (name == "red:green:blue:")
                return colour(levels[0], levels[1], levels[2]);
            if (name == "brightness:")
                return colour(levels[0], levels[0], levels[0]);
            return fail("ColorValue understands its colours' names, red:green:blue: and brightness:");
        }
        if (index == colourTag)
        {
            if (name == "printString")
            {
                return Value::string("ColorValue red: " + smalltalk::fieldText((*receiver.elements)[0]).value_or("")
                    + " green: " + smalltalk::fieldText((*receiver.elements)[1]).value_or("") + " blue: "
                    + smalltalk::fieldText((*receiver.elements)[2]).value_or(""));
            }
            if (name == "red" || name == "green" || name == "blue")
                return (*receiver.elements)[name == "red" ? 0 : name == "green" ? 1 : 2];
            return fail("A ColorValue does not understand " + name);
        }
        const auto points = [&](const Value& array) -> std::optional<std::vector<std::pair<double, double>>> {
            if (array.kind != Value::Kind::Array)
                return std::nullopt;
            std::vector<std::pair<double, double>> found;
            for (const auto& element : *array.elements)
            {
                const auto point = pointOf(element, failure);
                if (!point.has_value())
                    return std::nullopt;
                found.push_back(*point);
            }
            return found;
        };
        if (index == polylineClass)
        {
            if (name == "printString")
                return Value::string("Polyline");
            if (name != "vertices:" || !points(arguments[0]).has_value())
                return fail("Polyline understands vertices:, of an Array of Points");
            auto polyline = arguments[0];
            polyline.kind = Value::Kind::Object;
            polyline.index = polylineTag;
            return polyline;
        }
        if (index == polylineTag)
        {
            if (name == "printString")
                return Value::string("a Polyline");
            auto* view = arguments.size() == 1 ? graphicsContextOf(arguments[0]) : nullptr;
            if (name != "displayStrokedOn:" || view == nullptr)
                return fail("A Polyline understands displayStrokedOn:, of a View's graphicsContext");
            auto vertices = receiver;
            vertices.kind = Value::Kind::Array;
            Drawing drawing;
            drawing.shape = Drawing::Shape::Polyline;
            drawing.points = *points(vertices);
            return draw(*view, std::move(drawing), failure) ? std::optional{receiver} : std::nullopt;
        }
        if (index >= firstView && index - firstView < views.size())
        {
            const auto& view = views[index - firstView];
            if (name == "printString")
                return Value::string("a View");
            if (name == "graphicsContext")
                return Value::object(firstGraphicsContext + (index - firstView));
            // Its fly-by help, its controller's sensor and the cursor the
            // sensor finds.
            if (name == "helpString:" && arguments.size() == 1)
            {
                if (arguments[0].kind != Value::Kind::String)
                    return fail("helpString: takes a String");
                views[index - firstView].help = arguments[0].text;
                return receiver;
            }
            if (name == "controller")
                return Value::object(firstController + (index - firstView));
            if (name == "invalidateRectangle:" && arguments.size() == 1)
            {
                const auto rectangle = rectangleOf(arguments[0], failure);
                if (!rectangle.has_value())
                    return fail("invalidateRectangle: takes a Rectangle");
                Drawing drawing;
                drawing.shape = Drawing::Shape::Invalidate;
                drawing.points = {{(*rectangle)[0], (*rectangle)[1]}, {(*rectangle)[2], (*rectangle)[3]}};
                return draw(views[index - firstView], std::move(drawing), failure) ? std::optional{receiver}
                                                                                   : std::nullopt;
            }
            if (name == "bounds")
            {
                const auto origin = sendBack(Value::of(view.bounds[0]), "@", std::array{Value::of(view.bounds[1])}, failure);
                const auto corner = origin.has_value()
                    ? sendBack(Value::of(view.bounds[2]), "@", std::array{Value::of(view.bounds[3])}, failure)
                    : std::nullopt;
                return corner.has_value() ? sendBack(*origin, "corner:", std::array{*corner}, failure) : std::nullopt;
            }
            return fail("A View understands bounds and graphicsContext");
        }
        if (index >= firstController && index - firstController < views.size())
        {
            if (name == "sensor")
                return Value::object(firstSensor + (index - firstController));
            return fail("A View's controller understands sensor");
        }
        if (index >= firstSensor && index - firstSensor < views.size())
        {
            const auto& cursor = views[index - firstSensor].cursor;
            if (name == "cursorPoint")
                return sendBack(Value::of(cursor.first), "@", std::array{Value::of(cursor.second)}, failure);
            return fail("A controller's sensor understands cursorPoint");
        }
        auto& view = views[index - firstGraphicsContext];
        if (name == "printString")
            return Value::string("a GraphicsContext");
        if (name == "clear" || name == "flush")
        {
            if (name == "clear")
                view.drawings.clear();
            return receiver;
        }
        if (name == "paint:")
        {
            if (arguments[0].kind != Value::Kind::Object || arguments[0].index != colourTag)
                return fail("paint: takes a ColorValue");
            for (std::size_t part = 0; part < 3; ++part)
                view.colour[part] = (*arguments[0].elements)[part].number;
            return receiver;
        }
        if (name == "lineWidth:")
        {
            if (arguments[0].kind != Value::Kind::Number || !(arguments[0].number > 0.0))
                return fail("lineWidth: takes a width in pixels");
            view.lineWidth = arguments[0].number;
            return receiver;
        }
        Drawing drawing;
        const auto at = [&](const Value& value) {
            const auto point = pointOf(value, failure);
            if (point.has_value())
                drawing.points.push_back(*point);
            return point.has_value();
        };
        const auto within = [&](const Value& value) {
            const auto rectangle = rectangleOf(value, failure);
            if (rectangle.has_value())
                drawing.points = {{(*rectangle)[0], (*rectangle)[1]}, {(*rectangle)[2], (*rectangle)[3]}};
            return rectangle.has_value();
        };
        const auto number = [&](const Value& value, double& into) {
            if (value.kind != Value::Kind::Number)
                return false;
            into = value.number;
            return true;
        };
        auto drawn = false;
        if (name == "displayLineFrom:to:")
        {
            drawing.shape = Drawing::Shape::Line;
            drawn = at(arguments[0]) && at(arguments[1]);
        }
        else if (name == "displayString:at:")
        {
            drawing.shape = Drawing::Shape::String;
            drawing.text = arguments[0].text;
            drawn = (arguments[0].kind == Value::Kind::String || arguments[0].kind == Value::Kind::Symbol) && at(arguments[1]);
        }
        else if (name == "displayRectangle:")
        {
            drawing.shape = Drawing::Shape::Rectangle;
            drawn = within(arguments[0]);
        }
        else if (name == "displayDotOfDiameter:at:")
        {
            drawing.shape = Drawing::Shape::Dot;
            drawn = number(arguments[0], drawing.diameter) && drawing.diameter >= 0.0 && at(arguments[1]);
        }
        else if (name == "displayArcBoundedBy:startAngle:sweepAngle:at:")
        {
            drawing.shape = Drawing::Shape::Arc;
            drawn = within(arguments[0]) && number(arguments[1], drawing.startAngle)
                && number(arguments[2], drawing.sweepAngle) && pointOf(arguments[3], failure).has_value();
            if (drawn)
                drawing.offset = *pointOf(arguments[3], failure);
        }
        else if (name == "displayPolygon:")
        {
            drawing.shape = Drawing::Shape::Polygon;
            const auto vertices = points(arguments[0]);
            drawn = vertices.has_value();
            if (drawn)
                drawing.points = *vertices;
        }
        else
            return fail("A graphics context does not understand " + name);
        if (!drawn)
            return fail(name + " takes another kind of argument");
        return draw(view, std::move(drawing), failure) ? std::optional{receiver} : std::nullopt;
    }

    // The grid plays a Sound it compiled, or compiles one it holds
    // with its free variables bound by a BindingList's Associations.
    std::optional<Value> sendSoundGrid(const std::string& grid, std::string_view selector,
        std::span<const Value> arguments, std::string& failure)
    {
        if (selector == "printString" && arguments.empty())
            return Value::string("a CompiledSoundGrid('" + grid + "')");
        const auto byNumber = selector == "playSoundNumber:" || selector == "playSoundNumber:withSubstitutions:";
        const auto byName = selector == "playSoundNamed:" || selector == "playSoundNamed:withSubstitutions:";
        if (!byNumber && !byName)
        {
            failure = "A Compiled Sound Grid does not understand " + std::string{selector};
            return std::nullopt;
        }
        SoundRequest request;
        request.grid = grid;
        const auto& sound = arguments[0];
        if (byNumber)
        {
            if (sound.kind != Value::Kind::Number || sound.unit != talas::expr::Unit::None || sound.number < 1.0
                || sound.number != std::floor(sound.number) || sound.number > 1'000'000.0)
            {
                failure = std::string{selector} + " takes the Sound's place on the grid, from 1";
                return std::nullopt;
            }
            request.number = static_cast<std::size_t>(sound.number);
        }
        else
        {
            if (sound.kind != Value::Kind::String && sound.kind != Value::Kind::Symbol)
            {
                failure = std::string{selector} + " takes the Sound's name";
                return std::nullopt;
            }
            request.name = sound.text;
        }
        if (arguments.size() == 2)
        {
            request.compile = true;
            if (arguments[1].kind != Value::Kind::Array)
            {
                failure = "withSubstitutions: takes a BindingList";
                return std::nullopt;
            }
            for (const auto& association : *arguments[1].elements)
            {
                const auto key = sendBack(association, "key", {}, failure);
                const auto value = key.has_value() ? sendBack(association, "value", {}, failure) : std::nullopt;
                if (!value.has_value())
                    return std::nullopt;
                // ?sample1 reaches the script as the expression @sample1.
                auto variable = key->text;
                if (!variable.empty() && (variable.front() == '@' || variable.front() == '?' || variable.front() == '#'))
                    variable.erase(0, 1);
                if (variable.empty()
                    || (key->kind != Value::Kind::Expression && key->kind != Value::Kind::Symbol
                        && key->kind != Value::Kind::String))
                {
                    failure = "A BindingList binds variables, as ?sample1 -> 'siren.aif'";
                    return std::nullopt;
                }
                request.bindings.emplace_back(std::move(variable), *value);
            }
        }
        sounds.push_back(std::move(request));
        // True: the request is made.
        return Value::of(true);
    }

    [[nodiscard]] static bool on(const Value& value) noexcept
    {
        return (value.kind == Value::Kind::Boolean && value.boolean) || (value.kind == Value::Kind::Number && value.number > 0.0);
    }

    [[nodiscard]] static bool same(const Value& left, const Value& right) noexcept
    {
        if (left.kind != right.kind)
            return false;
        switch (left.kind)
        {
        case Value::Kind::Boolean:
            return left.boolean == right.boolean;
        case Value::Kind::Number:
            return left.number == right.number && left.unit == right.unit;
        case Value::Kind::String:
        case Value::Kind::Symbol:
        case Value::Kind::Expression:
            return left.text == right.text;
        case Value::Kind::Nil:
            return true;
        default:
        {
            // A Point or an Array is the same by its parts.
            if (left.index != right.index || (left.elements == nullptr) != (right.elements == nullptr))
                return false;
            if (left.elements == nullptr || left.elements == right.elements)
                return true;
            return std::ranges::equal(*left.elements, *right.elements, same);
        }
        }
    }

    // A variable's test against its value when the last step ended.
    std::optional<Value> variableTest(std::string_view test, std::string name, std::string& failure)
    {
        if (name.starts_with('#'))
            name.erase(0, 1);
        const auto now = variable(name);
        if (!now.has_value())
        {
            failure = "The Tool has no variable named " + name;
            return std::nullopt;
        }
        const auto found = lastStep.find(name);
        const auto before = found == lastStep.end() ? *now : found->second;
        if (test == "isOn")
            return Value::of(on(*now));
        if (test == "isOff")
            return Value::of(!on(*now));
        if (test == "hasChanged")
            return Value::of(!same(*now, before));
        if (test == "turnedOn")
            return Value::of(on(*now) && !on(before));
        return Value::of(!on(*now) && on(before));
    }

    void rememberStep()
    {
        lastStep = globals;
        for (const auto& [name, value] : locals)
            lastStep.insert_or_assign(name, value);
    }

    // A number set to a variable with constraints is rounded to its grid,
    // then clipped to its range.
    [[nodiscard]] static Value constrained(const Variable& declaration, Value value)
    {
        if (value.kind != Value::Kind::Number)
            return value;
        // A constraint leaves the number floating point.
        if (declaration.grid.has_value() || declaration.minimum.has_value() || declaration.maximum.has_value())
            value.exact.reset();
        if (declaration.grid.has_value() && *declaration.grid > 0.0)
            value.number = std::round(value.number / *declaration.grid) * *declaration.grid;
        if (declaration.minimum.has_value())
            value.number = std::max(value.number, *declaration.minimum);
        if (declaration.maximum.has_value())
            value.number = std::min(value.number, *declaration.maximum);
        return value;
    }

    bool store(std::string_view name, const Value& value)
    {
        auto& table = isGlobal(name) ? globals : locals;
        const auto found = table.find(name);
        const auto* declaration = declared(name);
        if (found == table.end() || declaration == nullptr)
            return false;
        found->second = constrained(*declaration, value);
        attach(*declaration, found->second);
        writeEvent(*declaration, found->second);
        return true;
    }

    // A View set to a View variable takes the variable's rectangle.
    void attach(const Variable& declaration, const Value& value)
    {
        if (declaration.kind != Kind::View || value.kind != Value::Kind::Object || value.index < firstView
            || value.index - firstView >= views.size())
            return;
        if (const auto bounds = viewBounds.find(declaration.name); bounds != viewBounds.end())
            views[value.index - firstView].bounds = bounds->second;
        if (const auto cursor = viewCursors.find(declaration.name); cursor != viewCursors.end())
            views[value.index - firstView].cursor = cursor->second;
    }

    void writeEvent(const Variable& declaration, const Value& value)
    {
        if (!declaration.eventValue.empty() && value.kind == Value::Kind::Number
            && (declaration.access == EventAccess::WriteOnly || declaration.access == EventAccess::ReadWrite))
        {
            writes.push_back({declaration.eventValue, value.number});
        }
    }

    // self postAllEvents sends every Event Value its variables write.
    void postAllEvents()
    {
        for (const auto& declaration : definition.variables)
        {
            if (const auto found = globals.find(declaration.name); found != globals.end())
                writeEvent(declaration, found->second);
        }
        if (current == nullptr)
            return;
        for (const auto& declaration : current->variables)
        {
            if (const auto found = locals.find(declaration.name); found != locals.end())
                writeEvent(declaration, found->second);
        }
    }

    [[nodiscard]] std::optional<Value> evaluate(const std::string& source, const std::string& what)
    {
        if (source.find_first_not_of(" \t\r\n") == std::string::npos)
            return Value::nil();
        auto text = withVariableTests(source, [&](std::string_view name) {
            return declared(name) != nullptr;
        });
        auto found = programs.find(text);
        if (found == programs.end())
        {
            const auto parsed = smalltalk::parse(text);
            if (!parsed.ok())
            {
                error = what + ": " + parsed.diagnostic->message;
                return std::nullopt;
            }
            found = programs.emplace(std::move(text), parsed.program).first;
        }
        const auto wasRunning = running;
        running = true;
        const auto ran = session.run(found->second);
        running = wasRunning;
        if (interrupted)
        {
            // The response ends at the View's initialization and the
            // machine steps; its value is no state to move to.
            interrupted = false;
            stepDue = true;
            return Value::nil();
        }
        if (!ran.ok())
        {
            error = what + ": " + ran.diagnostic->message;
            return std::nullopt;
        }
        return ran.value;
    }

    [[nodiscard]] bool initialize(const std::vector<Variable>& variables, std::map<std::string, Value, std::less<>>& table)
    {
        for (const auto& declaration : variables)
        {
            table.insert_or_assign(declaration.name, Value::nil());
            const auto value = declaration.kind == Kind::RandomNumbers || declaration.kind == Kind::TextFile
                    || declaration.kind == Kind::CompiledSoundGrid
                ? open(declaration, table)
                : evaluate(declaration.initially, "The initial value of " + declaration.name);
            if (!value.has_value())
                return false;
            table.insert_or_assign(declaration.name, constrained(declaration, *value));
            attach(declaration, table.at(declaration.name));
        }
        return true;
    }

    // A Random seeded afresh, or a text file or Compiled Sound Grid opened,
    // as its variable is set.
    [[nodiscard]] std::optional<Value> open(const Variable& declaration, std::map<std::string, Value, std::less<>>& table)
    {
        const auto first = declaration.initially.find_first_not_of(" \t\r\n");
        const auto last = declaration.initially.find_last_not_of(" \t\r\n");
        const auto named = first == std::string::npos ? std::string{} : declaration.initially.substr(first, last - first + 1);
        const auto made = opened.find(&declaration);
        if (declaration.kind == Kind::RandomNumbers)
        {
            std::size_t seed = seeds;
            if (!named.empty())
            {
                const auto digits = named.find_first_not_of("0123456789") == std::string::npos && named.size() <= 5;
                if (!digits || std::stoul(named) >= seeds)
                {
                    error = "The seed of " + declaration.name + " should be an integer from 0 to 65535";
                    return std::nullopt;
                }
                seed = std::stoul(named);
            }
            if (made == opened.end())
            {
                const auto made = evaluate(seed == seeds ? "Random new" : "Random newForTalasWithSeed: " + named,
                    "The Random of " + declaration.name);
                if (made.has_value())
                    opened.emplace(&declaration, *made);
                return made;
            }
            if (seed == seeds)
                return made->second;
            // Entered again, the state's Random starts over from its seed.
            table.insert_or_assign(declaration.name, made->second);
            if (!evaluate(declaration.name + " seed: " + named, "The Random of " + declaration.name).has_value())
                return std::nullopt;
            return made->second;
        }
        if (named.empty())
        {
            error = declaration.name + " names no file";
            return std::nullopt;
        }
        if (declaration.kind == Kind::CompiledSoundGrid)
        {
            if (made != opened.end())
                return made->second;
            soundGrids.push_back(named);
            return opened.emplace(&declaration, Value::object(firstSoundGrid + soundGrids.size() - 1)).first->second;
        }
        const auto access = declaration.fileAccess;
        const auto* existing = fileSystem.find(named);
        if (existing == nullptr && (access == FileAccess::ReadOnly || access == FileAccess::ReadAppend))
        {
            error = declaration.name + " reads " + named + ", which is not there";
            return std::nullopt;
        }
        if (access == FileAccess::WriteOnly || access == FileAccess::ReadWrite || existing == nullptr)
            fileSystem.write(named, std::string{});
        if (made != opened.end())
        {
            textFiles[made->second.index - firstTextFile].read = 0;
            return made->second;
        }
        textFiles.push_back({named, access, 0});
        return opened.emplace(&declaration, Value::object(firstTextFile + textFiles.size() - 1)).first->second;
    }

    // Its responses to a special condition: onEntry, onExit.
    [[nodiscard]] bool runSpecial(std::string_view condition)
    {
        if (current == nullptr)
            return true;
        const auto* state = current;
        for (const auto& response : state->responses)
        {
            if (response.condition != condition)
                continue;
            const auto due = stepDue;
            if (!evaluate(response.response, state->name + " " + std::string{condition}).has_value())
                return false;
            // A View initialized here steps the machine at once.
            if (stepDue && !due)
                return true;
        }
        return true;
    }

    // Entering a state sets its local variables and runs its onEntry
    // responses; TerminalState stops the Tool.
    [[nodiscard]] bool enter(std::string_view name)
    {
        if (++entries > maximumEntriesPerStep)
        {
            error = "The Tool enters states without end";
            return false;
        }
        stateName = std::string{name};
        locals.clear();
        if (name == "TerminalState")
        {
            current = nullptr;
            stopped = true;
            return true;
        }
        const auto found = std::ranges::find(definition.states, name, &State::name);
        if (found == definition.states.end())
        {
            error = "The Tool has no state named " + std::string{name};
            return false;
        }
        current = &*found;
        return initialize(current->variables, locals) && runSpecial("onEntry");
    }

    [[nodiscard]] bool transition(std::string_view name)
    {
        return runSpecial("onExit") && enter(name);
    }

    // A step, and the steps the View initializations ask for after it.
    [[nodiscard]] bool step()
    {
        return stepOnce() && settle();
    }

    // The steps due because a response initialized a View variable that
    // steps the machine, each of which may ask for another.
    [[nodiscard]] bool settle()
    {
        for (std::size_t steps = 0; stepDue; ++steps)
        {
            stepDue = false;
            if (steps >= maximumEntriesPerStep)
            {
                error = "The Tool steps without end as its Views are initialized";
                return false;
            }
            if (!stepOnce())
                return false;
        }
        return true;
    }

    // A step tests every condition of the state in order, then runs the
    // responses whose conditions held, in order, until one names another
    // state; under the Tool's FirstTrueResponse rule only the first
    // of them runs, and the state is then updated.
    [[nodiscard]] bool stepOnce()
    {
        if (!started || stopped || current == nullptr)
            return true;
        entries = 0;
        struct Remember
        {
            Impl& impl;
            ~Remember() { impl.rememberStep(); }
        } remember{*this};
        const auto* state = current;
        std::vector<const TriggeredResponse*> due;
        for (const auto& response : state->responses)
        {
            if (response.condition == "onEntry" || response.condition == "onExit")
                continue;
            if (response.condition == "true")
            {
                due.push_back(&response);
                continue;
            }
            const auto holds = evaluate(response.condition, state->name + " condition " + response.condition);
            if (!holds.has_value())
                return false;
            if (holds->kind != Value::Kind::Boolean)
            {
                error = state->name + " condition " + response.condition + " should answer true or false";
                return false;
            }
            if (holds->boolean)
                due.push_back(&response);
        }
        if (definition.stepRule == StepRule::FirstTrueResponse && due.size() > 1)
            due.resize(1);
        for (const auto* response : due)
        {
            if (stepDue)
                return true;
            const auto next = evaluate(response->response, state->name + " response to " + response->condition);
            if (!next.has_value())
                return false;
            // Naming the state it is in, it goes on to the next response
            //.
            if (next->kind == Value::Kind::Symbol)
            {
                auto name = next->text;
                if (name.starts_with('#'))
                    name.erase(0, 1);
                if (name != state->name)
                    return transition(name);
            }
        }
        return true;
    }

    [[nodiscard]] double stepInterval() const
    {
        const auto found = globals.find("StepIntervalInMilliseconds");
        return found != globals.end() && found->second.kind == Value::Kind::Number ? found->second.number : -1.0;
    }
};

Machine::Machine(Definition definition)
    : impl_(std::make_unique<Impl>(std::move(definition)))
{
}

Machine::~Machine() = default;

bool Machine::start()
{
    auto& impl = *impl_;
    if (impl.started)
        return true;
    impl.started = true;
    impl.entries = 0;
    for (const auto& state : impl.definition.states)
    {
        const auto special = [](const TriggeredResponse& response) {
            return response.condition == "onEntry" || response.condition == "onExit";
        };
        const auto first = std::ranges::find_if_not(state.responses, special);
        if (std::find_if(first, state.responses.end(), special) != state.responses.end())
        {
            impl.error = state.name + "'s onEntry and onExit come first among its Triggered Responses";
            return false;
        }
    }
    const auto entered = impl.initialize(impl.definition.variables, impl.globals) && impl.enter("InitialState");
    impl.rememberStep();
    // An onEntry that initialized a View variable stepping the
    // machine steps it now.
    return entered && impl.settle();
}

bool Machine::step()
{
    return impl_->step();
}

bool Machine::set(std::string_view name, const smalltalk::Value& value)
{
    auto& impl = *impl_;
    if (!impl.store(name, value))
    {
        impl.error = "The Tool has no variable named " + std::string{name};
        return false;
    }
    const auto* declaration = impl.declared(name);
    return declaration == nullptr || !declaration->stepWhenChanged || impl.step();
}

bool Machine::press(std::string_view name, ButtonEvent event)
{
    auto& impl = *impl_;
    const auto* declaration = impl.declared(name);
    if (declaration == nullptr)
    {
        impl.error = "The Tool has no variable named " + std::string{name};
        return false;
    }
    const auto valueOf = [&](const std::string& source) {
        return impl.evaluate(source, "A button value of " + declaration->name);
    };
    const auto setTo = [&](const std::string& source) {
        const auto value = valueOf(source);
        return value.has_value() && set(name, *value);
    };
    switch (declaration->widget.kind)
    {
    case WidgetKind::Button:
        return event == ButtonEvent::Press ? setTo(declaration->widget.downValue) : setTo(declaration->widget.upValue);
    case WidgetKind::TriggerButton:
        // The down and up values in quick succession, as it is released.
        return event != ButtonEvent::Release
            || (setTo(declaration->widget.downValue) && setTo(declaration->widget.upValue));
    case WidgetKind::ToggleButton:
    {
        if (event != ButtonEvent::Press)
            return true;
        const auto current = variable(name);
        const auto on = current.has_value() && current->kind == Value::Kind::Number && current->number != 0.0;
        return set(name, Value::of(on ? 0.0 : 1.0));
    }
    case WidgetKind::StickButton:
        return event != ButtonEvent::Press || set(name, Value::of(1.0));
    default:
        impl.error = declaration->name + " is not shown as a button";
        return false;
    }
}

bool Machine::readEventValue(std::string_view name, double value)
{
    auto& impl = *impl_;
    auto steps = false;
    const auto reads = [&](const Variable& declaration) {
        return declaration.eventValue == name
            && (declaration.access == EventAccess::ReadOnly || declaration.access == EventAccess::ReadWrite);
    };
    const auto readInto = [&](const std::vector<Variable>& variables, std::map<std::string, Value, std::less<>>& table) {
        for (const auto& declaration : variables)
        {
            const auto found = table.find(declaration.name);
            if (!reads(declaration) || found == table.end())
                continue;
            found->second = Impl::constrained(declaration, Value::of(value));
            steps = steps || declaration.stepWhenChanged;
        }
    };
    readInto(impl.definition.variables, impl.globals);
    if (impl.current != nullptr)
        readInto(impl.current->variables, impl.locals);
    return !steps || impl.step();
}

bool Machine::advance(double milliseconds)
{
    auto& impl = *impl_;
    if (!impl.started || impl.stopped)
        return true;
    const auto interval = impl.stepInterval();
    if (interval < 0.0)
        return true;
    if (interval == 0.0)
        return impl.step();
    impl.elapsed += milliseconds;
    for (std::size_t steps = 0; impl.elapsed >= interval && !impl.stopped; ++steps)
    {
        if (steps >= maximumStepsPerAdvance)
        {
            impl.error = "The Tool steps without end";
            return false;
        }
        impl.elapsed -= interval;
        if (!impl.step())
            return false;
    }
    return true;
}

std::string_view Machine::state() const
{
    return impl_->stateName;
}

bool Machine::stopped() const
{
    return impl_->stopped;
}

std::optional<smalltalk::Value> Machine::variable(std::string_view name) const
{
    if (const auto found = impl_->locals.find(name); found != impl_->locals.end())
        return found->second;
    if (const auto found = impl_->globals.find(name); found != impl_->globals.end())
        return found->second;
    return std::nullopt;
}

const std::vector<EventWrite>& Machine::eventWrites() const
{
    return impl_->writes;
}

void Machine::setTextFile(std::string_view file, std::string contents)
{
    impl_->fileSystem.write(file, std::move(contents));
}

std::optional<std::string> Machine::textFile(std::string_view file) const
{
    return impl_->fileSystem.read(file);
}

const std::vector<SoundRequest>& Machine::soundRequests() const
{
    return impl_->sounds;
}

void Machine::setViewBounds(std::string_view variable, double left, double top, double width, double height)
{
    auto& impl = *impl_;
    const std::array bounds{left, top, left + width, top + height};
    impl.viewBounds.insert_or_assign(std::string{variable}, bounds);
    if (const auto held = this->variable(variable);
        held.has_value() && held->kind == smalltalk::Value::Kind::Object && held->index >= firstView
        && held->index - firstView < impl.views.size())
        impl.views[held->index - firstView].bounds = bounds;
}

void Machine::setCursorPoint(std::string_view variable, double x, double y)
{
    auto& impl = *impl_;
    impl.viewCursors.insert_or_assign(std::string{variable}, std::pair{x, y});
    if (const auto held = this->variable(variable);
        held.has_value() && held->kind == smalltalk::Value::Kind::Object && held->index >= firstView
        && held->index - firstView < impl.views.size())
        impl.views[held->index - firstView].cursor = {x, y};
}

std::string Machine::helpString(std::string_view variable) const
{
    const auto held = this->variable(variable);
    if (!held.has_value() || held->kind != smalltalk::Value::Kind::Object || held->index < firstView
        || held->index - firstView >= impl_->views.size())
        return {};
    return impl_->views[held->index - firstView].help;
}

void Machine::setPlayback(Playback playback)
{
    impl_->playback = playback;
}

void Machine::setSampleFiles(std::function<std::optional<smalltalk::SampleFileInfo>(std::string_view)> files)
{
    impl_->sampleFiles = std::move(files);
}

std::vector<Drawing> Machine::drawings(std::string_view variable) const
{
    const auto held = this->variable(variable);
    if (!held.has_value() || held->kind != smalltalk::Value::Kind::Object || held->index < firstView
        || held->index - firstView >= impl_->views.size())
        return {};
    return impl_->views[held->index - firstView].drawings;
}

std::optional<std::pair<double, double>> Machine::openingExtent()
{
    auto& impl = *impl_;
    if (!impl.globals.contains("MinimumWindowExtent") || !impl.globals.contains("MaximumWindowExtent"))
        return std::nullopt;
    const auto coordinate = [&](std::string_view axis) -> std::optional<double> {
        const auto value = impl.evaluate("((MinimumWindowExtent + MaximumWindowExtent) / 2) " + std::string{axis},
            "The Tool's opening extent");
        if (!value.has_value() || value->kind != Value::Kind::Number)
            return std::nullopt;
        return value->number;
    };
    const auto x = coordinate("x");
    const auto y = x.has_value() ? coordinate("y") : std::nullopt;
    if (!y.has_value())
        return std::nullopt;
    return std::pair{*x, *y};
}

bool Machine::fixedExtent()
{
    auto& impl = *impl_;
    if (!impl.globals.contains("MinimumWindowExtent") || !impl.globals.contains("MaximumWindowExtent"))
        return false;
    const auto same = impl.evaluate("MinimumWindowExtent = MaximumWindowExtent", "The Tool's window extents");
    return same.has_value() && same->kind == Value::Kind::Boolean && same->boolean;
}

std::optional<std::pair<double, double>> Machine::layoutGrid()
{
    auto& impl = *impl_;
    if (!impl.globals.contains("LayoutGrid"))
        return std::nullopt;
    const auto grid = impl.evaluate("LayoutGrid", "The Tool's LayoutGrid");
    if (!grid.has_value())
        return std::nullopt;
    if (grid->kind == Value::Kind::Number)
    {
        if (grid->number <= 0.0)
            return std::nullopt;
        return std::pair{grid->number, grid->number};
    }
    const auto x = impl.evaluate("LayoutGrid x", "The Tool's LayoutGrid");
    const auto y = impl.evaluate("LayoutGrid y", "The Tool's LayoutGrid");
    if (!x.has_value() || !y.has_value() || x->kind != Value::Kind::Number || y->kind != Value::Kind::Number || x->number <= 0.0
        || y->number <= 0.0)
        return std::nullopt;
    return std::pair{x->number, y->number};
}

const std::string& Machine::error() const
{
    return impl_->error;
}

std::optional<std::string_view> valueOptionsFiles(Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::TextFile: return "*.txt";
    case Kind::CompiledSoundGrid: return "*.prg";
    default: return std::nullopt;
    }
}

std::string initiallyNamingFile(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

std::vector<std::string> editorVariableNames(const Definition& tool, std::optional<std::size_t> state)
{
    std::vector<std::string> names;
    if (state.has_value() && *state >= tool.states.size())
        return names;
    for (const auto& variable : state.has_value() ? tool.states[*state].variables : tool.variables)
        names.push_back(variable.name);
    return names;
}

std::optional<std::string> variableToEdit(const Definition& tool, std::optional<std::size_t> state, int row)
{
    const auto names = editorVariableNames(tool, state);
    if (row < 0 || static_cast<std::size_t>(row) >= names.size())
        return std::nullopt;
    return names[static_cast<std::size_t>(row)];
}

std::vector<const Variable*> shownVariables(const Definition& tool, std::string_view state)
{
    std::vector<const Variable*> shown;
    for (const auto& variable : tool.variables)
        shown.push_back(&variable);
    if (const auto found = std::ranges::find(tool.states, state, &State::name); found != tool.states.end())
    {
        for (const auto& variable : found->variables)
            shown.push_back(&variable);
    }
    return shown;
}

std::optional<std::pair<double, double>> layoutGridOf(const Definition& tool)
{
    const auto grid = std::ranges::find(tool.variables, std::string_view{"LayoutGrid"}, &Variable::name);
    if (grid == tool.variables.end())
        return std::nullopt;
    Definition alone;
    alone.variables = {*grid};
    alone.states = {State{"InitialState", {}, {}}};
    Machine machine(std::move(alone));
    if (!machine.start())
        return std::nullopt;
    return machine.layoutGrid();
}

namespace
{
constexpr std::array<std::pair<std::string_view, std::uint32_t>, 14> namedColours{{{"black", 0xff000000u},
    {"white", 0xffffffffu}, {"gray", 0xff808080u}, {"lightGray", 0xffd3d3d3u}, {"darkGray", 0xff404040u},
    {"red", 0xffff0000u}, {"orange", 0xffffa500u}, {"yellow", 0xffffff00u}, {"green", 0xff008000u},
    {"cyan", 0xff00ffffu}, {"blue", 0xff0000ffu}, {"magenta", 0xffff00ffu}, {"purple", 0xff800080u},
    {"brown", 0xffa52a2au}}};
}

const std::vector<std::string>& colourChoices()
{
    static const auto choices = [] {
        std::vector<std::string> names{"Transparent"};
        for (const auto& [name, colour] : namedColours)
            names.emplace_back(name);
        return names;
    }();
    return choices;
}

std::optional<std::uint32_t> colourNamed(std::string_view name)
{
    const auto found = std::ranges::find(namedColours, name, &std::pair<std::string_view, std::uint32_t>::first);
    if (found == namedColours.end())
        return std::nullopt;
    return found->second;
}

std::string colourInitially(std::string_view choice) { return "'" + std::string{choice} + "'"; }
}
