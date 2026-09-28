#include "app/app.hpp"
#include "asset/assets.hpp"
#include "asset/loader.hpp"
#include "asset/server.hpp"
#include "asset/source.hpp"
#include "ecs/commands.hpp"
#include "ecs/dynamic/events.hpp"
#include "ecs/dynamic/query.hpp"
#include "ecs/dynamic/world.hpp"
#include "ecs/state.hpp"
#include "ecs/world.hpp"
#include "refl/annotations.hpp"
#include "refl/cls.hpp"
#include "refl/generated.hpp"
#include "refl/registry.hpp"
#include "scripting/compiler.hpp"
#include "scripting/detail/plugin_install.hpp"
#include "scripting/detail/script_system_loader.hpp"
#include "scripting/runtime.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <string>

using namespace ets;

namespace ets::luau_system_test {

struct Config {
    int value {0};
};

struct ModuleOnlyConfig {
    int value {0};
};

} // namespace ets::luau_system_test

namespace {

struct LuauTestPosition {
    float x {0.0F};

    void advance(float amount) { x += amount; }

    bool operator==(const LuauTestPosition&) const = default;
};

struct LuauTestVelocity {
    float x {0.0F};
};

struct LuauTestTime {
    float delta {0.0F};

    float scaled(float value) const { return value * delta; }
};

struct LuauTestObstacle {
    int weight {0};
};

struct LuauTestError {
    int code {0};
};

struct LuauPropertyCacheTarget {
    int first {0};
    int second {0};
};

enum class LuauTestMode {
    Idle,
    Active,
};

struct LuauTestNested {
    LuauTestPosition position;
    LuauTestMode mode {LuauTestMode::Idle};
};

struct LuauChangeDetectionTarget {
    int operation {0};
    int value {0};
    LuauTestPosition position;

    bool operator==(const LuauChangeDetectionTarget&) const = default;
};

struct LuauTestConstructionState {
    float positional_x {0.0F};
    float initialized_x {0.0F};
    int spawned {0};
    LuauTestMode mode {LuauTestMode::Idle};

    void set_mode(LuauTestMode value) { mode = value; }
};

struct LuauTestConfig {
    int executions {0};
    int obstacle_total {0};
    float generated_x {0.0F};
    bool schedule_enabled {true};
    int schedule_order {0};
    int state_order {0};

    void add_execution(int count) { executions += count; }

    LuauTestPosition make_position(float x) const {
        return LuauTestPosition {.x = x};
    }

    Result<int, LuauTestError> result(bool succeed) const {
        if (succeed) {
            return 42;
        }
        return failure(LuauTestError {.code = 9});
    }

    bool matches_type(TypeId type) const {
        return type == type_id<LuauTestPosition>();
    }

    TypeId position_type() const { return type_id<LuauTestPosition>(); }
};

struct LuauTestCommandState {
    int target {0};
    int parent {0};
    int detached {0};
    int doomed {0};
    int spawned {0};
    int total {0};

    LuauTestPosition make_position(float x) const {
        return LuauTestPosition {.x = x};
    }

    LuauTestError make_error(int code) const {
        return LuauTestError {.code = code};
    }
};

struct LuauTestAsset {
    int byte_count {0};
};

struct LuauTestAssetState {
    Handle<LuauTestAsset> handle;
    Optional<Handle<LuauTestAsset>> optional_handle;
    bool loaded {false};
    bool readonly_rejected {false};
};

class LuauTestAssetSource : public AssetSource {
  private:
    std::array<std::byte, 3> m_bytes {
        std::byte {1},
        std::byte {2},
        std::byte {3},
    };

  public:
    std::string name() const override { return "memory"; }

    bool exists(const std::filesystem::path& path) const override {
        return path.generic_string() == "asset.bin";
    }

    Result<Reader, std::string>
    try_get_reader(const std::filesystem::path& path) const override {
        if (!exists(path)) {
            return failure(std::string("Luau test asset not found"));
        }
        return Reader(m_bytes.data(), m_bytes.size());
    }
};

class LuauTestAssetLoader : public AssetLoader<LuauTestAsset> {
  public:
    AssetLoadResult<LuauTestAsset>
    load(Reader& reader, const LoadContext& /*context*/) override {
        return std::make_unique<LuauTestAsset>(LuauTestAsset {
            .byte_count = static_cast<int>(reader.size()),
        });
    }
};

void register_luau_system_test_types() {
    auto& registry = Registry::instance();
    registry.register_cls<LuauTestPosition>()
        .add_constructor<LuauTestPosition, float>()
        .add_property("x", &LuauTestPosition::x)
        .add_method("advance", &LuauTestPosition::advance);
    registry.register_cls<LuauTestVelocity>().add_property(
        "x",
        &LuauTestVelocity::x
    );
    registry.register_cls<LuauTestTime>()
        .add_property("delta", &LuauTestTime::delta)
        .add_method("scaled", &LuauTestTime::scaled);
    registry.register_cls<LuauTestObstacle>().add_property(
        "weight",
        &LuauTestObstacle::weight
    );
    registry.register_cls<LuauTestError>().add_property(
        "code",
        &LuauTestError::code
    );
    registry.register_enum<LuauTestMode>()
        .add_enumerator("Idle", static_cast<std::int64_t>(LuauTestMode::Idle))
        .add_enumerator(
            "Active",
            static_cast<std::int64_t>(LuauTestMode::Active)
        );
    registry.register_cls<LuauTestNested>()
        .add_property("position", &LuauTestNested::position)
        .add_property("mode", &LuauTestNested::mode);
    registry.register_cls<LuauChangeDetectionTarget>()
        .add_property("operation", &LuauChangeDetectionTarget::operation)
        .add_property("value", &LuauChangeDetectionTarget::value)
        .add_property("position", &LuauChangeDetectionTarget::position);
    registry.register_cls<LuauTestConstructionState>()
        .add_property("positional_x", &LuauTestConstructionState::positional_x)
        .add_property(
            "initialized_x",
            &LuauTestConstructionState::initialized_x
        )
        .add_property("spawned", &LuauTestConstructionState::spawned)
        .add_property("mode", &LuauTestConstructionState::mode)
        .add_method("set_mode", &LuauTestConstructionState::set_mode);
    registry.register_cls<LuauTestConfig>()
        .add_property("executions", &LuauTestConfig::executions)
        .add_property("obstacle_total", &LuauTestConfig::obstacle_total)
        .add_property("generated_x", &LuauTestConfig::generated_x)
        .add_property("schedule_enabled", &LuauTestConfig::schedule_enabled)
        .add_property("schedule_order", &LuauTestConfig::schedule_order)
        .add_property("state_order", &LuauTestConfig::state_order)
        .add_method("add_execution", &LuauTestConfig::add_execution)
        .add_method("make_position", &LuauTestConfig::make_position)
        .add_method("result", &LuauTestConfig::result)
        .add_method("matches_type", &LuauTestConfig::matches_type)
        .add_method("position_type", &LuauTestConfig::position_type);
    registry.register_cls<LuauTestCommandState>()
        .add_property("target", &LuauTestCommandState::target)
        .add_property("parent", &LuauTestCommandState::parent)
        .add_property("detached", &LuauTestCommandState::detached)
        .add_property("doomed", &LuauTestCommandState::doomed)
        .add_property("spawned", &LuauTestCommandState::spawned)
        .add_property("total", &LuauTestCommandState::total)
        .add_method("make_position", &LuauTestCommandState::make_position)
        .add_method("make_error", &LuauTestCommandState::make_error);
    registry.register_type<LuauTestAsset>();
    registry.register_cls<LuauTestAssetState>()
        .add_property("handle", &LuauTestAssetState::handle)
        .add_property("optional_handle", &LuauTestAssetState::optional_handle)
        .add_property("loaded", &LuauTestAssetState::loaded)
        .add_property(
            "readonly_rejected",
            &LuauTestAssetState::readonly_rejected
        );
    registry.register_cls<AssetServer>();
}

} // namespace

