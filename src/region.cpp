#include "region.hpp"

#include "strutil.hpp"

#include <algorithm>
#include <cstdio>

namespace hpr {

namespace {

// Labels land in one-line records and bracket tags, so they are cut to the
// first line and to a bounded length.
constexpr size_t kMaxLabelBytes = 120;

std::string tidy_label(std::string_view s) {
    size_t nl = s.find_first_of("\r\n");
    if (nl != std::string_view::npos) s = s.substr(0, nl);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.size() > kMaxLabelBytes) {
        size_t n = kMaxLabelBytes;
        // Back off to a UTF-8 lead byte so a codepoint is never split.
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s = s.substr(0, n);
    }
    return std::string(s);
}

// Heuristic: does the regex text contain a capturing group? Only used to
// decide whether a std::regex compile failure deserves a warning.
bool has_capture_group(const std::string &re) {
    for (size_t i = 0; i < re.size(); ++i) {
        if (re[i] == '\\') { ++i; continue; }
        if (re[i] != '(') continue;
        if (i + 1 >= re.size() || re[i + 1] != '?') return true;
        // Named groups capture too: (?<name>…) / (?P<name>…), but not the
        // (?<= / (?<! lookbehind spellings.
        if (re.compare(i, 4, "(?P<") == 0) return true;
        if (re.compare(i, 3, "(?<") == 0 && i + 3 < re.size() &&
            re[i + 3] != '=' && re[i + 3] != '!')
            return true;
    }
    return false;
}

} // namespace

bool RegionSet::init(const std::vector<RegionDef> &defs, bool utf8,
                     std::string *err) {
    defs_ = defs;
    patterns_.clear();
    slots_.clear();
    label_re_.assign(defs_.size(), std::regex());
    has_label_re_.assign(defs_.size(), 0);
    if (defs_.empty()) return true;

    for (size_t d = 0; d < defs_.size(); ++d) {
        const RegionDef &def = defs_[d];
        Pattern open;
        open.id = def.id + ".open";
        open.regexp = def.open;
        open.utf8 = utf8;
        patterns_.push_back(std::move(open));
        slots_.push_back({static_cast<uint32_t>(d), false});
        if (!def.close.empty()) {
            Pattern close;
            close.id = def.id + ".close";
            close.regexp = def.close;
            close.utf8 = utf8;
            patterns_.push_back(std::move(close));
            slots_.push_back({static_cast<uint32_t>(d), true});
        }

        // Label extraction re-runs the open regex with std::regex over the
        // opening match (Vectorscan reports no capture groups). A leading
        // (?i) is the one inline flag translated; anything else std::regex
        // rejects degrades to "label = first line of the opening match".
        std::string re = def.open;
        auto flags = std::regex::ECMAScript;
        if (re.compare(0, 4, "(?i)") == 0) {
            re.erase(0, 4);
            flags |= std::regex::icase;
        }
        try {
            label_re_[d] = std::regex(re, flags);
            has_label_re_[d] = label_re_[d].mark_count() > 0 ? 1 : 0;
        } catch (const std::regex_error &e) {
            if (has_capture_group(def.open)) {
                std::fprintf(stderr,
                             "hprscript: -region %s: label capture "
                             "unavailable (open regex is not std::regex-"
                             "compatible: %s); labels fall back to the "
                             "opening match text\n",
                             def.id.c_str(), e.what());
            }
        }
    }

    CompileError ce;
    if (!matcher_.compile(patterns_, &ce)) {
        if (err) {
            *err = "region delimiter compile failed: " + ce.message;
            if (ce.pattern_index >= 0 &&
                static_cast<size_t>(ce.pattern_index) < patterns_.size()) {
                const Slot &s = slots_[static_cast<size_t>(ce.pattern_index)];
                *err += "\n  in -region " + defs_[s.def].id +
                        (s.is_close ? " close: " : " open: ") +
                        patterns_[static_cast<size_t>(ce.pattern_index)].regexp;
            }
        }
        return false;
    }
    return true;
}

int RegionSet::find(const std::string &id) const {
    for (size_t d = 0; d < defs_.size(); ++d)
        if (defs_[d].id == id) return static_cast<int>(d);
    return -1;
}

std::string RegionSet::label_for(uint32_t def,
                                 std::string_view open_text) const {
    if (has_label_re_[def] && open_text.size() <= kMaxStdRegexInput) {
        std::cmatch m;
        if (std::regex_search(open_text.data(),
                              open_text.data() + open_text.size(), m,
                              label_re_[def])) {
            for (size_t g = 1; g < m.size(); ++g) {
                if (m[g].matched)
                    return tidy_label(std::string_view(
                        m[g].first, static_cast<size_t>(m[g].length())));
            }
        }
    }
    return tidy_label(open_text);
}

