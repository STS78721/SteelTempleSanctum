#include "json.h"

#include <cstdlib>

namespace sanctum {

static const Json kNull;

const Json &Json::operator[](const char *k) const {
    if (type != OBJ) return kNull;
    auto it = obj.find(k);
    return it == obj.end() ? kNull : it->second;
}
const Json &Json::operator[](size_t i) const { return type == ARR && i < arr.size() ? arr[i] : kNull; }

namespace {
struct P {
    const char *c, *end;
    int depth = 0;
    void ws() { while (c < end && (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t')) ++c; }
    bool lit(const char *w) {
        const char *p = c;
        for (; *w; ++w, ++p) if (p >= end || *p != *w) return false;
        c = p;
        return true;
    }
    static void utf8(std::string &o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) o += (char)(0xC0 | cp >> 6), o += (char)(0x80 | (cp & 63));
        else if (cp < 0x10000) o += (char)(0xE0 | cp >> 12), o += (char)(0x80 | ((cp >> 6) & 63)), o += (char)(0x80 | (cp & 63));
        else o += (char)(0xF0 | cp >> 18), o += (char)(0x80 | ((cp >> 12) & 63)), o += (char)(0x80 | ((cp >> 6) & 63)), o += (char)(0x80 | (cp & 63));
    }
    bool hex4(unsigned &v) {
        if (end - c < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i, ++c) {
            char h = *c;
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (c >= end || *c != '"') return false;
        ++c;
        while (c < end && *c != '"') {
            if (*c != '\\') { o += *c++; continue; }
            if (++c >= end) return false;
            char e = *c++;
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned v;
                if (!hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00 && end - c >= 6 && c[0] == '\\' && c[1] == 'u') {
                    c += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, v);
                break;
            }
            default: o += e;
            }
        }
        if (c >= end) return false;
        ++c;
        return true;
    }
    bool value(Json &v) {
        if (++depth > 64) return false;
        ws();
        if (c >= end) return false;
        bool ok = true;
        if (*c == '{') {
            v.type = Json::OBJ;
            ++c, ws();
            if (c < end && *c == '}') ++c;
            else for (;;) {
                std::string k;
                ws();
                if (!string(k)) return false;
                ws();
                if (c >= end || *c != ':') return false;
                ++c;
                if (!value(v.obj[k])) return false;
                ws();
                if (c < end && *c == ',') { ++c; continue; }
                if (c < end && *c == '}') { ++c; break; }
                return false;
            }
        } else if (*c == '[') {
            v.type = Json::ARR;
            ++c, ws();
            if (c < end && *c == ']') ++c;
            else for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back())) return false;
                ws();
                if (c < end && *c == ',') { ++c; continue; }
                if (c < end && *c == ']') { ++c; break; }
                return false;
            }
        } else if (*c == '"') {
            v.type = Json::STR;
            ok = string(v.s);
        } else if (lit("true")) {
            v.type = Json::BOOL, v.b = true;
        } else if (lit("false")) {
            v.type = Json::BOOL;
        } else if (lit("null")) {
            v.type = Json::NUL;
        } else {
            char *e = nullptr;
            v.type = Json::NUM;
            v.n = strtod(c, &e);
            if (e == c) return false;
            c = e;
        }
        --depth;
        return ok;
    }
};
}  // namespace

bool json_parse(const std::string &text, Json &out) {
    P p{text.data(), text.data() + text.size()};
    out = Json();
    return p.value(out);
}

}  // namespace sanctum
