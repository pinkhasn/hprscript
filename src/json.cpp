#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace hpr::json {

const Value *Value::find(const std::string &key) const {
    if (!is_object()) return nullptr;
    auto it = obj_->find(key);
    return it == obj_->end() ? nullptr : &it->second;
}

namespace {

class Parser {
public:
    Parser(std::string_view text) : t_(text) {}

    static constexpr size_t kMaxDepth = 1000;

    ParseResult parse() {
        ParseResult r;
        skip_ws();
        Value v;
        if (!parse_value(v, r)) return r;
        skip_ws();
        if (i_ != t_.size()) {
            r.error = "trailing garbage";
            r.error_pos = i_;
            return r;
        }
        r.ok = true;
        r.value = std::move(v);
        return r;
    }

private:
    bool fail(ParseResult &r, const char *msg) {
        r.error = msg;
        r.error_pos = i_;
        return false;
    }

    struct DepthGuard {
        size_t &d;
        bool ok;
        DepthGuard(size_t &depth) : d(depth), ok(++d <= kMaxDepth) {}
        ~DepthGuard() { --d; }
    };

    void skip_ws() {
        while (i_ < t_.size()) {
            char c = t_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool parse_value(Value &out, ParseResult &r) {
        skip_ws();
        if (i_ >= t_.size()) return fail(r, "unexpected EOF");
        char c = t_[i_];
        if (c == '{') return parse_object(out, r);
        if (c == '[') return parse_array(out, r);
        if (c == '"') return parse_string_value(out, r);
        if (c == 't' || c == 'f') return parse_bool(out, r);
        if (c == 'n') return parse_null(out, r);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out, r);
        return fail(r, "unexpected character");
    }

    bool parse_object(Value &out, ParseResult &r) {
        DepthGuard g(depth_);
        if (!g.ok) return fail(r, "max nesting depth exceeded");
        ++i_; // '{'
        Object o;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; out = Value::make_object(std::move(o)); return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key, r)) return false;
            skip_ws();
            if (i_ >= t_.size() || t_[i_] != ':') return fail(r, "expected ':'");
            ++i_;
            Value v;
            if (!parse_value(v, r)) return false;
            o.emplace(std::move(key), std::move(v));
            skip_ws();
            if (i_ >= t_.size()) return fail(r, "unterminated object");
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == '}') { ++i_; out = Value::make_object(std::move(o)); return true; }
            return fail(r, "expected ',' or '}'");
        }
    }

    bool parse_array(Value &out, ParseResult &r) {
        DepthGuard g(depth_);
        if (!g.ok) return fail(r, "max nesting depth exceeded");
        ++i_; // '['
        Array a;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; out = Value::make_array(std::move(a)); return true; }
        while (true) {
            Value v;
            if (!parse_value(v, r)) return false;
            a.push_back(std::move(v));
            skip_ws();
            if (i_ >= t_.size()) return fail(r, "unterminated array");
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == ']') { ++i_; out = Value::make_array(std::move(a)); return true; }
            return fail(r, "expected ',' or ']'");
        }
    }

    bool parse_string_value(Value &out, ParseResult &r) {
        std::string s;
        if (!parse_string(s, r)) return false;
        out = Value::make_string(std::move(s));
        return true;
    }

    // Encode codepoint as UTF-8 into out.
    static void utf8_encode(uint32_t cp, std::string &out) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string &out, ParseResult &r) {
        if (i_ >= t_.size() || t_[i_] != '"') return fail(r, "expected string");
        ++i_;
        out.clear();
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= t_.size()) return fail(r, "bad escape");
                char e = t_[i_++];
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (i_ + 4 > t_.size()) return fail(r, "bad \\u escape");
                        uint32_t cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = t_[i_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return fail(r, "bad hex in \\u");
                        }
                        // Optional surrogate pair handling.
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            i_ + 6 <= t_.size() && t_[i_] == '\\' && t_[i_+1] == 'u') {
                            uint32_t lo = 0;
                            i_ += 2;
                            for (int k = 0; k < 4; ++k) {
                                char h = t_[i_++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= h - '0';
                                else if (h >= 'a' && h <= 'f') lo |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') lo |= h - 'A' + 10;
                                else return fail(r, "bad hex in \\u low");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8_encode(cp, out);
                        break;
                    }
                    default: return fail(r, "unknown escape");
                }
            } else {
                out += c;
            }
        }
        return fail(r, "unterminated string");
    }

    bool parse_bool(Value &out, ParseResult &r) {
        if (t_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value::make_bool(true); return true; }
        if (t_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value::make_bool(false); return true; }
        return fail(r, "expected true/false");
    }

    bool parse_null(Value &out, ParseResult &r) {
        if (t_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value(); return true; }
        return fail(r, "expected null");
    }

    bool parse_number(Value &out, ParseResult &r) {
        size_t start = i_;
        if (i_ < t_.size() && t_[i_] == '-') ++i_;
        while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        }
        if (i_ == start) return fail(r, "bad number");
        std::string sub(t_.substr(start, i_ - start));
        char *end = nullptr;
        double d = std::strtod(sub.c_str(), &end);
        if (end != sub.c_str() + sub.size()) return fail(r, "bad number");
        out = Value::make_number(d);
        return true;
    }

    std::string_view t_;
    size_t i_ = 0;
    size_t depth_ = 0;
};

} // namespace

ParseResult parse(std::string_view text) {
    Parser p(text);
    return p.parse();
}

} // namespace hpr::json
