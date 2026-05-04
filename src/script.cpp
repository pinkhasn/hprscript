// Script-mode (-s / -script) interpreter.
//
// Supports the script DSL feature set that maps cleanly onto Hyperscan's
// PCRE engine: typed variables, conditions and `if`, list/map ops,
// `for_each`, `submatch`, `block`, `lookup`, file/complete lifecycle hooks,
// `group_by`, `rank` (with weights), `skip`, `phases`, and absent patterns.
//
// Out-of-scope features (rejected with a clear error so users notice):
//   - file modification (`replace*`, `--write`, `--backup`)
//   - per-file `files[]` with `at`/`from`/`extract`
//   - `boundary`/`on_boundary`, `ascii_only`, `overlap`
//   - `run_pattern_at` / `run_pattern_from` / `run_pattern_to` /
//     `run_pattern_until`, per-pattern `pcre`
//
// File layout:
//   1. Runtime types (Action, Condition, CompiledPattern, ScriptState, ExecCtx)
//   2. Compile pass: JSON → typed AST + Hyperscan databases
//   3. Substitution helpers (`$VAR` resolution to RuntimeValue / string)
//   4. Action interpreter and condition evaluator
//   5. Per-file scan loop, phase orchestration, and `run_script` entry point.

#include "script.hpp"

#include "block.hpp"
#include "common.hpp"
#include "file_io.hpp"
#include "json.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "scope.hpp"
#include "value.hpp"
#include "walker.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpr {

namespace {

// ---- Forward declarations ---------------------------------------------------

struct Action;
struct CompiledPattern;
struct SubmatchInfo;
struct ExecCtx;
struct ScriptState;

// UTF-8-safe byte-budget truncation. limit==0 means no cap. Returns true when
// the string was shortened. Backs off to the last full codepoint boundary so
// JSON output never carries a half-codepoint.
bool truncate_to_bytes(std::string &s, uint64_t limit) {
    if (limit == 0 || s.size() <= limit) return false;
    size_t n = static_cast<size_t>(limit);
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return true;
}
struct CompiledPhase;

// ---- Small helpers ----------------------------------------------------------

bool slurp_file_to_string(const std::string &path, std::string &out) {
    MappedFile mf;
    if (!mf.open(path)) return false;
    out.assign(mf.view().data(), mf.view().size());
    return true;
}

bool slurp_stdin(std::string &out) { return read_stdin(out); }

std::string i64_str(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
}
std::string u64_str(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return buf;
}

// ---- Conditions -------------------------------------------------------------

enum class CondOp {
    Eq, Ne, Gt, Lt, Gte, Lte,
    And, Or, Not,
    Contains, IsSet,
};

struct Condition {
    CondOp op = CondOp::Eq;
    // For Eq/Ne/Gt/Lt/Gte/Lte/Contains: two value args (raw json::Value with
    // $-substitution applied at evaluation time).
    json::Value arg_a;
    json::Value arg_b;
    // For IsSet: variable name (no $ prefix).
    std::string var_name;
    // For And/Or/Not: nested conditions.
    std::vector<std::shared_ptr<Condition>> sub;
};

// ---- Actions ----------------------------------------------------------------

enum class ActionKind {
    Emit, Print,
    Set, Increment, Decrement, Add, Subtract, Multiply, Divide, Reset,
    Append, Collect, UniqueAppend, Sort,
    MapSet, MapIncrement, Count,
    If, ForEach, Stop,
    Submatch, Block, Lookup,
};

struct Action {
    ActionKind kind = ActionKind::Emit;

    // Emit / Print payload.
    json::Value data;       // emit's data object (or null = default record)
    bool has_data = false;
    json::Value value;      // print's value, or generic value param
    bool has_value = false;

    // Variable manipulation: target name (without $ prefix).
    std::string var;
    std::string target;
    std::string key;
    std::vector<std::string> vars; // for `reset`

    // Sort.
    std::string sort_key;
    bool sort_desc = false;

    // For-each.
    std::string as_var;
    std::string key_as_var;

    // Block.
    std::string block_open;
    std::string block_close;

    // If condition.
    std::shared_ptr<Condition> cond;

    // Sub-action lists.
    std::vector<Action> branch_a;   // if.then / for_each.do / block.on_block / lookup.on_hit
    std::vector<Action> branch_b;   // if.else / lookup.on_miss

    // Submatch.
    std::shared_ptr<SubmatchInfo> submatch;

    // Lookup.
    std::string lookup_map;
};

// ---- Compiled pattern -------------------------------------------------------

struct CompiledPattern {
    Pattern pat;
    std::vector<Action> on_match;
    bool absent = false;
};

// ---- Submatch info ----------------------------------------------------------

struct SubmatchInfo {
    std::vector<CompiledPattern> patterns;
    std::shared_ptr<Matcher> matcher;
    // Source text for the submatch. Valid identifiers (recognised at exec):
    //   ""           → use $MATCH (default)
    //   "$MATCH"     → match text
    //   "$BLOCK"     → block content (requires running inside an on_block)
    //   "$BLOCK_FULL"→ match-start to block-end
    //   "$CONTEXT"   → the line(s) of the match
    // Anything else falls back to substituted-literal — text has no clear
    // file-relative offset, so sub-matches report pseudo-offsets within the
    // text (line counted relative to outer match's line).
    std::string text_source;
};

// ---- Variable store ---------------------------------------------------------

enum class VarType { Str, Int, Bool, List, Map };

struct VarSpec {
    VarType type = VarType::Str;
    RuntimeValue default_value;
};

class VarStore {
public:
    void declare(const std::string &name, VarType t, RuntimeValue dflt) {
        specs_[name] = VarSpec{t, dflt};
        values_[name] = dflt;
    }
    bool exists(const std::string &name) const { return values_.count(name) > 0; }
    RuntimeValue *find(const std::string &name) {
        auto it = values_.find(name);
        return it == values_.end() ? nullptr : &it->second;
    }
    const RuntimeValue *find(const std::string &name) const {
        auto it = values_.find(name);
        return it == values_.end() ? nullptr : &it->second;
    }
    // Set; if not declared, declare on the fly (loosely-typed). Useful for
    // for_each iteration variables and other temporaries.
    void set(const std::string &name, RuntimeValue v) {
        if (!specs_.count(name)) {
            VarType t = VarType::Str;
            switch (v.kind()) {
                case RuntimeValue::Bool: t = VarType::Bool; break;
                case RuntimeValue::Int:  t = VarType::Int;  break;
                case RuntimeValue::List: t = VarType::List; break;
                case RuntimeValue::Map:  t = VarType::Map;  break;
                default: t = VarType::Str;
            }
            specs_[name] = VarSpec{t, v};
        }
        values_[name] = std::move(v);
    }
    void reset_all() {
        for (auto &kv : specs_) values_[kv.first] = kv.second.default_value;
    }
    void reset(const std::string &name) {
        auto it = specs_.find(name);
        if (it != specs_.end()) values_[name] = it->second.default_value;
    }
    const std::map<std::string, VarSpec> &specs() const { return specs_; }

private:
    std::map<std::string, VarSpec> specs_;
    std::map<std::string, RuntimeValue> values_;
};

// ---- Per-file rank info -----------------------------------------------------

struct FileRank {
    std::set<std::string> matched_pat_ids;
    double raw_score = 0.0;        // Σ weight of distinct matched pattern IDs.
    uint32_t line_count = 1;
    // Per-file pattern-id → local index (0..N-1) for compact match_points.
    std::map<std::string, uint16_t> pat_local_ids;
    // Every match recorded as (1-based line, pat_local_id) for proximity sweep.
    std::vector<std::pair<uint32_t, uint16_t>> match_points;
};

// ---- Script-wide state ------------------------------------------------------

struct ScriptState {
    VarStore vars;

    // Skip/limit accounting.
    int64_t skip_n = 0;
    int64_t limit = -1;
    int64_t limit_per_file = -1;
    int64_t matched_emits = 0;     // total emit/print calls (including skipped)
    int64_t emitted = 0;            // emits that actually produced output
    int64_t per_file_emits = 0;
    bool stop_file = false;
    bool stop_all = false;

    // Byte budgets (0 = unlimited). Truncation applies in populate_match_ctx
    // and the `block` action; output cap is checked in emit_record_string.
    uint64_t max_match_bytes = 0;
    uint64_t max_context_bytes = 0;
    uint64_t max_block_bytes = 0;
    uint64_t max_output_bytes = 0;
    uint64_t bytes_written = 0;
    bool output_truncated = false;

    // Enclosing-scope detection (script-level). When `scope_lang` is non-empty
    // (e.g. "go", "auto") OR `scope_custom` has a complete pattern+open+close,
    // a per-file ScopeIndex is built and threaded through ExecCtx.
    std::string scope_lang;
    ScopeConfig scope_custom;

    // -near / -far relations parsed from top-level `relations`.
    struct Relation {
        bool near = true; // false → "far"
        std::string a;
        std::string b;
        int lines = 0;
    };
    std::vector<Relation> relations;

    // group_by buffering.
    std::string group_by;
    std::vector<std::string> group_order;
    std::map<std::string, std::vector<std::string>> group_records;

    // Rank.
    bool rank_enabled = false;
    std::map<std::string, double> pattern_weights;
    // Total non-absent queried pattern IDs across phases (denominator for
    // coverage). Set once, after compile.
    uint32_t rank_total_queried = 0;
    std::vector<std::string> rank_file_order;
    std::map<std::string, FileRank> rank_per_file;
    // Per-current-file pattern-id matches (cleared on file end).
    std::set<std::string> rank_file_pat_ids;
    // While true, per-match output (emit/print) is dropped. Used by `rank`
    // mode so only the per-file rank table is shown — match records are
    // suppressed but side-effect actions (set/increment/...) still run.
    bool suppress_records = false;
};

// ---- Execution context ------------------------------------------------------

struct ExecCtx {
    std::string file;
    std::string pat_id;
    std::string match_text;
    std::string context_text;
    std::string context_before_text;
    std::string context_after_text;
    uint32_t line = 0;
    uint32_t col = 0;
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t word_no = 0;
    uint32_t sentence_no = 0;
    bool line_available = true;
    bool has_match = true;

    // Truncation flags (set when text fields above were cut to fit byte
    // budgets — see ScriptState::max_*_bytes).
    bool match_truncated = false;
    bool context_truncated = false;

    // Block.
    bool has_block = false;
    std::string block_text;
    std::string block_full_text;
    uint64_t block_start = 0;
    uint64_t block_end = 0;
    uint32_t block_line_start = 0;
    uint32_t block_line_end = 0;
    bool block_truncated = false;
    bool block_full_truncated = false;

    // Lookup.
    bool has_lookup = false;
    std::string lookup_key;
    RuntimeValue lookup_value;

    // Refs to environment.
    VarStore *vars = nullptr;
    LineIndex *line_idx = nullptr;
    std::string_view file_buf;
    ScriptState *state = nullptr;

    // Active phase's capture-group extractor (may be null).
    const ExtractTable *extract_table = nullptr;
    uint32_t pat_index = 0; // for ExtractTable lookups

