#include "manifest.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace ets::reflgen {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] std::filesystem::path
normalized_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

[[nodiscard]] std::string project_relative_path(
    const std::string& source_file,
    const std::filesystem::path& root_dir
) {
    const auto source = normalized_absolute_path(source_file);
    const auto root = normalized_absolute_path(root_dir);

    std::error_code error;
    auto relative = std::filesystem::relative(source, root, error);
    if (error) {
        relative = source.lexically_relative(root);
    }
    return relative.generic_string();
}

[[nodiscard]] Json annotation_json(const ReflectionAnnotation& annotation) {
    auto arguments = Json::array();
    for (const auto& argument : annotation.arguments) {
        arguments.push_back(
            Json {{"name", argument.name}, {"value", argument.value}}
        );
    }
    return Json {
        {"name", annotation.name},
        {"arguments", std::move(arguments)},
    };
}

[[nodiscard]] Json
annotations_json(const std::vector<ReflectionAnnotation>& annotations) {
    auto result = Json::array();
    for (const auto& annotation : annotations) {
        result.push_back(annotation_json(annotation));
    }
    return result;
}

[[nodiscard]] Json parameters_json(const std::vector<ParamInfo>& parameters) {
    auto result = Json::array();
    for (const auto& parameter : parameters) {
        result.push_back(
            Json {{"name", parameter.name}, {"cppType", parameter.type_name}}
        );
    }
    return result;
}

[[nodiscard]] Json annotation_schema_json(
    const AnnotationSchemaInfo& schema,
    const std::filesystem::path& root_dir
) {
    auto fields = Json::array();
    for (const auto& field : schema.fields) {
        if (!is_reflected_property(field)) {
            throw std::runtime_error(
                "Annotation schema field '" + schema.type_name + "." +
                field.name + "' must be public"
            );
        }
        fields.push_back(
            Json {{"name", field.name}, {"cppType", field.type_name}}
        );
    }
    return Json {
        {"name", schema.reflected_name},
        {"cppName", schema.type_name},
        {"source", project_relative_path(schema.source_file, root_dir)},
        {"fields", std::move(fields)},
    };
}

[[nodiscard]] Json
class_json(const ClassInfo& cls, const std::filesystem::path& root_dir) {
    auto properties = Json::array();
    for (const auto& property : cls.properties) {
        if (is_reflected_property(property)) {
            properties.push_back(
                Json {{"name", property.name}, {"cppType", property.type_name}}
            );
        }
    }

    auto methods = Json::array();
    for (const auto& method : cls.methods) {
        if (is_reflected_method(cls, method)) {
            Json method_json {
                {"name", method.name},
                {"annotations", annotations_json(method.annotations)},
                {"returnCppType", method.type_name},
                {"parameters", parameters_json(method.parameters)},
                {"static", method.is_static},
                {"const", method.is_const},
            };
            if (method.dependent_return_parameter) {
                method_json["dependentReturnParameter"] =
                    *method.dependent_return_parameter;
            }
            methods.push_back(std::move(method_json));
        }
    }

    auto constructors = Json::array();
    for (const auto& constructor : cls.constructors) {
        if (is_reflected_constructor(cls, constructor)) {
            constructors.push_back(
                Json {
                    {"parameters", parameters_json(constructor.parameters)},
                    {"converting", constructor.is_converting},
                }
            );
        }
    }

    return Json {
        {"cppName", cls.name},
        {"name", cls.local_name},
        {"namespace", cls.namespace_path},
        {"source", project_relative_path(cls.source_file, root_dir)},
        {"abstract", cls.is_abstract()},
        {"annotations", annotations_json(cls.annotations)},
        {"properties", std::move(properties)},
        {"methods", std::move(methods)},
        {"constructors", std::move(constructors)},
    };
}

[[nodiscard]] Json
enum_json(const EnumInfo& enum_info, const std::filesystem::path& root_dir) {
    if (has_annotation(enum_info.annotations, "Plugin")) {
        throw std::runtime_error(
            "Plugin annotation can only be applied to classes: " +
            enum_info.name
        );
    }

    auto values = Json::array();
    for (const auto& value : enum_info.values) {
        values.push_back(Json {{"name", value.name}, {"value", value.value}});
    }
    return Json {
        {"cppName", enum_info.name},
        {"name", enum_info.local_name},
        {"namespace", enum_info.namespace_path},
        {"source", project_relative_path(enum_info.source_file, root_dir)},
        {"underlyingCppType", enum_info.underlying_type},
        {"scoped", enum_info.is_scoped},
        {"annotations", annotations_json(enum_info.annotations)},
        {"values", std::move(values)},
    };
}

template<typename Type, typename Projection>
[[nodiscard]] std::vector<Type>
sorted_copy(const std::vector<Type>& values, Projection projection) {
    auto sorted = values;
    std::ranges::sort(sorted, {}, projection);
    return sorted;
}

} // namespace

void write_reflection_manifest(
    const ParseResult& result,
    const std::filesystem::path& root_dir,
    const std::filesystem::path& output_file,
    std::string_view script_module
) {
    if (output_file.empty()) {
        return;
    }

    auto annotation_schemas = Json::array();
    for (const auto& schema : sorted_copy(
             result.annotation_schemas,
             &AnnotationSchemaInfo::reflected_name
         )) {
        annotation_schemas.push_back(annotation_schema_json(schema, root_dir));
    }

    auto classes = Json::array();
    for (const auto& cls : sorted_copy(result.classes, &ClassInfo::name)) {
        if (is_reflected_class(cls)) {
            classes.push_back(class_json(cls, root_dir));
        }
    }

    auto enums = Json::array();
    for (const auto& enum_info : sorted_copy(result.enums, &EnumInfo::name)) {
        enums.push_back(enum_json(enum_info, root_dir));
    }

    const Json document {
        {"format", "entisium.reflection"},
        {"version", 1},
        {"module", script_module},
        {"annotationSchemas", std::move(annotation_schemas)},
        {"classes", std::move(classes)},
        {"enums", std::move(enums)},
    };
    const auto content = document.dump(2) + '\n';

    if (output_file.has_parent_path()) {
        std::filesystem::create_directories(output_file.parent_path());
    }
    std::ofstream out(output_file, std::ios::binary);
    if (!out) {
        throw std::runtime_error(
            "Failed to open reflection manifest file '" +
            output_file.generic_string() + "'"
        );
    }
    out << content;
}

} // namespace ets::reflgen
