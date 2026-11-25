#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::project::json
{
struct Error
{
    std::string message;
    std::string path;
};

class Value
{
public:
    enum class Type
    {
        Null,
        Object,
        Array,
        String,
        Number,
        Boolean,
    };

    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    [[nodiscard]] static Value nullValue();
    [[nodiscard]] static Value objectValue(Object object);
    [[nodiscard]] static Value arrayValue(Array array);
    [[nodiscard]] static Value stringValue(std::string value);
    [[nodiscard]] static Value numberValue(double value, std::string token = {});
    [[nodiscard]] static Value booleanValue(bool value);

    [[nodiscard]] Type type() const;
    [[nodiscard]] bool isNull() const;
    [[nodiscard]] bool isObject() const;
    [[nodiscard]] bool isArray() const;
    [[nodiscard]] bool isString() const;
    [[nodiscard]] bool isNumber() const;
    [[nodiscard]] bool isBoolean() const;

    [[nodiscard]] const Object& object() const;
    [[nodiscard]] const Array& array() const;
    [[nodiscard]] const std::string& string() const;
    [[nodiscard]] double number() const;
    [[nodiscard]] const std::string& numberToken() const;
    [[nodiscard]] bool boolean() const;

private:
    Type type_ = Type::Null;
    Object object_;
    Array array_;
    std::string string_;
    double number_ = 0.0;
    std::string numberToken_;
    bool boolean_ = false;
};

struct ParseResult
{
    Value value;
    std::vector<Error> errors;

    [[nodiscard]] bool ok() const
    {
        return errors.empty();
    }
};

// Recursive-descent parsing consumes stack per nesting level; project-side
// loaders bound it so a crafted file cannot overflow the thread stack.
inline constexpr std::size_t defaultMaximumNestingDepth = 128;

struct ParseLimits
{
    struct BoundedStringMember
    {
        std::string_view name;
        std::size_t maximumBytes = 0;
        // Empty applies globally. Otherwise the member is bounded only while
        // parsing an element/value owned by this named container member.
        std::string_view containerName;
        std::string_view parentContainerName;
        // Members sharing the same non-max index consume one decoded-byte
        // budget while parsing. This is enforced before DOM retention.
        std::size_t aggregateBudgetIndex =
            std::numeric_limits<std::size_t>::max();
        std::size_t aggregateMaximumBytes =
            std::numeric_limits<std::size_t>::max();
        // Optional closed fields may be either null or a bounded string.
        bool allowNull = false;
    };

    struct BoundedContainerMember
    {
        std::string_view name;
        std::size_t maximumElements = 0;
        // When true, all containers with this member name share one budget.
        bool aggregate = false;
        std::string_view ownerContainerName;
        // Distinct named containers may share one aggregate budget. This is
        // useful for closed records whose total inspected work is bounded
        // across several differently named child arrays.
        std::size_t aggregateBudgetIndex =
            std::numeric_limits<std::size_t>::max();
        // The aggregate budget is independent from the per-container limit.
        // When omitted, it defaults to maximumElements for compatibility.
        std::size_t aggregateMaximumElements =
            std::numeric_limits<std::size_t>::max();
    };

    // Bounds an entire named member subtree without imposing new global
    // limits on historical schemas. This is used by newly introduced closed
    // records so unknown members or wrong-shaped values cannot construct an
    // unbounded DOM before semantic closedness is checked.
    struct BoundedSubtreeMember
    {
        std::string_view name;
        std::string_view ownerContainerName;
        std::size_t maximumValues = 0;
        std::size_t maximumNestingDepth = 0;
        std::size_t maximumDecodedStringBytes = 0;
        std::size_t maximumObjectKeyBytes = 0;
        std::size_t maximumScalarTokenBytes = 0;
    };

    std::size_t maximumInputBytes =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximumNestingDepth =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximumValues =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximumDecodedStringBytes =
        std::numeric_limits<std::size_t>::max();
    std::size_t maximumContainerElements =
        std::numeric_limits<std::size_t>::max();
    // Optional exact object-member string bounds. The parser enforces each
    // while decoding the member value, before allocating or traversing the
    // remainder of an oversized string.
    std::span<const BoundedStringMember> boundedStringMembers;
    // Optional exact object-member container bounds. These are enforced while
    // parsing the named array or object, before retaining an extra element.
    std::span<const BoundedContainerMember> boundedContainerMembers;
    std::span<const BoundedSubtreeMember> boundedSubtreeMembers;
    bool requireValidUtf8 = false;
};

[[nodiscard]] ParseResult parse(std::string_view text);
[[nodiscard]] ParseResult parse(
    std::string_view text,
    const ParseLimits& limits);
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;
[[nodiscard]] const Value* findObjectMember(const Value& value, std::string_view key);
void appendEscapedString(std::string& output, std::string_view value);
}
