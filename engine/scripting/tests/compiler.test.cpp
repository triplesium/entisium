#include "scripting/compiler.hpp"

#include "asset/handle.hpp"
#include "compiler/compilation_session.hpp"
#include "compiler/module_ir.hpp"
#include "compiler/pass.hpp"
#include "ecs/dynamic/system_decl.hpp"
#include "refl/annotations.hpp"
#include "refl/cls.hpp"
#include "refl/generated.hpp"
#include "refl/registry.hpp"
#include "scripting/detail/plugin_install.hpp"
#include "scripting/library.hpp"
#include "scripting/runtime.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace ets::test {

namespace {

struct ChunkLoweringVector {
    float x {0.0F};
};

struct ChunkLoweringComponent {
    ChunkLoweringVector position;
    float x {0.0F};
};

struct NativePlaytestComponent {
    float value {0.0F};
};

Result<std::string, LuauScriptError> lower_chunk_query_source(
    const LuauScriptSource& source,
    LuauOptimizationPasses optimization_passes = {},
    std::vector<LuauOptimizationPassReport>* report = nullptr
) {
    Registry::instance().register_cls<ChunkLoweringVector>().add_property(
        "x",
        &ChunkLoweringVector::x
    );
    Registry::instance()
        .register_cls<ChunkLoweringComponent>()
        .add_property("position", &ChunkLoweringComponent::position)
        .add_property("x", &ChunkLoweringComponent::x);
    detail::luau_compiler::CompilationSession session {source};
    auto parsed = session.parse();
    if (!parsed) {
        return failure(std::move(parsed.error()));
    }

    auto query = std::make_unique<DynamicQueryParamDecl>();
    query->name = "query";
    query->fields.push_back(
        DynamicQueryFieldDecl {
            .name = "value",
            .type =
                DynamicTypeRef {.type_id = type_id<ChunkLoweringComponent>()},
            .access = DynamicParamAccess::Write,
        }
    );
    std::vector<LuauFunctionDecl> functions;
    functions.push_back(LuauFunctionDecl {.name = "run"});
    functions.back().params.push_back(std::move(query));

    auto lowered = detail::luau_compiler::PropertyLoweringPass {}
                       .run(parsed->root(), functions, optimization_passes);
    if (report != nullptr) {
        *report = lowered.optimization_report;
    }
    return lowered.source_patches.apply(source);
}

LuauOptimizationPasses only(LuauOptimizationPass pass) {
    auto result = LuauOptimizationPasses::none();
    result.set(pass);
    return result;
}

const LuauOptimizationPassReport& report_for(
    const std::vector<LuauOptimizationPassReport>& reports,
    LuauOptimizationPass pass
) {
    const auto found =
        std::ranges::find(reports, pass, &LuauOptimizationPassReport::pass);
    REQUIRE(found != reports.end());
    return *found;
}

const LuauOptimizationPassDescriptor&
descriptor_for(LuauOptimizationPass pass) {
    const auto descriptors = luau_optimization_pass_descriptors();
    const auto found = std::ranges::find(
        descriptors,
        pass,
        &LuauOptimizationPassDescriptor::pass
    );
    REQUIRE(found != descriptors.end());
    return *found;
}

} // namespace

TEST_CASE(
    "Luau compilation session owns one reusable parsed module",
    "[scripting_luau][compiler][session]"
) {
    const LuauScriptSource source {
        .name = "session.luau",
        .content = "local value = 1",
    };
    detail::luau_compiler::CompilationSession session {source};

    auto first = session.parse();
    REQUIRE(first);
    auto second = session.parse();
    REQUIRE(second);
    CHECK(&first->root() == &second->root());
    CHECK(&first->source() == &source);

    const LuauScriptSource invalid {
        .name = "invalid_session.luau",
        .content = "local =",
    };
    detail::luau_compiler::CompilationSession invalid_session {invalid};
    auto first_error = invalid_session.parse();
    REQUIRE_FALSE(first_error);
    auto second_error = invalid_session.parse();
    REQUIRE_FALSE(second_error);
    CHECK(first_error.error().message == second_error.error().message);
    CHECK(first_error.error().message.starts_with("invalid_session.luau:1:"));

    auto declaration_error = compile_luau_script_module(invalid);
    REQUIRE_FALSE(declaration_error);
    CHECK(
        declaration_error.error().message ==
        "Invalid Luau script declaration: " + first_error.error().message
    );
}

TEST_CASE(
    "Luau module frontend builds one semantic module IR",
    "[scripting_luau][compiler][frontend][ir]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/frontend_ir.luau",
        .content = R"(
            local Common = require("./common")

            export type Position = {
                x: f32,
                y: f32,
            }

            export type FrontendFlow = "Idle" | "Running"
        )",
    };
    detail::luau_compiler::CompilationSession session {source};
    auto parsed = session.parse();
    REQUIRE(parsed);

    auto module_ir = detail::luau_compiler::ModuleFrontendPass {}.run(*parsed);
    REQUIRE(module_ir);
    CHECK(module_ir->name() == "project.scripts.frontend_ir");
    CHECK(module_ir->source_name() == source.name);
    REQUIRE(module_ir->imports().size() == 1);
    CHECK(module_ir->imports().begin()->second == "./common");
    REQUIRE(module_ir->types().size() == 2);
    CHECK(module_ir->types()[0].name == "FrontendFlow");
    CHECK(
        std::holds_alternative<detail::luau_compiler::StringUnionTypeIR>(
            module_ir->types()[0].value
        )
    );
    CHECK(module_ir->types()[1].name == "Position");
    CHECK(
        std::holds_alternative<detail::luau_compiler::RecordTypeIR>(
            module_ir->types()[1].value
        )
    );

    auto schema =
        detail::luau_compiler::ModuleSchemaLoweringPass {}.run(*module_ir);
    REQUIRE(schema);
    CHECK(schema->name == "project.scripts.frontend_ir");
    CHECK(schema->source_name == source.name);
    REQUIRE(schema->types.size() == 1);
    REQUIRE(schema->enums.size() == 1);
    CHECK(schema->enums.front().name == "FrontendFlow");
}

