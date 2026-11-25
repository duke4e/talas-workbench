#include "talas/plugin/PluginPlist.h"

#include <charconv>
#include <cstddef>

namespace talas::plugin::plist
{
Value Value::string(std::string text)
{
    Value value;
    value.type_ = Type::String;
    value.text_ = std::move(text);
    return value;
}

Value Value::integer(std::int64_t number)
{
    Value value;
    value.type_ = Type::Integer;
    value.text_ = std::to_string(number);
    return value;
}

Value Value::boolean(bool flag)
{
    Value value;
    value.type_ = Type::Boolean;
    value.boolean_ = flag;
    return value;
}

Value Value::array(Array items)
{
    Value value;
    value.type_ = Type::Array;
    value.array_ = std::move(items);
    return value;
}

Value Value::dictionary(Dictionary entries)
{
    Value value;
    value.type_ = Type::Dictionary;
    value.dictionary_ = std::move(entries);
    return value;
}

Value Value::scalar(Type type, std::string text)
{
    Value value;
    value.type_ = type;
    value.text_ = std::move(text);
    return value;
}

std::optional<std::int64_t> Value::integerValue() const
{
    if (type_ != Type::Integer)
        return std::nullopt;
    std::int64_t number = 0;
    const auto* begin = text_.data();
    const auto* end = text_.data() + text_.size();
    const auto [next, status] = std::from_chars(begin, end, number);
    if (status != std::errc{} || next != end)
        return std::nullopt;
    return number;
}

Value* Value::find(std::string_view key)
{
    if (type_ != Type::Dictionary)
        return nullptr;
    for (auto& [entryKey, entryValue] : dictionary_)
    {
        if (entryKey == key)
            return &entryValue;
    }
    return nullptr;
}

const Value* Value::find(std::string_view key) const
{
    return const_cast<Value*>(this)->find(key);
}

const std::string* Value::findString(std::string_view key) const
{
    const auto* value = find(key);
    return value != nullptr && value->type() == Type::String ? &value->text() : nullptr;
}

void Value::set(std::string_view key, Value value)
{
    if (type_ != Type::Dictionary)
        return;
    if (auto* existing = find(key))
    {
        *existing = std::move(value);
        return;
    }
    dictionary_.emplace_back(std::string{key}, std::move(value));
}

namespace
{
constexpr std::size_t maximumDepth = 64;

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult run()
    {
        ParseResult result;
        if (text_.starts_with("bplist"))
        {
            result.error = "binary property lists are not supported";
            return result;
        }
        skipMisc();
        Tag tag;
        if (!readTag(tag) || tag.name != "plist" || tag.closing)
            return fail("expected <plist>");
        if (tag.selfClosing)
            return fail("<plist> is empty");
        skipMisc();
        auto root = parseValue(0);
        if (!root.has_value())
            return fail(error_.empty() ? "expected a value inside <plist>" : error_);
        skipMisc();
        if (!readTag(tag) || tag.name != "plist" || !tag.closing)
            return fail("expected </plist>");
        result.root = std::move(root);
        return result;
    }

private:
    struct Tag
    {
        std::string name;
        bool closing = false;
        bool selfClosing = false;
    };

    ParseResult fail(std::string message)
    {
        ParseResult result;
        result.error = std::move(message) + " at byte " + std::to_string(position_);
        return result;
    }

    [[nodiscard]] bool atEnd() const noexcept { return position_ >= text_.size(); }

    void skipWhitespace()
    {
        while (!atEnd()
            && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n'
                || text_[position_] == '\r'))
        {
            ++position_;
        }
    }

    // Whitespace, the XML declaration, a DOCTYPE and comments.
    void skipMisc()
    {
        while (true)
        {
            skipWhitespace();
            const auto rest = text_.substr(position_);
            std::string_view terminator;
            if (rest.starts_with("<?"))
                terminator = "?>";
            else if (rest.starts_with("<!--"))
                terminator = "-->";
            else if (rest.starts_with("<!"))
                terminator = ">";
            else
                return;
            const auto end = text_.find(terminator, position_ + 2);
            position_ = end == std::string_view::npos ? text_.size() : end + terminator.size();
        }
    }

