#include "emitter.hpp"
#include "manifest.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> manifests;
    std::filesystem::path manual_definitions;
    std::filesystem::path output_directory;
    std::filesystem::path runtime_directory;
    std::filesystem::path libraries_directory;

    CLI::App app {
        "Generate Luau definitions from Entisium reflection manifests"
    };
    app.add_option("--manifest", manifests, "Reflection manifest to consume")
        ->required()
        ->take_all();
    app.add_option(
           "--manual",
           manual_definitions,
           "Manual runtime definition file"
    )
        ->required();
    app.add_option(
           "--output",
           output_directory,
           "Generated definition directory"
    )
        ->required();
    app.add_option("--runtime", runtime_directory, "Runtime source directory")
        ->required();
    app.add_option(
           "--libraries",
           libraries_directory,
           "Generated Luau library manifest"
    )
        ->required();
    CLI11_PARSE(app, argc, argv);

    try {
        const auto database = ets::luau_defgen::load_manifests(manifests);
        const auto summary = ets::luau_defgen::emit_definitions(
            database,
            manual_definitions,
            output_directory,
            runtime_directory,
            libraries_directory
        );
        std::cout << "Generated Luau definitions for " << summary.class_count
                  << " classes, " << summary.enum_count << " enums and "
                  << summary.module_count << " native modules.\n";
        if (!summary.unsupported_cpp_types.empty()) {
            std::cerr << "Mapped " << summary.unsupported_cpp_types.size()
                      << " unsupported C++ reflection types to any:\n";
            for (const auto& type : summary.unsupported_cpp_types) {
                std::cerr << "  " << type << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "entisium-luau-defgen: " << error.what() << '\n';
        return 1;
    }
}