TEST_CASE(
    "Luau compiler generates reflection metadata per source module",
    "[scripting_luau][compiler][metadata]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/metadata.luau",
        .content = R"(
            local Common = require("./common")

            export type Position = {
                x: f32,
            }

            export function tick(positions: Query<Read<Position>>)
            end

            export function add(value: number): number
                return value + 1
            end

            export local MetadataPlugin = Plugin.new {
                dependencies = { Common.CommonPlugin },
                build = function(app: App)
                    app:add_system(Update, tick)
                end,
            }
        )",
    };

    auto metadata = compile_luau_module_metadata(source);
    REQUIRE(metadata);
    CHECK(metadata->source_hash != 0);
    CHECK(metadata->schema.name == "project.scripts.metadata");
    REQUIRE(metadata->schema.types.size() == 1);
    CHECK(
        metadata->schema.types.front().qualified_name ==
        "project.scripts.metadata.Position"
    );
    REQUIRE(metadata->imports.size() == 1);
    CHECK(metadata->imports.front() == "./common");

    const auto* tick = metadata->find_function("tick");
    REQUIRE(tick != nullptr);
    CHECK(tick->qualified_name == "project.scripts.metadata.tick");
    REQUIRE(tick->is_system_compatible());
    REQUIRE(tick->system_params.size() == 1);
    const auto& query =
        static_cast<const DynamicQueryParamDecl&>(*tick->system_params.front());
    REQUIRE(query.fields.size() == 1);
    CHECK(
        query.fields.front().type.type_name ==
        "project.scripts.metadata.Position"
    );

    const auto* add = metadata->find_function("add");
    REQUIRE(add != nullptr);
    CHECK_FALSE(add->is_system_compatible());
    CHECK_FALSE(add->system_signature_error.empty());

    const auto* plugin = metadata->find_plugin("MetadataPlugin");
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->dependencies.size() == 1);
    CHECK(plugin->dependencies.front().import_specifier == "./common");
    CHECK(plugin->dependencies.front().plugin_name == "CommonPlugin");
}

TEST_CASE(
    "Luau compiler source patches reject overlapping passes",
    "[scripting_luau][compiler][pass][source_patch]"
) {
    const auto location = [](unsigned int begin, unsigned int end) {
        Luau::Location result;
        result.begin = Luau::Position {0, begin};
        result.end = Luau::Position {0, end};
        return result;
    };
    const LuauScriptSource source {.name = "patch.luau", .content = "abcdef"};

    detail::luau_compiler::SourcePatchSet valid;
    valid.add(location(0, 1), "A", "first");
    valid.add(location(5, 6), "F", "second");
    auto applied = valid.apply(source);
    REQUIRE(applied);
    CHECK(*applied == "AbcdeF");

    detail::luau_compiler::SourcePatchSet overlapping;
    overlapping.add(location(1, 4), "left", "analysis");
    overlapping.add(location(3, 5), "right", "lowering");
    auto rejected = overlapping.apply(source);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("overlaps") != std::string::npos);
    CHECK(rejected.error().message.find("analysis") != std::string::npos);
    CHECK(rejected.error().message.find("lowering") != std::string::npos);
}

TEST_CASE(
    "Luau compiler transparently lowers reusable query loops to chunks",
    "[scripting_luau][compiler][query][chunk]"
) {
    const LuauScriptSource source {
        .name = "chunk_query.luau",
        .content = R"(
            local function run(query)
                for value in query do
                    value.x += 1
                    continue
                end
            end
        )",
    };
    auto lowered = lower_chunk_query_source(source);
    REQUIRE(lowered);
    CHECK(lowered->find("__ets_chunk_query(query, 1)") != std::string::npos);
    CHECK(lowered->find("while true do") != std::string::npos);
    CHECK(lowered->find("continue") != std::string::npos);
    CHECK(lowered->find("for value in query do") == std::string::npos);
}

TEST_CASE(
    "Luau optimization pipelines preserve independent pass selection",
    "[scripting_luau][compiler][pass][pipeline]"
) {
    const auto defaults =
        luau_optimization_pipeline_passes(LuauOptimizationPipeline::Default);
    CHECK(defaults == LuauOptimizationPasses::all());
    CHECK(
        luau_optimization_pipeline_passes(LuauOptimizationPipeline::None) ==
        LuauOptimizationPasses::none()
    );

    const auto property =
        luau_optimization_pipeline_passes(LuauOptimizationPipeline::Property);
    const auto property_order =
        luau_optimization_pipeline_order(LuauOptimizationPipeline::Property);
    REQUIRE(property_order.size() == 2);
    CHECK(property_order[0] == LuauOptimizationPass::ElidePropertyAliases);
    CHECK(property_order[1] == LuauOptimizationPass::FlattenPropertyPaths);
    CHECK(property.contains(LuauOptimizationPass::FlattenPropertyPaths));
    CHECK(property.contains(LuauOptimizationPass::ElidePropertyAliases));
    CHECK_FALSE(property.contains(LuauOptimizationPass::ReuseQueryUserdata));
    CHECK_FALSE(property.contains(LuauOptimizationPass::ChunkQueryIteration));

    const auto query =
        luau_optimization_pipeline_passes(LuauOptimizationPipeline::Query);
    CHECK_FALSE(query.contains(LuauOptimizationPass::FlattenPropertyPaths));
    CHECK_FALSE(query.contains(LuauOptimizationPass::ElidePropertyAliases));
    CHECK(query.contains(LuauOptimizationPass::ReuseQueryUserdata));
    CHECK(query.contains(LuauOptimizationPass::ChunkQueryIteration));
}

TEST_CASE(
    "Luau optimization metadata diagnoses profitability dependencies",
    "[scripting_luau][compiler][pass][dependency]"
) {
    const auto& alias =
        descriptor_for(LuauOptimizationPass::ElidePropertyAliases);
    CHECK(alias.required_passes == LuauOptimizationPasses::none());
    CHECK(
        alias.benefits_from.contains(LuauOptimizationPass::FlattenPropertyPaths)
    );

    const auto& chunk =
        descriptor_for(LuauOptimizationPass::ChunkQueryIteration);
    CHECK(chunk.required_passes == LuauOptimizationPasses::none());
    CHECK(
        chunk.benefits_from.contains(LuauOptimizationPass::ReuseQueryUserdata)
    );

    auto diagnostics = diagnose_luau_optimization_passes(
        only(LuauOptimizationPass::ElidePropertyAliases)
    );
    REQUIRE(diagnostics.size() == 1);
    CHECK(
        diagnostics.front().pass == LuauOptimizationPass::ElidePropertyAliases
    );
    CHECK(
        diagnostics.front().dependency ==
        LuauOptimizationPass::FlattenPropertyPaths
    );
    CHECK(
        diagnostics.front().kind == LuauOptimizationDependencyKind::BenefitsFrom
    );

    CHECK(
        diagnose_luau_optimization_passes(luau_optimization_pipeline_passes(
                                              LuauOptimizationPipeline::Property
                                          ))
            .empty()
    );
    CHECK(diagnose_luau_optimization_passes(
              luau_optimization_pipeline_passes(LuauOptimizationPipeline::Query)
    )
              .empty());
}

