// Minimal recursive-descent JSON parser.
//
// Just enough to parse hprscript script files (objects/arrays/strings/numbers/
// bool/null). Strings are UTF-8 pass-through except for the standard JSON
// escape set; \uXXXX is decoded to UTF-8.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hpr::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    enum Type { Null, Bool, Number, String, ArrayT, ObjectT };

    Value() : type_(Null) {}

    static Value make_bool(bool b) { Value v; v.type_ = Bool; v.bool_ = b; return v; }
    static Value make_number(double d) { Value v; v.type_ = Number; v.num_ = d; return v; }
    static Value make_string(std::string s) {
        Value v; v.type_ = String; v.str_ = std::move(s); return v;
    }
    static Value make_array(Array a) {
        Value v; v.type_ = ArrayT;
        v.arr_ = std::make_shared<Array>(std::move(a));
        return v;
    }
    static Value make_object(Object o) {
        Value v; v.type_ = ObjectT;
        v.obj_ = std::make_shared<Object>(std::move(o));
        return v;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool is_bool() const { return type_ == Bool; }
    bool is_number() const { return type_ == Number; }
    bool is_string() const { return type_ == String; }
    bool is_array() const { return type_ == ArrayT; }
    bool is_object() const { return type_ == ObjectT; }

    bool as_bool() const { return bool_; }
    double as_number() const { return num_; }
    int64_t as_int() const { return static_cast<int64_t>(num_); }
    const std::string &as_string() const { return str_; }
    const Array &as_array() const { return *arr_; }
    const Object &as_object() const { return *obj_; }

    // Convenience: lookup in object (returns nullptr if missing/not-object).
    const Value *find(const std::string &key) const;

private:
    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error;
    size_t error_pos = 0;
};

ParseResult parse(std::string_view text);

} // namespace hpr::json
