#pragma once

#include <string>
#include <string_view>

namespace hpr {

struct OccurrenceClassification {
    std::string classification = "unclassified_reference";
    std::string confidence = "low";
    std::string method = "lexical-generic-pack";
};

OccurrenceClassification classify_occurrence(const std::string &path,
                                               std::string_view line,
                                               std::string_view matched,
                                               const std::string &profile,
                                               bool test_file);

} // namespace hpr
