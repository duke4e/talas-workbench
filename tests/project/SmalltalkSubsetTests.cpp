#include "talas/project/SmalltalkSubset.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace st = talas::project::smalltalk;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// A host with one object, snd, holding named parameters, as a
// ParameterTransformer's.
class SoundHost final : public st::Host
{
public:
    std::map<std::string, st::Value> parameters;
    std::vector<std::string> sent;

    std::optional<st::Value> variable(std::string_view name) override
    {
        if (name == "snd")
            return st::Value::object(0);
        if (name == "time")
            return st::Value::of(250'000.0);
        if (name == "totalDuration")
            return st::Value::of(1'000'000.0);
        return std::nullopt;
    }

    std::optional<st::Value> send(
        const st::Value& receiver,
        std::string_view selector,
        std::span<const st::Value> arguments,
        std::string& error) override
    {
        sent.emplace_back(selector);
        if (receiver.index != 0)
        {
            error = "unknown object";
            return std::nullopt;
        }
        if (selector.ends_with(':') && arguments.size() == 1)
        {
            parameters[std::string{selector.substr(0, selector.size() - 1)}] = arguments[0];
            return receiver;
        }
        if (arguments.empty())
        {
            const auto found = parameters.find(std::string{selector});
            return found == parameters.end() ? st::Value::nil() : found->second;
        }
        error = "snd does not understand " + std::string{selector};
        return std::nullopt;
    }
};

st::RunResult runOn(SoundHost& host, std::string_view source)
{
    const auto parsed = st::parse(source);
    require(parsed.ok(), "should parse: " + std::string{source}
            + (parsed.diagnostic ? ": " + parsed.diagnostic->message : std::string{}));
    return st::run(*parsed.program, host);
}

st::Value evaluate(std::string_view source)
{
    SoundHost host;
    const auto result = runOn(host, source);
    require(result.ok(), "should run: " + std::string{source}
            + (result.diagnostic ? ": " + result.diagnostic->message : std::string{}));
    return result.value;
}

bool near(const st::Value& value, double number, talas::expr::Unit unit)
{
    return value.kind == st::Value::Kind::Number && value.unit == unit && std::abs(value.number - number) < 1.0e-9;
}

bool textIs(const st::Value& value, std::string_view text)
{
    return value.kind == st::Value::Kind::String && value.text == text;
}

// The Smalltalk-80 language: message sends,
// variables, numbers, literals, booleans, logical operations and blocks.
void testManualSmalltalkChapter()
{
    using talas::expr::Unit;
    // Unary, keyword and binary message sends.
    require(near(evaluate("10 negated"), -10.0, Unit::None), "negated should be a unary message");
    require(textIs(evaluate("'smalltalk' at: 3"), "a"), "'smalltalk' at: 3 should be its third character");
    require(textIs(evaluate("'smalltalk' copyFrom: 3 to: 5"), "all"),
        "copyFrom:to: should take characters 3 to 5");
    require(near(evaluate("3 + 5"), 8.0, Unit::None), "+ should be a binary message");
    // Variable names start with a letter and hold letters and
    // digits; a declaration lists them between bars.
    require(near(evaluate("| x2 tacuma mrJavelina theMainEvent darthBara tryToEnjoyTheDaylight | "
                          "x2 := 1. tacuma := x2 + 1. mrJavelina := tacuma * 2. theMainEvent := mrJavelina. "
                          "darthBara := theMainEvent. tryToEnjoyTheDaylight := darthBara + 1"),
                5.0, Unit::None),
        "typical variable names should be declared and assigned");
    // Integers, radix numbers in either case, printStringRadix:,
    // floating point, scientific notation and radix fractions.
    require(near(evaluate("2r1010"), 10.0, Unit::None) && near(evaluate("16rEFFF"), 61439.0, Unit::None)
            && near(evaluate("16rfff"), 4095.0, Unit::None) && near(evaluate("16rFFF"), 4095.0, Unit::None),
        "radix numbers should read in any case");
    require(textIs(evaluate("17 printStringRadix: 2"), "10001"), "printStringRadix: should write 17 in binary");
    require(near(evaluate("36.7 roundTo: 1"), 37.0, Unit::None) && near(evaluate("7.3 roundTo: 0.5"), 7.5, Unit::None)
            && near(evaluate("-7.8 truncateTo: 2"), -6.0, Unit::None) && near(evaluate("37 s roundTo: 10"), 40.0, Unit::Seconds),
        "roundTo: should give the nearest multiple, truncateTo: the one toward zero");
    require(near(evaluate("1.14e10"), 1.14e10, Unit::None) && near(evaluate("2r10.1"), 2.5, Unit::None),
        "scientific notation and radix fractions should read");
    // // and \\ divide toward negative infinity, quo: and rem:
    // toward zero; vmax: and vmin: give the larger and the smaller.
    require(near(evaluate("-7 // 3"), -3.0, Unit::None) && near(evaluate("7 // 3"), 2.0, Unit::None)
            && near(evaluate("-7 \\\\ 3"), 2.0, Unit::None) && near(evaluate("7 \\\\ 3"), 1.0, Unit::None)
            && near(evaluate("-7 quo: 3"), -2.0, Unit::None) && near(evaluate("7 quo: 3"), 2.0, Unit::None)
            && near(evaluate("-7 rem: 3"), -1.0, Unit::None) && near(evaluate("7 rem: 3"), 1.0, Unit::None)
            && near(evaluate("5 vmax: 7"), 7.0, Unit::None) && near(evaluate("5 vmin: 7"), 5.0, Unit::None),
        "the arithmetic table's divisions and vmax: should give its values");
    // Characters, strings, concatenation, symbols and arrays.
    require(textIs(evaluate("$T"), "T"), "$T should be the character T");
    require(textIs(evaluate("'this is a string'"), "this is a string"), "single quotes should delimit a string");
    require(textIs(evaluate("'first half', 'second half'"), "first halfsecond half"),
        "a comma should concatenate two strings");
    require(evaluate("#thisIsASymbol").kind == st::Value::Kind::Symbol,
        "a number sign should make a symbol");
    require(evaluate("'thisIsAString' asSymbol == #thisIsAString").boolean && evaluate("#abc == #abc").boolean,
        "asSymbol should make the symbol of a string's name, and symbols of one name should be one object");
    const auto literal = evaluate("#(2 $+ 5 #anArray)");
    const auto plain = evaluate("#(2 + 5 anArray)");
    require(literal.kind == st::Value::Kind::Array && literal.elements->size() == 4
            && near((*literal.elements)[0], 2.0, Unit::None) && textIs((*literal.elements)[1], "+")
            && near((*literal.elements)[2], 5.0, Unit::None)
            && (*literal.elements)[3].kind == st::Value::Kind::Symbol && (*literal.elements)[3].text == "anArray"
            && plain.kind == st::Value::Kind::Array && plain.elements->size() == 4
            && (*plain.elements)[3].kind == st::Value::Kind::Symbol,
        "an array literal should hold a number, a character, a number and a symbol, signs optional");
    // #(0 7 -12) holds three numbers, the last negative,
    // in nested parentheses too; a minus apart from its digits is a symbol.
    const auto shape = evaluate("#(0 7 -12 (1 -2) {3 -1})");
    const auto spaced = evaluate("#(2 - 5)");
    require(shape.kind == st::Value::Kind::Array && shape.elements->size() == 5
            && near((*shape.elements)[2], -12.0, Unit::None)
            && near((*(*shape.elements)[3].elements)[1], -2.0, Unit::None)
            && near((*shape.elements)[4], 2.0, Unit::None)
            && spaced.elements->size() == 3 && (*spaced.elements)[1].kind == st::Value::Kind::Symbol,
        "a minus before a literal array's digits should make a negative number");
    const auto nested = evaluate("| anArray anotherArray | anArray := #(a b c). anotherArray := Array with: 2 + 5 with: anArray. anotherArray");
    require(nested.kind == st::Value::Kind::Array && nested.elements->size() == 2
            && near((*nested.elements)[0], 7.0, Unit::None) && (*nested.elements)[1].kind == st::Value::Kind::Array
            && (*nested.elements)[1].elements->size() == 3,
        "Array with:with: should evaluate its elements");
    // Booleans and the logical operations, short-circuiting with
    // and: and or: and evaluating both sides with & and |.
    require(!evaluate("(2 + 2) = 5").boolean && evaluate("true").boolean && !evaluate("false").boolean,
        "comparisons and the boolean literals should give booleans");
    require(evaluate("| anArray | anArray := #(). anArray isEmpty or: [(anArray at: 1) = 0]").boolean,
        "or: should not evaluate its block when the receiver is true");
    require(!evaluate("| anArray i | anArray := #(1 2). i := 3. (anArray size >= i) and: [(anArray at: i) = 10]").boolean,
        "and: should not evaluate its block when the receiver is false");
    require(near(evaluate("| n | n := 0. (true | ((n := n + 1) > 0)) ifTrue: [n] ifFalse: [-1]"), 1.0, Unit::None)
            && near(evaluate("| n | n := 0. (false & ((n := n + 1) > 0)) ifTrue: [-1] ifFalse: [n]"), 1.0, Unit::None),
        "| and & should evaluate both expressions");
    // A block runs when sent value, its arguments given by
    // value:value:.
    require(near(evaluate("| incrBlk i result | i := 0. incrBlk := [i := i + 1]. result := incrBlk value. result"), 1.0,
                Unit::None),
        "a block should run when sent value");
    require(near(evaluate("| sumBlk result | sumBlk := [ :a :b | a + b]. result := sumBlk value: 3 value: 2.5. result"), 5.5,
                Unit::None),
        "value:value: should give a block its two arguments");
}

// Boolean expressions over variables, sets and streams; and:
// keeps a stream from being read past its end, while | and & read and
// change a stream or a collection even when the first side decides.
void testStreamsAndCollections()
{
    using talas::expr::Unit;
    require(evaluate("| i | i := 3. i >= 2").boolean && !evaluate("| i | i := 1. i >= 2").boolean,
        "i >= 2 should be a boolean expression");
    require(evaluate("| aSet | aSet := Set new. aSet isEmpty").boolean
            && near(evaluate("| aSet | aSet := Set new. aSet add: 1. aSet add: 1. aSet add: 2. aSet size"), 2.0, Unit::None)
            && !evaluate("| aSet | aSet := Set withAll: #(4). aSet isEmpty").boolean,
        "aSet isEmpty should tell whether a Set holds anything, each element once");
    require(evaluate("| aStream | aStream := ReadStream on: 'abc'. aStream atEnd not and: [aStream next == 'a']").boolean
            && textIs(evaluate("| aStream | aStream := ReadStream on: 'abc'. aStream atEnd not and: [aStream next == 'a']. aStream next"), "b")
            && !evaluate("| aStream | aStream := ReadStream on: ''. aStream atEnd not and: [aStream next == 'a']").boolean,
        "and: should read the stream only when it is not at its end");
    require(near(evaluate("| aStream | aStream := ReadStream on: #(1 2 3). aStream next. aStream next"), 2.0, Unit::None)
            && evaluate("| aStream | aStream := ReadStream on: #(1). aStream next. aStream atEnd").boolean
            && evaluate("| aStream | aStream := ReadStream on: #(1). aStream next. aStream next").kind == st::Value::Kind::Nil,
        "next should answer the next value and advance the stream");
    require(evaluate("| r s | r := Random newForTalasWithSeed: 5. s := Random newForTalasWithSeed: 5. "
                     "r next > 0.5 | (r next = 0.0). s next. s next. r next = s next")
                .boolean,
        "r next > 0.5 | (r next = 0.0) should read r twice");
    require(near(evaluate("| anArray aStream | anArray := #(). aStream := ReadStream on: #(0 5). "
                          "anArray isEmpty | (aStream next = 0). aStream next"),
                5.0, Unit::None),
        "| should read the stream even though the Array is empty");
    require(near(evaluate("| i aCollection | i := 5. aCollection := OrderedCollection withAll: #(1 2 3). "
                          "(i > 10 & (aCollection removeFirst = 1)) ifTrue: [0] ifFalse: [aCollection size]"),
                2.0, Unit::None)
            && near(evaluate("| aCollection | aCollection := OrderedCollection new. aCollection add: 7. aCollection addFirst: 6. "
                             "aCollection removeFirst"),
                6.0, Unit::None),
        "& should remove the collection's first element even though i > 10 is false");
    SoundHost host;
    require(!runOn(host, "OrderedCollection new removeFirst").ok() && !runOn(host, "OrderedCollection new removeLast").ok()
            && !runOn(host, "(Set new) remove: 3").ok(),
        "an empty collection should have nothing to remove first or last");
}

// Everyday Smalltalk messages: what kind of
// object a value is, loops and conditionals, finding, copying and changing
// collections, comparing and converting strings, and reading streams.
void testQuickReferenceMessages()
{
    using talas::expr::Unit;
    const auto yes = [](std::string_view source) { return evaluate(source).boolean; };
    const auto number = [](std::string_view source, double expected) { return near(evaluate(source), expected, Unit::None); };
    const auto text = [](std::string_view source, std::string_view expected) { return textIs(evaluate(source), expected); };
    require(yes("3 isNumber") && yes("3 isInteger") && !yes("3.5 isInteger") && yes("'a' isString") && yes("#a isString")
            && yes("#a isSymbol") && !yes("'a' isSymbol") && yes("#(1) isArray") && !yes("'ab' isArray") && yes("true isBoolean")
            && !yes("1 isBoolean") && yes("#(1) isCollection") && yes("'ab' isCollection")
            && yes("(OrderedCollection new) isCollection") && !yes("3 isCollection"),
        "a value should say what kind of object it is");
    require(!yes("nil notNil") && yes("3 notNil") && yes("3 ~~ 4") && !yes("3 ~~ 3") && text("3 printString", "3")
            && yes("#(1 2) copy = #(1 2)"),
        "notNil, ~~, printString and copy");
    require(number("| s | s := 0. 1 to: 10 by: 3 do: [:i | s := s + i]. s", 22.0)
            && number("| s | s := 0. 0.5 floatTo: 2 do: [:x | s := s + x]. s", 2.0)
            && number("| s | s := 0. 0 floatTo: 1 by: 0.25 do: [:x | s := s + x]. s", 2.5),
        "to:by:do:, floatTo:do: and floatTo:by:do: should run their block over the interval");
    require(evaluate("false ifTrue: [1]").kind == st::Value::Kind::Nil && evaluate("true ifFalse: [1]").kind == st::Value::Kind::Nil
            && number("true ifFalse: [1] ifTrue: [2]", 2.0) && number("false ifFalse: [1] ifTrue: [2]", 1.0),
        "ifTrue: and ifFalse: should answer nil when they do not run, and ifFalse:ifTrue: its chosen block");
    require(number("(Array new: 3) size", 3.0) && yes("#(1 2 3) includes: 2") && !yes("#(1 2 3) includes: 5")
            && number("#(1 4 9) detect: [:x | x > 3]", 4.0) && number("#(1 4 9) detect: [:x | x > 10] ifNone: [0]", 0.0)
            && yes("(#(1 2 3 4) reject: [:x | x even]) = #(1 3)") && yes("(OrderedCollection withAll: #(1 2)) asArray = #(1 2)")
            && number("#(1 2 1) indexOf: 1", 1.0) && number("#(1 2 1) lastIndexOf: 1", 3.0) && number("#(1 2 1) indexOf: 7", 0.0)
            && yes("#(1 2 3) reverse = #(3 2 1)"),
        "new:, includes:, detect:(ifNone:), reject:, asArray, indexOf:, lastIndexOf: and reverse");
    require(number("| s | s := 0. #(1 2 3) reverseDo: [:x | s := s * 10 + x]. s", 321.0)
            && number("| s | s := 0. #(7 8 9) keysDo: [:k | s := s + k]. s", 6.0)
            && number("#(1 4 9 16) findFirst: [:x | x > 3]", 2.0) && number("#(1 4 9 16) findLast: [:x | x < 10]", 3.0)
            && yes("(#(1 2) copyWith: 3) = #(1 2 3)") && yes("(#(1 2 1 3) copyWithout: 1) = #(2 3)")
            && yes("(#(1 2 3 1 2) copyReplaceAll: #(1 2) with: #(9)) = #(9 3 9)")
            && yes("(#(1 2 3) copyReplaceAll: #(7) with: #(9)) = #(1 2 3)")
            && yes("(#(1 2 3 4) copyReplaceFrom: 2 to: 3 with: #(8 8 8)) = #(1 8 8 8 4)")
            && number("#(1 2 3 1 2) indexOfSubCollection: #(1 2) startingAt: 2", 4.0),
        "reverseDo:, keysDo:, findFirst:, findLast:, copyWith:, copyWithout: and the replacing copies");
    require(number("| c | c := OrderedCollection withAll: #(1 2). c addLast: 3. c removeLast", 3.0)
            && number("| c | c := OrderedCollection withAll: #(1 2). c remove: 5 ifAbsent: [0]", 0.0)
            && number("| c | c := OrderedCollection withAll: #(1 2). c remove: 2 ifAbsent: [0]", 2.0)
            && yes("| c | c := OrderedCollection withAll: #(3). c addAll: #(4 5). c addAllFirst: #(1 2). c asArray = #(1 2 3 4 5)")
            && yes("| c | c := OrderedCollection withAll: #(1 2 3 2). c removeAll: #(2 3). c asArray = #(1 2)")
            && number("(OrderedCollection withAll: #(1 2 1 1)) occurrencesOf: 1", 3.0)
            && number("(Set withAll: #(1 2 1 1)) occurrencesOf: 1", 1.0),
        "addLast:, removeLast, remove:ifAbsent:, addAll:, addAllFirst:, removeAll: and occurrencesOf:");
    require(yes("'abc' < 'ABD'") && yes("'b' > 'A'") && yes("'abc' <= 'ABC'") && yes("'abc' >= 'ABC'") && !yes("'abd' < 'abc'")
            && number("'abc' trueCompare: 'abd'", -1.0) && number("'abc' trueCompare: 'abc'", 0.0)
            && number("'b' trueCompare: 'a'", 1.0) && yes("'wave' beginsWith: 'wa'") && yes("'wave' endsWith: 've'")
            && !yes("'wave' endsWith: 'wa'") && number("'abcabc' findString: 'bc' startingAt: 3", 5.0)
            && number("'abc' findString: 'x' startingAt: 1", 0.0),
        "strings compare in ASCII with case ignored, and begin, end and contain strings");
    require(yes("'a#c*' match: 'ABCdef' ignoreCase: true") && !yes("'a#c*' match: 'ABCdef' ignoreCase: false")
            && yes("'a#c*' match: 'abcdef' ignoreCase: false") && !yes("'a#c' match: 'abcd' ignoreCase: true"),
        "in match:ignoreCase: # matches one character and * any run, case counting unless ignored");
    require(text("'voice' & 3", "voice3") && text("'Wave' asLowercase", "wave") && text("'Wave' asUppercase", "WAVE")
            && text("'name.aif' copyUpTo: $.", "name") && text("'one\\two' withCRs", "one\rtwo")
            && number("'3.5' asNumber", 3.5) && evaluate("'three' asNumber").kind == st::Value::Kind::Nil
            && text("'abc' reverse", "cba") && number("'banana' indexOf: $n", 3.0)
            && text("'hello' copyReplaceAll: 'll' with: 'LL'", "heLLo") && text("'wave' select: [:c | c ~= $a]", "wve"),
        "& joins a printed value, case converts, copyUpTo:, withCRs and asNumber; a string is a collection");
    require(text("| s | s := ReadStream on: 'abc'. s next. s peek", "b")
            && text("| s | s := ReadStream on: 'abc'. s next. s peek. s next", "b")
            && yes("| s | s := ReadStream on: #(1 2 3). s next. s upToEnd = #(2 3)")
            && number("| s | s := ReadStream on: #(1 2 3). s next. s next. s reset. s next", 1.0)
            && number("| s | s := ReadStream on: #(1 2 3). s skip: 2. s next", 3.0)
            && text("(ReadStream on: 'abc') contents", "abc") && yes("(ReadStream on: #(1 2)) contents = #(1 2)"),
        "peek, upToEnd, reset, skip: and contents");
}

// Bags, Sets and SortedCollections and the
// conversions between them, Dictionaries, Integers' bits, and reading and
// writing streams.
void testBagsDictionariesIntegersAndStreams()
{
    using talas::expr::Unit;
    const auto yes = [](std::string_view source) { return evaluate(source).boolean; };
    const auto number = [](std::string_view source, double expected) { return near(evaluate(source), expected, Unit::None); };
    const auto text = [](std::string_view source, std::string_view expected) { return textIs(evaluate(source), expected); };
    require(number("| b | b := Bag new. b add: 3 withOccurrences: 2. b add: 4. b occurrencesOf: 3", 2.0)
            && yes("(Bag withAll: #(1 2 1)) containsDuplicates") && !yes("(Bag withAll: #(1 2)) containsDuplicates")
            && number("#(1 2 2 3) asBag size", 4.0) && number("#(1 2 2 3) asSet size", 3.0)
            && yes("((Set withAll: #(1 2 3)) - #(2)) asArray = #(1 3)") && number("(Set withAll: #(7)) anyElement", 7.0)
            && evaluate("Set new anyElement").kind == st::Value::Kind::Nil && yes("(Set withAll: #(1 2)) intersects: #(2 5)")
            && !yes("(Set withAll: #(1 2)) intersects: #(5)"),
        "a Bag holds an element many times, a Set once, with its difference, any element and overlap");
    require(yes("#(3 1 2) asSortedCollection asArray = #(1 2 3)")
            && yes("(#(3 1 2) asSortedCollection: [:a :b | a >= b]) asArray = #(3 2 1)")
            && yes("| s | s := SortedCollection sortBlock: [:a :b | a >= b]. s add: 1. s add: 5. s add: 3. s asArray = #(5 3 1)")
            && yes("| s | s := #(4 2) asSortedCollection. s add: 3. s asArray = #(2 3 4)")
            && yes("(#(1 2) asOrderedCollection addFirst: 0; yourself) isNil not")
            && number("| c | c := #(5 6 7) asOrderedCollection. c at: 2 put: 9. c at: 2", 9.0)
            && yes("| c | c := #(5 6 7) asOrderedCollection. c swap: 1 with: 3. c asArray = #(7 6 5)")
            && yes("| c | c := #(5 6) asOrderedCollection. c atAllPut: 0. c asArray = #(0 0)")
            && yes("(#(1 2 3) asOrderedCollection collect: [:x | x * 2]) = #(2 4 6)"),
        "sorted collections keep their order, an OrderedCollection's elements are reached by position");
    require(number("| d | d := Dictionary new. d at: #a put: 1. d at: #b put: 2. d at: #b", 2.0)
            && number("| d | d := Dictionary new. d at: #a put: 1. d at: #a put: 5. d at: #a", 5.0)
            && number("| d | d := Dictionary new. d at: #z ifAbsent: [7]", 7.0)
            && evaluate("| d | d := Dictionary new. d at: #a put: 1. d keyAtValue: 1").text == "a"
            && number("| d | d := Dictionary new. d keyAtValue: 3 ifAbsent: [0]", 0.0)
            && number("| d | d := Dictionary new. d at: #a put: 4. d removeKey: #a", 4.0)
            && number("| d | d := Dictionary new. d removeKey: #a ifAbsent: [9]", 9.0)
            && yes("| d | d := Dictionary new. d at: 1 put: 2. d includesKey: 1") && !yes("Dictionary new includesKey: 1")
            && number("| d s | d := Dictionary new. d at: 2 put: 10. d at: 3 put: 20. s := 0. d keysAndValuesDo: [:k :v | s := s + (k * v)]. s", 80.0)
            && number("| d s | d := Dictionary new. d at: 2 put: 10. d at: 3 put: 20. s := 0. d keysDo: [:k | s := s + k]. s", 5.0)
            && yes("| d | d := Dictionary new. d at: 2 put: 10. d at: 3 put: 20. d values = #(10 20)")
            && number("| d | d := Dictionary new. d at: 2 put: 10. d at: 2 put: 11. d keys size", 1.0)
            && number("| d s | d := Dictionary new. d at: 1 put: 4. d at: 2 put: 5. s := 0. d do: [:v | s := s + v]. s", 9.0),
        "a Dictionary keeps values at unique keys, its iteration over the values");
    require(number("1 << 3", 8.0) && number("16 >> 2", 4.0) && number("1 bitShift: 4", 16.0) && number("16 bitShift: -4", 1.0)
            && number("12 bitAnd: 10", 8.0) && number("12 bitOr: 10", 14.0) && number("12 bitXor: 10", 6.0) && number("0 bitInvert", -1.0)
            && number("16r12345678 bottom24", 0x345678) && number("16r12345678 bottom32", 0x12345678)
            && number("16r123456789ABC bottom48", static_cast<double>(0x123456789ABCLL)) && number("16r12345678 top24", 0x12)
            && number("16r12345678 byte0", 0x78) && number("16r12345678 byte1", 0x56) && number("16r12345678 byte2", 0x34)
            && number("16r12345678 byte3", 0x12) && yes("7 isPrime") && !yes("9 isPrime") && !yes("1 isPrime")
            && number("5 factorial", 120.0) && number("12 gcd: 18", 6.0) && number("4 lcm: 6", 12.0),
        "Integers shift, mask their bits and bytes, and answer primes, factorials, gcd: and lcm:");
    require(text("(ReadStream on: 'wave') next: 2", "wa") && text("(ReadStream on: 'wa') nextAvailable: 5", "wa")
            && text("(ReadStream on: 'one two') through: $ ", "one ") && text("(ReadStream on: 'one two') upTo: $ ", "one")
            && text("| s | s := ReadStream on: 'one two'. s upTo: $ . s upToEnd", "two")
            && text("(ReadStream on: 'abcdef') throughAll: 'cd'", "abcd") && text("(ReadStream on: 'abcdef') upToAll: 'cd'", "ab")
            && text("(ReadStream on: 'abc') upTo: $z", "abc")
            && yes("| s | s := ReadStream on: #(1 2 3). s nextMatchFor: 1") && number("| s | s := ReadStream on: #(1 2 3). s next. s position", 1.0)
            && number("| s | s := ReadStream on: #(1 2 3). s position: 2. s next", 3.0)
            && text("| s | s := ReadStream on: 'a,b'. s skipThrough: $,. s upToEnd", "b")
            && text("| s | s := ReadStream on: 'a,b'. s skipUpTo: $,. s upToEnd", ",b")
            && text("| s | s := ReadStream on: 'abcab'. s next. s skipToAll: 'ab'. s upToEnd", "ab")
            && evaluate("| s | s := ReadStream on: 'abc'. s skipUpTo: $z").kind == st::Value::Kind::Nil
            && text("(ReadStream on: 'wave') contentsBetween: 2 and: 3", "av") && text("'wave' readStream upToEnd", "wave"),
        "a ReadStream reads runs, up to and through an element or a run, skips and holds its position");
    require(text("| w | w := WriteStream on: String new. w nextPutAll: 'Wa'; nextPut: $v; nextPut: $e. w contents", "Wave")
            && text("| w | w := WriteStream on: String new. w print: 42; space; print: 1.5; tab; nextPutAll: 'x'; cr. w contents", "42 1.5\tx\r")
            && yes("| w | w := WriteStream on: (Array new: 0). w nextPut: 1; nextPut: 2. w contents = #(1 2)")
            && text("| w | w := 'xyz' writeStream. w next: 3 put: $-. w contents", "---")
            && text("| w | w := ReadWriteStream on: String new. w nextPutAll: 'ab'. w reset. w next", "a"),
        "a WriteStream puts elements, collections, printed values and white space");
}

// A number with the d exponent is a double
// (1.3d, 1.14d10, 9.996d-5), and a block declares temporaries of its own
// after its arguments.
void testDoublesAndBlockTemporaries()
{
    using talas::expr::Unit;
    require(near(evaluate("1.3d"), 1.3, Unit::None) && near(evaluate("1.14d10"), 1.14e10, Unit::None)
            && near(evaluate("9.996d-5 * 100000"), 9.996, Unit::None),
        "the d exponent should write a double");
    require(near(evaluate("[:str :nbr | | size | size := str size. size + nbr] value: 'ab' value: 3"), 5.0, Unit::None)
            && near(evaluate("| size | size := 10. [:x | | size | size := x. size] value: 4. size"), 10.0, Unit::None),
        "a block's temporaries should be its own");
}

// TimesRepeat: runs its block that many times
// and answers nil, so a program shows its variable only when it ends with
// it; a block's debugWithLabel:value: shows each step; to:do: gives the
// block each number in turn; and whileFalse: and whileTrue: loop until
// their block's condition changes.
void testLoopStructures()
{
    const auto parsedAndRun = [](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), std::string{source} + " should parse");
        struct NoHost final : st::Host
        {
            std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
            std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
            {
                error = "none";
                return std::nullopt;
            }
        } host;
        const auto ran = st::run(*parsed.program, host);
        require(ran.ok(), std::string{source} + " should run");
        return ran;
    };
    require(evaluate("| i | i := 5.\n4 timesRepeat: [i := i + 1].").kind == st::Value::Kind::Nil
            && evaluate("| i | i := 5.\n4 timesRepeat: [i := i + 1]. i").number == 9,
        "timesRepeat: should answer nil, the variable 9 after four increments of 5");
    const auto steps = parsedAndRun("| i | i := 5.\n4 timesRepeat: [ i := i + 1. self debugWithLabel: 'i' value: i]. i");
    require(steps.value.number == 9 && steps.debugLines.size() == 4 && steps.debugLines.front().ends_with("6")
            && steps.debugLines.back().ends_with("9"),
        "debugWithLabel:value: should show i as 6, 7, 8 and 9");
    const auto harmonics = parsedAndRun("| harm | 1 to: 4 do: [ :i | harm := 100 hz * i. self debug: harm]. harm");
    require(st::fieldText(harmonics.value) == "400 hz" && harmonics.debugLines.size() == 4
            && harmonics.debugLines[1].find("200") != std::string::npos,
        "to:do: should give the block 1, 2, 3 and 4, harm 200 hz the second time");
    const auto heads = parsedAndRun("| heads rando tries | heads := false. tries := 0. rando := Random newForTalasWithSeed: 3.\n"
                                    "[heads] whileFalse: [ heads := rando next > 0.5. tries := tries + 1]. tries");
    require(heads.value.number >= 1 && evaluate("| h | h := 0. [h < 3] whileTrue: [h := h + 1]. h").number == 3,
        "whileFalse: should loop until heads comes up, whileTrue: while its condition holds");
}