    bool readTag(Tag& tag)
    {
        tag = {};
        if (atEnd() || text_[position_] != '<')
            return false;
        ++position_;
        if (!atEnd() && text_[position_] == '/')
        {
            tag.closing = true;
            ++position_;
        }
        const auto nameStart = position_;
        while (!atEnd() && text_[position_] != '>' && text_[position_] != '/' && text_[position_] != ' '
            && text_[position_] != '\t' && text_[position_] != '\n' && text_[position_] != '\r')
        {
            ++position_;
        }
        tag.name = std::string{text_.substr(nameStart, position_ - nameStart)};
        // Attributes (plist version="1.0") are skipped.
        char quote = 0;
        while (!atEnd())
        {
            const auto character = text_[position_];
            if (quote != 0)
            {
                if (character == quote)
                    quote = 0;
            }
            else if (character == '"' || character == '\'')
            {
                quote = character;
            }
            else if (character == '>')
            {
                break;
            }
            ++position_;
        }
        if (atEnd())
            return false;
        tag.selfClosing = position_ > 0 && text_[position_ - 1] == '/';
        ++position_;
        return !tag.name.empty();
    }

    std::optional<std::string> readText(std::string_view element)
    {
        std::string decoded;
        while (!atEnd() && text_[position_] != '<')
        {
            const auto character = text_[position_];
            if (character != '&')
            {
                decoded.push_back(character);
                ++position_;
                continue;
            }
            const auto end = text_.find(';', position_);
            if (end == std::string_view::npos)
            {
                error_ = "unterminated entity";
                return std::nullopt;
            }
            const auto entity = text_.substr(position_ + 1, end - position_ - 1);
            position_ = end + 1;
            if (entity == "amp")
                decoded.push_back('&');
            else if (entity == "lt")
                decoded.push_back('<');
            else if (entity == "gt")
                decoded.push_back('>');
            else if (entity == "quot")
                decoded.push_back('"');
            else if (entity == "apos")
                decoded.push_back('\'');
            else if (entity.starts_with("#"))
            {
                std::uint32_t codePoint = 0;
                const auto hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                const auto digits = entity.substr(hex ? 2 : 1);
                const auto [next, status] = std::from_chars(
                    digits.data(), digits.data() + digits.size(), codePoint, hex ? 16 : 10);
                if (status != std::errc{} || next != digits.data() + digits.size() || codePoint > 0x10FFFFU)
                {
                    error_ = "bad character reference";
                    return std::nullopt;
                }
                appendUtf8(decoded, codePoint);
            }
            else
            {
                error_ = "unknown entity &" + std::string{entity} + ";";
                return std::nullopt;
            }
        }
        Tag closing;
        if (!readTag(closing) || !closing.closing || closing.name != element)
        {
            error_ = "expected </" + std::string{element} + ">";
            return std::nullopt;
        }
        return decoded;
    }

