#include "edit_plan.hpp"

#include "json.hpp"
#include "output.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HPRSCRIPT_VERSION
#define HPRSCRIPT_VERSION "unknown"
#endif

namespace hpr {
namespace {

uint32_t rotr(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    void update(std::string_view input) {
        total_ += input.size();
        for (unsigned char c : input) {
            block_[used_++] = c;
            if (used_ == block_.size()) {
                transform(block_.data());
                used_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        uint64_t bits = total_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < 64) block_[used_++] = 0;
            transform(block_.data());
            used_ = 0;
        }
        while (used_ < 56) block_[used_++] = 0;
        for (int i = 7; i >= 0; --i)
            block_[used_++] = static_cast<unsigned char>(bits >> (i * 8));
        transform(block_.data());
        std::array<unsigned char, 32> out{};
        for (size_t i = 0; i < state_.size(); ++i) {
            out[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
            out[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
            out[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
            out[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
        return out;
    }

private:
    void transform(const unsigned char *data) {
        static constexpr uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            size_t j = i * 4;
            w[i] = (uint32_t(data[j]) << 24) | (uint32_t(data[j + 1]) << 16) |
                   (uint32_t(data[j + 2]) << 8) | uint32_t(data[j + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (size_t i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }

    std::array<uint32_t, 8> state_{{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<unsigned char, 64> block_{};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

void append_json_string(std::string &out, std::string_view value) {
    out += '"';
    json_escape_to(out, value);
    out += '"';
}

std::string utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool read_bytes(const std::string &path, std::string &out, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read failed for " + path;
        return false;
    }
    return true;
}

bool beneath_root(const std::filesystem::path &root,
                  const std::filesystem::path &target) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(target, root, ec);
    if (ec || rel.empty() || rel.is_absolute()) return false;
    for (const auto &part : rel)
        if (part == "..") return false;
    return true;
}

const json::Value *required(const json::Object &object,
                            const char *key, json::Value::Type type,
                            std::string &error) {
    auto it = object.find(key);
    if (it == object.end()) {
        error = std::string("plan missing field '") + key + "'";
        return nullptr;
    }
    if (it->second.type() != type) {
        error = std::string("plan field '") + key + "' has wrong type";
        return nullptr;
    }
    return &it->second;
}

bool number_u64(const json::Object &object, const char *key,
                uint64_t &out, std::string &error) {
    const json::Value *v = required(object, key, json::Value::Number, error);
    if (!v || v->as_number() < 0 || v->as_number() > 9007199254740991.0) {
        if (v) error = std::string("plan field '") + key + "' is out of range";
        return false;
    }
    out = static_cast<uint64_t>(v->as_number());
    return true;
}

void emit_guard(const std::string &file, const std::string &message) {
    std::string out = "{\"type\":\"guard\",\"guard\":\"plan-verification\"";
    if (!file.empty()) {
        out += ",\"file\":";
        append_json_string(out, file);
    }
    out += ",\"message\":";
    append_json_string(out, message);
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

void emit_receipt(const std::string &file, const char *status,
                  const std::string &temporary, bool json_output) {
    if (!json_output) {
        std::printf("%s %s%s%s\n", status, file.c_str(),
                    temporary.empty() ? "" : " temp=",
                    temporary.empty() ? "" : temporary.c_str());
        return;
    }
    std::string out = "{\"type\":\"apply-file\",\"file\":";
    append_json_string(out, file);
    out += ",\"status\":";
    append_json_string(out, status);
    if (!temporary.empty()) {
        out += ",\"temporary\":";
        append_json_string(out, temporary);
    }
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

void emit_apply_summary(const char *status, uint64_t applied,
                        uint64_t not_applied,
                        const std::vector<std::string> &temporaries,
                        bool json_output) {
    if (!json_output) {
        std::printf("apply %s: %llu applied, %llu not applied\n", status,
                    (unsigned long long)applied,
                    (unsigned long long)not_applied);
        if (!temporaries.empty())
            std::printf("recovery: inspect the listed staged files before retrying\n");
        return;
    }
    std::string out = "{\"type\":\"apply-summary\",\"status\":";
    append_json_string(out, status);
    out += ",\"files_applied\":" + std::to_string(applied);
    out += ",\"files_not_applied\":" + std::to_string(not_applied);
    out += ",\"temporary_files\":[";
    for (size_t i = 0; i < temporaries.size(); ++i) {
        if (i) out += ',';
        append_json_string(out, temporaries[i]);
    }
    out += ']';
    if (!temporaries.empty()) {
        out += ",\"recovery\":\"inspect staged temporary files, then retry or replace targets manually\"";
    }
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

bool fault_at(const char *name, size_t ordinal) {
    const char *enabled = std::getenv("HPRSCRIPT_ENABLE_FAULT_INJECTION");
    if (!enabled || std::strcmp(enabled, "1") != 0) return false;
    const char *value = std::getenv(name);
    if (!value) return false;
    char *end = nullptr;
    unsigned long n = std::strtoul(value, &end, 10);
    return end != value && *end == '\0' && n == ordinal;
}

struct VerifiedFile {
    const PlannedFile *planned = nullptr;
    std::string logical_path;
    std::string write_path;
    std::string original;
    std::string result;
    bool changed = false;
};

struct StagedFile {
    VerifiedFile *verified = nullptr;
    std::string temporary;
    bool renamed = false;
};

bool stage_file(VerifiedFile &vf, StagedFile &staged, std::string &error,
                size_t ordinal) {
    if (fault_at("HPRSCRIPT_TEST_FAIL_STAGE_N", ordinal)) {
        error = "fault injection: staging failed";
        return false;
    }
    if (fault_at("HPRSCRIPT_TEST_FAIL_OPEN_N", ordinal)) {
        error = "fault injection: temporary-file open failed";
        return false;
    }
    std::filesystem::path target(vf.write_path);
    std::string dir = target.parent_path().empty() ? "." : target.parent_path().string();
    std::string templ = dir + "/.hpr-apply." + target.filename().string() + ".XXXXXX";
    std::vector<char> name(templ.begin(), templ.end());
    name.push_back('\0');
    int fd = ::mkstemp(name.data());
    if (fd < 0) {
        error = "cannot stage near " + vf.write_path + ": " + std::strerror(errno);
        return false;
    }
    staged.verified = &vf;
    staged.temporary = name.data();
    size_t offset = 0;
    bool ok = !fault_at("HPRSCRIPT_TEST_FAIL_WRITE_N", ordinal);
    if (!ok) error = "fault injection: temporary-file write failed";
    while (offset < vf.result.size()) {
        if (!ok) break;
        ssize_t n = ::write(fd, vf.result.data() + offset,
                            vf.result.size() - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            error = "write failed for " + staged.temporary + ": " +
                    std::strerror(errno);
            ok = false;
            break;
        }
        offset += static_cast<size_t>(n);
    }
    if (ok && (fault_at("HPRSCRIPT_TEST_FAIL_CHMOD_N", ordinal) ||
               ::fchmod(fd, vf.planned->mode) != 0)) {
        error = fault_at("HPRSCRIPT_TEST_FAIL_CHMOD_N", ordinal)
            ? "fault injection: temporary-file chmod failed"
            : "fchmod failed for " + staged.temporary + ": " + std::strerror(errno);
        ok = false;
    }
    if (ok && (fault_at("HPRSCRIPT_TEST_FAIL_FSYNC_N", ordinal) ||
               ::fsync(fd) != 0)) {
        error = fault_at("HPRSCRIPT_TEST_FAIL_FSYNC_N", ordinal)
            ? "fault injection: temporary-file fsync failed"
            : "fsync failed for " + staged.temporary + ": " + std::strerror(errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        error = "close failed for " + staged.temporary + ": " +
                std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        ::unlink(staged.temporary.c_str());
        staged.temporary.clear();
    }
    return ok;
}

} // namespace

std::string sha256_hex(std::string_view bytes) {
    Sha256 sha;
    sha.update(bytes);
    auto digest = sha.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t value = uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
        bool have2 = i + 1 < bytes.size();
        bool have3 = i + 2 < bytes.size();
        if (have2) value |= uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (have3) value |= uint32_t(static_cast<unsigned char>(bytes[i + 2]));
        out += table[(value >> 18) & 63];
        out += table[(value >> 12) & 63];
        out += have2 ? table[(value >> 6) & 63] : '=';
        out += have3 ? table[value & 63] : '=';
    }
    return out;
}

bool base64_decode(std::string_view encoded, std::string &bytes,
                   std::string &error) {
    static const std::array<int, 256> decode = [] {
        std::array<int, 256> map{};
        map.fill(-1);
        const char *alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; alphabet[i]; ++i)
            map[static_cast<unsigned char>(alphabet[i])] = i;
        return map;
    }();
    bytes.clear();
    if (encoded.size() % 4 != 0) {
        error = "base64 length is not a multiple of four";
        return false;
    }
    for (size_t i = 0; i < encoded.size(); i += 4) {
        int v[4];
        for (int j = 0; j < 4; ++j) {
            unsigned char c = static_cast<unsigned char>(encoded[i + j]);
            v[j] = c == '=' ? -2 : decode[c];
            if (v[j] == -1) {
                error = "base64 contains an invalid character";
                return false;
            }
        }
        if (v[0] < 0 || v[1] < 0 || (v[2] == -2 && v[3] != -2) ||
            (i + 4 != encoded.size() && (v[2] == -2 || v[3] == -2))) {
            error = "base64 padding is invalid";
            return false;
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12);
        if (v[2] >= 0) n |= uint32_t(v[2]) << 6;
        if (v[3] >= 0) n |= uint32_t(v[3]);
        bytes += static_cast<char>((n >> 16) & 0xff);
        if (v[2] >= 0) bytes += static_cast<char>((n >> 8) & 0xff);
        if (v[3] >= 0) bytes += static_cast<char>(n & 0xff);
    }
    return true;
}

bool write_edit_plan(const EditPlan &input, const std::string &path,
                     std::string &error) {
    EditPlan plan = input;
    if (plan.tool_version.empty()) plan.tool_version = HPRSCRIPT_VERSION;
    if (plan.created_at.empty()) plan.created_at = utc_now();
    std::string out = "{\n  \"schema\": \"hprscript-edit-plan\",\n  \"version\": 1,\n  \"tool_version\": ";
    append_json_string(out, plan.tool_version);
    out += ",\n  \"created_at\": "; append_json_string(out, plan.created_at);
    out += ",\n  \"working_root\": "; append_json_string(out, plan.working_root);
    out += ",\n  \"command\": [";
    for (size_t i = 0; i < plan.command.size(); ++i) {
        if (i) out += ", ";
        append_json_string(out, plan.command[i]);
    }
    out += "],\n  \"selection\": {\"site_count\": " +
           std::to_string(plan.site_count) + ", \"file_count\": " +
           std::to_string(plan.file_count) + "},\n  \"files\": [\n";
    for (size_t fi = 0; fi < plan.files.size(); ++fi) {
        const auto &file = plan.files[fi];
        out += "    {\"path\": "; append_json_string(out, file.path);
        out += ", \"planned_absolute_path\": ";
        append_json_string(out, file.planned_absolute_path);
        out += std::string(", \"symlink\": ") + (file.symlink ? "true" : "false");
        out += ", \"symlink_target\": ";
        if (file.symlink) append_json_string(out, file.symlink_target); else out += "null";
        out += ", \"original_size\": " + std::to_string(file.original_size);
        out += ", \"original_sha256\": "; append_json_string(out, file.original_sha256);
        out += ", \"mode\": " + std::to_string(file.mode) + ", \"edits\": [";
        for (size_t ei = 0; ei < file.edits.size(); ++ei) {
            const auto &edit = file.edits[ei];
            if (ei) out += ',';
            out += "{\"site_id\":" + std::to_string(edit.site_id);
            out += ",\"pattern_id\":"; append_json_string(out, edit.pattern_id);
            out += ",\"from\":" + std::to_string(edit.from);
            out += ",\"to\":" + std::to_string(edit.to);
            out += ",\"line_start\":" + std::to_string(edit.line_start);
            out += ",\"line_end\":" + std::to_string(edit.line_end);
            out += ",\"old_sha256\":"; append_json_string(out, edit.old_sha256);
            out += ",\"old_base64\":"; append_json_string(out, base64_encode(edit.old_bytes));
            out += ",\"replacement_base64\":"; append_json_string(out, base64_encode(edit.replacement));
            out += ",\"operation\":"; append_json_string(out, edit.operation);
            out += ",\"scope_name\":"; append_json_string(out, edit.scope_name);
            out += '}';
        }
        out += "]}";
        if (fi + 1 != plan.files.size()) out += ',';
        out += '\n';
    }
    out += "  ]\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create plan " + path + ": " + std::strerror(errno);
        return false;
    }
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    file.close();
    if (!file) {
        error = "cannot write plan " + path;
        return false;
    }
    return true;
}

bool read_edit_plan(const std::string &path, EditPlan &plan,
                    std::string &error) {
    std::string text;
    if (!read_bytes(path, text, error)) return false;
    auto parsed = json::parse(text);
    if (!parsed.ok || !parsed.value.is_object()) {
        error = parsed.ok ? "plan root must be an object"
                          : "invalid plan JSON: " + parsed.error;
        return false;
    }
    const auto &root = parsed.value.as_object();
    const auto *schema = required(root, "schema", json::Value::String, error);
    uint64_t version = 0;
    if (!schema || schema->as_string() != "hprscript-edit-plan" ||
        !number_u64(root, "version", version, error)) {
        if (error.empty()) error = "unsupported plan schema";
        return false;
    }
    if (version != 1) {
        error = "unsupported edit-plan version " + std::to_string(version);
        return false;
    }
    plan = EditPlan{};
    const auto *tool = required(root, "tool_version", json::Value::String, error);
    const auto *created = required(root, "created_at", json::Value::String, error);
    const auto *working = required(root, "working_root", json::Value::String, error);
    const auto *command = required(root, "command", json::Value::ArrayT, error);
    const auto *selection = required(root, "selection", json::Value::ObjectT, error);
    const auto *files = required(root, "files", json::Value::ArrayT, error);
    if (!tool || !created || !working || !command || !selection || !files) return false;
    plan.tool_version = tool->as_string();
    plan.created_at = created->as_string();
    plan.working_root = working->as_string();
    for (const auto &arg : command->as_array()) {
        if (!arg.is_string()) { error = "plan command entries must be strings"; return false; }
        plan.command.push_back(arg.as_string());
    }
    const auto &sel = selection->as_object();
    if (!number_u64(sel, "site_count", plan.site_count, error) ||
        !number_u64(sel, "file_count", plan.file_count, error)) return false;

    std::set<uint64_t> site_ids;
    for (const auto &fv : files->as_array()) {
        if (!fv.is_object()) { error = "plan files entries must be objects"; return false; }
        const auto &fo = fv.as_object();
        PlannedFile file;
        const auto *p = required(fo, "path", json::Value::String, error);
        const auto *absolute = required(fo, "planned_absolute_path", json::Value::String, error);
        const auto *symlink = required(fo, "symlink", json::Value::Bool, error);
        const auto *hash = required(fo, "original_sha256", json::Value::String, error);
        const auto *edits = required(fo, "edits", json::Value::ArrayT, error);
        uint64_t mode = 0;
        if (!p || !absolute || !symlink || !hash || !edits ||
            !number_u64(fo, "original_size", file.original_size, error) ||
            !number_u64(fo, "mode", mode, error)) return false;
        file.path = p->as_string();
        file.planned_absolute_path = absolute->as_string();
        file.symlink = symlink->as_bool();
        file.original_sha256 = hash->as_string();
        file.mode = static_cast<uint32_t>(mode);
        if (file.original_sha256.size() != 64) {
            error = "invalid original_sha256 for " + file.path;
            return false;
        }
        if (file.symlink) {
            const auto *target = required(fo, "symlink_target", json::Value::String, error);
            if (!target) return false;
            file.symlink_target = target->as_string();
        }
        for (const auto &ev : edits->as_array()) {
            if (!ev.is_object()) { error = "plan edits entries must be objects"; return false; }
            const auto &eo = ev.as_object();
            PlannedEdit edit;
            uint64_t ls=0, le=0;
            const auto *pattern = required(eo, "pattern_id", json::Value::String, error);
            const auto *old_hash = required(eo, "old_sha256", json::Value::String, error);
            const auto *old64 = required(eo, "old_base64", json::Value::String, error);
            const auto *replacement64 = required(eo, "replacement_base64", json::Value::String, error);
            const auto *operation = required(eo, "operation", json::Value::String, error);
            const auto *scope = required(eo, "scope_name", json::Value::String, error);
            if (!pattern || !old_hash || !old64 || !replacement64 || !operation || !scope ||
                !number_u64(eo, "site_id", edit.site_id, error) ||
                !number_u64(eo, "from", edit.from, error) ||
                !number_u64(eo, "to", edit.to, error) ||
                !number_u64(eo, "line_start", ls, error) ||
                !number_u64(eo, "line_end", le, error)) return false;
            if (!site_ids.insert(edit.site_id).second) {
                error = "duplicate site_id " + std::to_string(edit.site_id);
                return false;
            }
            edit.pattern_id = pattern->as_string();
            edit.old_sha256 = old_hash->as_string();
            edit.operation = operation->as_string();
            edit.scope_name = scope->as_string();
            edit.line_start = static_cast<uint32_t>(ls);
            edit.line_end = static_cast<uint32_t>(le);
            std::string b64err;
            if (!base64_decode(old64->as_string(), edit.old_bytes, b64err) ||
                !base64_decode(replacement64->as_string(), edit.replacement, b64err)) {
                error = "invalid base64 for site " + std::to_string(edit.site_id) + ": " + b64err;
                return false;
            }
            if (edit.old_sha256.size() != 64 ||
                sha256_hex(edit.old_bytes) != edit.old_sha256) {
                error = "old-span hash mismatch inside plan at site " +
                        std::to_string(edit.site_id);
                return false;
            }
            file.edits.push_back(std::move(edit));
        }
        plan.files.push_back(std::move(file));
    }
    if (plan.files.size() != plan.file_count || site_ids.size() != plan.site_count) {
        error = "plan selection counts do not match files/edits";
        return false;
    }
    return true;
}

int apply_edit_plan(const EditPlan &plan, const ApplyOptions &options,
                    bool emit_receipts) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::weakly_canonical(
        std::filesystem::current_path(), ec);
    std::filesystem::path planned_root = std::filesystem::weakly_canonical(
        plan.working_root, ec);
    if (!options.trusted_in_memory && !options.allow_root_move &&
        current != planned_root) {
        emit_guard("", "working root changed from '" + plan.working_root +
                          "' to '" + current.string() +
                          "'; use -allow-root-mismatch only after review");
        return 3;
    }

    std::vector<VerifiedFile> verified;
    verified.reserve(plan.files.size());
    bool refused = false;
    for (const auto &file : plan.files) {
        bool file_refused = false;
        VerifiedFile vf;
        vf.planned = &file;
        std::filesystem::path logical = options.allow_root_move
            ? (current / std::filesystem::path(file.path)).lexically_normal()
            : std::filesystem::path(file.planned_absolute_path);
        vf.logical_path = logical.string();
        if (!options.trusted_in_memory) {
            std::filesystem::path approved_root = options.allow_root_move ? current : planned_root;
            std::filesystem::path resolved = std::filesystem::weakly_canonical(logical, ec);
            if (ec || !beneath_root(approved_root, resolved)) {
                emit_guard(file.path, "target escapes the approved working root after canonicalization");
                refused = true;
                ec.clear();
                continue;
            }
        }
        std::filesystem::file_status lst = std::filesystem::symlink_status(logical, ec);
        if (ec || !std::filesystem::exists(lst)) {
            emit_guard(file.path, "target no longer exists");
            refused = true;
            continue;
        }
        bool now_symlink = std::filesystem::is_symlink(lst);
        if (now_symlink != file.symlink) {
            emit_guard(file.path, "symlink status changed since planning");
            refused = true;
            continue;
        }
        if (file.symlink) {
            if (!options.follow_symlinks) {
                emit_guard(file.path, "plan contains a symlink; repeat -follow-symlinks during apply");
                refused = true;
                continue;
            }
            auto target = std::filesystem::canonical(logical, ec);
            if (ec || target.string() != file.symlink_target) {
                emit_guard(file.path, "symlink target changed since planning");
                refused = true;
                continue;
            }
            vf.write_path = target.string();
        } else {
            if (!std::filesystem::is_regular_file(lst)) {
                emit_guard(file.path, "target is not a regular file");
                refused = true;
                continue;
            }
            vf.write_path = logical.string();
        }
        std::string error;
        if (!read_bytes(vf.write_path, vf.original, error)) {
            emit_guard(file.path, error);
            refused = true;
            continue;
        }
        if (vf.original.size() != file.original_size ||
            sha256_hex(vf.original) != file.original_sha256) {
            emit_guard(file.path, "whole-file SHA-256 or size changed since planning");
            refused = true;
            continue;
        }
        uint64_t cursor = 0;
        vf.result.reserve(vf.original.size() + 256);
        for (const auto &edit : file.edits) {
            if (edit.from > edit.to || edit.to > vf.original.size() ||
                edit.from < cursor) {
                emit_guard(file.path, "edit ranges are invalid or overlap at site " +
                                      std::to_string(edit.site_id));
                refused = true;
                file_refused = true;
                break;
            }
            std::string_view old(vf.original.data() + edit.from,
                                 edit.to - edit.from);
            if (old != edit.old_bytes || sha256_hex(old) != edit.old_sha256) {
                emit_guard(file.path, "stored old bytes differ at site " +
                                      std::to_string(edit.site_id));
                refused = true;
                file_refused = true;
                break;
            }
            vf.result.append(vf.original, cursor, edit.from - cursor);
            vf.result += edit.replacement;
            cursor = edit.to;
            if (old != edit.replacement) vf.changed = true;
        }
        if (file_refused) continue;
        vf.result.append(vf.original, cursor, vf.original.size() - cursor);
        verified.push_back(std::move(vf));
    }
    if (refused || verified.size() != plan.files.size()) return 3;

    if (options.diff) {
        for (const auto &vf : verified) {
            if (!vf.changed) continue;
            std::printf("--- %s\n+++ %s\n", vf.planned->path.c_str(),
                        vf.planned->path.c_str());
            for (const auto &edit : vf.planned->edits) {
                std::printf("@@ bytes %llu,%llu @@\n",
                            (unsigned long long)edit.from,
                            (unsigned long long)edit.to);
                std::fputc('-', stdout);
                std::fwrite(edit.old_bytes.data(), 1, edit.old_bytes.size(), stdout);
                std::fputs("\n+", stdout);
                std::fwrite(edit.replacement.data(), 1, edit.replacement.size(), stdout);
                std::fputc('\n', stdout);
            }
        }
    }

    std::vector<StagedFile> staged;
    const uint64_t changed_files = static_cast<uint64_t>(std::count_if(
        verified.begin(), verified.end(),
        [](const VerifiedFile &vf) { return vf.changed; }));
    size_t stage_ordinal = 0;
    for (auto &vf : verified) {
        if (!vf.changed) continue;
        staged.emplace_back();
        std::string error;
        if (!stage_file(vf, staged.back(), error, ++stage_ordinal)) {
            staged.pop_back();
            for (const auto &s : staged) ::unlink(s.temporary.c_str());
            std::fprintf(stderr, "hprscript: apply: %s\n", error.c_str());
            if (emit_receipts)
                emit_apply_summary("staging-failed", 0, changed_files, {},
                                   options.json);
            return 2;
        }
    }

    uint64_t committed = 0;
    for (size_t i = 0; i < staged.size(); ++i) {
        auto &s = staged[i];
        bool fail = fault_at("HPRSCRIPT_TEST_FAIL_RENAME_N", i + 1);
        if (fail || std::rename(s.temporary.c_str(),
                                s.verified->write_path.c_str()) != 0) {
            std::string reason = fail ? "fault injection: rename failed"
                                      : std::strerror(errno);
            std::fprintf(stderr, "hprscript: apply: rename failed for %s: %s\n",
                         s.verified->planned->path.c_str(), reason.c_str());
            std::vector<std::string> remaining;
            for (size_t j = i; j < staged.size(); ++j)
                remaining.push_back(staged[j].temporary);
            if (committed == 0) {
                for (const auto &tmp : remaining) ::unlink(tmp.c_str());
                if (emit_receipts)
                    emit_apply_summary("commit-failed", 0, staged.size(), {},
                                       options.json);
                return 2;
            }
            if (emit_receipts) {
                for (size_t j = 0; j < i; ++j)
                    emit_receipt(staged[j].verified->planned->path, "applied", "",
                                 options.json);
                for (size_t j = i; j < staged.size(); ++j)
                    emit_receipt(staged[j].verified->planned->path, "not-applied",
                                 staged[j].temporary, options.json);
                emit_apply_summary("partial", committed, staged.size() - committed,
                                   remaining, options.json);
            }
            return 4;
        }
        s.renamed = true;
        s.temporary.clear();
        ++committed;
    }

    bool dir_sync_failed = false;
    std::set<std::string> directories;
    for (const auto &s : staged) {
        std::filesystem::path target(s.verified->write_path);
        directories.insert(target.parent_path().empty() ? "." :
                           target.parent_path().string());
    }
    size_t dir_ordinal = 0;
    for (const auto &dir : directories) {
#ifdef O_DIRECTORY
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
#else
        int fd = ::open(dir.c_str(), O_RDONLY);
#endif
        bool fail = fault_at("HPRSCRIPT_TEST_FAIL_DIR_FSYNC_N", ++dir_ordinal);
        if (fd < 0 || fail || ::fsync(fd) != 0) dir_sync_failed = true;
        if (fd >= 0) ::close(fd);
    }
    if (emit_receipts) {
        for (const auto &vf : verified)
            emit_receipt(vf.planned->path, vf.changed ? "applied" : "noop", "",
                         options.json);
        emit_apply_summary(dir_sync_failed ? "durability-uncertain" : "complete",
                           committed, 0, {}, options.json);
    }
    return dir_sync_failed ? 4 : 0;
}

int run_apply(const Cli &cli) {
    EditPlan plan;
    std::string error;
    if (!read_edit_plan(cli.apply.plan_path, plan, error)) {
        std::fprintf(stderr, "hprscript: apply: %s\n", error.c_str());
        return 2;
    }
    return apply_edit_plan(plan, cli.apply, true);
}

} // namespace hpr