// X @ y is a Point, with its coordinates, its
// distance from 0@0 (r) and its angle from 3 o'clock (theta, from 0 to 2
// pi), and arithmetic with Points and numbers; clipTo01 keeps a number
// between 0 and 1.
void testPoints()
{
    const auto number = [](std::string_view source) {
        const auto value = evaluate(source);
        require(value.kind == st::Value::Kind::Number, std::string{source} + " should be a number");
        return value.number;
    };
    require(number("(3@4) r") == 5 && number("(3@4) x") == 3 && number("(3@4) y") == 4
            && std::abs(number("(0@1) theta") - std::numbers::pi / 2.0) < 1.0e-12
            && std::abs(number("(-1@0) theta") - std::numbers::pi) < 1.0e-12
            && std::abs(number("(0@ -1) theta") - 1.5 * std::numbers::pi) < 1.0e-12 && number("(1@0) theta") == 0,
        "a Point's coordinates, r and theta");
    require(number("((1@2) + (3@4)) x") == 4 && number("((1@2) * 2) y") == 4 && number("(2 * (1@2)) x") == 2
            && number("((4@6) / (2@3)) y") == 2 && number("((5@5) - 1) x") == 4,
        "arithmetic with Points and numbers");
    const auto same = evaluate("(1@2) = (1@2)");
    require(number("0.4 clipTo01") == 0.4 && number("1.7 clipTo01") == 1 && number("-3 clipTo01") == 0
            && same.kind == st::Value::Kind::Boolean && same.boolean,
        "clipTo01, and Points compared");
}

// A host whose scripts reach files, kept in memory.
class FilesHost final : public st::Host
{
public:
    st::MemoryFileSystem memory;

    st::FileSystem* files() override { return &memory; }
    std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
    std::optional<st::Value> send(const st::Value&, std::string_view selector, std::span<const st::Value>,
        std::string& error) override
    {
        error = "The host does not understand " + std::string{selector};
        return std::nullopt;
    }
};

st::RunResult runWithFiles(FilesHost& host, std::string_view source)
{
    const auto parsed = st::parse(source);
    require(parsed.ok(), "should parse: " + std::string{source}
            + (parsed.diagnostic ? ": " + parsed.diagnostic->message : std::string{}));
    return st::run(*parsed.program, host);
}

st::Value withFiles(FilesHost& host, std::string_view source)
{
    const auto result = runWithFiles(host, source);
    require(result.ok(), "should run: " + std::string{source}
            + (result.diagnostic ? ": " + result.diagnostic->message : std::string{}));
    return result.value;
}

std::vector<std::string> textsOf(const st::Value& value)
{
    require(value.kind == st::Value::Kind::Array, "should be an Array");
    std::vector<std::string> texts;
    for (const auto& element : *value.elements)
        texts.push_back(element.text);
    return texts;
}