TEST_CASE(
    "Luau system loader exposes ScriptPrelude reflected names",
    "[scripting_luau][system][namespace][prelude]"
) {
    register_generated_reflection();
    auto& registry = Registry::instance();
    registry
        .register_cls<luau_system_test::Config>(
            {"ets", "luau_system_test"},
            "Config"
        )
        .add_property("value", &luau_system_test::Config::value);
    registry.add_annotation<luau_system_test::Config>(
        annotations::ScriptPrelude {}
    );
    const LuauScriptSource source {
        .name = "structured_name.luau",
        .content = R"(
            local function verify(config: ResRO<luau_system_test.Config>)
                assert(luau_system_test.Config ~= nil)
                assert(config.value == 7)
            end

            export local TestPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, verify)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(luau_system_test::Config {.value = 7});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    world.run_schedule(Update);
    CHECK(remove_luau_plugin_systems(world, *systems));
}

TEST_CASE(
    "Luau system loader only exposes ScriptPrelude types as globals",
    "[scripting_luau][system][prelude]"
) {
    register_generated_reflection();
    auto& registry = Registry::instance();
    registry
        .register_cls<luau_system_test::ModuleOnlyConfig>(
            {"ets", "luau_system_test"},
            "ModuleOnlyConfig"
        )
        .add_property("value", &luau_system_test::ModuleOnlyConfig::value);
    registry.add_annotation<luau_system_test::ModuleOnlyConfig>(
        annotations::ScriptModule {.name = "test"}
    );

    const LuauScriptSource source {
        .name = "module_only_type.luau",
        .content = R"(
            local function verify(
                config: ResRO<luau_system_test.ModuleOnlyConfig>
            )
                assert(config.value == 7)
            end

            export local TestPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, verify)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(luau_system_test::ModuleOnlyConfig {.value = 7});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE_FALSE(systems);
    CHECK(
        systems.error().message.find("is not in ScriptPrelude") !=
        std::string::npos
    );
    CHECK(systems.error().message.find("@entisium/test") != std::string::npos);
}

TEST_CASE(
    "Luau systems load typed assets through AssetServer",
    "[scripting_luau][system][asset]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "asset_system.luau",
        .content = R"(
            export type LuauTestAssets = {
                primary: Handle<LuauTestAsset>,
            }

            local function load_asset(
                assets: ResRW<AssetServer>,
                handles: ResRW<LuauTestAssets>,
                state: ResRW<LuauTestAssetState>
            )
                local handle = assets:load(
                    LuauTestAsset,
                    "memory://asset.bin"
                )
                assert(assets:is_loaded(handle))
                local cached = assets:load_async(
                    LuauTestAsset,
                    "memory://asset.bin"
                )
                assert(assets:is_loaded(cached))
                assert(assets:load_error(cached) == nil)
                handles.primary = cached
                state.handle = handles.primary
                state.optional_handle = handles.primary
                state.loaded = true

                local missing = assets:load(
                    LuauTestAsset,
                    "memory://missing.bin"
                )
                assert(not assets:is_loaded(missing))
                local load_error = assets:load_error(missing)
                assert(load_error ~= nil)
                assert(
                    load_error.path:as_string() == "memory://missing.bin"
                )
                assert(
                    load_error.path:resolve("child.bin"):as_string() ==
                    "memory://missing.bin/child.bin"
                )
                local missing_again = assets:load_async(
                    LuauTestAsset,
                    load_error.path
                )
                assert(not assets:is_loaded(missing_again))
                assert(string.find(
                    load_error.message,
                    "Asset not found",
                    1,
                    true
                ))
            end

            local function reject_readonly_load(
                assets: ResRO<AssetServer>,
                state: ResRW<LuauTestAssetState>
            )
                local ok, err = pcall(function()
                    assets:load(LuauTestAsset, "memory://asset.bin")
                end)
                assert(not ok)
                assert(string.find(err, "ResRW<AssetServer>", 1, true))
                state.readonly_rejected = true
            end

            export local AssetPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(LuauTestAssets {})
                    app:add_systems(
                        Update,
                        load_asset,
                        reject_readonly_load
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    REQUIRE(artifact->metadata->schema.types.size() == 1);
    REQUIRE(artifact->metadata->schema.types.front().fields.size() == 1);
    const auto& handle_type =
        artifact->metadata->schema.types.front().fields.front().type;
    CHECK(handle_type.type_name == "UntypedHandle");
    REQUIRE(handle_type.type_id);
    CHECK(*handle_type.type_id == type_id<UntypedHandle>());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);

    App app;
    AssetServer server(&app);
    server.emplace_source<LuauTestAssetSource>();
    app.add_resource(std::move(server));
    app.resource<AssetServer>()
        .add_loader<LuauTestAsset, LuauTestAssetLoader>();
    app.add_resource(LuauTestAssetState {});

    auto systems = detail::install_luau_script_systems(
        app.world(),
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    app.world().sort_systems();
    app.run_schedule(Update);

    const auto& state = app.resource<LuauTestAssetState>();
    CHECK(state.loaded);
    CHECK(state.readonly_rejected);
    REQUIRE(state.optional_handle);
    CHECK(state.optional_handle->id() == state.handle.id());
    auto asset = app.resource<Assets<LuauTestAsset>>().get(state.handle);
    REQUIRE(asset);
    CHECK(asset->byte_count == 3);
}

TEST_CASE(
    "Luau constructs reflected values and exposes reflected enums",
    "[scripting_luau][system][construction][enum]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "construction_system.luau",
        .content = R"(
            local function construct_values(
                world: World,
                state: ResRW<LuauTestConstructionState>
            )
                local positional = LuauTestPosition.new(3.5)
                positional:advance(1.5)
                local initialized = LuauTestPosition.new { x = 7.25 }
                local nested = LuauTestNested.new {
                    position = initialized,
                    mode = LuauTestMode.Active,
                }
                local retained_position = LuauTestNested.new {
                    position = LuauTestPosition.new { x = 7.25 },
                    mode = LuauTestMode.Active,
                }.position
                for _ = 1, 10000 do
                    LuauTestPosition.new { x = 1.0 }
                end

                local writable = pcall(function()
                    LuauTestMode.Active = LuauTestMode.Idle
                end)
                assert(not writable)

                state.positional_x = positional.x
                state.initialized_x = retained_position.x
                state:set_mode(nested.mode)
                state.spawned = world:spawn(nested):id()
            end

            export local ConstructionPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, construct_values)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(
        source,
        LuauCompileOptions {.snapshot_safe = false}
    );
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConstructionState {});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());
    world.run_schedule(Update);

    const auto& state = world.resource<LuauTestConstructionState>();
    CHECK(state.positional_x == 5.0F);
    CHECK(state.initialized_x == 7.25F);
    CHECK(state.mode == LuauTestMode::Active);
    const Entity spawned {static_cast<std::uint32_t>(state.spawned)};
    REQUIRE(world.has_component<LuauTestNested>(spawned));
    const auto& nested = world.get_component<LuauTestNested>(spawned);
    CHECK(nested.position.x == 7.25F);
    CHECK(nested.mode == LuauTestMode::Active);
}

