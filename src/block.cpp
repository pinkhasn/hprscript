#include "block.hpp"

namespace hpr {

bool find_balanced_block(std::string_view buf, uint64_t pos,
                         const std::string &open, const std::string &close,
                         uint64_t &out_open, uint64_t &out_close) {
    if (open.empty() || close.empty()) return false;
    size_t op = buf.find(open, pos);
    if (op == std::string_view::npos) return false;
    out_open = op;
    int depth = 1;
    size_t i = op + open.size();
    while (i < buf.size() && depth > 0) {
        if (buf.compare(i, open.size(), open) == 0) {
            ++depth; i += open.size(); continue;
        }
        if (buf.compare(i, close.size(), close) == 0) {
            --depth; i += close.size();
            if (depth == 0) {
                out_close = i;
                return true;
            }
            continue;
        }
        ++i;
    }
    return false;
}

} // namespace hpr
