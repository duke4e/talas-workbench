#include "talas/project/Tool.h"
#include "talas/project/ToolFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace tool = talas::project::tool;
namespace st = talas::project::smalltalk;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

tool::Variable variable(std::string name, std::string initially, bool stepWhenChanged = false)
{
    tool::Variable declared;
    declared.name = std::move(name);
    declared.initially = std::move(initially);
    declared.stepWhenChanged = stepWhenChanged;
    return declared;
}

tool::Variable kinded(std::string name, tool::Kind kind, std::string initially,
    tool::FileAccess access = tool::FileAccess::ReadOnly)
{
    auto declared = variable(std::move(name), std::move(initially));
    declared.kind = kind;
    declared.fileAccess = access;
    return declared;
}

double number(const tool::Machine& machine, std::string_view name)
{
    const auto value = machine.variable(name);
    require(value.has_value() && value->kind == st::Value::Kind::Number, std::string{name} + " should be a number");
    return value->number;
}

void started(tool::Machine& machine)
{
    require(machine.start(), "the Tool should start: " + machine.error());
}

// Bozo faces North and turns right or left by 90 degrees;
// the next state depends on the state and the input, so L from South faces
// East and L from North faces West.
void testBozoTurnsByHisState()
{
    tool::Definition bozo;
    bozo.variables = {variable("Input", "''", true)};
    const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> turns{
        {"North", {"East", "West"}}, {"East", {"South", "North"}}, {"South", {"West", "East"}}, {"West", {"North", "South"}}};
    for (const auto& [facing, next] : turns)
    {
        tool::State state;
        state.name = facing;
        state.responses = {{"Input = 'R'", "Input := ''. #" + next.first}, {"Input = 'L'", "Input := ''. #" + next.second}};
        bozo.states.push_back(std::move(state));
    }
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true", "#North"}};
    bozo.states.push_back(std::move(initial));
    tool::Machine machine(std::move(bozo));
    started(machine);
    require(machine.step() && machine.state() == "North", "Bozo's initial state should face North");
    require(machine.set("Input", st::Value::string("L")) && machine.state() == "West",
        "L from North should face West");
    require(machine.set("Input", st::Value::string("L")) && machine.set("Input", st::Value::string("L"))
            && machine.state() == "East",
        "L from South should face East");
    require(machine.set("Input", st::Value::string("R")) && machine.state() == "South",
        "R from East should face South");

    // Each state is a page of its own, the globals and the
    // state's own variables.
    tool::Definition paged;
    paged.variables = {variable("Input", "''", true)};
    tool::State north;
    north.name = "North";
    north.variables = {variable("Compass", "0", true)};
    tool::State south;
    south.name = "South";
    south.variables = {variable("Map", "0", true)};
    paged.states = {north, south};
    const auto names = [&](std::string_view state) {
        std::vector<std::string> shown;
        for (const auto* shownVariable : tool::shownVariables(paged, state))
            shown.push_back(shownVariable->name);
        return shown;
    };
    require(names("North") == std::vector<std::string>{"Input", "Compass"} && names("South") == std::vector<std::string>{"Input", "Map"}
            && names("Elsewhere") == std::vector<std::string>{"Input"},
        "each state should show its own variables beside the globals");
}

// The responses whose conditions hold are collected,
// then run in order; one naming another state moves the machine at once,
// the rest not run, after the state's onExit and before the new state's
// onEntry; a last value that is no Symbol stays; onEntry cannot move; and
// TerminalState stops the Tool.
void testTriggeredResponsesRunInOrder()
{
    tool::Definition definition;
    definition.variables = {variable("Count", "0"), variable("Exited", "false"), variable("Entered", "0"),
        variable("Exit", "false", true)};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onEntry", "#Elsewhere"}, {"onExit", "Exited := true"}, {"true", "Count := Count + 1"},
        {"Count > 1", "#Next"}, {"true", "Count := 100"}};
    tool::State next;
    next.name = "Next";
    next.responses = {{"onEntry", "Entered := Entered + 1"}, {"Exit", "#TerminalState"}, {"true", "3 + 4"}};
    definition.states = {initial, next};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.state() == "InitialState", "onEntry should not move the machine");
    require(machine.step() && machine.state() == "InitialState" && number(machine, "Count") == 100,
        "every response whose condition held should run, in order");
    require(machine.step() && machine.state() == "Next" && number(machine, "Count") == 101
            && machine.variable("Exited")->boolean && number(machine, "Entered") == 1,
        "a response naming a state should move at once, after onExit and before onEntry, the rest not run");
    require(machine.step() && machine.state() == "Next", "a last value that is no Symbol should stay");
    tool::Definition staying;
    staying.variables = {variable("Entries", "0"), variable("After", "false")};
    tool::State here;
    here.name = "InitialState";
    here.responses = {{"onEntry", "Entries := Entries + 1"}, {"true", "#InitialState"}, {"true", "After := true"}};
    staying.states = {here};
    tool::Machine stays(std::move(staying));
    started(stays);
    require(stays.step() && number(stays, "Entries") == 1 && stays.variable("After")->boolean,
        "a response naming the state it is in should go on to the next response");
    require(machine.set("Exit", st::Value::of(true)) && machine.stopped() && machine.state() == "TerminalState",
        "TerminalState should stop the Tool");
}

// A global variable, uppercase, belongs to
// every state, a local one, lowercase, to its own and is set again on each
// entry; StepIntervalInMilliseconds steps a self-running Tool; a Random kept
// in a variable draws on from step to step, and a block kept in a global
// runs in another state. An undeclared variable is refused.
void testVariablesAndTheMachinesPace()
{
    tool::Definition definition;
    definition.variables = {variable("StepIntervalInMilliseconds", "10"), variable("Ticks", "0"), variable("Last", "0"),
        variable("Draws", "OrderedCollection new"), variable("Double", "nil")};
    tool::State initial;
    initial.name = "InitialState";
    initial.variables = {variable("rando", "Random newForTalasWithSeed: 7")};
    initial.responses = {{"onEntry", "Double := [:x | x * 2]"}, {"true", "Ticks := Ticks + 1. Draws add: rando next"},
        {"Ticks >= 5", "#Counting"}};
    tool::State counting;
    counting.name = "Counting";
    counting.variables = {variable("here", "Double value: 21")};
    counting.responses = {{"true", "Last := here"}};
    definition.states = {initial, counting};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.advance(45) && number(machine, "Ticks") == 4 && machine.state() == "InitialState",
        "a 10 ms interval should step four times in 45 ms");
    const auto draws = machine.variable("Draws");
    // Conditions are tested before the step's responses run, so the step
    // after the fifth moves on.
    require(machine.advance(10) && machine.state() == "InitialState" && number(machine, "Ticks") == 5
            && machine.advance(10) && machine.state() == "Counting" && number(machine, "Ticks") == 6,
        "the step after Ticks reached 5 should move on");
    require(machine.advance(10) && number(machine, "Last") == 42 && !machine.variable("rando").has_value(),
        "a block kept in a global should run in another state, whose locals are its own");
    require(draws.has_value(), "the draws should be kept");
    tool::Definition drawing;
    drawing.variables = {variable("First", "0"), variable("Second", "0")};
    tool::State state;
    state.name = "InitialState";
    state.variables = {variable("rando", "Random newForTalasWithSeed: 7")};
    state.responses = {{"First = 0", "First := rando next"}, {"true", "Second := rando next"}};
    drawing.states = {state};
    tool::Machine random(std::move(drawing));
    started(random);
    require(random.step() && random.step() && number(random, "First") != number(random, "Second"),
        "a Random kept in a variable should draw on from step to step");
    tool::Definition undeclared;
    tool::State refusing;
    refusing.name = "InitialState";
    refusing.responses = {{"true", "Nowhere := 1"}};
    undeclared.states = {refusing};
    tool::Machine refused(std::move(undeclared));
    started(refused);
    require(!refused.step() && !refused.error().empty(), "an undeclared variable should be refused");
    tool::Definition paced;
    paced.variables = {variable("StepIntervalInMilliseconds", "0"), variable("Ticks", "0")};
    tool::State ticking;
    ticking.name = "InitialState";
    ticking.responses = {{"true", "Ticks := Ticks + 1"}};
    paced.states = {ticking};
    auto eventDriven = paced;
    eventDriven.variables[0].initially = "-1";
    tool::Machine fast(std::move(paced));
    tool::Machine driven(std::move(eventDriven));
    started(fast);
    started(driven);
    require(fast.advance(1) && fast.advance(1) && number(fast, "Ticks") == 2 && driven.advance(1000)
            && number(driven, "Ticks") == 0,
        "an interval of 0 should step as fast as it can, a negative one only on changes");
}

