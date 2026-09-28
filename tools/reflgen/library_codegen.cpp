#include "library_codegen.hpp"

#include "../luau_defgen/type_mapper.hpp"
#include "codegen.hpp"
#include "model.hpp"
#include "parser.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ets::reflgen {
namespace {
using Json = nlohmann::ordered_json;
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read library input: " + path.string());
    }
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path) && read(path) == content) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(
            "Cannot write library output: " + path.string()
        );
    }
    stream << content;
}
std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte >= 32 && byte < 127) {
            result += static_cast<char>(byte);
        } else {
            // Encode UTF-8 bytes, including non-BMP characters, without C++
            // universal-character escapes or dependence on the source codepage.
            result += '\\';
            result += static_cast<char>('0' + ((byte >> 6) & 7));
            result += static_cast<char>('0' + ((byte >> 3) & 7));
            result += static_cast<char>('0' + (byte & 7));
        }
    }
    return result + '"';
}
std::string source_literal(std::string_view value) {
    std::string result = "std::string {}";
    while (!value.empty()) {
        // Separate expressions also avoid MSVC's concatenated literal limit.
        const auto count = std::min<std::size_t>(2048, value.size());
        result += " + " + quote(value.substr(0, count)) + "\n";
        value.remove_prefix(count);
    }
    return result;
}
const ReflectionAnnotation* annotation(
    const std::vector<ReflectionAnnotation>& values,
    std::string_view name
) {
    const auto found =
        std::ranges::find(values, name, &ReflectionAnnotation::name);
    return found == values.end() ? nullptr : &*found;
}
std::string field(
    const ReflectionAnnotation& value,
    std::string_view name,
    std::string fallback = {}
) {
    const auto found =
        std::ranges::find(value.arguments, name, &AnnotationArgument::name);
    return found == value.arguments.end() ? fallback : found->value;
}
bool identifier(const std::string& value) {
    static const std::regex pattern("[A-Za-z_][A-Za-z0-9_]*");
    static const std::set<std::string> reserved {
        "and", "break",    "do",     "else", "elseif", "end",   "false",
        "for", "function", "if",     "in",   "local",  "nil",   "not",
        "or",  "repeat",   "return", "then", "true",   "until", "while"
    };
    return std::regex_match(value, pattern) && !reserved.contains(value);
}
struct Library {
    std::string name, source, declarations, header, cpp_type, extra_types,
        custom_exports;
    std::vector<std::string> dependencies;
    std::vector<MethodInfo> methods;
    std::string globals;
    bool custom {false};
    ParseResult reflected;
    std::map<std::string, std::string> modules;
};
} // namespace

