#include "emitter.hpp"

#include "type_mapper.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace ets::luau_defgen {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] bool valid_identifier(const std::string_view value) {
    static const std::unordered_set<std::string_view> keywords {
        "and", "break",    "do",     "else", "elseif", "end",   "false",
        "for", "function", "if",     "in",   "local",  "nil",   "not",
        "or",  "repeat",   "return", "then", "true",   "until", "while",
    };
    if (value.empty() ||
        (std::isalpha(static_cast<unsigned char>(value.front())) == 0 &&
         value.front() != '_')) {
        return false;
    }
    return !keywords.contains(value) &&
           std::ranges::all_of(value.substr(1), [](const char character) {
               const auto byte = static_cast<unsigned char>(character);
               return std::isalnum(byte) != 0 || character == '_';
           });
}

[[nodiscard]] std::string
parameter_name(const std::string& name, const std::size_t index) {
    if (valid_identifier(name)) {
        return name;
    }
    return "arg" + std::to_string(index + 1);
}

[[nodiscard]] std::string parameter_type(
    TypeMapper& mapper,
    const Parameter& parameter,
    const std::optional<std::string_view> dependent_parameter = std::nullopt
) {
    if (dependent_parameter && parameter.name == *dependent_parameter) {
        if (mapper.map(parameter.cpp_type) != "TypeToken<any>") {
            throw std::runtime_error(
                "Dependent return source parameter '" + parameter.name +
                "' must have type ets::TypeId"
            );
        }
        return "TypeToken<T>";
    }
    return mapper.map_parameter(parameter.cpp_type);
}

[[nodiscard]] std::string parameters(
    TypeMapper& mapper,
    const std::vector<Parameter>& values,
    const std::optional<std::string_view> dependent_parameter = std::nullopt
) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << parameter_name(values[index].name, index) << ": "
               << parameter_type(mapper, values[index], dependent_parameter);
    }
    return output.str();
}

[[nodiscard]] std::optional<std::string_view>
dependent_parameter(const Method& method) {
    if (!method.dependent_return_parameter) {
        return std::nullopt;
    }
    const auto parameter = std::ranges::find(
        method.parameters,
        *method.dependent_return_parameter,
        &Parameter::name
    );
    if (parameter == method.parameters.end()) {
        throw std::runtime_error(
            "Dependent return method '" + method.name +
            "' names missing parameter '" + *method.dependent_return_parameter +
            "'"
        );
    }
    return *method.dependent_return_parameter;
}

[[nodiscard]] std::string
method_return_type(TypeMapper& mapper, const Method& method) {
    return method.dependent_return_parameter ?
               mapper.map_dependent_return(method.return_cpp_type) :
               mapper.map(method.return_cpp_type);
}

[[nodiscard]] std::string
function_type(TypeMapper& mapper, const Method& method) {
    const auto source = dependent_parameter(method);
    return std::string(source ? "<T>" : "") + "(" +
           parameters(mapper, method.parameters, source) + ") -> " +
           method_return_type(mapper, method);
}

[[nodiscard]] std::string parameter_signature_key(
    TypeMapper& mapper,
    const std::vector<Parameter>& values
) {
    std::string result;
    for (const auto& parameter : values) {
        const auto type = mapper.map_parameter(parameter.cpp_type);
        result += ':';
        result += std::to_string(type.size());
        result += ':';
        result += type;
    }
    return result;
}

[[nodiscard]] std::string
method_signature_key(TypeMapper& mapper, const Method& method) {
    std::string result = method.name;
    const auto source = dependent_parameter(method);
    for (const auto& parameter : method.parameters) {
        const auto type = parameter_type(mapper, parameter, source);
        result += ':';
        result += std::to_string(type.size());
        result += ':';
        result += type;
    }
    const auto return_type = method_return_type(mapper, method);
    result += "->";
    result += std::to_string(return_type.size());
    result += ':';
    result += return_type;
    return result;
}