// A Filename's file made, read whole, sized,
// copied, moved, renamed and deleted, its directory listed and filtered by
// file type; streams that write a file afresh, append to it, read it, or
// read and append, each with its line end, bytes or characters, positions,
// and closing; HostDriverInterface's file types, lists and Filenames. The
// files are the host's.
void testFilesThroughTheHost()
{
    FilesHost host;
    // ReadGIF answers the GIF image found in the file.
    host.memory.write("pic.gif", std::string{"GIF89a\x03\x00\x02\x01\x00\x00\x00;", 14});
    require(withFiles(host, "('pic.gif' asFilename readGIF) width").number == 3
            && withFiles(host, "('pic.gif' asFilename readGIF) height").number == 258
            && withFiles(host, "('pic.gif' asFilename readGIF) extent y").number == 258
            && withFiles(host, "('pic.gif' asFilename readGIF) class printString").text == "Image",
        "readGIF should answer the GIF image found in the file, its width and height");
    require(withFiles(host, "'notes/a.txt' asFilename createIfNonExistent; reallyExists").boolean
            && withFiles(host, "'notes' asFilename isDirectory").boolean
            && withFiles(host, "'notes/a.txt' asFilename fileSize").number == 0
            && !withFiles(host, "'notes/a.txt' asFilename isDirectory").boolean
            && withFiles(host, "'notes' asFilename reallyExistsOrIsDirectory").boolean
            && withFiles(host, "'notes/a.txt' asFilename reallyWritable").boolean,
        "createIfNonExistent should make the file, in its directory");
    require(withFiles(host, "| s | s := 'notes/a.txt' asFilename writeStream. s nextPutAll: 'one'; cr; nextPutAll: 'two'.\n"
                            "s close. 'notes/a.txt' asFilename contentsOfEntireFile").text
            == "one\ntwo",
        "a write stream should write the file, read back whole");
    require(withFiles(host, "| s | s := 'notes/a.txt' asFilename appendStream. s lineEndCRLF; cr; nextPutAll: 'three'.\n"
                            "s close. s closed").boolean
            && host.memory.read("notes/a.txt") == "one\ntwo\r\nthree",
        "an append stream should add to the end, with its line end");
    require(withFiles(host, "| s | s := 'notes/a.txt' asFilename readStream. {s next: 3. s readPosition}") .elements->at(0).text
            == "one",
        "a read stream should read the file");
    const auto writingToRead = runWithFiles(host, "'notes/a.txt' asFilename readStream nextPutAll: 'x'");
    require(!writingToRead.ok() && writingToRead.diagnostic->message.find("reading only") != std::string::npos,
        "a read stream should not write");
    host.memory.write("b.txt", "abc");
    const auto both = withFiles(host, "| s r | s := 'b.txt' asFilename readAppendStream. r := s next: 2.\n"
                                      "s nextPutAll: 'Z'. {r. s readPosition. s writePosition. s upToEnd}");
    require(textsOf(both)[0] == "ab" && (*both.elements)[1].number == 2 && (*both.elements)[2].number == 4
            && textsOf(both)[3] == "cZ" && host.memory.read("b.txt") == "abcZ",
        "a read and append stream should read from the start and write at the end");
    withFiles(host, "'c.txt' asFilename writeStream nextPutAll: 'kept'");
    require(host.memory.read("c.txt") == "kept", "a stream's writes should reach its file as the script ends");
    const auto afterClose = runWithFiles(host, "| s | s := 'c.txt' asFilename appendStream. s close. s nextPutAll: 'x'");
    require(!afterClose.ok() && afterClose.diagnostic->message.find("closed") != std::string::npos,
        "a closed stream should not write");
    host.memory.write("d.bin", std::string{"\x01\x02", 2});
    require(withFiles(host, "| s | s := 'd.bin' asFilename readStream. s binary. s isBinary & (s next = 1)").boolean
            && withFiles(host, "| s | s := 'e.bin' asFilename writeStream. s binary. s nextPut: 65; nextPut: 66. s close. "
                               "'e.bin' asFilename contentsOfEntireFile").text
                == "AB"
            && withFiles(host, "'d.bin' asFilename readStream isText").boolean,
        "binary and text should make the stream's data bytes or characters");
    withFiles(host, "'notes/a.txt' asFilename copyTo: 'copy.txt'. 'copy.txt' asFilename renameTo: 'renamed.txt'.\n"
                    "'renamed.txt' asFilename moveTo: 'moved.txt'");
    require(host.memory.read("moved.txt") == host.memory.read("notes/a.txt") && !host.memory.read("copy.txt")
            && !host.memory.read("renamed.txt"),
        "copyTo:, renameTo: and moveTo: should copy and move the file");
    require(!withFiles(host, "'moved.txt' asFilename delete; reallyExists").boolean,
        "delete should delete the file");
    require(textsOf(withFiles(host, "'notes' asFilename directoryContents")) == std::vector<std::string>{"a.txt"}
            && withFiles(host, "'new' asFilename makeDirectory; isDirectory").boolean,
        "directoryContents and makeDirectory");
    for (const auto* name : {"sounds/x.aif", "sounds/y.wav", "sounds/z.txt"})
        host.memory.write(name, "");
    require(textsOf(withFiles(host, "'sounds' asFilename directoryContentsFilteredBy: "
                                    "HostDriverInterface sampleFileTypes asFileTypeFilter"))
                == std::vector<std::string>{"x.aif", "y.wav"}
            && textsOf(withFiles(host, "'sounds' asFilename directoryContentsFilteredBy: "
                                       "HostDriverInterface aiffFileType asFileTypeFilter"))
                == std::vector<std::string>{"x.aif"},
        "a file type's asFileTypeFilter should filter a directory's contents");
    require(withFiles(host, "('sounds/x.aif' asFilename fileNamesInSameDirectory collect: [:f | f asString]) last").text
            == "sounds/z.txt",
        "fileNamesInSameDirectory should answer full paths");
    require(withFiles(host, "(HostDriverInterface createFilenameWithExtensionFromString: 'take' andType: "
                            "HostDriverInterface aiffFileType) asString").text
                == "take.aif"
            && withFiles(host, "(HostDriverInterface createFilenameFromString: 'take' andType: "
                               "HostDriverInterface textFileType) asString").text
                == "take"
            && textsOf(withFiles(host, "HostDriverInterface textFileTypes")) == std::vector<std::string>{"TEXT"}
            && withFiles(host, "HostDriverInterface waveFileType").text == "WAVE",
        "HostDriverInterface's file types and Filenames");
    withFiles(host, "HostDriverInterface copyFile: 'b.txt' to: 'f.txt'. HostDriverInterface "
                    "setFileTypeAndCreatorForTextFile: 'f.txt' asFilename. HostDriverInterface flushFileSystem");
    require(host.memory.read("f.txt") == "abcZ", "copyFile:to: should copy the file");
    withFiles(host, "HostDriverInterface copyFileTypeAndCreatorFrom: 'b.txt' to: 'f.txt'.\n"
                    "HostDriverInterface setFileTypeAndCreatorForAIFFSamplesFile: 'x.aif'.\n"
                    "HostDriverInterface setFileTypeAndCreatorForMIDIFile: 'x.mid'.\n"
                    "HostDriverInterface setFileTypeAndCreatorForSpectrumFile: 'x.spc'.\n"
                    "HostDriverInterface setFileTypeAndCreatorForWAVESamplesFile: 'x.wav'");
    for (const auto* type : {"aiffFileType", "waveFileType", "sampleFileType", "midiFileType", "textFileType",
             "gifFileType", "pngFileType"})
        require(!withFiles(host, std::string{"HostDriverInterface "} + type).text.empty(),
            std::string{"HostDriverInterface should answer its "} + type);
    for (const auto* types : {"sampleFileTypes", "spectrumFileTypes", "midiFileTypes", "textFileTypes"})
        require(!withFiles(host, std::string{"HostDriverInterface "} + types).elements->empty(),
            std::string{"HostDriverInterface should answer its "} + types);
    // Each line end, a write stream deleting the file it finds, and a
    // stream that reads and writes, skips, flushes and commits.
    withFiles(host, "| s | s := 'ends.txt' asFilename writeStream. s lineEndCR; cr. s lineEndLF; cr.\n"
                    "s lineEndTransparent; cr. s flush. s commit");
    require(host.memory.read("ends.txt") == "\r\n\n", "lineEndCR, lineEndLF and lineEndTransparent");
    withFiles(host, "'c.txt' asFilename writeStream close");
    require(host.memory.read("c.txt") == "", "a write stream should delete the file it finds");
    require(withFiles(host, "| s | s := 'g.txt' asFilename readWriteStream. s nextPutAll: 'rwx'. s reset. s skip: 1.\n"
                            "s next: 2").text
                == "wx"
            && withFiles(host, "| s | s := 'g.txt' asFilename readStream. {s skipThrough: $q. s atEnd}").elements->at(1).boolean
            && withFiles(host, "| s | s := 'd.bin' asFilename readStream. s binary; text. s isText & s isBinary not").boolean,
        "a stream that reads and writes, skip:, a search that ends the stream, and text again");
    // A file's dates, each a Date and a Time, by when it
    // was read, written and changed.
    host.memory.setClock([] { return 1704110400.0; }); // noon on 1 January 2024
    host.memory.write("dated.txt", "x");
    const auto dates = withFiles(host, "| d | d := 'dated.txt' asFilename dates.\n"
                                       "{d size. (d at: #modified) first printString. (d at: #accessed) last asSeconds\n"
                                       "- (d at: #statusChanged) last asSeconds}");
    require(dates.elements->at(0).number == 3 && dates.elements->at(1).text == "January 1, 2024"
            && dates.elements->at(2).number == 0 && !runWithFiles(host, "'undated.txt' asFilename dates").ok(),
        "a file's dates should be a Dictionary of when it was read, written and changed");
    SoundHost none;
    const auto unreached = runOn(none, "'a.txt' asFilename contentsOfEntireFile");
    require(!unreached.ok(), "a script whose host gives no files should reach none");
}

// Random new, unseeded, gives another sequence
// each time the program runs, from its host's fresh seed, where a seeded
// Random repeats; a project before format 115 writes its Random new as
// (Random newForTalasWithSeed: 0), the stream it had.
void testRandomNewDiffersEachRun()
{
    class Seeded final : public st::Host
    {
    public:
        explicit Seeded(std::uint32_t seed)
            : seed_(seed)
        {
        }
        std::uint32_t freshSeed() override { return seed_; }
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view selector, std::span<const st::Value>,
            std::string& error) override
        {
            error = "The host does not understand " + std::string{selector};
            return std::nullopt;
        }

    private:
        std::uint32_t seed_;
    };
    const auto drawn = [](std::uint32_t seed) {
        Seeded host{seed};
        const auto parsed = st::parse("Random new next");
        require(parsed.ok(), "Random new next should parse");
        const auto ran = st::run(*parsed.program, host);
        require(ran.ok() && ran.value.kind == st::Value::Kind::Number, "Random new next should be a number");
        return ran.value.number;
    };
    require(drawn(5) != drawn(6) && drawn(5) == drawn(5),
        "Random new should take its sequence from a fresh seed");
    SoundHost one;
    SoundHost other;
    require(runOn(one, "Random new next").value.number != runOn(other, "Random new next").value.number,
        "each run of an unseeded Random should give another sequence");
    require(evaluate("(Random newForTalasWithSeed: 3413) next").number == evaluate("(Random newForTalasWithSeed: 3413) next").number,
        "a seeded Random should repeat its sequence");
    require(st::withRandomNewRepeatable("| r | r := Random new. r next") == "| r | r := (Random newForTalasWithSeed: 0). r next"
            && st::withRandomNewRepeatable("(Random new seed: 92) next") == "((Random newForTalasWithSeed: 0) seed: 92) next"
            && st::withRandomNewRepeatable("Random newForTalasWithSeed: 3") == "Random newForTalasWithSeed: 3"
            && st::withRandomNewRepeatable("myRandom new") == "myRandom new"
            && drawn(0) == evaluate("(Random newForTalasWithSeed: 0) next").number,
        "a format 114 field's Random new should become the stream it had");
}

// An object's class and whether it is of a
// class or a subclass, a message performed with an Array of arguments, a
// number read from a stream, Float's and Double's error tolerance, and a
// Bag's sorted counts.
void testQuickReferenceClassesAndCounts()
{
    const auto text = [](std::string_view source) { return evaluate(source).text; };
    require(text("3 class printString") == "SmallInteger" && text("3.5 class printString") == "Double"
            && text("'a' class printString") == "String" && text("#a class printString") == "Symbol"
            && text("(3 @ 4) class printString") == "Point" && text("OrderedCollection new class printString") == "OrderedCollection"
            && text("nil class printString") == "UndefinedObject" && evaluate("3 class = SmallInteger").boolean,
        "class should answer the receiver's class");
    require(evaluate("3 isKindOf: Number").boolean && evaluate("3 isKindOf: Integer").boolean
            && !evaluate("3.5 isKindOf: Integer").boolean && evaluate("#a isKindOf: String").boolean
            && evaluate("OrderedCollection new isKindOf: Collection").boolean && !evaluate("3 isKindOf: String").boolean
            && evaluate("SortedCollection new isKindOf: OrderedCollection").boolean,
        "isKindOf: should be true of a class and its superclasses");
    // Recognizes: answers whether the receiver can
    // respond to the message, without sending it.
    require(evaluate("3 recognizes: #+").boolean && evaluate("3 recognizes: #between:and:").boolean
            && !evaluate("3 recognizes: #fooBar").boolean && evaluate("'abc' recognizes: #size").boolean
            && !evaluate("'abc' recognizes: #sqrt").boolean && evaluate("#(1 2) recognizes: #do:").boolean
            && evaluate("[:x | x] recognizes: #value:").boolean && !evaluate("[:x | x] recognizes: #size").boolean
            && evaluate("| c | c := OrderedCollection new. (c recognizes: #add:) & (c size = 0)").boolean,
        "recognizes: should be true of a message the receiver can respond to, the receiver untouched");
    require(evaluate("3 perform: #between:and: withArguments: #(1 5)").boolean,
        "perform:withArguments: should send the message with the Array's arguments");
    require(evaluate("Number readFrom: '  42.5 rest' readStream").number == 42.5
            && evaluate("Number readFrom: '-3e2x' readStream").number == -300.0,
        "Number readFrom: should parse the next number in the stream");
    require(evaluate("Float errorTolerance").number == static_cast<double>(std::numeric_limits<float>::epsilon())
            && evaluate("Double errorTolerance").number == std::numeric_limits<double>::epsilon(),
        "Float and Double should answer their error tolerance");
    const auto counted = evaluate("| b first | b := Bag new. b add: 3; add: 5; add: 3. first := b sortedCounts first. "
                                  "{first key. first value}");
    require(counted.kind == st::Value::Kind::Array && (*counted.elements)[0].number == 2 && (*counted.elements)[1].number == 3,
        "sortedCounts should answer the occurrences to objects, the most counted first");
}

// A script that makes collections, streams, Dictionaries and Randoms by the
// thousand keeps only those it holds.
void testObjectsNobodyHoldsAreMadeAgain()
{
    require(evaluate("| c | 1 to: 5000 do: [:i | c := OrderedCollection new. c add: i]. c first").number == 5000
            && evaluate("| d | 1 to: 3000 do: [:i | d := Dictionary new. d at: 1 put: i]. d at: 1").number == 3000
            && evaluate("| r | 1 to: 3000 do: [:i | r := Random newForTalasWithSeed: i]. r next").number > 0
            && evaluate("| s | 1 to: 3000 do: [:i | s := ReadStream on: #(1 2 3)]. s next").number == 1,
        "objects no Value holds should give up their places");
    SoundHost host;
    const auto full = runOn(host, "| kept | kept := OrderedCollection new. 1 to: 1100 do: [:i | kept add: "
                                  "OrderedCollection new]. kept size");
    require(!full.ok() && full.diagnostic->message.find("at most 1024") != std::string::npos,
        "a script should hold at most 1024 collections at once");
}

// Its Smalltalk examples' results: a block's
// value, a block given its argument, faderMaker's two-digit hot value,
// and: and or: evaluating their blocks only when the receiver leaves the
// answer open.
void testDocumentedSmalltalkExamples()
{
    require(evaluate("[5 + 6] value").number == 11
            && evaluate("| incrBlk | incrBlk := [ :nbr | nbr + 1]. incrBlk value: 20").number == 21,
        "[5 + 6] value should be 11, and incrBlk value: 20 should be 21");
    SoundHost host;
    const auto missing = runOn(host, "| incrBlk | incrBlk := [ :nbr | nbr + 1]. incrBlk value");
    require(!missing.ok(), "each of a block's arguments should be given a value");
    const auto fader = evaluate("| faderMaker | \"A block to construct a fader!\"\n"
                                "faderMaker := [ :str :nbr | | nbrString |\n"
                                "nbr < 10 ifTrue: [nbrString := '0', nbr printString] ifFalse: [nbrString := nbr printString].\n"
                                "(str, nbrString) asHotValue].\n"
                                "faderMaker value: 'fader' value: 6");
    require(fader.kind == st::Value::Kind::Expression && fader.text == "!fader06",
        "faderMaker value: 'fader' value: 6 should be the hot value !fader06, not " + fader.text);
    require(evaluate("| x | x := 4. x >= 3 and: [x <= 5]").boolean && !evaluate("| x | x := 6. x >= 3 and: [x <= 5]").boolean,
        "x >= 3 and: [x <= 5] should test that x lies from 3 to 5");
    const auto skipped = runOn(host, "| x | x := 1. x >= 3 and: [ self debug: x. x <= 5]");
    require(skipped.ok() && skipped.value.kind == st::Value::Kind::Boolean && !skipped.value.boolean
            && skipped.debugLines.empty(),
        "with x 1 the expression should be false, its block never run, so debug: shows nothing");
    const auto emptyOrZero = [](std::string_view array) {
        return evaluate("| anArray | anArray := " + std::string{array} + ". anArray isEmpty or: [(anArray at: 1) = 0]").boolean;
    };
    require(emptyOrZero("#()") && emptyOrZero("#(0 5)") && !emptyOrZero("#(3)"),
        "true if anArray is empty or its first entry is 0, at: never sent to an empty one");
    // For octaves 4 and 5 and every other pitch class, the octave
    // performs the pitch's message, 4 perform: (#(c d e f g a b) at: 1)
    // being 4 c.
    const auto pitches = evaluate("| pchs out | pchs := #(c d e f g a b). out := OrderedCollection new.\n"
                                  "4 to: 5 do: [:oct | (1 to: 7 by: 2) do: [:pch | out add: (oct perform: (pchs at: pch))]].\n"
                                  "out asArray");
    const std::vector<double> notes{60, 64, 67, 71, 72, 76, 79, 83};
    require(pitches.kind == st::Value::Kind::Array && pitches.elements->size() == notes.size()
            && std::ranges::equal(*pitches.elements, notes, [](const st::Value& pitch, double note) {
                   return pitch.number == note && pitch.unit == talas::expr::Unit::NoteNumber;
               })
            && evaluate("4 perform: (#(c d e f g a b) at: 1)").number == 60
            && evaluate("3 perform: #between:and: with: 1 with: 5").boolean,
        "the octave should perform each pitch, 4 c, 4 e, 4 g, 4 b, 5 c and on");
}

// A Rectangle, origin to corner, its sides,
// extent and centre, made from Points or its sides, and a cursor point made
// relative to a View's bounds.
void testRectangles()
{
    const auto number = [](std::string_view source) {
        const auto value = evaluate(source);
        require(value.kind == st::Value::Kind::Number, std::string{source} + " should be a number");
        return value.number;
    };
    require(number("(0 @ 0 corner: 100 @ 50) width") == 100 && number("(0 @ 0 corner: 100 @ 50) height") == 50
            && number("(0 @ 0 corner: 100 @ 50) extent x") == 100 && number("(0 @ 0 corner: 100 @ 50) center y") == 25
            && number("(Rectangle origin: 1 @ 2 corner: 3 @ 4) topLeft y") == 2
            && number("(Rectangle origin: 1 @ 2 extent: 3 @ 4) corner x") == 4
            && number("(Rectangle left: 1 right: 5 top: 2 bottom: 8) height") == 6
            && number("((0 @ 0 corner: 100 @ 50) insetBy: 10) width") == 80
            && number("((0 @ 0 corner: 100 @ 50) translatedBy: 5 @ 5) origin x") == 5,
        "a Rectangle's sides, extent and centre");
    require(evaluate("(0 @ 0 corner: 100 @ 50) containsPoint: 50 @ 25").boolean
            && !evaluate("(0 @ 0 corner: 100 @ 50) containsPoint: 100 @ 25").boolean
            && evaluate("(0 @ 0 corner: 1 @ 1) = (Rectangle origin: 0 @ 0 extent: 1 @ 1)").boolean
            && evaluate("(0 @ 0 corner: 100 @ 50) printString").text == "0 @ 0 corner: 100 @ 50",
        "a Rectangle should hold its points, compare and print");
    require(number("| bnds pt | bnds := 100 @ 50 corner: 300 @ 150. pt := 200 @ 75.\n"
                   "pt := pt - bnds topLeft / bnds extent asFloatPoint. pt y")
            == 0.25,
        "the cursor point relative to the bounds should run from 0 to 1");
}

// PrintString, which any object answers, and
// printOn:, which writes it on a stream; an Association, an object -> another,
// and a BindingList of them; and Points, Dates and Times made by the
// thousand, alike as keys when equal.
void testPrintStringsAndAssociations()
{
    const auto text = [](std::string_view source) {
        const auto value = evaluate(source);
        require(value.kind == st::Value::Kind::String, std::string{source} + " should be a string");
        return value.text;
    };
    require(text("1 printString") == "1" && text("0.25 printString") == "0.25" && text("440 hz printString") == "440 hz"
            && text("'it''s' printString") == "'it''s'" && text("'it''s' displayString") == "it's"
            && text("#abc printString") == "abc" && text("nil printString") == "nil"
            && text("true printString") == "true" && text("#(1 $a 'b' #c) printString") == "(1 'a' 'b' c )"
            && text("#({2+5} #(a b c)) printString") == "(7 (a b c ) )" && text("(3 @ 4) printString") == "3 @ 4"
            && text("[5 + 6] printString") == "BlockClosure [] in UndefinedObject>>unboundMethod",
        "printString should print numbers, strings, Symbols, Arrays without their sharp sign, "
        "Points and blocks");
    require(text("(Time fromSeconds: 3909) printString") == "1:05:09 am"
            && text("(Time fromSeconds: 13 * 3600) printString") == "1:00:00 pm"
            && text("(Time fromSeconds: 0) printString") == "12:00:00 am"
            && text("(Date newDay: 296 year: 2026) printString") == "October 23, 2026",
        "a Time and a Date should print as VisualWorks prints them");
    require(text("(OrderedCollection new add: 1; add: 'two'; yourself) printString") == "OrderedCollection (1 'two' )"
            && text("(Dictionary new at: #a put: 1; yourself) printString") == "Dictionary (a->1 )",
        "collections should print their elements");
    require(text("| s | s := WriteStream on: String new. 42 printOn: s. 'x' printOn: s. s contents") == "42'x'"
            && text("| s | s := WriteStream on: String new. (Time fromSeconds: 60) printOn: s. s contents")
                == "12:01:00 am",
        "printOn: should write the printString on the stream");
    require(text("(#a -> 3) printString") == "a->3" && evaluate("(#a -> 3) value").number == 3
            && evaluate("(#a -> 3) key").text == "a" && evaluate("(#a -> 3) = (#a -> 3)").boolean,
        "an Association should hold its key and its value");
    const auto bindings = evaluate("BindingList with: (?sample1 -> 'alien threat.aif') with: (?sample2 -> 'siren.aif')");
    require(bindings.kind == st::Value::Kind::Array && bindings.elements->size() == 2
            && evaluate("(BindingList new) isEmpty").boolean,
        "a BindingList should be a collection of Associations");
    require(evaluate("(BindingList with: (?sample1 -> 'x')) first key").text == "@sample1"
            && [] {
                   SoundHost host;
                   return !runOn(host, "BindingList with: 3").ok();
               }(),
        "a BindingList should hold only Associations");
    require(evaluate("| p t | 1 to: 70000 do: [:i | p := i @ i. t := Time fromSeconds: i]. p x + t asSeconds").number
            == 140000,
        "a script should make as many Points and Times as it likes");
    require(evaluate("| d | d := Dictionary new. d at: 1 @ 2 put: 5. d at: 1 @ 2").number == 5
            && evaluate("(Set new add: 1 @ 2; add: 1 @ 2; yourself) size").number == 1,
        "equal Points should be alike as keys");
}

