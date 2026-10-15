// Minimal JSON reader for the "@@ {json}" events of tools/launcher/launcher.py (--events) and the component index.
// Read-only: objects, arrays, strings (with \uXXXX and surrogate pairs), numbers, true/false/null.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace setup {

struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    static const JVal& Empty() {
        static const JVal v;
        return v;
    }
    const JVal& operator[](const char* k) const {
        if (t == Obj)
            for (auto& kv : o)
                if (kv.first == k) return kv.second;
        return Empty();
    }
    const JVal& operator[](size_t i) const { return t == Arr && i < a.size() ? a[i] : Empty(); }
    size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }
    bool exists() const { return t != Null; }
    std::string str(const std::string& def = "") const { return t == Str ? s : def; }
    double num(double def = 0) const { return t == Num ? n : def; }
    // true / false / null -> 1 / 0 / -1 (requirements use null = warning)
    int tri() const { return t == Bool ? (b ? 1 : 0) : -1; }
};

class JParser {
public:
    explicit JParser(const std::string& txt) : p_(txt.c_str()), e_(txt.c_str() + txt.size()) {}
    bool Read(JVal& v) {
        SkipWs();
        if (!Value(v)) return false;
        SkipWs();
        return p_ == e_;
    }

private:
    const char* p_;
    const char* e_;
    void SkipWs() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool Lit(const char* w) {
        const char* q = p_;
        for (; *w; ++w, ++q)
            if (q >= e_ || *q != *w) return false;
        p_ = q;
        return true;
    }
    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool Hex4(unsigned& v) {
        if (e_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_;
        while (p_ < e_ && *p_ != '"') {
            if (*p_ != '\\') { out += *p_++; continue; }
            if (++p_ >= e_) return false;
            const char c = *p_++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default: out += c; break;
            }
        }
        if (p_ >= e_) return false;
        ++p_;
        return true;
    }
    bool Value(JVal& v) {
        SkipWs();
        if (p_ >= e_) return false;
        const char c = *p_;
        if (c == '{') {
            v.t = JVal::Obj;
            ++p_;
            SkipWs();
            if (p_ < e_ && *p_ == '}') { ++p_; return true; }
            for (;;) {
                SkipWs();
                std::string k;
                if (!String(k)) return false;
                SkipWs();
                if (p_ >= e_ || *p_ != ':') return false;
                ++p_;
                JVal x;
                if (!Value(x)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                SkipWs();
                if (p_ < e_ && *p_ == ',') { ++p_; continue; }
                if (p_ < e_ && *p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = JVal::Arr;
            ++p_;
            SkipWs();
            if (p_ < e_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                JVal x;
                if (!Value(x)) return false;
                v.a.push_back(std::move(x));
                SkipWs();
                if (p_ < e_ && *p_ == ',') { ++p_; continue; }
                if (p_ < e_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::Str; return String(v.s); }
        if (Lit("true")) { v.t = JVal::Bool; v.b = true; return true; }
        if (Lit("false")) { v.t = JVal::Bool; v.b = false; return true; }
        if (Lit("null")) { v.t = JVal::Null; return true; }
        if (Lit("NaN") || Lit("Infinity") || Lit("-Infinity")) { v.t = JVal::Num; return true; }
        char* end = nullptr;
        v.n = std::strtod(p_, &end);
        if (end == p_) return false;
        v.t = JVal::Num;
        p_ = end;
        return true;
    }
};

inline bool ParseJson(const std::string& txt, JVal& v) { return JParser(txt).Read(v); }

}  // namespace setup
