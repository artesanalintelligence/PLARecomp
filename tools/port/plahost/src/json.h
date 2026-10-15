// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small JSON reader/writer for mod.json (objects keep their key order; numbers keep their
// text so a dump reproduces them).

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plahost::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    std::string text; // String: the decoded string; Number: the literal
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    bool is_string() const { return type == String; }
    const Value* find(const std::string& key) const;
    Value* find(const std::string& key);
    Value& set(const std::string& key, Value v); // replaces or appends
    std::string str(const std::string& key, const std::string& fallback = {}) const;
    bool boolean(const std::string& key, bool fallback) const;
    std::string dump() const;
};

/// Parses `text`; false (and `error`) on malformed input.
bool Parse(const std::string& text, Value& out, std::string* error = nullptr);
/// A string value, or a literal if `text` parses as JSON (true/false/number/object...).
Value FromLiteralOrString(const std::string& text);
Value MakeString(std::string s);

} // namespace plahost::json