    static void appendUtf8(std::string& out, std::uint32_t codePoint)
    {
        if (codePoint < 0x80U)
        {
            out.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800U)
        {
            out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else if (codePoint < 0x10000U)
        {
            out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    std::optional<Value> parseValue(std::size_t depth)
    {
        if (depth > maximumDepth)
        {
            error_ = "property list nests too deeply";
            return std::nullopt;
        }
        skipMisc();
        Tag tag;
        const auto tagStart = position_;
        if (!readTag(tag) || tag.closing)
        {
            position_ = tagStart;
            return std::nullopt;
        }
        const auto scalar = [&](Value::Type type) -> std::optional<Value> {
            if (tag.selfClosing)
                return Value::scalar(type, {});
            auto text = readText(tag.name);
            if (!text.has_value())
                return std::nullopt;
            return Value::scalar(type, std::move(*text));
        };
        if (tag.name == "string")
            return scalar(Value::Type::String);
        if (tag.name == "integer")
            return scalar(Value::Type::Integer);
        if (tag.name == "real")
            return scalar(Value::Type::Real);
        if (tag.name == "date")
            return scalar(Value::Type::Date);
        if (tag.name == "data")
            return scalar(Value::Type::Data);
        if (tag.name == "true" || tag.name == "false")
        {
            if (!tag.selfClosing)
            {
                Tag closing;
                skipWhitespace();
                if (!readTag(closing) || !closing.closing || closing.name != tag.name)
                {
                    error_ = "expected </" + tag.name + ">";
                    return std::nullopt;
                }
            }
            return Value::boolean(tag.name == "true");
        }
        if (tag.name == "array")
        {
            auto array = Value::array();
            if (tag.selfClosing)
                return array;
            while (true)
            {
                skipMisc();
                if (text_.substr(position_).starts_with("</"))
                {
                    Tag closing;
                    if (!readTag(closing) || closing.name != "array")
                    {
                        error_ = "expected </array>";
                        return std::nullopt;
                    }
                    return array;
                }
                auto item = parseValue(depth + 1);
                if (!item.has_value())
                {
                    if (error_.empty())
                        error_ = "expected an array item";
                    return std::nullopt;
                }
                array.items().push_back(std::move(*item));
            }
        }
        if (tag.name == "dict")
        {
            auto dictionary = Value::dictionary();
            if (tag.selfClosing)
                return dictionary;
            while (true)
            {
                skipMisc();
                Tag keyTag;
                if (!readTag(keyTag))
                {
                    error_ = "expected <key> or </dict>";
                    return std::nullopt;
                }
                if (keyTag.closing)
                {
                    if (keyTag.name != "dict")
                    {
                        error_ = "expected </dict>";
                        return std::nullopt;
                    }
                    return dictionary;
                }
                if (keyTag.name != "key")
                {
                    error_ = "expected <key>, found <" + keyTag.name + ">";
                    return std::nullopt;
                }
                std::string key;
                if (!keyTag.selfClosing)
                {
                    auto text = readText("key");
                    if (!text.has_value())
                        return std::nullopt;
                    key = std::move(*text);
                }
                auto value = parseValue(depth + 1);
                if (!value.has_value())
                {
                    if (error_.empty())
                        error_ = "expected a value for key '" + key + "'";
                    return std::nullopt;
                }
                dictionary.entries().emplace_back(std::move(key), std::move(*value));
            }
        }
        error_ = "unknown element <" + tag.name + ">";
        return std::nullopt;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string error_;
};

void appendEscaped(std::string& out, std::string_view text)
{
    for (const auto character : text)
    {
        switch (character)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out.push_back(character);
            break;
        }
    }
}

void writeValue(std::string& out, const Value& value, std::size_t depth)
{
    const std::string indent(depth, '\t');
    const auto element = [&](std::string_view name, std::string_view text) {
        out += indent;
        out += '<';
        out += name;
        out += '>';
        appendEscaped(out, text);
        out += "</";
        out += name;
        out += ">\n";
    };
    switch (value.type())
    {
    case Value::Type::String:
        element("string", value.text());
        break;
    case Value::Type::Integer:
        element("integer", value.text());
        break;
    case Value::Type::Real:
        element("real", value.text());
        break;
    case Value::Type::Date:
        element("date", value.text());
        break;
    case Value::Type::Data:
        element("data", value.text());
        break;
    case Value::Type::Boolean:
        out += indent + (value.booleanValue() ? "<true/>\n" : "<false/>\n");
        break;
    case Value::Type::Array:
        if (value.items().empty())
        {
            out += indent + "<array/>\n";
            break;
        }
        out += indent + "<array>\n";
        for (const auto& item : value.items())
            writeValue(out, item, depth + 1);
        out += indent + "</array>\n";
        break;
    case Value::Type::Dictionary:
        if (value.entries().empty())
        {
            out += indent + "<dict/>\n";
            break;
        }
        out += indent + "<dict>\n";
        for (const auto& [key, entry] : value.entries())
        {
            out += indent + "\t<key>";
            appendEscaped(out, key);
            out += "</key>\n";
            writeValue(out, entry, depth + 1);
        }
        out += indent + "</dict>\n";
        break;
    }
}
}

ParseResult parse(std::string_view xml)
{
    return Parser{xml}.run();
}

std::string write(const Value& root)
{
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n";
    writeValue(out, root, 0);
    out += "</plist>\n";
    return out;
}
}