    // Active file's scope index (may be null when scope detection is off or
    // no scopes were found).
    const ScopeIndex *scope = nullptr;
    const ScopeRange *enclosing = nullptr; // resolved per match
};

// ---- Forward decls for mutual recursion -------------------------------------

void execute_actions(const std::vector<Action> &actions, ExecCtx &ctx);
void execute_action(const Action &a, ExecCtx &ctx);
bool eval_condition(const Condition &c, const ExecCtx &ctx);
RuntimeValue resolve_value(const json::Value &v, const ExecCtx &ctx);
std::string substitute(const std::string &tmpl, const ExecCtx &ctx);
std::string render_data_to_json(const json::Value &v, const ExecCtx &ctx);
RuntimeValue build_default_record(const ExecCtx &ctx);
void emit_record_string(const std::string &json_line, const ExecCtx &ctx,
                        const std::string &group_key);

// Returns true if the named token is a built-in; if so, writes its string
// representation to `out`. (Built-in vs user variable disambiguation lives
// here so substitute() and resolve_value() share the same recogniser.)
bool builtin_token_str(const std::string &name, const ExecCtx &ctx,
                       std::string &out) {
    if (name == "FILE") { out = ctx.file; return true; }
    if (name == "PAT_ID") { out = ctx.pat_id; return true; }
    if (name == "MATCH") { out = ctx.match_text; return true; }
    if (name == "CONTEXT") { out = ctx.context_text; return true; }
    if (name == "CONTEXT_BEFORE") { out = ctx.context_before_text; return true; }
    if (name == "CONTEXT_AFTER") { out = ctx.context_after_text; return true; }
    if (name == "LINE") {
        out = ctx.line_available ? i64_str((int64_t)ctx.line) : "0"; return true;
    }
    if (name == "COL") {
        out = ctx.line_available ? i64_str((int64_t)ctx.col) : "0"; return true;
    }
    if (name == "FROM") { out = u64_str(ctx.from); return true; }
    if (name == "TO")   { out = u64_str(ctx.to); return true; }
    if (name == "WORD") { out = u64_str(ctx.word_no); return true; }
    if (name == "SENTENCE") { out = u64_str(ctx.sentence_no); return true; }
    if (name == "BLOCK") {
        out = ctx.has_block ? ctx.block_text : "";
        return true;
    }
    if (name == "BLOCK_FULL") {
        out = ctx.has_block ? ctx.block_full_text : "";
        return true;
    }
    if (name == "BLOCK_START") {
        out = ctx.has_block ? u64_str(ctx.block_start) : "0"; return true;
    }
    if (name == "BLOCK_END") {
        out = ctx.has_block ? u64_str(ctx.block_end) : "0"; return true;
    }
    if (name == "BLOCK_LINE_START") {
        out = ctx.has_block ? u64_str(ctx.block_line_start) : "0"; return true;
    }
    if (name == "BLOCK_LINE_END") {
        out = ctx.has_block ? u64_str(ctx.block_line_end) : "0"; return true;
    }
    if (name == "LOOKUP_KEY") { out = ctx.lookup_key; return true; }
    if (name == "LOOKUP_VALUE") {
        out = ctx.has_lookup ? ctx.lookup_value.to_str() : "";
        return true;
    }
    if (ctx.extract_table && name.size() > 8 &&
        name.compare(0, 8, "EXTRACT_") == 0) {
        const auto &names = ctx.extract_table->names(ctx.pat_index);
        std::string suffix = name.substr(8);
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].size() != suffix.size()) continue;
            bool eq_i = true;
            for (size_t j = 0; j < suffix.size(); ++j) {
                char c1 = std::toupper((unsigned char)names[i][j]);
                char c2 = std::toupper((unsigned char)suffix[j]);
                if (c1 != c2) { eq_i = false; break; }
            }
            if (!eq_i) continue;
            std::vector<std::string> values;
            ctx.extract_table->extract(ctx.pat_index, ctx.match_text, values);
            out = i < values.size() ? values[i] : "";
            return true;
        }
    }
    if (name == "ENCLOSING_NAME") { out = ctx.enclosing ? ctx.enclosing->name : ""; return true; }
    if (name == "ENCLOSING_KIND") { out = ctx.enclosing ? ctx.enclosing->kind : ""; return true; }
    if (name == "ENCLOSING_LINE_START") {
        out = ctx.enclosing ? u64_str((uint64_t)ctx.enclosing->line_start) : "0";
        return true;
    }
    if (name == "ENCLOSING_LINE_END") {
        out = ctx.enclosing ? u64_str((uint64_t)ctx.enclosing->line_end) : "0";
        return true;
    }
    return false;
}

// Same as above, but resolves to a typed RuntimeValue. Returns true on a
// recognised built-in (typed so callers can keep numeric semantics).
bool builtin_token_value(const std::string &name, const ExecCtx &ctx,
                         RuntimeValue &out) {
    if (name == "LINE") {
        out = RuntimeValue::make_int(ctx.line_available ? (int64_t)ctx.line : 0);
        return true;
    }
    if (name == "COL") {
        out = RuntimeValue::make_int(ctx.line_available ? (int64_t)ctx.col : 0);
        return true;
    }
    if (name == "FROM") { out = RuntimeValue::make_int((int64_t)ctx.from); return true; }
    if (name == "TO")   { out = RuntimeValue::make_int((int64_t)ctx.to); return true; }
    if (name == "WORD") { out = RuntimeValue::make_int((int64_t)ctx.word_no); return true; }
    if (name == "SENTENCE") { out = RuntimeValue::make_int((int64_t)ctx.sentence_no); return true; }
    if (name == "BLOCK_START") { out = RuntimeValue::make_int((int64_t)ctx.block_start); return true; }
    if (name == "BLOCK_END")   { out = RuntimeValue::make_int((int64_t)ctx.block_end); return true; }
    if (name == "BLOCK_LINE_START") { out = RuntimeValue::make_int((int64_t)ctx.block_line_start); return true; }
    if (name == "BLOCK_LINE_END")   { out = RuntimeValue::make_int((int64_t)ctx.block_line_end); return true; }
    if (name == "FILE") { out = RuntimeValue::make_str(ctx.file); return true; }
    if (name == "PAT_ID") { out = RuntimeValue::make_str(ctx.pat_id); return true; }
    if (name == "MATCH") { out = RuntimeValue::make_str(ctx.match_text); return true; }
    if (name == "CONTEXT") { out = RuntimeValue::make_str(ctx.context_text); return true; }
    if (name == "CONTEXT_BEFORE") { out = RuntimeValue::make_str(ctx.context_before_text); return true; }
    if (name == "CONTEXT_AFTER") { out = RuntimeValue::make_str(ctx.context_after_text); return true; }
    if (name == "BLOCK") { out = RuntimeValue::make_str(ctx.has_block ? ctx.block_text : std::string()); return true; }
    if (name == "BLOCK_FULL") { out = RuntimeValue::make_str(ctx.has_block ? ctx.block_full_text : std::string()); return true; }
    if (name == "LOOKUP_KEY") { out = RuntimeValue::make_str(ctx.lookup_key); return true; }
    if (name == "LOOKUP_VALUE") {
        out = ctx.has_lookup ? ctx.lookup_value : RuntimeValue::make_null();
        return true;
    }
    if (ctx.extract_table && name.size() > 8 &&
        name.compare(0, 8, "EXTRACT_") == 0) {
        const auto &names = ctx.extract_table->names(ctx.pat_index);
        std::string suffix = name.substr(8);
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].size() != suffix.size()) continue;
            bool eq_i = true;
            for (size_t j = 0; j < suffix.size(); ++j) {
                char c1 = std::toupper((unsigned char)names[i][j]);
                char c2 = std::toupper((unsigned char)suffix[j]);
                if (c1 != c2) { eq_i = false; break; }
            }
            if (!eq_i) continue;
            std::vector<std::string> values;
            ctx.extract_table->extract(ctx.pat_index, ctx.match_text, values);
            out = RuntimeValue::make_str(i < values.size() ? values[i] : std::string());
            return true;
        }
    }
    if (name == "ENCLOSING_NAME") {
        out = RuntimeValue::make_str(ctx.enclosing ? ctx.enclosing->name : std::string());
        return true;
    }
    if (name == "ENCLOSING_KIND") {
        out = RuntimeValue::make_str(ctx.enclosing ? ctx.enclosing->kind : std::string());
        return true;
    }
    if (name == "ENCLOSING_LINE_START") {
        out = RuntimeValue::make_int(ctx.enclosing ? (int64_t)ctx.enclosing->line_start : 0);
        return true;
    }
    if (name == "ENCLOSING_LINE_END") {
        out = RuntimeValue::make_int(ctx.enclosing ? (int64_t)ctx.enclosing->line_end : 0);
        return true;
    }
    return false;
}

// Identifier characters allowed after `$`.
bool is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_id_cont(char c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}

// Substitute every `$IDENT` in `tmpl` with its string value (built-in or
// user variable). Unknown names are left as `$NAME` so they're visible in
// output and easy to debug.
std::string substitute(const std::string &tmpl, const ExecCtx &ctx) {
    std::string out;
    out.reserve(tmpl.size());
    size_t i = 0;
    while (i < tmpl.size()) {
        char c = tmpl[i];
        if (c != '$' || i + 1 >= tmpl.size() || !is_id_start(tmpl[i + 1])) {
            out += c;
            ++i;
            continue;
        }
        size_t start = i + 1;
        size_t end = start;
        while (end < tmpl.size() && is_id_cont(tmpl[end])) ++end;
        std::string name(tmpl, start, end - start);
        std::string sub;
        if (builtin_token_str(name, ctx, sub)) {
            out += sub;
        } else if (ctx.vars && ctx.vars->find(name)) {
            out += ctx.vars->find(name)->to_str();
        } else {
            // Unknown — leave the literal `$NAME` in place.
            out += '$';
            out += name;
        }
        i = end;
    }
    return out;
}

// Resolve a raw json::Value (from script JSON) to a RuntimeValue, applying
// $-substitution. If the entire input string is `$NAME` and NAME is a known
// variable, the variable's native type is returned. Otherwise strings are
// substituted as templates.
RuntimeValue resolve_value(const json::Value &v, const ExecCtx &ctx) {
    switch (v.type()) {
        case json::Value::Null: return RuntimeValue::make_null();
        case json::Value::Bool: return RuntimeValue::make_bool(v.as_bool());
        case json::Value::Number: {
            double d = v.as_number();
            if (d == (double)(int64_t)d) return RuntimeValue::make_int((int64_t)d);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            return RuntimeValue::make_str(buf);
        }
        case json::Value::String: {
            const std::string &s = v.as_string();
            // "$NAME" alone → return native variable type.
            if (s.size() >= 2 && s[0] == '$' && is_id_start(s[1])) {
                size_t end = 1;
                while (end < s.size() && is_id_cont(s[end])) ++end;
                if (end == s.size()) {
                    std::string name(s, 1, end - 1);
                    RuntimeValue rv;
                    if (builtin_token_value(name, ctx, rv)) return rv;
                    if (ctx.vars && ctx.vars->find(name)) {
                        return *ctx.vars->find(name);
                    }
                    return RuntimeValue::make_str("");
                }
            }
            return RuntimeValue::make_str(substitute(s, ctx));
        }
        case json::Value::ArrayT: {
            RuntimeValue rv = RuntimeValue::make_list();
            for (const auto &e : v.as_array()) rv.as_list().push_back(resolve_value(e, ctx));
            return rv;
        }
        case json::Value::ObjectT: {
            RuntimeValue rv = RuntimeValue::make_map();
            for (const auto &kv : v.as_object()) {
                rv.as_map().emplace(kv.first, resolve_value(kv.second, ctx));
            }
            return rv;
        }
    }
    return RuntimeValue::make_null();
}

// Render an emit `data` JSON tree to a JSON Lines string, applying
// substitution to string leaves while preserving non-string types.
std::string render_data_to_json(const json::Value &v, const ExecCtx &ctx) {
    RuntimeValue rv = resolve_value(v, ctx);
    return rv.to_json();
}

