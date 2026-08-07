#pragma once

#include <string>
#include <vector>

namespace hpr {

struct FileRoleResult {
    std::vector<std::string> roles;
    std::string method = "path-heuristic";
};

FileRoleResult classify_file_roles(const std::string &path);

} // namespace hpr
