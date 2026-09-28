#pragma once

#include "model.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>

namespace ets::luau_defgen {

struct EmissionSummary {
    std::size_t class_count {0};
    std::size_t enum_count {0};
    std::size_t module_count {0};
    std::set<std::string> unsupported_cpp_types;
};

[[nodiscard]] EmissionSummary emit_definitions(
    const Database& database,
    const std::filesystem::path& manual_definitions,
    const std::filesystem::path& output_directory,
    const std::filesystem::path& runtime_directory,
    const std::filesystem::path& libraries_directory
);

} // namespace ets::luau_defgen
