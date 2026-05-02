// Runtime values for the script interpreter (variables, list/map elements,
// emit records). Distinct from json::Value (parser-only, immutable-ish) so we
// can mutate lists/maps in place during action execution.
#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hpr {

class RuntimeValue {
public:
    enum Kind { Null, Bool, Int, Str, List, Map };

    RuntimeValue() = default;

    static RuntimeValue make_null();
    static RuntimeValue make_bool(bool b);
    static RuntimeValue make_int(int64_t i);
    static RuntimeValue make_str(std::string s);
    static RuntimeValue make_list();
    static RuntimeValue make_map();

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Null; }
    bool is_bool() const { return kind_ == Bool; }
    bool is_int() const { return kind_ == Int; }
    bool is_str() const { return kind_ == Str; }
    bool is_list() const { return kind_ == List; }
    bool is_map() const { return kind_ == Map; }

    bool as_bool() const { return b_; }
    int64_t as_int() const { return i_; }
    const std::string &as_str() const { return s_; }
    std::vector<RuntimeValue> &as_list() { return *list_; }
    const std::vector<RuntimeValue> &as_list() const { return *list_; }
    std::map<std::string, RuntimeValue> &as_map() { return *map_; }
    const std::map<std::string, RuntimeValue> &as_map() const { return *map_; }

    // Coerce to the named representation.
    std::string to_str() const;       // integer as digits, bool as "true"/"false", list/map JSON-rendered
    int64_t to_int() const;           // string parsed, bool 1/0, list/map → 0
    double to_double() const;
    bool to_bool() const;             // truthiness: int!=0, ""=false, []=false, {}=false

    // Render as JSON Lines value.
    std::string to_json() const;
    void to_json_into(std::string &out) const;

    // Build from a parsed JSON value (used for default values, emit data, etc.).
    static RuntimeValue from_json(const json::Value &v);

    // Equality used by `eq`/`ne`/`contains` operators. Does best-effort
    // numeric comparison for int↔string and falls back to string compare.
    bool equals(const RuntimeValue &other) const;

private:
    Kind kind_ = Null;
    bool b_ = false;
    int64_t i_ = 0;
    std::string s_;
    std::shared_ptr<std::vector<RuntimeValue>> list_;
    std::shared_ptr<std::map<std::string, RuntimeValue>> map_;
};

} // namespace hpr