// A session's programs share the objects and blocks
// kept in the host's variables, as a Tool's responses do from step to step:
// a Random draws on, a block stored in one program runs in the next, and a
// collection keeps its elements.
void testSessionsKeepObjectsBetweenRuns()
{
    struct Variables final : st::Host
    {
        std::map<std::string, st::Value, std::less<>> values;
        std::optional<st::Value> variable(std::string_view name) override
        {
            const auto found = values.find(name);
            return found == values.end() ? std::nullopt : std::optional{found->second};
        }
        bool assign(std::string_view name, const st::Value& value) override
        {
            values.insert_or_assign(std::string{name}, value);
            return true;
        }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "none";
            return std::nullopt;
        }
    } host;
    st::Session session(host);
    const auto run = [&](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), std::string{source} + " should parse");
        const auto result = session.run(parsed.program);
        require(result.ok(), std::string{source} + " should run: " + (result.ok() ? "" : result.diagnostic->message));
        return result.value;
    };
    run("rando := Random newForTalasWithSeed: 7. counter := 0. double := [:x | x * 2]. points := OrderedCollection new");
    const auto first = run("rando next");
    const auto second = run("rando next");
    require(first.kind == st::Value::Kind::Number && second.kind == st::Value::Kind::Number && first.number != second.number
            && first.number >= 0.0 && first.number < 1.0,
        "a Random kept between runs should draw on");
    require(run("double value: 21").number == 42.0, "a block stored in one run should run in the next");
    run("counter := counter + 1");
    run("counter := counter + 1. points add: counter");
    require(run("counter").number == 2.0 && run("points size").number == 1.0 && run("points first").number == 2.0,
        "variables and collections should keep their values between runs");
}

// A stream's elements read and written as bytes,
// most significant first or, with LE, least, signed or not, and strings
// ending in a 0 or led by their length; padding to a boundary.
void testBinaryStreams()
{
    const auto text = [](std::string_view source) { return st::fieldText(evaluate(source)).value_or("?"); };
    require(text("#(1 2 3 4) readStream nextWord") == "258" && text("#(1 2) readStream nextWordLE") == "513"
            && text("#(255) readStream nextSigned8Bit") == "-1" && text("#(255 254) readStream nextSigned16Bit") == "-2"
            && text("#(254 255) readStream nextSigned16BitLE") == "-2" && text("#(1 2 3) readStream next24Bit") == "66051"
            && text("#(1 2 3) readStream next24BitLE") == "197121" && text("#(255 255 255) readStream nextSigned24Bit") == "-1"
            && text("#(255 255 128) readStream nextSigned24BitLE") == "-8323073" && text("#(0 0 1 0) readStream next32Bit") == "256"
            && text("#(255 255 255 255) readStream nextLong") == "-1" && text("#(1 0 0 0) readStream nextLongLE") == "1",
        "bytes read as words, 24-bit and 32-bit integers, signed and unsigned");
    require(text("#(104 105 0 33) readStream nextCString") == "hi" && text("#(2 104 105) readStream nextString") == "hi"
            && text("| s | s := #(1 2 3 4) readStream. s nextWord. s nextWord") == "772",
        "strings ending in 0 or led by their length, and reading on");
    // Extended floats, as an AIFF header's 44100 Hz, the
    // 96-bit form padded after the exponent.
    require(evaluate("(#(64 14 172 68 0 0 0 0 0 0) readStream next80BitFloat) = 44100").boolean
            && evaluate("(#(64 14 0 0 172 68 0 0 0 0 0 0) readStream next96BitFloat) = 44100").boolean
            && evaluate("(#(192 14 0 0 172 68 0 0 0 0 0 0) readStream next96BitDouble) = -44100").boolean
            && evaluate("(#(63 255 128 0 0 0 0 0 0 0) readStream next80BitFloat) = 1").boolean,
        "next80BitFloat and the 96-bit readers should read extended floats");
    SoundHost shortHost;
    require(!runOn(shortHost, "#(64 14 172) readStream next80BitFloat").ok(), "too few bytes for an extended float should fail");
    const auto written = [&](std::string_view statements) {
        return text("| w | w := WriteStream on: #(). " + std::string{statements} + " w contents");
    };
    require(written("w nextWordPut: 258.") == "#(1 2)" && written("w nextWordLEPut: 258.") == "#(2 1)"
            && written("w next24BitPut: 66051.") == "#(1 2 3)" && written("w next32BitPut: 256.") == "#(0 0 1 0)"
            && written("w nextLongPut: -1.") == "#(255 255 255 255)" && written("w nextLongLEPut: 1.") == "#(1 0 0 0)"
            && written("w nextCStringPut: 'hi'.") == "#(104 105 0)" && written("w nextStringPut: 'hi'.") == "#(2 104 105)"
            && written("w nextPut: 1. w padTo: 4.") == "#(1 0 0 0)"
            && text("| w | w := WriteStream on: #(). w nextPut: 1. w padTo: 4") == "3",
        "integers and strings written as bytes, and padding to a boundary");
    require(text("| w | w := WriteStream on: #(). w nextLongPut: -123456. w contents readStream nextLong") == "-123456",
        "what is written as bytes should read back");
}

// A sample file the host knows, by its name, tells
// its frames, duration and sample rate, and its header's loop, pitch and key
// range; one it does not know is refused, or its cancel block evaluated.
void testSamplesFiles()
{
    struct Samples final : st::Host
    {
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "none";
            return std::nullopt;
        }
        std::optional<st::SampleFileInfo> sampleFile(std::string_view name) override
        {
            if (name != "bell.aif")
                return std::nullopt;
            st::SampleFileInfo file{"Samples/bell.aif", 24000.0, 48000.0, talas::core::SampleLoop{1000, 5000},
                talas::core::SamplePitch{69, 0}, talas::core::SampleKeyRange{60, 72, 10, 100}};
            file.markers = {{"attack", 100}, {"release", 20000}, {"attack", 300}};
            return file;
        }
        double sampleRate() override { return 48000.0; }
    } host;
    const auto ran = [&](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), std::string{source} + " should parse");
        return st::run(*parsed.program, host);
    };
    const auto number = [&](std::string_view source) {
        const auto result = ran(source);
        require(result.ok() && result.value.kind == st::Value::Kind::Number, std::string{source} + " should be a number");
        return result.value.number;
    };
    const std::string bell = "'bell.aif' asSampleFile ";
    require(number(bell + "sampleFrames") == 24000 && number(bell + "fileDuration") == 0.5
            && number(bell + "uiSampleRate") == 48000 && number(bell + "uiLoopStart") == 1000
            && number(bell + "uiLoopEnd") == 4999 && number(bell + "uiBasePitch") == 69 && number(bell + "uiLowPitch") == 60
            && number(bell + "uiHighPitch") == 72 && std::abs(number(bell + "uiLowVelocity") - 10.0 / 127.0) < 1.0e-12
            && std::abs(number(bell + "uiHighVelocity") - 100.0 / 127.0) < 1.0e-12,
        "a SamplesFile's frames, duration, sample rate, loop, pitch and key range");
    const auto loops = ran(bell + "hasLoop");
    const auto valid = ran(bell + "fileExistsAndIsValid");
    const auto tail = ran("(" + bell + "filename) displayTail");
    require(loops.ok() && loops.value.boolean && valid.ok() && valid.value.boolean && tail.ok() && tail.value.text == "bell.aif",
        "a SamplesFile's loop and its Filename");
    require(number("SamplesFile fromOldFileNamed: 'bell.aif' ifCancelled: [0]. 1") == 1
            && number("(SamplesFile fromOldFileNamed: 'gone.aif' ifCancelled: [7])") == 7 && !ran("'gone.aif' asSampleFile").ok(),
        "a file not found should evaluate the cancel block, or be refused");
    // Markers maps each marker's Symbol to its frame, a
    // name that comes twice keeping its first.
    require(number("(" + bell + "markers) at: #attack") == 100 && number("(" + bell + "markers) at: #release") == 20000
            && number("(" + bell + "markers) size") == 2,
        "a SamplesFile's markers should be a Dictionary of their Symbols and frames");
    // A file's length in samples becomes a time at the
    // host's sample rate, as theFile sampleFrames samp s.
    require(number(bell + "sampleFrames samp s") == 0.5 && number("0.25 s samp") == 12000
            && number("24000 samp ms") == 500,
        "samples should become a time at the host's sample rate, and back");
    struct Rateless final : st::Host
    {
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "none";
            return std::nullopt;
        }
    } rateless;
    const auto parsed = st::parse("1000 samp s");
    require(parsed.ok() && !st::run(*parsed.program, rateless).ok(),
        "a host without a sample rate should refuse samples as a time");

    // A SamplesFile reads a mono file's samples, and finds
    // itself again, asking where it went, or evaluates the cancel block.
    struct Locating final : st::Host
    {
        int bellLookups = -1;
        st::PromptAnswer answer;
        std::vector<st::Prompt> asked;
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "none";
            return std::nullopt;
        }
        std::optional<st::SampleFileInfo> sampleFile(std::string_view name) override
        {
            if (name == "stereo.aif")
                return st::SampleFileInfo{"stereo.aif", 4.0, 48000.0};
            if (name == "other.aif")
                return st::SampleFileInfo{"other.aif", 100.0, 48000.0};
            if (name != "bell.aif" || bellLookups == 0)
                return std::nullopt;
            if (bellLookups > 0)
                --bellLookups;
            st::SampleFileInfo info{"bell.aif", 4.0, 48000.0};
            info.monoSamples = [](std::size_t start, std::size_t length) -> std::optional<std::vector<double>> {
                const std::vector<double> samples{0.0, 0.5, -0.5, 1.0};
                if (start > samples.size() || length > samples.size() - start)
                    return std::nullopt;
                return std::vector<double>(samples.begin() + static_cast<std::ptrdiff_t>(start),
                    samples.begin() + static_cast<std::ptrdiff_t>(start + length));
            };
            return info;
        }
        std::optional<st::PromptAnswer> ask(const st::Prompt& prompt) override
        {
            asked.push_back(prompt);
            return answer;
        }
    } locating;
    const auto located = [&](std::string_view source) {
        const auto program = st::parse(source);
        require(program.ok(), std::string{source} + " should parse");
        return st::run(*program.program, locating);
    };
    const auto read = located("'bell.aif' asSampleFile getSamplesFromMonoWavetableFileStartingAt: 1 length: 2");
    const auto past = located("'bell.aif' asSampleFile getSamplesFromMonoWavetableFileStartingAt: 3 length: 5");
    const auto stereo = located("'stereo.aif' asSampleFile getSamplesFromMonoWavetableFileStartingAt: 0 length: 1");
    require(read.ok() && read.value.kind == st::Value::Kind::Array && read.value.elements->size() == 2
            && read.value.elements->at(0).number == 0.5 && read.value.elements->at(1).number == -0.5 && past.ok()
            && past.value.kind == st::Value::Kind::Nil && stereo.ok() && stereo.value.kind == st::Value::Kind::Nil,
        "a mono file's samples from a start for a length, or nil where they cannot be read");
    const auto stillThere = located("('bell.aif' asSampleFile locateFileIfCancelled: [7]) sampleFrames");
    require(stillThere.ok() && stillThere.value.number == 4 && locating.asked.empty(),
        "a file still where it was should be found again without asking");
    locating.bellLookups = 1;
    locating.answer = {true, "other.aif"};
    const auto moved = located("('bell.aif' asSampleFile locateFileIfCancelled: [7]) sampleFrames");
    require(moved.ok() && moved.value.number == 100 && locating.asked.size() == 1
            && locating.asked[0].kind == st::PromptKind::OpenFile && locating.asked[0].question == "Locate bell.aif",
        "a file gone should ask where it is, and answer the file chosen");
    locating.bellLookups = 1;
    locating.answer = {};
    const auto cancelled = located("'bell.aif' asSampleFile locateFileIfCancelled: [7]");
    require(cancelled.ok() && cancelled.value.number == 7 && locating.asked.size() == 2,
        "Cancel while locating should evaluate the cancel block");
}

// Messages that find nothing raise an error
// or answer 0, ifFalse: answers its block's value when the receiver is
// false, a replacement keeps within its receiver's bounds, and printOn:
// abbreviates a long collection where printAllOn: prints it all.
void testQuickReferenceFailuresAndPrinting()
{
    SoundHost host;
    require(!runOn(host, "| d | d := Dictionary new. d at: #a put: 1. d keyAtValue: 2").ok()
            && !runOn(host, "| d | d := Dictionary new. d at: #a put: 1. d removeKey: #b").ok()
            && !runOn(host, "#(1 2 3) copyReplaceFrom: 2 to: 5 with: #(9)").ok(),
        "keyAtValue: and removeKey: should fail for what is not there, a replacement past the end");
    require(near(evaluate("#(1 2 3) lastIndexOf: 7"), 0.0, talas::expr::Unit::None)
            && near(evaluate("false ifFalse: [3]"), 3.0, talas::expr::Unit::None)
            && near(evaluate("| s | s := Set new. s add: 3; add: 4. s remove: 3. s size"), 1.0, talas::expr::Unit::None),
        "lastIndexOf: answers 0, ifFalse: its block, and remove: takes the element out");
    require(textIs(evaluate("| w | w := WriteStream on: String new. #(1 2) printAllOn: w. w contents"), "(1 2 )"),
        "printAllOn: should print the elements on the stream");
    // A printed collection longer than a stream holds is abbreviated.
    const auto some = evaluate("(1 to: 20000) printString");
    require(textIs(evaluate("| w | w := WriteStream on: String new. #(1 2) printOn: w. w contents"), "(1 2 )")
            && some.kind == st::Value::Kind::String && some.text.ends_with("...etc... )") && some.text.size() < 70000,
        "printOn: should print the elements, abbreviating a long collection");
}

// Integers have infinite precision, as 1000
// factorial shows, and a ratio of two integers stays exact, reduced but
// never made floating point: 3/2 stays a fraction and
// 4/2 is the integer 2. A host of a project before format 121 keeps every
// number floating point.
void testExactIntegersAndRatios()
{
    // A period ends a statement.
    require(near(evaluate("3. 4"), 4.0, talas::expr::Unit::None), "a period should end a statement");
    const auto text = [](std::string_view source, std::string_view expected) {
        const auto value = evaluate(source);
        require(textIs(value, expected), std::string{source} + " should be " + std::string{expected} + ", not "
                + (value.kind == st::Value::Kind::String ? value.text : std::string{"no string"}));
    };
    const auto yes = [](std::string_view source) { return evaluate(source).boolean; };
    const auto factorial = evaluate("1000 factorial printString");
    require(factorial.kind == st::Value::Kind::String && factorial.text.size() == 2568
            && factorial.text.starts_with("402387260077093773543702433923003985719374864210")
            && factorial.text.ends_with("000000"),
        "1000 factorial should have all of its 2568 digits");
    text("(2 raisedTo: 100) printString", "1267650600228229401496703205376");
    text("(100 factorial / 98 factorial) printString", "9900");
    text("(12345678901234567890 * 98765432109876543210) printString", "1219326311370217952237463801111263526900");
    text("(1024/3024) printString", "64/189");
    text("(3/2) printString", "3/2");
    text("(4/2) printString", "2");
    text("((1/3) + (1/6)) printString", "1/2");
    text("(-7/2) printString", "-7/2");
    text("((2/3) raisedTo: -2) printString", "9/4");
    text("{1. 1/2} printString", "(1 1/2 )");
    text("(16rFFFFFFFFFFFFFFFFFF + 1) printString", "4722366482869645213696");
    text("((2 raisedTo: 70) printStringRadix: 16)", "400000000000000000");
    require(yes("(4/2) isInteger") && !yes("(3/2) isInteger") && yes("(3/2) isFraction") && !yes("2 isFraction")
            && yes("((1/3) * 3) = 1") && yes("((1/10) + (2/10)) = (3/10)") && !yes("(0.1 + 0.2) = 0.3")
            && yes("(1/3) < (1/2)") && yes("((2 raisedTo: 64) + 1) > (2 raisedTo: 64)"),
        "ratios should be exact, compare exactly and reduce to integers");
    require(near(evaluate("(3/2) numerator"), 3.0, talas::expr::Unit::None)
            && near(evaluate("(6/4) denominator"), 2.0, talas::expr::Unit::None)
            && near(evaluate("(7/2) truncated"), 3.0, talas::expr::Unit::None)
            && near(evaluate("(-7/2) floor"), -4.0, talas::expr::Unit::None)
            && near(evaluate("(-7/2) ceiling"), -3.0, talas::expr::Unit::None)
            && near(evaluate("(7/2) rounded"), 4.0, talas::expr::Unit::None)
            && near(evaluate("(-7/2) rounded"), -4.0, talas::expr::Unit::None)
            && near(evaluate("(2/3) asFloat"), 2.0 / 3.0, talas::expr::Unit::None)
            && near(evaluate("(1/4) + 0.5"), 0.75, talas::expr::Unit::None)
            && near(evaluate("(7/2) \\\\ 1"), 0.5, talas::expr::Unit::None)
            && near(evaluate("(-7/2) // 1"), -4.0, talas::expr::Unit::None),
        "a ratio's parts, its integers near it, and floating point when mixed with a Float");
    require(evaluate("(1/3) + 0.5").exact == nullptr && evaluate("(1/3) * 3").exact != nullptr,
        "a ratio with a floating point number should be floating point, with an integer exact");
    // Intervals and loops over exact numbers stay exact.
    text("((1 to: 2 by: 1/2) collect: [:x | x]) printString", "(1 3/2 2 )");
    text("| s | s := 0. 1 to: 4 do: [:i | s := s + (1 / i)]. s printString", "25/12");
    struct FloatingHost final : st::Host
    {
        std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>,
            std::string& error) override
        {
            error = "no objects";
            return std::nullopt;
        }
        bool exactNumbers() override { return false; }
    } floating;
    const auto parsed = st::parse("((1/3) + (1/6)) printString");
    const auto ran = st::run(*parsed.program, floating);
    require(ran.ok() && ran.value.kind == st::Value::Kind::String && ran.value.text == "0.5",
        "a host of a project before format 121 should keep its numbers floating point");
    const auto refused = st::run(*st::parse("19 factorial").program, floating);
    require(!refused.ok(), "a host of a project before format 121 should keep factorial to 18");
}

