#include "query.hpp"

#include "extract.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "line_index.hpp"
#include "match_row.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "pipeline.hpp"
#include "planner.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpr {
namespace {

struct PatternSpec {
    std::string id;
    std::string regexp;
    bool case_insensitive = false;
    bool word_boundary = false;
    bool utf8 = true;
    bool ucp = false;
    std::vector<std::string> extracts;
};

struct DerivedSpec {
    bool active = false;
    std::string from_set;
    std::string field;
    std::string mode = "literal";
    bool word_boundary = false;
    bool deduplicate = true;
    uint64_t max_patterns = 10000;
    uint64_t max_value_bytes = 4096;
};

struct SetSpec {
    std::string id;
    std::vector<std::string> scan;
    std::vector<std::string> exclude;
    std::string scope;
    std::vector<PatternSpec> patterns;
    DerivedSpec derived;
};

struct Limits {
    uint64_t max_rows_per_set = 250000;
    uint64_t max_total_rows = 500000;
    uint64_t max_join_rows = 500000;
    uint64_t max_cartesian_rows = 1000000;
    uint64_t max_adaptive_stages = 4;
    uint64_t max_memory_bytes = 512ull * 1024 * 1024;
};

struct QueryDoc {
    std::vector<SetSpec> sets;
    json::Value query;
    Limits limits;
    bool partial_on_limit = false;
};

struct JoinedRow {
    std::map<std::string, const MatchRow *> aliases;
    uint64_t ordinal = 0;
};

using Projected = std::map<std::string, RuntimeValue>;

bool valid_id(const std::string &s) {
    if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_'))
        return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

bool fields_only(const json::Object &obj, std::initializer_list<const char *> allowed,
                 const std::string &where, std::string &err) {
    std::set<std::string> ok;
    for (const char *s : allowed) ok.insert(s);
    for (const auto &[key, value] : obj) {
        if (!ok.count(key)) {
            err = where + ": unknown field '" + key + "'";
            return false;
        }
    }
    return true;
}

bool string_array(const json::Value *v, std::vector<std::string> &out,
                  const std::string &where, std::string &err) {
    if (!v) return true;
    if (!v->is_array()) { err = where + " must be an array"; return false; }
    for (const auto &item : v->as_array()) {
        if (!item.is_string()) { err = where + " entries must be strings"; return false; }
        out.push_back(item.as_string());
    }
    return true;
}

std::string regex_escape(const std::string &s) {
    static const char *special = "\\^$.[]|()?*+{}";
    std::string out;
    for (char c : s) { if (std::strchr(special, c)) out += '\\'; out += c; }
    return out;
}

bool parse_pattern(const json::Value &value, PatternSpec &out,
                   const std::string &where, std::string &err) {
    if (!value.is_object()) { err = where + " must be an object"; return false; }
    const auto &o = value.as_object();
    if (!fields_only(o, {"id","regexp","literal","case_insensitive",
                         "word_boundary","utf8","ucp","extract"}, where, err)) return false;
    const json::Value *id = value.find("id");
    if (!id || !id->is_string() || !valid_id(id->as_string())) {
        err = where + ".id must be an identifier"; return false;
    }
    out.id = id->as_string();
    const json::Value *re = value.find("regexp");
    const json::Value *lit = value.find("literal");
    if ((re == nullptr) == (lit == nullptr) || (re && !re->is_string()) ||
        (lit && !lit->is_string())) {
        err = where + " needs exactly one string regexp or literal"; return false;
    }
    out.regexp = re ? re->as_string() : regex_escape(lit->as_string());
    auto get_bool = [&](const char *name, bool &dst) {
        const json::Value *v = value.find(name);
        if (!v) return true;
        if (!v->is_bool()) { err = where + "." + name + " must be boolean"; return false; }
        dst = v->as_bool(); return true;
    };
    if (!get_bool("case_insensitive", out.case_insensitive) ||
        !get_bool("word_boundary", out.word_boundary) ||
        !get_bool("utf8", out.utf8) || !get_bool("ucp", out.ucp)) return false;
    if (!string_array(value.find("extract"), out.extracts,
                      where + ".extract", err)) return false;
    std::set<std::string> names;
    for (const auto &name : out.extracts)
        if (!valid_id(name) || !names.insert(name).second) {
            err = where + ".extract names must be unique identifiers"; return false;
        }
    return true;
}

bool parse_derived(const json::Value &value, DerivedSpec &out,
                   const std::string &where, std::string &err) {
    if (!value.is_object()) { err = where + " must be an object"; return false; }
    const auto &o = value.as_object();
    if (!fields_only(o, {"from_set","field","mode","word_boundary",
                         "deduplicate","max_patterns","max_value_bytes"}, where, err)) return false;
    const auto *from = value.find("from_set");
    const auto *field = value.find("field");
    if (!from || !from->is_string() || !field || !field->is_string()) {
        err = where + " requires string from_set and field"; return false;
    }
    out.active = true; out.from_set = from->as_string(); out.field = field->as_string();
    if (const auto *v = value.find("mode")) {
        if (!v->is_string() || (v->as_string() != "literal" && v->as_string() != "regexp")) {
            err = where + ".mode must be literal or regexp"; return false;
        }
        out.mode = v->as_string();
    }
    if (const auto *v = value.find("word_boundary")) {
        if (!v->is_bool()) { err = where + ".word_boundary must be boolean"; return false; }
        out.word_boundary = v->as_bool();
    }
    if (const auto *v = value.find("deduplicate")) {
        if (!v->is_bool()) { err = where + ".deduplicate must be boolean"; return false; }
        out.deduplicate = v->as_bool();
    }
    auto number = [&](const char *name, uint64_t &dst) {
        const auto *v = value.find(name); if (!v) return true;
        if (!v->is_number() || v->as_number() < 0) {
            err = where + "." + name + " must be non-negative"; return false;
        }
        dst = static_cast<uint64_t>(v->as_number()); return true;
    };
    return number("max_patterns", out.max_patterns) &&
           number("max_value_bytes", out.max_value_bytes);
}

bool parse_doc(const json::Value &root, QueryDoc &doc, std::string &err) {
    if (!root.is_object()) { err = "query document must be an object"; return false; }
    const auto &o = root.as_object();
    if (!fields_only(o, {"version","sets","query","limits","on_limit"},
                     "query document", err)) return false;
    const auto *version = root.find("version");
    if (!version || !version->is_number() || version->as_int() != 1) {
        err = "query document version must be 1"; return false;
    }
    const auto *sets = root.find("sets");
    const auto *query = root.find("query");
    if (!sets || !sets->is_array() || !query || !query->is_object()) {
        err = "query document requires sets array and query object"; return false;
    }
    std::set<std::string> set_ids;
    for (size_t si = 0; si < sets->as_array().size(); ++si) {
        const auto &sv = sets->as_array()[si];
        const std::string where = "sets[" + std::to_string(si) + "]";
        if (!sv.is_object() || !fields_only(sv.as_object(),
                {"id","scan","exclude","scope","patterns","derive_patterns"},
                where, err)) return false;
        SetSpec set;
        const auto *id = sv.find("id");
        if (!id || !id->is_string() || !valid_id(id->as_string()) ||
            !set_ids.insert(id->as_string()).second) {
            err = where + ".id must be a unique identifier"; return false;
        }
        set.id = id->as_string();
        if (!string_array(sv.find("scan"), set.scan, where + ".scan", err) ||
            !string_array(sv.find("exclude"), set.exclude, where + ".exclude", err)) return false;
        if (const auto *scope = sv.find("scope")) {
            if (!scope->is_string()) { err = where + ".scope must be a string"; return false; }
            set.scope = scope->as_string();
        }
        const auto *patterns = sv.find("patterns");
        const auto *derived = sv.find("derive_patterns");
        if ((patterns == nullptr) == (derived == nullptr)) {
            err = where + " needs exactly one of patterns or derive_patterns"; return false;
        }
        if (patterns) {
            if (!patterns->is_array() || patterns->as_array().empty()) {
                err = where + ".patterns must be a non-empty array"; return false;
            }
            std::set<std::string> pat_ids;
            for (size_t pi = 0; pi < patterns->as_array().size(); ++pi) {
                PatternSpec p;
                if (!parse_pattern(patterns->as_array()[pi], p,
                    where + ".patterns[" + std::to_string(pi) + "]", err)) return false;
                if (!pat_ids.insert(p.id).second) {
                    err = where + ": duplicate pattern id '" + p.id + "'"; return false;
                }
                set.patterns.push_back(std::move(p));
            }
        } else if (!parse_derived(*derived, set.derived,
                                  where + ".derive_patterns", err)) return false;
        doc.sets.push_back(std::move(set));
    }
    doc.query = *query;
    if (const auto *limits = root.find("limits")) {
        if (!limits->is_object() || !fields_only(limits->as_object(),
            {"max_rows_per_set","max_total_rows","max_join_rows",
             "max_cartesian_rows","max_adaptive_stages","max_memory_bytes"},
            "limits", err)) return false;
        auto number = [&](const char *name, uint64_t &dst) {
            const auto *v = limits->find(name); if (!v) return true;
            if (!v->is_number() || v->as_number() <= 0) {
                err = std::string("limits.") + name + " must be positive"; return false;
            }
            dst = static_cast<uint64_t>(v->as_number()); return true;
        };
        if (!number("max_rows_per_set", doc.limits.max_rows_per_set) ||
            !number("max_total_rows", doc.limits.max_total_rows) ||
            !number("max_join_rows", doc.limits.max_join_rows) ||
            !number("max_cartesian_rows", doc.limits.max_cartesian_rows) ||
            !number("max_adaptive_stages", doc.limits.max_adaptive_stages) ||
            !number("max_memory_bytes", doc.limits.max_memory_bytes)) return false;
    }
    if (const auto *on = root.find("on_limit")) {
        if (!on->is_string() || (on->as_string() != "fail" && on->as_string() != "partial")) {
            err = "on_limit must be fail or partial"; return false;
        }
        doc.partial_on_limit = on->as_string() == "partial";
    }
    return true;
}

std::string scan_key(const SetSpec &set) {
    std::vector<std::string> scan = set.scan, exclude = set.exclude;
    std::sort(scan.begin(), scan.end()); std::sort(exclude.begin(), exclude.end());
    std::string key = set.scope + "\n";
    for (const auto &s : scan) key += "s:" + s + "\n";
    for (const auto &s : exclude) key += "x:" + s + "\n";
    return key;
}

const SetSpec *find_set(const QueryDoc &doc, const std::string &id, size_t *idx = nullptr) {
    for (size_t i = 0; i < doc.sets.size(); ++i)
        if (doc.sets[i].id == id) { if (idx) *idx = i; return &doc.sets[i]; }
    return nullptr;
}

bool parse_aliases(const QueryDoc &doc, std::map<std::string, size_t> &aliases,
                   std::string &from_alias, size_t &from_set,
                   const json::Array *&joins, std::string &err) {
    const auto &q = doc.query;
    if (!fields_only(q.as_object(), {"from","joins","where","select","group_by",
                     "having","order_by","limit","skip","max_rows","max_output_rows"},
                     "query", err)) return false;
    const auto *from = q.find("from");
    const auto *select = q.find("select");
    if (!from || !from->is_object() || !select || !select->is_object() ||
        !fields_only(from->as_object(), {"set","as"}, "query.from", err)) {
        if (err.empty()) err = "query requires object from and select";
        return false;
    }
    const auto *set = from->find("set"), *as = from->find("as");
    if (!set || !set->is_string() || !as || !as->is_string() ||
        !valid_id(as->as_string()) || !find_set(doc, set->as_string(), &from_set)) {
        err = "query.from requires a known set and identifier alias"; return false;
    }
    from_alias = as->as_string(); aliases[from_alias] = from_set;
    joins = nullptr;
    if (const auto *jv = q.find("joins")) {
        if (!jv->is_array()) { err = "query.joins must be an array"; return false; }
        joins = &jv->as_array();
        for (size_t i = 0; i < joins->size(); ++i) {
            const auto &j = (*joins)[i];
            std::string where = "query.joins[" + std::to_string(i) + "]";
            if (!j.is_object() || !fields_only(j.as_object(),
                    {"type","set","as","on","allow_cartesian"}, where, err))
                return false;
            const auto *type = j.find("type"), *js = j.find("set"), *ja = j.find("as"), *on = j.find("on");
            if (!type || !type->is_string() ||
                (type->as_string() != "inner" && type->as_string() != "left" &&
                 type->as_string() != "semi" && type->as_string() != "anti") ||
                !js || !js->is_string() || !ja || !ja->is_string() ||
                !valid_id(ja->as_string()) || aliases.count(ja->as_string()) ||
                !on || !on->is_array()) {
                err = where + " has invalid type/set/as/on"; return false;
            }
            if (const auto *allow = j.find("allow_cartesian")) {
                if (!allow->is_bool()) {
                    err = where + ".allow_cartesian must be boolean"; return false;
                }
            }
            size_t set_idx = 0;
            if (!find_set(doc, js->as_string(), &set_idx)) {
                err = where + ": unknown set '" + js->as_string() + "'"; return false;
            }
            aliases[ja->as_string()] = set_idx;
        }
    }
    return true;
}

bool known_row_field(const SetSpec &set, const std::string &field) {
    static const std::set<std::string> builtins = {
        "row_id","set_id","pattern_id","file","language","from","to","line",
        "column","match","context","enclosing.name","enclosing.kind",
        "enclosing.from","enclosing.to","enclosing.line_start","enclosing.line_end",
        "derived.value","derived.source_rows"
    };
    if (builtins.count(field)) return true;
    const std::string prefix = "capture.";
    if (field.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string name = field.substr(prefix.size());
    if (name == "derived_value") return set.derived.active;
    for (const auto &p : set.patterns)
        if (std::find(p.extracts.begin(), p.extracts.end(), name) != p.extracts.end()) return true;
    return false;
}

bool validate_ref(const std::string &ref, const QueryDoc &doc,
                  const std::map<std::string, size_t> &aliases, std::string &err) {
    const size_t dot = ref.find('.');
    if (dot == std::string::npos) return true; // literal or output field
    const std::string alias = ref.substr(0, dot);
    auto it = aliases.find(alias);
    if (it == aliases.end()) return true; // literal containing a dot
    const std::string field = ref.substr(dot + 1);
    if (!known_row_field(doc.sets[it->second], field)) {
        err = "unknown field reference '" + ref + "'"; return false;
    }
    return true;
}

bool validate_refs_recursive(const json::Value &v, const QueryDoc &doc,
                             const std::map<std::string, size_t> &aliases,
                             std::string &err) {
    if (v.is_string()) return validate_ref(v.as_string(), doc, aliases, err);
    if (v.is_array()) for (const auto &x : v.as_array())
        if (!validate_refs_recursive(x, doc, aliases, err)) return false;
    if (v.is_object()) {
        if (v.as_object().size() == 1 && v.as_object().count("literal")) return true;
        for (const auto &[k, x] : v.as_object())
            if (!validate_refs_recursive(x, doc, aliases, err)) return false;
    }
    return true;
}

bool validate_pred_schema(const json::Value &v, const std::string &where,
                          std::string &err) {
    if (!v.is_object()) { err = where + " must be an operation object"; return false; }
    const auto *opv = v.find("op");
    if (!opv || !opv->is_string()) { err = where + " requires string op"; return false; }
    const std::string op = opv->as_string();
    static const std::set<std::string> binary = {
        "eq","ne","lt","lte","gt","gte","contains","in","starts_with","ends_with",
        "same_file","same_scope","before","after","contains_span"
    };
    if (op == "and" || op == "or") {
        if (!fields_only(v.as_object(), {"op","args"}, where, err)) return false;
        const auto *args = v.find("args");
        if (!args || !args->is_array() || args->as_array().empty()) {
            err = where + ".args must be a non-empty array"; return false;
        }
        for (size_t i = 0; i < args->as_array().size(); ++i)
            if (!validate_pred_schema(args->as_array()[i], where + ".args[" +
                                      std::to_string(i) + "]", err)) return false;
        return true;
    }
    if (op == "not") {
        if (!fields_only(v.as_object(), {"op","arg","left"}, where, err)) return false;
        const auto *arg = v.find("arg"); if (!arg) arg = v.find("left");
        if (!arg) { err = where + " not requires arg"; return false; }
        return validate_pred_schema(*arg, where + ".arg", err);
    }
    if (op == "is_null" || op == "is_not_null") {
        if (!fields_only(v.as_object(), {"op","left"}, where, err) || !v.find("left")) {
            if (err.empty()) err = where + " requires left";
            return false;
        }
        return true;
    }
    if (op == "within_lines") {
        if (!fields_only(v.as_object(), {"op","left","right","lines"}, where, err) ||
            !v.find("left") || !v.find("right") || !v.find("lines") ||
            !v.find("lines")->is_number() || v.find("lines")->as_number() < 0) {
            if (err.empty()) err = where + " requires left, right, and non-negative lines";
            return false;
        }
        return true;
    }
    if (binary.count(op)) {
        if (!fields_only(v.as_object(), {"op","left","right"}, where, err) ||
            !v.find("left") || !v.find("right")) {
            if (err.empty()) err = where + " requires left and right";
            return false;
        }
        return true;
    }
    err = where + ": unknown predicate op '" + op + "'";
    return false;
}

bool validate_expr_schema(const json::Value &v, const std::string &where,
                          std::string &err) {
    if (!v.is_object()) return true;
    if (v.as_object().size() != 1) {
        err = where + " expression must contain exactly one operation"; return false;
    }
    const auto &[op, arg] = *v.as_object().begin();
    static const std::set<std::string> unary = {"lower","upper","basename","dirname"};
    static const std::set<std::string> aggregate = {"count","count_distinct","min","max",
                                                     "sum","collect","collect_distinct","first"};
    if (op == "literal") return true;
    if (op == "coalesce" || op == "concat") {
        if (!arg.is_array()) { err = where + "." + op + " must be an array"; return false; }
        for (size_t i = 0; i < arg.as_array().size(); ++i)
            if (!validate_expr_schema(arg.as_array()[i], where + "." + op + "[" +
                                      std::to_string(i) + "]", err)) return false;
        return true;
    }
    if (unary.count(op)) return validate_expr_schema(arg, where + "." + op, err);
    if (aggregate.count(op)) return op == "count" ||
        validate_expr_schema(arg, where + "." + op, err);
    err = where + ": unknown expression op '" + op + "'";
    return false;
}

bool validate_location_aliases(const json::Value &v,
                               const std::map<std::string, size_t> &aliases,
                               const std::string &where, std::string &err) {
    const std::string op = v.find("op")->as_string();
    if (op == "and" || op == "or") {
        size_t i = 0;
        for (const auto &arg : v.find("args")->as_array())
            if (!validate_location_aliases(arg, aliases, where + ".args[" +
                                           std::to_string(i++) + "]", err)) return false;
        return true;
    }
    if (op == "not") {
        const auto *arg = v.find("arg"); if (!arg) arg = v.find("left");
        return validate_location_aliases(*arg, aliases, where + ".arg", err);
    }
    static const std::set<std::string> location = {
        "same_file","same_scope","within_lines","before","after","contains_span"
    };
    if (!location.count(op)) return true;
    const auto *left = v.find("left"), *right = v.find("right");
    if (!left->is_string() || !right->is_string() ||
        !aliases.count(left->as_string()) || !aliases.count(right->as_string())) {
        err = where + " location predicate operands must be known aliases";
        return false;
    }
    return true;
}

RuntimeValue operand(const json::Value &v, const JoinedRow &row,
                     const Projected *projected = nullptr, bool *known = nullptr) {
    if (known) *known = true;
    if (v.is_null()) return RuntimeValue::make_null();
    if (v.is_bool()) return RuntimeValue::make_bool(v.as_bool());
    if (v.is_number()) return RuntimeValue::make_int(v.as_int());
    if (!v.is_string()) return RuntimeValue::from_json(v);
    const std::string &s = v.as_string();
    if (projected) {
        auto pit = projected->find(s);
        if (pit != projected->end()) return pit->second;
    }
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return RuntimeValue::make_str(s);
    auto it = row.aliases.find(s.substr(0, dot));
    if (it == row.aliases.end()) return RuntimeValue::make_str(s);
    if (!it->second) return RuntimeValue::make_null();
    return match_row_field(*it->second, s.substr(dot + 1), known);
}

const MatchRow *alias_row(const json::Value *v, const JoinedRow &row) {
    if (!v || !v->is_string()) return nullptr;
    auto it = row.aliases.find(v->as_string());
    return it == row.aliases.end() ? nullptr : it->second;
}

int compare_values(const RuntimeValue &a, const RuntimeValue &b) {
    if ((a.is_int() || a.is_bool()) && (b.is_int() || b.is_bool())) {
        const int64_t x = a.to_int(), y = b.to_int(); return x < y ? -1 : x > y ? 1 : 0;
    }
    const std::string x = a.to_str(), y = b.to_str(); return x < y ? -1 : x > y ? 1 : 0;
}

bool eval_pred(const json::Value &v, const JoinedRow &row,
               const Projected *projected = nullptr) {
    if (!v.is_object()) return operand(v, row, projected).to_bool();
    const auto *opv = v.find("op");
    if (!opv || !opv->is_string()) return false;
    const std::string op = opv->as_string();
    if (op == "and" || op == "or") {
        const auto *args = v.find("args"); if (!args || !args->is_array()) return false;
        bool value = op == "and";
        for (const auto &arg : args->as_array()) {
            bool part = eval_pred(arg, row, projected);
            if (op == "and") value = value && part; else value = value || part;
        }
        return value;
    }
    if (op == "not") {
        const auto *arg = v.find("arg");
        if (!arg) arg = v.find("left");
        return arg && !eval_pred(*arg, row, projected);
    }
    if (op == "same_file" || op == "same_scope" || op == "within_lines" ||
        op == "before" || op == "after" || op == "contains_span") {
        const MatchRow *a = alias_row(v.find("left"), row);
        const MatchRow *b = alias_row(v.find("right"), row);
        if (!a || !b) return false;
        if (op == "same_file") return a->file == b->file;
        if (op == "same_scope") return a->enclosing && b->enclosing &&
            a->file == b->file && a->enclosing->from == b->enclosing->from &&
            a->enclosing->to == b->enclosing->to;
        if (op == "within_lines") {
            const auto *n = v.find("lines");
            return n && n->is_number() && a->file == b->file &&
                std::llabs(static_cast<long long>(a->line) - b->line) <= n->as_int();
        }
        if (op == "before") return a->file == b->file && a->to <= b->from;
        if (op == "after") return a->file == b->file && a->from >= b->to;
        return a->file == b->file && a->from <= b->from && a->to >= b->to;
    }
    const auto *left = v.find("left");
    if (!left) return false;
    RuntimeValue a = operand(*left, row, projected);
    if (op == "is_null") return a.is_null();
    if (op == "is_not_null") return !a.is_null();
    const auto *right = v.find("right"); if (!right) return false;
    RuntimeValue b = operand(*right, row, projected);
    if (op == "eq") return a.equals(b);
    if (op == "ne") return !a.equals(b);
    int cmp = compare_values(a, b);
    if (op == "lt") return cmp < 0;
    if (op == "lte") return cmp <= 0;
    if (op == "gt") return cmp > 0;
    if (op == "gte") return cmp >= 0;
    const std::string x = a.to_str(), y = b.to_str();
    if (op == "contains") {
        if (a.is_list()) {
            for (const auto &item : a.as_list()) if (item.equals(b)) return true;
            return false;
        }
        return x.find(y) != std::string::npos;
    }
    if (op == "in") {
        if (!b.is_list()) return false;
        for (const auto &item : b.as_list()) if (a.equals(item)) return true;
        return false;
    }
    if (op == "starts_with") return x.compare(0, y.size(), y) == 0;
    if (op == "ends_with") return x.size() >= y.size() &&
        x.compare(x.size() - y.size(), y.size(), y) == 0;
    return false;
}

RuntimeValue eval_expr(const json::Value &v, const JoinedRow &row) {
    if (!v.is_object()) return operand(v, row);
    const auto &o = v.as_object();
    if (o.size() != 1) return RuntimeValue::make_null();
    const auto &[op, arg] = *o.begin();
    if (op == "literal") return RuntimeValue::from_json(arg);
    if (op == "coalesce" && arg.is_array()) {
        for (const auto &x : arg.as_array()) { RuntimeValue r = eval_expr(x, row); if (!r.is_null()) return r; }
        return RuntimeValue::make_null();
    }
    if (op == "concat" && arg.is_array()) {
        std::string s; for (const auto &x : arg.as_array()) s += eval_expr(x, row).to_str();
        return RuntimeValue::make_str(std::move(s));
    }
    RuntimeValue r = eval_expr(arg, row);
    if (op == "lower" || op == "upper") {
        std::string s = r.to_str();
        std::transform(s.begin(), s.end(), s.begin(), [&](unsigned char c) {
            return static_cast<char>(op == "lower" ? std::tolower(c) : std::toupper(c));
        });
        return RuntimeValue::make_str(std::move(s));
    }
    if (op == "basename") return RuntimeValue::make_str(std::filesystem::path(r.to_str()).filename().string());
    if (op == "dirname") return RuntimeValue::make_str(std::filesystem::path(r.to_str()).parent_path().string());
    return RuntimeValue::make_null();
}

bool aggregate_expr(const json::Value &v, std::string *op = nullptr,
                    const json::Value **arg = nullptr) {
    if (!v.is_object() || v.as_object().size() != 1) return false;
    const auto &item = *v.as_object().begin();
    static const std::set<std::string> ops = {"count","count_distinct","min","max",
                                              "sum","collect","collect_distinct","first"};
    if (!ops.count(item.first)) return false;
    if (op) *op = item.first;
    if (arg) *arg = &item.second;
    return true;
}

RuntimeValue aggregate(const std::string &op, const json::Value &expr,
                       const std::vector<const JoinedRow *> &rows) {
    if (op == "count") return RuntimeValue::make_int(rows.size());
    std::vector<RuntimeValue> values;
    for (const auto *row : rows) values.push_back(eval_expr(expr, *row));
    if (op == "first") return values.empty() ? RuntimeValue::make_null() : values.front();
    if (op == "sum") {
        int64_t n = 0; for (const auto &v : values) n += v.to_int(); return RuntimeValue::make_int(n);
    }
    if (op == "min" || op == "max") {
        if (values.empty()) return RuntimeValue::make_null();
        RuntimeValue best = values.front();
        for (size_t i = 1; i < values.size(); ++i) {
            int c = compare_values(values[i], best);
            if ((op == "min" && c < 0) || (op == "max" && c > 0)) best = values[i];
        }
        return best;
    }
    if (op == "count_distinct") {
        std::set<std::string> seen; for (const auto &v : values) seen.insert(v.to_json());
        return RuntimeValue::make_int(seen.size());
    }
    RuntimeValue list = RuntimeValue::make_list();
    std::set<std::string> seen;
    for (const auto &v : values) {
        if (op == "collect_distinct" && !seen.insert(v.to_json()).second) continue;
        list.as_list().push_back(v);
    }
    return list;
}

std::string value_key(const RuntimeValue &v) {
    return std::to_string(static_cast<int>(v.kind())) + ":" + v.to_json();
}

std::string row_group_key(const json::Array &fields, const JoinedRow &row) {
    std::string key;
    for (const auto &f : fields) { std::string k = value_key(eval_expr(f, row)); key += std::to_string(k.size()) + ":" + k; }
    return key;
}

std::string join_hash_key(const std::vector<std::pair<std::string,std::string>> &pairs,
                          const JoinedRow &row, bool right_side) {
    std::string key;
    for (const auto &[left, right] : pairs) {
        json::Value v = json::Value::make_string(right_side ? right : left);
        std::string k = value_key(operand(v, row)); key += std::to_string(k.size()) + ":" + k;
    }
    return key;
}

bool equality_pairs(const json::Array &preds, const std::string &join_alias,
                    std::vector<std::pair<std::string,std::string>> &pairs) {
    for (const auto &p : preds) {
        if (!p.is_object()) continue;
        const auto *op = p.find("op"), *l = p.find("left"), *r = p.find("right");
        if (!op || !op->is_string() || op->as_string() != "eq" ||
            !l || !l->is_string() || !r || !r->is_string()) continue;
        bool lj = l->as_string().compare(0, join_alias.size() + 1, join_alias + ".") == 0;
        bool rj = r->as_string().compare(0, join_alias.size() + 1, join_alias + ".") == 0;
        if (lj == rj) continue;
        pairs.push_back(lj ? std::make_pair(r->as_string(), l->as_string())
                           : std::make_pair(l->as_string(), r->as_string()));
    }
    return !pairs.empty();
}

bool location_strategy(const json::Array &preds, const std::string &join_alias,
                       std::string &peer, bool &scope_partition) {
    std::string fallback;
    for (const auto &pred : preds) {
        if (!pred.is_object()) continue;
        const auto *op = pred.find("op"), *left = pred.find("left"),
                   *right = pred.find("right");
        if (!op || !op->is_string() || !left || !left->is_string() ||
            !right || !right->is_string()) continue;
        const std::string &name = op->as_string();
        if (name != "same_file" && name != "same_scope" &&
            name != "within_lines" && name != "before" && name != "after" &&
            name != "contains_span") continue;
        std::string candidate;
        if (left->as_string() == join_alias) candidate = right->as_string();
        else if (right->as_string() == join_alias) candidate = left->as_string();
        else continue;
        if (name == "same_scope") {
            peer = std::move(candidate); scope_partition = true; return true;
        }
        if (fallback.empty()) fallback = std::move(candidate);
    }
    if (fallback.empty()) return false;
    peer = std::move(fallback); scope_partition = false; return true;
}

bool all_join_preds(const json::Array &preds, const JoinedRow &row) {
    for (const auto &p : preds) if (!eval_pred(p, row)) return false;
    return true;
}

std::string projected_json(const Projected &row) {
    std::string out = "{"; size_t n = 0;
    for (const auto &[k, v] : row) {
        if (n++) out += ',';
        out += '"'; json_escape_to(out, k); out += "\":"; out += v.to_json();
    }
    out += "}\n"; return out;
}

std::string normalized_cell(std::string s) {
    for (char &c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

} // namespace

int run_query(const Cli &cli) {
    const auto started = std::chrono::steady_clock::now();
    std::string source;
    if (!cli.query.inline_json.empty()) source = cli.query.inline_json;
    else {
        std::ifstream in(cli.query.path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "hprscript: query: cannot read %s\n", cli.query.path.c_str()); return 2; }
        source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto parsed = json::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "hprscript: query JSON parse error at %zu: %s\n",
                     parsed.error_pos, parsed.error.c_str()); return 2;
    }
    QueryDoc doc; std::string err;
    if (!parse_doc(parsed.value, doc, err)) {
        std::fprintf(stderr, "hprscript: query: %s\n", err.c_str()); return 2;
    }
    std::map<std::string, size_t> aliases;
    std::string from_alias; size_t from_set = 0; const json::Array *joins = nullptr;
    if (!parse_aliases(doc, aliases, from_alias, from_set, joins, err) ||
        !validate_refs_recursive(doc.query, doc, aliases, err)) {
        std::fprintf(stderr, "hprscript: query: %s\n", err.c_str()); return 2;
    }
    if (joins) for (size_t i = 0; i < joins->size(); ++i) {
        const auto &on = joins->at(i).find("on")->as_array();
        if (on.empty()) { err = "query.joins[" + std::to_string(i) + "].on cannot be empty"; break; }
        for (size_t p = 0; p < on.size(); ++p)
            if (!validate_pred_schema(on[p], "query.joins[" + std::to_string(i) +
                                      "].on[" + std::to_string(p) + "]", err) ||
                !validate_location_aliases(on[p], aliases, "query.joins[" +
                                           std::to_string(i) + "].on[" +
                                           std::to_string(p) + "]", err)) break;
        if (!err.empty()) break;
    }
    if (err.empty()) if (const auto *where = doc.query.find("where")) {
        validate_pred_schema(*where, "query.where", err);
        if (err.empty()) validate_location_aliases(*where, aliases, "query.where", err);
    }
    if (err.empty()) if (const auto *having = doc.query.find("having")) {
        validate_pred_schema(*having, "query.having", err);
        if (err.empty()) validate_location_aliases(*having, aliases, "query.having", err);
    }
    if (err.empty()) {
        const auto &select = doc.query.find("select")->as_object();
        for (const auto &[name, expr] : select)
            if (!validate_expr_schema(expr, "query.select." + name, err)) break;
    }
    if (err.empty()) if (const auto *group = doc.query.find("group_by")) {
        if (!group->is_array()) err = "query.group_by must be an array";
        else for (const auto &field : group->as_array())
            if (!field.is_string()) { err = "query.group_by entries must be field strings"; break; }
    }
    if (err.empty()) if (const auto *order = doc.query.find("order_by")) {
        if (!order->is_array()) err = "query.order_by must be an array";
        else for (size_t i = 0; i < order->as_array().size(); ++i) {
            const auto &item = order->as_array()[i];
            const std::string where = "query.order_by[" + std::to_string(i) + "]";
            if (!item.is_object() || !fields_only(item.as_object(), {"field","direction"}, where, err)) break;
            const auto *field = item.find("field"), *direction = item.find("direction");
            if (!field || !field->is_string() ||
                (direction && (!direction->is_string() ||
                 (direction->as_string() != "asc" && direction->as_string() != "desc")))) {
                err = where + " requires field and optional asc/desc direction"; break;
            }
            if (!doc.query.find("select")->as_object().count(field->as_string())) {
                err = where + ": unknown projected field '" + field->as_string() + "'"; break;
            }
        }
    }
    auto validate_nonnegative = [&](const char *name) {
        if (const auto *v = doc.query.find(name))
            if (!v->is_number() || v->as_number() < 0)
                err = std::string("query.") + name + " must be non-negative";
    };
    if (err.empty()) validate_nonnegative("limit");
    if (err.empty()) validate_nonnegative("skip");
    if (err.empty()) validate_nonnegative("max_rows");
    if (err.empty()) validate_nonnegative("max_output_rows");
    if (!err.empty()) {
        std::fprintf(stderr, "hprscript: query: %s\n", err.c_str()); return 2;
    }
    // Adaptive dependencies must point backward so execution is acyclic and
    // deterministic in version 1.
    uint64_t adaptive_stages = 0;
    for (size_t i = 0; i < doc.sets.size(); ++i) if (doc.sets[i].derived.active) {
        if (++adaptive_stages > doc.limits.max_adaptive_stages) {
            std::fprintf(stderr,
                "hprscript: query: adaptive stage count exceeds max_adaptive_stages=%llu\n",
                static_cast<unsigned long long>(doc.limits.max_adaptive_stages));
            return 2;
        }
        size_t src = 0;
        if (!find_set(doc, doc.sets[i].derived.from_set, &src) || src >= i) {
            std::fprintf(stderr, "hprscript: query: adaptive set %s must reference an earlier set\n",
                         doc.sets[i].id.c_str()); return 2;
        }
        if (!known_row_field(doc.sets[src], doc.sets[i].derived.field)) {
            std::fprintf(stderr, "hprscript: query: adaptive field %s is unknown on set %s\n",
                         doc.sets[i].derived.field.c_str(), doc.sets[src].id.c_str()); return 2;
        }
    }

    if (joins) for (size_t i = 0; i < joins->size(); ++i) {
        const auto &join = joins->at(i);
        const std::string alias = join.find("as")->as_string();
        const auto &predicates = join.find("on")->as_array();
        std::vector<std::pair<std::string,std::string>> pairs;
        std::string peer; bool scope_partition = false;
        const bool indexed = equality_pairs(predicates, alias, pairs) ||
                             location_strategy(predicates, alias, peer,
                                               scope_partition);
        const auto *allow = join.find("allow_cartesian");
        const bool explicitly_allowed = allow && allow->as_bool();
        const uint64_t left_bound = i == 0 ? doc.limits.max_rows_per_set
                                           : doc.limits.max_join_rows;
        const bool above_threshold = doc.limits.max_rows_per_set &&
            left_bound > doc.limits.max_cartesian_rows /
                         doc.limits.max_rows_per_set;
        if (!indexed && above_threshold && !explicitly_allowed) {
            std::fprintf(stderr,
                "hprscript: query: join %zu has no equality/location index and "
                "its predicted Cartesian product exceeds max_cartesian_rows=%llu; "
                "add an indexable predicate, raise the limit, or set allow_cartesian:true\n",
                i, static_cast<unsigned long long>(doc.limits.max_cartesian_rows));
            return 2;
        }
    }

    std::vector<std::vector<size_t>> static_groups;
    std::map<std::string, size_t> group_by_key;
    for (size_t i = 0; i < doc.sets.size(); ++i) {
        if (doc.sets[i].derived.active) continue;
        std::string key = scan_key(doc.sets[i]);
        auto [it, inserted] = group_by_key.emplace(key, static_groups.size());
        if (inserted) static_groups.push_back({});
        static_groups[it->second].push_back(i);
    }

    if (cli.explain_plan) {
        ExecutionPlan plan; plan.mode = "query";
        size_t stage_no = 0;
        for (const auto &group : static_groups) {
            PlanStage stage; stage.id = "scan" + std::to_string(stage_no++);
            for (size_t si : group) {
                stage.sets.push_back(doc.sets[si].id);
                stage.patterns += doc.sets[si].patterns.size();
            }
            stage.inputs = doc.sets[group.front()].scan;
            stage.scope = doc.sets[group.front()].scope;
            plan.scan_stages.push_back(std::move(stage));
        }
        for (const auto &set : doc.sets) if (set.derived.active) {
            PlanStage stage; stage.id = "adaptive" + std::to_string(stage_no++);
            stage.sets = {set.id}; stage.patterns = set.derived.max_patterns;
            stage.inputs = set.scan; stage.scope = set.scope; stage.adaptive = true;
            plan.scan_stages.push_back(std::move(stage));
        }
        if (joins) for (const auto &j : *joins) {
            std::vector<std::pair<std::string,std::string>> pairs;
            std::string peer; bool scope_partition = false;
            const bool hash = equality_pairs(j.find("on")->as_array(),
                                             j.find("as")->as_string(), pairs);
            const bool location = location_strategy(j.find("on")->as_array(),
                j.find("as")->as_string(), peer, scope_partition);
            std::string op = (hash ? "hash-" : location ? "interval-" : "cartesian-") +
                             j.find("type")->as_string() + "-join";
            plan.postprocess.push_back({op, {{"set", j.find("set")->as_string()}}});
        }
        if (doc.query.find("where")) plan.postprocess.push_back({"filter", {}});
        if (doc.query.find("group_by")) plan.postprocess.push_back({"group-aggregate", {}});
        plan.postprocess.push_back({"project", {}});
        if (doc.query.find("order_by")) plan.postprocess.push_back({"order", {}});
        plan.limits["max_rows_per_set"] = doc.limits.max_rows_per_set;
        plan.limits["max_total_rows"] = doc.limits.max_total_rows;
        plan.limits["max_join_rows"] = doc.limits.max_join_rows;
        plan.limits["max_cartesian_rows"] = doc.limits.max_cartesian_rows;
        plan.limits["max_adaptive_stages"] = doc.limits.max_adaptive_stages;
        plan.limits["max_memory_bytes"] = doc.limits.max_memory_bytes;
        emit_execution_plan(plan);
        if (cli.plan_only) return 0;
    }

    ScanStats stats;
    std::vector<std::vector<MatchRow>> rows(doc.sets.size());
    uint64_t next_row_id = 1, memory_bytes = 0;
    bool hard_limit = false;
    auto limit_hit = [&](const std::string &reason, uint64_t omitted = 1) {
        stats.stop_reason = reason; stats.rows_truncated += omitted;
        hard_limit = !doc.partial_on_limit;
    };

    const bool input_override = !cli.globs.empty() || !cli.positional.empty() ||
        !cli.file_lists.empty() || cli.git_changed || cli.git_staged ||
        cli.git_untracked || !cli.git_ranges.empty();

    struct CompiledMapping {
        size_t set_index;
        uint32_t local_pattern;
        std::string derived_value;
        std::vector<uint64_t> source_rows;
    };

    auto scan_stage = [&](const std::vector<size_t> &set_indices,
                          std::vector<Pattern> pats,
                          std::vector<CompiledMapping> mapping,
                          const SetSpec &scan_spec) -> bool {
        if (pats.empty()) return true;
        Matcher matcher; CompileError ce;
        if (!matcher.compile(pats, &ce)) {
            std::fprintf(stderr, "hprscript: query pattern compile failed");
            if (ce.pattern_index >= 0 && static_cast<size_t>(ce.pattern_index) < mapping.size() &&
                !mapping[ce.pattern_index].derived_value.empty())
                std::fprintf(stderr, " for derived value '%s'",
                             mapping[ce.pattern_index].derived_value.c_str());
            std::fprintf(stderr, ": %s\n", ce.message.c_str()); return false;
        }
        ++stats.matcher_compilations; stats.patterns_compiled += pats.size(); ++stats.scan_stages;
        ExtractTable extracts; std::string xerr; int xidx = -1;
        if (!extracts.build(pats, &xerr, &xidx)) {
            std::fprintf(stderr, "hprscript: query extraction compile failed: %s\n", xerr.c_str()); return false;
        }
        Cli stage_cli = cli;
        if (!input_override) {
            stage_cli.globs = scan_spec.scan;
            stage_cli.positional.clear(); stage_cli.file_lists.clear();
            stage_cli.git_changed = stage_cli.git_staged = stage_cli.git_untracked = false;
            stage_cli.git_ranges.clear();
        }
        stage_cli.excludes.insert(stage_cli.excludes.end(), scan_spec.exclude.begin(), scan_spec.exclude.end());
        Walker walker; std::unordered_map<std::string, AddedLines> unused;
        if (!add_walker_inputs(stage_cli, walker, stats, unused)) return false;
        MatchCollector collector(pats, {}, false);
        ScopeConfig custom;
        auto scan_file = [&](const std::string &path, std::string_view content) {
            LineIndex lines; lines.build(content);
            ScopeIndex scopes; const ScopeIndex *scope = nullptr;
            if (!scan_spec.scope.empty())
                scope = build_file_scope(scan_spec.scope, custom, path, content, lines, scopes);
            std::vector<Match> matches;
            collector.collect(matcher, content, lines, scope, nullptr, matches);
            stats.matches_seen += matches.size();
            for (const auto &m : matches) {
                if (m.pattern_index >= mapping.size()) continue;
                const CompiledMapping &map = mapping[m.pattern_index];
                auto &dest = rows[map.set_index];
                if (dest.size() >= doc.limits.max_rows_per_set ||
                    stats.rows_materialized >= doc.limits.max_total_rows) {
                    limit_hit("query_row_limit", matches.size()); return false;
                }
                MatchRow row = materialize_match_row(next_row_id++, map.set_index,
                    doc.sets[map.set_index].id, pats, m, path, content, lines, scope,
                    extracts.any() ? &extracts : nullptr);
                row.pattern_index = map.local_pattern;
                if (doc.sets[map.set_index].derived.active) {
                    row.pattern_id = "derived" + std::to_string(map.local_pattern);
                    row.derived_value = map.derived_value;
                    row.derived_source_rows = map.source_rows;
                    row.captures["derived_value"] = RuntimeValue::make_str(map.derived_value);
                } else {
                    row.pattern_id = doc.sets[map.set_index].patterns[map.local_pattern].id;
                }
                memory_bytes += sizeof(MatchRow) + row.file.size() + row.match.size() + row.context.size();
                for (const auto &[k,v] : row.captures) memory_bytes += k.size() + v.to_str().size();
                stats.buffered_bytes_peak = std::max(stats.buffered_bytes_peak, memory_bytes);
                if (memory_bytes > doc.limits.max_memory_bytes) {
                    limit_hit("query_memory_limit"); return false;
                }
                dest.push_back(std::move(row)); ++stats.rows_materialized;
            }
            return !hard_limit;
        };
        const bool no_inputs = stage_cli.globs.empty() && stage_cli.positional.empty() &&
            stage_cli.file_lists.empty() && !stage_cli.git_changed && !stage_cli.git_staged &&
            !stage_cli.git_untracked && stage_cli.git_ranges.empty();
        if (no_inputs) {
            std::fprintf(stderr, "hprscript: query scan set has no inputs\n"); return false;
        }
        walker.walk([&](const WalkItem &item) {
            MappedFile mf;
            if (!mf.open(item.path)) {
                ++stats.files_failed;
                if (cli.diagnostics) emit_warning_record("read_error", item.path);
                return !hard_limit;
            }
            if (looks_binary(mf.view())) { ++stats.files_binary; return true; }
            ++stats.files_scanned; stats.bytes_scanned += mf.view().size();
            return scan_file(item.path, mf.view());
        });
        return !hard_limit;
    };

    for (const auto &group : static_groups) {
        std::vector<Pattern> pats; std::vector<CompiledMapping> map;
        for (size_t si : group) for (size_t pi = 0; pi < doc.sets[si].patterns.size(); ++pi) {
            const auto &src = doc.sets[si].patterns[pi]; Pattern p;
            p.id = doc.sets[si].id + "__" + src.id; p.regexp = src.regexp;
            p.case_insensitive = src.case_insensitive; p.word_boundary = src.word_boundary;
            p.utf8 = src.utf8; p.ucp = src.ucp; p.extract_names = src.extracts;
            pats.push_back(std::move(p)); map.push_back({si, static_cast<uint32_t>(pi), {}, {}});
        }
        if (!scan_stage(group, std::move(pats), std::move(map), doc.sets[group.front()])) {
            if (hard_limit) break;
            return 2;
        }
    }
    if (!hard_limit) for (size_t si = 0; si < doc.sets.size(); ++si) {
        const SetSpec &set = doc.sets[si]; if (!set.derived.active) continue;
        size_t src_idx = 0; find_set(doc, set.derived.from_set, &src_idx);
        std::map<std::string, std::vector<uint64_t>> sources;
        uint64_t empty = 0, too_long = 0;
        for (const auto &row : rows[src_idx]) {
            bool known = false; RuntimeValue v = match_row_field(row, set.derived.field, &known);
            std::string value = v.to_str();
            if (!known || value.empty()) { ++empty; continue; }
            if (value.size() > set.derived.max_value_bytes) { ++too_long; continue; }
            sources[value].push_back(row.row_id);
        }
        if (sources.size() > set.derived.max_patterns) {
            std::fprintf(stderr, "hprscript: query adaptive set %s produced %zu patterns (limit %llu)\n",
                         set.id.c_str(), sources.size(),
                         static_cast<unsigned long long>(set.derived.max_patterns)); return 2;
        }
        if (cli.diagnostics && (empty || too_long)) {
            std::printf("{\"type\":\"warning\",\"code\":\"derived_values_skipped\",\"set\":\"%s\",\"empty\":%llu,\"too_long\":%llu}\n",
                json_escape(set.id).c_str(), (unsigned long long)empty, (unsigned long long)too_long);
        } else if (empty || too_long) {
            std::fprintf(stderr,
                "hprscript: query: adaptive set %s skipped %llu empty and %llu overlong values\n",
                set.id.c_str(), (unsigned long long)empty,
                (unsigned long long)too_long);
        }
        std::vector<Pattern> pats; std::vector<CompiledMapping> map; uint32_t pi = 0;
        for (const auto &[value, source_rows] : sources) {
            Pattern p; p.id = set.id + "__derived" + std::to_string(pi);
            p.regexp = set.derived.mode == "literal" ? regex_escape(value) : value;
            p.word_boundary = set.derived.word_boundary;
            pats.push_back(std::move(p)); map.push_back({si, pi++, value, source_rows});
        }
        if (!pats.empty() && !scan_stage({si}, std::move(pats), std::move(map), set)) return 2;
    }
    if (hard_limit) {
        std::fprintf(stderr, "hprscript: query resource limit reached: %s\n", stats.stop_reason.c_str());
        return 2;
    }
    if ((stats.files_failed || stats.missing_paths) && stats.stop_reason.empty())
        stats.stop_reason = "input_failure";

    std::vector<JoinedRow> joined;
    joined.reserve(rows[from_set].size());
    uint64_t ordinal = 0;
    for (const auto &r : rows[from_set]) joined.push_back({{{from_alias, &r}}, ordinal++});
    uint64_t query_max_rows = doc.limits.max_join_rows;
    if (const auto *v = doc.query.find("max_rows"))
        query_max_rows = static_cast<uint64_t>(v->as_number());
    auto enforce_query_rows = [&](std::vector<JoinedRow> &values) -> bool {
        if (values.size() <= query_max_rows) return true;
        if (!doc.partial_on_limit) {
            std::fprintf(stderr, "hprscript: query max_rows reached\n");
            return false;
        }
        stats.rows_truncated += values.size() - query_max_rows;
        values.resize(query_max_rows);
        stats.stop_reason = "query_max_rows";
        return true;
    };
    if (!enforce_query_rows(joined)) return 2;
    if (joins) for (const auto &j : *joins) {
        const std::string type = j.find("type")->as_string();
        const std::string alias = j.find("as")->as_string();
        size_t set_idx = 0; find_set(doc, j.find("set")->as_string(), &set_idx);
        const auto &right_rows = rows[set_idx];
        const auto &preds = j.find("on")->as_array();
        std::vector<std::pair<std::string,std::string>> pairs;
        const bool hash = equality_pairs(preds, alias, pairs);
        std::unordered_multimap<std::string, const MatchRow *> index;
        if (hash) {
            for (const auto &r : right_rows) {
                JoinedRow one{{{alias, &r}}, 0};
                index.emplace(join_hash_key(pairs, one, true), &r);
            }
        }
        std::string location_peer; bool scope_partition = false;
        const bool location = location_strategy(preds, alias, location_peer,
                                                scope_partition);
        std::map<std::string, std::vector<const MatchRow *>> interval_by_file;
        auto location_key = [&](const MatchRow &row) {
            if (!scope_partition) return row.file;
            if (!row.enclosing) return std::string();
            return row.file + "\n" + std::to_string(row.enclosing->from) + ":" +
                   std::to_string(row.enclosing->to);
        };
        if (!hash && location) {
            for (const auto &r : right_rows) {
                std::string key = location_key(r);
                if (!key.empty()) interval_by_file[key].push_back(&r);
            }
            for (auto &[file, values] : interval_by_file)
                std::sort(values.begin(), values.end(), [](const MatchRow *a, const MatchRow *b) {
                    if (a->from != b->from) return a->from < b->from;
                    return a->to < b->to;
                });
        }
        std::vector<JoinedRow> next;
        for (const auto &left : joined) {
            std::vector<const MatchRow *> candidates;
            if (hash) {
                auto range = index.equal_range(join_hash_key(pairs, left, false));
                for (auto it = range.first; it != range.second; ++it) candidates.push_back(it->second);
            } else if (location) {
                auto peer = left.aliases.find(location_peer);
                if (peer != left.aliases.end() && peer->second) {
                    auto file = interval_by_file.find(location_key(*peer->second));
                    if (file != interval_by_file.end()) candidates = file->second;
                }
            } else for (const auto &r : right_rows) candidates.push_back(&r);
            bool matched = false;
            for (const MatchRow *right : candidates) {
                JoinedRow combined = left; combined.aliases[alias] = right;
                if (!all_join_preds(preds, combined)) continue;
                matched = true;
                if (type == "semi") break;
                if (type == "anti") continue;
                combined.ordinal = ordinal++; next.push_back(std::move(combined));
                if (next.size() > doc.limits.max_join_rows) break;
            }
            if (type == "semi" && matched) next.push_back(left);
            else if (type == "anti" && !matched) next.push_back(left);
            else if (type == "left" && !matched) {
                JoinedRow empty = left; empty.aliases[alias] = nullptr; next.push_back(std::move(empty));
            }
            if (next.size() > doc.limits.max_join_rows) break;
        }
        if (next.size() > doc.limits.max_join_rows) {
            if (!doc.partial_on_limit) {
                std::fprintf(stderr, "hprscript: query join row limit reached\n"); return 2;
            }
            next.resize(doc.limits.max_join_rows); stats.stop_reason = "query_join_limit";
            ++stats.rows_truncated;
        }
        joined.swap(next);
        if (!enforce_query_rows(joined)) return 2;
    }
    if (const auto *where = doc.query.find("where")) {
        joined.erase(std::remove_if(joined.begin(), joined.end(),
            [&](const JoinedRow &r) { return !eval_pred(*where, r); }), joined.end());
    }

    const auto &select = doc.query.find("select")->as_object();
    bool has_aggregate = false;
    for (const auto &[name, expr] : select) if (aggregate_expr(expr)) has_aggregate = true;
    json::Array group_fields;
    if (const auto *group = doc.query.find("group_by")) {
        if (!group->is_array()) { std::fprintf(stderr, "hprscript: query.group_by must be array\n"); return 2; }
        group_fields = group->as_array();
    }
    std::vector<Projected> output;
    if (!group_fields.empty() || has_aggregate) {
        std::map<std::string, std::vector<const JoinedRow *>> groups;
        for (const auto &row : joined) groups[row_group_key(group_fields, row)].push_back(&row);
        if (joined.empty() && has_aggregate && group_fields.empty()) groups[""] = {};
        for (const auto &[key, members] : groups) {
            Projected projected;
            JoinedRow empty;
            const JoinedRow &first = members.empty() ? empty : *members.front();
            for (const auto &[name, expr] : select) {
                std::string op; const json::Value *arg = nullptr;
                projected[name] = aggregate_expr(expr, &op, &arg)
                    ? aggregate(op, *arg, members) : eval_expr(expr, first);
            }
            if (const auto *having = doc.query.find("having"))
                if (!eval_pred(*having, first, &projected)) continue;
            output.push_back(std::move(projected));
        }
    } else {
        for (const auto &row : joined) {
            Projected projected;
            for (const auto &[name, expr] : select) projected[name] = eval_expr(expr, row);
            output.push_back(std::move(projected));
        }
    }

    if (const auto *order = doc.query.find("order_by")) {
        if (!order->is_array()) { std::fprintf(stderr, "hprscript: query.order_by must be array\n"); return 2; }
        std::stable_sort(output.begin(), output.end(), [&](const Projected &a, const Projected &b) {
            for (const auto &ov : order->as_array()) {
                if (!ov.is_object()) continue;
                const auto *field = ov.find("field"), *dir = ov.find("direction");
                if (!field || !field->is_string()) continue;
                RuntimeValue av = a.count(field->as_string()) ? a.at(field->as_string()) : RuntimeValue::make_null();
                RuntimeValue bv = b.count(field->as_string()) ? b.at(field->as_string()) : RuntimeValue::make_null();
                int cmp = compare_values(av, bv);
                if (cmp) return dir && dir->is_string() && dir->as_string() == "desc" ? cmp > 0 : cmp < 0;
            }
            return projected_json(a) < projected_json(b);
        });
    }
    uint64_t skip = 0, semantic_limit = UINT64_MAX, max_output = UINT64_MAX;
    if (const auto *v = doc.query.find("skip")) {
        if (!v->is_number() || v->as_number() < 0) { std::fprintf(stderr, "hprscript: query.skip must be non-negative\n"); return 2; }
        skip = v->as_int();
    }
    if (const auto *v = doc.query.find("limit")) {
        if (!v->is_number() || v->as_number() < 0) { std::fprintf(stderr, "hprscript: query.limit must be non-negative\n"); return 2; }
        semantic_limit = v->as_int();
    }
    if (const auto *v = doc.query.find("max_output_rows")) {
        if (!v->is_number() || v->as_number() < 0) { std::fprintf(stderr, "hprscript: query.max_output_rows must be non-negative\n"); return 2; }
        max_output = v->as_int();
    }
    const size_t begin = std::min<uint64_t>(skip, output.size());
    size_t end = std::min<uint64_t>(output.size(), begin + semantic_limit);
    const size_t desired_end = end;
    end = std::min<uint64_t>(end, begin + max_output);
    if (end < desired_end) {
        stats.rows_truncated += desired_end - end; stats.stop_reason = "max_output_rows";
    }
    const bool llm = cli.out_mode == OutputMode::Llm;
    if (llm && begin < end) {
        size_t n = 0; for (const auto &[name,v] : output[begin]) { if (n++) std::putchar('\t'); std::fputs(name.c_str(), stdout); }
        std::putchar('\n');
    }
    for (size_t i = begin; i < end; ++i) {
        if (llm) {
            size_t n = 0; for (const auto &[name,v] : output[i]) {
                if (n++) std::putchar('\t');
                std::string cell = normalized_cell(v.to_str());
                std::fwrite(cell.data(), 1, cell.size(), stdout);
            }
            std::putchar('\n');
        } else {
            std::string line = projected_json(output[i]); std::fwrite(line.data(), 1, line.size(), stdout);
        }
        ++stats.rows_output;
    }
    if (llm && end < desired_end)
        std::printf("... %zu rows omitted (max_output_rows)\n", desired_end - end);
    if (!stats.complete() && !cli.summary) {
        if (llm) {
            std::printf("QUERY INCOMPLETE reason=%s rows_omitted=%llu\n",
                        stats.stop_reason.c_str(),
                        static_cast<unsigned long long>(stats.rows_truncated));
        } else {
            std::string footer = "{\"type\":\"query-footer\",\"complete\":false,\"stop_reason\":\"";
            json_escape_to(footer, stats.stop_reason);
            footer += "\",\"rows_truncated\":" + std::to_string(stats.rows_truncated) + "}\n";
            std::fwrite(footer.data(), 1, footer.size(), stdout);
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (cli.summary) emit_summary_record(stats, stats.rows_output, elapsed);
    if (cli.require_complete && !stats.complete()) return 2;
    return output.empty() ? 1 : 0;
}

} // namespace hpr