void generate_luau_libraries(
    const std::filesystem::path& catalog,
    const std::filesystem::path& output,
    const std::filesystem::path& manifest,
    const std::vector<std::string>& includes,
    const std::string& function,
    bool partial
) {
    if (!identifier(function)) {
        throw std::runtime_error(
            "Invalid library registration function: " + function
        );
    }
    const auto inputs = Json::parse(read(catalog));
    std::vector<Library> libraries;
    std::set<std::string> names;
    std::set<std::string> dependencies;
    for (const auto& input : inputs) {
        const auto descriptor = std::filesystem::path(input.get<std::string>());
        const auto root = descriptor.parent_path();
        const auto config = Json::parse(read(descriptor));
        dependencies.insert(
            std::filesystem::absolute(descriptor).generic_string()
        );
        for (const auto& [key, value] : config.items()) {
            if (key != "name" && key != "header" && key != "source" &&
                key != "definitions" && key != "dependencies" &&
                key != "headers" && key != "fallback_name" &&
                key != "modules") {
                throw std::runtime_error("Unknown library field: " + key);
            }
        }
        Library library;
        for (const auto& module : config.value("modules", Json::array())) {
            const auto name = module.at("path").get<std::string>();
            const auto path = std::filesystem::path(name);
            if (name.empty() || path.is_absolute() ||
                name.find('\\') != std::string::npos ||
                name.find(':') != std::string::npos ||
                name.starts_with("../") ||
                path.lexically_normal().generic_string() != name ||
                !name.ends_with(".luau") || name.ends_with(".d.luau") ||
                library.modules.contains(name)) {
                throw std::runtime_error(
                    "Invalid or duplicate library source path: " + name
                );
            }
            const auto file = root / module.at("file").get<std::string>();
            library.modules.emplace(name, read(file));
            dependencies.insert(
                std::filesystem::absolute(file).generic_string()
            );
        }
        for (const auto* key : {"source", "definitions"}) {
            if (config.contains(key)) {
                dependencies.insert(
                    std::filesystem::absolute(
                        root / config.at(key).get<std::string>()
                    )
                        .generic_string()
                );
            }
        }
        library.dependencies =
            config.value("dependencies", std::vector<std::string> {});
        if (config.contains("source")) {
            library.source =
                read(root / config.at("source").get<std::string>());
        }
        if (config.contains("header") || config.contains("headers")) {
            if (config.contains("name")) {
                throw std::runtime_error(
                    "Native module name belongs in LuauLibrary, not " +
                    descriptor.string()
                );
            }
            auto headers = config.value("headers", std::vector<std::string> {});
            if (config.contains("header")) {
                headers.push_back(config.at("header").get<std::string>());
            }
            for (auto& header : headers) {
                header = std::filesystem::absolute(root / header)
                             .lexically_normal()
                             .generic_string();
            }
            library.header = descriptor.string();
            auto parsed = HeaderParser(headers, includes, false).parse();
            if (parsed.has_errors) {
                throw std::runtime_error(
                    "Cannot generate library from invalid header: " +
                    library.header
                );
            }
            dependencies.insert(
                parsed.dependencies.begin(),
                parsed.dependencies.end()
            );
            library.reflected = parsed.result;
            for (const auto& cls : parsed.result.classes) {
                const auto* mark = annotation(cls.annotations, "LuauLibrary");
                if (!mark) {
                    continue;
                }
                if (!library.cpp_type.empty()) {
                    throw std::runtime_error(
                        "Expected one LuauLibrary per descriptor: " +
                        descriptor.string()
                    );
                }
                for (const auto& argument : mark->arguments) {
                    if (argument.name != "name" && argument.name != "custom" &&
                        argument.name != "types" &&
                        argument.name != "exports" &&
                        argument.name != "globals") {
                        throw std::runtime_error(
                            "Unknown LuauLibrary field: " + argument.name
                        );
                    }
                }
                library.name = field(*mark, "name");
                library.header = cls.source_file;
                library.cpp_type = cls.name;
                const auto custom = field(*mark, "custom", "false");
                if (custom != "false" && custom != "true") {
                    throw std::runtime_error(
                        "LuauLibrary.custom must be boolean"
                    );
                }
                library.custom = custom == "true";
                library.globals = field(*mark, "globals");
                library.extra_types = field(*mark, "types");
                library.custom_exports = field(*mark, "exports");
                if (!library.custom && !library.custom_exports.empty()) {
                    throw std::runtime_error(
                        "LuauLibrary.exports requires a custom library"
                    );
                }
                if (!library.custom) {
                    std::set<std::string> exports;
                    for (auto method : cls.methods) {
                        if (method.access != "public") {
                            continue;
                        }
                        auto name = method.name;
                        if (const auto* mark_export =
                                annotation(method.annotations, "LuauExport")) {
                            for (const auto& argument :
                                 mark_export->arguments) {
                                if (argument.name != "name" &&
                                    argument.name != "signature") {
                                    throw std::runtime_error(
                                        "Unknown LuauExport field: " +
                                        argument.name
                                    );
                                }
                            }
                            name = field(*mark_export, "name", name);
                        }
                        if (!identifier(name) || !exports.insert(name).second ||
                            method.is_abstract ||
                            !method.ref_qualifier.empty()) {
                            throw std::runtime_error(
                                "Invalid or duplicate Luau export: " +
                                cls.name + "." + name
                            );
                        }
                        // Declaration hints cannot bypass binding lifetime
                        // rules.
                        if (method.type_name.find('*') != std::string::npos ||
                            std::ranges::any_of(
                                method.parameters,
                                [](const auto& param) {
                                    return param.type_name.find('*') !=
                                           std::string::npos;
                                }
                            )) {
                            throw std::runtime_error(
                                "Pointer exports require an explicit lifetime "
                                "contract: " +
                                method.name
                            );
                        }
                        if (method.type_name.find('&') != std::string::npos &&
                            (method.is_static || !method.parameters.empty())) {
                            throw std::runtime_error(
                                "Reference returns require an instance-owned "
                                "result with no arguments: " +
                                method.name
                            );
                        }
                        library.methods.push_back(std::move(method));
                    }
                }
            }
            if (library.cpp_type.empty() && !config.contains("fallback_name")) {
                throw std::runtime_error(
                    "No LuauLibrary found in " + library.header
                );
            }
            if (library.custom) {
                if (!config.contains("definitions") &&
                    library.custom_exports.empty()) {
                    throw std::runtime_error(
                        "Custom library requires a type contract: " +
                        library.name
                    );
                }
                if (config.contains("definitions")) {
                    if (!library.custom_exports.empty() ||
                        !library.extra_types.empty()) {
                        throw std::runtime_error(
                            "Custom declaration metadata conflicts with "
                            "definitions file"
                        );
                    }
                    library.declarations = read(
                        root / config.at("definitions").get<std::string>()
                    );
                } else {
                    library.declarations =
                        "--!strict\n" + library.extra_types +
                        "\nlocal exports: {\n" + library.custom_exports +
                        "\n} = (nil :: any)\nreturn exports\n";
                }
            } else if (config.contains("definitions")) {
                library.declarations =
                    read(root / config.at("definitions").get<std::string>());
            }
        }
        if (library.cpp_type.empty()) {
            library.header.clear();
            library.name = config.value(
                "name",
                config.value("fallback_name", std::string {})
            );
            if (library.source.empty()) {
                throw std::runtime_error(
                    "Pure Luau library requires source: " + library.name
                );
            }
        }
        static const std::regex module_name(
            "@[a-z][a-z0-9_-]*(/[a-z][a-z0-9_-]*)*"
        );
        if (!std::regex_match(library.name, module_name) ||
            library.name.starts_with("@internal/") ||
            library.name.starts_with("@entisium/") ||
            !names.insert(library.name).second) {
            throw std::runtime_error(
                "Invalid or duplicate library name: " + library.name
            );
        }
        libraries.push_back(std::move(library));
    }
    std::ranges::sort(libraries, {}, &Library::name);
    std::map<std::string, int> marks;
    std::function<void(const Library&)> visit = [&](const Library& library) {
        if (marks[library.name] == 1) {
            throw std::runtime_error(
                "Circular Luau library dependency: " + library.name
            );
        }
        if (marks[library.name] == 2) {
            return;
        }
        marks[library.name] = 1;
        for (auto dependency : library.dependencies) {
            const bool internal = dependency.starts_with("@internal/");
            if (internal) {
                dependency = "@" + dependency.substr(10);
            }
            const auto found =
                std::ranges::find(libraries, dependency, &Library::name);
            if (partial && found == libraries.end()) {
                continue;
            }
            if (found == libraries.end() ||
                (internal && found->cpp_type.empty())) {
                throw std::runtime_error(
                    "Missing dependency " + dependency + " for " + library.name
                );
            }
            visit(*found);
        }
        marks[library.name] = 2;
    };
    for (const auto& library : libraries) {
        visit(library);
    }

    std::ostringstream cpp;
    cpp << "// Generated by entisium-reflgen.\n#include "
           "\"scripting/library.hpp\"\n#include "
           "\"scripting/library_binding.hpp\"\n#include "
           "\"refl/method.hpp\"\n#include "
           "\"scripting/detail/binding.hpp\"\n#include <lua.h>\n";
    for (const auto& library : libraries) {
        if (!library.header.empty()) {
            cpp << "#include " << quote(library.header) << "\n";
        }
    }
    cpp << "namespace ets {\nnamespace {\n";
    auto published = Json::array();
    auto generated_files = Json::array();
    for (std::size_t index = 0; index < libraries.size(); ++index) {
        auto& library = libraries[index];
        if (!library.cpp_type.empty() && !library.custom) {
            auto reflected = library.reflected;
            std::erase_if(reflected.classes, [](const auto& cls) {
                return annotation(cls.annotations, "LuauLibrary") ||
                       !is_reflected_class(cls);
            });
            const auto register_function =
                function + "_types_" + std::to_string(index);
            auto reflection_file =
                output.parent_path() / (register_function + ".inc");
            generate_cpp_file(
                reflected,
                output.parent_path(),
                reflection_file,
                register_function,
                ""
            );
            generated_files.push_back(
                std::filesystem::absolute(reflection_file).generic_string()
            );
            // Existing reflgen owns property, constructor and container
            // registration.
            cpp << "} }\n#include "
                << quote(reflection_file.filename().generic_string())
                << "\nnamespace ets { namespace {\n";
            luau_defgen::Database database;
            std::map<std::string, std::string> aliases, input_overrides;
            std::set<std::string> opaque, public_names;
            for (const auto& cls : reflected.classes) {
                auto name = cls.local_name;
                if (const auto* mark =
                        annotation(cls.annotations, "LuauType")) {
                    for (const auto& argument : mark->arguments) {
                        if (argument.name != "name" &&
                            argument.name != "input") {
                            throw std::runtime_error(
                                "Unknown LuauType field: " + argument.name
                            );
                        }
                    }
                    name = field(*mark, "name", name);
                    input_overrides[cls.name] = field(*mark, "input");
                }
                if (!identifier(name) || !public_names.insert(name).second) {
                    throw std::runtime_error(
                        "Invalid or duplicate Luau type alias: " + name
                    );
                }
                aliases[cls.name] = name;
                if (std::ranges::none_of(
                        cls.properties,
                        is_reflected_property
                    )) {
                    opaque.insert(cls.name);
                } else if (!public_names.insert(name + "Input").second) {
                    throw std::runtime_error(
                        "Conflicting Luau input alias: " + name
                    );
                }
                database.classes.push_back(
                    {.cpp_name = cls.name, .name = name}
                );
            }
            const auto reject_enum = [&](const std::string& type) {
                static const std::regex token("[A-Za-z_][A-Za-z_0-9:]*");
                for (auto it =
                         std::sregex_iterator(type.begin(), type.end(), token);
                     it != std::sregex_iterator();
                     ++it) {
                    for (const auto& enm : reflected.enums) {
                        if (it->str() == enm.name ||
                            it->str() == enm.local_name) {
                            throw std::runtime_error(
                                "Enum types are not supported in Luau library "
                                "contracts: " +
                                type
                            );
                        }
                    }
                }
            };
            for (const auto& method : library.methods) {
                reject_enum(method.type_name);
                for (const auto& parameter : method.parameters) {
                    reject_enum(parameter.type_name);
                }
            }
            for (const auto& cls : reflected.classes) {
                for (const auto& property : cls.properties) {
                    if (is_reflected_property(property)) {
                        reject_enum(property.type_name);
                    }
                }
            }
            luau_defgen::TypeMapper mapper(database);
            auto input_database = database;
            for (auto& cls : input_database.classes) {
                if (!opaque.contains(cls.cpp_name)) {
                    cls.name += "Input";
                }
            }
            luau_defgen::TypeMapper input_mapper(input_database);
            auto luau_type = [&](const std::string& type, bool result) {
                if (type.find('*') != std::string::npos) {
                    throw std::runtime_error(
                        "Pointer exports require an explicit lifetime "
                        "contract: " +
                        type
                    );
                }
                auto mapped = result ? mapper.map(type) :
                                       mapper.map_library_parameter(type);
                if (!result) {
                    auto input = input_mapper.map_library_parameter(type);
                    if (input != mapped && input != "any") {
                        mapped += " | " + input;
                    }
                }
                if (mapped == "any" && library.declarations.empty()) {
                    throw std::runtime_error("No Luau declaration for " + type);
                }
                return mapped;
            };
            cpp << "struct Binding" << index << " : LuauLibraryBinding<"
                << library.cpp_type
                << "> {\nusing LuauLibraryBinding::LuauLibraryBinding;\n";
            std::ostringstream types;
            types << "--!strict\n-- Generated native contract.\n"
                  << library.extra_types << "\n";
            for (const auto& cls : reflected.classes) {
                const auto& name = aliases.at(cls.name);
                types << "export type " << name << " = {\n";
                if (opaque.contains(cls.name)) {
                    // A private, uninhabitable phantom field prevents an opaque
                    // C++ handle being confused with an arbitrary empty table.
                    types << "    read "
                          << luau_defgen::internal_type_name(cls.name)
                          << ": never,\n";
                }
                for (const auto& property : cls.properties) {
                    if (is_reflected_property(property)) {
                        types << "    " << property.name << ": "
                              << luau_type(property.type_name, true) << ",\n";
                    }
                }
                types << "}\n";
                if (opaque.contains(cls.name)) {
                    continue;
                }
                if (!input_overrides[cls.name].empty()) {
                    types << "export type " << name
                          << "Input = " << input_overrides[cls.name] << "\n";
                    continue;
                }
                types << "export type " << name << "Input = {\n";
                for (const auto& property : cls.properties) {
                    if (is_reflected_property(property)) {
                        types << "    " << property.name << ": ("
                              << input_mapper.map_library_parameter(
                                     property.type_name
                                 )
                              << ")?,\n";
                    }
                }
                types << "}\n";
            }
            types << "local exports: "
                     "{\n";
            for (std::size_t j = 0; j < library.methods.size(); ++j) {
                const auto& method = library.methods[j];
                auto name = method.name;
                if (const auto* mark =
                        annotation(method.annotations, "LuauExport")) {
                    name = field(*mark, "name", name);
                }
                const auto* export_mark =
                    annotation(method.annotations, "LuauExport");
                const auto signature = export_mark ?
                                           field(*export_mark, "signature") :
                                           std::string {};
                if (!signature.empty()) {
                    types << "    read " << name << ": " << signature << ",\n";
                } else {
                    types << "    read " << name << ": (";
                    for (std::size_t k = 0; k < method.parameters.size(); ++k) {
                        const auto& param = method.parameters[k];
                        if (k) {
                            types << ", ";
                        }
                        types << (identifier(param.name) ?
                                      param.name :
                                      "arg" + std::to_string(k))
                              << ": " << luau_type(param.type_name, false);
                    }
                    types << ") -> " << luau_type(method.type_name, true)
                          << ",\n";
                }
                cpp << "static int call" << j << "(lua_State* L) {\n"
                    << "const MethodImpl method(" << quote(name) << ", &"
                    << library.cpp_type << "::" << method.name << ");\n"
                    << "return detail::invoke_luau_library(L, method, "
                       "static_cast<Binding"
                    << index
                    << "*>(lua_touserdata(L, "
                       "lua_upvalueindex(1)))->m_instance"
                    << ");\n}\n";
            }
            types << "} = (nil :: any)\nreturn exports\n";
            if (library.declarations.empty()) {
                if (!mapper.unsupported_types().empty()) {
                    throw std::runtime_error(
                        "Unsupported declaration type: " +
                        *mapper.unsupported_types().begin()
                    );
                }
                library.declarations = types.str();
            }
            cpp << "void open(lua_State* L) override { refl::generated::"
                << register_function
                << "(Registry::instance()); "
                   "detail::install_luau_borrowed_object_metatable(L); "
                   "lua_newtable(L);\n";
            for (std::size_t j = 0; j < library.methods.size(); ++j) {
                const auto& method = library.methods[j];
                auto name = method.name;
                if (const auto* mark =
                        annotation(method.annotations, "LuauExport")) {
                    name = field(*mark, "name", name);
                }
                cpp << "lua_pushlightuserdata(L, this); lua_pushcclosure(L, "
                       "call"
                    << j << ", " << quote(name) << ", 1); lua_setfield(L, -2, "
                    << quote(name) << ");\n";
            }
            cpp << "}\n};\n";
        }
        published.push_back(
            Json {
                {"name", library.name},
                {"source", library.source},
                {"native", library.declarations},
                {"globals", library.globals},
                {"modules", library.modules},
                {"dependencies", library.dependencies}
            }
        );
    }
    cpp << "}\nstd::span<const LuauLibraryDefinition> " << function
        << "() {\nstatic const std::vector<LuauLibraryDefinition> definitions "
           "{\n";
    for (std::size_t index = 0; index < libraries.size(); ++index) {
        const auto& library = libraries[index];
        cpp << "{.name=" << quote(library.name)
            << ", .source=" << source_literal(library.source)
            << ", .dependencies={";
        for (const auto& dependency : library.dependencies) {
            cpp << quote(dependency) << ",";
        }
        cpp << "}";
        if (!library.cpp_type.empty()) {
            cpp << ", .create=[](LuauLibraryServices& services) -> "
                   "std::unique_ptr<LuauNativeLibrary> { ";
            if (library.custom) {
                cpp << "return std::make_unique<" << library.cpp_type
                    << ">(services);";
            } else {
                cpp << "(void)services; return std::make_unique<Binding"
                    << index << ">(services);";
            }
            cpp << "}";
        }
        cpp << ", .modules={";
        for (const auto& [path, source] : library.modules) {
            cpp << "{" << quote(path) << "," << source_literal(source) << "},";
        }
        cpp << "}},\n";
    }
    cpp << "};\nreturn definitions;\n}\n}\n";
    write(output, cpp.str());
    write(
        manifest,
        Json {
            {"format", "entisium.luau-libraries"},
            {"version", 1},
            {"dependencies", dependencies},
            {"libraries", published},
            {"generated_files", generated_files}
        }.dump(2) +
            "\n"
    );
}
} // namespace ets::reflgen
