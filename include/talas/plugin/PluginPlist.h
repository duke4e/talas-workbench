#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ADR-0029: a small XML property list model, enough to rewrite the
// Info.plist of a template bundle. Portable C++ so it is tested everywhere,
// though only macOS bundles carry an Info.plist.
namespace talas::plugin::plist
{
class Value
{
public:
    enum class Type
    {
        String,
        Integer,
        Real,
        Boolean,
        Date,
        Data,
        Array,
        Dictionary,
    };

    using Array = std::vector<Value>;
    using Dictionary = std::vector<std::pair<std::string, Value>>;

    [[nodiscard]] static Value string(std::string text);
    [[nodiscard]] static Value integer(std::int64_t number);
    [[nodiscard]] static Value boolean(bool flag);
    [[nodiscard]] static Value array(Array items = {});
    [[nodiscard]] static Value dictionary(Dictionary entries = {});
    // Real, date and data values keep their text as written.
    [[nodiscard]] static Value scalar(Type type, std::string text);

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool booleanValue() const noexcept { return boolean_; }
    [[nodiscard]] std::optional<std::int64_t> integerValue() const;
    [[nodiscard]] Array& items() noexcept { return array_; }
    [[nodiscard]] const Array& items() const noexcept { return array_; }
    [[nodiscard]] Dictionary& entries() noexcept { return dictionary_; }
    [[nodiscard]] const Dictionary& entries() const noexcept { return dictionary_; }

    // Dictionary access; nullptr when absent or this is not a dictionary.
    [[nodiscard]] Value* find(std::string_view key);
    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] const std::string* findString(std::string_view key) const;
    // Replaces the value under key, or appends it.
    void set(std::string_view key, Value value);

    bool operator==(const Value&) const = default;

private:
    Type type_ = Type::String;
    std::string text_;
    bool boolean_ = false;
    Array array_;
    Dictionary dictionary_;
};

struct ParseResult
{
    std::optional<Value> root;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return root.has_value(); }
};

[[nodiscard]] ParseResult parse(std::string_view xml);
// An XML property list as Apple's tools write it, tab indented.
[[nodiscard]] std::string write(const Value& root);
}