TEST_CASE(
    "Luau systems use script-defined components and resources",
    "[scripting_luau][system][types][resources]"
) {
    const LuauScriptSource source {
        .name = "dynamic_types_system.luau",
        .content = R"(
            export type Health = {
                current: i32,
            }

            export type CombatState = {
                last_created: i32,
                ticks: i32,
            }

            local function tick(
                health_values: Query<Write<Health>>,
                state: ResRW<CombatState>
            )
                local created = Health.new { current = 7 }
                state.last_created = created.current
                state.ticks += 1
                for health in health_values do
                    health.current += 5
                end
            end

            export local CombatPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(CombatState {ticks = 2})
                    app:add_system(Update, tick)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());

    auto health_type =
        Registry::instance().try_get_type("dynamic_types_system.Health");
    REQUIRE(health_type.has_value());
    auto health = Val::default_construct(*health_type);
    const Entity entity = world.entity();
    world.add_component(entity, health.ref());

    world.run_schedule(Update);

    auto& registry = Registry::instance();
    auto& health_cls = registry.get_cls(health_type->id());
    auto current = health_cls.get_property("current").get(
        world.get_component(entity, health_type->id())
    );
    REQUIRE(current.has_value());
    CHECK(current->get<int>() == 5);

    auto state_type =
        Registry::instance().try_get_type("dynamic_types_system.CombatState");
    REQUIRE(state_type.has_value());
    auto& state_cls = registry.get_cls(state_type->id());
    const Ref state = world.resource(state_type->id());
    auto ticks = state_cls.get_property("ticks").get(state);
    auto last_created = state_cls.get_property("last_created").get(state);
    REQUIRE(ticks.has_value());
    REQUIRE(last_created.has_value());
    CHECK(ticks->get<int>() == 3);
    CHECK(last_created->get<int>() == 7);
}

TEST_CASE(
    "Luau World exposes live entities resources queries and commands",
    "[scripting_luau][system][world]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "world_system.luau",
        .content = R"(
            local function use_world(world: World)
                local state = world:resource(LuauTestCommandState)
                assert(state ~= nil)
                assert(world:has_entity(state.doomed))
                assert(world:entity(999999) == nil)
                assert(world:has_resource(LuauTestCommandState))

                local parent = world:spawn()
                local spawned = world:spawn(state:make_position(5))
                assert(spawned:has(LuauTestPosition))
                spawned:add(state:make_error(9))
                assert(spawned:get(LuauTestError).code == 9)
                spawned:remove(LuauTestError)

                spawned:set_parent(parent:id())
                local children = parent:children()
                assert(#children == 1 and children[1] == spawned:id())
                assert(spawned:parent() == parent:id())
                spawned:remove_parent()
                assert(spawned:parent() == nil)

                local positions = world:query {
                    Entity,
                    Write(LuauTestPosition),
                    Without(LuauTestError),
                }
                assert(not positions:empty() and positions:size() == 2)
                local first_entity, first_position = positions:first()
                assert(first_entity ~= nil and first_position ~= nil)

                local total = 0
                for entity, position in positions do
                    assert(entity ~= nil)
                    position.x += 10
                    total += position.x
                end

                world:commands():entity(state.doomed):despawn()
                state.spawned = spawned:id()
                state.total = total
                world:set_resource(state:make_error(total))
            end

            export local WorldPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, use_world)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    const Entity matched = world.entity();
    world.add_component(matched, LuauTestPosition {.x = 1});
    const Entity filtered = world.entity();
    world.add_component(filtered, LuauTestPosition {.x = 100});
    world.add_component(filtered, LuauTestError {.code = 1});
    const Entity doomed = world.entity();
    world.add_component(doomed, LuauTestVelocity {.x = 2});
    world.add_resource(
        LuauTestCommandState {.doomed = static_cast<int>(doomed.value)}
    );

    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());
    world.run_schedule(Update);

    const auto& state =
        static_cast<const World&>(world).resource<LuauTestCommandState>();
    const Entity spawned {static_cast<std::uint32_t>(state.spawned)};
    CHECK(state.total == 26);
    CHECK(world.get_component<LuauTestPosition>(matched).x == 11);
    CHECK(world.get_component<LuauTestPosition>(spawned).x == 15);
    CHECK_FALSE(world.has_entity(doomed));
    REQUIRE(world.has_resource<LuauTestError>());
    CHECK(world.resource<LuauTestError>().code == 26);
}