// A variable tied to an Event Value writes it when it
// changes, or reads it, and self postAllEvents sends them all; a number set
// to a constrained variable is rounded to its grid, then clipped to its
// range.
void testEventValuesAndConstraints()
{
    tool::Definition definition;
    auto exFader = variable("ExFader", "0.25", true);
    exFader.eventValue = "TimeJitter";
    exFader.access = tool::EventAccess::WriteOnly;
    auto listen = variable("Level", "0", true);
    listen.eventValue = "Level";
    listen.access = tool::EventAccess::ReadOnly;
    // None disconnects the variable from its Event Value for now.
    auto unplugged = variable("Unplugged", "0", true);
    unplugged.eventValue = "Pitch";
    unplugged.access = tool::EventAccess::None;
    auto snapped = variable("Snapped", "0");
    snapped.minimum = 0.0;
    snapped.maximum = 1.0;
    snapped.grid = 0.25;
    definition.variables = {exFader, listen, unplugged, snapped, variable("Heard", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onEntry", "self postAllEvents"}, {"true", "Heard := Level"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.eventWrites().size() == 1 && machine.eventWrites().back().name == "TimeJitter"
            && machine.eventWrites().back().value == 0.25,
        "self postAllEvents should send the Event Values the variables write");
    require(machine.set("ExFader", st::Value::of(0.5)) && machine.eventWrites().back().value == 0.5,
        "a change to ExFader should be written to !TimeJitter");
    require(machine.set("Unplugged", st::Value::of(0.5)) && machine.eventWrites().back().name == "TimeJitter",
        "a variable whose access is None should not write its Event Value");
    require(machine.readEventValue("Level", 0.75) && number(machine, "Heard") == 0.75,
        "a variable reading an Event Value should take its value and step");
    require(machine.set("Snapped", st::Value::of(0.6)) && number(machine, "Snapped") == 0.5
            && machine.set("Snapped", st::Value::of(3.0)) && number(machine, "Snapped") == 1.0,
        "a number should be rounded to the grid, then clipped to the range");
}

// A Tool variable answers hasChanged, isOn, isOff,
// turnedOn and turnedOff against its value at the last step, and the Tool
// posts an Event Value as the signal processor uses it or raw, from 0 to 1.
void testVariableTestsAndPostedEvents()
{
    tool::Definition definition;
    definition.variables = {variable("Button", "0", true), variable("Ons", "0"), variable("Offs", "0"),
        variable("Changes", "0"), variable("Held", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"Button turnedOn", "Ons := Ons + 1"}, {"Button turnedOff", "Offs := Offs + 1"},
        {"Button hasChanged", "Changes := Changes + 1"}, {"Button isOn & Button isOff not", "Held := Held + 1"},
        {"true", "self postEventValueNamed: 'Level' dspValue: 0.5. self postEventValueNamed: #Level rawValue: 0.25"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.set("Button", st::Value::of(1.0)) && number(machine, "Ons") == 1 && number(machine, "Changes") == 1
            && number(machine, "Held") == 1,
        "a button pressed should have turned on and changed");
    require(machine.set("Button", st::Value::of(1.0)) && number(machine, "Ons") == 1 && number(machine, "Changes") == 1
            && number(machine, "Held") == 2,
        "a button held should be on, but neither turned on nor changed");
    require(machine.set("Button", st::Value::of(0.0)) && number(machine, "Offs") == 1 && number(machine, "Changes") == 2
            && number(machine, "Held") == 2,
        "a button released should have turned off and changed");
    const auto& writes = machine.eventWrites();
    require(writes.size() >= 2 && writes[writes.size() - 2].name == "Level" && writes[writes.size() - 2].value == 0.5
            && !writes[writes.size() - 2].raw && writes.back().value == 0.25 && writes.back().raw,
        "postEventValueNamed:dspValue: and rawValue: should post the Event Value");
}

// The Tool that turns the cursor into Event Values:
// InitialState keeps the last twenty points and a block that adds one and
// maps it to X and Y in (-1,1); each step of ActiveState runs the block,
// then sets distance and radius to the point's r, panner and theta from its
// angle, and x and y in (0,1), each written to its Event Value. Here the
// cursor, from 0 at the window's left and top to 1, is two variables the
// test sets.
void testCursorTurnsIntoEventValues()
{
    tool::Definition definition;
    definition.variables = {variable("StepIntervalInMilliseconds", "10"), variable("CursorX", "0.5"),
        variable("CursorY", "0.5"), variable("X", "0"), variable("Y", "0"), variable("PointCollection", "nil"),
        variable("UpdateXY", "nil")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true",
        "PointCollection := OrderedCollection new. 20 timesRepeat: [PointCollection add: 0@0].\n"
        "UpdateXY := [:pt | PointCollection removeLast. PointCollection addFirst: pt.\n"
        "X := pt x clipTo01 * 2 - 1. Y := 1 - (pt y clipTo01) * 2 - 1].\n#ActiveState"}};
    tool::State active;
    active.name = "ActiveState";
    for (const auto& [name, event] : std::vector<std::pair<std::string, std::string>>{{"distance", "Distance"},
             {"radius", "Radius"}, {"panner", "Panner"}, {"theta", "Angle"}, {"x", "x"}, {"y", "y"}})
    {
        auto local = variable(name, "0");
        local.eventValue = event;
        local.access = tool::EventAccess::WriteOnly;
        active.variables.push_back(std::move(local));
    }
    active.responses = {{"true",
        "UpdateXY value: CursorX @ CursorY.\n"
        "distance := radius := (X@Y) r.\n"
        "panner := theta := 2 - ((X@Y) theta / Float pi) - 1.\n"
        "x := X * 0.5 + 0.5. y := Y * 0.5 + 0.5.\n#ActiveState"}};
    definition.states = {initial, active};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.advance(10) && machine.state() == "ActiveState", "InitialState should go on to ActiveState");
    const auto written = [&](std::string_view name) {
        auto value = -99.0;
        for (const auto& write : machine.eventWrites())
        {
            if (write.name == name)
                value = write.value;
        }
        return value;
    };
    require(machine.advance(10), "the Tool should step: " + machine.error());
    require(number(machine, "X") == 0 && number(machine, "Y") == 0 && written("x") == 0.5 && written("y") == 0.5
            && written("Distance") == 0,
        "the window's centre should be x and y 0.5, at distance 0");
    // Right of the centre and above it, at 45 degrees: X and Y 0.5.
    require(machine.set("CursorX", st::Value::of(0.75)) && machine.set("CursorY", st::Value::of(0.25))
            && machine.advance(10),
        "the Tool should step: " + machine.error());
    require(number(machine, "X") == 0.5 && number(machine, "Y") == 0.5 && written("x") == 0.75 && written("y") == 0.75
            && std::abs(written("Distance") - std::sqrt(0.5)) < 1.0e-12 && std::abs(written("Radius") - std::sqrt(0.5)) < 1.0e-12
            && std::abs(written("Panner") - 0.75) < 1.0e-12 && std::abs(written("Angle") - 0.75) < 1.0e-12,
        "a cursor up and to the right should give its distance and its pan from the front");
    // Outside the window the point is clipped to its edge.
    require(machine.set("CursorX", st::Value::of(2.0)) && machine.advance(10) && number(machine, "X") == 1.0,
        "a cursor outside the window should be clipped to (0,1)");
}
// A Text file variable logs the time, a tab, a
// label and a value's printString, and a carriage return, each entry on a
// line of its own; Append writes after what the file held, and onEntry logs
// the date and time.
void testTextFilesLogEntries()
{
    tool::Definition definition;
    definition.variables = {variable("TriggerButton", "0", true)};
    tool::State initial;
    initial.name = "InitialState";
    initial.variables = {kinded("logFile", tool::Kind::TextFile, "widgetSamplerLog.txt", tool::FileAccess::Append)};
    initial.responses = {{"onEntry", "logFile nextPutAll: 'Enter:'. logFile tab.\n"
                                     "Time dateAndTimeNow printOn: logFile. logFile cr."},
        {"TriggerButton turnedOn", "Time now printOn: logFile.\nlogFile tab.\n"
                                   "logFile nextPutAll: 'TriggerButton = ', TriggerButton printString.\nlogFile cr."}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setTextFile("widgetSamplerLog.txt", "Earlier\n");
    started(machine);
    require(machine.set("TriggerButton", st::Value::of(1.0)), "the Tool should step: " + machine.error());
    const auto log = machine.textFile("widgetSamplerLog.txt").value_or("");
    const std::regex entries{"Earlier\n"
                             "Enter:\t\\((January|February|March|April|May|June|July|August|September|October|November|"
                             "December) [0-9]{1,2}, [0-9]{4} [0-9]{1,2}:[0-9]{2}:[0-9]{2} [ap]m \\)\n"
                             "[0-9]{1,2}:[0-9]{2}:[0-9]{2} [ap]m\tTriggerButton = 1\n"};
    require(std::regex_match(log, entries), "the log should hold its entries, a line each: " + log);
}

// A text file read to a character, through it, a character
// or n of them at a time, to its end and a character at a time with do:;
// written a character, a string, spaces and tabs; and opened only for
// reading, only for writing, appending after its contents, reading them and
// appending, or emptied to be written and read.
void testTextFilesReadAndWrite()
{
    tool::Definition definition;
    definition.variables = {kinded("Source", tool::Kind::TextFile, "words.txt"),
        kinded("Notes", tool::Kind::TextFile, "notes.txt", tool::FileAccess::ReadAppend),
        kinded("Scratch", tool::Kind::TextFile, "scratch.txt", tool::FileAccess::ReadWrite),
        kinded("Out", tool::Kind::TextFile, "out.txt", tool::FileAccess::WriteOnly), variable("Results", "nil"),
        variable("Count", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true",
        "| upTo next nextThree through rest atEnd contents notes scratch |\n"
        "upTo := Source upTo: $,.\n"
        "next := Source next.\n"
        "nextThree := Source next: 3.\n"
        "Source skipThrough: $g.\n"
        "through := Source through: $m.\n"
        "rest := Source upToEnd.\n"
        "atEnd := Source atEnd.\n"
        "contents := Source contents.\n"
        "Source reset. Source do: [:c | c = $a ifTrue: [Count := Count + 1]].\n"
        "Notes nextPutAll: 'and more'; nextPut: $!.\n"
        "notes := Notes upToEnd.\n"
        "Scratch nextPutAll: 'x'; space: 2; tab: 2; nextPutAll: 'y'; space; tab.\n"
        "scratch := Scratch upToEnd.\n"
        "Out nextPutAll: 'written'; cr.\n"
        "Results := {upTo. next. nextThree. through. rest. atEnd. contents. notes. scratch}.\n"
        "#TerminalState"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setTextFile("words.txt", "alpha,beta gamma");
    machine.setTextFile("notes.txt", "kept ");
    machine.setTextFile("scratch.txt", "gone");
    machine.setTextFile("out.txt", "overwritten");
    started(machine);
    require(machine.step() && machine.stopped(), "the Tool should run its response: " + machine.error());
    const auto results = machine.variable("Results");
    require(results.has_value() && results->kind == st::Value::Kind::Array && results->elements->size() == 9,
        "the results should be an Array");
    const std::vector<std::string> texts{"alpha", "b", "eta", "am", "ma"};
    for (std::size_t index = 0; index < texts.size(); ++index)
    {
        require((*results->elements)[index].text == texts[index],
            "reading " + texts[index] + " should answer it, not " + (*results->elements)[index].text);
    }
    require((*results->elements)[5].kind == st::Value::Kind::Boolean && (*results->elements)[5].boolean
            && (*results->elements)[6].text == "alpha,beta gamma" && number(machine, "Count") == 5.0,
        "atEnd, contents and do: should answer the end, the whole file and each character");
    require((*results->elements)[7].text == "kept and more!" && machine.textFile("notes.txt") == "kept and more!",
        "Read/Append should read what the file held and write after it");
    require((*results->elements)[8].text == "x  \t\ty \t" && machine.textFile("scratch.txt") == "x  \t\ty \t",
        "Read/Write should empty the file, and space: and tab: write that many");
    require(machine.textFile("out.txt") == "written\n", "Write only should write over the file");

    const auto refuses = [](tool::Variable file, std::string response, std::string_view why, std::string_view message) {
        tool::Definition refusing;
        refusing.variables = {std::move(file)};
        tool::State state;
        state.name = "InitialState";
        state.responses = {{"true", std::move(response)}};
        refusing.states = {state};
        tool::Machine machine(std::move(refusing));
        machine.setTextFile("there.txt", "text");
        const auto ran = machine.start() && machine.step();
        require(!ran && machine.error().find(why) != std::string::npos, std::string{message} + ": " + machine.error());
    };
    refuses(kinded("In", tool::Kind::TextFile, "there.txt"), "In nextPutAll: 'x'", "for reading only",
        "a Read only file should not be written");
    refuses(kinded("Log", tool::Kind::TextFile, "there.txt", tool::FileAccess::Append), "Log next", "for writing only",
        "an Append file should not be read");
    refuses(kinded("In", tool::Kind::TextFile, "missing.txt"), "In next", "not there",
        "a Read only file that is not there should not open");
}

// A Random number stream variable, seeded from 0 to 65535
// in Initially, answers next from (0,1); the same seed the same numbers, and
// a state entered again starts its Random over.
void testRandomNumberVariables()
{
    const auto draws = [](std::string seed) {
        tool::Definition definition;
        definition.variables = {variable("First", "nil"), variable("Again", "nil"), variable("Second", "nil")};
        tool::State initial;
        initial.name = "InitialState";
        initial.variables = {kinded("rando", tool::Kind::RandomNumbers, std::move(seed))};
        initial.responses = {{"onEntry", "First isNil ifTrue: [First := rando next. Second := rando next] "
                                         "ifFalse: [Again := rando next]"},
            {"true", "#Elsewhere"}};
        tool::State elsewhere;
        elsewhere.name = "Elsewhere";
        elsewhere.responses = {{"true", "#InitialState"}};
        definition.states = {initial, elsewhere};
        tool::Machine machine(std::move(definition));
        started(machine);
        require(machine.step() && machine.step(), "the Tool should step: " + machine.error());
        return std::vector{number(machine, "First"), number(machine, "Second"), number(machine, "Again")};
    };
    const auto seven = draws("7");
    require(seven[0] > 0.0 && seven[0] < 1.0 && seven[1] > 0.0 && seven[1] < 1.0 && seven[0] != seven[1],
        "rando next should answer numbers in (0,1)");
    require(draws("7") == seven && draws("8")[0] != seven[0],
        "the seed should set the numbers the Random answers");
    require(seven[2] == seven[0], "a state entered again should start its seeded Random over");
    require(draws("")[0] > 0.0, "an empty seed should give a Random too");
    tool::Definition refusing;
    refusing.variables = {kinded("rando", tool::Kind::RandomNumbers, "65536")};
    refusing.states = {tool::State{"InitialState", {}, {}}};
    tool::Machine machine(std::move(refusing));
    require(!machine.start() && machine.error().find("0 to 65535") != std::string::npos,
        "a seed above 65535 should be refused: " + machine.error());
}

// In a Tool a SamplesFile of the project's plays its
// recording at the current sample rate, asking the environment to play the
// file; a Script compiling has nothing to play it.
void testASamplesFilePlaysItsRecording()
{
    tool::Definition definition;
    definition.variables = {variable("Exit", "0", true)};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true", "('bell.aif' asSampleFile) play"}, {"Exit turnedOn", "#TerminalState"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setSampleFiles([](std::string_view name) -> std::optional<st::SampleFileInfo> {
        if (name != "bell.aif")
            return std::nullopt;
        st::SampleFileInfo info;
        info.name = "Samples/bell.aif";
        info.frames = 24000.0;
        info.sampleRate = 48000.0;
        return info;
    });
    started(machine);
    require(machine.step() && !machine.soundRequests().empty() && machine.soundRequests().back().sampleFile == "Samples/bell.aif",
        "play should ask for the sample file to be played: " + machine.error());
    struct Compiling final : st::Host
    {
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "none";
            return std::nullopt;
        }
        std::optional<st::SampleFileInfo> sampleFile(std::string_view) override
        {
            st::SampleFileInfo info;
            info.name = "Samples/bell.aif";
            return info;
        }
    } compiling;
    const auto parsed = st::parse("'bell.aif' asSampleFile play");
    require(parsed.ok() && !st::run(*parsed.program, compiling).ok(), "a host that cannot play should refuse play");
}

