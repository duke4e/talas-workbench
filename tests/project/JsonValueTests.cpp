#include "JsonValue.h"

#include <array>
#include <clocale>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool hasErrorContaining(
    const talas::project::json::ParseResult& result,
    const std::string& fragment)
{
    for (const auto& error : result.errors)
    {
        if (error.message.find(fragment) != std::string::npos)
            return true;
    }
    return false;
}

talas::project::json::ParseLimits boundedLimits()
{
    talas::project::json::ParseLimits limits;
    limits.maximumInputBytes = 1024;
    limits.maximumNestingDepth = 8;
    limits.maximumValues = 64;
    limits.maximumDecodedStringBytes = 256;
    limits.maximumContainerElements = 32;
    limits.requireValidUtf8 = true;
    return limits;
}

void testBoundedParseAcceptsAnInBudgetDocument()
{
    const auto parsed = talas::project::json::parse(
        R"json({"name":"Grüße","values":[1,true,null]})json",
        boundedLimits());

    require(parsed.ok(), "in-budget valid UTF-8 JSON should parse");
    require(
        parsed.value.isObject(),
        "bounded parsing should preserve the ordinary JSON value model");
}

void testInputByteBudgetRejectsBeforeParsing()
{
    auto limits = boundedLimits();
    limits.maximumInputBytes = 2;

    const auto parsed = talas::project::json::parse("null", limits);

    require(
        !parsed.ok() && hasErrorContaining(parsed, "input byte limit"),
        "oversized input should fail at the explicit byte boundary");
}

void testNestingDepthBudgetCountsContainers()
{
    auto limits = boundedLimits();
    limits.maximumNestingDepth = 1;

    require(
        talas::project::json::parse("[0]", limits).ok(),
        "one root container should fit a depth-one budget");
    const auto nested = talas::project::json::parse("[[]]", limits);
    require(
        !nested.ok() && hasErrorContaining(nested, "nesting depth"),
        "a nested container should exceed a depth-one budget");
    require(
        talas::project::json::parse("0", {
            .maximumInputBytes = 1,
            .maximumNestingDepth = 0,
            .maximumValues = 1,
            .maximumDecodedStringBytes = 0,
            .maximumContainerElements = 0,
            .requireValidUtf8 = true,
        }).ok(),
        "a scalar should not consume container nesting depth");
}

void testValueAndContainerBudgetsRejectBeforeGrowth()
{
    auto valueLimits = boundedLimits();
    valueLimits.maximumValues = 2;
    const auto tooManyValues =
        talas::project::json::parse("[0,1]", valueLimits);
    require(
        !tooManyValues.ok()
            && hasErrorContaining(tooManyValues, "value limit"),
        "the root and both array values should exceed a two-value budget");

    auto elementLimits = boundedLimits();
    elementLimits.maximumContainerElements = 1;
    const auto tooManyArrayElements =
        talas::project::json::parse("[0,1]", elementLimits);
    require(
        !tooManyArrayElements.ok()
            && hasErrorContaining(tooManyArrayElements, "element limit"),
        "an array should reject before retaining a second element");
    const auto tooManyObjectMembers = talas::project::json::parse(
        R"json({"a":0,"b":1})json",
        elementLimits);
    require(
        !tooManyObjectMembers.ok()
            && hasErrorContaining(tooManyObjectMembers, "element limit"),
        "an object should reject before retaining a second member");
}

void testDecodedStringBudgetIncludesKeysAndValues()
{
    auto limits = boundedLimits();
    limits.maximumDecodedStringBytes = 3;
    require(
        talas::project::json::parse(R"json({"a":"bc"})json", limits).ok(),
        "one key byte and two value bytes should fit exactly");

    limits.maximumDecodedStringBytes = 2;
    const auto parsed =
        talas::project::json::parse(R"json({"a":"bc"})json", limits);
    require(
        !parsed.ok() && hasErrorContaining(parsed, "string byte limit"),
        "decoded keys and values should share one aggregate string budget");
}

void testStrictUtf8RejectsMalformedRawBytes()
{
    std::string malformed{"\""};
    malformed.push_back(static_cast<char>(0xc0));
    malformed.push_back(static_cast<char>(0xaf));
    malformed.push_back('"');

    auto strict = boundedLimits();
    const auto rejected = talas::project::json::parse(malformed, strict);
    require(
        !rejected.ok() && hasErrorContaining(rejected, "UTF-8"),
        "strict bounded parsing should reject overlong UTF-8");

    strict.requireValidUtf8 = false;
    require(
        talas::project::json::parse(malformed, strict).ok(),
        "legacy callers should be able to retain the existing raw-byte policy");
}

