#pragma once

#include "cli.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct PlannedEdit {
    uint64_t site_id = 0;
    std::string pattern_id;
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t line_start = 0;
    uint32_t line_end = 0;
    std::string old_sha256;
    std::string old_bytes;
    std::string replacement;
    std::string operation;
    std::string scope_name;
};

struct PlannedFile {
    std::string path;
    std::string planned_absolute_path;
    bool symlink = false;
    std::string symlink_target;
    uint64_t original_size = 0;
    std::string original_sha256;
    uint32_t mode = 0644;
    std::vector<PlannedEdit> edits;
};

struct EditPlan {
    std::string schema = "hprscript-edit-plan";
    uint32_t version = 1;
    std::string tool_version;
    std::string created_at;
    std::string working_root;
    std::vector<std::string> command;
    uint64_t site_count = 0;
    uint64_t file_count = 0;
    std::vector<PlannedFile> files;
};

std::string sha256_hex(std::string_view bytes);
std::string base64_encode(std::string_view bytes);
bool base64_decode(std::string_view encoded, std::string &bytes,
                   std::string &error);

bool write_edit_plan(const EditPlan &plan, const std::string &path,
                     std::string &error);
bool read_edit_plan(const std::string &path, EditPlan &plan,
                    std::string &error);

// Apply exact stored byte ranges without running a matcher. Returns the
// public apply exit codes: 0 complete, 2 pre-write system/staging error,
// 3 verification refusal (no writes), 4 partially committed.
int apply_edit_plan(const EditPlan &plan, const ApplyOptions &options,
                    bool emit_receipts = true);
int run_apply(const Cli &cli);

} // namespace hpr
