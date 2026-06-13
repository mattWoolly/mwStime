// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// MiniJson — recursive-descent JSON reader (see MiniJson.h for the contract).

#include "MiniJson.h"

#include <cstdlib>
#include <string>

namespace mws::tools::json {

const Value* Value::find(std::string_view key) const noexcept
{
    if (type != Type::Object)
        return nullptr;
    for (const auto& [k, v] : object)
        if (k == key)
            return &v;
    return nullptr;
}

namespace {

/// Guards against adversarial deeply-nested documents (the cases file is
/// two levels deep; 64 is generous).
constexpr int kMaxDepth = 64;

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult run()
    {
        ParseResult result;
        skipWs();
        if (!parseValue(result.value, 0))
        {
            result.error = error_;
            return result;
        }
        skipWs();
        if (pos_ != text_.size())
            result.error = errorAt("trailing characters after the document");
        return result;
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return text_[pos_]; }

    void skipWs() noexcept
    {
        while (!atEnd())
        {
            const char c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            advance();
        }
    }

    void advance() noexcept
    {
        if (text_[pos_] == '\n')
        {
            ++line_;
            col_ = 1;
        }
        else
        {
            ++col_;
        }
        ++pos_;
    }

    [[nodiscard]] std::string errorAt(const std::string& what) const
    {
        return "line " + std::to_string(line_) + ", col "
               + std::to_string(col_) + ": " + what;
    }

    bool fail(const std::string& what)
    {
        if (error_.empty())
            error_ = errorAt(what);
        return false;
    }

    bool consumeLiteral(std::string_view literal, const char* what)
    {
        for (const char expected : literal)
        {
            if (atEnd() || peek() != expected)
                return fail(std::string("invalid ") + what);
            advance();
        }
        return true;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("document nested too deeply");
        if (atEnd())
            return fail("unexpected end of document");

        switch (peek())
        {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': out.type = Value::Type::String;
                      return parseString(out.string);
            case 't': out.type = Value::Type::Bool;
                      out.boolean = true;
                      return consumeLiteral("true", "literal");
            case 'f': out.type = Value::Type::Bool;
                      out.boolean = false;
                      return consumeLiteral("false", "literal");
            case 'n': out.type = Value::Type::Null;
                      return consumeLiteral("null", "literal");
            default:  return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth)
    {
        out.type = Value::Type::Object;
        advance(); // '{'
        skipWs();
        if (!atEnd() && peek() == '}')
        {
            advance();
            return true;
        }
        while (true)
        {
            skipWs();
            if (atEnd() || peek() != '"')
                return fail("expected a string object key");
            std::string key;
            if (!parseString(key))
                return false;
            skipWs();
            if (atEnd() || peek() != ':')
                return fail("expected ':' after object key");
            advance();
            skipWs();
            Value member;
            if (!parseValue(member, depth + 1))
                return false;
            out.object.emplace_back(std::move(key), std::move(member));
            skipWs();
            if (atEnd())
                return fail("unterminated object");
            if (peek() == ',')
            {
                advance();
                continue;
            }
            if (peek() == '}')
            {
                advance();
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value& out, int depth)
    {
        out.type = Value::Type::Array;
        advance(); // '['
        skipWs();
        if (!atEnd() && peek() == ']')
        {
            advance();
            return true;
        }
        while (true)
        {
            skipWs();
            Value element;
            if (!parseValue(element, depth + 1))
                return false;
            out.array.push_back(std::move(element));
            skipWs();
            if (atEnd())
                return fail("unterminated array");
            if (peek() == ',')
            {
                advance();
                continue;
            }
            if (peek() == ']')
            {
                advance();
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    static void appendUtf8(std::string& out, unsigned codepoint)
    {
        if (codepoint <= 0x7F)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parseHex4(unsigned& out)
    {
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (atEnd())
                return fail("truncated \\u escape");
            const char c = peek();
            unsigned digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A' + 10);
            else
                return fail("invalid \\u escape digit");
            out = (out << 4) | digit;
            advance();
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        advance(); // opening '"'
        out.clear();
        while (true)
        {
            if (atEnd())
                return fail("unterminated string");
            const char c = peek();
            if (c == '"')
            {
                advance();
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("unescaped control character in string");
            if (c != '\\')
            {
                out.push_back(c);
                advance();
                continue;
            }
            advance(); // '\'
            if (atEnd())
                return fail("truncated escape sequence");
            const char esc = peek();
            switch (esc)
            {
                case '"':  out.push_back('"');  advance(); break;
                case '\\': out.push_back('\\'); advance(); break;
                case '/':  out.push_back('/');  advance(); break;
                case 'b':  out.push_back('\b'); advance(); break;
                case 'f':  out.push_back('\f'); advance(); break;
                case 'n':  out.push_back('\n'); advance(); break;
                case 'r':  out.push_back('\r'); advance(); break;
                case 't':  out.push_back('\t'); advance(); break;
                case 'u':
                {
                    advance();
                    unsigned codepoint = 0;
                    if (!parseHex4(codepoint))
                        return false;
                    // Surrogate pair handling (RFC 8259 §7).
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                    {
                        if (atEnd() || peek() != '\\')
                            return fail("unpaired UTF-16 surrogate");
                        advance();
                        if (atEnd() || peek() != 'u')
                            return fail("unpaired UTF-16 surrogate");
                        advance();
                        unsigned low = 0;
                        if (!parseHex4(low))
                            return false;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail("invalid UTF-16 low surrogate");
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10)
                                    + (low - 0xDC00);
                    }
                    else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                    {
                        return fail("unpaired UTF-16 surrogate");
                    }
                    appendUtf8(out, codepoint);
                    break;
                }
                default:
                    return fail("invalid escape sequence");
            }
        }
    }

    bool parseNumber(Value& out)
    {
        const std::size_t start = pos_;
        if (!atEnd() && peek() == '-')
            advance();
        // Integer part: one or more digits; no leading zeros (RFC 8259 §6).
        if (atEnd() || peek() < '0' || peek() > '9')
            return fail("invalid number");
        if (peek() == '0')
        {
            advance();
        }
        else
        {
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                advance();
        }
        if (!atEnd() && peek() == '.')
        {
            advance();
            if (atEnd() || peek() < '0' || peek() > '9')
                return fail("invalid number fraction");
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                advance();
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-'))
                advance();
            if (atEnd() || peek() < '0' || peek() > '9')
                return fail("invalid number exponent");
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                advance();
        }
        const std::string token(text_.substr(start, pos_ - start));
        out.type = Value::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;
    std::string error_;
};

} // namespace

ParseResult parse(std::string_view text)
{
    return Parser(text).run();
}

} // namespace mws::tools::json
