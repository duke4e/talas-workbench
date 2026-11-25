#include "JsonValue.h"

#include "talas/core/FromChars.h"

#include <charconv>
#include <cstddef>
#include <optional>
#include <unordered_set>

namespace talas::project::json
{
Value Value::nullValue()
{
    return {};
}

Value Value::objectValue(Object object)
{
    Value value;
    value.type_ = Type::Object;
    value.object_ = std::move(object);
    return value;
}

Value Value::arrayValue(Array array)
{
    Value value;
    value.type_ = Type::Array;
    value.array_ = std::move(array);
    return value;
}

Value Value::stringValue(std::string string)
{
    Value value;
    value.type_ = Type::String;
    value.string_ = std::move(string);
    return value;
}

Value Value::numberValue(double number, std::string token)
{
    Value value;
    value.type_ = Type::Number;
    value.number_ = number;
    value.numberToken_ = std::move(token);
    return value;
}

Value Value::booleanValue(bool boolean)
{
    Value value;
    value.type_ = Type::Boolean;
    value.boolean_ = boolean;
    return value;
}

Value::Type Value::type() const
{
    return type_;
}

bool Value::isNull() const
{
    return type_ == Type::Null;
}

bool Value::isObject() const
{
    return type_ == Type::Object;
}

bool Value::isArray() const
{
    return type_ == Type::Array;
}

bool Value::isString() const
{
    return type_ == Type::String;
}

bool Value::isNumber() const
{
    return type_ == Type::Number;
}

bool Value::isBoolean() const
{
    return type_ == Type::Boolean;
}

const Value::Object& Value::object() const
{
    return object_;
}

const Value::Array& Value::array() const
{
    return array_;
}

const std::string& Value::string() const
{
    return string_;
}

double Value::number() const
{
    return number_;
}

const std::string& Value::numberToken() const
{
    return numberToken_;
}

bool Value::boolean() const
{
    return boolean_;
}

namespace
{
bool isWhitespace(char value)
{
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

bool isDigit(char value)
{
    return value >= '0' && value <= '9';
}

int hexValue(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool validUtf8Bytes(std::string_view value)
{
    std::size_t offset = 0;
    while (offset < value.size())
    {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU)
        {
            ++offset;
            continue;
        }

        auto continuation = [&](std::size_t index) {
            return index < value.size()
                && (static_cast<unsigned char>(value[index]) & 0xc0U) == 0x80U;
        };

        if (first >= 0xc2U && first <= 0xdfU)
        {
            if (!continuation(offset + 1U))
                return false;
            offset += 2U;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU)
        {
            if (!continuation(offset + 1U)
                || !continuation(offset + 2U))
            {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[offset + 1U]);
            if ((first == 0xe0U && second < 0xa0U)
                || (first == 0xedU && second > 0x9fU))
            {
                return false;
            }
            offset += 3U;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U)
        {
            if (!continuation(offset + 1U)
                || !continuation(offset + 2U)
                || !continuation(offset + 3U))
            {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(value[offset + 1U]);
            if ((first == 0xf0U && second < 0x90U)
                || (first == 0xf4U && second > 0x8fU))
            {
                return false;
            }
            offset += 4U;
            continue;
        }

        return false;
    }
    return true;
}

struct SubtreeBudget
{
    std::size_t rootNestingDepth = 0;
    std::size_t maximumValues = 0;
    std::size_t maximumNestingDepth = 0;
    std::size_t maximumDecodedStringBytes = 0;
    std::size_t maximumObjectKeyBytes = 0;
    std::size_t maximumScalarTokenBytes = 0;
    std::size_t values = 0;
    std::size_t decodedStringBytes = 0;
};

class Parser
{
public:
    Parser(std::string_view text, const ParseLimits& limits)
        : text_(text),
          limits_(limits),
          boundedContainerElementCounts_(
              limits.boundedContainerMembers.size(),
              0U),
          boundedStringByteCounts_(
              limits.boundedStringMembers.size(),
              0U)
    {
    }

    ParseResult parseDocument()
    {
        ParseResult result;
        if (text_.starts_with("\xEF\xBB\xBF"))
            offset_ = 3U;
        skipWhitespace();
        if (!parseValue(result.value, 0))
        {
            result.errors = std::move(errors_);
            return result;
        }

        skipWhitespace();
        if (offset_ != text_.size())
            fail("Unexpected trailing input");

        result.errors = std::move(errors_);
        return result;
    }

private:
    bool parseValue(
        Value& value,
        std::size_t nestingDepth,
        std::size_t stringByteLimit =
            std::numeric_limits<std::size_t>::max(),
        std::size_t containerElementLimit =
            std::numeric_limits<std::size_t>::max(),
        std::string_view containerName = {},
        std::string_view parentContainerName = {},
        std::size_t* aggregateElementCount = nullptr,
        std::size_t aggregateElementLimit =
            std::numeric_limits<std::size_t>::max(),
        std::size_t* aggregateStringByteCount = nullptr,
        std::size_t aggregateStringByteLimit =
            std::numeric_limits<std::size_t>::max(),
        SubtreeBudget* subtreeBudget = nullptr)
    {
        if (valueCount_ >= limits_.maximumValues)
            return fail("JSON value limit exceeded");
        ++valueCount_;
        if (subtreeBudget != nullptr)
        {
            if (subtreeBudget->values >= subtreeBudget->maximumValues)
                return fail("JSON bounded subtree value limit exceeded");
            ++subtreeBudget->values;
            if (nestingDepth < subtreeBudget->rootNestingDepth
                || nestingDepth - subtreeBudget->rootNestingDepth
                    > subtreeBudget->maximumNestingDepth)
            {
                return fail("JSON bounded subtree nesting limit exceeded");
            }
        }

        skipWhitespace();
        if (offset_ >= text_.size())
            return fail("Expected JSON value");
        if (stringByteLimit
                != std::numeric_limits<std::size_t>::max()
            && text_[offset_] != '"')
        {
            return fail("Expected bounded object member string");
        }

        switch (text_[offset_])
        {
        case '{':
            if (nestingDepth >= limits_.maximumNestingDepth)
                return fail("JSON nesting depth limit exceeded");
            return parseObject(
                value,
                nestingDepth + 1U,
                containerElementLimit,
                containerName,
                parentContainerName,
                aggregateElementCount,
                aggregateElementLimit,
                subtreeBudget);
        case '[':
            if (nestingDepth >= limits_.maximumNestingDepth)
                return fail("JSON nesting depth limit exceeded");
            return parseArray(
                value,
                nestingDepth + 1U,
                containerElementLimit,
                containerName,
                parentContainerName,
                aggregateElementCount,
                aggregateElementLimit,
                subtreeBudget);
        case '"':
        {
            std::string string;
            if (!parseString(
                    string,
                    stringByteLimit,
                    aggregateStringByteCount,
                    aggregateStringByteLimit,
                    subtreeBudget))
                return false;
            value = Value::stringValue(std::move(string));
            return true;
        }
        case 't':
            return parseLiteral("true", Value::booleanValue(true), value);
        case 'f':
            return parseLiteral("false", Value::booleanValue(false), value);
        case 'n':
            return parseLiteral("null", Value::nullValue(), value);
        default:
            if (text_[offset_] == '-' || isDigit(text_[offset_]))
                return parseNumber(value, subtreeBudget);
            return fail("Expected JSON value");
        }
    }

    bool parseObject(
        Value& value,
        std::size_t nestingDepth,
        std::size_t memberElementLimit,
        std::string_view containerName,
        std::string_view parentContainerName,
        std::size_t* aggregateElementCount,
        std::size_t aggregateElementLimit,
        SubtreeBudget* subtreeBudget)
    {
        ++offset_;
        skipWhitespace();

        Value::Object object;
        if (consume('}'))
        {
            value = Value::objectValue(std::move(object));
            return true;
        }

        std::unordered_set<std::string> memberKeys;
        while (offset_ < text_.size())
        {
            if (object.size() >= limits_.maximumContainerElements
                || object.size() >= memberElementLimit
                || (aggregateElementCount != nullptr
                    && *aggregateElementCount
                        >= aggregateElementLimit))
                return fail("JSON container element limit exceeded");
            if (text_[offset_] != '"')
                return fail("Expected object member name");

            std::string key;
            if (!parseString(
                    key,
                    subtreeBudget == nullptr
                        ? std::numeric_limits<std::size_t>::max()
                        : subtreeBudget->maximumObjectKeyBytes,
                    nullptr,
                    std::numeric_limits<std::size_t>::max(),
                    subtreeBudget))
                return false;

            if (!memberKeys.emplace(key).second)
                return fail("Duplicate object member: " + key);

            skipWhitespace();
            if (!consume(':'))
                return fail("Expected ':' after object member name");

            Value memberValue;
            auto memberStringLimit =
                std::numeric_limits<std::size_t>::max();
            std::size_t* memberAggregateStringByteCount = nullptr;
            auto memberAggregateStringByteLimit =
                std::numeric_limits<std::size_t>::max();
            auto boundedStringAllowsNull = false;
            for (const auto& bounded : limits_.boundedStringMembers)
            {
                if (key == bounded.name
                    && (bounded.containerName.empty()
                        || bounded.containerName == containerName)
                    && (bounded.parentContainerName.empty()
                        || bounded.parentContainerName
                            == parentContainerName))
                {
                    memberStringLimit = bounded.maximumBytes;
                    boundedStringAllowsNull = bounded.allowNull;
                    if (bounded.aggregateBudgetIndex
                        != std::numeric_limits<std::size_t>::max())
                    {
                        if (bounded.aggregateBudgetIndex
                            >= boundedStringByteCounts_.size())
                        {
                            return fail(
                                "Invalid aggregate string budget index");
                        }
                        memberAggregateStringByteCount =
                            &boundedStringByteCounts_[
                                bounded.aggregateBudgetIndex];
                        memberAggregateStringByteLimit =
                            bounded.aggregateMaximumBytes;
                    }
                    break;
                }
            }
            skipWhitespace();
            if (memberStringLimit
                    != std::numeric_limits<std::size_t>::max()
                && (offset_ >= text_.size()
                    || (text_[offset_] != '"'
                        && !(boundedStringAllowsNull
                            && text_.substr(offset_).starts_with("null")))))
            {
                return fail(
                    "Expected bounded object member string: "
                    + key);
            }
            auto memberContainerLimit =
                std::numeric_limits<std::size_t>::max();
            std::size_t* memberAggregateCount = nullptr;
            auto memberAggregateElementLimit =
                std::numeric_limits<std::size_t>::max();
            for (std::size_t index = 0;
                 index < limits_.boundedContainerMembers.size();
                 ++index)
            {
                const auto& bounded =
                    limits_.boundedContainerMembers[index];
                if (key == bounded.name
                    && (bounded.ownerContainerName.empty()
                        || bounded.ownerContainerName == containerName))
                {
                    memberContainerLimit = bounded.maximumElements;
                    if (bounded.aggregate)
                    {
                        memberAggregateCount =
                            &boundedContainerElementCounts_[index];
                        memberAggregateElementLimit =
                            bounded.aggregateMaximumElements
                                == std::numeric_limits<std::size_t>::max()
                            ? bounded.maximumElements
                            : bounded.aggregateMaximumElements;
                    }
                    else if (bounded.aggregateBudgetIndex
                             != std::numeric_limits<std::size_t>::max())
                    {
                        if (bounded.aggregateBudgetIndex
                            >= boundedContainerElementCounts_.size())
                        {
                            return fail(
                                "Invalid aggregate container budget index");
                        }
                        memberAggregateCount =
                            &boundedContainerElementCounts_[
                                bounded.aggregateBudgetIndex];
                        memberAggregateElementLimit =
                            bounded.aggregateMaximumElements
                                == std::numeric_limits<std::size_t>::max()
                            ? bounded.maximumElements
                            : bounded.aggregateMaximumElements;
                    }
                    break;
                }
            }
            std::optional<SubtreeBudget> ownedSubtreeBudget;
            auto* memberSubtreeBudget = subtreeBudget;
            if (memberSubtreeBudget == nullptr)
            {
                for (const auto& bounded : limits_.boundedSubtreeMembers)
                {
                    if (key == bounded.name
                        && (bounded.ownerContainerName.empty()
                            || bounded.ownerContainerName == containerName))
                    {
                        ownedSubtreeBudget.emplace(SubtreeBudget{
                            nestingDepth,
                            bounded.maximumValues,
                            bounded.maximumNestingDepth,
                            bounded.maximumDecodedStringBytes,
                            bounded.maximumObjectKeyBytes,
                            bounded.maximumScalarTokenBytes,
                        });
                        memberSubtreeBudget = &*ownedSubtreeBudget;
                        break;
                    }
                }
            }
            if (!parseValue(
                    memberValue,
                    nestingDepth,
                    boundedStringAllowsNull
                            && offset_ < text_.size()
                            && text_[offset_] == 'n'
                        ? std::numeric_limits<std::size_t>::max()
                        : memberStringLimit,
                    memberContainerLimit,
                    key,
                    containerName,
                    memberAggregateCount,
                    memberAggregateElementLimit,
                    memberAggregateStringByteCount,
                    memberAggregateStringByteLimit,
                    memberSubtreeBudget))
                return false;
            if (aggregateElementCount != nullptr)
                ++*aggregateElementCount;
            object.emplace_back(std::move(key), std::move(memberValue));

            skipWhitespace();
            if (consume('}'))
            {
                value = Value::objectValue(std::move(object));
                return true;
            }
            if (!consume(','))
                return fail("Expected ',' or '}' after object member");
            skipWhitespace();
        }

        return fail("Unterminated object");
    }

    bool parseArray(
        Value& value,
        std::size_t nestingDepth,
        std::size_t memberElementLimit,
        std::string_view containerName,
        std::string_view parentContainerName,
        std::size_t* aggregateElementCount,
        std::size_t aggregateElementLimit,
        SubtreeBudget* subtreeBudget)
    {
        ++offset_;
        skipWhitespace();

        Value::Array array;
        if (consume(']'))
        {
            value = Value::arrayValue(std::move(array));
            return true;
        }

        while (offset_ < text_.size())
        {
            if (array.size() >= limits_.maximumContainerElements
                || array.size() >= memberElementLimit
                || (aggregateElementCount != nullptr
                    && *aggregateElementCount
                        >= aggregateElementLimit))
                return fail("JSON container element limit exceeded");
            Value element;
            if (!parseValue(
                    element,
                    nestingDepth,
                    std::numeric_limits<std::size_t>::max(),
                    std::numeric_limits<std::size_t>::max(),
                    containerName,
                    parentContainerName,
                    nullptr,
                    std::numeric_limits<std::size_t>::max(),
                    nullptr,
                    std::numeric_limits<std::size_t>::max(),
                    subtreeBudget))
                return false;
            if (aggregateElementCount != nullptr)
                ++*aggregateElementCount;
            array.push_back(std::move(element));

            skipWhitespace();
            if (consume(']'))
            {
                value = Value::arrayValue(std::move(array));
                return true;
            }
            if (!consume(','))
                return fail("Expected ',' or ']' after array element");
            skipWhitespace();
        }

        return fail("Unterminated array");
    }

    bool parseString(
        std::string& value,
        std::size_t byteLimit =
            std::numeric_limits<std::size_t>::max(),
        std::size_t* aggregateByteCount = nullptr,
        std::size_t aggregateByteLimit =
            std::numeric_limits<std::size_t>::max(),
        SubtreeBudget* subtreeBudget = nullptr)
    {
        if (!consume('"'))
            return fail("Expected string");

        while (offset_ < text_.size())
        {
            const char next = text_[offset_++];
            if (next == '"')
            {
                if (limits_.requireValidUtf8 && !validUtf8Bytes(value))
                    return fail("Invalid UTF-8 in string");
                return true;
            }

            if (static_cast<unsigned char>(next) < 0x20U)
                return fail("Unescaped control character in string");

            if (next != '\\')
            {
                if (!appendDecodedByte(
                        value, next, byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                continue;
            }

            if (offset_ >= text_.size())
                return fail("Unterminated string escape");

            const char escape = text_[offset_++];
            switch (escape)
            {
            case '"':
                if (!appendDecodedByte(value, '"', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case '\\':
                if (!appendDecodedByte(value, '\\', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 'n':
                if (!appendDecodedByte(value, '\n', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 'r':
                if (!appendDecodedByte(value, '\r', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 't':
                if (!appendDecodedByte(value, '\t', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case '/':
                if (!appendDecodedByte(value, '/', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 'b':
                if (!appendDecodedByte(value, '\b', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 'f':
                if (!appendDecodedByte(value, '\f', byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            case 'u':
            {
                unsigned int codePoint = 0;
                if (!parseUnicodeEscapeDigits(codePoint))
                    return false;
                if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
                    return fail("Unpaired unicode surrogate escape");
                if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
                {
                    unsigned int lowSurrogate = 0;
                    if (!text_.substr(offset_).starts_with("\\u"))
                        return fail("Unpaired unicode surrogate escape");
                    offset_ += 2U;
                    if (!parseUnicodeEscapeDigits(lowSurrogate))
                        return false;
                    if (lowSurrogate < 0xDC00U || lowSurrogate > 0xDFFFU)
                        return fail("Unpaired unicode surrogate escape");
                    codePoint = 0x10000U
                        + ((codePoint - 0xD800U) << 10U)
                        + (lowSurrogate - 0xDC00U);
                }
                if (!appendDecodedCodePoint(
                        value, codePoint, byteLimit,
                        aggregateByteCount, aggregateByteLimit,
                        subtreeBudget))
                    return false;
                break;
            }
            default:
                return fail("Unsupported string escape");
            }
        }

        return fail("Unterminated string");
    }

    bool parseUnicodeEscapeDigits(unsigned int& codePoint)
    {
        codePoint = 0;
        for (int index = 0; index < 4; ++index)
        {
            if (offset_ >= text_.size())
                return fail("Unterminated unicode escape");
            const int digit = hexValue(text_[offset_++]);
            if (digit < 0)
                return fail("Invalid unicode escape");
            codePoint = (codePoint << 4U) | static_cast<unsigned int>(digit);
        }
        return true;
    }

    bool reserveMemberStringBytes(
        const std::string& output,
        std::size_t count,
        std::size_t byteLimit,
        std::size_t* aggregateByteCount = nullptr,
        std::size_t aggregateByteLimit =
            std::numeric_limits<std::size_t>::max(),
        SubtreeBudget* subtreeBudget = nullptr)
    {
        if (count > byteLimit || output.size() > byteLimit - count)
            return fail("JSON object member string byte limit exceeded");
        if (aggregateByteCount != nullptr
            && (count > aggregateByteLimit
                || *aggregateByteCount > aggregateByteLimit - count))
        {
            return fail(
                "JSON aggregate object member string byte limit exceeded");
        }
        if (!reserveDecodedStringBytes(count))
            return false;
        if (subtreeBudget != nullptr)
        {
            if (count > subtreeBudget->maximumDecodedStringBytes
                || subtreeBudget->decodedStringBytes
                    > subtreeBudget->maximumDecodedStringBytes - count)
            {
                return fail(
                    "JSON bounded subtree decoded string limit exceeded");
            }
            subtreeBudget->decodedStringBytes += count;
        }
        if (aggregateByteCount != nullptr)
            *aggregateByteCount += count;
        return true;
    }

    bool appendDecodedByte(
        std::string& output,
        char value,
        std::size_t byteLimit,
        std::size_t* aggregateByteCount = nullptr,
        std::size_t aggregateByteLimit =
            std::numeric_limits<std::size_t>::max(),
        SubtreeBudget* subtreeBudget = nullptr)
    {
        if (!reserveMemberStringBytes(
            output, 1U, byteLimit,
            aggregateByteCount, aggregateByteLimit,
            subtreeBudget))
            return false;
        output.push_back(value);
        return true;
    }

    bool appendDecodedCodePoint(
        std::string& output,
        unsigned int codePoint,
        std::size_t byteLimit,
        std::size_t* aggregateByteCount = nullptr,
        std::size_t aggregateByteLimit =
            std::numeric_limits<std::size_t>::max(),
        SubtreeBudget* subtreeBudget = nullptr)
    {
        if (codePoint <= 0x7fU)
        {
            return appendDecodedByte(
                output,
                static_cast<char>(codePoint),
                byteLimit,
                aggregateByteCount,
                aggregateByteLimit,
                subtreeBudget);
        }

        const auto byteCount =
            codePoint <= 0x7ffU ? 2U : (codePoint <= 0xffffU ? 3U : 4U);
        if (!reserveMemberStringBytes(
                output, byteCount, byteLimit,
                aggregateByteCount, aggregateByteLimit,
                subtreeBudget))
            return false;
        if (byteCount == 2U)
        {
            output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
            return true;
        }
        if (byteCount == 4U)
        {
            output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
            return true;
        }
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
        return true;
    }

    bool reserveDecodedStringBytes(std::size_t count)
    {
        if (count > limits_.maximumDecodedStringBytes
            || decodedStringBytes_
                > limits_.maximumDecodedStringBytes - count)
        {
            return fail("JSON decoded string byte limit exceeded");
        }
        decodedStringBytes_ += count;
        return true;
    }

    bool parseNumber(Value& value, const SubtreeBudget* subtreeBudget)
    {
        const std::size_t start = offset_;
        const auto scalarTokenLimitExceeded = [&] {
            return subtreeBudget != nullptr
                && offset_ - start
                    > subtreeBudget->maximumScalarTokenBytes;
        };

        if (consume('-') && offset_ >= text_.size())
            return fail("Expected digit after '-'");
        if (scalarTokenLimitExceeded())
            return fail("JSON bounded subtree scalar token limit exceeded");

        if (consume('0'))
        {
            if (offset_ < text_.size() && isDigit(text_[offset_]))
                return fail("Leading zeroes are not allowed in numbers");
        }
        else
        {
            if (offset_ >= text_.size() || !isDigit(text_[offset_]))
                return fail("Expected digit in number");
            while (offset_ < text_.size() && isDigit(text_[offset_]))
            {
                ++offset_;
                if (scalarTokenLimitExceeded())
                    return fail("JSON bounded subtree scalar token limit exceeded");
            }
        }

        if (consume('.'))
        {
            if (offset_ >= text_.size() || !isDigit(text_[offset_]))
                return fail("Expected digit after decimal point");
            while (offset_ < text_.size() && isDigit(text_[offset_]))
            {
                ++offset_;
                if (scalarTokenLimitExceeded())
                    return fail("JSON bounded subtree scalar token limit exceeded");
            }
        }

        if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E'))
        {
            ++offset_;
            if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-'))
                ++offset_;
            if (offset_ >= text_.size() || !isDigit(text_[offset_]))
                return fail("Expected exponent digit");
            while (offset_ < text_.size() && isDigit(text_[offset_]))
            {
                ++offset_;
                if (scalarTokenLimitExceeded())
                    return fail("JSON bounded subtree scalar token limit exceeded");
            }
        }

        const auto tokenSize = offset_ - start;
        if (subtreeBudget != nullptr
            && tokenSize > subtreeBudget->maximumScalarTokenBytes)
        {
            return fail("JSON bounded subtree scalar token limit exceeded");
        }
        const std::string numberText(text_.substr(start, tokenSize));
        // from_chars ignores the process locale and accepts subnormals,
        // which std::to_chars (the writer) emits.
        double number = 0.0;
        const auto* numberEnd = numberText.data() + numberText.size();
        const auto parsed =
            talas::fromChars(numberText.data(), numberEnd, number);
        if (parsed.ec != std::errc{} || parsed.ptr != numberEnd)
            return fail("Invalid number");

        value = Value::numberValue(number, numberText);
        return true;
    }

    bool parseLiteral(std::string_view literal, Value literalValue, Value& value)
    {
        if (text_.substr(offset_, literal.size()) != literal)
            return fail("Expected JSON literal");
        offset_ += literal.size();
        value = std::move(literalValue);
        return true;
    }

    void skipWhitespace()
    {
        while (offset_ < text_.size() && isWhitespace(text_[offset_]))
            ++offset_;
    }

    bool consume(char expected)
    {
        if (offset_ >= text_.size() || text_[offset_] != expected)
            return false;
        ++offset_;
        return true;
    }

    bool fail(std::string message)
    {
        if (errors_.empty())
        {
            errors_.push_back({
                std::move(message),
                "$",
            });
        }
        return false;
    }

    std::string_view text_;
    ParseLimits limits_;
    std::size_t offset_ = 0;
    std::size_t valueCount_ = 0;
    std::size_t decodedStringBytes_ = 0;
    std::vector<Error> errors_;
    std::vector<std::size_t>
        boundedContainerElementCounts_;
    std::vector<std::size_t> boundedStringByteCounts_;
};

char lowercaseHex(unsigned int value)
{
    return static_cast<char>(value < 10U ? ('0' + value) : ('a' + (value - 10U)));
}
}

ParseResult parse(std::string_view text)
{
    return parse(text, {});
}

ParseResult parse(std::string_view text, const ParseLimits& limits)
{
    if (text.size() > limits.maximumInputBytes)
    {
        ParseResult result;
        result.errors.push_back({
            "JSON input byte limit exceeded",
            "$",
        });
        return result;
    }

    Parser parser(text, limits);
    return parser.parseDocument();
}

bool isValidUtf8(std::string_view value) noexcept
{
    return validUtf8Bytes(value);
}

const Value* findObjectMember(const Value& value, std::string_view key)
{
    if (!value.isObject())
        return nullptr;

    for (const auto& [memberKey, memberValue] : value.object())
    {
        if (memberKey == key)
            return &memberValue;
    }

    return nullptr;
}

void appendEscapedString(std::string& output, std::string_view value)
{
    output.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                output += "\\u00";
                output.push_back(lowercaseHex(character >> 4U));
                output.push_back(lowercaseHex(character & 0x0FU));
            }
            else
            {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}
}