// Build the default match record (used by `emit` without `data` and
// `append`/`collect` without `value`).
RuntimeValue build_default_record(const ExecCtx &ctx) {
    RuntimeValue rec = RuntimeValue::make_map();
    auto &m = rec.as_map();
    m["file"] = RuntimeValue::make_str(ctx.file);
    if (!ctx.pat_id.empty()) m["pat"] = RuntimeValue::make_str(ctx.pat_id);
    if (ctx.has_match) {
        if (ctx.line_available) {
            m["line"] = RuntimeValue::make_int((int64_t)ctx.line);
            m["col"] = RuntimeValue::make_int((int64_t)ctx.col);
        }
        m["from"] = RuntimeValue::make_int((int64_t)ctx.from);
        m["to"] = RuntimeValue::make_int((int64_t)ctx.to);
        m["match"] = RuntimeValue::make_str(ctx.match_text);
        m["context"] = RuntimeValue::make_str(ctx.context_text);
        if (!ctx.context_before_text.empty())
            m["context_before"] = RuntimeValue::make_str(ctx.context_before_text);
        if (!ctx.context_after_text.empty())
            m["context_after"] = RuntimeValue::make_str(ctx.context_after_text);
    }
    if (ctx.has_block) {
        m["block"] = RuntimeValue::make_str(ctx.block_text);
        m["block_full"] = RuntimeValue::make_str(ctx.block_full_text);
        m["block_start"] = RuntimeValue::make_int((int64_t)ctx.block_start);
        m["block_end"] = RuntimeValue::make_int((int64_t)ctx.block_end);
        if (ctx.line_available) {
            m["block_line_start"] = RuntimeValue::make_int((int64_t)ctx.block_line_start);
            m["block_line_end"] = RuntimeValue::make_int((int64_t)ctx.block_line_end);
        }
    }
    bool any_trunc = ctx.match_truncated || ctx.context_truncated
                     || ctx.block_truncated || ctx.block_full_truncated;
    if (ctx.match_truncated)       m["match_truncated"]      = RuntimeValue::make_bool(true);
    if (ctx.context_truncated)     m["context_truncated"]    = RuntimeValue::make_bool(true);
    if (ctx.block_truncated)       m["block_truncated"]      = RuntimeValue::make_bool(true);
    if (ctx.block_full_truncated)  m["block_full_truncated"] = RuntimeValue::make_bool(true);
    if (any_trunc)                 m["truncated"]            = RuntimeValue::make_bool(true);
    if (ctx.has_match && ctx.extract_table &&
        ctx.extract_table->has(ctx.pat_index)) {
        std::vector<std::string> values;
        ctx.extract_table->extract(ctx.pat_index, ctx.match_text, values);
        const auto &names = ctx.extract_table->names(ctx.pat_index);
        RuntimeValue ex = RuntimeValue::make_map();
        auto &emap = ex.as_map();
        for (size_t i = 0; i < names.size(); ++i) {
            emap[names[i]] = RuntimeValue::make_str(i < values.size() ? values[i] : "");
        }
        m["extracted"] = std::move(ex);
    }
    if (ctx.enclosing) {
        RuntimeValue enc = RuntimeValue::make_map();
        auto &emap = enc.as_map();
        emap["name"]       = RuntimeValue::make_str(ctx.enclosing->name);
        emap["kind"]       = RuntimeValue::make_str(ctx.enclosing->kind);
        emap["line_start"] = RuntimeValue::make_int((int64_t)ctx.enclosing->line_start);
        emap["line_end"]   = RuntimeValue::make_int((int64_t)ctx.enclosing->line_end);
        m["enclosing"] = std::move(enc);
    }
    return rec;
}

// Pull the value of a record's group_by key (default: "" if missing).
std::string extract_group_key(const std::string &json_line, const std::string &field) {
    // The line is a single-line JSON object. Find `"field":` and capture
    // the next JSON literal. This is best-effort — anything sufficiently
    // exotic in the value gets stringified verbatim.
    std::string needle = "\"" + field + "\":";
    size_t p = json_line.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    while (p < json_line.size() && json_line[p] == ' ') ++p;
    if (p >= json_line.size()) return "";
    if (json_line[p] == '"') {
        size_t end = p + 1;
        while (end < json_line.size()) {
            if (json_line[end] == '\\') {
                if (end + 2 > json_line.size()) { end = json_line.size(); break; }
                end += 2;
                continue;
            }
            if (json_line[end] == '"') break;
            ++end;
        }
        return json_line.substr(p + 1, end - p - 1);
    }
    size_t end = p;
    while (end < json_line.size() && json_line[end] != ',' && json_line[end] != '}'
           && json_line[end] != ' ' && json_line[end] != '\n')
        ++end;
    return json_line.substr(p, end - p);
}

// Final emit: respects skip/limit, buffers if group_by is active, otherwise
// writes straight to stdout. Caller passes the already-stringified record.
void emit_record_string(const std::string &json_line, const ExecCtx &ctx,
                        const std::string &group_key_hint) {
    ScriptState &st = *ctx.state;
    if (st.suppress_records) return;
    // Per-file cap stops the per-file scan, and treats already-skipped
    // records as part of the cap.
    if (st.limit_per_file > 0 && st.per_file_emits >= st.limit_per_file) {
        st.stop_file = true;
        return;
    }
    ++st.matched_emits;
    if (st.skip_n > 0 && st.matched_emits <= st.skip_n) {
        // Counted against limit but no output.
        if (st.limit > 0 && st.matched_emits >= st.skip_n + st.limit) {
            st.stop_all = true;
        }
        return;
    }
    if (st.limit > 0 && st.emitted >= st.limit) {
        st.stop_all = true;
        return;
    }
    ++st.emitted;
    ++st.per_file_emits;

    if (!st.group_by.empty()) {
        std::string key = group_key_hint.empty()
                              ? extract_group_key(json_line, st.group_by)
                              : group_key_hint;
        if (st.group_records.find(key) == st.group_records.end())
            st.group_order.push_back(key);
        st.group_records[key].push_back(json_line);
        st.bytes_written += json_line.size() + 1; // approx (flushed later)
    } else {
        std::fwrite(json_line.data(), 1, json_line.size(), stdout);
        std::fputc('\n', stdout);
        st.bytes_written += json_line.size() + 1;
    }
    if (st.max_output_bytes > 0 && st.bytes_written >= st.max_output_bytes) {
        st.output_truncated = true;
        st.stop_all = true;
    }
    if (st.limit > 0 && st.emitted >= st.limit) st.stop_all = true;
    if (st.limit_per_file > 0 && st.per_file_emits >= st.limit_per_file)
        st.stop_file = true;
}

// ---- Compile ----------------------------------------------------------------

bool compile_condition(const json::Value &cv, std::shared_ptr<Condition> &out,
                       std::string &err) {
    if (!cv.is_object()) { err = "condition must be an object"; return false; }
    const json::Value *op = cv.find("op");
    const json::Value *args = cv.find("args");
    if (!op || !op->is_string()) { err = "condition missing 'op'"; return false; }
    if (!args || !args->is_array()) { err = "condition missing 'args' array"; return false; }
    auto c = std::make_shared<Condition>();
    const std::string &name = op->as_string();
    if (name == "eq") c->op = CondOp::Eq;
    else if (name == "ne") c->op = CondOp::Ne;
    else if (name == "gt") c->op = CondOp::Gt;
    else if (name == "lt") c->op = CondOp::Lt;
    else if (name == "gte") c->op = CondOp::Gte;
    else if (name == "lte") c->op = CondOp::Lte;
    else if (name == "and") c->op = CondOp::And;
    else if (name == "or")  c->op = CondOp::Or;
    else if (name == "not") c->op = CondOp::Not;
    else if (name == "contains") c->op = CondOp::Contains;
    else if (name == "isset") c->op = CondOp::IsSet;
    else { err = "unknown condition op '" + name + "'"; return false; }
    const auto &a = args->as_array();
    if (c->op == CondOp::And || c->op == CondOp::Or) {
        if (a.empty()) { err = "and/or requires at least one arg"; return false; }
        for (const auto &sub : a) {
            std::shared_ptr<Condition> sc;
            if (!compile_condition(sub, sc, err)) return false;
            c->sub.push_back(sc);
        }
    } else if (c->op == CondOp::Not) {
        if (a.size() != 1) { err = "not requires exactly one arg"; return false; }
        std::shared_ptr<Condition> sc;
        if (!compile_condition(a[0], sc, err)) return false;
        c->sub.push_back(sc);
    } else if (c->op == CondOp::IsSet) {
        if (a.size() != 1 || !a[0].is_string()) {
            err = "isset requires exactly one string arg (variable name)";
            return false;
        }
        // Allow either "name" or "$name".
        std::string n = a[0].as_string();
        if (!n.empty() && n[0] == '$') n.erase(0, 1);
        c->var_name = n;
    } else {
        if (a.size() != 2) { err = "binary op requires 2 args"; return false; }
        c->arg_a = a[0];
        c->arg_b = a[1];
    }
    out = c;
    return true;
}

bool compile_action(const json::Value &av, Action &out, std::string &err);

// Compile an array of action JSON objects into `out`. `where` is used for
// errors only.
bool compile_action_list(const json::Value &arr, std::vector<Action> &out,
                         const std::string &where, std::string &err) {
    if (!arr.is_array()) { err = where + " must be an array"; return false; }
    for (const auto &av : arr.as_array()) {
        Action a;
        std::string e;
        if (!compile_action(av, a, e)) {
            err = where + ": " + e;
            return false;
        }
        out.push_back(std::move(a));
    }
    return true;
}

bool compile_pattern(const json::Value &pv, size_t idx, CompiledPattern &out,
                     std::string &err);

// Build a Hyperscan-backed matcher from a list of compiled patterns. Returns
// nullptr-and-err if compilation fails.
std::shared_ptr<Matcher> build_matcher(const std::vector<CompiledPattern> &cps,
                                       std::string &err) {
    std::vector<Pattern> ps;
    ps.reserve(cps.size());
    for (const auto &c : cps) ps.push_back(c.pat);
    auto m = std::make_shared<Matcher>();
    CompileError ce;
    if (!m->compile(ps, &ce)) {
        err = ce.message;
        return nullptr;
    }
    return m;
}

