#include "planner.hpp"

#include "output.hpp"

#include <cstdio>

namespace hpr {

namespace {

void append_string(std::string &out, const std::string &value) {
    out += '"';
    json_escape_to(out, value);
    out += '"';
}

void append_strings(std::string &out, const std::vector<std::string> &values) {
    out += '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ',';
        append_string(out, values[i]);
    }
    out += ']';
}

} // namespace

void emit_execution_plan(const ExecutionPlan &plan) {
    std::string out = "{\"type\":\"execution-plan\",\"mode\":";
    append_string(out, plan.mode);
    out += ",\"scan_stages\":[";
    for (size_t i = 0; i < plan.scan_stages.size(); ++i) {
        if (i) out += ',';
        const auto &stage = plan.scan_stages[i];
        out += "{\"id\":";
        append_string(out, stage.id);
        out += ",\"sets\":";
        append_strings(out, stage.sets);
        out += ",\"patterns\":" + std::to_string(stage.patterns);
        out += ",\"inputs\":";
        append_strings(out, stage.inputs);
        if (!stage.scope.empty()) {
            out += ",\"scope\":";
            append_string(out, stage.scope);
        }
        if (stage.adaptive) out += ",\"adaptive\":true";
        out += '}';
    }
    out += "],\"postprocess\":[";
    for (size_t i = 0; i < plan.postprocess.size(); ++i) {
        if (i) out += ',';
        const auto &operation = plan.postprocess[i];
        out += "{\"op\":";
        append_string(out, operation.op);
        for (const auto &[key, value] : operation.attributes) {
            out += ',';
            append_string(out, key);
            out += ':';
            append_string(out, value);
        }
        out += '}';
    }
    out += "],\"limits\":{";
    size_t li = 0;
    for (const auto &[key, value] : plan.limits) {
        if (li++) out += ',';
        append_string(out, key);
        out += ':' + std::to_string(value);
    }
    out += "}}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

} // namespace hpr