// A Compiled Sound Grid plays a Sound by its number or
// its name, or compiles one with its free variables bound by a BindingList
// of Associations; SignalProcessor stop stops the Sound playing.
void testCompiledSoundGridsPlaySounds()
{
    tool::Definition definition;
    definition.variables = {kinded("someSounds", tool::Kind::CompiledSoundGrid, "widgetSounds.grid"),
        variable("Exit", "0", true)};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onExit", "SignalProcessor stop"},
        {"true", "someSounds playSoundNumber: 3.\n"
                 "someSounds playSoundNamed: 'SampleCloud'.\n"
                 "someSounds playSoundNamed: 'Matrix4-fixed' withSubstitutions: (BindingList new).\n"
                 "someSounds playSoundNamed: 'garbleChop' withSubstitutions:\n"
                 "    (BindingList with: (?sample1 -> 'alien threat.aif') with: (?sample2 -> 'siren.aif'))"},
        {"Exit turnedOn", "#TerminalState"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.step(), "the Tool should play its Sounds: " + machine.error());
    const auto& played = machine.soundRequests();
    require(played.size() == 4 && played[0].grid == "widgetSounds.grid" && played[0].number == 3 && !played[0].compile
            && played[1].name == "SampleCloud" && !played[1].compile,
        "playSoundNumber: and playSoundNamed: should play the grid's compiled Sounds");
    require(played[2].name == "Matrix4-fixed" && played[2].compile && played[2].bindings.empty(),
        "withSubstitutions: (BindingList new) should compile a Sound without free variables");
    require(played[3].compile && played[3].bindings.size() == 2 && played[3].bindings[0].first == "sample1"
            && played[3].bindings[0].second.text == "alien threat.aif" && played[3].bindings[1].first == "sample2"
            && played[3].bindings[1].second.text == "siren.aif",
        "a BindingList should bind ?sample1 and ?sample2 to their files");
    require(machine.set("Exit", st::Value::of(1.0)) && machine.stopped() && machine.soundRequests().back().stop,
        "onExit's SignalProcessor stop should stop the Sound");
    // Set Value Options... locates the grid's file with the
    // file dialog, its name becoming the variable's Initially.
    require(tool::valueOptionsFiles(tool::Kind::CompiledSoundGrid) == std::optional<std::string_view>{"*.prg"}
            && tool::valueOptionsFiles(tool::Kind::TextFile) == std::optional<std::string_view>{"*.txt"}
            && !tool::valueOptionsFiles(tool::Kind::Number).has_value()
            && tool::initiallyNamingFile("/grids/Set A.prg") == "Set A.prg",
        "Set Value Options... should locate the Compiled Sound Grid's file");
}