TEST_CASE(
    "Luau optimization passes can be enabled independently",
    "[scripting_luau][compiler][pass][options]"
) {
    const LuauScriptSource source {
        .name = "independent_query_passes.luau",
        .content = R"(
            local function run(query)
                for value in query do
                    value.x += 1
                end
            end
        )",
    };

    auto none =
        lower_chunk_query_source(source, LuauOptimizationPasses::none());
    REQUIRE(none);
    CHECK(none->find("for value in query do") != std::string::npos);
    CHECK(none->find("__ets_p") == std::string::npos);
    CHECK(none->find("__ets_reuse_query") == std::string::npos);
    CHECK(none->find("__ets_chunk_query") == std::string::npos);

    auto flattened = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::FlattenPropertyPaths)
    );
    REQUIRE(flattened);
    CHECK(flattened->find("value.__ets_p") != std::string::npos);
    CHECK(flattened->find("for value in query do") != std::string::npos);

    auto reused = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::ReuseQueryUserdata)
    );
    REQUIRE(reused);
    CHECK(
        reused->find("for value in __ets_reuse_query(query, 1) do") !=
        std::string::npos
    );
    CHECK(reused->find("value.x += 1") != std::string::npos);

    auto chunked = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::ChunkQueryIteration)
    );
    REQUIRE(chunked);
    CHECK(chunked->find("__ets_chunk_query(query, 0)") != std::string::npos);
    CHECK(chunked->find("value.x += 1") != std::string::npos);
}

TEST_CASE(
    "Luau optimization reports distinguish candidates from applications",
    "[scripting_luau][compiler][pass][report]"
) {
    const LuauScriptSource source {
        .name = "query_pass_report.luau",
        .content = R"(
            local function run(query)
                for value in query do
                    value.x += 1
                end
            end
        )",
    };
    std::vector<LuauOptimizationPassReport> reports;
    auto lowered = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::ChunkQueryIteration),
        &reports
    );
    REQUIRE(lowered);
    REQUIRE(
        reports.size() == static_cast<std::size_t>(LuauOptimizationPass::Count)
    );

    const auto& flatten =
        report_for(reports, LuauOptimizationPass::FlattenPropertyPaths);
    CHECK_FALSE(flatten.enabled);
    CHECK(flatten.candidates == 1);
    CHECK(flatten.applied == 0);

    const auto& reuse =
        report_for(reports, LuauOptimizationPass::ReuseQueryUserdata);
    CHECK_FALSE(reuse.enabled);
    CHECK(reuse.candidates == 1);
    CHECK(reuse.applied == 0);

    const auto& chunk =
        report_for(reports, LuauOptimizationPass::ChunkQueryIteration);
    CHECK(chunk.enabled);
    CHECK(chunk.candidates == 1);
    CHECK(chunk.applied == 1);
}

TEST_CASE(
    "Luau property alias elision is independent from path flattening",
    "[scripting_luau][compiler][pass][property][alias]"
) {
    const LuauScriptSource source {
        .name = "independent_property_passes.luau",
        .content = R"(
            local function run(query)
                local total = 0
                for value in query do
                    local position = value.position
                    total += position.x
                end
            end
        )",
    };

    auto aliases = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::ElidePropertyAliases)
    );
    REQUIRE(aliases);
    CHECK(aliases->find("local position = value") != std::string::npos);
    CHECK(aliases->find("position.position.x") != std::string::npos);
    CHECK(aliases->find("__ets_p") == std::string::npos);

    auto paths = lower_chunk_query_source(
        source,
        only(LuauOptimizationPass::FlattenPropertyPaths)
    );
    REQUIRE(paths);
    CHECK(paths->find("local position = value.position") != std::string::npos);
    CHECK(paths->find("position.__ets_p") != std::string::npos);
}

TEST_CASE(
    "Luau compiler keeps query loops with break on the row iterator",
    "[scripting_luau][compiler][query][chunk][fallback]"
) {
    const LuauScriptSource source {
        .name = "break_query.luau",
        .content = R"(
            local function run(query)
                for value in query do
                    if value.x > 0 then
                        break
                    end
                end
            end
        )",
    };
    auto lowered = lower_chunk_query_source(source);
    REQUIRE(lowered);
    CHECK(
        lowered->find("for value in __ets_reuse_query(query, 1) do") !=
        std::string::npos
    );
    CHECK(lowered->find("__ets_chunk_query") == std::string::npos);
}

TEST_CASE(
    "Luau compiler keeps property-heavy query loops on the row iterator",
    "[scripting_luau][compiler][query][chunk][cost]"
) {
    const LuauScriptSource source {
        .name = "heavy_query.luau",
        .content = R"(
            local function run(query)
                local total = 0
                for value in query do
                    total += value.x
                    total += value.x
                    total += value.x
                    total += value.x
                end
            end
        )",
    };
    auto lowered = lower_chunk_query_source(source);
    REQUIRE(lowered);
    CHECK(
        lowered->find("for value in __ets_reuse_query(query, 1) do") !=
        std::string::npos
    );
    CHECK(lowered->find("__ets_chunk_query") == std::string::npos);
}

TEST_CASE(
    "Luau compiler builds exported plugin declarations",
    "[scripting_luau][compiler][plugin][export]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/player.luau",
        .content = R"(
            export type Player = {
                health: i32,
                speed: f32,
            }

            local function move(players: Query<Write<Player>>)
            end

            export local PlayerPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, move)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK(artifact->plugins.front().name == "PlayerPlugin");
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    CHECK(
        artifact->metadata->schema.types.front().qualified_name ==
        "project.scripts.player.Player"
    );
    REQUIRE(artifact->plugins.front().functions.size() == 1);
    CHECK(artifact->plugins.front().functions.front().name == "move");
}

