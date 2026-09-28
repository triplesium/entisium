#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ets::reflgen {

struct AnnotationArgument {
    std::string name;
    std::string value;
};

struct ReflectionAnnotation {
    std::string name;
    std::vector<AnnotationArgument> arguments;
};

struct ParamInfo {
    std::string name;
    std::string type_name;
};

struct MemberInfo {
    std::string name;
    std::string type_name;
    std::string access;
};

struct AnnotationSchemaInfo {
    std::string reflected_name;
    std::string type_name;
    std::string source_file;
    std::vector<MemberInfo> fields;
};

struct MethodInfo : MemberInfo {
    std::vector<ReflectionAnnotation> annotations;
    std::vector<ParamInfo> parameters;
    std::optional<std::string> dependent_return_parameter;
    bool is_static = false;
    bool is_const = false;
    bool is_converting = false;
    std::string ref_qualifier;
    bool is_abstract = false;

    [[nodiscard]] std::string to_cpp_type(std::string_view parent) const;
};

struct ClassInfo {
    std::string name;
    std::vector<std::string> namespace_path;
    std::string local_name;
    std::string source_file;
    std::vector<ReflectionAnnotation> annotations;
    std::vector<MemberInfo> properties;
    std::vector<MethodInfo> methods;
    std::vector<MethodInfo> constructors;

    [[nodiscard]] bool is_abstract() const;
};

struct EnumValueInfo {
    std::string name;
    long long value = 0;
};

struct EnumInfo {
    std::string name;
    std::vector<std::string> namespace_path;
    std::string local_name;
    std::string source_file;
    std::vector<ReflectionAnnotation> annotations;
    std::string underlying_type;
    bool is_scoped = false;
    std::vector<EnumValueInfo> values;
};

struct ParseResult {
    std::vector<AnnotationSchemaInfo> annotation_schemas;
    std::vector<ClassInfo> classes;
    std::vector<EnumInfo> enums;
};

[[nodiscard]] bool has_annotation(
    const std::vector<ReflectionAnnotation>& annotations,
    std::string_view name
);
[[nodiscard]] bool is_reflected_class(const ClassInfo& cls);
[[nodiscard]] bool is_reflected_property(const MemberInfo& property);
[[nodiscard]] bool
is_reflected_method(const ClassInfo& cls, const MethodInfo& method);
[[nodiscard]] bool
is_reflected_constructor(const ClassInfo& cls, const MethodInfo& constructor);

void dedupe_reflected_types(ParseResult& result);
void filter_codegen_unsupported_members(ParseResult& result);

} // namespace ets::reflgen