TEST_CASE(
    "Luau optional entity fields use nil semantics",
    "[scripting_luau][system][types][optional][entity]"
) {
    const LuauScriptSource source {
        .name = "optional_entity_system.luau",
        .content = R"(
            export type TargetState = {
                observed: entity,
                target: entity?,
            }

            local function tick(
                entities: Query<Entity>,
                state: ResRW<TargetState>
            )
                assert(state.target == nil)
                for entity in entities do
                    state.target = entity
                    assert(state.target == entity)
                    state.observed = state.target
                    state.target = nil
                    assert(state.target == nil)
                    break
                end
            end

            export local TargetPlugin = Plugin.new {
                build = function(app: App)
                    app:add_resource(TargetState {})
                    app:add_system(Update, tick)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    const Entity entity = world.entity();
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());

    world.run_schedule(Update);

    auto type =
        Registry::instance().try_get_type("optional_entity_system.TargetState");
    REQUIRE(type.has_value());
    auto& cls = Registry::instance().get_cls(type->id());
    const Ref state = world.resource(type->id());
    auto observed = cls.get_property("observed").get(state);
    auto target = cls.get_property("target").get(state);
    REQUIRE(observed.has_value());
    REQUIRE(target.has_value());
    CHECK(observed->get_const<Entity>() == entity);
    CHECK(target->get_const<Optional<Entity>>() == nullopt);
}

TEST_CASE(
    "Luau World queries reject structural changes during iteration",
    "[scripting_luau][world][query]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "world_query_invalidation.luau",
        .content = R"(
            local function invalidate(world: World)
                local positions = world:query { Write(LuauTestPosition) }
                for position in positions do
                    world:spawn(position)
                end
            end

            export local QueryPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, invalidate)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());
    REQUIRE(runtime.bind_module_type(
        *module,
        "LuauTestPosition",
        type<LuauTestPosition>()
    ));

    World world;
    const Entity entity = world.entity();
    world.add_component(entity, LuauTestPosition {.x = 1});
    DynamicWorld dynamic_world("world");
    auto prepared = dynamic_world.prepare(
        world,
        SystemTicks {
            .last_run = 0,
            .this_run = world.increment_change_tick(),
        }
    );
    REQUIRE(prepared.has_value());
    auto status = runtime.call_module_function(
        *module,
        "invalidate",
        std::span<const Ref> {&*prepared, 1}
    );
    dynamic_world.finish();

    REQUIRE_FALSE(status.has_value());
    CHECK(
        status.error().message.find(
            "World structurally changed during query iteration"
        ) != std::string::npos
    );
}

TEST_CASE(
    "Luau World views expire after the system invocation",
    "[scripting_luau][world][borrow]"
) {
    const LuauScriptSource source {
        .name = "world_borrow.luau",
        .content = R"(
            local escaped_entity = nil
            local escaped_query = nil

            local function capture(world: World)
                escaped_entity = world:spawn()
                escaped_query = world:query { Entity }
            end

            local function use_entity()
                return escaped_entity:id()
            end

            local function use_query()
                return escaped_query:size()
            end

            export local BorrowPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(Update, capture, use_entity, use_query)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(
        source,
        LuauCompileOptions {.snapshot_safe = false}
    );
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    DynamicWorld dynamic_world("world");
    auto prepared = dynamic_world.prepare(
        world,
        SystemTicks {
            .last_run = 0,
            .this_run = world.increment_change_tick(),
        }
    );
    REQUIRE(prepared.has_value());
    REQUIRE(runtime.call_module_function(
        *module,
        "capture",
        std::span<const Ref> {&*prepared, 1}
    ));
    dynamic_world.finish();

    auto entity = runtime.call_module_function(*module, "use_entity");
    REQUIRE_FALSE(entity.has_value());
    CHECK(entity.error().message.find("no longer active") != std::string::npos);
    auto query = runtime.call_module_function(*module, "use_query");
    REQUIRE_FALSE(query.has_value());
    CHECK(query.error().message.find("no longer active") != std::string::npos);
}

TEST_CASE(
    "Luau systems queue entity hierarchy and resource commands",
    "[scripting_luau][system][commands]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "commands_system.luau",
        .content = R"(
            local function apply_commands(
                commands: Commands,
                state: ResRW<LuauTestCommandState>,
                _velocities: Query<Entity, Read<LuauTestVelocity>>
            )
                local target = commands:entity(state.target)
                assert(target:has(LuauTestVelocity))
                target:add(state:make_position(8)):remove(LuauTestVelocity)
                target:set_parent(state.parent)

                commands:entity(state.detached):remove_parent()
                commands:entity(state.doomed):despawn()

                local spawned_parent = commands:spawn()
                local spawned = commands:spawn(state:make_position(3))
                spawned:set_parent(spawned_parent:id())
                state.spawned = spawned:id()
                state.total = spawned_parent:id()
                commands:add_resource(state:make_position(11))
            end

            export local CommandsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, apply_commands)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    const Entity target = world.entity();
    const Entity parent = world.entity();
    const Entity detached = world.entity();
    const Entity doomed = world.entity();
    world.add_component(target, LuauTestVelocity {.x = 2});
    world.add_component(doomed, LuauTestVelocity {.x = 4});
    world.set_parent(detached, parent);
    world.add_resource(
        LuauTestCommandState {
            .target = static_cast<int>(target.value),
            .parent = static_cast<int>(parent.value),
            .detached = static_cast<int>(detached.value),
            .doomed = static_cast<int>(doomed.value),
        }
    );

    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());

    world.run_schedule(Update);

    CHECK(world.has_component<LuauTestPosition>(target));
    CHECK(world.get_component<LuauTestPosition>(target).x == 8);
    CHECK_FALSE(world.has_component<LuauTestVelocity>(target));
    REQUIRE(world.parent(target));
    CHECK(*world.parent(target) == parent);
    CHECK_FALSE(world.has_parent(detached));
    CHECK_FALSE(world.has_entity(doomed));

    const Entity spawned {
        static_cast<std::uint32_t>(
            world.resource<LuauTestCommandState>().spawned
        ),
    };
    CHECK(world.has_entity(spawned));
    CHECK(world.get_component<LuauTestPosition>(spawned).x == 3);
    const Entity spawned_parent {
        static_cast<std::uint32_t>(
            world.resource<LuauTestCommandState>().total
        ),
    };
    REQUIRE(world.parent(spawned));
    CHECK(*world.parent(spawned) == spawned_parent);
    REQUIRE(world.has_resource<LuauTestPosition>());
    CHECK(world.resource<LuauTestPosition>().x == 11);
}

