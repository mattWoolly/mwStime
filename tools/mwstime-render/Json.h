// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// A minimal, hand-rolled JSON reader for tests/golden/cases.json
// (docs/design/testing-strategy.md §4; task plan/backlog/025-mwstime-render-cli.md
// scope: "a minimal vendored or hand-rolled JSON reader is fine — no heavyweight
// deps"). Read-only: parses the subset JSON the case file needs — objects,
// arrays, strings, numbers, true/false/null. No JUCE, no third-party code.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mwsrender {

/// A parsed JSON value. Objects preserve no key order (cases are looked up by
/// id, never iterated for output). Numbers are kept as double; bools and the
/// null literal are distinct kinds so callers can distinguish them.
class JsonValue
{
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool isObject() const noexcept { return kind_ == Kind::Object; }
    [[nodiscard]] bool isArray() const noexcept { return kind_ == Kind::Array; }
    [[nodiscard]] bool isString() const noexcept { return kind_ == Kind::String; }
    [[nodiscard]] bool isNumber() const noexcept { return kind_ == Kind::Number; }
    [[nodiscard]] bool isBool() const noexcept { return kind_ == Kind::Bool; }
    [[nodiscard]] bool isNull() const noexcept { return kind_ == Kind::Null; }

    [[nodiscard]] double number() const noexcept { return number_; }
    [[nodiscard]] bool boolean() const noexcept { return bool_; }
    [[nodiscard]] const std::string& string() const noexcept { return string_; }
    [[nodiscard]] const std::vector<JsonValue>& array() const noexcept { return array_; }
    [[nodiscard]] const std::map<std::string, JsonValue>& object() const noexcept
    {
        return object_;
    }

    /// True iff this is an object that has `key`.
    [[nodiscard]] bool has(const std::string& key) const;

    /// Member lookup; returns a static Null value when absent or not an object.
    [[nodiscard]] const JsonValue& at(const std::string& key) const;

    // Builders (used only by the parser).
    static JsonValue makeNull();
    static JsonValue makeBool(bool b);
    static JsonValue makeNumber(double n);
    static JsonValue makeString(std::string s);
    static JsonValue makeArray(std::vector<JsonValue> a);
    static JsonValue makeObject(std::map<std::string, JsonValue> o);

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;
};

/// Result of a parse: `error` is empty on success.
struct JsonParseResult {
    JsonValue value;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Parses a complete JSON document from `text`. On failure `error` carries a
/// human-readable message with a 1-based line/column; on success it is empty.
[[nodiscard]] JsonParseResult parseJson(const std::string& text);

} // namespace mwsrender