void write_if_changed(
    const std::filesystem::path& path,
    const std::string& content
) {
    std::ifstream previous(path, std::ios::binary);
    if (previous) {
        const std::string old_content(
            std::istreambuf_iterator<char> {previous},
            std::istreambuf_iterator<char> {}
        );
        if (old_content == content) {
            return;
        }
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "Failed to write generated file '" + path.string() + "'"
        );
    }
    output << content;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to open manual Luau definitions '" + path.string() + "'"
        );
    }
    return {
        std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {},
    };
}

[[nodiscard]] std::string
initializer_type(TypeMapper& mapper, const Class& cls) {
    if (cls.properties.empty()) {
        return "{ [string]: any }";
    }
    std::ostringstream result;
    result << "{ ";
    for (std::size_t index = 0; index < cls.properties.size(); ++index) {
        if (index != 0) {
            result << ", ";
        }
        const auto& property = cls.properties[index];
        const auto type = mapper.map(property.cpp_type);
        result << property.name << ": " << type;
        if (type != "any" && !type.ends_with('?')) {
            result << '?';
        }
    }
    result << " }";
    return result.str();
}

void emit_class_declaration(
    std::ostringstream& output,
    TypeMapper& mapper,
    const Class& cls
) {
    const auto internal_type = internal_type_name(cls.cpp_name);
    output << "declare extern type " << internal_type << " with\n";
    for (const auto& property : cls.properties) {
        if (valid_identifier(property.name)) {
            output << "    " << property.name << ": "
                   << mapper.map(property.cpp_type) << "\n";
        }
    }
    std::unordered_set<std::string> emitted_method_signatures;
    for (const auto& method : cls.methods) {
        if (method.is_static || method.name.starts_with("operator") ||
            !valid_identifier(method.name)) {
            continue;
        }
        if (!emitted_method_signatures
                 .insert(method_signature_key(mapper, method))
                 .second) {
            continue;
        }
        const auto source = dependent_parameter(method);
        if (source) {
            output << "    " << method.name << ": <T>(self: " << internal_type;
        } else {
            output << "    function " << method.name << "(self";
        }
        if (!method.parameters.empty()) {
            output << ", " << parameters(mapper, method.parameters, source);
        }
        output << (source ? ") -> " : "): ")
               << method_return_type(mapper, method) << "\n";
    }
    output << "end\n";
    output << "export type " << cls.name << " = " << internal_type << "\n";

    const auto public_value = has_annotation(cls.annotations, "ScriptPrelude");
    output << "declare "
           << (public_value ? cls.name : internal_value_name(cls.cpp_name))
           << ": {\n";
    if (!cls.abstract) {
        std::vector<std::string> constructors;
        constructors.reserve(cls.constructors.size() + 2);
        std::unordered_set<std::string> constructor_signatures;
        for (const auto& constructor : cls.constructors) {
            if (!constructor_signatures
                     .insert(
                         parameter_signature_key(mapper, constructor.parameters)
                     )
                     .second) {
                continue;
            }
            constructors.push_back(
                "(" + parameters(mapper, constructor.parameters) + ") -> " +
                cls.name
            );
        }
        const bool has_default_constructor = std::ranges::any_of(
            cls.constructors,
            [](const Constructor& constructor) {
                return constructor.parameters.empty();
            }
        );
        if (cls.constructors.empty()) {
            constructors.push_back("() -> " + cls.name);
        }
        if (cls.constructors.empty() || has_default_constructor) {
            constructors.push_back(
                "(values: " + initializer_type(mapper, cls) + ") -> " + cls.name
            );
        }
        if (!constructors.empty()) {
            output << "    new: ";
            for (std::size_t index = 0; index < constructors.size(); ++index) {
                if (index != 0) {
                    output << " & ";
                }
                output << '(' << constructors[index] << ')';
            }
            output << ",\n";
        }
    }
    output << "    __ets_type: " << cls.name << "?,\n";
    std::map<std::string, std::vector<const Method*>> static_methods;
    for (const auto& method : cls.methods) {
        if (!method.is_static || method.name.starts_with("operator") ||
            !valid_identifier(method.name)) {
            continue;
        }
        static_methods[method.name].push_back(&method);
    }
    for (const auto& [name, overloads] : static_methods) {
        output << "    " << name << ": ";
        std::unordered_set<std::string> emitted_signatures;
        bool first = true;
        for (const auto* overload : overloads) {
            if (!emitted_signatures
                     .insert(method_signature_key(mapper, *overload))
                     .second) {
                continue;
            }
            if (!first) {
                output << " & ";
            }
            output << '(' << function_type(mapper, *overload) << ')';
            first = false;
        }
        output << ",\n";
    }
    output << "}\n\n";
}

