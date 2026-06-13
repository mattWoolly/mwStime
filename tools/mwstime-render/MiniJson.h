// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// MiniJson — a minimal, dependency-free JSON reader for the mwstime-render
// CLI (plan/backlog/025-mwstime-render-cli.md: "a minimal vendored or
// hand-rolled JSON reader is fine — no heavyweight deps"). Reads the
// tests/golden/cases.json case matrix (schema documented in main.cpp; the
// corpus itself is task 026).
//
// Supported: RFC 8259 objects, arrays, strings (incl. \uXXXX escapes),
// numbers, true/false/null. Not supported (rejected): comments, trailing
// commas, NaN/Infinity. Errors are reported as a 1-based line/column message;
// no exceptions cross the API.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mws::tools::json {

/// One parsed JSON value. A plain tagged struct (no variant gymnastics —
/// this is a tool-layer helper, clarity over compactness).
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    /// Insertion-ordered key/value pairs (duplicate keys: first one wins
    /// in find()).
    std::vector<std::pair<std::string, Value>> object;

    [[nodiscard]] bool isNull() const noexcept { return type == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type == Type::Object; }

    /// Object member lookup; nullptr when absent or not an object.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
};

struct ParseResult {
    Value value;
    std::string error; ///< empty on success; "line L, col C: ..." otherwise.

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Parses a complete JSON document (one top-level value, trailing
/// whitespace only).
[[nodiscard]] ParseResult parse(std::string_view text);

} // namespace mws::tools::json
