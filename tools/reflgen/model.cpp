#include "model.hpp"

#include <algorithm>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace ets::reflgen {
namespace {

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) {
    return text.starts_with(prefix);
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
    return text.ends_with(suffix);
}

[[nodiscard]] std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

[[nodiscard]] std::string collapse_spaces(std::string_view text) {
    std::istringstream stream {std::string(text)};
    std::string word;
    std::string result;
    while (stream >> word) {
        if (!result.empty()) {
            result += ' ';
        }
        result += word;
    }
    return result;
}

[[nodiscard]] std::string strip_cv_ref(std::string_view type_name) {
    auto type = collapse_spaces(trim(type_name));
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto* prefix :
             {"const ", "volatile ", "class ", "struct ", "enum "}) {
            if (starts_with(type, prefix)) {
                type = trim(
                    std::string_view(type).substr(
                        std::string_view(prefix).size()
                    )
                );
                changed = true;
            }
        }
        for (const auto* suffix : {" const", " volatile", "&&", "&"}) {
            if (ends_with(type, suffix)) {
                type = trim(
                    std::string_view(type).substr(
                        0,
                        type.size() - std::string_view(suffix).size()
                    )
                );
                changed = true;
            }
        }
    }
    return type;
}

[[nodiscard]] bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

[[nodiscard]] bool
is_codegen_supported_type(std::string_view type_name, bool allow_void) {
    const auto type = strip_cv_ref(type_name);
    if (type.empty()) {
        return false;
    }
    if (type == "void") {
        return allow_void;
    }

    if (contains(type, "(anonymous") || contains(type, "<anonymous") ||
        contains(type, "anonymous namespace")) {
        return false;
    }

    return true;
}

[[nodiscard]] std::string
join_param_types(const std::vector<ParamInfo>& parameters) {
    std::string result;
    for (const auto& param : parameters) {
        if (!result.empty()) {
            result += ", ";
        }
        result += param.type_name;
    }
    return result;
}

} // namespace

std::string MethodInfo::to_cpp_type(std::string_view parent) const {
    const auto param_types = join_param_types(parameters);
    if (is_static) {
        return type_name + "(*)(" + param_types + ")";
    }

    std::string qualifier_suffix;
    if (is_const) {
        qualifier_suffix += " const";
    }
    if (!ref_qualifier.empty()) {
        qualifier_suffix += " " + ref_qualifier;
    }

    const std::regex function_pointer_return_regex(R"(^(.+)\(\*\)\((.*)\)$)");
    std::smatch match;
    if (std::regex_match(type_name, match, function_pointer_return_regex)) {
        return trim(match[1].str()) + " (* (" + std::string(parent) + "::*)(" +
               param_types + ")" + qualifier_suffix + ")(" +
               trim(match[2].str()) + ")";
    }

    return type_name + "(" + std::string(parent) + "::*)(" + param_types + ")" +
           qualifier_suffix;
}

bool ClassInfo::is_abstract() const {
    return std::ranges::any_of(methods, [](const MethodInfo& method) {
        return method.is_abstract;
    });
}

bool has_annotation(
    const std::vector<ReflectionAnnotation>& annotations,
    std::string_view name
) {
    return std::ranges::any_of(
        annotations,
        [name](const ReflectionAnnotation& annotation) {
            return annotation.name == name;
        }
    );
}

bool is_reflected_class(const ClassInfo& cls) {
    return cls.name != "ets::Registry" &&
           !has_annotation(cls.annotations, "LuauLibrary");
}

bool is_reflected_property(const MemberInfo& property) {
    return property.access == "public";
}

bool is_reflected_method(const ClassInfo& cls, const MethodInfo& method) {
    if (method.access != "public") {
        return false;
    }
    if (!has_annotation(cls.annotations, "Plugin")) {
        return true;
    }
    return method.name != "dependencies" && method.name != "setup" &&
           method.name != "finish" && method.name != "cleanup";
}

bool is_reflected_constructor(
    const ClassInfo& cls,
    const MethodInfo& constructor
) {
    return !cls.is_abstract() && constructor.access == "public";
}

void dedupe_reflected_types(ParseResult& result) {
    std::unordered_set<std::string> seen_annotation_schemas;
    std::vector<AnnotationSchemaInfo> annotation_schemas;
    for (auto& schema : result.annotation_schemas) {
        if (seen_annotation_schemas.insert(schema.reflected_name).second) {
            annotation_schemas.push_back(std::move(schema));
        }
    }
    result.annotation_schemas = std::move(annotation_schemas);

    std::unordered_set<std::string> seen_classes;
    std::vector<ClassInfo> classes;
    for (auto& cls : result.classes) {
        if (seen_classes.insert(cls.name).second) {
            classes.push_back(std::move(cls));
        }
    }
    result.classes = std::move(classes);

    std::unordered_set<std::string> seen_enums;
    std::vector<EnumInfo> enums;
    for (auto& enum_info : result.enums) {
        if (seen_enums.insert(enum_info.name).second) {
            enums.push_back(std::move(enum_info));
        }
    }
    result.enums = std::move(enums);
}

void filter_codegen_unsupported_members(ParseResult& result) {
    for (auto& cls : result.classes) {
        std::vector<MemberInfo> properties;
        for (auto& prop : cls.properties) {
            if (is_codegen_supported_type(prop.type_name, false)) {
                properties.push_back(std::move(prop));
            } else if (prop.access == "public") {
                std::cerr << "Skipping " << cls.name << "." << prop.name
                          << ": unsupported property type " << prop.type_name
                          << '\n';
            }
        }
        cls.properties = std::move(properties);

        std::vector<MethodInfo> methods;
        for (auto& method : cls.methods) {
            const auto return_ok =
                is_codegen_supported_type(method.type_name, true);
            const auto params_ok = std::ranges::all_of(
                method.parameters,
                [&](const ParamInfo& param) {
                    return is_codegen_supported_type(param.type_name, false);
                }
            );
            if (return_ok && params_ok) {
                methods.push_back(std::move(method));
            } else if (method.access == "public") {
                std::cerr << "Skipping " << cls.name << "." << method.name
                          << "(" << join_param_types(method.parameters)
                          << "): unsupported return/parameter type\n";
            }
        }
        cls.methods = std::move(methods);

        std::vector<MethodInfo> constructors;
        for (auto& constructor : cls.constructors) {
            const auto params_ok = std::ranges::all_of(
                constructor.parameters,
                [&](const ParamInfo& param) {
                    return is_codegen_supported_type(param.type_name, false);
                }
            );
            if (params_ok) {
                constructors.push_back(std::move(constructor));
            } else if (constructor.access == "public") {
                std::cerr << "Skipping " << cls.name << " constructor("
                          << join_param_types(constructor.parameters)
                          << "): unsupported parameter type\n";
            }
        }
        cls.constructors = std::move(constructors);
    }
}

} // namespace ets::reflgen
