// Balanced delimiter scan used by both the script `block` action and the
// quick-search `-block-open`/`-block-close` CLI flags.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hpr {

// Walk forward from `pos` looking for the first occurrence of `open`, then
// count nesting until the matching `close` is found. On success returns true
// and writes the (start, end-exclusive) byte range to out_open/out_close.
bool find_balanced_block(std::string_view buf, uint64_t pos,
                         const std::string &open, const std::string &close,
                         uint64_t &out_open, uint64_t &out_close);

} // namespace hpr