// A String is an Array of Characters; a comment in double quotes is left
// out; a variable's name begins with a letter and holds only letters and
// digits, and lasts only as long as its own program; code run on a
// condition is a block; the Smalltalk subset makes instances of its classes
// but defines no new ones; inject:into: starts from its value; a
// Dictionary refuses a key it lacks; collections are equal when of one
// kind, size and elements; indexOfSubCollection:startingAt: answers 0 for
// no match; and a binary selector has a Symbol, #+.
void testSmalltalkChapterRules()
{
    const auto runs = [](std::string_view source) {
        const auto parsed = st::parse(source);
        if (!parsed.ok())
            return false;
        SoundHost host;
        return runOn(host, source).ok();
    };
    require(evaluate("'abc' at: 2").text == "b" && near(evaluate("'abc' size"), 3.0, talas::expr::Unit::None),
        "a String should be an Array of its Characters");
    require(near(evaluate("3 \"+ 4\" + 1"), 4.0, talas::expr::Unit::None), "a comment should be left out");
    require(!st::parse("| 2x | 2x := 1").ok()
            && near(evaluate("| r2d2 x theThunderthief siliconNectar | r2d2 := 1. x := 2. siliconNectar := r2d2 + x"),
                3.0, talas::expr::Unit::None),
        "a variable should begin with a letter and hold only letters and digits");
    require(near(evaluate("| x | x := 5. x"), 5.0, talas::expr::Unit::None) && !runs("x + 1"),
        "a variable should last only as long as its own program");
    require(near(evaluate("true ifTrue: [7]"), 7.0, talas::expr::Unit::None) && !runs("true ifTrue: 3"),
        "code run on a condition should be a block, true ifTrue: answering its value");
    require(!runs("Object subclass: #Foo") && evaluate("OrderedCollection new size").number == 0.0,
        "the Smalltalk subset should make instances but define no new classes");
    require(near(evaluate("#(1 2 3) inject: 10 into: [:a :b | a + b]"), 16.0, talas::expr::Unit::None),
        "inject:into: should start from its value");
    require(!runs("| d | d := Dictionary new. d at: #a put: 1. d at: #b"),
        "a Dictionary should refuse a key it does not hold");
    require(evaluate("#(1 2 3) = #(1 2 3)").boolean && !evaluate("#(1 2) = #(1 2 3)").boolean
            && !evaluate("| c | c := OrderedCollection new. c add: 1. c add: 2. #(1 2) = c").boolean,
        "collections should be equal only when of one kind, size and elements");
    require(near(evaluate("'hello' indexOfSubCollection: 'xyz' startingAt: 1"), 0.0, talas::expr::Unit::None)
            && near(evaluate("'hello' indexOfSubCollection: 'll' startingAt: 1"), 3.0, talas::expr::Unit::None),
        "indexOfSubCollection:startingAt: should answer 0 for no match");
    const auto plus = evaluate("#+");
    require(plus.kind == st::Value::Kind::Symbol && plus.text == "+" && near(evaluate("3 perform: #+ with: 4"), 7.0,
                                                                            talas::expr::Unit::None),
        "a binary selector should have a Symbol, #+");
}

// A character's code, as f next asInteger reads a
// text's characters; a Symbol quoted, #'.'; a CSound octave point pitch
// class; and a text's next line as its numbers and Symbols.
void testScoreReadingMessages()
{
    using Unit = talas::expr::Unit;
    require(near(evaluate("$a asInteger"), 97.0, Unit::None) && near(evaluate("'H' asInteger"), 72.0, Unit::None)
            && near(evaluate("$  value"), 32.0, Unit::None),
        "a character's asInteger should be its ASCII number");
    require(near(evaluate("8.09 octavePointPitchClass"), 69.0, Unit::NoteNumber)
            && near(evaluate("9.00 octavePointPitchClass"), 72.0, Unit::NoteNumber)
            && near(evaluate("7.115 octavePointPitchClass"), 59.5, Unit::NoteNumber),
        "8.09 should be the a above middle c");
    const auto dot = evaluate("#'.'");
    require(dot.kind == st::Value::Kind::Symbol && dot.text == "." && evaluate("#'.' == #'.'").boolean,
        "a Symbol may be quoted");
    const auto line = evaluate("| s | s := 'i 1  8.09 e\n. 2' readStream. s nextParameters");
    require(line.kind == st::Value::Kind::Array && line.elements->size() == 4
            && (*line.elements)[0].kind == st::Value::Kind::Symbol && (*line.elements)[0].text == "i"
            && near((*line.elements)[1], 1.0, Unit::None) && near((*line.elements)[2], 8.09, Unit::None)
            && (*line.elements)[3].kind == st::Value::Kind::Symbol,
        "nextParameters should read a line's numbers and words");
    const auto next = evaluate("| s | s := 'i 1\n. 2' readStream. s nextParameters. s nextParameters");
    require(next.kind == st::Value::Kind::Array && next.elements->size() == 2 && (*next.elements)[0].text == ".",
        "each nextParameters should read the next line");
}

// A String names a Filename, whose head, tail,
// extension and separator it answers, and the names it makes within it; a
// String's extension; a String as an Event Value. Scripts change no files.
void testFilenames()
{
    const auto text = [](std::string_view source) {
        const auto value = evaluate(source);
        return value.kind == st::Value::Kind::Nil   ? std::string{"nil"}
            : value.kind == st::Value::Kind::Boolean ? std::string{value.boolean ? "true" : "false"}
                                                     : st::fieldText(value).value_or("?");
    };
    const std::string bell = "('/Samples/bell.aif' asFilename) ";
    require(text(bell + "displayTail") == "bell.aif" && text(bell + "displayHead") == "/Samples"
            && text(bell + "displayTailWithoutFileExtension") == "bell" && text(bell + "fileExtension") == ".aif"
            && text(bell + "asDisplayString") == "/Samples/bell.aif" && text(bell + "separator") == "/"
            && text(bell + "directory displayTail") == "Samples",
        "a Filename's head, tail, extension and separator");
    require(text("('/Samples' asFilename construct: 'bell.aif') asDisplayString") == "/Samples/bell.aif"
            && text("'/Samples' asFilename constructString: 'bell.aif'") == "/Samples/bell.aif"
            && text("(('/Samples' asFilename construct: 'bell.aif') = ('/Samples/bell.aif' asFilename))") == "true",
        "the names a Filename makes within it");
    require(text(bell + "= '/Samples/bell.aif'") == "true" && text(bell + "= '/Samples/gong.aif'") == "false",
        "a Filename should be = to the String of its name, character for character");
    require(text("'bell.aif' fileExtension") == ".aif" && text("'bell' fileExtension") == "nil"
            && text("'Amp' asHotValue") == "!Amp",
        "a String's extension, and a String as an Event Value");
    for (const auto* refused : {"'2x' asHotValue", "('/tmp/talas-script-test' asFilename) delete",
             "('/tmp/talas-script-test' asFilename) makeDirectory"})
    {
        const auto parsed = st::parse(refused);
        struct NoHost final : st::Host
        {
            std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
            std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
            {
                error = "none";
                return std::nullopt;
            }
        } host;
        require(parsed.ok() && !st::run(*parsed.program, host).ok(), std::string{refused} + " should be refused");
    }
}

// Eqv: and xor:, an Array of four with:s, the
// identity, next and previous index of an element, an element chosen by a
// Random, adding next to an element or a position in an OrderedCollection
// and removing at one, and where a SortedCollection would put an element.
void testMoreCollectionMessages()
{
    const auto text = [](std::string_view source) {
        const auto value = evaluate(source);
        return st::fieldText(value).value_or(value.kind == st::Value::Kind::Nil ? "nil"
                : value.kind == st::Value::Kind::Boolean ? (value.boolean ? "true" : "false") : "?");
    };
    require(text("true eqv: true") == "true" && text("true eqv: false") == "false" && text("false eqv: false") == "true"
            && text("true xor: false") == "true" && text("true xor: true") == "false",
        "eqv: is both alike, xor: not");
    require(text("Array with: 1 with: 2 with: 3 with: 4") == "#(1 2 3 4)" && text("Array with: 7") == "#(7)",
        "a collection of with:s");
    require(text("#(3 4 5) identityIndexOf: 4") == "2" && text("#(3 4 5) identityIndexOf: 9") == "0"
            && text("#(3 4 5 4) nextIndexOf: 4 from: 3 to: 4") == "4" && text("#(3 4 5 4) nextIndexOf: 4 from: 3 to: 3") == "nil"
            && text("#(3 4 5 4) prevIndexOf: 4 from: 1 to: 3") == "2" && text("#(3 4 5 4) prevIndexOf: 3 from: 2 to: 4") == "nil",
        "an element's identity, next and previous index");
    const auto chosen = text("#(3 4 5) selectRandomElementUsing: (Random newForTalasWithSeed: 3)");
    require((chosen == "3" || chosen == "4" || chosen == "5")
            && chosen == text("#(3 4 5) selectRandomElementUsing: (Random newForTalasWithSeed: 3)"),
        "an element chosen by a seeded Random");
    const std::string collection = "| c | c := OrderedCollection new. c add: 1; add: 3. ";
    require(text(collection + "c add: 2 after: 1. c asArray") == "#(1 2 3)"
            && text(collection + "c add: 2 before: 3. c asArray") == "#(1 2 3)"
            && text(collection + "c add: 2 afterIndex: 1. c asArray") == "#(1 2 3)"
            && text(collection + "c add: 0 afterIndex: 0. c asArray") == "#(0 1 3)"
            && text(collection + "c add: 2 beforeIndex: 2. c asArray") == "#(1 2 3)"
            && text(collection + "c removeAtIndex: 1. c asArray") == "#(3)"
            && text(collection + "c addAllLast: #(5 7). c asArray") == "#(1 3 5 7)",
        "adding next to an element or a position, and removing at one");
    require(text("| c | c := SortedCollection new. c add: 1; add: 3. c whereToInsert: 2") == "2"
            && text("| c | c := SortedCollection sortBlock: [:a :b | a > b]. c add: 3; add: 1; add: 2. c asArray") == "#(3 2 1)"
            && text("| c | c := SortedCollection new. c add: 3; add: 1; add: 2. c asArray") == "#(1 2 3)",
        "a SortedCollection's order and where it would put an element");
}

// 3 % 4 is a complex number, with its parts, angle,
// magnitude and conjugate, a square root, and arithmetic with numbers and
// other complex numbers; a real number's imaginary part is 0, and a
// negative one has a complex square root.
void testComplexNumbers()
{
    const auto number = [](std::string_view source) {
        const auto value = evaluate(source);
        require(value.kind == st::Value::Kind::Number, std::string{source} + " should be a number");
        return value.number;
    };
    const auto truth = [](std::string_view source) {
        const auto value = evaluate(source);
        return value.kind == st::Value::Kind::Boolean && value.boolean;
    };
    require(number("(3 % 4) real") == 3 && number("(3 % 4) imaginary") == 4 && number("(3 % 4) magnitudeSquared") == 25
            && number("(3 % 4) abs") == 5 && number("(3 % 4) conjugated imaginary") == -4
            && std::abs(number("(3 % 4) angle") - std::atan2(4.0, 3.0)) < 1.0e-12,
        "a complex number's parts, magnitude, conjugate and angle");
    require(number("((3 % 4) * (1 % 2)) real") == -5 && number("((3 % 4) * (1 % 2)) imaginary") == 10
            && number("(1 + (3 % 4)) real") == 4 && number("((3 % 4) - 1) real") == 2
            && number("((3 % 4) / (3 % 4)) real") == 1 && number("((3 % 4) / (3 % 4)) imaginary") == 0,
        "complex arithmetic");
    require(number("-4 complexSqrt imaginary") == 2 && number("-4 complexSqrt real") == 0 && number("4 complexSqrt") == 2
            && std::abs(number("((3 % 4) complexSqrt * (3 % 4) complexSqrt) imaginary") - 4.0) < 1.0e-12,
        "square roots, complex for a negative number");
    require(truth("(3 % 4) isComplex") && !truth("3 isComplex") && number("3 real") == 3 && number("3 imaginary") == 0
            && number("3 conjugated") == 3 && number("-2 angle") == std::numbers::pi && number("2.5 asDouble") == 2.5
            && truth("(3 % 4) = (3 % 4)") && !truth("(3 % 4) = (4 % 3)"),
        "a real number's parts, and comparing complex numbers");
}

// Dates count days from 1901 and Times seconds
// from midnight: a day of a year, its month, its place in the week and the
// year, days added and subtracted, the days between two Dates, and a Time's
// hours, minutes and seconds, added to and compared.
void testDatesAndTimes()
{
    const auto number = [](std::string_view source) {
        const auto value = evaluate(source);
        require(value.kind == st::Value::Kind::Number, std::string{source} + " should be a number");
        return value.number;
    };
    const auto truth = [](std::string_view source) {
        const auto value = evaluate(source);
        return value.kind == st::Value::Kind::Boolean && value.boolean;
    };
    const std::string day = "(Date newDay: 296 year: 2026) ";
    require(number(day + "dayOfMonth") == 23 && number(day + "daysInMonth") == 31 && number(day + "day") == 296
            && number(day + "firstDayOfMonth") == 274 && number(day + "daysLeftInYear") == 69
            && number(day + "daysInYear") == 365 && number(day + "year") == 2026 && number(day + "weekdayIndex") == 5,
        "the 296th day of 2026 should be Friday 23 October");
    require(number(day + "subtractDate: (Date newDay: 1 year: 2026)") == 295
            && number("(" + day + "addDays: 10) dayOfMonth") == 2
            && number("(" + day + "subtractDays: 296) year") == 2025 && number("(" + day + "subtractDays: 296) day") == 365,
        "days added to and subtracted from a Date, and between two");
    require(number("(Date newDay: 1 year: 1901) asSeconds") == 0 && number("(Date newDay: 2 year: 1901) asSeconds") == 86400
            && number("(Date newDay: 1 year: 2024) daysInYear") == 366 && number("(Date newDay: 60 year: 2024) dayOfMonth") == 29
            && number("(Date newDay: 1 year: 1901) weekdayIndex") == 2,
        "seconds since 1901 began, leap years, and Tuesday 1 January 1901");
    require(truth("(Date newDay: 5 year: 2026) < (Date newDay: 6 year: 2026)")
            && !truth("(Date newDay: 6 year: 2026) < (Date newDay: 5 year: 2026)")
            && truth("(Date newDay: 5 year: 2026) = (Date newDay: 5 year: 2026)")
            && !truth("(Date newDay: 5 year: 2026) = (Date newDay: 6 year: 2026)"),
        "Dates compare by their days");
    const std::string time = "(Time fromSeconds: 3725) ";
    require(number(time + "hours") == 1 && number(time + "minutes") == 2 && number(time + "seconds") == 5
            && number(time + "asSeconds") == 3725
            && number("(" + time + "addTime: (Time fromSeconds: 60)) asSeconds") == 3785
            && number("(" + time + "subtractTime: (Time fromSeconds: 25)) asSeconds") == 3700
            && number("(" + time + "addTime: (Date newDay: 2 year: 1901)) asSeconds") == 3725 + 86400
            && truth("(Time fromSeconds: 1) < (Time fromSeconds: 2)") && truth(time + "= (Time fromSeconds: 3725)"),
        "a Time's hours, minutes and seconds, added to, subtracted from and compared");
    require(number("Date today year") >= 2026 && number("Time now asSeconds") >= 0 && number("Time now asSeconds") < 86400
            && number("Date dateAndTimeNow size") == 2 && number("Time millisecondClockValue") >= 0,
        "today, now, both together and the millisecond clock");
}

// A Smalltalk program in a field gives the
// Val expression it builds: the Fibonacci triggers' collection,
// stepped through by a metronome counting modulo its size, becomes an
// Array Val indexes; a keyword message to a number with an Event Value
// is Val's; and Float phi is the golden ratio. Val's own programs
// and plain expressions are left to Val.
void testFieldProgramsBuildVal()
{
    const std::string fibonacci = "| pattern on off fib fib1 fib2| pattern := OrderedCollection new. "
                                  "on := #(-1 1). off := #(0 0). fib1 := 1. fib2 := 0. fib := 0.\n"
                                  "8 timesRepeat: [ fib timesRepeat: [pattern addAll: off]. pattern addAll: on. "
                                  "fib := fib1 + fib2. fib2 := fib1. fib1 := fib].\n"
                                  "((1 bpm: !BPM * 2) countTriggersMod: pattern size) of: pattern";
    const auto built = st::fieldProgramText(fibonacci);
    require(built.has_value() && built->find("bpm:") != std::string::npos
            && built->find("countTriggersMod: (122)") != std::string::npos
            && built->find("of: #(-1 1 0 0 -1 1 0 0 0 0 -1 1 0 0 0 0 0 0 -1 1 0 0 0 0 0 0 0 0 0 0 -1 1") != std::string::npos,
        "the Fibonacci program should build a Val expression: " + built.value_or("<none>"));
    // Five rounds fill the 32 elements a Val Array holds here.
    auto shorter = fibonacci;
    shorter.replace(shorter.find("8 timesRepeat:"), 1, "5");
    const auto played = st::fieldProgramText(shorter);
    require(played.has_value() && played->find("countTriggersMod: (32)") != std::string::npos
            && talas::expr::parse(*played, talas::expr::ExpressionLanguageVersion::ValV1).ok(),
        "five rounds of the Fibonacci program should be Val: " + played.value_or("<none>"));
    require(st::fieldProgramText("| x | x := 3. 1 bpm: !BPM * x") == std::nullopt
            && st::fieldProgramText("!BPM bpm s") == std::nullopt && st::fieldProgramText("Float phi") == std::nullopt,
        "Val's own programs and expressions should stay Val's");
    require(st::fieldProgramText("| n | n := 0. 3 timesRepeat: [n := n + 2]. !Fader * n") == "(!Fader) * 6",
        "an expression with an Event Value is an Event Value expression");
    require(near(evaluate("Float phi"), (1.0 + std::sqrt(5.0)) / 2.0, talas::expr::Unit::None) && near(evaluate("Float pi"), std::numbers::pi, talas::expr::Unit::None),
        "Float phi should be the golden ratio");
}

