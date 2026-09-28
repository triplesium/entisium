#include "refl/generated.hpp"
#include "scripting/module_loader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (text.size() > 1024ULL * 1024) {
        throw std::runtime_error("Module exceeds 1 MiB");
    }
    return text;
}
bool inside(
    const std::filesystem::path& root,
    const std::filesystem::path& path
) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
}
} // namespace

int main(int argc, char** argv) {
    std::unique_ptr<ets::LuauRuntime> owned_runtime;
    try {
        ets::register_generated_reflection();
        // Keep accepting the old SDK-directory argument for existing callers.
        // All library sources now come from the linked host catalog.
        const bool configured =
            argc > 3 && std::string_view(argv[argc - 1]) == "--config-stdin";
        const int positional = argc - (configured ? 1 : 0);
        if (positional != 3 && positional != 4) {
            throw std::runtime_error(
                "Usage: entisium-luau-host <source-root> <entry.luau> "
                "[legacy-sdk-directory] [--config-stdin]"
            );
        }
        const auto root = std::filesystem::canonical(argv[1]);
        const auto entry = std::filesystem::canonical(argv[2]);
        if (!inside(root, entry)) {
            throw std::runtime_error("Test entry is outside the source root");
        }
        const auto entry_name =
            "project://" + entry.lexically_relative(root).generic_string();
        owned_runtime = std::make_unique<ets::LuauRuntime>(
            configured ? ets::read_luau_host_config(std::cin) : nullptr
        );
        auto& runtime = *owned_runtime;
        runtime.enable_host_calls();
        ets::LuauModuleLoader loader(
            runtime,
            [&](std::string_view importer, std::string_view specifier)
                -> ets::Result<ets::LuauScriptSource, ets::LuauScriptError> {
                try {
                    if (specifier == "@launcher") {
                        return ets::LuauScriptSource {
                            .name = "launcher://main.luau",
                            .content = "local scheduler = "
                                       "require(\"@playtest/"
                                       "scheduler\")\nlocal suite = "
                                       "require(" +
                                       Json(entry_name).dump() +
                                       ")\nreturn function() "
                                       "scheduler.run(suite.run) end",
                        };
                    }
                    std::filesystem::path relative;
                    if (specifier.starts_with("project://")) {
                        relative = std::string(specifier.substr(10));
                    } else {
                        if (!importer.starts_with("project://") ||
                            !specifier.starts_with(".")) {
                            throw std::runtime_error(
                                "Unsupported module: " + std::string(specifier)
                            );
                        }
                        relative = std::filesystem::path(
                                       std::string(importer.substr(10))
                                   )
                                       .parent_path() /
                                   std::string(specifier);
                    }
                    if (relative.extension().empty()) {
                        relative += ".luau";
                    }
                    const auto path =
                        std::filesystem::canonical(root / relative);
                    if (!inside(root, path) || path.extension() != ".luau") {
                        throw std::runtime_error(
                            "Module escapes source root or is not Luau"
                        );
                    }
                    return ets::LuauScriptSource {
                        .name = "project://" +
                                path.lexically_relative(root).generic_string(),
                        .content = read_file(path)
                    };
                } catch (const std::exception& error) {
                    return ets::failure(ets::LuauScriptError {error.what()});
                }
            }
        );
        auto module = loader.load({}, "@launcher");
        if (!module) {
            throw std::runtime_error(module.error().message);
        }
        auto started = runtime.start_task(*module);
        if (!started) {
            throw std::runtime_error(started.error().message);
        }
        std::string reply;
        while (true) {
            auto event = runtime.resume_task(reply);
            if (!event) {
                throw std::runtime_error(event.error().message);
            }
            auto message = Json::parse(*event);
            if (message.at("kind") == "completed") {
                const auto tape = runtime.library_diagnostic("@http", "tape");
                if (!tape.empty()) {
                    message["tape"] = Json::parse(tape);
                    if (message["tape"]["mode"] == "replay" &&
                        message["tape"]["consumed"] !=
                            message["tape"]["entries"].size()) {
                        throw std::runtime_error(
                            "HTTP replay has unused requests"
                        );
                    }
                }
                std::cout << "ETS_LUAU:" << message.dump() << '\n';
                return 0;
            }
            std::cout << "ETS_LUAU:" << *event << '\n';
            if (!std::getline(std::cin, reply)) {
                throw std::runtime_error("Host disconnected");
            }
            if (reply.size() > 32ULL * 1024 * 1024) {
                throw std::runtime_error("Host reply exceeds 32 MiB");
            }
        }
    } catch (const std::exception& error) {
        Json message {{"kind", "failed"}, {"error", error.what()}};
        if (owned_runtime) {
            const auto tape =
                owned_runtime->library_diagnostic("@http", "tape");
            if (!tape.empty()) {
                message["tape"] = Json::parse(tape);
            }
        }
        std::cout << "ETS_LUAU:" << message.dump() << '\n';
        return 1;
    }
}
