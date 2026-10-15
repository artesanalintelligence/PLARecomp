// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace plahost::json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
            ++i;
        }
    }
    bool Fail(const char* what) {
        if (err.empty()) {
            char b[96];
            std::snprintf(b, sizeof b, "%s at offset %zu", what, i);
            err = b;
        }
        return false;
    }
    static void PutUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(unsigned& v) {
        if (i + 4 > s.size()) {
            return Fail("bad \\u escape");
        }
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return Fail("bad \\u escape");
        }
        return true;
    }
    bool Str(std::string& out) {
        if (s[i] != '"') {
            return Fail("expected string");
        }
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) {
                break;
            }
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                PutUtf8(out, cp);
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Val(Value& v, int depth) {
        if (depth > 64) {
            return Fail("too deep");
        }
        Ws();
        if (i >= s.size()) {
            return Fail("unexpected end");
        }
        const char c = s[i];
        if (c == '{') {
            v.type = Value::Object;
            ++i;
            Ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            while (true) {
                Ws();
                std::string key;
                if (i >= s.size() || !Str(key)) return Fail("expected key");
                Ws();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                Value child;
                if (!Val(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++i;
            Ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            while (true) {
                Value child;
                if (!Val(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::String;
            return Str(v.text);
        }
        if (s.compare(i, 4, "true") == 0) { v.type = Value::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.type = Value::Bool; v.b = false; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v.type = Value::Null; i += 4; return true; }
        const size_t start = i;
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-')) {
            return Fail("unexpected character");
        }
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
            ++i;
        }
        if (i == start) {
            return Fail("unexpected character");
        }
        v.type = Value::Number;
        v.text = s.substr(start, i - start);
        return true;
    }
};

void Escape(const std::string& in, std::string& out) {
    out += '"';
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void Dump(const Value& v, std::string& out) {
    switch (v.type) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.b ? "true" : "false"; break;
    case Value::Number: out += v.text; break;
    case Value::String: Escape(v.text, out); break;
    case Value::Array:
        out += '[';
        for (size_t k = 0; k < v.arr.size(); ++k) {
            if (k) out += ',';
            Dump(v.arr[k], out);
        }
        out += ']';
        break;
    case Value::Object:
        out += '{';
        for (size_t k = 0; k < v.obj.size(); ++k) {
            if (k) out += ',';
            Escape(v.obj[k].first, out);
            out += ':';
            Dump(v.obj[k].second, out);
        }
        out += '}';
        break;
    }
}

} // namespace

const Value* Value::find(const std::string& key) const {
    for (const auto& [k, v] : obj) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value* Value::find(const std::string& key) {
    for (auto& [k, v] : obj) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value& Value::set(const std::string& key, Value v) {
    type = Object;
    if (Value* old = find(key)) {
        *old = std::move(v);
        return *old;
    }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}

std::string Value::str(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    return v && v->type == String ? v->text : fallback;
}

bool Value::boolean(const std::string& key, bool fallback) const {
    const Value* v = find(key);
    return v && v->type == Bool ? v->b : fallback;
}

std::string Value::dump() const {
    std::string out;
    Dump(*this, out);
    return out;
}

bool Parse(const std::string& text, Value& out, std::string* error) {
    Parser p{text};
    // UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        p.i = 3;
    }
    out = Value{};
    bool ok = p.Val(out, 0);
    if (ok) {
        p.Ws();
        if (p.i != text.size()) {
            ok = p.Fail("trailing characters");
        }
    }
    if (!ok && error) {
        *error = p.err;
    }
    return ok;
}

Value MakeString(std::string s) {
    Value v;
    v.type = Value::String;
    v.text = std::move(s);
    return v;
}

Value FromLiteralOrString(const std::string& text) {
    Value v;
    if (Parse(text, v) && v.type != Value::String) {
        return v;
    }
    return MakeString(text);
}

} // namespace plahost::json
