// nnshim::cfg: minimal JSON reader for the mod.json "config" object.
//
// The mod runtime passes that object already serialized on one line (nlohmann::json::dump), so
// a small recursive reader is enough. Supports nested objects ("hid": {"map": {...}}),
// arrays, strings with escapes \" \\ \/ \n \t \uXXXX, numbers, true/false/null.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nnshim::cfg {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    // Looks up a dotted path ("hid.map.A"). nullptr if missing.
    const Value* Find(const char* path) const;

    bool IsObject() const { return type == Object; }
    std::string GetString(const char* path, const std::string& def = {}) const;
    bool GetBool(const char* path, bool def) const;
    double GetNumber(const char* path, double def) const;
    int64_t GetInt(const char* path, int64_t def) const;   // accepts "0x..." in strings
};

// Returns false (and leaves `out` null) if the text is not valid JSON.
bool Parse(const char* text, Value& out, std::string* error = nullptr);

} // namespace nnshim::cfg