TEST_CASE(
    "Luau systems execute resource and query parameters",
    "[scripting_luau][system][query][resource]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "movement_system.luau",
        .content = R"(
            local function movement_system(
                movers: Query<Write<LuauTestPosition>, Read<LuauTestVelocity>>,
                obstacles: Query<Read<LuauTestObstacle>>,
                time: ResRO<LuauTestTime>,
                config: ResRW<LuauTestConfig>?
            )
                for position, velocity in movers do
                    position:advance(time:scaled(velocity.x))
                end
                if config then
                    config:add_execution(1)
                    for obstacle in obstacles do
                        config.obstacle_total += obstacle.weight
                    end
                    local generated = config:make_position(3.5)
                    generated:advance(1)
                    config.generated_x = generated.x
                    local value, err = config:result(false)
                    assert(value == nil and err.code == 9)
                    assert(config:matches_type(LuauTestPosition))
                    assert(config:matches_type(config:position_type()))
                end
            end

            export local MovementPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(MainSchedules.Update, movement_system)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    const auto has_direct_path = [&](TypeId root, std::string_view property) {
        return std::ranges::any_of(
            artifact->property_paths,
            [&](const LuauPropertyPathDecl& path) {
                return path.root_type == root && path.properties.size() == 1 &&
                       path.properties.front() == property;
            }
        );
    };
    CHECK(has_direct_path(type_id<LuauTestVelocity>(), "x"));
    CHECK(has_direct_path(type_id<LuauTestConfig>(), "obstacle_total"));
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestTime {.delta = 0.5F});
    world.add_resource(LuauTestConfig {});
    Entity entity = world.entity();
    world.add_component(entity, LuauTestPosition {.x = 1.0F});
    world.add_component(entity, LuauTestVelocity {.x = 2.0F});
    Entity obstacle = world.entity();
    world.add_component(obstacle, LuauTestObstacle {.weight = 7});

    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());
    REQUIRE(systems->size() == 1);

    world.run_schedule(Update);
    CHECK(world.get_component<LuauTestPosition>(entity).x == 2.0F);
    CHECK(world.resource<LuauTestConfig>().executions == 1);
    CHECK(world.resource<LuauTestConfig>().obstacle_total == 7);
    CHECK(world.resource<LuauTestConfig>().generated_x == 4.5F);

    CHECK(remove_luau_plugin_systems(world, *systems));

    World world_without_optional_config;
    world_without_optional_config.add_resource(CommandsQueue {});
    world_without_optional_config.add_resource(LuauTestTime {.delta = 0.5F});
    Entity optional_entity = world_without_optional_config.entity();
    world_without_optional_config.add_component(
        optional_entity,
        LuauTestPosition {.x = 3.0F}
    );
    world_without_optional_config.add_component(
        optional_entity,
        LuauTestVelocity {.x = 2.0F}
    );
    auto optional_systems = detail::install_luau_script_systems(
        world_without_optional_config,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(optional_systems.has_value());
    world_without_optional_config.run_schedule(Update);
    CHECK(
        world_without_optional_config
            .get_component<LuauTestPosition>(optional_entity)
            .x == 4.0F
    );
    CHECK(remove_luau_plugin_systems(
        world_without_optional_config,
        *optional_systems
    ));
    REQUIRE(runtime.unload_module(*module));
}

TEST_CASE(
    "Luau write queries mark only components whose values change",
    "[scripting_luau][system][query][change_detection]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "change_detection_system.luau",
        .content = R"(
            local function update(
                targets: Query<Write<LuauChangeDetectionTarget>>
            )
                for target in targets do
                    if target.operation == 0 then
                        local _value = target.value
                    elseif target.operation == 1 then
                        target.value = target.value
                    elseif target.operation == 2 then
                        target.value += 1
                    elseif target.operation == 3 then
                        target.position.x = target.position.x
                    elseif target.operation == 4 then
                        target.position.x += 1
                    elseif target.operation == 5 then
                        target.position:advance(0)
                    elseif target.operation == 6 then
                        target.position:advance(1)
                    elseif target.operation == 9 then
                        local position = target.position
                        position.x = position.x
                    elseif target.operation == 10 then
                        local position = target.position
                        position.x += 1
                    end
                end
            end

            local function update_world(world: World)
                local targets = world:query {
                    Write(LuauChangeDetectionTarget),
                }
                for target in targets do
                    if target.operation == 7 then
                        target.value = target.value
                    elseif target.operation == 8 then
                        target.position.x += 1
                    end
                end
            end

            export local ChangeDetectionPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(
                        MainSchedules.Update,
                        update,
                        update_world
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact.has_value());
    CHECK(
        std::ranges::any_of(
            artifact->property_paths,
            [](const LuauPropertyPathDecl& path) {
                return path.root_type == type_id<LuauChangeDetectionTarget>() &&
                       path.properties ==
                           std::vector<std::string> {"position", "x"};
            }
        )
    );
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    World world;
    world.add_resource(CommandsQueue {});
    std::array<Entity, 11> entities;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        entities[index] = world.entity();
        world.add_component(
            entities[index],
            LuauChangeDetectionTarget {
                .operation = static_cast<int>(index),
                .value = 10,
                .position = LuauTestPosition {.x = 20.0F},
            }
        );
    }

    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems.has_value());

    const auto changed_tick = [&world](Entity entity) {
        const auto location = world.entity_location(entity);
        REQUIRE(location.has_value());
        return world.archetypes()
            .get(location->archetype_id)
            .component_ticks(
                type_id<LuauChangeDetectionTarget>(),
                location->row
            )
            .changed;
    };
    std::array<Tick, 11> before;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        before[index] = changed_tick(entities[index]);
    }

    world.run_schedule(Update);

    for (const std::size_t index : {0U, 1U, 3U, 5U, 7U, 9U}) {
        CHECK(changed_tick(entities[index]) == before[index]);
    }
    for (const std::size_t index : {2U, 4U, 6U, 8U, 10U}) {
        CHECK(changed_tick(entities[index]) > before[index]);
    }
    CHECK(
        world.get_component<LuauChangeDetectionTarget>(entities[2]).value == 11
    );
    CHECK(
        world.get_component<LuauChangeDetectionTarget>(entities[4])
            .position.x == 21.0F
    );
    CHECK(
        world.get_component<LuauChangeDetectionTarget>(entities[6])
            .position.x == 21.0F
    );
    CHECK(
        world.get_component<LuauChangeDetectionTarget>(entities[8])
            .position.x == 21.0F
    );
    CHECK(
        world.get_component<LuauChangeDetectionTarget>(entities[10])
            .position.x == 21.0F
    );

    CHECK(remove_luau_plugin_systems(world, *systems));
    REQUIRE(runtime.unload_module(*module));
}

