#include "codegen.hpp"
#include "library_codegen.hpp"
#include "manifest.hpp"
#include "metadata.hpp"
#include "model.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::vector<std::string> headers;
    std::vector<std::string> includes;
    std::vector<std::string> metadata_files;
    std::vector<std::string> registrars;
    std::filesystem::path root_dir = std::filesystem::current_path();
    std::filesystem::path output_file;
    std::filesystem::path manifest_output;
    std::filesystem::path metadata_output;
    std::filesystem::path libraries;
    bool library_part = false;
    std::filesystem::path stamp_file;
    std::filesystem::path depfile;
    std::string dep_target;
    std::string function_name = "register_reflection";
    std::string script_module;
    bool aggregate = false;
    bool validate_metadata = false;
    bool verbose = false;
};

[[nodiscard]] bool is_header_path(std::string_view path) {
    const auto ext = std::filesystem::path(path).extension().string();
    return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

void configure_options(CLI::App& app, Options& options) {
    app.add_option("headers", options.headers, "Header files to parse");
    app.add_flag(
        "--library-part",
        options.library_part,
        "Generate one target's library; dependencies are validated by the host "
        "catalog"
    );
    app.add_option(
        "--libraries",
        options.libraries,
        "Luau library build catalog"
    );
    auto* include_option = app.add_option_function<std::string>(
        "-I,--include",
        [&options](const std::string& include) {
            options.includes.push_back(include);
        },
        "Include directory"
    );
    include_option->trigger_on_parse();
    app.add_flag("-v,--verbose", options.verbose, "Enable verbose output");
    app.add_flag("--aggregate", options.aggregate, "Generate aggregate output");
    app.add_flag(
        "--validate-metadata",
        options.validate_metadata,
        "Validate reflection metadata files"
    );
    app.add_option(
        "--rootdir",
        options.root_dir,
        "Root directory for relative paths"
    );
    app.add_option("-o,--output", options.output_file, "Output C++ file");
    app.add_option(
        "--manifest-output",
        options.manifest_output,
        "Reflection manifest JSON file to write"
    );
    app.add_option(
        "--metadata-output",
        options.metadata_output,
        "Reflection metadata file to write"
    );
    auto* metadata_option = app.add_option_function<std::string>(
        "--metadata",
        [&options](const std::string& metadata) {
            options.metadata_files.push_back(metadata);
        },
        "Reflection metadata file to validate"
    );
    metadata_option->trigger_on_parse();
    app.add_option("--depfile", options.depfile, "Dependency file to write");
    app.add_option(
        "--dep-target",
        options.dep_target,
        "Target name to write into the dependency file"
    );
    app.add_option(
        "--function",
        options.function_name,
        "Generated registration function name"
    );
    app.add_option(
        "--script-module",
        options.script_module,
        "Script module that owns the reflected types"
    );
    auto* registrar_option = app.add_option_function<std::string>(
        "--registrar",
        [&options](const std::string& registrar) {
            options.registrars.push_back(registrar);
        },
        "Registration function to call from aggregate output"
    );
    registrar_option->trigger_on_parse();
    app.add_option("--stamp", options.stamp_file, "Stamp file to write");
}

void normalize_options(Options& options) {
    options.headers.erase(
        std::remove_if(
            options.headers.begin(),
            options.headers.end(),
            [](const std::string& header) {
                return !is_header_path(header);
            }
        ),
        options.headers.end()
    );

    if (options.validate_metadata) {
        if (options.metadata_files.empty()) {
            throw std::runtime_error("no reflection metadata files provided");
        }
        return;
    }
    if (options.headers.empty() && !options.aggregate &&
        options.libraries.empty()) {
        throw std::runtime_error("no headers provided");
    }
    if (options.aggregate && options.output_file.empty()) {
        throw std::runtime_error("aggregate output file is required");
    }
}

void write_stamp_file(const std::filesystem::path& stamp_file) {
    if (stamp_file.empty()) {
        return;
    }
    if (stamp_file.has_parent_path()) {
        std::filesystem::create_directories(stamp_file.parent_path());
    }

    std::ofstream out(stamp_file, std::ios::binary);
    if (!out) {
        throw std::runtime_error(
            "failed to open stamp file: " + stamp_file.string()
        );
    }
    out << std::chrono::system_clock::now().time_since_epoch().count() << '\n';
}

[[nodiscard]] std::filesystem::path
normalized_absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

[[nodiscard]] std::string
generic_absolute_path(const std::filesystem::path& path) {
    return normalized_absolute_path(path).generic_string();
}

[[nodiscard]] std::string comparable_path(std::string path) {
    std::ranges::replace(path, '\\', '/');
#ifdef _WIN32
    std::ranges::transform(path, path.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return path;
}

[[nodiscard]] bool path_is_under_root(
    const std::filesystem::path& filepath,
    const std::filesystem::path& root_dir
) {
    const auto file = comparable_path(generic_absolute_path(filepath));
    const auto root = comparable_path(generic_absolute_path(root_dir));
    return file == root || file.starts_with(root + "/");
}

void insert_unique_path(
    std::vector<std::string>& paths,
    const std::filesystem::path& filepath
) {
    auto normalized = generic_absolute_path(filepath);
    if (normalized.empty()) {
        return;
    }

    const auto comparable = comparable_path(normalized);
    const auto found =
        std::ranges::find_if(paths, [&comparable](const std::string& path) {
            return comparable_path(path) == comparable;
        });
    if (found == paths.end()) {
        paths.push_back(std::move(normalized));
    }
}

[[nodiscard]] std::vector<std::string> project_dependencies(
    const std::vector<std::string>& dependencies,
    const std::filesystem::path& root_dir
) {
    std::vector<std::string> result;
    for (const auto& dependency : dependencies) {
        if (path_is_under_root(dependency, root_dir)) {
            insert_unique_path(result, dependency);
        }
    }
    std::ranges::sort(result);
    return result;
}

[[nodiscard]] std::string escape_makefile_token(std::string token) {
    std::ranges::replace(token, '\\', '/');
    std::string escaped;
    escaped.reserve(token.size());
    for (const char ch : token) {
        if (ch == ' ' || ch == '#' || ch == ':') {
            escaped.push_back('\\');
            escaped.push_back(ch);
        } else if (ch == '$') {
            escaped += "$$";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

void write_depfile(
    const std::filesystem::path& depfile,
    const std::string& target,
    const std::vector<std::string>& dependencies
) {
    if (depfile.empty()) {
        return;
    }
    if (depfile.has_parent_path()) {
        std::filesystem::create_directories(depfile.parent_path());
    }

    std::ofstream out(depfile, std::ios::binary);
    if (!out) {
        throw std::runtime_error(
            "failed to open dependency file: " + depfile.string()
        );
    }

    out << escape_makefile_token(target) << ':';
    for (const auto& dependency : dependencies) {
        out << " \\\n  " << escape_makefile_token(dependency);
    }
    out << '\n';
}

[[nodiscard]] std::string depfile_target(const Options& options) {
    if (!options.dep_target.empty()) {
        return options.dep_target;
    }
    if (!options.output_file.empty()) {
        return options.output_file.generic_string();
    }
    return options.depfile.generic_string();
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    CLI::App app {"Generate C++ reflection metadata"};
    configure_options(app, options);
    CLI11_PARSE(app, argc, argv);

    try {
        normalize_options(options);

        if (options.validate_metadata) {
            ets::reflgen::validate_reflection_metadata(options.metadata_files);
            std::cout << "Validated " << options.metadata_files.size()
                      << " reflection metadata files.\n";
            return 0;
        }

        if (options.aggregate) {
            ets::reflgen::generate_aggregate_cpp_file(
                options.registrars,
                options.output_file
            );
            write_stamp_file(options.stamp_file);
            std::cout << "Generated C++ reflection aggregate: "
                      << options.output_file.generic_string() << '\n';
            return 0;
        }

        if (!options.libraries.empty()) {
            ets::reflgen::generate_luau_libraries(
                options.libraries,
                options.output_file,
                options.manifest_output,
                options.includes,
                options.function_name == "register_reflection" ?
                    "default_luau_libraries" :
                    options.function_name,
                options.library_part
            );
            return 0;
        }

        ets::reflgen::HeaderParser parser(
            options.headers,
            options.includes,
            options.verbose
        );
        auto output = parser.parse();
        auto& result = output.result;
        ets::reflgen::dedupe_reflected_types(result);
        ets::reflgen::filter_codegen_unsupported_members(result);
        ets::reflgen::write_reflection_metadata(
            result,
            options.metadata_output,
            options.script_module
        );
        ets::reflgen::write_reflection_manifest(
            result,
            options.root_dir,
            options.manifest_output,
            options.script_module
        );

        std::cout << "\nParsing complete! Found "
                  << result.annotation_schemas.size() << " annotation schemas, "
                  << result.classes.size() << " classes and "
                  << result.enums.size() << " enums in "
                  << options.headers.size() << " files.\n";

        if (result.annotation_schemas.empty() && result.classes.empty() &&
            result.enums.empty()) {
            std::cout << "No annotation schemas, classes or enums found in the "
                         "header files.\n";
        }

        if (!options.output_file.empty()) {
            ets::reflgen::generate_cpp_file(
                result,
                options.root_dir,
                options.output_file,
                options.function_name,
                options.script_module
            );
            write_stamp_file(options.stamp_file);
            write_depfile(
                options.depfile,
                depfile_target(options),
                project_dependencies(output.dependencies, options.root_dir)
            );
            std::cout << "Generated C++ reflection code: "
                      << options.output_file.generic_string() << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