// A list field written as a braced program gives
// the numbers of its Array: doubled steps, harmonics' amplitudes and
// frequencies, velocities counted modulo 5, an octatonic scale kept in a
// variable, a 3+3+2 pattern of durations, and pitches drawn from the scale
// by a seeded Random, the same each time. Other fields give nothing.
void testListFieldPrograms()
{
    const auto numbers = [](std::string_view field) {
        const auto written = st::listFieldNumbers(field);
        require(written.has_value(), std::string{field} + " should give numbers");
        return *written;
    };
    require(numbers("{(1 to: 8) collect: [:i | i * 2]}") == "2 4 6 8 10 12 14 16",
        "(1 to: 8) collect: [:i | i * 2] should double each step");
    require(numbers("{(1 to: 4) collect: [:i | 1 / i]}").starts_with("1 0.5 0.333"),
        "the amplitudes should be the harmonic numbers' inverses");
    require(numbers("{(1 to: 3) collect: [:i | 100 hz * i]}") == "100 hz 200 hz 300 hz",
        "the frequencies should be harmonics of 100 hz");
    require(numbers("{(1 to: 15) collect: [:i | (i mod: 5)]}") == "1 2 3 4 0 1 2 3 4 0 1 2 3 4 0",
        "counting from 1 to 15 modulo 5");
    require(numbers("{(1 to: 5) collect: [:i | (i mod: 5) + 1.0 / 5]}") == "0.4 0.6 0.8 1 0.2",
        "velocities with no 0, at most 1");
    require(numbers("{| p | p := 36.\n(1 to: 15) collect: [ :i | p := p + (i mod: 2) + 1]}")
            == "38 39 41 42 44 45 47 48 50 51 53 54 56 57 59",
        "the octatonic scale from 36");
    require(numbers("{(1 to: 15) collect: [:i | (i - 1) mod: 3]}") == "0 1 2 0 1 2 0 1 2 0 1 2 0 1 2"
            && numbers("{(1 to: 15) collect: [:i | ((i - 1) mod: 3) // 2]}") == "0 0 1 0 0 1 0 0 1 0 0 1 0 0 1"
            && numbers("{(1 to: 15) collect: [:i | 3 - (((i - 1) mod: 3) // 2)]}") == "3 3 2 3 3 2 3 3 2 3 3 2 3 3 2",
        "the 3+3+2 durations");
    const auto drawn = std::string{"{| p pchs r | p := 36.\n"
        "\"Save 15 pitches of an octatonic scale starting at p in the variable pchs.\" "
        "pchs := (1 to: 15) collect: [ :i | p := p + (i mod: 2) + 1].\n"
        "\"Create a random number generator\" r := Random newForTalasWithSeed: 7.\n"
        "(1 to: 15) collect: [:i | | index | index := (r next * (pchs size - 1) + 1) rounded. pchs at: index]}"};
    const auto pitches = numbers(drawn);
    std::set<std::string> scale;
    std::istringstream words(pitches);
    for (std::string pitch; words >> pitch;)
        scale.insert(pitch);
    require(pitches == numbers(drawn) && scale.size() > 3
            && std::ranges::all_of(scale, [](const std::string& pitch) {
                   return std::string_view{"38 39 41 42 44 45 47 48 50 51 53 54 56 57 59"}.find(pitch)
                       != std::string_view::npos;
               }),
        "pitches drawn by a seeded Random should come from the scale, the same each time: " + pitches);
    for (const auto* field : {"{30 ms}", "1 2 3", "{1} {2}", "{(1 to: 3) collect: [:i | !Amp suffix2: i]}", "{}"})
        require(!st::listFieldNumbers(field).has_value(), std::string{field} + " should not be read as a program");
}

// Floats' hyperbolic functions and their inverses,
// and the Bessel functions I0, J0, J1, Jn, Y0, Y1 and Yn.
void testHyperbolicAndBesselFunctions()
{
    using talas::expr::Unit;
    const auto value = [](std::string_view source, double expected) {
        const auto answer = evaluate(source);
        return answer.kind == st::Value::Kind::Number && std::abs(answer.number - expected) < 1.0e-9;
    };
    require(value("1 cosh", std::cosh(1.0)) && value("1 sinh", std::sinh(1.0)) && value("2 arcCosh", std::acosh(2.0))
            && value("1 arcSinh", std::asinh(1.0)) && value("1 sinh arcSinh", 1.0),
        "cosh, sinh, arcCosh and arcSinh");
    require(value("0 besselI0", 1.0) && value("1 besselI0", 1.2660658777520082) && value("0 besselJ0", 1.0)
            && value("2.404825557695773 besselJ0", 0.0) && value("1 besselJ1", 0.44005058574493355)
            && value("1 besselJ: 2", 0.11490348493190049) && value("1 besselY0", 0.08825696421567697)
            && value("1 besselY1", -0.7812128213002887) && value("1 besselY: 2", -1.6506826068162546),
        "the Bessel functions should take their values");
    SoundHost host;
    require(!runOn(host, "0 besselY0").ok(), "besselY0 should take a positive receiver");
}

// Self debugWithLabel: 'i' value: i shows "i = " and i's
// value as a script runs, and debug: the value alone, answering it;
// debugWithLabel:value: answers nil.
void testScriptsShowValues()
{
    SoundHost host;
    const auto shown = runOn(host, "0 to: 3 do: [ :i | self debugWithLabel: 'i' value: i]. self debug: 2 + 3");
    require(shown.ok() && near(shown.value, 5.0, talas::expr::Unit::None)
            && shown.debugLines == std::vector<std::string>{"i = 0", "i = 1", "i = 2", "i = 3", "5"},
        "debugWithLabel:value: and debug: should show the values as the script runs");
    require(runOn(host, "self debugWithLabel: 'x' value: 6").value.kind == st::Value::Kind::Nil,
        "the value of debugWithLabel:value: is nil");
    // Evaluate in a text window shows them too.
    st::EvaluationHost evaluation;
    const auto evaluated = st::evaluateSelection(
        "| thePrisoner | thePrisoner := 6. self debugWithLabel: 'thePrisoner' value: thePrisoner. thePrisoner", 0, 0, evaluation);
    require(evaluated.ok() && evaluated.debugLines == std::vector<std::string>{"thePrisoner = 6"},
        "Evaluate should show what debugWithLabel:value: shows");
    // Asked, each value pauses the
    // program until OK goes on or Cancel stops it.
    const auto loop = "0 to: 5 do: [ :i | self debugWithLabel: 'i' value: i]. 7";
    st::EvaluationHost asking;
    asking.answerFrom({}, {});
    const auto paused = st::evaluateSelection(loop, 0, 0, asking);
    require(paused.pendingPrompt.has_value() && paused.pendingPrompt->kind == st::PromptKind::Confirm
            && paused.pendingPrompt->question == "i = 0",
        "the program should pause at its first value");
    st::EvaluationHost throughAll;
    throughAll.answerFrom(std::vector<st::PromptAnswer>(6, st::PromptAnswer{true, {}}), {});
    const auto finished = st::evaluateSelection(loop, 0, 0, throughAll);
    st::EvaluationHost cancelled;
    cancelled.answerFrom({{true, {}}, {true, {}}, {false, {}}}, {});
    const auto stopped = st::evaluateSelection(loop, 0, 0, cancelled);
    require(finished.ok() && finished.debugLines.size() == 6 && !stopped.ok() && !stopped.pendingPrompt.has_value()
            && stopped.debugLines.size() == 3,
        "OK six times should go through the loop, and Cancel should stop it");
}

// Smalltalk evaluates binary messages left to right, after unary ones and
// before keyword ones.
void testPrecedenceAndArithmetic()
{
    using talas::expr::Unit;
    require(near(evaluate("3 + 4 * 2"), 14.0, Unit::None), "binary messages should run left to right");
    require(near(evaluate("3 + (4 * 2)"), 11.0, Unit::None), "parentheses should group");
    require(near(evaluate("16 sqrt + 1"), 5.0, Unit::None), "unary messages should come first");
    require(near(evaluate("7 // 2"), 3.0, Unit::None) && near(evaluate("7 \\\\ 2"), 1.0, Unit::None),
        "integer division and remainder should floor");
    require(near(evaluate("3 max: 4 + 1"), 5.0, Unit::None), "keyword arguments take whole binary expressions");
    require(near(evaluate("-3 abs"), 3.0, Unit::None) && near(evaluate("5 - -3"), 8.0, Unit::None),
        "a minus before a digit with no operand before it starts a negative literal");
    require(near(evaluate("16r1F"), 31.0, Unit::None) && near(evaluate("1.5e2"), 150.0, Unit::None),
        "radix and exponent literals should read");
}

// Units convert as in Val: note numbers through
// hertz, times through seconds.
void testUnits()
{
    using talas::expr::Unit;
    require(near(evaluate("440 hz nn"), 69.0, Unit::NoteNumber), "440 hz should be 69 nn");
    require(near(evaluate("69 nn - 3 nn"), 66.0, Unit::NoteNumber), "note numbers should subtract");
    require(near(evaluate("100 hz + 10 hz * 2"), 220.0, Unit::Hertz),
        "a frequency times a number stays a frequency");
    require(near(evaluate("69 nn + 110 hz"), 550.0, Unit::Hertz), "mixed frequencies should add in hertz");
    require(near(evaluate("0.5 s inverse * 100"), 200.0, Unit::Hertz), "the inverse of a time is a frequency");
    require(near(evaluate("250 ms + 1 s"), 1.25, Unit::Seconds), "mixed times should add in seconds");
    require(near(evaluate("(2 s / 500 ms)"), 4.0, Unit::None), "a time over a time is a number");
    // A script writes one beat as 1 beat.
    require(near(evaluate("1 beat"), 1.0, Unit::Beats) && near(evaluate("1.5 beat"), 1.5, Unit::Beats),
        "beat should name the beats unit");
    SoundHost host;
    require(!runOn(host, "1 hz + 1 s").ok(), "a frequency and a time should not add");
}

// The ParameterTransformer's worked examples.
void testParameterTransformerExamples()
{
    using talas::expr::Unit;
    SoundHost host;
    host.parameters["frequency"] = st::Value::of(440.0, Unit::Hertz);
    require(runOn(host, "snd frequency isNil ifFalse: [snd frequency: snd frequency * 2].").ok()
            && near(host.parameters["frequency"], 880.0, Unit::Hertz),
        "a transformation should double the frequency");
    require(runOn(host, "snd frequency isNil ifFalse: [\nsnd frequency: (snd frequency nn - 3 nn)].").ok()
            && std::abs(host.parameters["frequency"].number - 78.0) < 1.0e-9
            && host.parameters["frequency"].unit == Unit::NoteNumber,
        "a transformation should shift the frequency down three half steps");
    host.parameters["frequency"] = st::Value::of(69.0, Unit::NoteNumber);
    require(runOn(host,
                "snd frequency isNil ifFalse: [\nsnd frequency:\n(snd frequency nn -\n(3 * (time / totalDuration)) nn)].")
                .ok()
            && near(host.parameters["frequency"], 68.25, Unit::NoteNumber),
        "a transformation should depend on time");
    host.parameters["duration"] = st::Value::of(0.5, Unit::Seconds);
    require(runOn(host, "snd duration isNil ifFalse: [\nsnd frequency: snd duration inverse * 100].").ok()
            && near(host.parameters["frequency"], 200.0, Unit::Hertz),
        "a frequency should follow the duration's inverse");
    // One parameter a function of another, the
    // duration exactly one cycle of the frequency.
    host.parameters["frequency"] = st::Value::of(100.0, Unit::Hertz);
    require(runOn(host, "snd duration: (1.0 / snd frequency hz).").ok()
            && near(host.parameters["duration"], 0.01, Unit::Seconds),
        "the duration should be one cycle of the frequency");
    require(runOn(host, "snd sample: 'celtHrp2a'.").ok()
            && host.parameters["sample"].kind == st::Value::Kind::String
            && host.parameters["sample"].text == "celtHrp2a",
        "a transformation should set a sample by name");
    require(runOn(host, "snd missing isNil ifTrue: [snd marked: 1]").ok()
            && near(host.parameters["marked"], 1.0, Unit::None),
        "a missing parameter should read nil");
}

void testBlocksLoopsAndTemporaries()
{
    using talas::expr::Unit;
    require(near(evaluate("| sum | sum := 0. 1 to: 10 do: [:i | sum := sum + i]. sum"), 55.0, Unit::None),
        "to:do: should run its block for each number");
    // CurTime is assigned 0 and curFreq 100.
    require(near(evaluate("| curTime curFreq | curTime := 0. curFreq := 100. curFreq"), 100.0, Unit::None)
            && near(evaluate("| curTime curFreq | curTime := 0. curFreq := 100. curTime"), 0.0, Unit::None),
        ":= should assign the declared variables their values");
    require(near(evaluate("| n | n := 0. [n < 5] whileTrue: [n := n + 1]. n"), 5.0, Unit::None),
        "whileTrue: should loop while its receiver is true");
    require(near(evaluate("| twice | twice := [:x | x * 2]. twice value: 21"), 42.0, Unit::None),
        "blocks should take arguments");
    require(near(evaluate("| x | x := 3. (x > 2) ifTrue: [10] ifFalse: [20]"), 10.0, Unit::None),
        "ifTrue:ifFalse: should answer its chosen block's value");
    require(near(evaluate("^ 7. 8"), 7.0, Unit::None), "a return should end the script");
    require(near(evaluate("\"a comment\" 2 + 2"), 4.0, Unit::None), "comments should be skipped");
    SoundHost host;
    require(!runOn(host, "[true] whileTrue").ok(), "a loop that never ends should run out of steps");
    require(!runOn(host, "undefined + 1").ok(), "an unknown name should fail");
    require(!st::parse("3 + (4").ok() && !st::parse("'open").ok(), "malformed scripts should not parse");
    // A variable assigned a value never read is warned of;
    // one read, or declared alone, is not.
    const auto unread = [](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), std::string{source} + " should parse");
        return st::unreadTemporaries(*parsed.program);
    };
    require(unread("| a b c | a := 1. b := a. [:x | | d | d := x] value: 2")
                == std::vector<std::string>{"b", "d"}
            && unread("| a | a := 1. a + 1").empty() && unread("| a | 3").empty(),
        "the variables assigned and never read");
}

// An event may take the pitch 4 a, an octave number and a pitch name (4 c
// sharp, 4 d flat), and a tempo may be written in bpm.
void testPitchNamesAndTempo()
{
    using talas::expr::Unit;
    require(near(evaluate("4 a"), 69.0, Unit::NoteNumber) && near(evaluate("3 c"), 48.0, Unit::NoteNumber)
            && near(evaluate("4 la"), 69.0, Unit::NoteNumber),
        "4 a should be the note number 69");
    require(near(evaluate("4 c sharp"), 61.0, Unit::NoteNumber) && near(evaluate("4 d flat"), 61.0, Unit::NoteNumber),
        "sharp and flat should raise and lower a pitch a half step");
    require(near(evaluate("4 a hz"), 440.0, Unit::Hertz) && near(evaluate("4 a + 1 nn"), 70.0, Unit::NoteNumber),
        "a pitch should convert and add as a note number");
    require(st::fieldText(evaluate("4 a")) == std::optional<std::string>{"69 nn"},
        "a pitch should write back as a note number");
    require(near(evaluate("120 bpm hz"), 2.0, Unit::Hertz) && near(evaluate("2 hz bpm"), 120.0, Unit::BeatsPerMinute)
            && st::fieldText(evaluate("90 bpm")) == std::optional<std::string>{"90 bpm"},
        "a tempo in bpm should be a frequency");
    SoundHost host;
    require(!runOn(host, "440 hz a").ok() && !runOn(host, "4 sharp").ok(),
        "a pitch name should follow an octave number, and sharp a pitch");
}

// A Script gives an event delay: (!Delay * (i inverse)),
// an expression of an Event Value the Sound evaluates as it plays; a
// Script may loop 1 to: ?total do:, a variable the script reads as Talas
// asks for it when the Script loads, and which a field holds as @total.
// Collect: over Event Values builds an Array of
// Val expressions, one for each step, as the test window shows.
void testCollectBuildsArraysOfExpressions()
{
    const auto expressions = [](const st::Value& value, std::size_t count, std::string_view holding) {
        return value.kind == st::Value::Kind::Array && value.elements->size() == count
            && std::ranges::all_of(*value.elements, [&](const st::Value& element) {
                   return element.kind == st::Value::Kind::Expression && element.text.find(holding) != std::string::npos;
               });
    };
    const auto latches = evaluate("(0 to: 4) collect: [:i | ((!KeyDown countTriggersMod: 5) eq: i) sampleAndHold: !KeyNumber]");
    require(expressions(latches, 5, "sampleAndHold:") && latches.elements->at(0).text.find("eq: (0)") != std::string::npos
            && latches.elements->at(4).text.find("eq: (4)") != std::string::npos,
        "the collect: should give five latch expressions, the k-th for key k");
    const auto smoothed = evaluate("| timeInterval | timeInterval := !BPM bpm s.\n"
                                   "(1 to: 32) collect: [:i | i inverse * (timeInterval random abs smooth: timeInterval)]");
    require(expressions(smoothed, 32, "smooth:"), "the code should construct 32 random, smoothed expressions");
    const auto frequencies = evaluate("(0 to: 7) collect: [:i | !FreqLow nn + (i * 12 * !OctSpacing) nn]");
    require(expressions(frequencies, 8, "OctSpacing"),
        "the code should construct eight frequencies from !FreqLow, spaced by !OctSpacing");
}

void testEventValuesAndTalasVariables()
{
    using talas::expr::Unit;
    const auto delay = evaluate("| i | i := 2. !Delay * (i inverse)");
    require(delay.kind == st::Value::Kind::Expression && st::fieldText(delay) == std::optional<std::string>{"(!Delay) * 0.5"},
        "an Event Value times a number should be an expression");
    const auto smoothed = evaluate("(!Fader1 * 100) hz");
    require(smoothed.kind == st::Value::Kind::Expression && smoothed.text == "((!Fader1) * 100) hz",
        "a unit message to an Event Value expression should be Val's");
    const auto keyed = evaluate("!KeyDown nextRandom");
    require(keyed.text == "(!KeyDown) nextRandom", "a message to an Event Value should build its expression");
    const auto compared = evaluate("0.5 < !Fader1");
    require(compared.kind == st::Value::Kind::Expression && compared.text == "0.5 < (!Fader1)",
        "a comparison with an Event Value should be an expression");
    // A function of time, as ramp:, makes the expression
    // Val; numbers alone are Smalltalk.
    require(evaluate("!KeyDown ramp: 1 s").kind == st::Value::Kind::Expression && evaluate("3 + 4").kind == st::Value::Kind::Number,
        "an expression with a red variable or ramp: should be Val, one without Smalltalk");
    SoundHost host;
    require(!runOn(host, "1 to: !Count do: [:i | i]").ok(),
        "a loop should not run to an Event Value, which has no value while the script runs");

    talas::project::ProjectVariableLibrary library;
    library.variables = {{"total", "total", talas::expr::ValueDomain::Scalar, 3.0},
        {"freq", "freq", talas::expr::ValueDomain::Hertz, 100.0}};
    const auto total = st::declaredVariable(library, "?total");
    require(total.has_value() && total->kind == st::Value::Kind::Expression && total->known
            && st::fieldText(*total) == std::optional<std::string>{"@total"},
        "?total should be the expression @total with its declared value");
    require(!st::declaredVariable(library, "?missing").has_value(), "an undeclared variable has no value");

    class VariableHost final : public st::Host
    {
    public:
        explicit VariableHost(const talas::project::ProjectVariableLibrary& library)
            : library_(library)
        {
        }
        std::optional<st::Value> variable(std::string_view name) override
        {
            return name.starts_with('?') ? st::declaredVariable(library_, name) : std::nullopt;
        }
        std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
        {
            error = "no objects";
            return std::nullopt;
        }

    private:
        const talas::project::ProjectVariableLibrary& library_;
    };
    VariableHost variables(library);
    const auto run = [&](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), "should parse: " + std::string{source});
        const auto ran = st::run(*parsed.program, variables);
        require(ran.ok(), "should run: " + std::string{source}
                + (ran.diagnostic ? ": " + ran.diagnostic->message : std::string{}));
        return ran.value;
    };
    require(near(run("| n | n := 0. 1 to: ?total do: [:i | n := n + i]. n"), 6.0, Unit::None),
        "1 to: ?total do: should run ?total times");
    const auto doubled = run("?freq * 2");
    require(doubled.kind == st::Value::Kind::Expression && doubled.text == "(@freq) * 2" && doubled.known
            && near(st::numberOf(doubled), 200.0, Unit::Hertz),
        "arithmetic on a ?variable should keep its expression and know its value");
    require(run("?total > 2").kind == st::Value::Kind::Boolean && run("?total > 2").boolean,
        "a ?variable should compare by its declared value");
    // VarIfTrue:ifFalse: chooses by a comparison of ?variables, as
    // (?dur > 1 s) varIfTrue: [1 s] ifFalse: [0.1 * ?dur].
    library.variables.push_back({"dur", "dur", talas::expr::ValueDomain::Seconds, 2.0});
    library.variables.push_back({"short", "short", talas::expr::ValueDomain::Seconds, 0.5});
    require(near(run("(?dur > 1 s) varIfTrue: [1 s] ifFalse: [0.1 * ?dur]"), 1.0, Unit::Seconds)
            && near(st::numberOf(run("(?short > 1 s) varIfTrue: [1 s] ifFalse: [0.1 * ?short]")), 0.05, Unit::Seconds)
            && near(run("(?short > 1 s) varIfFalse: [2 s] ifTrue: [1 s]"), 2.0, Unit::Seconds)
            && near(run("(?dur > 1 s) varIfTrue: [3 s]"), 3.0, Unit::Seconds)
            && run("(?dur > 1 s) varIfFalse: [3 s]").kind == st::Value::Kind::Nil,
        "varIfTrue:ifFalse: and its kin should choose by the variables' declared values");
    require(run("(?dur > 1 s) varAnd: [?short < 1 s]").boolean && !run("(?short > 1 s) varAnd: [?dur > 1 s]").boolean
            && run("(?short > 1 s) varOr: [?dur > 1 s]").boolean && !run("(?short > 1 s) varOr: [?dur < 1 s]").boolean,
        "varAnd: and varOr: should join comparisons of ?variables");
    const auto unknown = run("?other + 1");
    require(unknown.kind == st::Value::Kind::Expression && unknown.text == "(@other) + 1" && !unknown.known,
        "a ?variable the library does not declare should reach a field as @other");
}