TEST_CASE(
    "Luau chunked queries handle tails, filters, and writes",
    "[scripting_luau][query][chunk]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "chunk_query_system.luau",
        .content = R"(
            local function update(
                config: ResRW<LuauTestConfig>,
                velocities: Query<Write<LuauTestVelocity>>
            )
                local count = 0
                local mutated = 0
                for velocity in velocities do
                    count += 1
                    if velocity.x % 2 == 0 then
                        continue
                    end
                    velocity.x += 100
                    mutated += 1
                end
                config.executions = count
                config.obstacle_total = mutated
            end

            export local ChunkQueryPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, update)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);

    World world;
    std::array<Entity, 65> entities;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        entities[index] = world.entity();
        world.add_component(
            entities[index],
            LuauTestVelocity {.x = static_cast<float>(index)}
        );
        if (index % 2 == 0) {
            world.add_component(entities[index], LuauTestPosition {});
        }
    }
    const auto excluded = world.entity();
    world.add_component(excluded, LuauTestVelocity {.x = 501.0F});
    world.add_component(excluded, LuauTestObstacle {});

    DynamicQuery query(
        "velocities",
        {DynamicQueryField {
            .name = "velocity",
            .type = type_id<LuauTestVelocity>(),
            .access = DynamicParamAccess::Write,
        }},
        {DynamicQueryFilter {
            .kind = DynamicQueryFilter::Kind::Without,
            .type = type_id<LuauTestObstacle>(),
        }}
    );
    auto query_ref = query.prepare(world);
    REQUIRE(query_ref);
    LuauTestConfig config;
    const std::array<Ref, 2> arguments {Ref(config), *query_ref};
    REQUIRE(runtime.call_module_function(*module, "update", arguments));

    CHECK(config.executions == 65);
    CHECK(config.obstacle_total == 32);
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const float expected =
            static_cast<float>(index) + (index % 2 == 0 ? 0.0F : 100.0F);
        CHECK(
            world.get_component<LuauTestVelocity>(entities[index]).x == expected
        );
    }
    CHECK(world.get_component<LuauTestVelocity>(excluded).x == 501.0F);
}

TEST_CASE(
    "Luau rejects read-only mutation and escaped ECS borrows",
    "[scripting_luau][borrow]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "borrow_system.luau",
        .content = R"(
            local escaped = nil
            local escaped_entity = nil

            local function mutate(config: ResRO<LuauTestConfig>)
                config.executions += 1
            end

            local function mutate_method(config: ResRO<LuauTestConfig>)
                config:add_execution(1)
            end

            local function capture(config: ResRO<LuauTestConfig>)
                escaped = config
            end

            local function use_escaped()
                return escaped.executions
            end

            local function capture_entity(commands: Commands)
                escaped_entity = commands:spawn()
            end

            local function use_escaped_entity()
                return escaped_entity:id()
            end

            local function mutate_query(
                velocities: Query<Read<LuauTestVelocity>>
            )
                for velocity in velocities do
                    velocity.x += 1
                end
            end

            local function retain_query(
                velocities: Query<Read<LuauTestVelocity>>
            )
                local retained = {}
                for velocity in velocities do
                    table.insert(retained, velocity)
                end
                assert(#retained == 2)
                assert(retained[1].x ~= retained[2].x)
            end

            local function capture_query(
                velocities: Query<Read<LuauTestVelocity>>
            )
                local readers = {}
                for velocity in velocities do
                    table.insert(readers, function()
                        return velocity.x
                    end)
                end
                assert(#readers == 2)
                assert(readers[1]() ~= readers[2]())
            end

            local function retain_nested_query(
                targets: Query<Read<LuauChangeDetectionTarget>>
            )
                local retained = {}
                for target in targets do
                    local position = target.position
                    table.insert(retained, position)
                end
                assert(#retained == 2)
                assert(retained[1].x ~= retained[2].x)
            end

            local function capture_nested_query(
                targets: Query<Read<LuauChangeDetectionTarget>>
            )
                local readers = {}
                for target in targets do
                    local position = target.position
                    table.insert(readers, function()
                        return position.x
                    end)
                end
                assert(#readers == 2)
                assert(readers[1]() ~= readers[2]())
            end

            export local BorrowPlugin = Plugin.new {
                build = function(app: App)
                    app:add_systems(
                        Update,
                        mutate,
                        mutate_method,
                        capture,
                        use_escaped,
                        capture_entity,
                        use_escaped_entity,
                        mutate_query,
                        retain_query,
                        capture_query,
                        retain_nested_query,
                        capture_nested_query
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(
        source,
        LuauCompileOptions {.snapshot_safe = false}
    );
    REQUIRE(artifact.has_value());
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module.has_value());

    const LuauTestConfig config {};
    const Ref config_ref {config};
    auto mutation = runtime.call_module_function(
        *module,
        "mutate",
        std::span<const Ref> {&config_ref, 1}
    );
    REQUIRE_FALSE(mutation.has_value());
    CHECK(mutation.error().message.find("read-only") != std::string::npos);

    auto method_mutation = runtime.call_module_function(
        *module,
        "mutate_method",
        std::span<const Ref> {&config_ref, 1}
    );
    REQUIRE_FALSE(method_mutation.has_value());

    REQUIRE(runtime.call_module_function(
        *module,
        "capture",
        std::span<const Ref> {&config_ref, 1}
    ));
    auto escaped = runtime.call_module_function(*module, "use_escaped");
    REQUIRE_FALSE(escaped.has_value());
    CHECK(
        escaped.error().message.find("expired ECS borrow") != std::string::npos
    );

    World commands_world;
    commands_world.add_resource(CommandsQueue {});
    Commands commands(commands_world.resource<CommandsQueue>(), commands_world);
    const Ref commands_ref {commands};
    REQUIRE(runtime.call_module_function(
        *module,
        "capture_entity",
        std::span<const Ref> {&commands_ref, 1}
    ));
    auto escaped_entity =
        runtime.call_module_function(*module, "use_escaped_entity");
    REQUIRE_FALSE(escaped_entity.has_value());
    CHECK(
        escaped_entity.error().message.find("expired EntityCommands") !=
        std::string::npos
    );

    World world;
    Entity entity = world.entity();
    world.add_component(entity, LuauTestVelocity {.x = 2.0F});
    DynamicQuery velocities(
        "velocities",
        {DynamicQueryField {
            .name = "velocity",
            .type = type_id<LuauTestVelocity>(),
            .access = DynamicParamAccess::Read,
        }},
        {}
    );
    auto query_ref = velocities.prepare(world);
    REQUIRE(query_ref.has_value());
    auto query_mutation = runtime.call_module_function(
        *module,
        "mutate_query",
        std::span<const Ref> {&*query_ref, 1}
    );
    REQUIRE_FALSE(query_mutation.has_value());
    CHECK(
        query_mutation.error().message.find("read-only") != std::string::npos
    );

    Entity other = world.entity();
    world.add_component(other, LuauTestVelocity {.x = 7.0F});
    query_ref = velocities.prepare(world);
    REQUIRE(query_ref.has_value());
    REQUIRE(runtime.call_module_function(
        *module,
        "retain_query",
        std::span<const Ref> {&*query_ref, 1}
    ));
    REQUIRE(runtime.call_module_function(
        *module,
        "capture_query",
        std::span<const Ref> {&*query_ref, 1}
    ));

    world.add_component(
        entity,
        LuauChangeDetectionTarget {
            .position = LuauTestPosition {.x = 3.0F},
        }
    );
    world.add_component(
        other,
        LuauChangeDetectionTarget {
            .position = LuauTestPosition {.x = 9.0F},
        }
    );
    DynamicQuery targets(
        "targets",
        {DynamicQueryField {
            .name = "target",
            .type = type_id<LuauChangeDetectionTarget>(),
            .access = DynamicParamAccess::Read,
        }},
        {}
    );
    auto target_ref = targets.prepare(world);
    REQUIRE(target_ref.has_value());
    REQUIRE(runtime.call_module_function(
        *module,
        "retain_nested_query",
        std::span<const Ref> {&*target_ref, 1}
    ));
    REQUIRE(runtime.call_module_function(
        *module,
        "capture_nested_query",
        std::span<const Ref> {&*target_ref, 1}
    ));
}

TEST_CASE(
    "Luau property cache follows reflected property replacement",
    "[scripting_luau][borrow][property][cache]"
) {
    auto& registry = Registry::instance();
    registry.register_cls<LuauPropertyCacheTarget>().add_property(
        "value",
        &LuauPropertyCacheTarget::first
    );
    const LuauScriptSource source {
        .name = "property_cache.luau",
        .content = R"(
            local function increment(
                target: ResRW<LuauPropertyCacheTarget>
            )
                target.value += 1
            end

            export local PropertyCachePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, increment)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);

    LuauPropertyCacheTarget target {.first = 1, .second = 10};
    Ref target_ref {target};
    REQUIRE(runtime.call_module_function(
        *module,
        "increment",
        std::span<const Ref> {&target_ref, 1}
    ));
    CHECK(target.first == 2);
    CHECK(target.second == 10);

    registry.register_cls<LuauPropertyCacheTarget>().add_property(
        "value",
        &LuauPropertyCacheTarget::second
    );
    REQUIRE(runtime.call_module_function(
        *module,
        "increment",
        std::span<const Ref> {&target_ref, 1}
    ));
    CHECK(target.first == 2);
    CHECK(target.second == 11);
}

TEST_CASE(
    "Luau RemovedComponents clear discards unread removals",
    "[scripting_luau][system][removed_components]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "removed_components.luau",
        .content = R"(
            local function discard_removed(
                removed: RemovedComponents<LuauTestPosition>,
                config: ResRW<LuauTestConfig>
            )
                removed:clear()
                for _entity in removed do
                    config.executions += 100
                end
                config.executions += 1
            end

            export local RemovedPlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(Update, discard_removed)
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);

    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});
    const auto entity = world.entity();
    world.add_component(entity, LuauTestPosition {});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    world.sort_systems();

    world.run_schedule(Update);
    REQUIRE(world.resource<LuauTestConfig>().executions == 1);
    world.remove_component<LuauTestPosition>(entity);
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().executions == 2);
}