// A Compiled Sound Grid's play messages answer true once
// the Sound is asked for, by number or name, with substitutions or without.
void testGridPlayMessagesAnswerTrue()
{
    tool::Definition definition;
    definition.variables = {kinded("someSounds", tool::Kind::CompiledSoundGrid, "widgetSounds.grid"),
        variable("ByNumber", "nil"), variable("ByName", "nil"), variable("Bound", "nil"), variable("BoundByName", "nil")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true", "ByNumber := someSounds playSoundNumber: 2.\n"
                                  "ByName := someSounds playSoundNamed: 'SampleCloud'.\n"
                                  "Bound := someSounds playSoundNumber: 4 withSubstitutions: (BindingList new).\n"
                                  "BoundByName := someSounds playSoundNamed: 'garbleChop' withSubstitutions: (BindingList new)"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.step(), "the Tool should play its Sounds: " + machine.error());
    const auto answeredTrue = [&](std::string_view name) {
        const auto value = machine.variable(name);
        return value.has_value() && value->kind == st::Value::Kind::Boolean && value->boolean;
    };
    require(answeredTrue("ByNumber") && answeredTrue("ByName") && answeredTrue("Bound") && answeredTrue("BoundByName"),
        "each play message should return true when it succeeds");
    const auto& played = machine.soundRequests();
    require(played.size() == 4 && played[2].number == 4 && played[2].compile && played[3].name == "garbleChop",
        "playSoundNumber:withSubstitutions: should play the grid square's Sound with its bindings");
}