// AsSMPTE writes a time as time code at the frames a
// second of the project, 25.5 s as 00:00:25.15 at 30, the complement of
// 00:00:25.15 SMPTE.
void testTimesAsTimeCode()
{
    const auto smpte = [](std::string_view source, unsigned framesPerSecond) {
        st::EvaluationHost host(48000.0, framesPerSecond);
        const auto parsed = st::parse(source);
        require(parsed.ok(), "should parse: " + std::string{source});
        const auto ran = st::run(*parsed.program, host);
        return ran.ok() && ran.value.kind == st::Value::Kind::String ? ran.value.text : std::string{"?"};
    };
    require(smpte("25.5 s asSMPTE", 30) == "00:00:25.15" && smpte("3661 s asSMPTE", 30) == "01:01:01.00"
            && smpte("1500 ms asSMPTE", 30) == "00:00:01.15",
        "25.5 s asSMPTE should be 00:00:25.15 at 30 frames a second");
    require(smpte("25.5 s asSMPTE", 24) == "00:00:25.12" && smpte("25.5 s asSMPTE", 25) == "00:00:25.12",
        "the frames a second chosen should count the frames");
    require(smpte("25.5 asSMPTE", 30) == "?" && smpte("-1 s asSMPTE", 30) == "?",
        "a number that is no time, or a time before zero, is not time code");
}

// Random newForTalasWithSeed: makes "an endless stream of
// random numbers" between zero and one, one for each next, repeatable for
// a seed (66508, say).
void testRandomNumbers()
{
    using talas::expr::Unit;
    const auto first = evaluate("| r | r := Random newForTalasWithSeed: 1002. r next");
    require(first.kind == st::Value::Kind::Number && first.number >= 0.0 && first.number < 1.0,
        "next should answer a number from 0 to 1");
    require(near(evaluate("| r | r := Random newForTalasWithSeed: 1002. r next"), first.number, Unit::None),
        "a seed should make the stream repeatable");
    require(evaluate("| r | r := Random newForTalasWithSeed: 1003. r next").number != first.number,
        "another seed should give another stream");
    require(evaluate("| r a | r := Random newForTalasWithSeed: 5. a := r next. a = r next").boolean == false
            && evaluate("| r a | r := Random newForTalasWithSeed: 5. a := r next. r seed: 5. a = r next").boolean,
        "next should move along the stream, and seed: start it again");
    const auto spread = evaluate("| r low high x | r := Random newForTalasWithSeed: 2530. low := 1. high := 0. "
                                 "1 to: 1000 do: [:i | x := r next. x < low ifTrue: [low := x]. "
                                 "x > high ifTrue: [high := x]]. high - low");
    require(spread.number > 0.99, "a thousand numbers should spread across 0 to 1");
    SoundHost host;
    require(runOn(host, "Random newForTalasWithSeed: 66508").ok() && !runOn(host, "Random newForTalasWithSeed: -1").ok()
            && !runOn(host, "Random newForTalasWithSeed: 1.5").ok(),
        "a seed should be a whole number that is not negative, as 66508 is");
    // Debug: is not part of the random number generator.
    const auto quiet = runOn(host, "| r | r := Random newForTalasWithSeed: 217. 4 timesRepeat: [r next]. r next");
    const auto debugged = runOn(host, "| r | r := Random newForTalasWithSeed: 217. 4 timesRepeat: [self debug: r next]. r next");
    require(quiet.ok() && debugged.ok() && debugged.value.number == quiet.value.number && debugged.debugLines.size() == 4,
        "showing the numbers with debug: should leave the random sequence as it is");
}

// OneOverF newForTalasWithSeed: 52 states: 128
// "generates random numbers between zero and one" that stay close to one
// another for a while, then jump: its steps are smaller than a Random's.
void testOneOverFNumbers()
{
    using talas::expr::Unit;
    const auto first = evaluate("| r | r := OneOverF newForTalasWithSeed: 52 states: 128. r next");
    require(first.kind == st::Value::Kind::Number && first.number >= 0.0 && first.number <= 1.0,
        "a OneOverF's next should answer a number from 0 to 1");
    require(near(evaluate("| r | r := OneOverF newForTalasWithSeed: 52 states: 128. r next"), first.number, Unit::None)
            && evaluate("| r | r := OneOverF newForTalasWithSeed: 53 states: 128. r next").number != first.number,
        "a seed should make a OneOverF's stream repeatable, another seed another stream");
    const auto steps = [](const std::string& generator) {
        return evaluate("| r last sum x | r := " + generator + ". last := r next. sum := 0. "
                        "1 to: 2000 do: [:i | x := r next. sum := sum + (x - last) abs. last := x]. sum / 2000")
            .number;
    };
    const auto oneOverF = steps("OneOverF newForTalasWithSeed: 17661");
    const auto uniform = steps("Random newForTalasWithSeed: 17661");
    require(oneOverF > 0.0 && oneOverF < uniform / 3.0,
        "a OneOverF should mostly move in small steps, a Random's numbers jump "
            + std::to_string(oneOverF) + " " + std::to_string(uniform));
    const auto spread = evaluate("| r low high x | r := OneOverF newForTalasWithSeed: 7. low := 1. high := 0. "
                                 "1 to: 5000 do: [:i | x := r next. x < low ifTrue: [low := x]. "
                                 "x > high ifTrue: [high := x]]. high - low");
    require(spread.number > 0.5, "a OneOverF should wander across much of 0 to 1");
    SoundHost host;
    require(!runOn(host, "OneOverF newForTalasWithSeed: 5 states: 1").ok()
            && !runOn(host, "OneOverF newForTalasWithSeed: -1").ok(),
        "a OneOverF should take a seed as a Random does and at least two states");
}

// The words of an Array of symbols, in order.
std::vector<std::string> wordsOf(const st::Value& value)
{
    require(value.kind == st::Value::Kind::Array, "a MarkovChain's sequence should be an Array");
    std::vector<std::string> words;
    for (const auto& element : *value.elements)
        words.push_back(element.text);
    return words;
}

// Whether the runs of the given length in made all occur in training.
bool runsOccur(const std::vector<std::string>& made, const std::vector<std::string>& training, std::size_t length)
{
    for (std::size_t at = 0; at + length <= made.size(); ++at)
    {
        const auto found = std::ranges::search(training, std::span{made}.subspan(at, length));
        if (found.empty())
            return false;
    }
    return true;
}

// A MarkovChain trained on a sequence of words with
// chain length 2 makes new sequences whose every pair of words occurs in
// the training sequence, a new sequence for each seed; one that backs
// itself into a corner returns what it has so far. The transitions are
// weighted by how often they occur.
void testMarkovChains()
{
    const std::string training =
        "#( what is your name is it an animal name or is your name one that would belong to a human like my name "
        "is a name that would belong to a very human being )";
    const auto trainingWords = wordsOf(evaluate(training));
    const auto sequence = [&](int seed) {
        return wordsOf(evaluate("| chain | chain := MarkovChain trainingSequence: " + training
                                + " chainLength: 2. chain newSequenceOfLength: 100 seed: " + std::to_string(seed)));
    };
    const auto made = sequence(9008);
    require(!made.empty() && made.size() <= 100 && runsOccur(made, trainingWords, 2),
        "every pair of words in the generated sequence should occur in the training sequence");
    std::set<std::vector<std::string>> distinct;
    auto anyNew = false;
    auto cornered = false;
    for (int seed = 1; seed <= 20; ++seed)
    {
        const auto words = sequence(seed);
        require(runsOccur(words, trainingWords, 2), "every seed's pairs should occur in the training");
        require(words.size() == 100 || words.back() == "being",
            "a sequence shorter than asked for should end where no transition is left");
        anyNew = anyNew || std::ranges::search(trainingWords, words).empty();
        cornered = cornered || words.size() < 100;
        distinct.insert(words);
    }
    require(sequence(9008) == made, "a seed should make the sequence repeatable");
    require(distinct.size() == 20, "each seed should give a new sequence");
    require(anyNew, "a sequence should be new, not found anywhere in the training sequence");
    require(cornered, "a chain should back itself into a corner at being");
    // Not tied to musical applications; Arrays as elements, and
    // longer chains whose runs of three all occur.
    const std::string pairs = "#( #(0 4) #(3 2) #(-5 2) #(2 4) #(3 2) #(-5 2) #(2 4) #(3 2) #(-5 4) #(2 2) #(3 2) "
                              "#(-5 4) #(2 2) #(-5 2) #(5 2) #(-7 4) #(1 2) #(-2 2) #(1 4) #(0 0.5) #(12 4) )";
    const auto offsets = evaluate("| chain | chain := MarkovChain trainingSequence: " + pairs
                                  + " chainLength: 2. chain newSequenceOfLength: 100 seed: 26");
    require(offsets.kind == st::Value::Kind::Array && !offsets.elements->empty()
            && std::ranges::all_of(*offsets.elements, [](const st::Value& element) {
                   return element.kind == st::Value::Kind::Array && element.elements->size() == 2;
               }),
        "a chain trained on an Array of Arrays should make a sequence of them");
    const auto triples = wordsOf(evaluate("| chain | chain := MarkovChain trainingSequence: " + training
                                          + " chainLength: 3. chain newSequenceOfLength: 100 seed: 3"));
    require(runsOccur(triples, trainingWords, 3), "every three words in a row of a chain of length 3 should occur");
    // Weighted by how many times each transition occurs.
    const auto counts = wordsOf(evaluate(
        "| chain | chain := MarkovChain trainingSequence: #(a a a b) chainLength: 1. "
        "chain newSequenceOfLength: 10000 seed: 7"));
    const auto as = std::ranges::count(counts, std::string{"a"});
    require(counts.size() == 10000 && std::abs(static_cast<double>(as) / 10000.0 - 0.75) < 0.02,
        "transitions should be weighted by how often they occur");
    SoundHost host;
    require(!runOn(host, "MarkovChain trainingSequence: #() chainLength: 2").ok()
            && !runOn(host, "MarkovChain trainingSequence: #(a b) chainLength: 3").ok()
            && !runOn(host, "MarkovChain trainingSequence: #(a b) chainLength: 0").ok()
            && !runOn(host, "(MarkovChain trainingSequence: #(a b) chainLength: 2) newSequenceOfLength: 5 seed: -1").ok(),
        "a chain should need a training sequence at least its chain length long, and a seed as a Random takes it");
}

// A host's send may draw from the script's Random and evaluate the blocks
// it is given, as a MIDI Script's randomizeUsing: and a MIDIFileInterpreter's
// ifNone: do.
void testHostSendsBack()
{
    struct Drawing final : st::Host
    {
        std::optional<st::Value> variable(std::string_view name) override
        {
            return name == "thing" ? std::optional{st::Value::object(7)} : std::nullopt;
        }
        std::optional<st::Value> send(const st::Value&, std::string_view selector, std::span<const st::Value> arguments,
            std::string& error) override
        {
            if (selector == "twiceOf:")
            {
                const auto first = sendBack(arguments[0], "next", {}, error);
                const auto second = first ? sendBack(arguments[0], "next", {}, error) : std::nullopt;
                if (!second)
                    return std::nullopt;
                return st::Value::of(first->number + second->number);
            }
            if (selector == "valueOf:")
                return sendBack(arguments[0], "value", {}, error);
            return std::nullopt;
        }
    } host;
    const auto on = [&](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), "should parse: " + std::string{source});
        return st::run(*parsed.program, host);
    };
    const auto drawn = on("thing twiceOf: (Random newForTalasWithSeed: 9)");
    const auto direct = evaluate("| r | r := Random newForTalasWithSeed: 9. r next + r next");
    require(drawn.ok() && near(drawn.value, direct.number, talas::expr::Unit::None),
        "a host should draw a script's Random's next numbers");
    const auto valued = on("| x | x := 4. thing valueOf: [x * 2]");
    require(valued.ok() && near(valued.value, 8.0, talas::expr::Unit::None), "a host should evaluate a block it is given");
    const auto failed = on("thing twiceOf: 3");
    require(!failed.ok(), "a host's message back that fails should fail the script");
}

// #(0 1 11 5), #( {Note ...} {...} ) with elements
// computed in braces, Array with: a with: b, and collect: over an Array or
// an Interval, as the MIDI scripts there use them.
void testArrays()
{
    using talas::expr::Unit;
    const auto numbers = evaluate("#(0 1 11 5) collect: [:int | int * 2]");
    require(numbers.kind == st::Value::Kind::Array && numbers.elements->size() == 4
            && near((*numbers.elements)[2], 22.0, Unit::None),
        "a literal array should collect into a new Array");
    require(near(evaluate("| t | t := 0. (#(0 1 11 5) collect: [:int | t := t + int]) last"), 17.0, Unit::None),
        "collect: should run its block on each element in order");
    const auto braced = evaluate("#( {2 + 3} {4 a} foo 'bar' (1 2) )");
    require(braced.elements->size() == 5 && near((*braced.elements)[0], 5.0, Unit::None)
            && near((*braced.elements)[1], 69.0, Unit::NoteNumber)
            && (*braced.elements)[2].kind == st::Value::Kind::Symbol && (*braced.elements)[2].text == "foo"
            && (*braced.elements)[3].text == "bar" && (*braced.elements)[4].elements->size() == 2,
        "a literal array should hold {computed} elements, symbols, strings and arrays");
    // Without braces each number, word and
    // operator of an expression is an element of its own.
    const auto unbraced = evaluate("#(2 squared + 1)");
    require(unbraced.elements->size() == 4 && (*unbraced.elements)[1].text == "squared"
            && (*unbraced.elements)[2].text == "+" && near(evaluate("#({2 squared + 1}) size"), 1.0, Unit::None)
            && near(evaluate("#({2 squared + 1}) first"), 5.0, Unit::None),
        "braces should make one element of an expression, evaluated first");
    require(near(evaluate("(Array with: 1 with: 2 with: 3) inject: 0 into: [:sum :each | sum + each]"), 6.0, Unit::None)
            && near(evaluate("{1. 2. 3} size"), 3.0, Unit::None)
            && near(evaluate("((1 to: 9) collect: [:i | (i - 1) / 8.0 * 2 - 1]) last"), 1.0, Unit::None)
            && near(evaluate("(1 to: 10 by: 3) size"), 4.0, Unit::None),
        "Array with:, braces, inject:into: and Intervals should make and fold arrays");
    require(near(evaluate("(#(3 1 2) at: 2) + #(3 1 2) max"), 4.0, Unit::None)
            && evaluate("#(1 2) = (Array with: 1 with: 2)").boolean
            && near(evaluate("((#(1 2 3) select: [:x | x odd]) , #(9)) size"), 3.0, Unit::None),
        "at:, max, equality, select: and , should work on arrays");
    require(near(evaluate("4/3 ** 2"), 16.0 / 9.0, Unit::None),
        "** should raise a number to a power");
    SoundHost host;
    require(!runOn(host, "#(1 2) at: 3").ok() && !st::parse("#(1 2").ok(),
        "an index outside an Array, or an unclosed literal array, should fail");
}

void testFieldText()
{
    using talas::expr::Unit;
    require(st::fieldText(st::Value::of(400.0, Unit::Hertz)) == std::optional<std::string>{"400 hz"}
            && st::fieldText(st::Value::of(0.1, Unit::Seconds)) == std::optional<std::string>{"0.1 s"}
            && st::fieldText(st::Value::of(66.0, Unit::NoteNumber)) == std::optional<std::string>{"66 nn"}
            && st::fieldText(st::Value::of(2.5)) == std::optional<std::string>{"2.5"}
            && !st::fieldText(st::Value::nil()).has_value(),
        "values should write back as field text with their units");
    const auto hot = evaluate("| x | x := 2. x * 3 hz");
    require(near(hot, 6.0, Unit::Hertz), "a number times a frequency is a frequency");
}
}

