#include "script_type_values.hpp"

#include "Flags.hpp"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Error.h"
#include "Luau/ExperimentalFlags.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Plugin/SourceMapping.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class MemoryFileResolver final : public Luau::FileResolver {
  public:
    std::unordered_map<Luau::ModuleName, std::string> sources;

    std::optional<Luau::SourceCode>
    readSource(const Luau::ModuleName& name) override {
        const auto found = sources.find(name);
        if (found == sources.end()) {
            return std::nullopt;
        }
        return Luau::SourceCode {
            .source = found->second,
            .type = Luau::SourceCode::Module,
        };
    }

    std::optional<Luau::ModuleInfo> resolveModule(
        const Luau::ModuleInfo* context,
        Luau::AstExpr* expression,
        const Luau::TypeCheckLimits&
    ) override {
        const auto* specifier = expression->as<Luau::AstExprConstantString>();
        if (specifier == nullptr) {
            return std::nullopt;
        }
        std::filesystem::path path {
            std::string {specifier->value.data, specifier->value.size}
        };
        if (context != nullptr && path.is_relative()) {
            path = std::filesystem::path {context->name}.parent_path() / path;
        }
        path = path.lexically_normal();
        if (!path.has_extension()) {
            path += ".luau";
        }
        return Luau::ModuleInfo {.name = path.generic_string()};
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
    applyRequiredFlags();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return {
        std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {},
    };
}

std::string transformed(std::string_view source) {
    const auto edits = ets::lsp::script_type_value_edits(source);
    return Luau::LanguageServer::Plugin::SourceMapping::fromEdits(
               std::string {source},
               edits
    )
        .transformedSource;
}

std::vector<std::string> check(std::string source) {
    enable_language_features();

    MemoryFileResolver files;
    files.sources.emplace("script-test", transformed(source));
    Luau::NullConfigResolver configs;
    configs.defaultConfig.mode = Luau::Mode::Strict;
    Luau::Frontend frontend {
        Luau::SolverMode::New,
        &files,
        &configs,
    };
    Luau::registerBuiltinGlobals(frontend, frontend.globals);

    const auto definitions = read_file(
        std::filesystem::path {ETS_PROJECT_ROOT} /
        "tools/luau_defgen/entisium-runtime.d.luau"
    );
    const auto loaded = frontend.loadDefinitionFile(
        frontend.globals,
        frontend.globals.globalScope,
        definitions,
        "@entisium",
        false
    );
    REQUIRE(loaded.success);
    const auto result = frontend.check("script-test");
    std::vector<std::string> errors;
    errors.reserve(result.errors.size());
    for (const auto& error : result.errors) {
        errors.push_back(Luau::toString(error));
    }
    return errors;
}

class ScriptWorkspace final {
  public:
    ScriptWorkspace() :
        frontend(
            (enable_language_features(), &files),
            &configs,
            Luau::FrontendOptions {.retainFullTypeGraphs = true}
        ) {
        enable_language_features();
        configs.defaultConfig.mode = Luau::Mode::Strict;
        Luau::registerBuiltinGlobals(frontend, frontend.globals);
        const auto definitions = read_file(
            std::filesystem::path {ETS_PROJECT_ROOT} /
            "tools/luau_defgen/entisium-runtime.d.luau"
        );
        const auto loaded = frontend.loadDefinitionFile(
            frontend.globals,
            frontend.globals.globalScope,
            definitions,
            "@entisium",
            false
        );
        REQUIRE(loaded.success);
    }

    void set_source(const Luau::ModuleName& name, std::string source) {
        files.sources.insert_or_assign(name, transformed(source));
    }

    std::vector<std::string> check(const Luau::ModuleName& name) {
        const auto result = frontend.check(name);
        std::vector<std::string> errors;
        errors.reserve(result.errors.size());
        for (const auto& error : result.errors) {
            errors.push_back(Luau::toString(error));
        }
        return errors;
    }

    MemoryFileResolver files;
    Luau::NullConfigResolver configs;
    Luau::Frontend frontend;
};

} // namespace

TEST_CASE(
    "script record aliases receive module-local runtime type values",
    "[lsp][script_type]"
) {
    constexpr std::string_view source = R"(
        export type Player = {
            speed: f32,
            target: entity?,
        }

        local empty = Player {}
        local direct = Player { speed = 2.0 }
        local created = Player.new { speed = 3.5, target = nil }
        local speed: number = created.speed
        local token: TypeToken<Player> = Player
        local descriptor = Read(Player)
    )";

    const auto edits = ets::lsp::script_type_value_edits(source);
    REQUIRE(edits.size() == 1);
    const auto generated = transformed(source);
    CHECK(
        generated.find("type __ets_lsp_Player_initializer") != std::string::npos
    );
    CHECK(generated.find("export local Player:") != std::string::npos);
    CHECK(generated.find("new: (() -> Player)") != std::string::npos);
    CHECK(check(std::string {source}).empty());
}

TEST_CASE(
    "script record constructors retain field type diagnostics",
    "[lsp][script_type]"
) {
    const auto errors = check(R"(
        export type Player = {
            speed: f32,
        }

        local wrong_type = Player.new { speed = "fast" }
    )");

    for (const auto& error : errors) {
        UNSCOPED_INFO(error);
    }
    CHECK(errors.size() == 1);
}

