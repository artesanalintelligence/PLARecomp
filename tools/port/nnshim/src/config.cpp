// nnshim::cfg: minimal JSON reader (see include/nnshim/config.hpp).
#include "nnshim/config.hpp"

#include <cstdlib>
#include <cstring>

namespace nnshim::cfg {
namespace {

struct Reader {
    const char* p;
    std::string err;

    // Whitespace and "// ..." line comments (hand-written files, e.g. controls.json).
    void Ws() {
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
            if (p[0] != '/' || p[1] != '/') break;
            while (*p && *p != '\n') ++p;
        }
    }
    bool Fail(const char* what) {
        if (err.empty()) err = what;
        return false;
    }
    static void Utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            const char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("invalid \\u");
        }
        return true;
    }
    bool Str(std::string& out) {
        if (*p != '"') return Fail("expected a string");
        ++p;
        out.clear();
        while (*p && *p != '"') {
            if (*p != '\\') { out.push_back(*p++); continue; }
            ++p;
            switch (*p) {
            case '"': out.push_back('"'); ++p; break;
            case '\\': out.push_back('\\'); ++p; break;
            case '/': out.push_back('/'); ++p; break;
            case 'b': out.push_back('\b'); ++p; break;
            case 'f': out.push_back('\f'); ++p; break;
            case 'n': out.push_back('\n'); ++p; break;
            case 'r': out.push_back('\r'); ++p; break;
            case 't': out.push_back('\t'); ++p; break;
            case 'u': {
                ++p;
                uint32_t cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default: return Fail("invalid escape");
            }
        }
        if (*p != '"') return Fail("unterminated string");
        ++p;
        return true;
    }
    bool Val(Value& v, int depth) {
        if (depth > 64) return Fail("nested too deeply");
        Ws();
        if (*p == '{') {
            ++p;
            v.type = Value::Object;
            Ws();
            if (*p == '}') { ++p; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (!Str(key)) return false;
                Ws();
                if (*p != ':') return Fail("expected ':'");
                ++p;
                Value child;
                if (!Val(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                Ws();
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (*p == '[') {
            ++p;
            v.type = Value::Array;
            Ws();
            if (*p == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!Val(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (*p == '"') {
            v.type = Value::String;
            return Str(v.str);
        }
        if (!std::strncmp(p, "true", 4)) { p += 4; v.type = Value::Bool; v.b = true; return true; }
        if (!std::strncmp(p, "false", 5)) { p += 5; v.type = Value::Bool; v.b = false; return true; }
        if (!std::strncmp(p, "null", 4)) { p += 4; v.type = Value::Null; return true; }
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) return Fail("invalid value");
        p = end;
        v.type = Value::Number;
        v.num = d;
        return true;
    }
};

} // namespace

bool Parse(const char* text, Value& out, std::string* error) {
    out = Value{};
    if (!text) return true;
    Reader r{text, {}};
    Value v;
    if (!r.Val(v, 0)) {
        if (error) *error = r.err;
        return false;
    }
    out = std::move(v);
    return true;
}

const Value* Value::Find(const char* path) const {
    const Value* cur = this;
    std::string key;
    const char* s = path;
    while (cur) {
        const char* dot = std::strchr(s, '.');
        key.assign(s, dot ? static_cast<size_t>(dot - s) : std::strlen(s));
        if (cur->type != Object) return nullptr;
        const Value* next = nullptr;
        for (const auto& [k, v] : cur->obj)
            if (k == key) { next = &v; break; }
        cur = next;
        if (!dot) break;
        s = dot + 1;
    }
    return cur;
}

std::string Value::GetString(const char* path, const std::string& def) const {
    const Value* v = Find(path);
    return (v && v->type == String) ? v->str : def;
}

bool Value::GetBool(const char* path, bool def) const {
    const Value* v = Find(path);
    if (!v) return def;
    if (v->type == Bool) return v->b;
    if (v->type == Number) return v->num != 0;
    if (v->type == String) return v->str == "true" || v->str == "1" || v->str == "on";
    return def;
}

double Value::GetNumber(const char* path, double def) const {
    const Value* v = Find(path);
    if (!v) return def;
    if (v->type == Number) return v->num;
    if (v->type == String && !v->str.empty()) return std::strtod(v->str.c_str(), nullptr);
    return def;
}

int64_t Value::GetInt(const char* path, int64_t def) const {
    const Value* v = Find(path);
    if (!v) return def;
    if (v->type == Number) return static_cast<int64_t>(v->num);
    if (v->type == String && !v->str.empty()) return static_cast<int64_t>(std::strtoull(v->str.c_str(), nullptr, 0));
    if (v->type == Bool) return v->b ? 1 : 0;
    return def;
}

} // namespace nnshim::cfg
