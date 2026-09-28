#include "base/log.hpp"
#include "project/project.hpp"
#include "runtime_host/application.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

using namespace ets;

int main(int argc, char** argv) {
    // Runtime MCP reads these pipes while the game is paused between requests.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    runtime_host::RuntimeHostOptions options;
    const char* project_path = nullptr;
    bool config_stdin = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config-stdin") {
            config_stdin = true;
        } else if (argument == "--hidden") {
            options.hidden = true;
        } else if (argument.starts_with("--") || project_path != nullptr) {
            error("Unexpected argument: {}", argument);
            return 1;
        } else {
            project_path = argv[index];
        }
    }
    if (project_path == nullptr) {
        error("Usage: entisium-runtime-host [--hidden] <path-to-project.yaml>");
        return 1;
    }

    auto project = Project::load(project_path);
    if (!project) {
        error(
            "Failed to load project '{}': {}",
            project.error().path.string(),
            project.error().message
        );
        return 1;
    }

    try {
        if (config_stdin) {
            options.luau_config = read_luau_host_config(std::cin);
        }
        runtime_host::RuntimeHostApplication application(
            std::move(*project),
            options
        );
        application.run();
    } catch (const std::exception& exception) {
        error("Runtime Host failed: {}", exception.what());
        return 1;
    }
    return 0;
}
