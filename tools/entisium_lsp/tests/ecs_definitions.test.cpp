#include "Flags.hpp"
#include "Luau/Ast.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/ConfigResolver.h"
#include "Luau/ExperimentalFlags.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

class MemoryFileResolver final : public Luau::FileResolver {
  public:
    std::unordered_map<Luau::ModuleName, std::string> sources;

    std::optional<Luau::ModuleInfo> resolveModule(
        const Luau::ModuleInfo*,
        Luau::AstExpr* expression,
        const Luau::TypeCheckLimits&
    ) override {
        if (const auto* name = expression->as<Luau::AstExprConstantString>()) {
            return Luau::ModuleInfo {
                std::string(name->value.data, name->value.size)
            };
        }
        return std::nullopt;
    }

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
};

std::string runtime_definitions() {
    std::ifstream input(
        std::filesystem::path {ETS_PROJECT_ROOT} /
            "tools/luau_defgen/entisium-runtime.d.luau",
        std::ios::binary
    );
    REQUIRE(input);
    return {
        std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {},
    };
}

std::vector<unsigned int>
error_lines(std::string source, const std::string_view extra_definitions = {}) {
    for (auto* flag = Luau::FValue<bool>::list; flag != nullptr;
         flag = flag->next) {
        if (std::strncmp(flag->name, "Luau", 4) == 0 &&
            !Luau::isAnalysisFlagExperimental(flag->name)) {
            flag->value = true;
        }
    }
    applyRequiredFlags();

    MemoryFileResolver files;
    std::ifstream catalog_file(ETS_TEST_LUAU_CATALOG);
    REQUIRE(catalog_file);
    const auto catalog = nlohmann::json::parse(catalog_file);
    for (const auto& library : catalog.at("libraries")) {
        const auto name = library.at("name").get<std::string>();
        const auto source = library.at("source").get<std::string>();
        if (!source.empty()) {
            files.sources[name] = source;
        }
        const auto native = library.at("native").get<std::string>();
        if (!native.empty()) {
            const auto alias = library.at("source").get<std::string>().empty() ?
                                   name :
                                   "@internal/" + name.substr(1);
            files.sources[alias] = native;
        }
    }
    files.sources.emplace("ecs-test", std::move(source));
    files.sources.emplace(
        "@internal/task",
        "local native: {now: () -> number} = (nil :: any); return native"
    );
    Luau::NullConfigResolver configs;
    configs.defaultConfig.mode = Luau::Mode::Strict;
    Luau::Frontend frontend {
        Luau::SolverMode::New,
        &files,
        &configs,
    };
    Luau::registerBuiltinGlobals(frontend, frontend.globals);

    auto definitions = runtime_definitions();
    for (const auto& library : catalog.at("libraries")) {
        definitions += "\n" + library.value("globals", std::string {}) + "\n";
    }
    definitions += R"(
        declare extern type TestTransform with
            position: number
        end
        declare TestTransform: {
            __ets_type: TestTransform?,
        }
    )";
    definitions += extra_definitions;
    const auto loaded = frontend.loadDefinitionFile(
        frontend.globals,
        frontend.globals.globalScope,
        definitions,
        "@entisium",
        false
    );
    REQUIRE(loaded.success);

    const auto result = frontend.check("ecs-test");
    std::vector<unsigned int> lines;
    lines.reserve(result.errors.size());
    for (const auto& error : result.errors) {
        UNSCOPED_INFO(error.moduleName << ":" << error.location.begin.line + 1);
        UNSCOPED_INFO(Luau::toString(error));
        lines.push_back(error.location.begin.line);
    }
    return lines;
}

} // namespace

TEST_CASE("Luau library sources pass strict checking", "[lsp][libraries]") {
    CHECK(error_lines(R"(
        local json = require("@json")
        local http = require("@http")
        local schema = require("@schema")
        local task = require("@task")
        local core = require("@context/core")
        local context = require("@context")
        local game = require("@playtest/game")
        local ai = require("@ai")
    )")
              .empty());
}

TEST_CASE(
    "playtest resource reads infer the imported value type",
    "[lsp][definitions][playtest]"
) {
    CHECK(error_lines(R"(
        local game = require("@playtest/game")
        local state = game:read_resource(TestTransform)
        local position: number = state.position
    )")
              .empty());
    CHECK_FALSE(error_lines(R"(
        local game = require("@playtest/game")
        local state = game:read_resource(TestTransform)
        local wrong: string = state.position
    )")
                    .empty());
}

