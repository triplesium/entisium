#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace ets::reflgen {
void generate_luau_libraries(
    const std::filesystem::path& catalog,
    const std::filesystem::path& output,
    const std::filesystem::path& manifest,
    const std::vector<std::string>& includes,
    const std::string& function = "default_luau_libraries",
    bool partial = false
);
}
