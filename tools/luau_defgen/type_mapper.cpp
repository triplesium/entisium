#include "type_mapper.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <optional>
#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace ets::luau_defgen {
namespace {

[[nodiscard]] std::string trim(std::string value) {
    const auto not_space = [](const unsigned char character) {
        return std::isspace(character) == 0;
    };
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), not_space)
    );
    value.erase(
        std::find_if(value.rbegin(), value.rend(), not_space).base(),
        value.end()
    );
    return value;
}

[[nodiscard]] std::string normalized_base_type(std::string value) {
    value = trim(std::move(value));
    if (value.starts_with("const ")) {
        value.erase(0, 6);
    }
    value =
        std::regex_replace(value, std::regex {R"(\s+(const|volatile)$)"}, "");
    value = std::regex_replace(value, std::regex {R"(\s*(&&|&|\*)$)"}, "");
    if (value.starts_with("class ")) {
        value.erase(0, 6);
    } else if (value.starts_with("struct ")) {
        value.erase(0, 7);
    } else if (value.starts_with("enum ")) {
        value.erase(0, 5);
    }
    return trim(std::move(value));
}

[[nodiscard]] bool is_number_type(const std::string_view type) {
    constexpr std::array numeric_types {
        "char",          "signed char",
        "unsigned char", "char8_t",
        "char16_t",      "char32_t",
        "short",         "unsigned short",
        "int",           "unsigned int",
        "long",          "unsigned long",
        "long long",     "unsigned long long",
        "float",         "double",
        "long double",   "size_t",
        "std::size_t",   "int8",
        "int16",         "int32",
        "int64",         "uint8",
        "uint16",        "uint32",
        "uint64",        "std::int8_t",
        "std::int16_t",  "std::int32_t",
        "std::int64_t",  "std::uint8_t",
        "std::uint16_t", "std::uint32_t",
        "std::uint64_t", "ets::Entity",
    };
    return std::ranges::find(numeric_types, type) != numeric_types.end();
}

[[nodiscard]] std::string safe_identifier(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) != 0 ? character : '_');
    }
    while (result.find("__") != std::string::npos) {
        result = std::regex_replace(result, std::regex {"__+"}, "_");
    }
    return result;
}

[[nodiscard]] std::optional<std::string_view>
optional_argument(const std::string_view type) {
    constexpr std::array prefixes {
        std::string_view {"ets::Optional<"},
        std::string_view {"Optional<"},
        std::string_view {"std::optional<"},
    };
    for (const auto prefix : prefixes) {
        if (type.starts_with(prefix) && type.ends_with('>')) {
            return type.substr(prefix.size(), type.size() - prefix.size() - 1);
        }
    }
    return std::nullopt;
}

struct TemplateType {
    std::string name;
    std::vector<std::string> arguments;
};

[[nodiscard]] std::optional<TemplateType>
parse_template_type(const std::string_view type) {
    const auto opening = type.find('<');
    if (opening == std::string_view::npos || !type.ends_with('>')) {
        return std::nullopt;
    }

    TemplateType result {.name = trim(std::string {type.substr(0, opening)})};
    const auto arguments = type.substr(opening + 1, type.size() - opening - 2);
    std::size_t argument_begin = 0;
    int depth = 0;
    for (std::size_t position = 0; position <= arguments.size(); ++position) {
        const bool at_end = position == arguments.size();
        const char character = at_end ? ',' : arguments[position];
        if (!at_end) {
            if (character == '<') {
                ++depth;
            } else if (character == '>') {
                --depth;
                if (depth < 0) {
                    return std::nullopt;
                }
            }
        }
        if (character == ',' && depth == 0) {
            auto argument = trim(
                std::string {
                    arguments.substr(argument_begin, position - argument_begin),
                }
            );
            if (argument.empty()) {
                return std::nullopt;
            }
            result.arguments.push_back(std::move(argument));
            argument_begin = position + 1;
        }
    }
    return depth == 0 && !result.arguments.empty() ?
               std::optional<TemplateType> {std::move(result)} :
               std::nullopt;
}

[[nodiscard]] std::string_view unqualified_name(std::string_view name) {
    const auto separator = name.rfind("::");
    return separator == std::string_view::npos ? name :
                                                 name.substr(separator + 2);
}

} // namespace

