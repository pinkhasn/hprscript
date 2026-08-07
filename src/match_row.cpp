#include "match_row.hpp"

#include <algorithm>

namespace hpr {

MatchRow materialize_match_row(uint64_t row_id, uint32_t set_index,
                               const std::string &set_id,
                               const std::vector<Pattern> &patterns,
                               const Match &m, const std::string &file,
                               std::string_view content, const LineIndex &idx,
                               const ScopeIndex *scope,
                               const ExtractTable *extract) {
    MatchRow row;
    row.row_id = row_id;
    row.set_index = set_index;
    row.pattern_index = m.pattern_index;
    row.set_id = set_id;
    row.pattern_id = patterns[m.pattern_index].id;
    row.file = file;
    row.language = auto_lang_for_path(file);
    row.from = m.from;
    row.to = m.to;
    row.line = idx.line_of(m.from);
    row.column = idx.col_of(m.from);
    if (m.from <= m.to && m.to <= content.size())
        row.match.assign(content.data() + m.from,
                         static_cast<size_t>(m.to - m.from));
    std::string_view line = idx.line_text(row.line);
    row.context.assign(line.data(), line.size());

    if (extract && extract->has(m.pattern_index)) {
        std::vector<std::string> values;
        extract->extract(m.pattern_index, row.match, values);
        const auto &names = extract->names(m.pattern_index);
        for (size_t i = 0; i < names.size(); ++i) {
            std::string value = i < values.size() ? values[i] : std::string{};
            row.captures[names[i]] = RuntimeValue::make_str(std::move(value));
        }
    }
    if (scope) {
        if (const ScopeRange *s = scope->find_innermost(m.from)) {
            ScopeRef ref;
            ref.name = s->name;
            ref.kind = s->kind;
            ref.from = s->start_off;
            ref.to = s->end_off;
            ref.line_start = s->line_start;
            ref.line_end = s->line_end;
            row.enclosing = std::move(ref);
        }
    }
    return row;
}

RuntimeValue match_row_field(const MatchRow &r, const std::string &field,
                             bool *known) {
    auto yes = [&]() { if (known) *known = true; };
    auto no = [&]() { if (known) *known = false; return RuntimeValue::make_null(); };
    if (field == "row_id") { yes(); return RuntimeValue::make_int(r.row_id); }
    if (field == "set_id") { yes(); return RuntimeValue::make_str(r.set_id); }
    if (field == "pattern_id") { yes(); return RuntimeValue::make_str(r.pattern_id); }
    if (field == "file") { yes(); return RuntimeValue::make_str(r.file); }
    if (field == "language") { yes(); return RuntimeValue::make_str(r.language); }
    if (field == "from") { yes(); return RuntimeValue::make_int(r.from); }
    if (field == "to") { yes(); return RuntimeValue::make_int(r.to); }
    if (field == "line") { yes(); return RuntimeValue::make_int(r.line); }
    if (field == "column") { yes(); return RuntimeValue::make_int(r.column); }
    if (field == "match") { yes(); return RuntimeValue::make_str(r.match); }
    if (field == "context") { yes(); return RuntimeValue::make_str(r.context); }
    if (field == "derived.value") { yes(); return RuntimeValue::make_str(r.derived_value); }
    if (field == "derived.source_rows") {
        yes(); RuntimeValue out = RuntimeValue::make_list();
        for (uint64_t id : r.derived_source_rows)
            out.as_list().push_back(RuntimeValue::make_int(id));
        return out;
    }
    static const std::string cap = "capture.";
    if (field.compare(0, cap.size(), cap) == 0) {
        yes();
        auto it = r.captures.find(field.substr(cap.size()));
        return it == r.captures.end() ? RuntimeValue::make_null() : it->second;
    }
    static const std::string enc = "enclosing.";
    if (field.compare(0, enc.size(), enc) == 0) {
        yes();
        if (!r.enclosing) return RuntimeValue::make_null();
        const std::string sub = field.substr(enc.size());
        if (sub == "name") return RuntimeValue::make_str(r.enclosing->name);
        if (sub == "kind") return RuntimeValue::make_str(r.enclosing->kind);
        if (sub == "from") return RuntimeValue::make_int(r.enclosing->from);
        if (sub == "to") return RuntimeValue::make_int(r.enclosing->to);
        if (sub == "line_start") return RuntimeValue::make_int(r.enclosing->line_start);
        if (sub == "line_end") return RuntimeValue::make_int(r.enclosing->line_end);
        return no();
    }
    return no();
}

} // namespace hpr