void testExactObjectMemberStringBudgetRejectsDuringDecode()
{
    auto limits = boundedLimits();
    limits.maximumInputBytes = 4096;
    limits.maximumDecodedStringBytes = 4096;
    const std::array boundedStringMembers{
        talas::project::json::ParseLimits::BoundedStringMember{
            "sourceIdentifier", 3},
    };
    limits.boundedStringMembers = boundedStringMembers;

    require(
        talas::project::json::parse(
            R"json({"sourceIdentifier":"abc","other":"unbounded"})json",
            limits).ok(),
        "an exact bounded member should accept its byte limit without constraining unrelated strings");
    const auto rawOversized = talas::project::json::parse(
        R"json({"sourceIdentifier":"abcd"})json",
        limits);
    require(
        !rawOversized.ok()
            && hasErrorContaining(rawOversized, "object member string byte limit"),
        "an exact bounded member should reject before retaining its next decoded byte");
    const auto escapedOversized = talas::project::json::parse(
        R"json({"sourceIdentifier":"\u0061\u0062\u0063\u0064"})json",
        limits);
    require(
        !escapedOversized.ok()
            && hasErrorContaining(escapedOversized, "object member string byte limit"),
        "escaped text should be bounded by decoded member bytes");
    require(
        !talas::project::json::parse(
            R"json({"sourceIdentifier":{"nested":"value"}})json",
            limits).ok(),
        "a bounded string member should reject a container before traversing it");

    const std::array contextualStringMembers{
        talas::project::json::ParseLimits::BoundedStringMember{
            "title", 3, "markers"},
    };
    limits.boundedStringMembers = contextualStringMembers;
    require(
        talas::project::json::parse(
            R"json({"title":"unrelated","markers":[{"title":"abc"}]})json",
            limits).ok(),
        "a contextual member bound should not constrain the same key outside its owning collection");
    require(
        !talas::project::json::parse(
            R"json({"markers":[{"title":"abcd"}]})json",
            limits).ok(),
        "a contextual member bound should reject oversized text inside its owning collection");
}

void testExactObjectMemberContainerBudgetRejectsDuringParse()
{
    auto limits = boundedLimits();
    limits.maximumInputBytes = 4096;
    const std::array boundedContainerMembers{
        talas::project::json::ParseLimits::BoundedContainerMember{
            "tracks", 2},
    };
    limits.boundedContainerMembers = boundedContainerMembers;

    require(
        talas::project::json::parse(
            R"json({"tracks":[{},{}],"other":[1,2,3]})json",
            limits).ok(),
        "an exact bounded container should accept its element limit without constraining unrelated arrays");
    const auto oversized = talas::project::json::parse(
        R"json({"tracks":[{},{},{}]})json",
        limits);
    require(
        !oversized.ok()
            && hasErrorContaining(
                oversized,
                "container element limit"),
        "an exact bounded container should reject before retaining its next element");

    const std::array aggregateContainerMembers{
        talas::project::json::ParseLimits::BoundedContainerMember{
            "points", 3, true},
    };
    limits.boundedContainerMembers = aggregateContainerMembers;
    require(
        talas::project::json::parse(
            R"json({"first":{"points":[1,2]},"second":{"points":[3]}})json",
            limits).ok(),
        "aggregate member containers should accept their shared element limit");
    require(
        !talas::project::json::parse(
            R"json({"first":{"points":[1,2]},"second":{"points":[3,4]}})json",
            limits).ok(),
        "aggregate member containers should reject the first element beyond their shared limit");

    const std::array sharedNamedContainerMembers{
        talas::project::json::ParseLimits::BoundedContainerMember{
            "cells", 2, false, {}, 0, 3},
        talas::project::json::ParseLimits::BoundedContainerMember{
            "commands", 2, false, {}, 0, 3},
    };
    limits.boundedContainerMembers = sharedNamedContainerMembers;
    require(
        talas::project::json::parse(
            R"json({"cells":[1,2],"commands":[3]})json",
            limits).ok(),
        "different named containers should accept their shared aggregate budget");
    require(
        !talas::project::json::parse(
            R"json({"cells":[1,2],"commands":[3,4]})json",
            limits).ok(),
        "different named containers should reject the first element beyond their shared budget");
    require(
        !talas::project::json::parse(
            R"json({"cells":[1,2,3],"commands":[]})json",
            limits).ok(),
        "a shared aggregate budget should preserve each member's smaller individual limit");
}

void testZeroValueBudgetAndDuplicateKeysFailClosed()
{
    auto limits = boundedLimits();
    limits.maximumValues = 0;
    const auto noValues = talas::project::json::parse("null", limits);
    require(
        !noValues.ok() && hasErrorContaining(noValues, "value limit"),
        "a zero-value budget should reject the root before construction");

    const auto duplicate = talas::project::json::parse(
        R"json({"same":0,"same":1})json",
        boundedLimits());
    require(
        !duplicate.ok() && hasErrorContaining(duplicate, "Duplicate"),
        "bounded parsing must preserve duplicate-key rejection");
}