TypeMapper::TypeMapper(const Database& database) {
    std::unordered_set<std::string> ambiguous_names;
    const auto add_reflected_type = [&](const std::string& cpp_name,
                                        const std::string& luau_name) {
        const auto qualified_inserted =
            m_reflected_types.emplace(cpp_name, luau_name).second;
        if (!qualified_inserted) {
            return;
        }
        const auto separator = cpp_name.rfind("::");
        const auto unqualified =
            cpp_name.substr(separator == std::string::npos ? 0 : separator + 2);
        if (ambiguous_names.contains(unqualified)) {
            return;
        }
        const auto [existing, inserted] =
            m_unqualified_reflected_types.emplace(unqualified, luau_name);
        if (!inserted) {
            m_unqualified_reflected_types.erase(existing);
            ambiguous_names.insert(unqualified);
        }
    };
    for (const auto& cls : database.classes) {
        add_reflected_type(cls.cpp_name, cls.name);
    }
    for (const auto& enm : database.enums) {
        add_reflected_type(enm.cpp_name, enm.name);
    }
    for (const auto& cls : database.classes) {
        std::vector<std::string> coercions;
        for (const auto& constructor : cls.constructors) {
            if (constructor.converting && constructor.parameters.size() == 1) {
                const auto& source = constructor.parameters.front().cpp_type;
                if (std::ranges::find(coercions, source) == coercions.end()) {
                    coercions.push_back(source);
                }
            }
        }
        if (coercions.empty()) {
            continue;
        }
        m_parameter_coercions.emplace(cls.cpp_name, coercions);

        const auto separator = cls.cpp_name.rfind("::");
        const auto unqualified = cls.cpp_name.substr(
            separator == std::string::npos ? 0 : separator + 2
        );
        if (const auto reflected =
                m_unqualified_reflected_types.find(unqualified);
            reflected != m_unqualified_reflected_types.end() &&
            reflected->second == cls.name) {
            m_parameter_coercions.emplace(unqualified, std::move(coercions));
        }
    }
}

std::string TypeMapper::map(const std::string_view cpp_type) {
    const auto original = trim(std::string {cpp_type});
    const auto base = normalized_base_type(original);
    const auto find_reflected =
        [&](const std::string_view type) -> const std::string* {
        if (const auto reflected = m_reflected_types.find(std::string {type});
            reflected != m_reflected_types.end()) {
            return &reflected->second;
        }
        if (const auto reflected =
                m_unqualified_reflected_types.find(std::string {type});
            reflected != m_unqualified_reflected_types.end()) {
            return &reflected->second;
        }
        return nullptr;
    };
    if (const auto argument = optional_argument(base)) {
        auto mapped = map(*argument);
        if (mapped == "any") {
            m_unsupported_types.insert(original);
            return mapped;
        }
        if (!mapped.ends_with('?')) {
            mapped += '?';
        }
        return mapped;
    }
    if (const auto generic = parse_template_type(base)) {
        const auto& args = generic->arguments;
        if (generic->name == "std::vector" || generic->name == "std::array") {
            return "ReflectedSequence<" + map(args[0]) + ">";
        }
        if ((generic->name == "std::map" ||
             generic->name == "std::unordered_map") &&
            args.size() >= 2) {
            return "ReflectedMap<" + map(args[0]) + ", " + map(args[1]) + ">";
        }
        if ((generic->name == "ets::Result" || generic->name == "Result") &&
            args.size() == 2) {
            const auto success = args[0] == "void" ? "boolean" : map(args[0]);
            return "(" + success + "?, " + map(args[1]) + "?)";
        }
    }
    const bool pointer = original.find('*') != std::string::npos;
    if (pointer) {
        if (const auto* reflected = find_reflected(base)) {
            return *reflected + '?';
        }
        m_unsupported_types.insert(original);
        return "any";
    }
    if (base == "void") {
        return "()";
    }
    if (base == "bool") {
        return "boolean";
    }
    if (base == "TypeId" || base == "ets::TypeId") {
        return "TypeToken<any>";
    }
    if (is_number_type(base)) {
        return "number";
    }
    if (base == "std::string" || base == "std::basic_string<char>" ||
        base == "std::string_view" || base == "std::basic_string_view<char>") {
        return "string";
    }
    if (const auto* reflected = find_reflected(base)) {
        return *reflected;
    }
    m_unsupported_types.insert(original);
    return "any";
}