bool compile_action(const json::Value &av, Action &out, std::string &err) {
    if (!av.is_object()) { err = "action must be an object"; return false; }
    const json::Value *act = av.find("action");
    if (!act || !act->is_string()) { err = "action missing 'action' field"; return false; }
    const std::string &name = act->as_string();

    auto take_str = [&](const char *k, std::string &v) {
        const json::Value *p = av.find(k);
        if (p && p->is_string()) { v = p->as_string(); return true; }
        return false;
    };
    auto take_var = [&](const char *k, std::string &v) {
        // Variable name: accept "x" or "$x".
        if (!take_str(k, v)) return false;
        if (!v.empty() && v[0] == '$') v.erase(0, 1);
        return true;
    };

    if (name == "emit" || name == "print") {
        out.kind = (name == "emit") ? ActionKind::Emit : ActionKind::Print;
        if (const json::Value *d = av.find("data")) {
            out.data = *d;
            out.has_data = true;
        }
        if (const json::Value *vv = av.find("value")) {
            out.value = *vv;
            out.has_value = true;
        }
        return true;
    }
    if (name == "set" || name == "add" || name == "subtract" ||
        name == "multiply" || name == "divide") {
        if (name == "set") out.kind = ActionKind::Set;
        else if (name == "add") out.kind = ActionKind::Add;
        else if (name == "subtract") out.kind = ActionKind::Subtract;
        else if (name == "multiply") out.kind = ActionKind::Multiply;
        else out.kind = ActionKind::Divide;
        if (!take_var("var", out.var)) { err = name + " requires 'var'"; return false; }
        const json::Value *vv = av.find("value");
        if (!vv) { err = name + " requires 'value'"; return false; }
        out.value = *vv;
        out.has_value = true;
        return true;
    }
    if (name == "increment" || name == "decrement") {
        out.kind = (name == "increment") ? ActionKind::Increment : ActionKind::Decrement;
        if (!take_var("var", out.var)) { err = name + " requires 'var'"; return false; }
        return true;
    }
    if (name == "reset") {
        out.kind = ActionKind::Reset;
        const json::Value *vv = av.find("vars");
        if (!vv || !vv->is_array()) { err = "reset requires 'vars' array"; return false; }
        for (const auto &e : vv->as_array()) {
            if (!e.is_string()) { err = "reset 'vars' must be strings"; return false; }
            std::string n = e.as_string();
            if (!n.empty() && n[0] == '$') n.erase(0, 1);
            out.vars.push_back(std::move(n));
        }
        return true;
    }
    if (name == "append" || name == "collect" || name == "unique_append") {
        if (name == "append") out.kind = ActionKind::Append;
        else if (name == "collect") out.kind = ActionKind::Collect;
        else out.kind = ActionKind::UniqueAppend;
        if (!take_var("target", out.target)) {
            err = name + " requires 'target'"; return false;
        }
        if (const json::Value *vv = av.find("value")) {
            out.value = *vv;
            out.has_value = true;
        }
        return true;
    }
    if (name == "sort") {
        out.kind = ActionKind::Sort;
        if (!take_var("var", out.var)) { err = "sort requires 'var'"; return false; }
        if (!take_str("key", out.sort_key)) { err = "sort requires 'key'"; return false; }
        if (const json::Value *vv = av.find("value"); vv && vv->is_string() &&
            vv->as_string() == "desc") {
            out.sort_desc = true;
        }
        return true;
    }
    if (name == "map_set") {
        out.kind = ActionKind::MapSet;
        if (!take_var("target", out.target)) { err = "map_set requires 'target'"; return false; }
        if (!take_str("key", out.key)) { err = "map_set requires 'key'"; return false; }
        const json::Value *vv = av.find("value");
        if (!vv) { err = "map_set requires 'value'"; return false; }
        out.value = *vv;
        out.has_value = true;
        return true;
    }
    if (name == "map_increment") {
        out.kind = ActionKind::MapIncrement;
        if (!take_var("target", out.target)) {
            err = "map_increment requires 'target'"; return false;
        }
        if (!take_str("key", out.key)) {
            err = "map_increment requires 'key'"; return false;
        }
        return true;
    }
    if (name == "count") {
        out.kind = ActionKind::Count;
        // Spec: `var` is the map name; `key` defaults to $PAT_ID.
        if (!take_var("var", out.var)) {
            err = "count requires 'var'"; return false;
        }
        return true;
    }
    if (name == "if") {
        out.kind = ActionKind::If;
        const json::Value *c = av.find("condition");
        if (!c) { err = "if requires 'condition'"; return false; }
        if (!compile_condition(*c, out.cond, err)) return false;
        if (const json::Value *t = av.find("then"); t && t->is_array()) {
            if (!compile_action_list(*t, out.branch_a, "if.then", err)) return false;
        }
        if (const json::Value *e = av.find("else"); e && e->is_array()) {
            if (!compile_action_list(*e, out.branch_b, "if.else", err)) return false;
        }
        return true;
    }
    if (name == "for_each") {
        out.kind = ActionKind::ForEach;
        if (!take_var("var", out.var)) { err = "for_each requires 'var'"; return false; }
        if (!take_str("as", out.as_var)) { err = "for_each requires 'as'"; return false; }
        if (!out.as_var.empty() && out.as_var[0] == '$') out.as_var.erase(0, 1);
        // Optional key_as for maps; defaults to "key".
        std::string ka;
        if (take_str("key_as", ka)) {
            if (!ka.empty() && ka[0] == '$') ka.erase(0, 1);
            out.key_as_var = ka;
        } else {
            out.key_as_var = "key";
        }
        const json::Value *d = av.find("do");
        if (!d || !d->is_array()) { err = "for_each requires 'do' array"; return false; }
        if (!compile_action_list(*d, out.branch_a, "for_each.do", err)) return false;
        return true;
    }
    if (name == "stop") {
        out.kind = ActionKind::Stop;
        return true;
    }
    if (name == "submatch") {
        out.kind = ActionKind::Submatch;
        const json::Value *sp = av.find("patterns");
        if (!sp || !sp->is_array() || sp->as_array().empty()) {
            err = "submatch requires non-empty 'patterns' array";
            return false;
        }
        auto sm = std::make_shared<SubmatchInfo>();
        for (size_t i = 0; i < sp->as_array().size(); ++i) {
            CompiledPattern cp;
            std::string e;
            if (!compile_pattern(sp->as_array()[i], i, cp, e)) {
                err = "submatch pattern: " + e;
                return false;
            }
            sm->patterns.push_back(std::move(cp));
        }
        std::string merr;
        sm->matcher = build_matcher(sm->patterns, merr);
        if (!sm->matcher) {
            err = "submatch compile failed: " + merr;
            return false;
        }
        if (const json::Value *t = av.find("text"); t && t->is_string()) {
            sm->text_source = t->as_string();
        }
        out.submatch = sm;
        return true;
    }
    if (name == "block") {
        out.kind = ActionKind::Block;
        if (!take_str("open", out.block_open)) { err = "block requires 'open'"; return false; }
        if (!take_str("close", out.block_close)) { err = "block requires 'close'"; return false; }
        const json::Value *ob = av.find("on_block");
        if (!ob || !ob->is_array()) {
            err = "block requires 'on_block' array"; return false;
        }
        if (!compile_action_list(*ob, out.branch_a, "block.on_block", err)) return false;
        return true;
    }
    if (name == "lookup") {
        out.kind = ActionKind::Lookup;
        if (!take_var("map", out.lookup_map)) { err = "lookup requires 'map'"; return false; }
        const json::Value *kv = av.find("key");
        if (!kv) { err = "lookup requires 'key'"; return false; }
        out.value = *kv;
        out.has_value = true;
        if (const json::Value *oh = av.find("on_hit"); oh && oh->is_array()) {
            if (!compile_action_list(*oh, out.branch_a, "lookup.on_hit", err)) return false;
        }
        if (const json::Value *om = av.find("on_miss"); om && om->is_array()) {
            if (!compile_action_list(*om, out.branch_b, "lookup.on_miss", err)) return false;
        }
        return true;
    }
    err = "action '" + name + "' not supported";
    return false;
}

bool compile_pattern(const json::Value &pv, size_t idx, CompiledPattern &out,
                     std::string &err) {
    if (!pv.is_object()) { err = "pattern must be an object"; return false; }
    const json::Value *re = pv.find("regexp");
    if (!re || !re->is_string()) { err = "pattern missing 'regexp'"; return false; }

    const json::Value *id = pv.find("id");
    out.pat.id = (id && id->is_string()) ? id->as_string() : "p" + std::to_string(idx);
    out.pat.regexp = re->as_string();

    if (const json::Value *ci = pv.find("case_insensitive"))
        out.pat.case_insensitive = ci->is_bool() && ci->as_bool();
    if (const json::Value *wb = pv.find("word_boundary"))
        out.pat.word_boundary = wb->is_bool() && wb->as_bool();
    if (const json::Value *u8 = pv.find("utf8"))
        out.pat.utf8 = !u8->is_bool() || u8->as_bool();
    if (const json::Value *uc = pv.find("ucp"))
        out.pat.ucp = !uc->is_bool() || uc->as_bool();
    if (!out.pat.utf8) out.pat.ucp = false;

    if (const json::Value *w = pv.find("weight"); w && w->is_number())
        out.pat.weight = w->as_number();
    if (const json::Value *ab = pv.find("absent"); ab && ab->is_bool())
        out.absent = ab->as_bool();
    if (const json::Value *ex = pv.find("extract"); ex) {
        if (!ex->is_array()) {
            err = "pattern 'extract' must be an array of strings";
            return false;
        }
        for (const auto &nv : ex->as_array()) {
            if (!nv.is_string() || nv.as_string().empty()) {
                err = "pattern 'extract' entries must be non-empty strings";
                return false;
            }
            out.pat.extract_names.push_back(nv.as_string());
        }
    }

    // Reject features we don't support so users can detect them.
    static const char *unsupported[] = {
        "pcre", "run_pattern_at", "run_pattern_from", "run_pattern_to",
        "run_pattern_until",
    };
    for (const char *k : unsupported) {
        if (pv.find(k)) {
            err = std::string("pattern field '") + k + "' not supported";
            return false;
        }
    }

    const json::Value *om = pv.find("on_match");
    if (om && om->is_array()) {
        for (const auto &av : om->as_array()) {
            Action a;
            std::string aerr;
            if (!compile_action(av, a, aerr)) {
                err = "in pattern '" + out.pat.id + "': " + aerr;
                return false;
            }
            out.on_match.push_back(std::move(a));
        }
    } else {
        // Default on_match: emit the standard record.
        Action a;
        a.kind = ActionKind::Emit;
        out.on_match.push_back(std::move(a));
    }
    return true;
}

// ---- Execution --------------------------------------------------------------

// Compare two RuntimeValues numerically; falls back to string compare when
// both are non-numeric.
int rt_compare(const RuntimeValue &a, const RuntimeValue &b) {
    auto numeric = [](const RuntimeValue &v) {
        return v.kind() == RuntimeValue::Int || v.kind() == RuntimeValue::Bool;
    };
    auto stringy = [](const RuntimeValue &v) {
        return v.kind() == RuntimeValue::Str;
    };
    if (numeric(a) && numeric(b)) {
        int64_t ai = a.to_int(), bi = b.to_int();
        return ai < bi ? -1 : (ai > bi ? 1 : 0);
    }
    // If one side is numeric and the other a numeric-looking string,
    // compare numerically.
    if (numeric(a) && stringy(b)) {
        double da = a.to_double(), db = b.to_double();
        return da < db ? -1 : (da > db ? 1 : 0);
    }
    if (stringy(a) && numeric(b)) {
        double da = a.to_double(), db = b.to_double();
        return da < db ? -1 : (da > db ? 1 : 0);
    }
    std::string sa = a.to_str(), sb = b.to_str();
    return sa.compare(sb);
}

bool eval_condition(const Condition &c, const ExecCtx &ctx) {
    switch (c.op) {
        case CondOp::And:
            for (const auto &s : c.sub) if (!eval_condition(*s, ctx)) return false;
            return true;
        case CondOp::Or:
            for (const auto &s : c.sub) if (eval_condition(*s, ctx)) return true;
            return false;
        case CondOp::Not:
            return !eval_condition(*c.sub[0], ctx);
        case CondOp::Eq:
        case CondOp::Ne: {
            RuntimeValue a = resolve_value(c.arg_a, ctx);
            RuntimeValue b = resolve_value(c.arg_b, ctx);
            bool eq = a.equals(b);
            return c.op == CondOp::Eq ? eq : !eq;
        }
        case CondOp::Gt:
        case CondOp::Lt:
        case CondOp::Gte:
        case CondOp::Lte: {
            RuntimeValue a = resolve_value(c.arg_a, ctx);
            RuntimeValue b = resolve_value(c.arg_b, ctx);
            int cmp = rt_compare(a, b);
            switch (c.op) {
                case CondOp::Gt:  return cmp > 0;
                case CondOp::Lt:  return cmp < 0;
                case CondOp::Gte: return cmp >= 0;
                case CondOp::Lte: return cmp <= 0;
                default: break;
            }
            return false;
        }
        case CondOp::Contains: {
            RuntimeValue hay = resolve_value(c.arg_a, ctx);
            RuntimeValue needle = resolve_value(c.arg_b, ctx);
            if (hay.is_list()) {
                for (const auto &e : hay.as_list()) if (e.equals(needle)) return true;
                return false;
            }
            std::string h = hay.to_str();
            std::string n = needle.to_str();
            return h.find(n) != std::string::npos;
        }
        case CondOp::IsSet: {
            if (!ctx.vars) return false;
            const RuntimeValue *v = ctx.vars->find(c.var_name);
            if (!v) return false;
            return v->to_bool();
        }
    }
    return false;
}

void execute_actions(const std::vector<Action> &actions, ExecCtx &ctx) {
    for (const auto &a : actions) {
        if (ctx.state->stop_all || ctx.state->stop_file) return;
        execute_action(a, ctx);
    }
}

// Run sub-patterns from `info` over the substring `text` whose first byte
// in the original file is at `text_offset_in_file`. `valid_offsets` controls
// whether reported offsets are file-relative.
void run_submatch(const SubmatchInfo &info, std::string_view text,
                  uint64_t text_offset_in_file, bool valid_offsets,
                  ExecCtx &outer);

