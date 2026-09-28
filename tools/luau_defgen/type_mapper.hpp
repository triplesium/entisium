#pragma once

#include "model.hpp"

#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ets::luau_defgen {

class TypeMapper {
  public:
    explicit TypeMapper(const Database& database);

    [[nodiscard]] std::string map(std::string_view cpp_type);
    [[nodiscard]] std::string map_parameter(std::string_view cpp_type);
    [[nodiscard]] std::string map_library_parameter(std::string_view cpp_type);
    [[nodiscard]] std::string map_dependent_return(
        std::string_view cpp_type,
        std::string_view type_parameter = "T"
    );
    [[nodiscard]] const std::set<std::string>& unsupported_types() const;

  private:
    std::unordered_map<std::string, std::string> m_reflected_types;
    std::unordered_map<std::string, std::string> m_unqualified_reflected_types;
    std::unordered_map<std::string, std::vector<std::string>>
        m_parameter_coercions;
    std::set<std::string> m_unsupported_types;
};

[[nodiscard]] std::string internal_type_name(std::string_view cpp_name);
[[nodiscard]] std::string internal_value_name(std::string_view cpp_name);

} // namespace ets::luau_defgen
