#include "talas/app/ExpressionsLibraryModel.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace talas::app
{
namespace
{
const std::vector<ExpressionLibraryEntry>& entries()
{
    static const std::vector<ExpressionLibraryEntry> library{
        {"Arrays", "Pitch from a list by fader",
            "Steps through a list of note numbers as !Index goes from 0 to 1; for a Frequency field.",
            "((!Index * 7.99) of: #(60 62 64 65 67 69 71 72)) nn"},
        {"Arrays", "Breakpoints through a fader",
            "Bends a fader's 0 to 1 through the points of a curve; for any 0 to 1 field.",
            "!Fader into: #({0 @ 0} {0.5 @ 0.2} {1 @ 1})"},
        {"Tempo", "Beat trigger", "A trigger on every beat at !BPM (0 to 1 scaled to 30 to 330 beats a minute); for a Gate or Trigger field.",
            "1 bpm: (!BPM * 300 + 30)"},
        {"Tempo", "Beat with duty cycle", "A gate on for the first !Duty of every beat at 120 beats a minute; for a Gate field.",
            "1 bpm: 120 dutyCycle: !Duty"},
        {"Tempo", "Beat count", "Counts the beats at 120 beats a minute, wrapping every 16; to index a sequence.",
            "(1 bpm: 120) countTriggersMod: 16"},
        {"Time-varying", "Ramp on each key", "Rises from 0 to 1 over 2 seconds after each key down; for an amplitude or morph field.",
            "!KeyDown ramp: 2 s"},
        {"Time-varying", "Repeating ramp", "A sawtooth from 0 to 1 every half second; for a slow sweep.",
            "1 repeatingRamp: 0.5 s"},
        {"Time-varying", "Repeating triangle", "Up and down between 0 and 1 every 4 seconds; for a pan or filter sweep.",
            "1 repeatingTriangle: 4 s"},
        {"Time-varying", "Slow LFO", "A sine between 0 and 1 at 0.25 hz from the Sound's own time; for a gentle modulation.",
            "(!LocalTime * 0.25 * 2 * Float pi) sin * 0.5 + 0.5"},
        {"Time-varying", "Smoothed fader", "Follows !Fader along a straight line over 0.2 s, so steps do not click.",
            "!Fader smooth: 0.2 s"},
        {"Fader arithmetic", "Inverse fader", "Goes down as !Fader goes up; one fader for two opposite fields.",
            "1 - !Fader"},
        {"Fader arithmetic", "Fader into a range", "Scales a 0 to 1 fader to 100 to 5000 hz; for a Frequency field.",
            "(!Fader * 4900 + 100) hz"},
        {"Fader arithmetic", "Bipolar fader", "Turns a 0 to 1 fader into -1 to 1; for a Pan or detune field.",
            "!Fader * 2 - 1"},
        {"Fader arithmetic", "Crossfade partner", "The other half of a crossfade driven by !Mix; put !Mix in the other field.",
            "1 - !Mix"},
        {"Fader warping", "Exponential fader", "Spends more of the fader's travel on small values; for a gain or rate.",
            "!Fader squared"},
        {"Fader warping", "Frequency on an octave scale", "Moves 20 hz to 20480 hz an octave per tenth of the fader.",
            "(2 raisedTo: !Fader * 10) * 20 hz"},
        {"Fader warping", "Stepped fader", "Rounds a fader to eighths, so it moves in steps.",
            "!Fader roundTo: 0.125"},
        {"Fader warping", "Decibels to gain", "A fader from -60 dB to 0 dB as a linear amplitude.",
            "10 raisedTo: (!Fader * 60 - 60) / 20"},
        {"MIDI", "Pitch plus an interval", "The keyboard's pitch an interval of !Interval half steps away.",
            "!KeyPitch + !Interval nn"},
        {"MIDI", "Pitch an octave up", "The keyboard's pitch an octave higher; for a second oscillator.",
            "!KeyPitch + 12 nn"},
        {"MIDI", "Velocity curve", "Key velocity squared, so soft keys are softer; for an amplitude field.",
            "!KeyVelocity squared"},
        {"MIDI", "Key gate with velocity", "Key down scaled by its velocity; for an envelope's Gate or Scale.",
            "!KeyDown * !KeyVelocity"},
        {"MIDI", "Pitch as frequency", "The keyboard's pitch in hertz, for a field that wants a frequency.",
            "!KeyPitch hz"},
        {"Random", "Random on each key", "A new random number from -1 to 1 at each key down.",
            "!KeyDown nextRandom"},
        {"Random", "Random walk of pitch", "A random pitch within an octave above middle C, new on each beat.",
            "((1 bpm: 120) nextRandom abs * 12 + 60) nn"},
        {"Random", "Normal spread", "Numbers clustered around 0 on each key down, for slight detuning.",
            "!KeyDown nextNormal * 0.1"},
        {"Memory", "Capture while a key is down", "Writes into sample memory for as long as a key is held; for a MemoryWriter's Trigger.",
            "!KeyDown"},
        {"Memory", "Read point by fader", "Moves the reader through the whole memory as !Position goes from 0 to 1; for a TimeIndex field (-1 to 1).",
            "!Position * 2 - 1"},
        {"Reserved words", "Time since the start", "!LocalTime counts the seconds since the Sound started; wrapped every 4 seconds, a slow sawtooth.",
            "!LocalTime mod: 4"},
        {"Reserved words", "Key number as pitch", "The number of the key last pressed as a pitch, without the pitch bend !KeyPitch has.",
            "!KeyNumber nn"},
        {"Sequences", "Eight-step pitch sequence", "Plays eight note numbers in turn, one a beat at 120 beats a minute.",
            "(((1 bpm: 120) countTriggersMod: 8) of: #(60 63 67 70 72 70 67 63)) nn"},
        {"Sequences", "Alternating values", "Toggles between 0 and 1 on each beat.",
            "(1 bpm: 120) asToggle"},
        {"Spectra", "Spectrum transposed by fader", "Scales every partial's frequency up or down an octave as !Interval goes from 0 to 1; for a frequency scale field.",
            "2 raisedTo: !Interval * 2 - 1"},
        {"Spectra", "Partials below a fader", "The number of the highest partial kept, 1 to 128 by !Cutoff; for a field that limits the partials.",
            "(!Cutoff * 127 + 1) truncated"},
        {"Time expansion and compression", "Rate by fader", "Plays from a quarter speed to four times as fast, normal speed at the fader's middle; for a Rate field.",
            "4 raisedTo: !Rate * 2 - 1"},
        {"Time expansion and compression", "Time index over a duration", "Goes from -1 to 1 over 1 to 11 seconds after each key down, stretching a sample or analysis; for a TimeIndex field.",
            "(!KeyDown ramp: (!Duration * 10 + 1) s) * 2 - 1"},
        {"Triggers", "Every fourth beat", "Passes the first of every four beat triggers.",
            "(1 bpm: 120) triggerEvery: 4"},
        {"Triggers", "Sample and hold", "Holds !Fader's value at each beat.",
            "(1 bpm: 120) sampleAndHold: !Fader"},
        {"Unit conversions", "Note number to hertz", "Middle A as a frequency.", "69 nn hz"},
        {"Unit conversions", "Period of a frequency", "The period of 440 hz as a duration.", "440 hz s"},
        {"Unit conversions", "Beats to seconds", "One beat at 120 beats a minute in seconds.", "120 bpm s"},
    };
    return library;
}

[[nodiscard]] std::string lower(std::string_view text)
{
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

constexpr std::string_view dragPrefix = "talasExpression:";
}

std::span<const ExpressionLibraryEntry> expressionsLibrary()
{
    return entries();
}

std::vector<ExpressionLibraryEntry> findExpressionLibraryEntries(std::string_view query)
{
    return findExpressionLibraryEntries(query, {});
}

std::vector<ExpressionLibraryEntry> findExpressionLibraryEntries(
    std::string_view query, std::span<const ExpressionLibraryEntry> more)
{
    std::vector<std::string> words;
    std::string word;
    for (const auto character : lower(query))
    {
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            if (!word.empty())
                words.push_back(std::move(word));
            word.clear();
        }
        else
        {
            word.push_back(character);
        }
    }
    if (!word.empty())
        words.push_back(std::move(word));
    std::vector<ExpressionLibraryEntry> found;
    const auto take = [&](const ExpressionLibraryEntry& entry) {
        const auto text = lower(entry.category + " " + entry.name + " " + entry.comment + " " + entry.expression);
        if (std::ranges::all_of(words, [&](const auto& wanted) { return text.find(wanted) != std::string::npos; }))
            found.push_back(entry);
    };
    std::ranges::for_each(entries(), take);
    std::ranges::for_each(more, take);
    return found;
}

std::optional<std::vector<ExpressionLibraryEntry>> parseExpressionsFile(std::string_view text, std::string category)
{
    std::vector<std::string> lines;
    for (std::size_t start = 0; start <= text.size();)
    {
        const auto end = std::min(text.find('\n', start), text.size());
        auto line = std::string{text.substr(start, end - start)};
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    const auto trimmed = [](std::string value) {
        const auto first = value.find_first_not_of(" \t");
        const auto last = value.find_last_not_of(" \t");
        return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
    };
    std::size_t index = 0;
    while (index < lines.size() && trimmed(lines[index]).empty())
        ++index;
    if (index == lines.size() || trimmed(lines[index]) != "EXPR")
        return std::nullopt;
    ++index;
    std::vector<ExpressionLibraryEntry> found;
    while (index < lines.size())
    {
        const auto opening = trimmed(lines[index]);
        ++index;
        if (opening.empty() || opening.front() != '"')
            continue;
        ExpressionLibraryEntry entry;
        entry.category = category;
        // The name to the end of its line, or to a quote closing an entry
        // with no comment.
        auto name = opening.substr(1);
        auto closed = false;
        if (const auto quote = name.find('"'); quote != std::string::npos)
        {
            name = name.substr(0, quote);
            closed = true;
        }
        entry.name = trimmed(name);
        std::vector<std::string> comment;
        while (!closed && index < lines.size())
        {
            const auto& line = lines[index];
            ++index;
            if (const auto quote = line.find('"'); quote != std::string::npos)
            {
                comment.push_back(line.substr(0, quote));
                closed = true;
            }
            else
            {
                comment.push_back(line);
            }
        }
        for (const auto& line : comment)
        {
            const auto part = trimmed(line);
            if (!part.empty())
                entry.comment += (entry.comment.empty() ? "" : " ") + part;
        }
        std::vector<std::string> expression;
        while (index < lines.size() && (trimmed(lines[index]).empty() || trimmed(lines[index]).front() != '"'))
        {
            if (!trimmed(lines[index]).empty())
                expression.push_back(trimmed(lines[index]));
            ++index;
        }
        for (const auto& line : expression)
            entry.expression += (entry.expression.empty() ? "" : "\n") + line;
        if (!entry.name.empty() && !entry.expression.empty())
            found.push_back(std::move(entry));
    }
    return found;
}

std::vector<ExpressionLibraryEntry> expressionsFilesIn(const std::vector<std::filesystem::path>& folders)
{
    std::vector<std::filesystem::path> files;
    for (const auto& folder : folders)
    {
        for (const auto& place : {folder, folder / "Expressions Library"})
        {
            std::error_code error;
            for (const auto& item : std::filesystem::directory_iterator(place, error))
            {
                if (item.is_regular_file(error) && lower(item.path().extension().string()) == ".txt"
                    && std::ranges::find(files, item.path()) == files.end())
                    files.push_back(item.path());
            }
        }
    }
    std::ranges::sort(files, {}, [](const std::filesystem::path& path) { return path.filename().string(); });
    std::vector<ExpressionLibraryEntry> found;
    for (const auto& file : files)
    {
        std::ifstream input(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (auto parsed = parseExpressionsFile(text, file.stem().string()))
            found.insert(found.end(), std::make_move_iterator(parsed->begin()), std::make_move_iterator(parsed->end()));
    }
    return found;
}

std::string expressionLibraryDragDescription(const ExpressionLibraryEntry& entry)
{
    return std::string{dragPrefix} + entry.expression;
}

std::optional<std::string> expressionFromLibraryDrag(std::string_view description)
{
    if (!description.starts_with(dragPrefix) || description.size() == dragPrefix.size())
        return std::nullopt;
    return std::string{description.substr(dragPrefix.size())};
}

std::string fieldTextAfterExpressionDrop(
    std::string_view text, std::size_t start, std::size_t end, std::string_view expression)
{
    start = std::min(start, text.size());
    end = std::clamp(end, start, text.size());
    if (start == end)
        return std::string{expression};
    return std::string{text.substr(0, start)} + std::string{expression} + std::string{text.substr(end)};
}

std::vector<ExpressionsLibraryFile> expressionsLibraryFiles()
{
    // The file for each of the library's kinds, else the kind's own
    // name.
    const std::vector<std::pair<std::string, std::string>> fileOfKind{{"Tempo", "BPM.txt"}, {"Arrays", "arrays.txt"},
        {"Time-varying", "control functions.txt"}, {"Fader arithmetic", "fader expressions.txt"},
        {"Fader warping", "fader warping.txt"}, {"MIDI", "keyboard expressions.txt"}, {"Sequences", "sequences.txt"}};
    std::vector<ExpressionsLibraryFile> files;
    const auto fileNamed = [&files](const std::string& name) -> ExpressionsLibraryFile& {
        const auto found = std::ranges::find(files, name, &ExpressionsLibraryFile::name);
        if (found != files.end())
            return *found;
        return files.emplace_back(ExpressionsLibraryFile{name, {}});
    };
    const auto add = [&](const std::string& file, std::string name, std::string comment, std::string expression) {
        auto& into = fileNamed(file);
        into.entries.push_back({file.substr(0, file.size() - 4), std::move(name), std::move(comment), std::move(expression)});
    };
    // The tutorial's expressions, under the names it
    // gives them.
    add("fader expressions.txt", "Fader inverted (max=10)",
        "One over the fader: 1 at the top, larger as the fader comes down, and no more than 10 below a tenth. "
        "To make up for a narrower band, put the bandwidth's name, as !BW1, in place of !Fader in a Gain field.",
        "1 / (!Fader max: (1/10))");
    add("samples & looping.txt", "Backwards & forwards through a GenericSource or Sample",
        "For the Frequency field of a Sample or a GenericSource: default is the recording's own rate, so !Scale at 2 plays "
        "it an octave up and twice as fast, at 0.5 an octave down, and below 0 backwards.",
        "default * !Scale");
    add("BPM.txt", "Loop a sample at any !BPM",
        "For the Frequency field of a Sample of a loop eight beats long: its rate set so that the loop plays at !BPM. "
        "Put the name of your own sample file in place of 'my loop', between the single quotes.",
        "default * (!BPM / (('my loop' sampleFileDuration s bpm * 8) removeUnits))");
    add("control functions.txt", "(-1 -> 1) repeating linear in sample's duration",
        "For a TimeIndex field: from -1 to 1 in the time the sample takes, over and over, so the whole sample goes by "
        "at its own speed. Put your sample's name in place of 'my sample'.",
        "1 repeatingFullRamp: ('my sample' sampleFileDuration s)");
    add("control functions.txt", "(0 -> 1-> 0) repeating triangle in sample's duration",
        "For a TimeIndex field: up from 0 to 1 and back again in the time the sample takes, forward and backward through "
        "the second half of it. Put your sample's name in place of 'my sample'.",
        "1 repeatingTriangle: ('my sample' sampleFileDuration s)");
    add("fader warping.txt", "Nonlinear fader",
        "Gives the bottom of the fader more of its travel: half way up is only 0.15, three quarters 0.4, the top 1; "
        "for a field whose useful values all lie low, as a Density.",
        "!Fader into: #({0 @ 0} {0.25 @ 0.05} {0.5 @ 0.15} {0.75 @ 0.4} {1 @ 1})");
    add("arrays.txt", "!Amp fader array algorithmically",
        "For a field that takes an array of amplitudes: sixteen faders, !Amp01 to !Amp16, made by the program rather "
        "than typed; change 16 for as many as the field wants.",
        "{(1 to: 16) collect: [:i | !Amp suffix2: i]}");
    add("arrays.txt", "Frequency (pitch) array spaced !Interval apart",
        "For a field that takes an array of frequencies: sixteen pitches from the key played upward, each !Interval half "
        "steps above the one before.",
        "{(0 to: 15) collect: [:i | !KeyPitch + (i * !Interval nn)]}");
    add("arrays.txt", "Musical scale steps using a controller",
        "For a Frequency field: the key's pitch and a step of the major scale above it, chosen by !Index, so the fader "
        "moves only through the scale.",
        "!KeyPitch + ((!Index * 6) of: #(0 2 4 5 7 9 11)) nn");
    add("sequences.txt", "Arpeggiator array",
        "For a Frequency field: while a key is down, a beat at !BPM steps through a major chord and its octave above "
        "the key, then starts again.",
        "!KeyPitch + (((!KeyDown bpm: !BPM) countTriggersMod: 4) of: #(0 4 7 12)) nn");
    add("sequences.txt", "Arpeggiator array (variable intervals)",
        "As Arpeggiator array, its four intervals faders of their own, !Interval1 to !Interval4.",
        "!KeyPitch + (((!KeyDown bpm: !BPM) countTriggersMod: 4) of: #(!Interval1 !Interval2 !Interval3 !Interval4)) nn");
    add("sequences.txt", "Arpeggiator random access array (variable)",
        "As Arpeggiator array (variable intervals), but each beat picks one of the four intervals at random.",
        "!KeyPitch + (((!KeyDown bpm: !BPM) nextRandom abs * 3.99) of: #(!Interval1 !Interval2 !Interval3 !Interval4)) nn");
    // The library's own expressions, each kind in its file.
    for (const auto& entry : entries())
    {
        const auto mapped = std::ranges::find(fileOfKind, entry.category, &std::pair<std::string, std::string>::first);
        auto file = mapped != fileOfKind.end() ? mapped->second : lower(entry.category) + ".txt";
        auto copy = entry;
        copy.category = file.substr(0, file.size() - 4);
        fileNamed(file).entries.push_back(std::move(copy));
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) { return lower(left.name) < lower(right.name); });
    return files;
}

std::string expressionsFileText(std::span<const ExpressionLibraryEntry> entries)
{
    std::string text = "EXPR\n";
    for (const auto& entry : entries)
    {
        text += "\"" + entry.name + (entry.comment.empty() ? std::string{"\""} : "\n" + entry.comment + "\"") + "\n";
        text += entry.expression + "\n\n";
    }
    return text;
}

std::vector<std::filesystem::path> writeExpressionsLibrary(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> written;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    for (const auto& file : expressionsLibraryFiles())
    {
        const auto path = folder / file.name;
        if (std::filesystem::exists(path, error))
            continue;
        std::ofstream output(path, std::ios::binary);
        output << expressionsFileText(file.entries);
        if (output)
            written.push_back(path);
    }
    return written;
}
}