void execute_action(const Action &a, ExecCtx &ctx) {
    auto &state = *ctx.state;
    auto &vars = *ctx.vars;

    auto var_or_create_int = [&](const std::string &name) -> RuntimeValue & {
        if (!vars.exists(name)) vars.declare(name, VarType::Int, RuntimeValue::make_int(0));
        return *vars.find(name);
    };
    auto var_or_create_list = [&](const std::string &name) -> RuntimeValue & {
        if (!vars.exists(name)) vars.declare(name, VarType::List, RuntimeValue::make_list());
        if (!vars.find(name)->is_list())
            *vars.find(name) = RuntimeValue::make_list();
        return *vars.find(name);
    };
    auto var_or_create_map = [&](const std::string &name) -> RuntimeValue & {
        if (!vars.exists(name)) vars.declare(name, VarType::Map, RuntimeValue::make_map());
        if (!vars.find(name)->is_map())
            *vars.find(name) = RuntimeValue::make_map();
        return *vars.find(name);
    };

    switch (a.kind) {
    case ActionKind::Emit: {
        std::string line;
        if (a.has_data) {
            line = render_data_to_json(a.data, ctx);
        } else {
            line = build_default_record(ctx).to_json();
        }
        emit_record_string(line, ctx, "");
        return;
    }
    case ActionKind::Print: {
        if (state.suppress_records) return;
        // print emits a raw text line. Without `value`, emits the default
        // JSON record (effectively same as emit in that case).
        std::string text;
        if (a.has_value && a.value.is_string()) {
            text = substitute(a.value.as_string(), ctx);
        } else if (a.has_value) {
            text = resolve_value(a.value, ctx).to_str();
        } else {
            text = build_default_record(ctx).to_json();
        }
        // print bypasses skip/limit by design. But it does count toward the
        // emit cap for stop semantics — we treat it the same as emit here for
        // predictable behaviour.
        if (state.group_by.empty()) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fputc('\n', stdout);
        } else {
            // group_by buffers JSON-Lines records; print produces non-JSON
            // text — flush directly so it's not lost.
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fputc('\n', stdout);
        }
        ++state.emitted;
        return;
    }
    case ActionKind::Set: {
        RuntimeValue v = a.has_value ? resolve_value(a.value, ctx)
                                     : RuntimeValue::make_null();
        // Preserve declared type when feasible.
        const auto &specs = vars.specs();
        auto sit = specs.find(a.var);
        if (sit != specs.end()) {
            switch (sit->second.type) {
                case VarType::Int:
                    v = RuntimeValue::make_int(v.to_int()); break;
                case VarType::Bool:
                    v = RuntimeValue::make_bool(v.to_bool()); break;
                case VarType::Str:
                    v = RuntimeValue::make_str(v.to_str()); break;
                case VarType::List:
                case VarType::Map:
                    /* keep as-is; user is on their own */ break;
            }
        }
        vars.set(a.var, std::move(v));
        return;
    }
    case ActionKind::Increment: {
        RuntimeValue &cur = var_or_create_int(a.var);
        cur = RuntimeValue::make_int(cur.to_int() + 1);
        return;
    }
    case ActionKind::Decrement: {
        RuntimeValue &cur = var_or_create_int(a.var);
        cur = RuntimeValue::make_int(cur.to_int() - 1);
        return;
    }
    case ActionKind::Add: {
        RuntimeValue &cur = var_or_create_int(a.var);
        RuntimeValue v = resolve_value(a.value, ctx);
        cur = RuntimeValue::make_int(cur.to_int() + v.to_int());
        return;
    }
    case ActionKind::Subtract: {
        RuntimeValue &cur = var_or_create_int(a.var);
        RuntimeValue v = resolve_value(a.value, ctx);
        cur = RuntimeValue::make_int(cur.to_int() - v.to_int());
        return;
    }
    case ActionKind::Multiply: {
        RuntimeValue &cur = var_or_create_int(a.var);
        RuntimeValue v = resolve_value(a.value, ctx);
        cur = RuntimeValue::make_int(cur.to_int() * v.to_int());
        return;
    }
    case ActionKind::Divide: {
        RuntimeValue &cur = var_or_create_int(a.var);
        RuntimeValue v = resolve_value(a.value, ctx);
        int64_t div = v.to_int();
        if (div == 0) return; // silent ignore per spec
        cur = RuntimeValue::make_int(cur.to_int() / div);
        return;
    }
    case ActionKind::Reset: {
        for (const auto &n : a.vars) vars.reset(n);
        return;
    }
    case ActionKind::Append: {
        RuntimeValue &lst = var_or_create_list(a.target);
        RuntimeValue v = a.has_value ? resolve_value(a.value, ctx)
                                     : build_default_record(ctx);
        lst.as_list().push_back(std::move(v));
        return;
    }
    case ActionKind::Collect: {
        RuntimeValue &lst = var_or_create_list(a.target);
        lst.as_list().push_back(build_default_record(ctx));
        return;
    }
    case ActionKind::UniqueAppend: {
        RuntimeValue &lst = var_or_create_list(a.target);
        RuntimeValue v = a.has_value ? resolve_value(a.value, ctx)
                                     : build_default_record(ctx);
        for (const auto &e : lst.as_list()) if (e.equals(v)) return;
        lst.as_list().push_back(std::move(v));
        return;
    }
    case ActionKind::Sort: {
        if (!vars.exists(a.var)) return;
        RuntimeValue *lp = vars.find(a.var);
        if (!lp || !lp->is_list()) return;
        auto &lst = lp->as_list();
        const std::string &key = a.sort_key;
        bool desc = a.sort_desc;
        std::sort(lst.begin(), lst.end(),
                  [&](const RuntimeValue &x, const RuntimeValue &y) {
            auto get = [&](const RuntimeValue &v) -> RuntimeValue {
                if (!v.is_map()) return RuntimeValue::make_null();
                auto it = v.as_map().find(key);
                if (it == v.as_map().end()) return RuntimeValue::make_null();
                return it->second;
            };
            int c = rt_compare(get(x), get(y));
            return desc ? c > 0 : c < 0;
        });
        return;
    }
    case ActionKind::MapSet: {
        RuntimeValue &m = var_or_create_map(a.target);
        std::string k = substitute(a.key, ctx);
        RuntimeValue v = resolve_value(a.value, ctx);
        m.as_map()[k] = std::move(v);
        return;
    }
    case ActionKind::MapIncrement: {
        RuntimeValue &m = var_or_create_map(a.target);
        std::string k = substitute(a.key, ctx);
        auto &mm = m.as_map();
        auto it = mm.find(k);
        if (it == mm.end()) mm.emplace(k, RuntimeValue::make_int(1));
        else it->second = RuntimeValue::make_int(it->second.to_int() + 1);
        return;
    }
    case ActionKind::Count: {
        // Increment map[var][$PAT_ID].
        RuntimeValue &m = var_or_create_map(a.var);
        const std::string &k = ctx.pat_id;
        auto &mm = m.as_map();
        auto it = mm.find(k);
        if (it == mm.end()) mm.emplace(k, RuntimeValue::make_int(1));
        else it->second = RuntimeValue::make_int(it->second.to_int() + 1);
        return;
    }
    case ActionKind::If: {
        if (eval_condition(*a.cond, ctx)) execute_actions(a.branch_a, ctx);
        else execute_actions(a.branch_b, ctx);
        return;
    }
    case ActionKind::ForEach: {
        const RuntimeValue *src = vars.find(a.var);
        if (!src) return;
        if (src->is_list()) {
            // Take a copy so the loop is stable if `do` mutates the list.
            std::vector<RuntimeValue> items = src->as_list();
            for (const auto &item : items) {
                if (state.stop_all || state.stop_file) return;
                vars.set(a.as_var, item);
                execute_actions(a.branch_a, ctx);
            }
        } else if (src->is_map()) {
            std::vector<std::pair<std::string, RuntimeValue>> entries(
                src->as_map().begin(), src->as_map().end());
            for (const auto &kv : entries) {
                if (state.stop_all || state.stop_file) return;
                vars.set(a.key_as_var.empty() ? "key" : a.key_as_var,
                         RuntimeValue::make_str(kv.first));
                vars.set(a.as_var, kv.second);
                execute_actions(a.branch_a, ctx);
            }
        }
        return;
    }
    case ActionKind::Stop: {
        state.stop_file = true;
        return;
    }
    case ActionKind::Submatch: {
        if (!a.submatch) return;
        // Resolve text source.
        std::string_view text;
        std::string scratch;
        uint64_t base = ctx.from;
        bool valid = ctx.has_match;
        const std::string &src = a.submatch->text_source;
        if (src.empty() || src == "$MATCH") {
            text = ctx.match_text;
            base = ctx.from;
            valid = ctx.has_match;
        } else if (src == "$BLOCK" && ctx.has_block) {
            text = ctx.block_text;
            base = ctx.block_start;
            valid = true;
        } else if (src == "$BLOCK_FULL" && ctx.has_block) {
            text = ctx.block_full_text;
            base = ctx.from;
            valid = true;
        } else if (src == "$CONTEXT") {
            text = ctx.context_text;
            // context begins at start of the (first) context line in the file
            if (ctx.line_idx && ctx.line_available) {
                uint64_t ls, le;
                ctx.line_idx->line_range(ctx.from, ls, le);
                base = ls;
                valid = true;
            } else {
                valid = false;
            }
        } else {
            scratch = substitute(src, ctx);
            text = scratch;
            valid = false;
        }
        run_submatch(*a.submatch, text, base, valid, ctx);
        return;
    }
    case ActionKind::Block: {
        if (!ctx.has_match) return;
        if (ctx.file_buf.empty()) return;
        std::string open = substitute(a.block_open, ctx);
        std::string close = substitute(a.block_close, ctx);
        uint64_t op = 0, cp = 0;
        if (!find_balanced_block(ctx.file_buf, ctx.to, open, close, op, cp))
            return;
        ExecCtx sub = ctx;
        sub.has_block = true;
        sub.block_start = op;
        sub.block_end = cp;
        sub.block_text.assign(ctx.file_buf.data() + op, cp - op);
        sub.block_full_text.assign(ctx.file_buf.data() + ctx.from, cp - ctx.from);
        if (ctx.state) {
            sub.block_truncated      = truncate_to_bytes(sub.block_text,      ctx.state->max_block_bytes);
            sub.block_full_truncated = truncate_to_bytes(sub.block_full_text, ctx.state->max_block_bytes);
        }
        if (ctx.line_idx && ctx.line_available) {
            sub.block_line_start = ctx.line_idx->line_of(op);
            sub.block_line_end = ctx.line_idx->line_of(cp == 0 ? 0 : cp - 1);
        } else {
            sub.block_line_start = 0;
            sub.block_line_end = 0;
        }
        execute_actions(a.branch_a, sub);
        return;
    }
    case ActionKind::Lookup: {
        const RuntimeValue *m = vars.find(a.lookup_map);
        std::string key = a.has_value ? resolve_value(a.value, ctx).to_str()
                                      : "";
        ExecCtx sub = ctx;
        sub.has_lookup = true;
        sub.lookup_key = key;
        if (m && m->is_map()) {
            auto it = m->as_map().find(key);
            if (it != m->as_map().end()) {
                sub.lookup_value = it->second;
                execute_actions(a.branch_a, sub);
                return;
            }
        }
        sub.lookup_value = RuntimeValue::make_null();
        execute_actions(a.branch_b, sub);
        return;
    }
    }
}

// Apply leftmost-longest non-overlapping dedup to a raw match list.
void dedup_matches(std::vector<Match> &raw) {
    std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
        if (a.pattern_index != b.pattern_index)
            return a.pattern_index < b.pattern_index;
        if (a.from != b.from) return a.from < b.from;
        return a.to > b.to;
    });
    std::vector<Match> kept;
    kept.reserve(raw.size());
    uint32_t cur = UINT32_MAX;
    uint64_t last_to = 0;
    for (const auto &m : raw) {
        if (m.pattern_index != cur) { cur = m.pattern_index; last_to = 0; }
        if (m.from < last_to) continue;
        last_to = m.to;
        kept.push_back(m);
    }
    raw = std::move(kept);
}