TEST_CASE(
    "Luau exported types resolve imported type namespaces",
    "[scripting_luau][compiler][type][import]"
) {
    auto artifact = compile_luau_script_module(
        LuauScriptSource {
            .name = "project://scripts/game.luau",
            .content = R"(
                local Player = require("./player")

                export type Selection = {
                    player: Player.Player,
                }

                export local GamePlugin = Plugin.new {
                    dependencies = { Player.PlayerPlugin },
                    build = function(app: App)
                    end,
                }
            )",
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(
        artifact->plugin_dependencies(artifact->plugins.front().name).size() ==
        1
    );
    CHECK(
        artifact->plugin_dependencies(artifact->plugins.front().name)[0]
            .import_specifier == "./player"
    );
    CHECK(
        artifact->plugin_dependencies(artifact->plugins.front().name)[0]
            .plugin_name == "PlayerPlugin"
    );
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    REQUIRE(artifact->metadata->schema.types[0].fields.size() == 1);
    CHECK(
        artifact->metadata->schema.types[0].fields[0].type.type_name ==
        "project.scripts.player.Player"
    );
    CHECK(artifact->metadata->schema.types[0].fields[0].type.script_type);
}

TEST_CASE(
    "Luau type qualification recognizes catalog libraries without a name list",
    "[scripting_luau][compiler][type][import]"
) {
    for (const auto* specifier :
         {"@fixture", "@internal/fixture", "@context/bridge"}) {
        INFO(specifier);
        REQUIRE(is_luau_library(specifier));
        auto metadata = compile_luau_module_metadata({
            .name = "project://scripts/catalog_types.luau",
            .content =
                std::string("local Library = require(\"") + specifier + R"(")
                local Player = require("./player")
                export type Selection = {
                    library: Library.Payload,
                    player: Player.Player,
                }
            )",
        });
        if (!metadata) {
            FAIL(metadata.error().message);
        }
        REQUIRE(metadata->schema.types.size() == 1);
        const auto& fields = metadata->schema.types.front().fields;
        REQUIRE(fields.size() == 2);
        CHECK(fields[0].type.type_name == "Library::Payload");
        CHECK_FALSE(fields[0].type.script_type);
        CHECK(fields[1].type.type_name == "project.scripts.player.Player");
        CHECK(fields[1].type.script_type);
    }
}

TEST_CASE(
    "Luau compiler adds multiple systems from exported Plugins",
    "[scripting_luau][compiler][plugin][system]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/systems.luau",
        .content = R"(
            local function enabled(): boolean
                return true
            end

            local function first()
            end

            local function second()
            end

            local function third()
            end

            local function last()
            end

            export local SystemsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(
                        Update,
                        first,
                        second:after(first):run_if(enabled),
                        chain(third, last)
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.front().functions.size() == 5);
}

TEST_CASE(
    "Luau compiler initializes exported state types from Plugins",
    "[scripting_luau][compiler][plugin][state]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/state.luau",
        .content = R"(
            export type GameFlow = "Boot" | "Running" | "Paused"

            local function update(state: State<GameFlow>)
                assert(state:get() == GameFlow.Boot)
            end

            local function enter_running()
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameFlow.Boot)
                    app:add_system(
                        Update,
                        update:run_if(in_state(GameFlow.Boot))
                    )
                    app:add_system(
                        OnEnter(GameFlow.Running),
                        enter_running
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.front().states.size() == 1);
    const auto& state = artifact->plugins.front().states[0];
    CHECK(state.name == "GameFlow");
    REQUIRE(state.values.size() == 3);
    CHECK(state.values[0].name == "Boot");
    CHECK(state.values[1].name == "Running");
    CHECK(state.values[2].name == "Paused");
    REQUIRE(artifact->plugins.front().functions.size() == 2);
    CHECK(
        artifact->plugins.front().functions[0].params[0]->decl_type_id() ==
        type_id<DynamicStateParamDecl>()
    );
}

TEST_CASE(
    "Luau compiler keeps unused string unions as ordinary types",
    "[scripting_luau][compiler][type][string_union]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/direction_type.luau",
        .content = R"(
            export type Direction = "Left" | "Right"

            export type Motion = {
                direction: Direction,
            }

            export local DirectionPlugin = Plugin.new {
                build = function(app: App)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->metadata->schema.enums.size() == 1);
    CHECK(artifact->metadata->schema.enums.front().name == "Direction");
    CHECK(artifact->plugins.front().states.empty());
    auto bindings = ensure_luau_types(artifact->metadata->schema);
    REQUIRE(bindings);
    CHECK(bindings->size() == 2);
}

TEST_CASE(
    "Luau compiler leaves build-only state initialization to runtime",
    "[scripting_luau][compiler][plugin][state][usage]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/usage_state.luau",
        .content = R"(
            export type Flow = "Idle" | "Running"

            export local UsagePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(Flow.Idle)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->metadata->schema.enums.size() == 1);
    CHECK(artifact->plugins.front().states.empty());
}

TEST_CASE(
    "Luau compiler validates types used through State",
    "[scripting_luau][compiler][plugin][state][usage]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/invalid_state_type.luau",
        .content = R"(
            export type Position = {
                x: f32,
            }

            local function update(state: State<Position>)
            end

            export local InvalidStatePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, update)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE_FALSE(artifact);
    CHECK(
        artifact.error().message.find(
            "used as State<T> must be a string literal union"
        ) != std::string::npos
    );
}

TEST_CASE(
    "Luau compiler records used state types without requiring static init",
    "[scripting_luau][compiler][plugin][state][usage]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/uninitialized_used_state.luau",
        .content = R"(
            export type Flow = "Idle" | "Running"

            local function update(state: State<Flow>)
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, update)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    REQUIRE(artifact->plugins.front().states.size() == 1);
}

TEST_CASE(
    "Luau compiler defers dynamic event registration to Plugin build",
    "[scripting_luau][compiler][plugin][event]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/events.luau",
        .content = R"(
            export type DamageEvent = {
                amount: i32,
            }

            local function send_damage(
                events: EventWriter<DamageEvent>
            )
                events:send(DamageEvent.new { amount = 3 })
            end

            local function read_damage(
                events: EventReaderRO<DamageEvent>?
            )
            end

            export local EventsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_event(DamageEvent)
                    app:add_systems(Update, send_damage, read_damage)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.front().functions.size() == 2);

    LuauScriptSource undeclared = source;
    const auto declaration = undeclared.content.find(
        "                    app:add_event(DamageEvent)\n"
    );
    REQUIRE(declaration != std::string::npos);
    undeclared.content.erase(
        declaration,
        std::string_view {"                    app:add_event(DamageEvent)\n"}
            .size()
    );
    auto invalid = compile_luau_script_module(undeclared);
    REQUIRE(invalid);
}

TEST_CASE(
    "Luau compiler resolves imported exported system functions",
    "[scripting_luau][compiler][system][import]"
) {
    const LuauScriptSource movement {
        .name = "project://scripts/movement.luau",
        .content = R"(
            export function move(players: Query<Write<Player>>)
            end
        )",
    };
    const LuauScriptSource game {
        .name = "project://scripts/game.luau",
        .content = R"(
            local Movement = require("./movement")

            export type Player = {
                speed: f32,
            }

            export local GamePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, Movement.move)
                end,
            }
        )",
    };
    auto movement_metadata = compile_luau_module_metadata(movement);
    REQUIRE(movement_metadata);
    auto shared_movement_metadata = std::make_shared<const LuauModuleMetadata>(
        std::move(*movement_metadata)
    );

    auto artifact = compile_luau_script_module(
        game,
        LuauCompileOptions {
            .module_metadata_resolver =
                [shared_movement_metadata](std::string_view specifier)
                -> Result<
                    std::shared_ptr<const LuauModuleMetadata>,
                    LuauScriptError> {
                if (specifier != "./movement") {
                    return failure(
                        LuauScriptError {"unexpected module specifier"}
                    );
                }
                return shared_movement_metadata;
            },
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.front().functions.size() == 1);
    const auto& system = artifact->plugins.front().functions.front();
    CHECK(system.name == "project.scripts.movement.move");
    REQUIRE(system.params.size() == 1);
    const auto& query =
        static_cast<const DynamicQueryParamDecl&>(*system.params.front());
    REQUIRE(query.fields.size() == 1);
    CHECK(query.fields.front().type.type_name == "project.scripts.game.Player");
}