TEST_CASE(
    "exported string unions receive enum-like values",
    "[lsp][script_type][state]"
) {
    const auto generated =
        transformed(R"(export type GameFlow = "Boot" | "Running" | "Paused")");
    CHECK(generated.find("export local GameFlow:") != std::string::npos);

    const auto errors = check(R"(
        export type GameFlow = "Boot" | "Running" | "Paused"

        local boot: GameFlow = GameFlow.Boot
        local running: GameFlow = GameFlow.Running
    )");
    CHECK(errors.empty());

    const auto invalid = check(R"(
        export type GameFlow = "Boot" | "Running"
        local missing = GameFlow.Missing
    )");
    CHECK(invalid.size() == 1);
}

TEST_CASE(
    "required script modules expose runtime type values",
    "[lsp][script_type][module]"
) {
    ScriptWorkspace workspace;
    workspace.set_source("scripts/player.luau", R"(
        export type Player = {
            speed: f32,
        }
        export type Phase = "Boot" | "Running"
        export local PlayerPlugin = {}
    )");
    workspace.set_source("scripts/game.luau", R"(
        local PlayerModule = require("./player")

        local player: PlayerModule.Player =
            PlayerModule.Player.new { speed = 4 }
        local token: TypeToken<PlayerModule.Player> = PlayerModule.Player
        local phase: PlayerModule.Phase = PlayerModule.Phase.Boot
        local plugin = PlayerModule.PlayerPlugin
    )");

    const auto errors = workspace.check("scripts/game.luau");
    for (const auto& error : errors) {
        UNSCOPED_INFO(error);
    }
    CHECK(errors.empty());
}

TEST_CASE(
    "required script module type values retain exact member shapes",
    "[lsp][script_type][module]"
) {
    ScriptWorkspace workspace;
    workspace.set_source("scripts/player.luau", R"(
        export type Player = { speed: f32 }
        export type Phase = "Boot" | "Running"
    )");
    workspace.set_source("scripts/game.luau", R"(
        local PlayerModule = require("./player")
        local missing_type = PlayerModule.Missing
        local missing_constructor = PlayerModule.Player.missing
        local missing_state = PlayerModule.Phase.Missing
    )");

    const auto errors = workspace.check("scripts/game.luau");
    for (const auto& error : errors) {
        UNSCOPED_INFO(error);
    }
    CHECK(errors.size() == 3);
}

TEST_CASE(
    "script module dependents refresh when exported types change",
    "[lsp][script_type][module][invalidation]"
) {
    ScriptWorkspace workspace;
    workspace.set_source(
        "scripts/player.luau",
        "export type Phase = \"Boot\" | \"Running\"\n"
    );
    workspace.set_source("scripts/game.luau", R"(
        local PlayerModule = require("./player")
        local phase: PlayerModule.Phase = PlayerModule.Phase.Boot
    )");
    REQUIRE(workspace.check("scripts/game.luau").empty());

    workspace.set_source(
        "scripts/player.luau",
        "export type Phase = \"Stopped\"\n"
    );
    std::vector<Luau::ModuleName> invalidated;
    workspace.frontend.markDirty("scripts/player.luau", &invalidated);
    CHECK(
        std::ranges::find(invalidated, "scripts/game.luau") != invalidated.end()
    );
    CHECK_FALSE(workspace.check("scripts/game.luau").empty());
}

TEST_CASE(
    "ordinary aliases and non-exported aliases do not create values",
    "[lsp][script_type][scope]"
) {
    CHECK(
        ets::lsp::script_type_value_edits("type Local = { value: number }")
            .empty()
    );
    CHECK(
        ets::lsp::script_type_value_edits("export type Value = number").empty()
    );
    CHECK(
        ets::lsp::script_type_value_edits(
            "export type Nested = { values: {number} }"
        )
            .empty()
    );
    CHECK(
        ets::lsp::script_type_value_edits(
            "export type Result<T> = { value: T }"
        )
            .empty()
    );
}

TEST_CASE(
    "script type edits preserve original source positions",
    "[lsp][script_type][source_mapping]"
) {
    const std::string source =
        "export type Player = { speed: f32 }\nlocal value = Player.new {}\n";
    const auto result = Luau::LanguageServer::Plugin::SourceMapping::fromEdits(
        source,
        ets::lsp::script_type_value_edits(source)
    );
    Luau::LanguageServer::Plugin::SourceMapping mapping {result.edits};
    const Luau::Position original {1, 14};
    const auto mapped = mapping.originalToTransformed(original);
    REQUIRE(mapped.has_value());
    const Luau::Position missing {
        std::numeric_limits<unsigned int>::max(),
        std::numeric_limits<unsigned int>::max(),
    };
    const auto round_trip =
        mapping.transformedToOriginal(mapped.value_or(missing));
    REQUIRE(round_trip.has_value());
    const auto round_trip_position = round_trip.value_or(missing);
    CHECK(round_trip_position.line == original.line);
    CHECK(round_trip_position.column == original.column);
}

TEST_CASE(
    "scripting sample resolves all script-defined type values",
    "[lsp][script_type][sample]"
) {
    const auto source = read_file(
        std::filesystem::path {ETS_PROJECT_ROOT} /
        "samples/projects/scripting/assets/scripts/movement.luau"
    );
    const auto errors = check(source);
    for (const auto& error : errors) {
        UNSCOPED_INFO(error);
    }
    for (const auto& name :
         {"Player", "Collectible", "GameState", "PlaytestInput"}) {
        CHECK(std::ranges::none_of(errors, [&](const std::string& error) {
            return error.find("'" + std::string {name} + "'") !=
                   std::string::npos;
        }));
    }
}