// Extract context block text spanning from `first_line` to `last_line`.
std::string slice_lines(const LineIndex &idx, uint32_t first_line,
                        uint32_t last_line) {
    if (first_line == 0) first_line = 1;
    if (last_line < first_line) last_line = first_line;
    auto first = idx.line_text(first_line);
    auto last = idx.line_text(last_line);
    if (!first.data() || !last.data()) return "";
    const char *cs = first.data();
    const char *ce = last.data() + last.size();
    return std::string(cs, ce - cs);
}

// Build the per-match exec context (line/col/match/context fields).
void populate_match_ctx(const Match &m, std::string_view buf,
                        const LineIndex &idx, int ctx_before, int ctx_after,
                        ExecCtx &ctx) {
    ctx.from = m.from;
    ctx.to = m.to;
    ctx.match_text.assign(buf.data() + m.from, m.to - m.from);
    ctx.line = idx.line_of(m.from);
    ctx.col = idx.col_of(m.from);
    uint32_t total = idx.line_count();
    uint32_t first = (ctx.line > (uint32_t)ctx_before) ? ctx.line - ctx_before : 1;
    uint32_t last = std::min<uint32_t>(ctx.line + ctx_after, total);
    ctx.context_text = slice_lines(idx, first, last);
    ctx.context_before_text = (ctx_before > 0 && ctx.line > 1)
        ? slice_lines(idx, first, ctx.line - 1) : "";
    ctx.context_after_text = (ctx_after > 0 && ctx.line < total)
        ? slice_lines(idx, ctx.line + 1, last) : "";
    ctx.line_available = true;
    ctx.has_match = true;
    if (ctx.state) {
        ctx.match_truncated   = truncate_to_bytes(ctx.match_text,   ctx.state->max_match_bytes);
        ctx.context_truncated = truncate_to_bytes(ctx.context_text, ctx.state->max_context_bytes);
        // Truncate before/after pieces with the same context budget so they
        // remain visually consistent with $CONTEXT.
        truncate_to_bytes(ctx.context_before_text, ctx.state->max_context_bytes);
        truncate_to_bytes(ctx.context_after_text,  ctx.state->max_context_bytes);
    } else {
        ctx.match_truncated = ctx.context_truncated = false;
    }
}

void run_submatch(const SubmatchInfo &info, std::string_view text,
                  uint64_t text_offset_in_file, bool valid_offsets,
                  ExecCtx &outer) {
    if (!info.matcher) return;
    std::vector<Match> raw;
    info.matcher->scan(text, [&](const Match &m) -> bool {
        raw.push_back(m);
        return true;
    });
    dedup_matches(raw);
    // Sort by (from, pat) for stable interleaved output.
    std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
        if (a.from != b.from) return a.from < b.from;
        return a.pattern_index < b.pattern_index;
    });

    std::set<uint32_t> matched_pat_idx;

    // Build a lightweight LineIndex over `text` for $LINE/$COL when offsets
    // are not file-relative. When valid_offsets is true, defer to outer's
    // file-level idx via the parent context.
    LineIndex sub_idx;
    if (!valid_offsets) sub_idx.build(text);

    for (const auto &m : raw) {
        if (outer.state->stop_all || outer.state->stop_file) return;
        if (m.pattern_index >= info.patterns.size()) continue;
        const CompiledPattern &cp = info.patterns[m.pattern_index];
        matched_pat_idx.insert(m.pattern_index);
        if (cp.absent) continue;

        ExecCtx sub = outer;
        sub.pat_id = cp.pat.id;
        sub.has_match = true;
        sub.match_text.assign(text.data() + m.from, m.to - m.from);
        if (valid_offsets && outer.line_idx) {
            sub.from = text_offset_in_file + m.from;
            sub.to = text_offset_in_file + m.to;
            sub.line = outer.line_idx->line_of(sub.from);
            sub.col = outer.line_idx->col_of(sub.from);
            sub.line_available = true;
        } else {
            sub.from = m.from;
            sub.to = m.to;
            sub.line = sub_idx.line_of(m.from);
            sub.col = sub_idx.col_of(m.from);
            sub.line_available = true;
        }
        // Recompute context window from the appropriate index.
        if (valid_offsets && outer.line_idx) {
            uint32_t total = outer.line_idx->line_count();
            sub.context_text = slice_lines(*outer.line_idx, sub.line, sub.line);
            sub.context_before_text = "";
            sub.context_after_text = "";
            (void)total;
        } else {
            sub.context_text = std::string(sub_idx.line_text(sub.line));
            sub.context_before_text = "";
            sub.context_after_text = "";
        }
        execute_actions(cp.on_match, sub);
    }

    // Fire absent sub-patterns that did not match.
    for (size_t i = 0; i < info.patterns.size(); ++i) {
        const CompiledPattern &cp = info.patterns[i];
        if (!cp.absent) continue;
        if (matched_pat_idx.count((uint32_t)i)) continue;
        // Inherit outer's match context (per spec: $FILE, $LINE, $FROM,
        // $TO, $MATCH come from the outer match).
        ExecCtx sub = outer;
        sub.pat_id = cp.pat.id;
        execute_actions(cp.on_match, sub);
    }
}

// ---- Phase --------------------------------------------------------------

struct CompiledPhase {
    std::string id;
    std::vector<CompiledPattern> patterns;
    std::shared_ptr<Matcher> matcher;
    std::shared_ptr<ExtractTable> extract_table;
    std::vector<std::string> scan_globs;
    std::vector<std::string> excludes;
    int ctx_before = 0;
    int ctx_after = 0;
    std::vector<Action> on_file_end;
    std::vector<Action> on_complete;
};

bool compile_phase(const json::Value &pv, const json::Object &root,
                   CompiledPhase &out, std::string &err) {
    if (!pv.is_object()) { err = "phase must be an object"; return false; }
    const json::Value *id = pv.find("id");
    if (!id || !id->is_string()) { err = "phase missing 'id'"; return false; }
    out.id = id->as_string();
    const json::Value *pp = pv.find("patterns");
    if (!pp || !pp->is_array() || pp->as_array().empty()) {
        err = "phase '" + out.id + "' needs non-empty 'patterns'";
        return false;
    }
    for (size_t i = 0; i < pp->as_array().size(); ++i) {
        CompiledPattern cp;
        std::string e;
        if (!compile_pattern(pp->as_array()[i], i, cp, e)) {
            err = "phase '" + out.id + "': " + e;
            return false;
        }
        out.patterns.push_back(std::move(cp));
    }
    std::string merr;
    out.matcher = build_matcher(out.patterns, merr);
    if (!out.matcher) {
        err = "phase '" + out.id + "': pattern compile failed: " + merr;
        return false;
    }
    {
        std::vector<Pattern> ps;
        ps.reserve(out.patterns.size());
        for (const auto &c : out.patterns) ps.push_back(c.pat);
        auto et = std::make_shared<ExtractTable>();
        std::string ee;
        int ee_idx = -1;
        if (!et->build(ps, &ee, &ee_idx)) {
            err = "phase '" + out.id + "': pattern '"
                  + (ee_idx >= 0 ? out.patterns[ee_idx].pat.id : std::string("?"))
                  + "': " + ee;
            return false;
        }
        if (et->any()) out.extract_table = et;
    }
    auto take_array = [&](const char *key, std::vector<std::string> &out_v) {
        const json::Value *v = pv.find(key);
        if (!v) {
            // Inherit script-level.
            auto it = root.find(key);
            if (it == root.end()) return;
            v = &it->second;
        }
        if (!v->is_array()) return;
        for (const auto &e : v->as_array()) {
            if (e.is_string()) out_v.push_back(e.as_string());
        }
    };
    take_array("scan", out.scan_globs);
    take_array("exclude", out.excludes);

    // Per-phase context (falls back to script-level).
    auto get_int = [&](const char *key, int dflt) {
        const json::Value *v = pv.find(key);
        if (!v) {
            auto it = root.find(key);
            if (it == root.end()) return dflt;
            v = &it->second;
        }
        return v->is_number() ? (int)v->as_number() : dflt;
    };
    int ctx = get_int("context", 0);
    out.ctx_before = ctx;
    out.ctx_after = ctx;
    if (auto it = pv.find("context_before"); it && it->is_number())
        out.ctx_before = (int)it->as_number();
    if (auto it = pv.find("context_after"); it && it->is_number())
        out.ctx_after = (int)it->as_number();

    if (const json::Value *fe = pv.find("on_file_end"); fe && fe->is_array()) {
        if (!compile_action_list(*fe, out.on_file_end,
                                 "phase '" + out.id + "' on_file_end", err))
            return false;
    }
    if (const json::Value *oc = pv.find("on_complete"); oc && oc->is_array()) {
        if (!compile_action_list(*oc, out.on_complete,
                                 "phase '" + out.id + "' on_complete", err))
            return false;
    }
    return true;
}

