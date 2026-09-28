#include "definition_index.hpp"
#include "entisium_platform.hpp"
#include "Flags.hpp"
#include "LSP/Client.hpp"
#include "LSP/LanguageServer.hpp"
#include "LSP/Transport/StdioTransport.hpp"
#include "Luau/Common.h"
#include "Luau/ExperimentalFlags.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#    include <fcntl.h>
// MSVC declares _setmode here, although include-cleaner does not attribute it.
#    include <io.h> // NOLINT(misc-include-cleaner)
#endif

LUAU_FASTFLAG(LuauSolverV2)

namespace {

constexpr std::string_view version = "0.1.0";

struct Options {
    bool show_version {false};
    std::optional<std::filesystem::path> definitions_index;
};

[[nodiscard]] Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument {argv[index]};
        if (argument == "--stdio") {
            continue;
        }
        if (argument == "--version") {
            options.show_version = true;
            continue;
        }
        if (argument == "--definitions-index" && index + 1 < argc) {
            options.definitions_index = argv[++index];
            continue;
        }
        throw std::invalid_argument(
            "unknown or incomplete argument '" + std::string {argument} + "'"
        );
    }
    return options;
}

[[nodiscard]] std::optional<std::filesystem::path>
default_definition_index(char* executable) {
    auto candidate = std::filesystem::absolute(executable).parent_path() /
                     "luau-definitions" / "entisium-runtime-host" /
                     "index.json";
    if (std::filesystem::is_regular_file(candidate)) {
        return candidate;
    }
    return std::nullopt;
}

class EntisiumClient final : public LSPClient {
  public:
    explicit EntisiumClient(std::unique_ptr<Transport> transport) :
        LSPClient(std::move(transport)) {}

    [[nodiscard]] std::unique_ptr<LSPPlatform> createPlatform(
        const ClientConfiguration&,
        WorkspaceFileResolver* file_resolver,
        WorkspaceFolder* workspace_folder
    ) override {
        return std::make_unique<ets::lsp::EntisiumPlatform>(
            file_resolver,
            workspace_folder
        );
    }
};

void enable_language_features() {
    for (auto* flag = Luau::FValue<bool>::list; flag != nullptr;
         flag = flag->next) {
        if (std::strncmp(flag->name, "Luau", 4) == 0 &&
            !Luau::isAnalysisFlagExperimental(flag->name)) {
            flag->value = true;
        }
    }
    FFlag::LuauSolverV2.value = true;
    applyRequiredFlags();
}

void configure_stdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "entisium-lsp: " << error.what() << '\n';
        std::cerr << "usage: entisium-lsp [--stdio] [--definitions-index PATH] "
                     "[--version]\n";
        return 2;
    }
    if (options.show_version) {
        std::cout << "entisium-lsp " << version << '\n';
        return 0;
    }

    Luau::assertHandler() = [](const char* expression,
                               const char* file,
                               int line,
                               const char*) -> int {
        std::cerr << file << '(' << line
                  << "): ASSERTION FAILED: " << expression << '\n';
        return 1;
    };

    configure_stdio();
    enable_language_features();

    auto transport = std::make_unique<StdioTransport>();
    EntisiumClient client {std::move(transport)};
    client.globalConfig.platform.type = LSPPlatformConfig::Standard;
    client.globalConfig.sourcemap.enabled = false;
    client.globalConfig.types.roblox = false;

    std::optional<Luau::Config> base_config;
    const auto definition_index = options.definitions_index ?
                                      options.definitions_index :
                                      default_definition_index(argv[0]);
    if (definition_index) {
        try {
            auto loaded = ets::lsp::load_definition_index(*definition_index);
            client.definitionsFiles = std::move(loaded.definition_files);
            base_config = std::move(loaded.base_config);
        } catch (const std::exception& error) {
            std::cerr << "entisium-lsp: " << error.what() << '\n';
            return 1;
        }
    }

    LanguageServer server {&client, std::move(base_config)};
    server.processInputLoop();
    return server.requestedShutdown() ? 0 : 1;
}