// A block kept in a global variable runs from any
// response, as UpdatePointQueue value: pt adds a Point to the front of
// PointCollection and drops its oldest; a Tool that makes a Point each
// millisecond goes on making them.
void testBlocksInVariablesAndManyPoints()
{
    tool::Definition definition;
    definition.variables = {variable("StepIntervalInMilliseconds", "0"),
        variable("PointCollection", "OrderedCollection new add: 1 @ 1; add: 2 @ 2; yourself"),
        variable("UpdatePointQueue", "nil"), variable("Steps", "0"), variable("Newest", "0"), variable("Oldest", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onEntry", "UpdatePointQueue := [ :newPt | PointCollection removeLast.\n"
                                     "PointCollection addFirst: newPt]"},
        {"true", "Steps := Steps + 1. Steps odd ifTrue: [UpdatePointQueue value: Steps @ 0] ifFalse: [\n"
                 "UpdatePointQueue value: Steps @ 0].\n"
                 "Newest := PointCollection first x. Oldest := PointCollection last x"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.advance(1) && number(machine, "Newest") == 1 && number(machine, "Oldest") == 1,
        "the queue should hold the new Point and drop its oldest");
    for (auto step = 0; step < 70'000; ++step)
        require(machine.advance(1), "the Tool should go on making Points: " + machine.error());
    require(number(machine, "Newest") == 70'001 && number(machine, "Oldest") == 70'000,
        "the queue should hold the last two Points of 70,001 steps");
}
// A variable represents a Number, as a button's 1
// or 0, which steps the Tool when Step When Changed is checked; a String,
// whose value the code tests to set a number; or any Smalltalk Object, as an
// Array, a Point or a Symbol.
void testVariableKinds()
{
    tool::Definition definition;
    auto sticky = kinded("StickyButton", tool::Kind::Number, "0");
    sticky.stepWhenChanged = true;
    auto choice = kinded("Choice", tool::Kind::String, "'BluePill'");
    choice.stepWhenChanged = true;
    definition.variables = {sticky, choice, kinded("Level", tool::Kind::Number, "0"), variable("Presses", "0"),
        kinded("Things", tool::Kind::Object, "#(1 2 3)"), kinded("Where", tool::Kind::Object, "3 @ 4"),
        kinded("Mood", tool::Kind::Object, "#calm")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"StickyButton turnedOn", "Presses := Presses + 1"}, {"Choice = 'RedPill'", "Level := 1"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.set("StickyButton", st::Value::of(1.0)) && number(machine, "Presses") == 1,
        "a Number with Step When Changed should step the Tool as it changes");
    require(machine.set("Choice", st::Value::string("RedPill")) && number(machine, "Level") == 1,
        "the code should test a String's value to set a number");
    const auto things = machine.variable("Things");
    const auto where = machine.variable("Where");
    const auto mood = machine.variable("Mood");
    require(things->kind == st::Value::Kind::Array && things->elements->size() == 3
            && where->kind == st::Value::Kind::Object && where->elements->size() == 2
            && (*where->elements)[0].number == 3 && mood->kind == st::Value::Kind::Symbol && mood->text == "calm",
        "an Object variable should hold an Array, a Point or a Symbol");
}

// MaximumWindowExtent and MinimumWindowExtent bound the
// Tool's size, which opens halfway between them; and onEntry and
// onExit, if present, come first among a state's Triggered Responses.
void testWindowExtentsAndSpecialConditions()
{
    tool::Definition definition;
    definition.variables = {variable("MinimumWindowExtent", "Point x: 100 y: 100"),
        variable("MaximumWindowExtent", "800 @ 600")};
    definition.states = {tool::State{"InitialState", {}, {}}};
    tool::Machine machine(std::move(definition));
    started(machine);
    require(machine.openingExtent() == std::pair{450.0, 350.0},
        "the Tool should open halfway between its minimum and maximum extents");
    require(!machine.fixedExtent(), "different extents should leave the Tool's window resizable");
    tool::Definition fixed;
    fixed.variables = {variable("MinimumWindowExtent", "300 @ 200"), variable("MaximumWindowExtent", "Point x: 300 y: 200")};
    fixed.states = {tool::State{"InitialState", {}, {}}};
    tool::Machine pinned(std::move(fixed));
    started(pinned);
    require(pinned.fixedExtent() && pinned.openingExtent() == std::pair{300.0, 200.0},
        "equal minimum and maximum extents should make a window that cannot be resized");
    tool::Definition gridded;
    gridded.variables = {variable("LayoutGrid", "10"), variable("Other", "0")};
    gridded.states = {tool::State{"InitialState", {}, {}}};
    require(tool::layoutGridOf(gridded) == std::pair{10.0, 10.0},
        "a LayoutGrid number should space the Layout's grid both ways");
    gridded.variables.front().initially = "8 @ 12";
    require(tool::layoutGridOf(gridded) == std::pair{8.0, 12.0}, "a LayoutGrid Point should space the grid by its x and y");
    gridded.variables.front().initially = "0";
    require(!tool::layoutGridOf(gridded).has_value(), "a LayoutGrid of 0 should draw no grid");
    tool::Definition unbounded;
    unbounded.states = {tool::State{"InitialState", {}, {}}};
    tool::Machine free(std::move(unbounded));
    started(free);
    require(!free.openingExtent().has_value(), "a Tool without extents should open at the environment's size");
    tool::Definition late;
    late.states = {tool::State{"InitialState", {}, {{"true", "3"}, {"onEntry", "4"}}}};
    tool::Machine refusing(std::move(late));
    require(!refusing.start() && refusing.error().find("come first") != std::string::npos,
        "an onEntry after another condition should be refused: " + refusing.error());
}
// A View variable holds View new, whose bounds are its
// rectangle in the window and whose graphicsContext clears, paints with a
// ColorValue, sets the line's width, and draws a line, a string, filled and
// stroked rectangles, a dot, an arc, a polygon and a Polyline.
void testViewsDraw()
{
    tool::Definition definition;
    definition.variables = {kinded("AView", tool::Kind::View, ""), variable("TriggerButton", "0", true),
        variable("Width", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.variables = {variable("graphRectangle", "nil"), variable("graph", "nil")};
    initial.responses = {{"onEntry", "AView := View new"},
        {"TriggerButton turnedOn",
            "graphRectangle := AView bounds. graph := AView graphicsContext.\n"
            "Width := graphRectangle width.\n"
            "graph clear. graph paint: ColorValue purple.\n"
            "graph displayLineFrom: (graphRectangle origin) to: (graphRectangle corner).\n"
            "graph lineWidth: 3.\n"
            "graph displayString: 'Hello' at: 20 @ 30.\n"
            "graph displayRectangle: (10 @ 10 corner: 40 @ 50).\n"
            "(10 @ 10 extent: 5 @ 5) displayStrokedOn: graph.\n"
            "graph displayDotOfDiameter: 8 at: 60 @ 60.\n"
            "graph displayArcBoundedBy: (0 @ 0 corner: 100 @ 100) startAngle: 0 sweepAngle: -90 at: 5 @ 0.\n"
            "graph paint: (ColorValue red: 1 green: 0.5 blue: 0).\n"
            "graph displayPolygon: {0 @ 0. 50 @ 0. 25 @ 40. 0 @ 0}.\n"
            "(Polyline vertices: {0 @ 0. 10 @ 10. 20 @ 0}) displayStrokedOn: graph.\n"
            "graph flush"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setViewBounds("AView", 100, 50, 200, 150);
    started(machine);
    require(machine.drawings("AView").empty(), "a new View should hold no drawings");
    require(machine.set("TriggerButton", st::Value::of(1.0)), "the Tool should draw: " + machine.error());
    using Shape = tool::Drawing::Shape;
    const auto drawn = machine.drawings("AView");
    const std::vector<Shape> shapes{Shape::Line, Shape::String, Shape::Rectangle, Shape::Outline, Shape::Dot, Shape::Arc,
        Shape::Polygon, Shape::Polyline};
    require(drawn.size() == shapes.size() && number(machine, "Width") == 200,
        "the View's bounds should be its rectangle, and each message should draw");
    for (std::size_t index = 0; index < shapes.size(); ++index)
        require(drawn[index].shape == shapes[index], "the drawings should come in order");
    using Points = std::vector<std::pair<double, double>>;
    require(drawn[0].points == Points{{100, 50}, {300, 200}} && drawn[0].colour == std::array{0.5, 0.0, 0.5}
            && drawn[0].lineWidth == 1,
        "a purple diagonal line from the bounds' origin to its corner");
    require(drawn[1].text == "Hello" && drawn[1].points == Points{{20, 30}} && drawn[1].lineWidth == 3,
        "displayString:at: and lineWidth:");
    require(drawn[2].points == Points{{10, 10}, {40, 50}} && drawn[3].points == Points{{10, 10}, {15, 15}},
        "a filled rectangle and one stroked by its border");
    require(drawn[4].diameter == 8 && drawn[4].points == Points{{60, 60}}, "a dot of a diameter");
    require(drawn[5].points == Points{{0, 0}, {100, 100}} && drawn[5].startAngle == 0 && drawn[5].sweepAngle == -90
            && drawn[5].offset == std::pair{5.0, 0.0},
        "an arc's bounds, start, counterclockwise sweep and offset");
    require(drawn[6].points.size() == 4 && drawn[6].points.front() == drawn[6].points.back()
            && drawn[6].colour == std::array{1.0, 0.5, 0.0} && drawn[7].points == Points{{0, 0}, {10, 10}, {20, 0}},
        "a closed polygon and a Polyline's vertices");
    require(machine.set("TriggerButton", st::Value::of(0.0)) && machine.set("TriggerButton", st::Value::of(1.0))
            && machine.drawings("AView").size() == shapes.size(),
        "clear should empty the View before it draws again");
    machine.setViewBounds("AView", 0, 0, 50, 50);
    require(machine.set("TriggerButton", st::Value::of(0.0)) && machine.set("TriggerButton", st::Value::of(1.0))
            && number(machine, "Width") == 50,
        "a resized window should give the View's current bounds");
}
// A Tool asks the signal processor where it is in the
// Sound playing and whether that Sound has ended, pauses and continues it,
// skips it to a time, stops it and replays the last Sound loaded, opens the
// virtual control surface if needed; a Compiled Sound Grid's play answers
// true; and a View takes fly-by help, has a rectangle drawn again, and its
// controller's sensor answers the cursor point.
void testToolsAskTheSignalProcessorAndViews()
{
    tool::Definition definition;
    definition.variables = {kinded("AView", tool::Kind::View, ""), kinded("someSounds", tool::Kind::CompiledSoundGrid, "grid.grid"),
        variable("Now", "0"), variable("Code", "0"), variable("Ended", "false"), variable("Cursor", "nil"),
        variable("Played", "false"), variable("Channels", "0"), variable("Loudest", "0")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onEntry", "AView := View new"},
        {"true", "Now := self currentTimeInSamples. Code := self currentTimeCodeInSeconds. Ended := self atSoundEnd.\n"
                 "self pause. self continue. self skipToTime: 2 s. SignalProcessor stopSound.\n"
                 "SignalProcessor dspStop. SignalProcessor dspReplay.\n"
                 "AView helpString: 'Drag here'. Cursor := AView controller sensor cursorPoint.\n"
                 "AView invalidateRectangle: (0 @ 0 corner: 10 @ 10).\n"
                 "Played := someSounds playSoundNumber: 1. self openControllerSurfaceIfNeeded.\n"
                 "Channels := SignalProcessor displayableAudioChannels. Loudest := SignalProcessor maximumAmplitude"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setViewBounds("AView", 0, 0, 100, 100);
    machine.setCursorPoint("AView", 30, 40);
    started(machine);
    machine.setPlayback({4800.0, 12.5, true, 8});
    require(machine.step(), "the Tool should step: " + machine.error());
    require(number(machine, "Now") == 4800 && number(machine, "Code") == 12.5 && machine.variable("Ended")->boolean,
        "the Tool should hear the signal processor's time, time code and the Sound's end");
    const auto& requested = machine.soundRequests();
    require(requested.size() == 8 && requested[7].openControllerSurface && !requested[7].stop,
        "openControllerSurfaceIfNeeded should ask for the virtual control surface");
    require(number(machine, "Channels") == 8, "displayableAudioChannels should answer the channels the preferences show");
    require(number(machine, "Loudest") == 1, "maximumAmplitude should answer a signal's full scale, 1, an Integer");
    require(requested[0].pause && requested[1].resume
            && requested[2].skipToSeconds == std::optional<double>{2.0} && requested[3].stop && requested[6].number == 1
            && machine.variable("Played")->boolean,
        "pause, continue, skipToTime: and stopSound should reach the signal processor, a play answering true");
    require(requested[4].stop && !requested[4].replay && requested[5].replay && !requested[5].stop,
        "dspStop should stop the Sound and dspReplay replay the last one loaded");
    const auto cursor = machine.variable("Cursor");
    const auto drawn = machine.drawings("AView");
    require(machine.helpString("AView") == "Drag here" && cursor.has_value() && cursor->elements != nullptr
            && cursor->elements->size() == 2 && (*cursor->elements)[0].number == 30 && (*cursor->elements)[1].number == 40
            && !drawn.empty() && drawn.back().shape == tool::Drawing::Shape::Invalidate,
        "helpString:, controller sensor cursorPoint and invalidateRectangle: should reach the View");
}

// A Tool's scripts reach its files by
// Filename too, so an append stream a response writes adds to the file a
// Text file variable reads.
void testScriptsShareTheToolsFiles()
{
    tool::Definition definition;
    definition.variables = {kinded("Data", tool::Kind::TextFile, "data.txt", tool::FileAccess::ReadAppend),
        variable("Size", "0"), variable("Read", "''")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"true", "| s | s := 'data.txt' asFilename appendStream. s nextPutAll: 'x'. s close.\n"
                                  "Size := 'data.txt' asFilename fileSize. Read := Data contents"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setTextFile("data.txt", "");
    started(machine);
    for (auto step = 0; step < 3; ++step)
        require(machine.step(), "the Tool should step: " + machine.error());
    require(machine.textFile("data.txt") == "xxx" && number(machine, "Size") == 3
            && machine.variable("Read")->text == "xxx",
        "a script's Filename and a Text file variable should share the Tool's files");
}

// The four buttons. A Button sets its
// variable to its down value as it is pressed and its up value as it is
// released or the mouse leaves it; a trigger sends both in quick
// succession as it is released; a toggle switches between 0 and 1; a stick
// goes to 1, which only the Tool's own code undoes. The values may be any
// objects, such as Booleans or Strings.
void testTheFourButtons()
{
    const auto button = [](std::string name, tool::WidgetKind kind, std::string down = "1", std::string up = "0") {
        auto declared = variable(std::move(name), "0");
        declared.widget.kind = kind;
        declared.widget.downValue = std::move(down);
        declared.widget.upValue = std::move(up);
        return declared;
    };
    tool::Definition definition;
    definition.variables = {button("Plain", tool::WidgetKind::Button, "'down'", "'up'"),
        button("Trigger", tool::WidgetKind::TriggerButton), button("Toggle", tool::WidgetKind::ToggleButton),
        button("Stick", tool::WidgetKind::StickButton), variable("Presses", "0")};
    // The trigger's two values are seen as they come: each sends a step.
    definition.variables[1].stepWhenChanged = true;
    definition.states.push_back({"InitialState", {}, {{"Trigger = 1", "Presses := Presses + 1"}}});
    tool::Machine machine(definition);
    require(machine.start(), "the buttons' Tool should start: " + machine.error());
    const auto text = [&](std::string_view name) {
        const auto value = machine.variable(name);
        return value.has_value() && value->kind == st::Value::Kind::String ? value->text : std::string{};
    };
    const auto number = [&](std::string_view name) {
        const auto value = machine.variable(name);
        return value.has_value() && value->kind == st::Value::Kind::Number ? value->number : -1.0;
    };
    require(machine.press("Plain", tool::ButtonEvent::Press) && text("Plain") == "down"
            && machine.press("Plain", tool::ButtonEvent::Leave) && text("Plain") == "up",
        "a Button's down value as it is pressed, its up value as the mouse leaves it");
    require(machine.press("Trigger", tool::ButtonEvent::Press) && number("Trigger") == 0.0
            && machine.press("Trigger", tool::ButtonEvent::Release) && number("Trigger") == 0.0 && number("Presses") == 1.0,
        "a trigger sends its down and up values in quick succession as it is released");
    require(machine.press("Toggle", tool::ButtonEvent::Press) && number("Toggle") == 1.0
            && machine.press("Toggle", tool::ButtonEvent::Release) && number("Toggle") == 1.0
            && machine.press("Toggle", tool::ButtonEvent::Press) && number("Toggle") == 0.0,
        "a toggle switches between 0 and 1 on successive clicks");
    require(machine.press("Stick", tool::ButtonEvent::Press) && machine.press("Stick", tool::ButtonEvent::Release)
            && number("Stick") == 1.0 && machine.set("Stick", st::Value::of(0.0)) && number("Stick") == 0.0,
        "a stick goes down and only the program resets it");
    require(!machine.press("Presses", tool::ButtonEvent::Press), "a variable not shown as a button is not pressed");
}

// A Tool saved by its editor and read back, its globals, its
// states' variables and triggered responses and each widget; a new Tool
// begins with an InitialState.
void testToolFiles()
{
    auto tool = tool::newTool();
    require(tool.states.size() == 1 && tool.states.front().name == "InitialState", "a new Tool has its InitialState");
    auto fader = variable("Level", "0.5", true);
    fader.kind = tool::Kind::Number;
    fader.eventValue = "Level";
    fader.access = tool::EventAccess::ReadWrite;
    fader.minimum = 0.0;
    fader.maximum = 1.0;
    fader.grid = 0.01;
    fader.widget.kind = tool::WidgetKind::Fader;
    fader.widget.labelled = true;
    fader.widget.left = 10;
    fader.widget.top = 20;
    fader.widget.digits = 2;
    fader.widget.logTaper = true;
    auto choice = variable("Shape", "'sine'");
    choice.kind = tool::Kind::String;
    choice.widget.kind = tool::WidgetKind::RadioButtons;
    choice.widget.columns = 2;
    choice.widget.strings = {"sine", "saw \"bright\""};
    tool.variables = {fader, choice};
    tool.states.front().variables.push_back(variable("count", "0"));
    tool.states.front().responses.push_back({"onEntry", "count := count + 1.\n#TerminalState"});
    const auto saved = tool::writeToolFile(tool);
    const auto read = tool::readToolFile(saved);
    require(read.ok() && read.tool->variables.size() == 2 && read.tool->variables[0].widget.kind == tool::WidgetKind::Fader
            && read.tool->variables[0].widget.digits == std::optional<int>{2} && read.tool->variables[0].grid == std::optional<double>{0.01}
            && read.tool->variables[0].access == tool::EventAccess::ReadWrite && read.tool->variables[1].widget.strings == choice.widget.strings
            && read.tool->states.front().variables.front().name == "count"
            && read.tool->states.front().responses.front().response == tool.states.front().responses.front().response,
        "the Tool's variables, widgets, states and responses should be saved: " + read.error);
    require(tool::writeToolFile(*read.tool) == saved, "a Tool read back saves the same");
    require(!tool::readToolFile("{\"format\": \"talas.project\"}").ok(), "a file of another kind is refused");
    // BackgroundColor's colour menu, Transparent first,
    // sets the variable's Initially to the colour chosen.
    const auto& colours = tool::colourChoices();
    require(!colours.empty() && colours.front() == "Transparent" && !tool::colourNamed("Transparent").has_value()
            && tool::colourNamed("red") == std::optional<std::uint32_t>{0xffff0000u} && !tool::colourNamed("plaid").has_value()
            && std::ranges::all_of(colours.begin() + 1, colours.end(), [](const std::string& name) { return tool::colourNamed(name).has_value(); }),
        "the colour menu should hold Transparent and colours");
    tool::Definition coloured;
    coloured.variables = {variable("BackgroundColor", tool::colourInitially("blue"))};
    tool::State only;
    only.name = "InitialState";
    coloured.states = {only};
    tool::Machine painted(std::move(coloured));
    require(painted.start() && painted.variable("BackgroundColor").has_value() && painted.variable("BackgroundColor")->text == "blue",
        "a colour chosen from the menu should be BackgroundColor's value");
}
}

// The widgetSampler's InitialState: onEntry keeps the
// number of its sample names less one, makes AView a View and logs the time
// and date; while the BluePill is selected, the variables of two Event
// Values take random values and the StickyButton may come unstuck; onExit
// logs the time and date and stops the Sound.
void testTheWidgetSamplersInitialState()
{
    tool::Definition definition;
    auto sticky = variable("StickyButton", "0");
    sticky.widget.kind = tool::WidgetKind::StickButton;
    auto pills = kinded("ListSelection", tool::Kind::String, "'RedPill'");
    pills.stepWhenChanged = true;
    pills.widget.kind = tool::WidgetKind::RadioButtons;
    pills.widget.strings = {"RedPill", "BluePill"};
    definition.variables = {kinded("AView", tool::Kind::View, ""), sticky, pills, variable("Exit", "0", true)};
    tool::State initial;
    initial.name = "InitialState";
    auto timeIndex = variable("timeIndex", "0");
    timeIndex.eventValue = "TimeIndex";
    timeIndex.access = tool::EventAccess::WriteOnly;
    auto cycles = variable("cycles", "0");
    cycles.eventValue = "Cycles";
    cycles.access = tool::EventAccess::WriteOnly;
    initial.variables = {kinded("rando", tool::Kind::RandomNumbers, "11"),
        kinded("logFile", tool::Kind::TextFile, "widgetSamplerLog.txt", tool::FileAccess::Append), timeIndex, cycles,
        variable("sampleNames", "nil"), variable("nbrSampleNames", "0")};
    initial.responses = {
        {"onEntry", "sampleNames := #('roderick.aif' 'karsten honey pie.aif' 'bkgnd air ambience.aif').\n"
                    "nbrSampleNames := sampleNames size - 1.\n"
                    "AView := View new.\n"
                    "logFile nextPutAll: 'Enter:'. logFile tab. Time dateAndTimeNow printOn: logFile. logFile cr."},
        {"onExit", "logFile nextPutAll: 'Exit:'. logFile tab. Time dateAndTimeNow printOn: logFile. logFile cr.\n"
                   "SignalProcessor stop"},
        {"ListSelection = 'BluePill'", "timeIndex := rando next. cycles := rando next.\n"
                                       "(rando next > 0.9) ifTrue: [StickyButton := 0]"},
        {"Exit turnedOn", "#TerminalState"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setTextFile("widgetSamplerLog.txt", "");
    machine.setViewBounds("AView", 10, 10, 200, 100);
    started(machine);
    require(machine.variable("nbrSampleNames").has_value() && number(machine, "nbrSampleNames") == 2,
        "onEntry should keep the size of sampleNames less one in nbrSampleNames");
    require(machine.variable("AView")->kind == st::Value::Kind::Object && machine.drawings("AView").empty(),
        "onEntry should make the global AView a View");
    require(machine.press("StickyButton", tool::ButtonEvent::Press) && number(machine, "StickyButton") == 1,
        "the StickyButton should stick down");
    require(machine.step() && machine.eventWrites().empty(),
        "nothing should change while the BluePill is not selected");
    require(machine.set("ListSelection", st::Value::string("BluePill")) && machine.eventWrites().size() >= 2,
        "selecting the BluePill should run its response: " + machine.error());
    const auto& writes = machine.eventWrites();
    const auto wrote = [&](std::string_view name) {
        return std::ranges::any_of(writes, [&](const auto& write) { return write.name == name && write.value > 0.0 && write.value < 1.0; });
    };
    require(wrote("TimeIndex") && wrote("Cycles"),
        "timeIndex and cycles should write random values to their Event Values");
    auto steps = 0;
    while (number(machine, "StickyButton") == 1 && steps < 200)
    {
        require(machine.step(), "the Tool should step: " + machine.error());
        ++steps;
    }
    require(number(machine, "StickyButton") == 0,
        "the StickyButton should now and then come unstuck by the code");
    require(machine.set("Exit", st::Value::of(1.0)) && machine.stopped() && !machine.soundRequests().empty()
            && machine.soundRequests().back().stop,
        "onExit should stop the Sound playing");
    const auto log = machine.textFile("widgetSamplerLog.txt").value_or("");
    const std::regex logged{"Enter:\t\\(.+ [0-9]{4} .+\\)\n"
                            "Exit:\t\\(.+ [0-9]{4} .+\\)\n"};
    require(std::regex_match(log, logged), "onEntry and onExit should log the time and date: " + log);
}

// Selecting a state in the States list shows its
// Local Variables, InitialState's rando among them, apart from the Global
// Variables.
void testSelectingAStateShowsItsLocalVariables()
{
    tool::Definition definition;
    definition.variables.push_back({"StickyButton"});
    definition.states.push_back({"InitialState", {{"rando"}}, {}});
    definition.states.push_back({"Playing", {{"count"}, {"step"}}, {}});
    require(tool::editorVariableNames(definition, std::nullopt) == std::vector<std::string>{"StickyButton"},
        "the Global Variables list should hold the globals");
    require(tool::editorVariableNames(definition, 0) == std::vector<std::string>{"rando"}
            && tool::editorVariableNames(definition, 1) == std::vector<std::string>{"count", "step"},
        "selecting InitialState should display its Local Variables, another state its own");
    require(tool::editorVariableNames(definition, 2).empty(), "a state the Tool lacks has no variables to show");
}

// On each step the machine tests every condition,
// runs the first block whose condition is true and then updates its state.
// The default rule runs every true condition's response instead; a Tool
// may choose the first-true rule, saved in its file, and a file without
// one keeps the default.
void testTheFirstTrueResponseRule()
{
    const auto make = [](tool::StepRule rule) {
        tool::Definition definition;
        definition.stepRule = rule;
        definition.variables = {variable("Go", "0", true), variable("Count", "0"), variable("Other", "0"),
            variable("Tested", "0")};
        tool::State initial;
        initial.name = "InitialState";
        initial.responses = {{"Go = 1", "#Next"}, {"true", "Count := Count + 1"}, {"true", "Other := Other + 1"},
            {"(Tested := Tested + 1) > 100", "Count := 1000"}};
        tool::State next;
        next.name = "Next";
        definition.states = {initial, next};
        return definition;
    };
    tool::Machine first(make(tool::StepRule::FirstTrueResponse));
    started(first);
    require(first.step() && number(first, "Count") == 1 && number(first, "Other") == 0,
        "a step should run only the first block whose condition is true");
    require(number(first, "Tested") == 1,
        "a step should still test every condition, those after the first true one too");
    require(first.set("Go", st::Value::of(1.0)) && first.state() == "Next" && number(first, "Count") == 1
            && number(first, "Other") == 0 && number(first, "Tested") == 2,
        "the first true block naming a state should update the machine's state");
    tool::Machine every(make(tool::StepRule::EveryTrueResponse));
    started(every);
    require(every.step() && number(every, "Count") == 1 && number(every, "Other") == 1,
        "under the default rule every true condition's response should run");
    // The rule is the Tool's, kept in its file; older files have none.
    const auto saved = tool::writeToolFile(make(tool::StepRule::FirstTrueResponse));
    const auto read = tool::readToolFile(saved);
    require(saved.find("\"stepRule\": \"firstTrueResponse\"") != std::string::npos && read.ok()
            && read.tool->stepRule == tool::StepRule::FirstTrueResponse,
        "the first-true-response rule should be saved with the Tool: " + read.error);
    const auto older = tool::writeToolFile(make(tool::StepRule::EveryTrueResponse));
    require(older.find("stepRule") == std::string::npos
            && tool::readToolFile(older).tool->stepRule == tool::StepRule::EveryTrueResponse,
        "a Tool file without a rule should keep every true response");
    auto bad = saved;
    bad.replace(bad.find("firstTrueResponse"), std::string_view{"firstTrueResponse"}.size(), "someResponse");
    require(!tool::readToolFile(bad).ok(), "an unknown step rule should be refused");
}

// A View variable marked Step When Changed and
// initialized in InitialState's onEntry steps the machine as it is set, so
// the code after the initialization is skipped. Setting it to the View it
// holds is no change, and a View that does not step the machine leaves the
// rest of the code to run.
void testInitializingAViewStepsTheMachine()
{
    tool::Definition definition;
    auto area = kinded("ActiveArea", tool::Kind::View, "");
    area.stepWhenChanged = true;
    definition.variables = {area, variable("Steps", "0"), variable("After", "false"), variable("Kept", "false")};
    tool::State initial;
    initial.name = "InitialState";
    initial.responses = {{"onEntry", "ActiveArea := View new. After := true"},
        {"true", "Steps := Steps + 1. ActiveArea := ActiveArea. Kept := true"}};
    definition.states = {initial};
    tool::Machine machine(std::move(definition));
    machine.setViewBounds("ActiveArea", 0, 0, 200, 100);
    started(machine);
    require(machine.variable("ActiveArea")->kind == st::Value::Kind::Object,
        "onEntry should initialize the View variable");
    require(!machine.variable("After")->boolean,
        "the code after the View's initialization should be skipped");
    require(number(machine, "Steps") == 1,
        "initializing the View variable should step the state machine");
    require(machine.variable("Kept")->boolean,
        "a View set to the View it already holds is no change, and the code after it runs");
    require(machine.step() && number(machine, "Steps") == 2, "the machine should step as usual afterwards");
    tool::Definition still;
    still.variables = {kinded("Plain", tool::Kind::View, ""), variable("Steps", "0"), variable("After", "false")};
    tool::State plain;
    plain.name = "InitialState";
    plain.responses = {{"onEntry", "Plain := View new. After := true"}, {"true", "Steps := Steps + 1"}};
    still.states = {plain};
    tool::Machine quiet(std::move(still));
    started(quiet);
    require(quiet.variable("After")->boolean && number(quiet, "Steps") == 0,
        "a View without Step When Changed should leave the rest of onEntry to run, and not step");
}

// StickyButton opens in the Variable Settings when
// its name is double-clicked in the Global Variables list, or selected
// there and Edit clicked; Edit with nothing selected opens nothing.
void testEditingAVariableFromItsList()
{
    tool::Definition definition;
    definition.variables.push_back({"StickyButton"});
    definition.variables.push_back({"Exit"});
    definition.states.push_back({"InitialState", {{"rando"}}, {}});
    require(tool::variableToEdit(definition, std::nullopt, 0) == std::optional<std::string>{"StickyButton"},
        "double-clicking StickyButton's name, or selecting it and clicking Edit, should edit StickyButton");
    require(tool::variableToEdit(definition, std::nullopt, 1) == std::optional<std::string>{"Exit"}
            && tool::variableToEdit(definition, 0, 0) == std::optional<std::string>{"rando"},
        "the row chosen in the Global or Local Variables list should name the variable edited");
    require(!tool::variableToEdit(definition, std::nullopt, -1).has_value()
            && !tool::variableToEdit(definition, std::nullopt, 2).has_value()
            && !tool::variableToEdit(definition, 3, 0).has_value(),
        "Edit with no row selected, or a row or state that is not there, should edit nothing");
}

int main()
{
    try
    {
        testBozoTurnsByHisState();
        testASamplesFilePlaysItsRecording();
        testTriggeredResponsesRunInOrder();
        testVariablesAndTheMachinesPace();
        testEventValuesAndConstraints();
        testCursorTurnsIntoEventValues();
        testVariableTestsAndPostedEvents();
        testTextFilesLogEntries();
        testTextFilesReadAndWrite();
        testRandomNumberVariables();
        testCompiledSoundGridsPlaySounds();
        testGridPlayMessagesAnswerTrue();
        testBlocksInVariablesAndManyPoints();
        testVariableKinds();
        testWindowExtentsAndSpecialConditions();
        testViewsDraw();
        testScriptsShareTheToolsFiles();
        testToolsAskTheSignalProcessorAndViews();
        testTheFourButtons();
        testToolFiles();
        testTheWidgetSamplersInitialState();
        testSelectingAStateShowsItsLocalVariables();
        testTheFirstTrueResponseRule();
        testInitializingAViewStepsTheMachine();
        testEditingAVariableFromItsList();
        std::cout << "Tool tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