// Walk-and-scan one phase against its scan globs / cli-overridden positionals.
void run_phase(const CompiledPhase &phase, ScriptState &state,
               LineIndex *unused_idx,
               const std::vector<Action> &script_on_file_end,
               const std::vector<std::string> &override_scan,
               bool reading_stdin_content,
               const std::string &stdin_label,
               std::string_view stdin_content) {
    (void)unused_idx;
    // Helpers to run scan over a buffer (file or stdin).
    auto scan_buf = [&](const std::string &display_name,
                        std::string_view content) -> bool {
        LineIndex idx;
        idx.build(content);

        ScopeIndex scope_idx;
        const ScopeIndex *scope_ptr = nullptr;
        bool scope_enabled = !state.scope_lang.empty() ||
            (!state.scope_custom.anchor_regex.empty() &&
             !state.scope_custom.open.empty() &&
             !state.scope_custom.close.empty());
        if (scope_enabled) {
            ScopeConfig sc = resolve_scope_for_file(state.scope_lang,
                                                    state.scope_custom,
                                                    display_name);
            if (!sc.anchor_regex.empty()) {
                std::string serr;
                if (!scope_idx.build(content, sc, idx, &serr)) {
                    std::fprintf(stderr, "hprscript: %s\n", serr.c_str());
                } else {
                    scope_ptr = &scope_idx;
                }
            }
        }

        std::vector<Match> raw;
        phase.matcher->scan(content, [&](const Match &m) -> bool {
            raw.push_back(m);
            return true;
        });
        dedup_matches(raw);
        if (phase.patterns.size() > 1) {
            std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
                if (a.from != b.from) return a.from < b.from;
                return a.pattern_index < b.pattern_index;
            });
        }

        // Apply -near / -far relations against THIS phase's pattern list.
        // Strings that don't resolve to a pattern in this phase are silently
        // skipped (the relation simply doesn't filter that pattern's matches).
        if (!state.relations.empty() && !raw.empty()) {
            auto resolve = [&](const std::string &id, uint32_t &out) -> bool {
                for (size_t i = 0; i < phase.patterns.size(); ++i) {
                    if (phase.patterns[i].pat.id == id) {
                        out = static_cast<uint32_t>(i); return true;
                    }
                }
                char *end = nullptr;
                long n = std::strtol(id.c_str(), &end, 10);
                if (end != id.c_str() && *end == '\0' && n >= 0 &&
                    static_cast<size_t>(n) < phase.patterns.size()) {
                    out = static_cast<uint32_t>(n);
                    return true;
                }
                return false;
            };
            struct Resolved { bool near; uint32_t a, b; int lines; };
            std::vector<Resolved> rels;
            for (const auto &r : state.relations) {
                Resolved rr;
                rr.near = r.near;
                rr.lines = r.lines;
                if (!resolve(r.a, rr.a) || !resolve(r.b, rr.b)) continue;
                rels.push_back(rr);
            }
            if (!rels.empty()) {
                std::vector<std::vector<uint32_t>> lines_by_pat(phase.patterns.size());
                for (const auto &mm : raw)
                    lines_by_pat[mm.pattern_index].push_back(idx.line_of(mm.from));
                for (auto &v : lines_by_pat) std::sort(v.begin(), v.end());
                std::vector<Match> filtered;
                filtered.reserve(raw.size());
                for (const auto &mm : raw) {
                    uint32_t mline = idx.line_of(mm.from);
                    bool drop = false;
                    for (const auto &r : rels) {
                        if (r.a != mm.pattern_index) continue;
                        const auto &v = lines_by_pat[r.b];
                        uint32_t lo = (mline > (uint32_t)r.lines)
                                      ? mline - (uint32_t)r.lines : 1;
                        uint32_t hi = mline + (uint32_t)r.lines;
                        auto lit = std::lower_bound(v.begin(), v.end(), lo);
                        bool found = false;
                        if (r.a == r.b) {
                            for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                                if (*it != mline) { found = true; break; }
                            }
                            if (!found) {
                                size_t cnt = 0;
                                for (auto it = lit; it != v.end() && *it <= hi; ++it)
                                    if (*it == mline) ++cnt;
                                if (cnt > 1) found = true;
                            }
                        } else {
                            for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                                found = true; break;
                            }
                        }
                        if (r.near && !found) { drop = true; break; }
                        if (!r.near && found) { drop = true; break; }
                    }
                    if (!drop) filtered.push_back(mm);
                }
                raw = std::move(filtered);
            }
        }

        std::set<uint32_t> matched_pat_idx;

        state.stop_file = false;
        state.per_file_emits = 0;
        if (state.rank_enabled) state.rank_file_pat_ids.clear();
        state.suppress_records = state.rank_enabled;

        ExecCtx ctx;
        ctx.file = display_name;
        ctx.vars = &state.vars;
        ctx.line_idx = &idx;
        ctx.file_buf = content;
        ctx.state = &state;
        ctx.extract_table = phase.extract_table.get();
        ctx.scope = scope_ptr;

        for (const auto &m : raw) {
            if (state.stop_all) return false;
            if (state.stop_file) break;
            const CompiledPattern &cp = phase.patterns[m.pattern_index];
            matched_pat_idx.insert(m.pattern_index);
            if (cp.absent) continue;
            ctx.pat_id = cp.pat.id;
            ctx.pat_index = m.pattern_index;
            populate_match_ctx(m, content, idx, phase.ctx_before, phase.ctx_after,
                               ctx);
            ctx.enclosing = scope_ptr ? scope_ptr->find_innermost(m.from) : nullptr;
            ctx.has_block = false;
            ctx.has_lookup = false;
            if (state.rank_enabled) {
                auto &fr = state.rank_per_file[display_name];
                if (fr.matched_pat_ids.empty() && fr.match_points.empty())
                    state.rank_file_order.push_back(display_name);
                // First time seeing this pattern in this file: bump raw score.
                if (state.rank_file_pat_ids.insert(cp.pat.id).second) {
                    fr.matched_pat_ids.insert(cp.pat.id);
                    fr.raw_score += cp.pat.weight;
                    fr.line_count = idx.line_count();
                }
                // Record the match point for proximity. Cap to keep memory
                // bounded on pathological files.
                static constexpr size_t kMaxMatchPoints = 4096;
                if (fr.match_points.size() < kMaxMatchPoints) {
                    auto pit = fr.pat_local_ids.find(cp.pat.id);
                    uint16_t local;
                    if (pit == fr.pat_local_ids.end()) {
                        local = (uint16_t)std::min<size_t>(
                            fr.pat_local_ids.size(), 0xFFFFu);
                        fr.pat_local_ids[cp.pat.id] = local;
                    } else {
                        local = pit->second;
                    }
                    fr.match_points.emplace_back(idx.line_of(m.from), local);
                }
            }
            execute_actions(cp.on_match, ctx);
        }

        // Absent file-level patterns whose pattern was not seen.
        for (size_t i = 0; i < phase.patterns.size(); ++i) {
            if (state.stop_all) return false;
            const CompiledPattern &cp = phase.patterns[i];
            if (!cp.absent) continue;
            if (matched_pat_idx.count((uint32_t)i)) continue;
            ExecCtx ac;
            ac.file = display_name;
            ac.pat_id = cp.pat.id;
            ac.vars = &state.vars;
            ac.line_idx = &idx;
            ac.file_buf = content;
            ac.state = &state;
            ac.has_match = false;
            ac.line_available = true;
            execute_actions(cp.on_match, ac);
        }
        state.suppress_records = false;

        // Phase-level on_file_end.
        if (!phase.on_file_end.empty()) {
            ExecCtx ec;
            ec.file = display_name;
            ec.vars = &state.vars;
            ec.line_idx = &idx;
            ec.file_buf = content;
            ec.state = &state;
            ec.has_match = false;
            execute_actions(phase.on_file_end, ec);
        }
        // Script-level on_file_end (only run from the last phase to avoid
        // double-firing — handled by caller passing empty list when not last).
        if (!script_on_file_end.empty()) {
            ExecCtx ec;
            ec.file = display_name;
            ec.vars = &state.vars;
            ec.line_idx = &idx;
            ec.file_buf = content;
            ec.state = &state;
            ec.has_match = false;
            execute_actions(script_on_file_end, ec);
        }

        return !state.stop_all;
    };

    // Decide inputs.
    if (reading_stdin_content) {
        scan_buf(stdin_label, stdin_content);
        return;
    }

    Walker walker;
    if (!override_scan.empty()) {
        for (const auto &p : override_scan) walker.add_scan(p);
    } else {
        for (const auto &g : phase.scan_globs) walker.add_scan(g);
    }
    for (const auto &e : phase.excludes) walker.add_exclude(e);

    walker.walk([&](const WalkItem &it) {
        if (state.stop_all) return false;
        if (it.is_binary) return true;
        MappedFile mf;
        if (!mf.open(it.path)) {
            std::fprintf(stderr, "hprscript: cannot read %s\n", it.path.c_str());
            return true;
        }
        return scan_buf(it.path, mf.view());
    });
}

void flush_groups(ScriptState &state) {
    if (state.group_by.empty()) return;
    for (const auto &key : state.group_order) {
        std::string line = "{\"key\":\"";
        json_escape_to(line, key);
        line += "\",\"group\":[";
        const auto &recs = state.group_records[key];
        for (size_t i = 0; i < recs.size(); ++i) {
            if (i) line += ',';
            line += recs[i];
        }
        line += "]}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }
    state.group_records.clear();
    state.group_order.clear();
}

// Count proximity clusters: contiguous groups of match points where each
// adjacent pair is within K lines, and the group spans ≥2 distinct pattern
// IDs. K=20 is roughly "same function" in typical code.
static uint32_t count_prox_clusters(
    std::vector<std::pair<uint32_t, uint16_t>> pts) {
    static constexpr uint32_t K = 20;
    if (pts.size() < 2) return 0;
    std::sort(pts.begin(), pts.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    uint32_t clusters = 0;
    size_t i = 0;
    while (i < pts.size()) {
        size_t j = i + 1;
        while (j < pts.size() && pts[j].first - pts[j - 1].first <= K) ++j;
        if (j - i >= 2) {
            uint16_t first_id = pts[i].second;
            for (size_t k = i + 1; k < j; ++k) {
                if (pts[k].second != first_id) { ++clusters; break; }
            }
        }
        i = j;
    }
    return clusters;
}

void flush_rank(ScriptState &state) {
    if (!state.rank_enabled) return;
    struct Row {
        std::string file;
        double score;
        double density;
        std::vector<std::string> pats;
    };
    // Tunables. Coverage exponent makes "matches all queried patterns" a
    // strong multiplier; density divisor gently penalizes huge files;
    // proximity bonus rewards co-located matches (cohesion).
    static constexpr double kCoverageExp = 1.5;
    static constexpr double kProximityWeight = 0.5;
    const double queried = state.rank_total_queried > 0
                               ? (double)state.rank_total_queried
                               : 1.0;

    std::vector<Row> rows;
    rows.reserve(state.rank_file_order.size());
    for (const auto &f : state.rank_file_order) {
        const FileRank &fr = state.rank_per_file[f];
        double matched = (double)fr.matched_pat_ids.size();
        double coverage = matched / queried;
        if (coverage > 1.0) coverage = 1.0;
        double cov_factor = std::pow(coverage, kCoverageExp);
        double divisor = std::log((double)fr.line_count + 10.0);
        if (divisor <= 0.0) divisor = 1.0;
        uint32_t clusters = count_prox_clusters(fr.match_points);
        double prox_bonus = kProximityWeight * (double)clusters;

        Row r;
        r.file = f;
        r.score = cov_factor * fr.raw_score / divisor + prox_bonus;
        r.density = fr.line_count > 0
                        ? fr.raw_score / (double)fr.line_count
                        : 0.0;
        r.pats.assign(fr.matched_pat_ids.begin(), fr.matched_pat_ids.end());
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.density > b.density;
    });
    for (const auto &r : rows) {
        std::string line = "{\"file\":\"";
        json_escape_to(line, r.file);
        line += "\",\"score\":";
        // Format score: integer when possible, else fixed/sane.
        if (r.score == (double)(int64_t)r.score) {
            line += i64_str((int64_t)r.score);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", r.score);
            line += buf;
        }
        line += ",\"density\":";
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", r.density);
            line += buf;
        }
        line += ",\"matched_patterns\":[";
        for (size_t i = 0; i < r.pats.size(); ++i) {
            if (i) line += ',';
            line += '"';
            json_escape_to(line, r.pats[i]);
            line += '"';
        }
        line += "]}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }
}

} // namespace