TEST_CASE(
    "runtime definitions type ECS system parameters",
    "[lsp][definitions][ecs]"
) {
    const auto errors = error_lines(R"(
        type TestEvent = { value: number }

        local function update(
            query: Query<Entity, Write<TestTransform>, With<TestTransform>>,
            writer: EventWriter<TestEvent>,
            state: State<"idle" | "running">,
            next_state: NextState<"idle" | "running">,
            removed: RemovedComponents<TestTransform>,
            commands: Commands
        )
            for entity, transform in query do
                local entity_id: number = entity
                local position: number = transform.position
                writer({ value = position })
                writer:send({ value = entity_id })
            end

            for removed_entity in removed do
                commands:entity(removed_entity):despawn()
            end

            if state:get() == "idle" then
                next_state:set("running")
            end
        end

        local configured = system(update):run_if(in_state("idle"))
        local chained = chain(configured, update)
        local function inspect_world(world: World)
            local transform = world:resource(TestTransform)
            if transform then
                local position: number = transform.position
            end
        end
        local plugin = Plugin.new {
            build = function(app: App)
                app:add_systems(MainSchedules.Update, configured, chained)
                app:add_system(OnEnter("idle"), update)
            end,
        }
    )");

    CHECK(errors.empty());
}

TEST_CASE(
    "runtime definitions preserve query item types",
    "[lsp][definitions][ecs]"
) {
    const auto errors = error_lines(R"(
        type TestEvent = { value: number }

        local function invalid(query: Query<Entity, Write<TestTransform>>)
            local _, first = query:first()
            local invalid_first: string = first.position
            for _, transform in query do
                local invalid_position: string = transform.position
            end
        end

        local direct = nil :: Write<TestTransform>?
        if direct then
            local invalid_direct: string = direct.position
        end

        local function invalid_world(world: World)
            local transform = world:resource(TestTransform)
            if transform then
                local invalid_resource: string = transform.position
            end
        end

        local function invalid_params(
            writer: EventWriter<TestEvent>,
            next_state: NextState<"idle" | "running">
        )
            writer({ value = "invalid" })
            next_state:set("paused")
        end

    )");

    CHECK(errors == std::vector<unsigned int> {5, 7, 13, 19, 27, 28});
}

TEST_CASE(
    "runtime definitions infer dependent asset handle types",
    "[lsp][definitions][asset]"
) {
    const auto errors = error_lines(
        R"(
        local function load_transform(assets: TestAssetServer)
            local handle: Handle<TestTransform> = assets:load(
                TestTransform,
                "transform.asset"
            )
        end
    )",
        R"(
        declare extern type TestAssetServer with
            load: <T>(
                self: TestAssetServer,
                type_token: TypeToken<T>,
                path: string
            ) -> Handle<T>
        end
    )"
    );

    CHECK(errors.empty());
}

TEST_CASE(
    "context definitions preserve named query and projection types",
    "[lsp][definitions][context]"
) {
    const auto errors = error_lines(R"(
        local context = require("@context")
        local resource = context.resource(TestTransform)
        local rows = context.query { transform = TestTransform, other = TestTransform }
        local mapped = resource:map(function(p: TestTransform): number return p.position end)
        local invalidMapped: string = mapped.__context_value
        local invalidQuery: string = rows.__context_value[1].transform.position
        local view = context.view { fields = {
            nested = context.combine { position = mapped },
            rows = rows:map(function(row: { entity: number, transform: TestTransform, other: TestTransform }): number return row.transform.position + row.other.position end),
        } }
        local sample = view:sample()
        local good: number = sample.data.nested.position
        local bad: string = sample.data.nested.position
        local badRow: string = sample.data.rows[1]
    )");
    CHECK(errors == std::vector<unsigned int> {5, 6, 13, 14});
}

TEST_CASE(
    "decision tasks retain typed answers through await",
    "[lsp][definitions][decisions]"
) {
    const auto errors = error_lines(R"(
        local ai = require("@ai")
        local task = require("@task")
        local policy = ai.decision { questions = { move = ai.choice { instructions = "move", criteria = { left = "left", right = "right" } } } }
        local result = task.timeout(policy:evaluate({ encode = function(self: any): string return "{}" end }), 2):await()
        local answer = result.answers.move
        if answer.type == "choice" then
            local bad: string = answer.confidence
        end
    )");
    CHECK(errors == std::vector<unsigned int> {7});
}

TEST_CASE(
    "schema definitions infer nested parsed values",
    "[lsp][definitions][schema]"
) {
    const auto errors = error_lines(R"(
        local schema = require("@schema")
        local shape = schema.object { name = schema.string(), score = schema.optional(schema.number()), rows = schema.array(schema.object { active = schema.boolean() }) }
        local parsed = shape:describe("test"):parse({})
        local good: string = parsed.name
        local optional: number? = parsed.score
        local nested: boolean = parsed.rows[1].active
        local bad: number = parsed.name
        local badNested: string = parsed.rows[1].active
        local result = shape:safe_parse({})
        if result.success then
            local badResult: number = result.data.name
        else
            local message: string = result.issues[1].message
        end
    )");
    CHECK(errors == std::vector<unsigned int> {7, 8, 11});
}