void RegionIndex::build(std::string_view buf, RegionSet &set,
                        const LineIndex &idx) {
    set_ = &set;
    ranges_.clear();
    const size_t ndefs = set.defs_.size();
    def_begin_.assign(ndefs + 1, 0);
    if (ndefs == 0 || buf.empty()) return;

    std::vector<Match> raw;
    Matcher::MatchCb cb = [&](const Match &m) -> bool {
        raw.push_back(m);
        return true;
    };
    set.matcher_.scan(buf, cb);
    std::vector<Match> hits;
    dedup_leftmost_longest(raw, hits);

    // hits are sorted by (pattern, from): split them per definition/side.
    std::vector<std::vector<Match>> opens(ndefs), closes(ndefs);
    for (const Match &m : hits) {
        const RegionSet::Slot &s = set.slots_[m.pattern_index];
        (s.is_close ? closes : opens)[s.def].push_back(m);
    }

    auto open_region = [&](uint32_t d, const Match &o) -> RegionRange {
        RegionRange r;
        r.def = d;
        r.start_off = o.from;
        r.end_off = buf.size();
        r.label = set.label_for(
            d, std::string_view(buf.data() + o.from, o.to - o.from));
        return r;
    };

    for (uint32_t d = 0; d < ndefs; ++d) {
        def_begin_[d] = ranges_.size();
        const RegionDef &def = set.defs_[d];
        const auto &op = opens[d];
        const auto &cl = closes[d];

        if (def.close.empty()) {
            // Section mode: each open runs to the next open (or EOF).
            for (size_t i = 0; i < op.size(); ++i) {
                RegionRange r = open_region(d, op[i]);
                if (i + 1 < op.size()) r.end_off = op[i + 1].from;
                ranges_.push_back(std::move(r));
            }
        } else if (!def.nested) {
            // Range mode: open … first close at/after the open's end.
            size_t ci = 0;
            uint64_t pos = 0; // end of the previous region
            for (size_t oi = 0; oi < op.size(); ++oi) {
                const Match &o = op[oi];
                if (o.from < pos) continue; // inside the previous region
                while (ci < cl.size() && cl[ci].from < o.to) ++ci;
                RegionRange r = open_region(d, o);
                if (ci == cl.size()) {
                    r.closed = false; // runs to EOF; later opens are inside
                    ranges_.push_back(std::move(r));
                    break;
                }
                r.end_off = cl[ci].to;
                pos = cl[ci].to;
                ++ci;
                ranges_.push_back(std::move(r));
            }
        } else {
            // Nested mode: merge both sides in offset order and balance
            // them with a stack. A delimiter overlapping the previously
            // consumed one is skipped; at equal offsets a close wins while
            // something is open, so identical open/close text toggles.
            struct Ev {
                uint64_t from, to;
                bool is_close;
            };
            std::vector<Ev> evs;
            evs.reserve(op.size() + cl.size());
            for (const Match &m : op) evs.push_back({m.from, m.to, false});
            for (const Match &m : cl) evs.push_back({m.from, m.to, true});
            std::sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) {
                if (a.from != b.from) return a.from < b.from;
                return a.is_close && !b.is_close;
            });
            std::vector<size_t> stack;
            uint64_t last_to = 0;
            for (const Ev &e : evs) {
                if (e.from < last_to) continue;
                if (e.is_close) {
                    if (stack.empty()) continue; // stray close
                    RegionRange &r = ranges_[stack.back()];
                    r.end_off = e.to;
                    r.closed = true;
                    stack.pop_back();
                } else {
                    Match o;
                    o.from = e.from;
                    o.to = e.to;
                    RegionRange r = open_region(d, o);
                    r.closed = false; // until its close arrives
                    r.depth = static_cast<uint32_t>(stack.size()) + 1;
                    r.parent = stack.empty()
                                   ? -1
                                   : static_cast<int32_t>(stack.back());
                    stack.push_back(ranges_.size());
                    ranges_.push_back(std::move(r));
                }
                last_to = e.to;
            }
        }
    }
    def_begin_[ndefs] = ranges_.size();

    for (RegionRange &r : ranges_) {
        r.line_start = idx.line_of(r.start_off);
        r.line_end = idx.line_of(r.end_off > r.start_off ? r.end_off - 1
                                                         : r.start_off);
    }
}

std::vector<const RegionRange *> RegionIndex::in_file_order() const {
    std::vector<const RegionRange *> out;
    out.reserve(ranges_.size());
    for (const RegionRange &r : ranges_) out.push_back(&r);
    std::stable_sort(out.begin(), out.end(),
                     [](const RegionRange *a, const RegionRange *b) {
                         if (a->start_off != b->start_off)
                             return a->start_off < b->start_off;
                         return a->def < b->def;
                     });
    return out;
}

const RegionRange *RegionIndex::find_innermost(uint32_t def,
                                               uint64_t offset) const {
    if (def + 1 >= def_begin_.size()) return nullptr;
    auto first = ranges_.begin() + static_cast<std::ptrdiff_t>(def_begin_[def]);
    auto last = ranges_.begin() + static_cast<std::ptrdiff_t>(def_begin_[def + 1]);
    // Last region of this definition starting at or before `offset`.
    auto it = std::upper_bound(first, last, offset,
                               [](uint64_t off, const RegionRange &r) {
                                   return off < r.start_off;
                               });
    if (it == first) return nullptr;
    const RegionRange *r = &*(it - 1);
    // Regions of one definition are disjoint or properly nested, so any
    // region containing `offset` is `r` itself or one of its ancestors.
    while (r) {
        if (offset < r->end_off) return r;
        r = parent_of(*r);
    }
    return nullptr;
}

} // namespace hpr