TEST_CASE(
    "Luau compiler omits unused imported exported functions",
    "[scripting_luau][compiler][system][import]"
) {
    const LuauScriptSource helpers {
        .name = "project://scripts/helpers.luau",
        .content = R"(
            export function unused()
            end
        )",
    };
    const LuauScriptSource game {
        .name = "project://scripts/game.luau",
        .content = R"(
            local Helpers = require("./helpers")

            export type Counter = {
                count: i32,
            }

            export local GamePlugin = Plugin.new {
                build = function(app: App)
                    app:insert_resource(Counter { count = 0 })
                end,
            }
        )",
    };
    auto helpers_metadata = compile_luau_module_metadata(helpers);
    REQUIRE(helpers_metadata);
    auto shared_helpers_metadata = std::make_shared<const LuauModuleMetadata>(
        std::move(*helpers_metadata)
    );

    auto artifact = compile_luau_script_module(
        game,
        LuauCompileOptions {
            .module_metadata_resolver =
                [shared_helpers_metadata](std::string_view specifier)
                -> Result<
                    std::shared_ptr<const LuauModuleMetadata>,
                    LuauScriptError> {
                if (specifier != "./helpers") {
                    return failure(
                        LuauScriptError {"unexpected module specifier"}
                    );
                }
                return shared_helpers_metadata;
            },
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK(artifact->functions.empty());
    CHECK(artifact->plugins.front().functions.empty());
}

TEST_CASE(
    "Luau Plugin playtests use imported reflected types",
    "[scripting_luau][compiler][plugin][playtest][import]"
) {
    auto artifact = compile_luau_script_module(
        LuauScriptSource {
            .name = "project://scripts/playtest.luau",
            .content = R"(
                local Gameplay = require("./gameplay")

                local function begin_step(ctx, action)
                    ctx:resource(Gameplay.State).value = action.value
                end

                export local PlaytestPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_playtest {
                            id = "game.main",
                            action = {type = "object"},
                            begin_step = begin_step,
                        }
                    end,
                }
            )",
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK(artifact->plugins.front().name == "PlaytestPlugin");
}

TEST_CASE(
    "Luau native playtest types do not require script module metadata",
    "[scripting_luau][compiler][plugin][playtest][native]"
) {
    register_generated_reflection();
    auto& registry = Registry::instance();
    registry
        .register_cls<NativePlaytestComponent>(
            {"ets", "test"},
            "NativePlaytestComponent"
        )
        .add_property("value", &NativePlaytestComponent::value);
    registry.add_annotation<NativePlaytestComponent>(
        annotations::ScriptModule {.name = "compiler-test"}
    );

    bool resolved_script_metadata = false;
    auto artifact = compile_luau_script_module(
        LuauScriptSource {
            .name = "project://scripts/native_playtest.luau",
            .content = R"(
                local native = require("@entisium/compiler-test")

                local function observe(ctx)
                    local values = ctx:query {
                        Read(native.NativePlaytestComponent),
                    }
                    return {count = values:size()}
                end

                export local PlaytestPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_playtest {
                            id = "game.main",
                            action = {type = "object"},
                            observe = observe,
                        }
                    end,
                }
            )",
        },
        LuauCompileOptions {
            .module_metadata_resolver =
                [&resolved_script_metadata](std::string_view)
                -> Result<
                    std::shared_ptr<const LuauModuleMetadata>,
                    LuauScriptError> {
                resolved_script_metadata = true;
                return failure(LuauScriptError {"unexpected resolver call"});
            },
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK_FALSE(resolved_script_metadata);
}

TEST_CASE(
    "Luau compiler selects one of multiple exported Plugins",
    "[scripting_luau][compiler][plugin][export]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/features.luau",
        .content = R"(
            local function core()
            end

            local function debug_draw()
            end

            export local CorePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, core)
                end,
            }

            export local DebugPlugin = Plugin.new {
                dependencies = { CorePlugin },
                build = function(app: App)
                    app:add_system(Update, debug_draw)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.size() == 2);
    const auto* debug = artifact->find_plugin("DebugPlugin");
    REQUIRE(debug != nullptr);
    const auto dependencies = artifact->plugin_dependencies(debug->name);
    REQUIRE(dependencies.size() == 1);
    CHECK(dependencies[0].import_specifier.empty());
    CHECK(dependencies[0].plugin_name == "CorePlugin");
    REQUIRE(debug->functions.size() == 2);
    REQUIRE(artifact->functions.size() == 2);
}

TEST_CASE(
    "Luau compiler resolves fixed main schedules",
    "[scripting_luau][compiler][schedule]"
) {
    const LuauScriptSource source {
        .name = "fixed_update.luau",
        .content = R"(
            local function fixed_system()
            end

            export local FixedPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(MainSchedules.FixedUpdate, fixed_system)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact->plugins.front().functions.size() == 1);
}

