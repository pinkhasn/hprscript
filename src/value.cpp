#include "value.hpp"

#include "output.hpp" // json_escape_to

#include <cstdio>
#include <cstdlib>

namespace hpr {

RuntimeValue RuntimeValue::make_null() { return RuntimeValue(); }

RuntimeValue RuntimeValue::make_bool(bool b) {
    RuntimeValue v; v.kind_ = Bool; v.b_ = b; return v;
}

RuntimeValue RuntimeValue::make_int(int64_t i) {
    RuntimeValue v; v.kind_ = Int; v.i_ = i; return v;
}

RuntimeValue RuntimeValue::make_str(std::string s) {
    RuntimeValue v; v.kind_ = Str; v.s_ = std::move(s); return v;
}

RuntimeValue RuntimeValue::make_list() {
    RuntimeValue v; v.kind_ = List;
    v.list_ = std::make_shared<std::vector<RuntimeValue>>();
    return v;
}

RuntimeValue RuntimeValue::make_map() {
    RuntimeValue v; v.kind_ = Map;
    v.map_ = std::make_shared<std::map<std::string, RuntimeValue>>();
    return v;
}

std::string RuntimeValue::to_str() const {
    switch (kind_) {
        case Null: return "";
        case Bool: return b_ ? "true" : "false";
        case Int: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)i_);
            return buf;
        }
        case Str: return s_;
        case List:
        case Map: return to_json();
    }
    return "";
}

int64_t RuntimeValue::to_int() const {
    switch (kind_) {
        case Null: return 0;
        case Bool: return b_ ? 1 : 0;
        case Int: return i_;
        case Str: {
            // Strip whitespace, parse signed integer; return 0 on failure.
            const char *p = s_.c_str();
            while (*p == ' ' || *p == '\t') ++p;
            char *end = nullptr;
            long long v = std::strtoll(p, &end, 10);
            return end == p ? 0 : (int64_t)v;
        }
        case List: return (int64_t)list_->size();
        case Map: return (int64_t)map_->size();
    }
    return 0;
}

double RuntimeValue::to_double() const {
    switch (kind_) {
        case Null: return 0.0;
        case Bool: return b_ ? 1.0 : 0.0;
        case Int: return (double)i_;
        case Str: {
            const char *p = s_.c_str();
            char *end = nullptr;
            double v = std::strtod(p, &end);
            return end == p ? 0.0 : v;
        }
        case List: return (double)list_->size();
        case Map: return (double)map_->size();
    }
    return 0.0;
}

bool RuntimeValue::to_bool() const {
    switch (kind_) {
        case Null: return false;
        case Bool: return b_;
        case Int: return i_ != 0;
        case Str: return !s_.empty();
        case List: return !list_->empty();
        case Map: return !map_->empty();
    }
    return false;
}

void RuntimeValue::to_json_into(std::string &out) const {
    switch (kind_) {
        case Null: out += "null"; return;
        case Bool: out += b_ ? "true" : "false"; return;
        case Int: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)i_);
            out += buf;
            return;
        }
        case Str:
            out += '"';
            json_escape_to(out, s_);
            out += '"';
            return;
        case List: {
            out += '[';
            bool first = true;
            for (const auto &e : *list_) {
                if (!first) out += ',';
                first = false;
                e.to_json_into(out);
            }
            out += ']';
            return;
        }
        case Map: {
            out += '{';
            bool first = true;
            for (const auto &kv : *map_) {
                if (!first) out += ',';
                first = false;
                out += '"';
                json_escape_to(out, kv.first);
                out += "\":";
                kv.second.to_json_into(out);
            }
            out += '}';
            return;
        }
    }
}

std::string RuntimeValue::to_json() const {
    std::string s;
    to_json_into(s);
    return s;
}

RuntimeValue RuntimeValue::from_json(const json::Value &v) {
    switch (v.type()) {
        case json::Value::Null: return make_null();
        case json::Value::Bool: return make_bool(v.as_bool());
        case json::Value::Number: {
            double d = v.as_number();
            // Preserve integers as Int so emit prints 5 not 5.0.
            if (d == (double)(int64_t)d) return make_int((int64_t)d);
            // Fall back to string for non-integral numbers (rare in scripts).
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            return make_str(buf);
        }
        case json::Value::String: return make_str(v.as_string());
        case json::Value::ArrayT: {
            RuntimeValue rv = make_list();
            for (const auto &e : v.as_array()) {
                rv.list_->push_back(from_json(e));
            }
            return rv;
        }
        case json::Value::ObjectT: {
            RuntimeValue rv = make_map();
            for (const auto &kv : v.as_object()) {
                (*rv.map_)[kv.first] = from_json(kv.second);
            }
            return rv;
        }
    }
    return make_null();
}

bool RuntimeValue::equals(const RuntimeValue &other) const {
    // Numeric/string cross-equality: 5 == "5", true == 1, etc. — the loose
    // comparison the `eq` operator exposes.
    if (kind_ == Int && other.kind_ == Int) return i_ == other.i_;
    if (kind_ == Bool && other.kind_ == Bool) return b_ == other.b_;
    if (kind_ == Str && other.kind_ == Str) return s_ == other.s_;
    if (kind_ == Null && other.kind_ == Null) return true;
    if (kind_ == Null || other.kind_ == Null) return false;
    // Mixed: compare as strings.
    return to_str() == other.to_str();
}

} // namespace hpr