int run_script(const Cli &cli) {
    // 1. Resolve script source.
    std::string script_text;
    if (!cli.script_inline.empty()) {
        script_text = cli.script_inline;
    } else if (!cli.script_path.empty()) {
        if (!slurp_file_to_string(cli.script_path, script_text)) {
            std::fprintf(stderr, "hprscript: cannot read script %s: %s\n",
                         cli.script_path.c_str(), std::strerror(errno));
            return 2;
        }
    } else if (!cli.positional.empty()) {
        std::ifstream probe(cli.positional[0]);
        if (probe) {
            if (!slurp_file_to_string(cli.positional[0], script_text)) {
                std::fprintf(stderr, "hprscript: cannot read %s\n",
                             cli.positional[0].c_str());
                return 2;
            }
        } else {
            std::fprintf(stderr, "hprscript: no -p/-s/-script given and no script file found\n");
            return 2;
        }
    } else if (!isatty(fileno(stdin))) {
        if (!slurp_stdin(script_text)) {
            std::fprintf(stderr, "hprscript: failed to read script from stdin\n");
            return 2;
        }
    } else {
        std::fprintf(stderr, "hprscript: no input. Run 'hprscript --help'.\n");
        return 2;
    }

    auto pr = json::parse(script_text);
    if (!pr.ok) {
        std::fprintf(stderr, "hprscript: script JSON parse error at byte %zu: %s\n",
                     pr.error_pos, pr.error.c_str());
        return 2;
    }
    if (!pr.value.is_object()) {
        std::fprintf(stderr, "hprscript: script must be a JSON object\n");
        return 2;
    }
    const json::Object &root = pr.value.as_object();

    // Reject features still out of scope so callers see them.
    static const char *reject[] = {
        "boundary", "on_boundary", "ascii_only", "overlap", "files",
    };
    for (const char *k : reject) {
        if (root.count(k)) {
            std::fprintf(stderr, "hprscript: top-level '%s' is not supported\n", k);
            return 2;
        }
    }

    ScriptState state;

    // Variables.
    if (auto it = root.find("variables"); it != root.end() && it->second.is_object()) {
        for (const auto &kv : it->second.as_object()) {
            const json::Value &spec = kv.second;
            if (!spec.is_object()) {
                std::fprintf(stderr, "hprscript: variable '%s' must be an object\n",
                             kv.first.c_str());
                return 2;
            }
            const json::Value *t = spec.find("type");
            if (!t || !t->is_string()) {
                std::fprintf(stderr, "hprscript: variable '%s' needs 'type'\n",
                             kv.first.c_str());
                return 2;
            }
            VarType vt = VarType::Str;
            const std::string &tn = t->as_string();
            if (tn == "string") vt = VarType::Str;
            else if (tn == "int") vt = VarType::Int;
            else if (tn == "bool") vt = VarType::Bool;
            else if (tn == "list") vt = VarType::List;
            else if (tn == "map")  vt = VarType::Map;
            else {
                std::fprintf(stderr, "hprscript: variable '%s' unknown type '%s'\n",
                             kv.first.c_str(), tn.c_str());
                return 2;
            }
            RuntimeValue dflt;
            if (const json::Value *dv = spec.find("default")) {
                dflt = RuntimeValue::from_json(*dv);
            } else {
                switch (vt) {
                    case VarType::Str:  dflt = RuntimeValue::make_str(""); break;
                    case VarType::Int:  dflt = RuntimeValue::make_int(0); break;
                    case VarType::Bool: dflt = RuntimeValue::make_bool(false); break;
                    case VarType::List: dflt = RuntimeValue::make_list(); break;
                    case VarType::Map:  dflt = RuntimeValue::make_map(); break;
                }
            }
            state.vars.declare(kv.first, vt, dflt);
        }
    }

    // skip / limit.
    if (auto it = root.find("skip"); it != root.end() && it->second.is_number())
        state.skip_n = (int64_t)it->second.as_number();
    if (auto it = root.find("limit"); it != root.end() && it->second.is_number())
        state.limit = (int64_t)it->second.as_number();
    if (auto it = root.find("limit_per_file"); it != root.end() && it->second.is_number())
        state.limit_per_file = (int64_t)it->second.as_number();

    // group_by, rank.
    if (auto it = root.find("group_by"); it != root.end() && it->second.is_string())
        state.group_by = it->second.as_string();
    if (auto it = root.find("rank"); it != root.end() && it->second.is_bool())
        state.rank_enabled = it->second.as_bool();

    // Byte budgets (top-level; applied during populate_match_ctx, the block
    // action, and emit_record_string).
    auto take_u64 = [&](const char *key, uint64_t &dst) {
        if (auto it = root.find(key); it != root.end() && it->second.is_number()) {
            double n = it->second.as_number();
            if (n > 0) dst = static_cast<uint64_t>(n);
        }
    };
    take_u64("max_match_bytes",   state.max_match_bytes);
    take_u64("max_context_bytes", state.max_context_bytes);
    take_u64("max_block_bytes",   state.max_block_bytes);
    take_u64("max_output_bytes",  state.max_output_bytes);

    // Pattern relations (top-level `relations`). Resolution to pattern
    // indices happens lazily inside run_phase against the active phase's
    // pattern list.
    if (auto it = root.find("relations"); it != root.end() && it->second.is_array()) {
        for (const auto &rv : it->second.as_array()) {
            if (!rv.is_object()) {
                std::fprintf(stderr, "hprscript: 'relations' entries must be objects\n");
                return 2;
            }
            ScriptState::Relation rel;
            const json::Value *kv = rv.find("kind");
            if (!kv || !kv->is_string()) {
                std::fprintf(stderr, "hprscript: relation needs 'kind' (\"near\"|\"far\")\n");
                return 2;
            }
            const std::string &k = kv->as_string();
            if (k == "near") rel.near = true;
            else if (k == "far") rel.near = false;
            else {
                std::fprintf(stderr, "hprscript: unknown relation kind '%s'\n", k.c_str());
                return 2;
            }
            const json::Value *av = rv.find("a");
            const json::Value *bv = rv.find("b");
            const json::Value *lv = rv.find("lines");
            if (!av || !av->is_string() || !bv || !bv->is_string() ||
                !lv || !lv->is_number()) {
                std::fprintf(stderr, "hprscript: relation needs string a, string b, number lines\n");
                return 2;
            }
            rel.a = av->as_string();
            rel.b = bv->as_string();
            rel.lines = (int)lv->as_number();
            if (rel.lines < 0) rel.lines = 0;
            state.relations.push_back(std::move(rel));
        }
    }

    // Enclosing-scope configuration: either a string ("go", "auto", …) or an
    // object {pattern, open, close, kind} for custom anchors.
    if (auto it = root.find("scope"); it != root.end()) {
        const json::Value &sv = it->second;
        if (sv.is_string()) {
            state.scope_lang = sv.as_string();
        } else if (sv.is_object()) {
            if (const json::Value *p = sv.find("pattern"); p && p->is_string())
                state.scope_custom.anchor_regex = p->as_string();
            if (const json::Value *o = sv.find("open"); o && o->is_string())
                state.scope_custom.open = o->as_string();
            if (const json::Value *c = sv.find("close"); c && c->is_string())
                state.scope_custom.close = c->as_string();
            if (const json::Value *k = sv.find("kind"); k && k->is_string())
                state.scope_custom.kind = k->as_string();
            if (state.scope_custom.kind.empty()) state.scope_custom.kind = "func";
        }
    }

    // Top-level context (used as default when a phase doesn't set its own
    // and when the script has no `phases` block — built into the synthetic
    // single phase below).
    int top_ctx_before = 0, top_ctx_after = 0;
    if (auto it = root.find("context"); it != root.end() && it->second.is_number()) {
        int n = (int)it->second.as_number();
        top_ctx_before = top_ctx_after = n;
    }
    if (auto it = root.find("context_before"); it != root.end() && it->second.is_number())
        top_ctx_before = (int)it->second.as_number();
    if (auto it = root.find("context_after"); it != root.end() && it->second.is_number())
        top_ctx_after = (int)it->second.as_number();

    std::vector<CompiledPhase> phases;

    // Compile phases (or synthesise one from top-level patterns).
    if (auto it = root.find("phases"); it != root.end() && it->second.is_array()) {
        if (root.count("patterns")) {
            std::fprintf(stderr, "hprscript: cannot use both 'phases' and top-level 'patterns'\n");
            return 2;
        }
        for (const auto &pv : it->second.as_array()) {
            CompiledPhase ph;
            std::string err;
            if (!compile_phase(pv, root, ph, err)) {
                std::fprintf(stderr, "hprscript: %s\n", err.c_str());
                return 2;
            }
            phases.push_back(std::move(ph));
        }
        if (phases.empty()) {
            std::fprintf(stderr, "hprscript: 'phases' is empty\n");
            return 2;
        }
    } else {
        const json::Value *pp = nullptr;
        if (auto it2 = root.find("patterns"); it2 != root.end()) pp = &it2->second;
        if (!pp || !pp->is_array() || pp->as_array().empty()) {
            std::fprintf(stderr, "hprscript: script needs 'patterns' or 'phases'\n");
            return 2;
        }
        CompiledPhase ph;
        ph.id = "_default";
        for (size_t i = 0; i < pp->as_array().size(); ++i) {
            CompiledPattern cp;
            std::string err;
            if (!compile_pattern(pp->as_array()[i], i, cp, err)) {
                std::fprintf(stderr, "hprscript: %s\n", err.c_str());
                return 2;
            }
            ph.patterns.push_back(std::move(cp));
        }
        std::string merr;
        ph.matcher = build_matcher(ph.patterns, merr);
        if (!ph.matcher) {
            std::fprintf(stderr, "hprscript: pattern compile failed: %s\n", merr.c_str());
            return 2;
        }
        {
            std::vector<Pattern> ps;
            ps.reserve(ph.patterns.size());
            for (const auto &c : ph.patterns) ps.push_back(c.pat);
            auto et = std::make_shared<ExtractTable>();
            std::string ee;
            int ee_idx = -1;
            if (!et->build(ps, &ee, &ee_idx)) {
                std::fprintf(stderr, "hprscript: pattern '%s': %s\n",
                             ee_idx >= 0 ? ph.patterns[ee_idx].pat.id.c_str() : "?",
                             ee.c_str());
                return 2;
            }
            if (et->any()) ph.extract_table = et;
        }
        ph.ctx_before = top_ctx_before;
        ph.ctx_after = top_ctx_after;
        if (auto it2 = root.find("scan"); it2 != root.end() && it2->second.is_array()) {
            for (const auto &g : it2->second.as_array()) {
                if (g.is_string()) ph.scan_globs.push_back(g.as_string());
            }
        }
        if (auto it2 = root.find("exclude"); it2 != root.end() && it2->second.is_array()) {
            for (const auto &g : it2->second.as_array()) {
                if (g.is_string()) ph.excludes.push_back(g.as_string());
            }
        }
        phases.push_back(std::move(ph));
    }

    // Track per-pattern weights (for rank). After compile.
    if (state.rank_enabled) {
        std::set<std::string> queried_ids;
        for (const auto &ph : phases) {
            for (const auto &p : ph.patterns) {
                state.pattern_weights[p.pat.id] = p.pat.weight;
                if (!p.absent) queried_ids.insert(p.pat.id);
            }
        }
        state.rank_total_queried = (uint32_t)queried_ids.size();
    }

    // Top-level on_file_end / on_complete (run after each phase / at end).
    std::vector<Action> top_on_file_end;
    std::vector<Action> top_on_complete;
    if (auto it = root.find("on_file_end"); it != root.end() && it->second.is_array()) {
        std::string err;
        if (!compile_action_list(it->second, top_on_file_end, "on_file_end", err)) {
            std::fprintf(stderr, "hprscript: %s\n", err.c_str());
            return 2;
        }
    }
    if (auto it = root.find("on_complete"); it != root.end() && it->second.is_array()) {
        std::string err;
        if (!compile_action_list(it->second, top_on_complete, "on_complete", err)) {
            std::fprintf(stderr, "hprscript: %s\n", err.c_str());
            return 2;
        }
    }

    // Override scan: positional args after -s / -script / positional script.
    std::vector<std::string> override_scan;
    {
        size_t pos_start;
        if (!cli.script_inline.empty() || !cli.script_path.empty()) {
            pos_start = 0;
        } else {
            pos_start = 1;
        }
        for (size_t i = pos_start; i < cli.positional.size(); ++i) {
            override_scan.push_back(cli.positional[i]);
        }
    }

    // Decide stdin mode.
    bool any_scan_set = !override_scan.empty();
    if (!any_scan_set) {
        for (const auto &ph : phases) {
            if (!ph.scan_globs.empty()) { any_scan_set = true; break; }
        }
    }
    bool reading_stdin = !any_scan_set && !isatty(fileno(stdin));
    std::string stdin_content;
    if (reading_stdin) {
        if (!slurp_stdin(stdin_content)) {
            std::fprintf(stderr, "hprscript: failed to read stdin\n");
            return 2;
        }
    }

    // Run phases sequentially.
    for (size_t pi = 0; pi < phases.size(); ++pi) {
        if (state.stop_all) break;
        const CompiledPhase &ph = phases[pi];
        bool last_phase = (pi + 1 == phases.size());
        run_phase(ph, state, nullptr,
                  last_phase ? top_on_file_end : std::vector<Action>{},
                  override_scan, reading_stdin, "<stdin>", stdin_content);
        // Phase-level on_complete runs after each phase.
        if (!ph.on_complete.empty()) {
            ExecCtx ec;
            ec.vars = &state.vars;
            ec.state = &state;
            ec.has_match = false;
            execute_actions(ph.on_complete, ec);
        }
        // Reset stop_file flag between phases (but keep stop_all).
        state.stop_file = false;
    }

    // Script-level on_complete after all phases.
    if (!top_on_complete.empty()) {
        ExecCtx ec;
        ec.vars = &state.vars;
        ec.state = &state;
        ec.has_match = false;
        execute_actions(top_on_complete, ec);
    }

    // Flush group_by buffer, then rank table.
    flush_groups(state);
    flush_rank(state);

    if (state.output_truncated) {
        std::fprintf(stdout, "{\"info\":\"output_truncated\",\"emitted\":%lld}\n",
                     (long long)state.emitted);
    }
    std::fflush(stdout);
    bool any_output = state.emitted > 0 || !state.rank_per_file.empty();
    return any_output ? 0 : 1;
}

} // namespace hpr