TEST_CASE(
    "Luau compiler derives module identity from the source path",
    "[scripting_luau][compiler][module]"
) {
    const LuauScriptSource source {
        .name = "project://scripts/gameplay/movement.luau",
        .content = R"(
            export type Position = {}

            export local MovementPlugin = Plugin.new {
                build = function(app: App)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK(
        artifact->metadata->schema.name == "project.scripts.gameplay.movement"
    );
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    CHECK(
        artifact->metadata->schema.types.front().qualified_name ==
        "project.scripts.gameplay.movement.Position"
    );
}

TEST_CASE(
    "Luau compiler rejects top-level return declarations",
    "[scripting_luau][compiler][module][error]"
) {
    const std::vector<std::string> sources {
        "return {}",
        "return nil",
    };

    for (const auto& content : sources) {
        auto artifact = compile_luau_script_module(
            LuauScriptSource {.name = "movement.luau", .content = content}
        );
        REQUIRE_FALSE(artifact);
        CHECK(
            artifact.error().message.find("top-level return declarations") !=
            std::string::npos
        );
    }
}

TEST_CASE(
    "Luau compiler accepts export-only modules",
    "[scripting_luau][compiler][module][export]"
) {
    auto artifact = compile_luau_script_module(
        LuauScriptSource {
            .name = "project://scripts/math_helpers.luau",
            .content = R"(
                export type Offset = {
                    value: i32,
                }

                export function add(lhs: i32, rhs: i32): i32
                    return lhs + rhs
                end
            )",
        }
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    CHECK(artifact->plugins.empty());
    CHECK(artifact->functions.empty());
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    CHECK(artifact->metadata->schema.types[0].name == "Offset");
}

TEST_CASE(
    "Luau sample modules use exported Plugins",
    "[scripting_luau][compiler][sample][plugin]"
) {
    const auto repository =
        std::filesystem::path {ETS_ASSETS_PATH}.parent_path();
    const std::vector<std::filesystem::path> samples {
        "samples/snapshot_game.luau",
        "samples/projects/scripting/assets/scripts/card_battle_test.luau",
        "samples/projects/scripting/assets/scripts/checkpoint_render.luau",
        "samples/projects/scripting/assets/scripts/movement.luau",
        "samples/projects/scripting/assets/scripts/platformer_test.luau",
        "samples/projects/scripting/assets/scripts/pointer_puzzle_test.luau",
        "samples/projects/scripting/assets/scripts/ui_demo.luau",
    };
    for (const auto& relative : samples) {
        const auto path = repository / relative;
        std::ifstream input(path, std::ios::binary);
        INFO(path.string());
        REQUIRE(input);
        const std::string content {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>(),
        };
        auto artifact = compile_luau_script_module(
            LuauScriptSource {
                .name = relative.generic_string(),
                .content = content,
            },
            LuauCompileOptions {.snapshot_safe = false}
        );
        if (!artifact) {
            FAIL(artifact.error().message);
        }
        CHECK_FALSE(artifact->plugins.front().name.empty());
    }
}

TEST_CASE(
    "Skyline Strike stores generic asset fields as untyped handles",
    "[scripting_luau][compiler][sample][asset]"
) {
    const auto repository =
        std::filesystem::path {ETS_ASSETS_PATH}.parent_path();
    const auto path =
        repository / "samples/projects/skyline_strike/assets/gameplay.luau";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const std::string content {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
    auto artifact = compile_luau_script_module(
        LuauScriptSource {
            .name = "skyline_strike/gameplay.luau",
            .content = content,
        },
        LuauCompileOptions {.snapshot_safe = false}
    );
    if (!artifact) {
        FAIL(artifact.error().message);
    }

    const auto game_assets = std::ranges::find(
        artifact->metadata->schema.types,
        "GameAssets",
        &LuauTypeDecl::name
    );
    REQUIRE(game_assets != artifact->metadata->schema.types.end());
    REQUIRE(game_assets->fields.size() == 6);
    for (const auto& field : game_assets->fields) {
        CHECK(field.type.type_name == "UntypedHandle");
        REQUIRE(field.type.type_id);
        CHECK(*field.type.type_id == type_id<UntypedHandle>());
    }
}

TEST_CASE(
    "Luau compiler extracts multiple queries and resources from parameters",
    "[scripting_luau][compiler]"
) {
    const LuauScriptSource source {
        .name = "movement.luau",
        .content = R"(
            local function movement_system(
                movers: Query<Write<Position>, Read<Velocity>>,
                obstacles: Filtered<Query<Read<Transform>, Entity>, With<Collider>, Without<Disabled>>,
                time: ResRO<Time>,
                config: ResRW<MovementConfig>?
            )
            end

            export local MovementPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(MainSchedules.Update, movement_system)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact.has_value());
    CHECK(artifact->metadata->schema.name == "movement");
    REQUIRE(artifact->plugins.front().functions.size() == 1);
    const auto& system = artifact->plugins.front().functions.front();
    CHECK(system.name == "movement_system");
    REQUIRE(system.params.size() == 4);

    const auto* movers =
        dynamic_cast<const DynamicQueryParamDecl*>(system.params[0].get());
    REQUIRE(movers != nullptr);
    CHECK(movers->name == "movers");
    REQUIRE(movers->fields.size() == 2);
    CHECK(movers->fields[0].type.type_name == "Position");
    CHECK(movers->fields[0].access == DynamicParamAccess::Write);
    CHECK(movers->fields[1].type.type_name == "Velocity");
    CHECK(movers->fields[1].access == DynamicParamAccess::Read);

    const auto* obstacles =
        dynamic_cast<const DynamicQueryParamDecl*>(system.params[1].get());
    REQUIRE(obstacles != nullptr);
    REQUIRE(obstacles->fields.size() == 2);
    CHECK(obstacles->fields[1].kind == DynamicQueryFieldDeclKind::Entity);
    REQUIRE(obstacles->filters.size() == 2);
    CHECK(obstacles->filters[0].type.type_name == "Collider");
    CHECK(obstacles->filters[0].required);
    CHECK(obstacles->filters[1].type.type_name == "Disabled");
    CHECK_FALSE(obstacles->filters[1].required);

    const auto* time =
        dynamic_cast<const DynamicResourceParamDecl*>(system.params[2].get());
    REQUIRE(time != nullptr);
    CHECK(time->type.type_name == "Time");
    CHECK(time->access == DynamicParamAccess::Read);
    CHECK_FALSE(time->optional);

    const auto* config =
        dynamic_cast<const DynamicResourceParamDecl*>(system.params[3].get());
    REQUIRE(config != nullptr);
    CHECK(config->type.type_name == "MovementConfig");
    CHECK(config->access == DynamicParamAccess::Write);
    CHECK(config->optional);
    CHECK_FALSE(artifact->bytecode.empty());
}

TEST_CASE(
    "Luau runtime rejects unannotated functions used as systems",
    "[scripting_luau][compiler]"
) {
    const LuauScriptSource source {
        .name = "invalid.luau",
        .content = R"(
            local function invalid_system(value)
            end

            export local InvalidPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, invalid_system)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    auto built = runtime.call_module_plugin_build(
        *module,
        "InvalidPlugin",
        [](LuauPluginBuildOperation) -> Status<LuauScriptError> {
            return {};
        }
    );
    REQUIRE_FALSE(built);
}

TEST_CASE(
    "Luau compiler extracts exported script types and resources",
    "[scripting_luau][compiler][types][resources]"
) {
    const LuauScriptSource source {
        .name = "combat.luau",
        .content = R"(
            local function tick(
                health: Query<Write<Health>>,
                config: ResRW<CombatConfig>
            )
            end

            export type Health = {
                current: i32,
                scale: f32,
            }

            export type CombatConfig = {
                enabled: bool,
                health: Health,
                label: str,
            }

            export local CombatPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(CombatConfig {
                        enabled = false,
                        label = "runtime",
                    })
                    app:add_system(Update, tick)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact.has_value());
    REQUIRE(artifact->metadata->schema.types.size() == 2);
    const auto& config_type = artifact->metadata->schema.types[0];
    CHECK(config_type.name == "CombatConfig");
    CHECK(config_type.qualified_name == "combat.CombatConfig");
    REQUIRE(config_type.fields.size() == 3);
    CHECK(config_type.fields[0].name == "enabled");
    CHECK_FALSE(config_type.fields[0].has_default);
    CHECK(config_type.fields[1].name == "health");
    CHECK(config_type.fields[1].type.type_name == "combat.Health");
    CHECK(config_type.fields[1].type.script_type);
    CHECK_FALSE(config_type.fields[1].has_default);
    CHECK(config_type.fields[2].name == "label");
    CHECK(config_type.fields[2].type.type_name == "string");
    CHECK_FALSE(config_type.fields[2].has_default);

    const auto& health_type = artifact->metadata->schema.types[1];
    REQUIRE(health_type.fields.size() == 2);
    CHECK(health_type.fields[0].name == "current");
    CHECK(health_type.fields[0].type.type_name == "i32");
    CHECK_FALSE(health_type.fields[0].has_default);
    CHECK(health_type.fields[1].name == "scale");
    CHECK_FALSE(health_type.fields[1].has_default);

    REQUIRE(artifact->plugins.front().functions.size() == 1);
    const auto& system = artifact->plugins.front().functions.front();
    const auto& query =
        static_cast<const DynamicQueryParamDecl&>(*system.params[0]);
    CHECK(query.fields[0].type.type_name == "combat.Health");
    const auto& config =
        static_cast<const DynamicResourceParamDecl&>(*system.params[1]);
    CHECK(config.type.type_name == "combat.CombatConfig");
}

TEST_CASE(
    "Luau compiler keeps unused unsupported exports as ordinary Luau types",
    "[scripting_luau][compiler][types]"
) {
    const LuauScriptSource source {
        .name = "invalid_type.luau",
        .content = R"(
            export type Health = {
                current: {number},
            }

            export local InvalidPlugin = Plugin.new {
                build = function(app: App)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    CHECK(artifact->metadata->schema.types.empty());
    const auto* exported = artifact->metadata->find_exported_type("Health");
    REQUIRE(exported != nullptr);
    CHECK_FALSE(exported->runtime_compatible);
    CHECK(
        exported->runtime_error.find("non-generic named types") !=
        std::string::npos
    );
}

TEST_CASE(
    "Luau compiler validates unsupported types when used as runtime values",
    "[scripting_luau][compiler][types][usage]"
) {
    const LuauScriptSource source {
        .name = "invalid_runtime_type.luau",
        .content = R"(
            export type Health = {
                current: {number},
            }

            export local InvalidPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(Health { current = {1} })
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE_FALSE(artifact.has_value());
    CHECK(
        artifact.error().message.find(
            "cannot be used by an Entisium runtime"
        ) != std::string::npos
    );
    CHECK(
        artifact.error().message.find("non-generic named types") !=
        std::string::npos
    );
}

TEST_CASE(
    "Luau compiler does not restrict ordinary exported aliases",
    "[scripting_luau][compiler][types][ordinary]"
) {
    const LuauScriptSource source {
        .name = "ordinary_types.luau",
        .content = R"(
            export type Result<T, E> = {
                value: T?,
                error: E?,
            }
            export type Callback = (number) -> string
            export type Nested = { values: {number} }

            export local OrdinaryPlugin = Plugin.new {
                build = function(app: App)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    CHECK(artifact->metadata->schema.types.empty());
    REQUIRE(artifact->metadata->exported_types.size() == 3);
    CHECK(
        std::ranges::none_of(
            artifact->metadata->exported_types,
            [](const LuauExportedTypeMetadata& type) {
                return type.runtime_compatible;
            }
        )
    );
}

TEST_CASE(
    "Luau compiler limits unsupported type checks to runtime APIs",
    "[scripting_luau][compiler][types][usage]"
) {
    const LuauScriptSource source {
        .name = "ordinary_type_value.luau",
        .content = R"(
            export type Health = {
                current: {number},
            }

            local ordinary_value = Health

            export local OrdinaryPlugin = Plugin.new {
                build = function(app: App)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    CHECK(artifact->metadata->schema.types.empty());
}

TEST_CASE(
    "Luau compiler validates imported types at runtime API boundaries",
    "[scripting_luau][compiler][types][usage][import]"
) {
    const LuauScriptSource types {
        .name = "project://scripts/types.luau",
        .content = R"(
            export type Health = {
                current: {number},
            }
        )",
    };
    const LuauScriptSource game {
        .name = "project://scripts/game.luau",
        .content = R"(
            local Types = require("./types")

            export local GamePlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(Types.Health { current = {1} })
                end,
            }
        )",
    };

    auto metadata = compile_luau_module_metadata(types);
    REQUIRE(metadata.has_value());
    const auto shared_metadata =
        std::make_shared<const LuauModuleMetadata>(std::move(*metadata));
    auto artifact = compile_luau_script_module(
        game,
        LuauCompileOptions {
            .module_metadata_resolver =
                [shared_metadata](std::string_view specifier)
                -> Result<
                    std::shared_ptr<const LuauModuleMetadata>,
                    LuauScriptError> {
                if (specifier != "./types") {
                    return failure(
                        LuauScriptError {"unexpected module specifier"}
                    );
                }
                return shared_metadata;
            },
        }
    );

    REQUIRE_FALSE(artifact.has_value());
    CHECK(
        artifact.error().message.find(
            "cannot be used by an Entisium runtime"
        ) != std::string::npos
    );
    CHECK(artifact.error().message.find("Health") != std::string::npos);
}

TEST_CASE(
    "Luau compiler extracts optional entity fields",
    "[scripting_luau][compiler][types][optional]"
) {
    const LuauScriptSource source {
        .name = "optional_entity.luau",
        .content = R"(
            export type TargetState = {
                target: entity?,
            }

            export local TargetPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(TargetState {})
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    REQUIRE(artifact->metadata->schema.types[0].fields.size() == 1);
    const auto& target = artifact->metadata->schema.types[0].fields[0];
    CHECK(target.type.type_name == "entity");
    REQUIRE(target.type.type_id.has_value());
    CHECK(*target.type.type_id == type_id<Entity>());
    CHECK(target.type.optional);
    CHECK_FALSE(target.has_default);
}

TEST_CASE(
    "Luau compiler validates script-defined state declarations",
    "[scripting_luau][compiler][state]"
) {
    const LuauScriptSource source {
        .name = "invalid_state.luau",
        .content = R"(
            export type GameState = "Menu" | "Playing"

            export local InvalidPlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameState.Missing)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    auto built = runtime.call_module_plugin_build(
        *module,
        "InvalidPlugin",
        [](LuauPluginBuildOperation) -> Status<LuauScriptError> {
            return {};
        }
    );
    REQUIRE_FALSE(built);
}

TEST_CASE(
    "Luau compiler extracts Bevy-style system configuration chains",
    "[scripting_luau][compiler][schedule]"
) {
    const LuauScriptSource source {
        .name = "configured_systems.luau",
        .content = R"(
            local function first()
            end

            local function enabled(): boolean
                return true
            end

            local function second()
            end

            local function third()
            end

            export local SystemsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(
                        Update,
                        third,
                        second
                            :after(first)
                            :before(third)
                            :run_if(enabled),
                        first
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact);
    REQUIRE(artifact->plugins.front().functions.size() == 4);
}

TEST_CASE(
    "Luau compiler defers configured system scheduling to runtime",
    "[scripting_luau][compiler][schedule][error]"
) {
    const std::vector<std::pair<std::string, std::string>> invalid_sources {
        {
            R"(
                local function first() end
                local function missing() end
                export local InvalidPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_system(Update, first:after(missing))
                    end,
                }
            )",
            "unregistered system",
        },
        {
            R"(
                local function first() end
                local function second() end
                export local InvalidPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_systems(
                            Update,
                            first:after(second),
                            second:after(first)
                        )
                    end,
                }
            )",
            "cycle detected",
        },
        {
            R"(
                local function tick() end
                export local InvalidPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_system(Update, chain(tick))
                    end,
                }
            )",
            "at least two",
        },
        {
            R"(
                local function writable(config: ResRW<Config>): boolean
                    return true
                end
                local function tick() end
                export local InvalidPlugin = Plugin.new {
                    build = function(app: App)
                        app:add_system(Update, tick:run_if(writable))
                    end,
                }
            )",
            "read-only",
        },
    };

    for (const auto& [content, expected] : invalid_sources) {
        auto artifact = compile_luau_script_module(
            LuauScriptSource {.name = "invalid.luau", .content = content}
        );
        INFO(expected);
        REQUIRE(artifact);
    }
}

TEST_CASE(
    "Luau compiler expands nested system chains",
    "[scripting_luau][compiler][schedule][chain]"
) {
    const LuauScriptSource source {
        .name = "system_chain.luau",
        .content = R"(
            local function first() end
            local function enabled(): boolean return true end
            local function second() end
            local function third() end
            local function independent() end

            export local SystemsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(
                        Update,
                        chain(
                            first,
                            chain(second:run_if(enabled), third)
                        ),
                        independent
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    if (!artifact) {
        FAIL(artifact.error().message);
    }
    REQUIRE(artifact);
    REQUIRE(artifact->plugins.front().functions.size() == 5);
}

TEST_CASE(
    "Luau compiler rejects snapshot-unsafe hidden state",
    "[scripting_luau][compiler][snapshot]"
) {
    const std::vector<std::pair<std::string_view, std::string_view>>
        invalid_sources {
            {
                R"(
                    counter = 0
                    local function tick() end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "assignment to global 'counter'",
            },
            {
                R"(
                    function tick() end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "assignment to global 'tick'",
            },
            {
                R"(
                    local counter = 0
                    local function tick()
                        counter += 1
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot reassign readonly module binding 'counter'",
            },
            {
                R"(
                    local counter = 0
                    counter = 1
                    local function tick() end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot reassign readonly module binding 'counter'",
            },
            {
                R"(
                    local counter = 0
                    counter += 1
                    local function tick() end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot reassign readonly module binding 'counter'",
            },
            {
                R"(
                    local counter = 0
                    local function tick()
                        counter = 1
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot reassign readonly module binding 'counter'",
            },
            {
                R"(
                    local function tick() end
                    tick = function() end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot reassign readonly module binding 'tick'",
            },
            {
                R"(
                    local cache = { value = 0 }
                    local function tick()
                        cache.value = cache.value + 1
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "mutation of module state 'cache'",
            },
            {
                R"(
                    local cache = {}
                    local function tick()
                        table.insert(cache, 1)
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "cannot mutate captured module state",
            },
            {
                R"(
                    local cache = { value = 0 }
                    local function tick()
                        local alias = cache
                        alias.value += 1
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "mutation of module state 'alias'",
            },
            {
                R"(
                    local cache = { value = 0 }
                    local function mutate(value)
                        value.value += 1
                    end
                    local function tick()
                        mutate(cache)
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "captured module state cannot be passed to a call",
            },
            {
                R"(
                    local function tick()
                        math.snapshot_unsafe_value = 1
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "assignment to global 'math'",
            },
            {
                R"(
                    local function tick()
                        local value = math.random()
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "nondeterministic API 'math.random'",
            },
            {
                R"(
                    local core = require("@entisium/core")
                    local function tick()
                        core.Random = nil
                    end
                    export local InvalidPlugin = Plugin.new {
                        build = function(app: App)
                            app:add_system(Update, tick)
                        end,
                    }
                )",
                "assignment to readonly native module 'core'",
            },
        };

    for (const auto& [content, expected] : invalid_sources) {
        auto artifact = compile_luau_script_module(
            LuauScriptSource {
                .name = "snapshot_unsafe.luau",
                .content = std::string(content),
            }
        );
        REQUIRE_FALSE(artifact);
        CHECK(artifact.error().message.find(expected) != std::string::npos);
    }

    const LuauScriptSource safe_nested_capture {
        .name = "safe_nested_capture.luau",
        .content = R"(
            local function tick()
                local total = 0
                local function add(value: number)
                    total += value
                end
                add(2)
                assert(total == 2)
            end
            export local SafePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, tick)
                end,
            }
        )",
    };
    CHECK(compile_luau_script_module(safe_nested_capture));

    const LuauScriptSource safe_native_module_capture {
        .name = "safe_native_module_capture.luau",
        .content = R"(
            local core = require("@entisium/core")
            local function use_type(value)
                assert(value ~= nil)
            end
            local function tick()
                use_type(core.Random)
            end
            export local SafePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, tick)
                end,
            }
        )",
    };
    CHECK(compile_luau_script_module(safe_native_module_capture));

    auto module = compile_luau_script_module(
        LuauScriptSource {
            .name = "stateful_module.luau",
            .content = R"(
                local value = 0
                export function next(): number
                    value += 1
                    return value
                end
            )",
        }
    );
    REQUIRE_FALSE(module);
    CHECK(
        module.error().message.find(
            "cannot reassign readonly module binding 'value'"
        ) != std::string::npos
    );
}

} // namespace ets::test