TEST_CASE(
    "LLM generation preserves schema output types through await",
    "[lsp][definitions][llm]"
) {
    const auto errors = error_lines(R"(
        local ai = require("@ai")
        local schema = require("@schema")
        local task = require("@task")
        local model = ai.model { model = "planner", output = schema.object { active = schema.boolean(), target = schema.optional(schema.number()) } }
        local result = model:generate { prompt = "Choose" }:await()
        local good: boolean = result.data.active
        local optional: number? = result.data.target
        local bad: string = result.data.active
        local text = ai.model { model = "planner" }
        local summary: string = text:generate { prompt = "Summarize" }:await().data
        local badText: number = text:generate { prompt = "Summarize" }:await().data
    )");
    CHECK(errors == std::vector<unsigned int> {8, 11});
}

TEST_CASE(
    "task handles retain values across spawn and timeout",
    "[lsp][definitions][task]"
) {
    const auto errors = error_lines(R"(
        local task = require("@task")
        local worker = task.spawn(function(): number return 42 end)
        local good: number = task.timeout(worker, 1):await()
        local bad: string = worker:await()
        local status: string = worker:status()
        local source = task.completion() :: task.TaskCompletion<number>
        source.resolve(12)
        local event: number = source.task:await()
        local wrong: boolean = source.task:await()
    )");
    CHECK(errors == std::vector<unsigned int> {4, 9});
}

TEST_CASE(
    "task groups retain named values and race discriminants",
    "[lsp][definitions][task]"
) {
    const auto errors = error_lines(R"(
        local task = require("@task")
        local jobs = { count = task.spawn(function(): number return 42 end), text = task.spawn(function(): string return "done" end) }
        local results = task.all(jobs):await()
        local good: number = results.count
        local bad: boolean = results.text
        local winner = task.race(jobs):await()
        if winner.kind == "count" then
            local count: number = winner.value
            local wrong: string = winner.value
        end
        local typed: task.Task<number> = jobs.count
    )");
    CHECK(errors == std::vector<unsigned int> {5, 9});
}

TEST_CASE(
    "HTTP responses retain their types through Tasks",
    "[lsp][definitions][http]"
) {
    const auto errors = error_lines(R"(
        local http = require("@http")
        local response = http.request({url = "https://example.com"}):await()
        local good: string = response.body
        local bad: number = response.body
        http.request({url = "https://example.com", timeout = "bad"})
    )");
    CHECK(errors == std::vector<unsigned int> {4, 5});
}

TEST_CASE(
    "JSON types preserve unknown decoding and array elements",
    "[lsp][json]"
) {
    const auto errors = error_lines(R"(
        local json = require("@json")
        local text: string = json.encode({value = json.null})
        local null: json.Null = json.null
        local values = json.array({1, 2})
        local good: number = values[1]
        local bad: string = values[1]
        local decoded: string = json.decode(text)
    )");
    CHECK(errors == std::vector<unsigned int> {6, 7});
}

TEST_CASE(
    "HTTP response containers expose reflected access",
    "[lsp][libraries]"
) {
    CHECK(error_lines(R"(
        local http = require("@http")
        local function inspect(response: http.Response)
            local count: number = #response.headers
            local name: string = response.headers[1].name
            for index, header in response.headers do
                local key: number = index
                local value: string = header.value
            end
        end
    )")
              .empty());
    CHECK_FALSE(error_lines(R"(
        local http = require("@http")
        local function mutate(response: http.Response)
            local wrong: number = response.headers[1].name
        end
    )")
                    .empty());
}

TEST_CASE(
    "Generated native contracts preserve erased type relations and opaque "
    "handles",
    "[lsp][libraries]"
) {
    CHECK(error_lines(R"(
        local bridge = require("@context/bridge")
        local http = require("@http")
        local json = require("@json")
        local state = bridge.decode_snapshot(TestTransform, "{}")
        local copied = bridge.copy_value(state)
        local position: number = copied.position
        local words: {string} = json.array({"one"})
        local connection: http.Connection = http.connection("test")
        http.request({connection = connection, path = "/"})
        http.request({url = "http://example.test"})
    )")
              .empty());
    CHECK_FALSE(error_lines(R"(
        local bridge = require("@context/bridge")
        local state = bridge.copy_value(bridge.decode_snapshot(TestTransform, "{}"))
        local wrong: string = state.position
    )")
                    .empty());
    CHECK_FALSE(error_lines(R"(
        local http = require("@http")
        local forged: http.Connection = {}
        http.close(forged)
    )")
                    .empty());
    CHECK_FALSE(error_lines(R"(
        local native = require("@internal/http")
        native.submit({connection = native.connection("test")})
    )")
                    .empty());
}