TEST_CASE(
    "Luau system chains honor dependencies and run conditions",
    "[scripting_luau][system][schedule][chain]"
) {
    register_luau_system_test_types();
    const LuauScriptSource source {
        .name = "configured_schedule.luau",
        .content = R"(
            local function first(config: ResRW<LuauTestConfig>)
                config.schedule_order = config.schedule_order * 10 + 1
            end

            local function enabled(config: ResRO<LuauTestConfig>): boolean
                return config.schedule_enabled
            end

            local function second(config: ResRW<LuauTestConfig>)
                config.schedule_order = config.schedule_order * 10 + 2
            end

            local function third(config: ResRW<LuauTestConfig>)
                config.schedule_order = config.schedule_order * 10 + 3
            end

            export local SchedulePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(
                        Update,
                        chain(
                            first,
                            second:run_if(enabled),
                            third
                        )
                    )
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);

    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);

    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().schedule_order == 123);

    auto& config = world.resource<LuauTestConfig>();
    config.schedule_enabled = false;
    config.schedule_order = 0;
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().schedule_order == 13);
}

TEST_CASE(
    "Luau systems use reflected C++ states and state schedules",
    "[scripting_luau][system][state]"
) {
    register_luau_system_test_types();

    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});
    world.init_state(LuauTestMode::Idle);

    const LuauScriptSource source {
        .name = "state_schedule.luau",
        .content = R"(
            local function update_idle(
                config: ResRW<LuauTestConfig>,
                state: State<LuauTestMode>,
                next_state: NextState<LuauTestMode>
            )
                assert(state:get() == LuauTestMode.Idle)
                config.state_order = config.state_order * 10 + 1
                next_state:set(LuauTestMode.Active)
            end

            local function exit_idle(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 2
            end

            local function idle_to_active(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 3
            end

            local function enter_active(
                config: ResRW<LuauTestConfig>,
                state: State<LuauTestMode>
            )
                assert(state:get() == LuauTestMode.Active)
                config.state_order = config.state_order * 10 + 4
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:add_system(
                        Update,
                        update_idle:run_if(in_state(LuauTestMode.Idle))
                    )
                    app:add_system(OnExit(LuauTestMode.Idle), exit_idle)
                    app:add_system(
                        OnTransition(
                            LuauTestMode.Idle,
                            LuauTestMode.Active
                        ),
                        idle_to_active
                    )
                    app:add_system(
                        OnEnter(LuauTestMode.Active),
                        enter_active
                    )
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    std::string module_error;
    if (!module) {
        module_error = module.error().message;
    }
    INFO(module_error);
    REQUIRE(module);
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    world.sort_systems();

    world.run_state_transitions();
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().state_order == 1);

    world.run_state_transitions();
    CHECK(world.resource<State<LuauTestMode>>().get() == LuauTestMode::Active);
    CHECK(world.resource<LuauTestConfig>().state_order == 1234);

    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().state_order == 1234);
}

TEST_CASE(
    "Luau Plugins initialize and insert exported state types",
    "[scripting_luau][system][plugin][state]"
) {
    register_luau_system_test_types();
    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});

    const LuauScriptSource source {
        .name = "project://scripts/plugin_state.luau",
        .content = R"(
            export type GameFlow = "Boot" | "Running" | "Paused"

            local function update_boot(
                config: ResRW<LuauTestConfig>,
                state: State<GameFlow>,
                next_state: NextState<GameFlow>
            )
                assert(state:get() == GameFlow.Boot)
                config.state_order = config.state_order * 10 + 1
                next_state:set(GameFlow.Running)
            end

            local function exit_boot(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 2
            end

            local function enter_running(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 3
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameFlow.Boot)
                    app:add_system(
                        Update,
                        update_boot:run_if(in_state(GameFlow.Boot))
                    )
                    app:add_system(OnExit(GameFlow.Boot), exit_boot)
                    app:add_system(
                        OnEnter(GameFlow.Running),
                        enter_running
                    )
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    world.sort_systems();

    world.run_state_transitions();
    world.run_schedule(Update);
    world.run_state_transitions();
    CHECK(world.resource<LuauTestConfig>().state_order == 123);

    REQUIRE(remove_luau_plugin_systems(world, *systems));
    const LuauScriptSource inserted_source {
        .name = "project://scripts/plugin_state.luau",
        .content = R"(
            export type GameFlow = "Boot" | "Running" | "Paused"

            local function verify_paused(
                config: ResRW<LuauTestConfig>,
                state: State<GameFlow>
            )
                assert(state:get() == GameFlow.Paused)
                config.state_order = config.state_order * 10 + 4
            end

            local function enter_paused(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 5
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:insert_state(GameFlow.Paused)
                    app:add_system(Update, verify_paused)
                    app:add_system(
                        OnEnter(GameFlow.Paused),
                        enter_paused
                    )
                end,
            }
        )",
    };
    auto inserted_artifact = compile_luau_script_module(inserted_source);
    REQUIRE(inserted_artifact);
    auto inserted_module = runtime.load_module(*inserted_artifact);
    REQUIRE(inserted_module);
    auto inserted_systems = detail::install_luau_script_systems(
        world,
        runtime,
        *inserted_module,
        inserted_artifact->metadata->schema,
        inserted_artifact->plugins.front()
    );
    REQUIRE(inserted_systems);
    world.sort_systems();
    world.run_state_transitions();
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().state_order == 12354);
}