// Evaluate
// (Ctrl+Y) runs the selection as Smalltalk and puts what it prints just
// after it, selected: 5 c gives 72 nn and 5 c hz its frequency, 440 hz s
// one cycle's seconds and 440 hz samp its samples at 44.1 kHz, -6 dB its
// linear amplitude, 3.77952 s bpm the tempo of a beat that long; and
// 256 samp inverse, selected within its braces, is 172.265625 hz.
void testEvaluateAnswersWhatTheSelectionPrints()
{
    st::EvaluationHost host(44100.0);
    const auto printed = [&](std::string_view source) {
        const auto evaluation = st::evaluate(source, host);
        require(evaluation.ok(), "should evaluate: " + std::string{source}
                + (evaluation.diagnostic ? ": " + evaluation.diagnostic->message : std::string{}));
        return evaluation.printed;
    };
    const auto starts = [&](std::string_view source, std::string_view prefix, std::string_view unit) {
        const auto text = printed(source);
        return text.starts_with(prefix) && text.ends_with(unit);
    };
    require(printed("3 + 4") == "7", "3 + 4 should evaluate to 7");
    require(printed("5 c") == "72 nn", "5 c should evaluate to 72 nn");
    require(starts("5 c hz", "523.251", " hz"), "5 c hz should evaluate to its frequency");
    require(printed("5 c hz nn") == "72 nn", "5 c hz nn should be note number 72 again");
    require(starts("440 hz s", "0.002272", " s"), "440 hz s should be one cycle's seconds");
    require(starts("440 hz samp", "100.227", " samp"), "440 hz samp should be one cycle's samples");
    require(printed("-6 dB").starts_with("0.501187"), "-6 dB should evaluate to its linear amplitude");
    require(printed("-12 dB").starts_with("0.251188"), "-12 dB should halve -6 dB's amplitude");
    require(starts("3.77952 s bpm", "15.875", " bpm"), "3.77952 s bpm should be one beat a loop");
    require(starts("3.77952 s bpm * 8", "127.000", " bpm"), "eight beats a loop should be 127 bpm");
    require(printed("3 + 4.\n5 + 5.") == "10", "a program's value should be its last line's");
    require(printed("51.75 * 0.07").starts_with("3.6225"), "a text window should do arithmetic");
    require(starts("16 samp inverse", "2756.25", " hz"), "16 samp inverse should be its frequency");
    const auto quoted = st::evaluateSelection("\"(9.0/5.0) * 100.0 + 32.0\"", 1, 25, host);
    require(quoted.ok() && quoted.text == "\"(9.0/5.0) * 100.0 + 32.0 212\"",
        "a formula selected within its quotes should evaluate");

    const auto field = st::evaluateSelection("{256 samp inverse}", 1, 17, host);
    require(field.ok() && field.text == "{256 samp inverse 172.265625 hz}"
            && field.text.substr(field.resultStart, field.resultEnd - field.resultStart) == "172.265625 hz",
        "the result should follow the selection in the field, selected");
    const auto line = st::evaluateSelection("3 + 4\n5 c hz", 2, 2, host);
    require(line.ok() && line.text == "3 + 4 7\n5 c hz", "with nothing selected the caret's line should be evaluated");
    const auto failed = st::evaluateSelection("x 3 + ", 2, 6, host);
    require(!failed.ok() && failed.text == "x 3 + " && failed.diagnostic->range.offset >= 2,
        "a selection in error should leave the text and say what is wrong");
}

// XYGraphView openOnPoints: opens a graph of the
// points joined by lines; openOnValues: of values at x 1, 2, 3 and on.
void testGraphsOfPointsAndValues()
{
    const auto ran = [](std::string_view source) {
        const auto parsed = st::parse(source);
        require(parsed.ok(), std::string{source} + " should parse");
        struct NoHost final : st::Host
        {
            std::optional<st::Value> variable(std::string_view) override { return std::nullopt; }
            std::optional<st::Value> send(const st::Value&, std::string_view, std::span<const st::Value>, std::string& error) override
            {
                error = "none";
                return std::nullopt;
            }
        } host;
        return st::run(*parsed.program, host);
    };
    const auto values = ran("XYGraphView openOnValues: #(3 1 2)");
    require(values.ok() && values.value.kind == st::Value::Kind::Nil && values.graphs.size() == 1
            && values.graphs.front() == st::XYGraph{{1.0, 3.0}, {2.0, 1.0}, {3.0, 2.0}},
        "openOnValues: should graph each value as the y of points at x 1, 2 and 3");
    const auto points = ran("XYGraphView openOnPoints: (Array with: 0 @ 1 with: 2 @ -1). XYGraphView openOnValues: #(5)");
    require(points.ok() && points.graphs.size() == 2 && points.graphs.front() == st::XYGraph{{0.0, 1.0}, {2.0, -1.0}},
        "openOnPoints: should graph the points in order, each call a graph of its own");
    require(!ran("XYGraphView openOnPoints: #(1 2)").ok() && !ran("XYGraphView openOnValues: #('a')").ok(),
        "a graph of points refuses numbers, one of values refuses text");
}

// The questions a script asks the user, answered in
// order; one not yet answered stops the evaluation, to be asked and the
// evaluation run again, drawing the same random numbers.
void testQuestionsForTheUser()
{
    st::EvaluationHost host;
    const auto answered = [&host](std::string_view source, std::vector<st::PromptAnswer> answers) {
        host.answerFrom(std::move(answers), {});
        return st::evaluate(source, host);
    };
    require(answered("(self confirm: 'Go on?') ifTrue: [1] ifFalse: [2]", {{true, {}}}).printed == "1"
            && answered("(self confirm: 'Go on?') ifTrue: [1] ifFalse: [2]", {{false, {}}}).printed == "2"
            && answered("self confirmYesNo: 'Again?'", {{true, {}}}).printed == "true",
        "confirm: and confirmYesNo: should answer true for OK or Yes, false for Cancel or No");
    require(answered("self confirm: 'Sure?' ifCancel: ['cancelled']", {{false, {}}}).printed == "'cancelled'"
            && answered("self confirm: 'Sure?' ifCancel: ['cancelled']", {{true, {}}}).printed == "true",
        "confirm:ifCancel: should evaluate its block on Cancel");
    const auto typed = answered("(self getValueFromUser: 'How many?') * 2", {{true, "3 + 4"}});
    require(typed.ok() && typed.printed == "14", "getValueFromUser: should answer the value entered: " + typed.printed);
    const auto request = [&](bool evaluating, st::PromptAnswer answer) {
        return answered(std::string{"self request: 'Sum?' initialAnswer: '1 + 1' evaluating: "} + (evaluating ? "true" : "false")
                + " ifCancel: ['none']",
            {std::move(answer)});
    };
    const auto both = request(true, {true, "3 + 4"});
    require(both.ok() && both.printed == "('3 + 4' 7 )" && request(false, {true, "3 + 4"}).printed == "'3 + 4'"
            && request(true, {false, {}}).printed == "'none'",
        "request: should answer the string and its value, or the string only, its cancel block on Cancel: "
            + both.printed);
    host.answerFrom({}, {});
    const auto waiting = st::evaluate("self request: 'Sum?' initialAnswer: '1 + 1' evaluating: true ifCancel: [nil]", host);
    require(!waiting.ok() && waiting.pendingPrompt == st::Prompt{st::PromptKind::Request, "Sum?", "1 + 1"},
        "a question not yet answered should stop the evaluation, the question pending");
    // A file from the user, by a file dialog of the types
    // given; the cancel block's value on Cancel.
    const auto chosen = answered("(Filename getFileWithPrompt: 'Which sample?' fileTypes: #(#AIFF #WAVE) ifCancel: [nil]) asString",
        {{true, "/Samples/kick.aif"}});
    require(chosen.ok() && chosen.printed == "'/Samples/kick.aif'"
            && answered("Filename getFileWithPrompt: 'Which?' fileTypes: #AIFF ifCancel: ['none']", {{false, {}}}).printed == "'none'",
        "getFileWithPrompt: should answer the Filename chosen, or the cancel block's value: " + chosen.printed);
    host.answerFrom({}, {});
    const auto saving = st::evaluate("Filename putFileWithPrompt: 'Save as?' initially: 'take.aif' fileTypes: #AIFF ifCancel: [nil]", host);
    require(!saving.ok()
            && saving.pendingPrompt
                == st::Prompt{st::PromptKind::SaveFile, "Save as?", "take.aif", {"aif", "aiff", "aifc"}},
        "putFileWithPrompt: should ask with its prompt, its initial name and the files of its types");
    st::EvaluationHost alone;
    require(st::evaluate("self confirm: 'Go on?'", alone).printed == "false",
        "a host that asks no one answers Cancel");
    host.answerFrom({}, {});
    const auto random = "| r | r := Random new next. (self confirm: 'Keep?') ifTrue: [r] ifFalse: [0]";
    require(!st::evaluate(random, host).ok() && host.seeds().size() == 1, "the question should stop the run after one Random");
    const auto seeds = host.seeds();
    host.answerFrom({{true, {}}}, seeds);
    const auto first = st::evaluate(random, host).printed;
    host.answerFrom({{true, {}}}, seeds);
    require(first != "0" && st::evaluate(random, host).printed == first,
        "run again with the answer, the evaluation should draw the same random numbers");
}

// Two strings of the same
// characters are equal but not necessarily the same object; a variable
// holding one is the same object, and a Symbol is unique.
void testStringsOfTheSameCharactersNeedNotBeTheSameObject()
{
    const auto yes = [](std::string_view source) { return evaluate(source).boolean; };
    require(yes("'first half' = 'first half'") && !yes("'first half' == 'first half'") && yes("'first half' ~~ 'first half'")
            && yes("| s | s := 'first half'. s == s") && yes("#half == #half") && !yes("#(1 2) == #(1 2)")
            && yes("#(1 2) = #(1 2)") && yes("3 == 3"),
        "two strings of the same characters should be equal yet not necessarily the same object");
}

// Allow stack traces
// makes an error more verbose, telling each message it happened within,
// innermost first; without it the error alone.
void testStackTracesTellWhereAnErrorHappened()
{
    constexpr std::string_view program = "#(1 2) do: [:each | each frobnicate]";
    talas::project::smalltalk::EvaluationHost plain;
    const auto quiet = talas::project::smalltalk::evaluate(program, plain);
    talas::project::smalltalk::EvaluationHost tracing;
    tracing.setStackTraces(true);
    const auto traced = talas::project::smalltalk::evaluate(program, tracing);
    require(!quiet.ok() && !traced.ok() && quiet.diagnostic->message.find("Stack trace") == std::string::npos,
        "without stack traces an error should tell its message alone");
    const auto& verbose = traced.diagnostic->message;
    const auto inner = verbose.find("\n  frobnicate (bytes ");
    const auto outer = verbose.find("\n  do: (bytes ");
    require(verbose.starts_with(quiet.diagnostic->message + "\nStack trace:") && inner != std::string::npos
            && outer != std::string::npos && inner < outer && traced.diagnostic->range == quiet.diagnostic->range,
        "with stack traces the error should tell the messages it happened within, innermost first: " + verbose);
}

// Any message Val takes in real time has a
// compile-time equivalent in Smalltalk: each of Val's messages to a
// number answers the same in a Smalltalk program, and
// a function of time or of an Event Value passes through as an expression.
void testValMessagesHaveSmalltalkEquivalents()
{
    std::vector<std::string> sources;
    for (const auto* message : {"abs", "exp", "twoExp", "squared", "inverse", "sign", "truncated", "rounded", "clipTo01",
             "wrapTo02", "cos", "sin", "normCos", "normSin", "asLogicValue", "removeUnits", "reciprocal", "asInteger", "floor",
             "ceiling", "arcTan", "arcSin", "arcCos", "tan", "sinc", "negated"})
    {
        sources.push_back("-0.7 " + std::string{message});
        sources.push_back("0.3 " + std::string{message});
    }
    for (const auto* message : {"sqrt", "log", "twoLog", "ln"})
        sources.push_back("0.3 " + std::string{message});
    for (const auto* expression : {"0.3 + 0.2", "0.3 - 0.5", "0.3 * 4", "0.3 / 4", "7 // 2", "-7 // 2", "7 \\\\ 3", "-7 \\\\ 3",
             "2 ** 3", "7 mod: 3", "-7 rem: 3", "-7 quo: 2", "2 raisedTo: 5", "0.3 vmin: 0.5", "0.3 vmax: 0.5",
             "0.37 roundTo: 0.25", "0.3 max: 0.5", "0.3 min: 0.5", "-0.3 maxAbs: 0.2", "-0.3 minAbs: 0.2", "0.37 truncateTo: 0.25",
             "0.37 ceilingTo: 0.25", "100 log: 10", "100 floorLog: 10", "0.3 between: 0 and: 1"})
    {
        sources.emplace_back(expression);
    }
    std::vector<std::string> apart;
    for (const auto& source : sources)
    {
        const auto val = talas::expr::parse(source, talas::expr::ExpressionLanguageVersion::ValV1);
        const auto realTime = val.ok() ? talas::expr::evaluate(*val.expression, {}) : talas::expr::EvaluationResult{};
        SoundHost host;
        const auto parsed = st::parse(source);
        const auto compiled = parsed.ok() ? st::run(*parsed.program, host) : st::RunResult{};
        const auto number = compiled.ok()
            ? (compiled.value.kind == st::Value::Kind::Boolean ? (compiled.value.boolean ? 1.0 : 0.0)
                                                                : st::numberOf(compiled.value).number)
            : std::numeric_limits<double>::quiet_NaN();
        if (!val.ok() || !realTime.ok() || !(std::abs(number - realTime.value.value) < 1.0e-9))
            apart.push_back(source + " (Val " + std::to_string(realTime.value.value) + ", Smalltalk " + std::to_string(number) + ")");
    }
    std::string list;
    for (const auto& source : apart)
        list += "\n  " + source;
    require(apart.empty(), "these Val messages should answer the same in Smalltalk:" + list);
    const auto timed = evaluate("!KeyDown ramp: 2 s");
    const auto smoothed = evaluate("!Fader smoothed");
    require(timed.kind == st::Value::Kind::Expression && smoothed.kind == st::Value::Kind::Expression,
        "a function of time should pass through Smalltalk as the expression to play");
}

// Format 156: a project taking the floor-based rounding rule runs its
// Scripts, transformations and fields with rounded adding 0.5 and
// truncating toward negative infinity, so -2.5 rounded is -2.0, and
// truncated rounding toward negative infinity, so -2.5 truncated is -3;
// the table's other examples answer as in any project. Without the choice
// rounded and truncated are Smalltalk's, and either way Val's fields agree
// with Smalltalk.
void testManualTableRoundingTable()
{
    const auto value = [](std::string_view source, talas::expr::RoundingRule rule) {
        st::EvaluationHost host;
        host.setRounding(rule);
        const auto parsed = st::parse(source);
        require(parsed.ok(), "should parse: " + std::string{source});
        const auto ran = st::run(*parsed.program, host);
        require(ran.ok(), "should run: " + std::string{source}
                + (ran.diagnostic ? ": " + ran.diagnostic->message : std::string{}));
        return ran.value;
    };
    const auto manual = talas::expr::RoundingRule::ManualTable;
    const auto smalltalk = talas::expr::RoundingRule::Smalltalk;
    talas::expr::ExpressionDefinitionLibrary manualFields;
    manualFields.rounding = manual;
    struct Row
    {
        std::string_view source;
        double manual;
        double smalltalk;
    };
    for (const auto& row : {
             // The table's examples.
             Row{"-7 // 3", -3.0, -3.0}, Row{"7 // 3", 2.0, 2.0}, Row{"-7 \\\\ 3", 2.0, 2.0},
             Row{"7 \\\\ 3", 1.0, 1.0}, Row{"2 inverse", 0.5, 0.5}, Row{"-7 quo: 3", -2.0, -2.0},
             Row{"7 quo: 3", 2.0, 2.0}, Row{"-7 rem: 3", -1.0, -1.0}, Row{"7 rem: 3", 1.0, 1.0},
             Row{"-2.5 rounded", -2.0, -3.0}, Row{"-2.5 truncated", -3.0, -2.0}, Row{"5 vmax: 2", 5.0, 5.0},
             Row{"5 vmin: 2", 2.0, 2.0},
             // rounded: add 0.5 and truncate toward negative infinity.
             Row{"2.5 rounded", 3.0, 3.0}, Row{"-0.5 rounded", 0.0, -1.0}, Row{"-3.5 rounded", -3.0, -4.0},
             Row{"(-5/2) rounded", -2.0, -3.0}, Row{"(7/2) rounded", 4.0, 4.0},
             // truncated: round toward negative infinity.
             Row{"3.7 truncated", 3.0, 3.0}, Row{"-3.2 truncated", -4.0, -3.0}, Row{"(-5/2) truncated", -3.0, -2.0},
         })
    {
        const auto underManual = st::numberOf(value(row.source, manual)).number;
        const auto underSmalltalk = st::numberOf(value(row.source, smalltalk)).number;
        require(underManual == row.manual, "" + std::string{row.source} + " should be "
                + std::to_string(row.manual) + " under the floor-based rule, not " + std::to_string(underManual));
        require(underSmalltalk == row.smalltalk, "" + std::string{row.source} + " should be "
                + std::to_string(row.smalltalk) + " under Smalltalk's, not " + std::to_string(underSmalltalk));
        const auto field = talas::expr::parse(row.source, talas::expr::ExpressionLanguageVersion::ValV1, manualFields);
        const auto played = field.ok() ? talas::expr::evaluate(*field.expression, {}) : talas::expr::EvaluationResult{};
        require(field.ok() && played.ok() && played.value.value == row.manual,
            "a field's " + std::string{row.source} + " should agree with Smalltalk's under the table");
    }
    // 2 inverse is the fraction 1/2, as the table prints it.
    require(textIs(value("2 inverse printString", manual), "1/2"), "2 inverse should print 1/2");
    // A list field's own Smalltalk program rounds as the project does.
    const auto program = st::listFieldNumbers("{Array with: -2.5 rounded with: -2.5 truncated}", true, manual);
    require(program.has_value() && program->find("-2") != std::string::npos && program->find("-3") != std::string::npos
            && program->find("-2") < program->find("-3"),
        "a list field's program should round as the table does, not "
            + program.value_or("nothing"));
}

int main()
{
    try
    {
        testPrecedenceAndArithmetic();
        testValMessagesHaveSmalltalkEquivalents();
        testManualTableRoundingTable();
        testStackTracesTellWhereAnErrorHappened();
        testStringsOfTheSameCharactersNeedNotBeTheSameObject();
        testGraphsOfPointsAndValues();
        testQuestionsForTheUser();
        testUnits();
        testEvaluateAnswersWhatTheSelectionPrints();
        testParameterTransformerExamples();
        testBlocksLoopsAndTemporaries();
        testFieldText();
        testPitchNamesAndTempo();
        testEventValuesAndTalasVariables();
        testCollectBuildsArraysOfExpressions();
        testRandomNumbers();
        testTimesAsTimeCode();
        testOneOverFNumbers();
        testHostSendsBack();
        testArrays();
        testManualSmalltalkChapter();
        testScriptsShowValues();
        testStreamsAndCollections();
        testQuickReferenceMessages();
        testBagsDictionariesIntegersAndStreams();
        testDoublesAndBlockTemporaries();
        testHyperbolicAndBesselFunctions();
        testListFieldPrograms();
        testFieldProgramsBuildVal();
        testDatesAndTimes();
        testComplexNumbers();
        testMoreCollectionMessages();
        testFilenames();
        testSamplesFiles();
        testScoreReadingMessages();
        testSmalltalkChapterRules();
        testExactIntegersAndRatios();
        testQuickReferenceFailuresAndPrinting();
        testBinaryStreams();
        testSessionsKeepObjectsBetweenRuns();
        testPoints();
        testPrintStringsAndAssociations();
        testRectangles();
        testDocumentedSmalltalkExamples();
        testFilesThroughTheHost();
        testObjectsNobodyHoldsAreMadeAgain();
        testRandomNewDiffersEachRun();
        testQuickReferenceClassesAndCounts();
        testLoopStructures();
        testMarkovChains();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Smalltalk subset tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Smalltalk subset tests passed\n";
    return 0;
}