std::string TypeMapper::map_parameter(const std::string_view cpp_type) {
    const auto original = trim(std::string {cpp_type});
    const auto base = normalized_base_type(original);
    auto mapped = map(original);
    const auto coercions = m_parameter_coercions.find(base);
    if (coercions == m_parameter_coercions.end()) {
        return mapped;
    }
    for (const auto& cpp_coercion : coercions->second) {
        const auto coercion = map(cpp_coercion);
        if (coercion != mapped && coercion != "any") {
            mapped += " | ";
            mapped += coercion;
        }
    }
    return mapped;
}

std::string TypeMapper::map_library_parameter(const std::string_view cpp_type) {
    const auto original = trim(std::string {cpp_type});
    const auto base = normalized_base_type(original);
    auto mapped = map_parameter(original);
    if (const auto argument = optional_argument(base)) {
        auto inner = map_library_parameter(*argument);
        return inner.find(" | ") == std::string::npos ? inner + '?' :
                                                        "(" + inner + ")?";
    }
    if (const auto generic = parse_template_type(base)) {
        const auto& args = generic->arguments;
        if (generic->name == "std::vector" || generic->name == "std::array") {
            return mapped + " | {" + map_library_parameter(args[0]) + "}";
        }
        if ((generic->name == "std::map" ||
             generic->name == "std::unordered_map") &&
            args.size() >= 2) {
            return mapped + " | {[" + map(args[0]) +
                   "]: " + map_library_parameter(args[1]) + "}";
        }
    }
    return mapped;
}

std::string TypeMapper::map_dependent_return(
    const std::string_view cpp_type,
    const std::string_view type_parameter
) {
    struct Projection {
        std::string type;
        std::size_t slots {0};
    };
    std::function<Projection(std::string_view)> project;
    project = [&](const std::string_view type) -> Projection {
        const auto base = normalized_base_type(std::string {type});
        const auto local_name = unqualified_name(base);
        if (local_name == "UntypedHandle") {
            return {
                .type = "Handle<" + std::string {type_parameter} + ">",
                .slots = 1,
            };
        }

        const auto generic = parse_template_type(base);
        if (!generic) {
            return {.type = map(base)};
        }
        const auto generic_name = unqualified_name(generic->name);
        if (generic_name == "Result") {
            if (generic->arguments.size() != 2) {
                throw std::runtime_error(
                    "Dependent return Result must have two type arguments"
                );
            }
            return project(generic->arguments.front());
        }
        if (generic_name == "Optional" || generic_name == "optional") {
            if (generic->arguments.size() != 1) {
                throw std::runtime_error(
                    "Dependent return Optional must have one type argument"
                );
            }
            auto result = project(generic->arguments.front());
            if (!result.type.ends_with('?')) {
                result.type += '?';
            }
            return result;
        }

        bool contains_slot = false;
        for (const auto& argument : generic->arguments) {
            const auto projected = project(argument);
            contains_slot = contains_slot || projected.slots != 0;
        }
        if (contains_slot) {
            throw std::runtime_error(
                "Unsupported dependent return wrapper '" + generic->name + "'"
            );
        }
        return {.type = map(base)};
    };

    auto result = project(cpp_type);
    if (result.slots != 1) {
        throw std::runtime_error(
            "Dependent return type '" + std::string {cpp_type} +
            "' must project to exactly one untyped handle slot; found " +
            std::to_string(result.slots)
        );
    }
    return result.type;
}

const std::set<std::string>& TypeMapper::unsupported_types() const {
    return m_unsupported_types;
}

std::string internal_type_name(const std::string_view cpp_name) {
    return "__Entisium_" + safe_identifier(cpp_name);
}

std::string internal_value_name(const std::string_view cpp_name) {
    return "__EntisiumType_" + safe_identifier(cpp_name);
}

} // namespace ets::luau_defgen
