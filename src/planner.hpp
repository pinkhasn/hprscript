#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hpr {

// Stable, presentation-neutral execution plan shared by quick search,
// investigation, query, and legacy script phases. Keep implementation
// details out of this model: it is a machine-readable explanation contract.
struct PlanStage {
    std::string id;
    std::vector<std::string> sets;
    uint64_t patterns = 0;
    std::vector<std::string> inputs;
    std::string scope;
    bool adaptive = false;
};

struct PlanOperation {
    std::string op;
    std::map<std::string, std::string> attributes;
};

struct ExecutionPlan {
    std::string mode;
    std::vector<PlanStage> scan_stages;
    std::vector<PlanOperation> postprocess;
    std::map<std::string, uint64_t> limits;
};

// Emit one deterministic JSONL execution-plan record to stdout.
void emit_execution_plan(const ExecutionPlan &plan);

} // namespace hpr