void testNamedSubtreeBudgetPreservesLegacyOutsideIt()
{
    auto limits = boundedLimits();
    limits.maximumInputBytes = 4096;
    limits.maximumValues = 128;
    limits.maximumNestingDepth = 32;
    limits.maximumDecodedStringBytes = 4096;
    const std::array boundedSubtrees{
        talas::project::json::ParseLimits::BoundedSubtreeMember{
            "closed", {}, 6, 2, 8, 4, 3},
    };
    limits.boundedSubtreeMembers = boundedSubtrees;

    require(
        talas::project::json::parse(
            R"json({"legacy":{"unboundedName":"unbounded value"},"closed":{"a":"bc"}})json",
            limits).ok(),
        "a named subtree should not impose its new-schema limits on historical siblings");
    require(
        !talas::project::json::parse(
            R"json({"closed":{"oversized":0}})json",
            limits).ok(),
        "bounded subtree object keys should fail during decode");
    require(
        !talas::project::json::parse(
            R"json({"closed":{"a":[[[0]]]}})json",
            limits).ok(),
        "bounded subtree nesting should fail before retaining the over-depth value");
    require(
        !talas::project::json::parse(
            R"json({"closed":{"a":1234}})json",
            limits).ok(),
        "bounded subtree numeric tokens should fail before token allocation");
    require(
        !talas::project::json::parse(
            R"json({"closed":{"a":"123456789"}})json",
            limits).ok(),
        "bounded subtree strings should share one local decoded-byte budget");
}
}

void testSubnormalAndLocaleIndependentNumbersParse()
{
    const auto subnormal = talas::project::json::parse(
        "[4.9406564584124654e-324, 2.2250738585072009e-308]");
    require(
        subnormal.ok()
            && subnormal.value.array()[0].number()
                == std::numeric_limits<double>::denorm_min()
            && subnormal.value.array()[1].number()
                < std::numeric_limits<double>::min(),
        "subnormal numbers written by to_chars must parse back");
    require(
        !talas::project::json::parse("1e400").ok(),
        "numbers beyond the double range must stay invalid");

    const char* commaLocale = nullptr;
    for (const char* candidate :
         {"de_DE.UTF-8", "hr_HR.UTF-8", "fr_FR.UTF-8", "de_DE"})
    {
        if (std::setlocale(LC_NUMERIC, candidate) != nullptr)
        {
            commaLocale = candidate;
            break;
        }
    }
    require(
        commaLocale != nullptr,
        "a decimal-comma locale is required for this test");
    const auto localized = talas::project::json::parse("0.5");
    std::setlocale(LC_NUMERIC, "C");
    require(
        localized.ok() && localized.value.number() == 0.5,
        "number parsing must not depend on the process locale");
}

void testStandardEscapesSurrogatePairsAndByteOrderMarkParse()
{
    const auto escapes = talas::project::json::parse(
        R"json("a\/b\bc\fd")json");
    require(
        escapes.ok() && escapes.value.string() == "a/b\bc\fd",
        "\\/, \\b and \\f are valid JSON escapes");

    const auto pair = talas::project::json::parse(
        R"json("\ud83d\ude00\u00e9")json",
        boundedLimits());
    require(
        pair.ok()
            && pair.value.string()
                == "\xF0\x9F\x98\x80\xC3\xA9",
        "a surrogate pair must decode to one supplementary code point");

    for (const auto* unpaired : {
             R"json("\ud83d")json",
             R"json("\ude00")json",
             R"json("\ud83dx")json",
             R"json("\ud83d\u0041")json",
         })
    {
        require(
            !talas::project::json::parse(unpaired).ok(),
            std::string{"unpaired surrogates must stay invalid: "}
                + unpaired);
    }

    const auto withBom = talas::project::json::parse(
        "\xEF\xBB\xBF{\"format\":\"talas.project\"}",
        boundedLimits());
    require(
        withBom.ok() && withBom.value.isObject(),
        "a leading UTF-8 byte order mark must be ignored");
    require(
        !talas::project::json::parse("{}\xEF\xBB\xBF").ok(),
        "a byte order mark is only ignored before the document");
}

int main()
{
    testBoundedParseAcceptsAnInBudgetDocument();
    testInputByteBudgetRejectsBeforeParsing();
    testNestingDepthBudgetCountsContainers();
    testValueAndContainerBudgetsRejectBeforeGrowth();
    testDecodedStringBudgetIncludesKeysAndValues();
    testStrictUtf8RejectsMalformedRawBytes();
    testExactObjectMemberStringBudgetRejectsDuringDecode();
    testExactObjectMemberContainerBudgetRejectsDuringParse();
    testZeroValueBudgetAndDuplicateKeysFailClosed();
    testNamedSubtreeBudgetPreservesLegacyOutsideIt();
    testSubnormalAndLocaleIndependentNumbersParse();
    testStandardEscapesSurrogatePairsAndByteOrderMarkParse();
    return 0;
}
