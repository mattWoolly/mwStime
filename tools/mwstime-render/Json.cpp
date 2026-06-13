// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Minimal recursive-descent JSON reader (Json.h). Read-only and dependency-free.

#include "Json.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace mwsrender {

const JsonValue& JsonValue::at(const std::string& key) const
{
    static const JsonValue kNull;
    if (kind_ != Kind::Object)
        return kNull;
    const auto it = object_.find(key);
    return it == object_.end() ? kNull : it->second;
}

bool JsonValue::has(const std::string& key) const
{
    return kind_ == Kind::Object && object_.find(key) != object_.end();
}

JsonValue JsonValue::makeNull()
{
    return JsonValue{};
}

JsonValue JsonValue::makeBool(bool b)
{
    JsonValue v;
    v.kind_ = Kind::Bool;
    v.bool_ = b;
    return v;
}

JsonValue JsonValue::makeNumber(double n)
{
    JsonValue v;
    v.kind_ = Kind::Number;
    v.number_ = n;
    return v;
}

JsonValue JsonValue::makeString(std::string s)
{
    JsonValue v;
    v.kind_ = Kind::String;
    v.string_ = std::move(s);
    return v;
}

JsonValue JsonValue::makeArray(std::vector<JsonValue> a)
{
    JsonValue v;
    v.kind_ = Kind::Array;
    v.array_ = std::move(a);
    return v;
}

JsonValue JsonValue::makeObject(std::map<std::string, JsonValue> o)
{
    JsonValue v;
    v.kind_ = Kind::Object;
    v.object_ = std::move(o);
    return v;
}

namespace {

/// One-pass recursive-descent parser over a std::string. Tracks line/column so
/// the case file is debuggable from agent logs.
class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    JsonParseResult run()
    {
        skipWs();
        JsonValue value;
        if (!parseValue(value))
            return { {}, error_ };
        skipWs();
        if (pos_ != text_.size())
        {
            fail("trailing characters after JSON value");
            return { {}, error_ };
        }
        return { std::move(value), {} };
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;
    std::string error_;

    [[nodiscard]] bool atEnd() const { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return text_[pos_]; }

    char advance()
    {
        const char c = text_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; }
        else { ++col_; }
        return c;
    }

    void skipWs()
    {
        while (!atEnd())
        {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                advance();
            else
                break;
        }
    }

    bool fail(const std::string& message)
    {
        if (error_.empty())
            error_ = "line " + std::to_string(line_) + " col "
                     + std::to_string(col_) + ": " + message;
        return false;
    }

    bool parseValue(JsonValue& out)
    {
        skipWs();
        if (atEnd())
            return fail("unexpected end of input");
        switch (peek())
        {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': return parseString(out);
            case 't': case 'f': return parseBool(out);
            case 'n': return parseNull(out);
            default:  return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out)
    {
        advance(); // '{'
        std::map<std::string, JsonValue> members;
        skipWs();
        if (!atEnd() && peek() == '}') { advance(); out = JsonValue::makeObject(std::move(members)); return true; }
        for (;;)
        {
            skipWs();
            if (atEnd() || peek() != '"')
                return fail("expected string object key");
            JsonValue key;
            if (!parseString(key))
                return false;
            skipWs();
            if (atEnd() || peek() != ':')
                return fail("expected ':' after object key");
            advance();
            JsonValue value;
            if (!parseValue(value))
                return false;
            members[key.string()] = std::move(value);
            skipWs();
            if (atEnd())
                return fail("unterminated object");
            const char c = advance();
            if (c == '}')
                break;
            if (c != ',')
                return fail("expected ',' or '}' in object");
        }
        out = JsonValue::makeObject(std::move(members));
        return true;
    }

    bool parseArray(JsonValue& out)
    {
        advance(); // '['
        std::vector<JsonValue> items;
        skipWs();
        if (!atEnd() && peek() == ']') { advance(); out = JsonValue::makeArray(std::move(items)); return true; }
        for (;;)
        {
            JsonValue value;
            if (!parseValue(value))
                return false;
            items.push_back(std::move(value));
            skipWs();
            if (atEnd())
                return fail("unterminated array");
            const char c = advance();
            if (c == ']')
                break;
            if (c != ',')
                return fail("expected ',' or ']' in array");
        }
        out = JsonValue::makeArray(std::move(items));
        return true;
    }

    bool parseString(JsonValue& out)
    {
        advance(); // opening quote
        std::string s;
        for (;;)
        {
            if (atEnd())
                return fail("unterminated string");
            const char c = advance();
            if (c == '"')
                break;
            if (c == '\\')
            {
                if (atEnd())
                    return fail("unterminated escape");
                const char e = advance();
                switch (e)
                {
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/';  break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    case 'u':  return fail("\\u escapes are not supported");
                    default:   return fail("invalid escape sequence");
                }
            }
            else
            {
                s += c;
            }
        }
        out = JsonValue::makeString(std::move(s));
        return true;
    }

    bool parseBool(JsonValue& out)
    {
        if (match("true"))  { out = JsonValue::makeBool(true);  return true; }
        if (match("false")) { out = JsonValue::makeBool(false); return true; }
        return fail("invalid literal");
    }

    bool parseNull(JsonValue& out)
    {
        if (match("null")) { out = JsonValue::makeNull(); return true; }
        return fail("invalid literal");
    }

    bool match(const char* literal)
    {
        std::size_t i = 0;
        while (literal[i] != '\0')
        {
            if (pos_ + i >= text_.size() || text_[pos_ + i] != literal[i])
                return false;
            ++i;
        }
        for (std::size_t k = 0; k < i; ++k)
            advance();
        return true;
    }

    bool parseNumber(JsonValue& out)
    {
        const std::size_t start = pos_;
        if (!atEnd() && peek() == '-')
            advance();
        bool anyDigit = false;
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        {
            advance();
            anyDigit = true;
        }
        if (!atEnd() && peek() == '.')
        {
            advance();
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
            {
                advance();
                anyDigit = true;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-'))
                advance();
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
                advance();
        }
        if (!anyDigit)
            return fail("invalid number");
        const std::string token = text_.substr(start, pos_ - start);
        out = JsonValue::makeNumber(std::strtod(token.c_str(), nullptr));
        return true;
    }
};

} // namespace

JsonParseResult parseJson(const std::string& text)
{
    Parser parser(text);
    return parser.run();
}

} // namespace mwsrender