TEST_CASE(
    "Luau Plugins explicitly register dynamic event channels",
    "[scripting_luau][system][plugin][event]"
) {
    register_luau_system_test_types();
    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});

    const LuauScriptSource source {
        .name = "project://scripts/plugin_events.luau",
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
                events: EventReaderRO<DamageEvent>?,
                config: ResRW<LuauTestConfig>
            )
                assert(events ~= nil)
                for event in events do
                    config.executions += event.amount
                end
            end

            export local EventsPlugin = Plugin.new {
                build = function(app: App)
                    app:add_event(DamageEvent)
                    app:add_system(
                        Update,
                        chain(send_damage, read_damage)
                    )
                end,
            }
        )",
    };
    auto artifact = compile_luau_script_module(source);
    REQUIRE(artifact);
    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    REQUIRE(world.has_resource<DynamicEvents>());
    REQUIRE(world.resource<DynamicEvents>().registered(
        TypeId {artifact->metadata->schema.types[0].qualified_name}
    ));
    world.sort_systems();
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().executions == 3);
}

TEST_CASE(
    "Luau modules declare states without resetting them on reload",
    "[scripting_luau][system][state][reload]"
) {
    register_luau_system_test_types();
    World world;
    world.add_resource(CommandsQueue {});
    world.add_resource(LuauTestConfig {});

    const LuauScriptSource source {
        .name = "script_state.luau",
        .content = R"(
            export type GameFlow = "Boot" | "Running" | "Paused"

            local function enter_boot(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 1
            end

            local function update_boot(
                config: ResRW<LuauTestConfig>,
                state: State<GameFlow>,
                next_state: NextState<GameFlow>
            )
                assert(state:get() == GameFlow.Boot)
                config.state_order = config.state_order * 10 + 2
                next_state:set(GameFlow.Running)
            end

            local function exit_boot(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 3
            end

            local function boot_to_running(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 4
            end

            local function enter_running(config: ResRW<LuauTestConfig>)
                config.state_order = config.state_order * 10 + 5
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameFlow.Boot)
                    app:add_system(
                        Update,
                        update_boot:run_if(in_state(GameFlow.Boot))
                    )
                    app:add_system(OnEnter(GameFlow.Boot), enter_boot)
                    app:add_system(OnExit(GameFlow.Boot), exit_boot)
                    app:add_system(
                        OnTransition(GameFlow.Boot, GameFlow.Running),
                        boot_to_running
                    )
                    app:add_system(OnEnter(GameFlow.Running), enter_running)
                end,
            }
        )",
    };

    auto artifact = compile_luau_script_module(source);
    std::string artifact_error;
    if (!artifact) {
        artifact_error = artifact.error().message;
    }
    INFO(artifact_error);
    REQUIRE(artifact);
    REQUIRE(artifact->plugins.front().states.size() == 1);
    CHECK(artifact->plugins.front().states[0].name == "GameFlow");
    REQUIRE(artifact->plugins.front().states[0].values.size() == 3);

    LuauRuntime runtime;
    auto module = runtime.load_module(*artifact);
    REQUIRE(module);
    auto systems = detail::install_luau_script_systems(
        world,
        runtime,
        *module,
        artifact->metadata->schema,
        artifact->plugins.front()
    );
    REQUIRE(systems);
    world.sort_systems();

    world.run_state_transitions();
    CHECK(world.resource<LuauTestConfig>().state_order == 1);
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().state_order == 12);
    world.run_state_transitions();
    CHECK(world.resource<LuauTestConfig>().state_order == 12345);

    const LuauScriptSource reloaded_source {
        .name = "script_state.luau",
        .content = R"(
            export type GameFlow = "Paused" | "Running" | "Boot"

            local function verify_running(
                config: ResRW<LuauTestConfig>,
                state: State<GameFlow>
            )
                assert(state:get() == GameFlow.Running)
                config.state_order = config.state_order * 10 + 6
            end

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameFlow.Paused)
                    app:add_system(Update, verify_running)
                end,
            }
        )",
    };
    auto reloaded_artifact = compile_luau_script_module(reloaded_source);
    REQUIRE(reloaded_artifact);
    auto reloaded_module = runtime.load_module(*reloaded_artifact);
    REQUIRE(reloaded_module);
    auto reloaded_systems = detail::install_luau_script_systems(
        world,
        runtime,
        *reloaded_module,
        reloaded_artifact->metadata->schema,
        reloaded_artifact->plugins.front()
    );
    REQUIRE(reloaded_systems);
    REQUIRE(remove_luau_plugin_systems(world, *systems));
    world.sort_systems();
    world.run_schedule(Update);
    CHECK(world.resource<LuauTestConfig>().state_order == 123456);

    const LuauScriptSource invalid_reload {
        .name = "script_state.luau",
        .content = R"(
            export type GameFlow = "Boot" | "Paused"

            export local StatePlugin = Plugin.new {
                build = function(app: App)
                    app:init_state(GameFlow.Boot)
                end,
            }
        )",
    };
    auto invalid_artifact = compile_luau_script_module(invalid_reload);
    REQUIRE(invalid_artifact);
    auto invalid_module = runtime.load_module(*invalid_artifact);
    REQUIRE(invalid_module);
    auto invalid_systems = detail::install_luau_script_systems(
        world,
        runtime,
        *invalid_module,
        invalid_artifact->metadata->schema,
        invalid_artifact->plugins.front()
    );
    REQUIRE_FALSE(invalid_systems);
    CHECK(
        invalid_systems.error().message.find("currently active") !=
        std::string::npos
    );
}