void emit_enum_declaration(std::ostringstream& output, const Enum& enm) {
    const auto internal_type = internal_type_name(enm.cpp_name);
    output << "export type " << internal_type << " = number\n";
    output << "export type " << enm.name << " = " << internal_type << "\n";
    const auto public_value = has_annotation(enm.annotations, "ScriptPrelude");
    output << "declare "
           << (public_value ? enm.name : internal_value_name(enm.cpp_name))
           << ": {\n";
    for (const auto& value : enm.values) {
        if (valid_identifier(value.name)) {
            output << "    " << value.name << ": " << enm.name << ",\n";
        }
    }
    output << "}\n\n";
}

template<typename Type>
void append_module_export(
    std::map<std::string, std::vector<const Type*>>& modules,
    const Type& value
) {
    modules[value.module].push_back(&value);
}

} // namespace

EmissionSummary emit_definitions(
    const Database& database,
    const std::filesystem::path& manual_definitions,
    const std::filesystem::path& output_directory,
    const std::filesystem::path& libraries_directory
) {
    TypeMapper mapper {database};
    std::ostringstream globals;
    globals << "-- Generated by entisium-luau-defgen. Do not edit.\n\n";
    globals << read_file(manual_definitions);
    if (!globals.str().ends_with('\n')) {
        globals << '\n';
    }
    globals << '\n';
    for (const auto& cls : database.classes) {
        emit_class_declaration(globals, mapper, cls);
    }
    for (const auto& enm : database.enums) {
        emit_enum_declaration(globals, enm);
    }

    const auto modules_directory = output_directory / "modules";
    std::filesystem::create_directories(modules_directory);
    std::map<std::string, std::vector<const Class*>> module_classes;
    std::map<std::string, std::vector<const Enum*>> module_enums;
    for (const auto& cls : database.classes) {
        append_module_export(module_classes, cls);
    }
    for (const auto& enm : database.enums) {
        append_module_export(module_enums, enm);
    }

    std::set<std::string> module_names;
    for (const auto& [module, unused] : module_classes) {
        module_names.insert(module);
    }
    for (const auto& [module, unused] : module_enums) {
        module_names.insert(module);
    }
    for (const auto& entry :
         std::filesystem::directory_iterator(modules_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".luau" &&
            !module_names.contains(entry.path().stem().string())) {
            std::filesystem::remove(entry.path());
        }
    }
    for (const auto& module : module_names) {
        std::ostringstream source;
        source << "--!strict\n";
        source << "-- Generated by entisium-luau-defgen. Do not edit.\n\n";
        for (const auto* cls : module_classes[module]) {
            source << "export type " << cls->name << " = "
                   << internal_type_name(cls->cpp_name) << "\n";
        }
        for (const auto* enm : module_enums[module]) {
            source << "export type " << enm->name << " = "
                   << internal_type_name(enm->cpp_name) << "\n";
        }
        source << "\nreturn {\n";
        for (const auto* cls : module_classes[module]) {
            const auto public_value =
                has_annotation(cls->annotations, "ScriptPrelude");
            source << "    " << cls->name << " = "
                   << (public_value ? cls->name :
                                      internal_value_name(cls->cpp_name))
                   << ",\n";
        }
        for (const auto* enm : module_enums[module]) {
            const auto public_value =
                has_annotation(enm->annotations, "ScriptPrelude");
            source << "    " << enm->name << " = "
                   << (public_value ? enm->name :
                                      internal_value_name(enm->cpp_name))
                   << ",\n";
        }
        source << "}\n";
        write_if_changed(modules_directory / (module + ".luau"), source.str());
    }

    const auto library_manifest = Json::parse(read_file(libraries_directory));
    if (library_manifest.at("format") != "entisium.luau-libraries" ||
        library_manifest.at("version") != 1) {
        throw std::runtime_error("Unsupported Luau library manifest");
    }
    Json aliases {{"entisium", "./modules"}, {"internal", "./internal"}};
    Json outputs = Json::array();
    const auto publish = [&](const std::string& name,
                             const std::string& content) {
        if (name.empty() || name.front() != '@' ||
            name.find("..") != std::string::npos ||
            name.find('\\') != std::string::npos ||
            name.find(':') != std::string::npos) {
            throw std::runtime_error("Invalid SDK module name: " + name);
        }
        const auto plain = name.substr(1);
        const auto slash = plain.find('/');
        const auto alias = plain.substr(0, slash);
        const auto relative =
            plain + (slash == std::string::npos ? "/init.luau" : ".luau");
        if (std::find(outputs.begin(), outputs.end(), relative) !=
            outputs.end()) {
            throw std::runtime_error(
                "Conflicting SDK library file: " + relative
            );
        }
        aliases[alias] = "./" + alias;
        write_if_changed(output_directory / relative, content);
        outputs.push_back(relative);
    };
    for (const auto& library : library_manifest.at("libraries")) {
        const auto library_globals = library.value("globals", std::string {});
        if (!library_globals.empty()) {
            globals << "\n-- Native declarations from "
                    << library.at("name").get<std::string>() << "\n"
                    << library_globals << "\n";
        }
        const auto name = library.at("name").get<std::string>();
        const auto source = library.at("source").get<std::string>();
        const auto native = library.at("native").get<std::string>();
        if (!source.empty()) {
            publish(name, source);
        } else {
            publish(name, native);
        }
        if (!native.empty()) {
            publish("@internal/" + name.substr(1), native);
        }
        const auto parent = name.find_last_of('/');
        const auto prefix =
            parent == std::string::npos ? name : name.substr(0, parent);
        const auto source_modules = library.value("modules", Json::object());
        for (const auto& [file, content] : source_modules.items()) {
            if (!file.ends_with(".luau") || file.ends_with(".d.luau")) {
                throw std::runtime_error(
                    "Invalid library source file: " + file
                );
            }
            publish(
                prefix + "/" + file.substr(0, file.size() - 5),
                content.get<std::string>()
            );
        }
    }
    write_if_changed(output_directory / "globals.d.luau", globals.str());
    const auto owned = output_directory / "library-files.json";
    if (std::filesystem::exists(owned)) {
        for (const auto& previous : Json::parse(read_file(owned))) {
            const auto relative = previous.get<std::string>();
            const auto path = std::filesystem::path(relative);
            if (path.is_absolute() ||
                relative.find("..") != std::string::npos ||
                relative.find(':') != std::string::npos ||
                relative.find('\\') != std::string::npos) {
                throw std::runtime_error("Invalid SDK cleanup path");
            }
            if (std::find(outputs.begin(), outputs.end(), previous) ==
                outputs.end()) {
                std::filesystem::remove(output_directory / path);
            }
        }
    }
    write_if_changed(owned, outputs.dump(2) + '\n');
    write_if_changed(
        output_directory / ".luaurc",
        Json {{"aliases", aliases}}.dump(2) + '\n'
    );
    const Json index {
        {"format", "entisium.luau-definitions"},
        {"version", 1},
        {"definitionFiles", Json {{"@entisium", "globals.d.luau"}}},
        {"baseLuaurc", ".luaurc"},
    };
    write_if_changed(output_directory / "index.json", index.dump(2) + '\n');

    for (const auto& module : module_names) {
        outputs.push_back("modules/" + module + ".luau");
    }
    for (const auto* file :
         {"globals.d.luau", ".luaurc", "index.json", "library-files.json"}) {
        outputs.push_back(file);
    }
    write_if_changed(
        output_directory / "sdk-files.json",
        outputs.dump(2) + '\n'
    );

    return EmissionSummary {
        .class_count = database.classes.size(),
        .enum_count = database.enums.size(),
        .module_count = module_names.size(),
        .unsupported_cpp_types = mapper.unsupported_types(),
    };
}

} // namespace ets::luau_defgen
