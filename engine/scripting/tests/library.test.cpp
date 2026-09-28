#include "scripting/library.hpp"

#include "detail/binding_internal.hpp"
#include "detail/library_scalar.hpp"
#include "refl/ref_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <lua.h>
#include <lualib.h>
#include <stdexcept>
#include <vector>

namespace ets {
std::span<const LuauLibraryDefinition>
luau_library_656e74697369756d2d6c7561752d66697874757265();
}

namespace {
struct Vm {
    lua_State* state {luaL_newstate()};
    std::unique_ptr<ets::LuauLibraries> libraries;
    explicit Vm(
        std::span<const ets::LuauLibraryDefinition> definitions,
        ets::LuauLibraryServices services = {}
    ) {
        luaL_openlibs(state);
        luaL_sandbox(state);
        libraries = std::make_unique<ets::LuauLibraries>(
            state,
            std::move(services),
            definitions
        );
    }
    ~Vm() {
        libraries->close();
        lua_close(state);
    }
};
} // namespace

TEST_CASE(
    "Generated native libraries validate values and isolate instances",
    "[luau][library]"
) {
    auto definitions = std::vector<ets::LuauLibraryDefinition>(
        ets::luau_library_656e74697369756d2d6c7561752d66697874757265().begin(),
        ets::luau_library_656e74697369756d2d6c7561752d66697874757265().end()
    );
    definitions.push_back(
        {.name = "@consumer",
         .source = R"(
        local fixture = require("@fixture")
        assert(fixture.add(2) == 2 and fixture.add(3) == 5)
        assert(fixture.add_value == nil)
        assert(fixture.echo("a\0b") == "a\0b")
        assert(fixture.optional(nil) == nil and fixture.optional() == nil)
        assert(fixture.optional(7) == 7)
        assert(not pcall(fixture.add, 1.5))
        assert(not pcall(fixture.add, "1"))
        assert(not pcall(fixture.add))
        assert(not pcall(fixture.add, 1, 2))
        local uok, uvalue = pcall(fixture.unsigned_value, -1)
        assert(not uok, tostring(uvalue))
        assert(not pcall(fixture.add, 2^53))
        assert(not pcall(fixture.add, math.huge))
        local ok, message = pcall(fixture.fail)
        assert(not ok and string.find(message, "fixture failure", 1, true))
        local character = fixture.character(65)
        assert(character == 65, "character: " .. tostring(character))
        local codepoint = fixture.codepoint(128512)
        assert(codepoint == 128512, "codepoint: " .. tostring(codepoint))
        local owned = fixture.owned()
        assert(fixture.inspect(owned) == 17 and fixture.inspect(owned) == 17)
        local record = fixture.record({name = "test", values = {1, 2, 3}, counts = {a = 4}})
        assert(type(record) == "userdata" and record.name == "test")
        assert(fixture.sum(record) == 6)
        local retained = fixture.retained()
        assert(retained.name == "retained" and fixture.sum(retained) == 3)
        assert(fixture.view("a\0b") == "a\0b")
        local value, err = fixture.checked(false)
        assert(value == nil and err == "expected failure")
        assert(fixture.checked(true) == 42)
        local output = fixture.readonly_record({name="copy", values={1,2}, counts={a=3}})
        assert(type(output) == "userdata" and output.counts.a == 3)
        assert(not pcall(function() output.name = "changed" end))
        assert(not pcall(function() output.values[1] = 4 end))
        assert(#record.values == 3 and record.values[1] == 1 and record.values[3] == 3)
        assert(record.values[4] == nil and record.values[0] == nil)
        assert(not pcall(function() return record.values[1.5] end))
        assert(record.counts.a == 4 and record.counts.missing == nil and #record.counts == 1)
        local sum = 0
        for index, item in record.values do sum += index * item end
        assert(sum == 14)
        for key, item in record.counts do assert(key == "a" and item == 4) end
        local count = 0
        for _ in fixture.record({}).values do count += 1 end
        assert(count == 0)
        local values = fixture.record({values={8,9}}).values
        assert(values[2] == 9)
        assert(not pcall(table.clone, values))
        local rows = fixture.readonly_records()
        assert(rows[1].name == "nested")
        assert(not pcall(function() rows[1].name = "changed" end))
        for _, row in rows do
            assert(not pcall(function() row.name = "changed" end))
        end
        assert(not pcall(fixture.record, {values={[2]=1}}))
        local okField, fieldError = pcall(fixture.record, {values={1,"bad"}})
        assert(not okField and string.find(fieldError, "values: [2]", 1, true))
        local cyclic = {}; cyclic.values = cyclic
        assert(not pcall(fixture.record, cyclic))
        assert(fixture.sum(fixture.record({values={4}})) == 4)
        assert(table.isfrozen(fixture))
        return { fixture = fixture, values = values }
    )",
         .dependencies = {"@fixture"}}
    );
    for (int i = 0; i < 2; ++i) {
        Vm vm(definitions);
        vm.libraries->require(vm.state, "@consumer");
        lua_gc(vm.state, LUA_GCCOLLECT, 0);
        const auto* first = lua_topointer(vm.state, -1);
        lua_getfield(vm.state, -1, "values");
        lua_pushinteger(vm.state, 2);
        lua_gettable(vm.state, -2);
        CHECK(lua_tointeger(vm.state, -1) == 9);
        lua_pop(vm.state, 2);
        vm.libraries->require(vm.state, "@consumer");
        CHECK(lua_topointer(vm.state, -1) == first);
        CHECK_THROWS(vm.libraries->require(vm.state, "@internal/fixture"));
    }
}

TEST_CASE(
    "Library loading uses stable environments and retries failures",
    "[luau][library]"
) {
    std::vector<ets::LuauLibraryDefinition> definitions {
        {.name = "@stable",
         .source = "assert(private_value == nil); return {}"},
        {.name = "@bad", .source = "error('load failure')"},
        {.name = "@undeclared", .source = "return require('@stable')"},
    };
    Vm vm(definitions);
    auto* importer = lua_newthread(vm.state);
    luaL_sandboxthread(importer);
    lua_pushinteger(importer, 42);
    lua_setglobal(importer, "private_value");
    CHECK_NOTHROW(vm.libraries->require(importer, "@stable"));
    for (int i = 0; i < 2; ++i) {
        const int top = lua_gettop(vm.state);
        CHECK_THROWS_WITH(
            vm.libraries->require(vm.state, "@bad"),
            "bad:1: load failure"
        );
        CHECK(lua_gettop(vm.state) == top);
    }
    CHECK_THROWS(vm.libraries->require(vm.state, "@undeclared"));
    vm.libraries->close();
    CHECK_THROWS(vm.libraries->require(vm.state, "@stable"));
}

TEST_CASE(
    "Library catalogs reject missing duplicate and cyclic dependencies",
    "[luau][library]"
) {
    auto* state = luaL_newstate();
    for (const auto& definitions :
         std::vector<std::vector<ets::LuauLibraryDefinition>> {
             {{.name = "@a"}, {.name = "@a"}},
             {{.name = "@a", .dependencies = {"@missing"}}},
             {{.name = "@a", .dependencies = {"@b"}},
              {.name = "@b", .dependencies = {"@a"}}},
         }) {
        CHECK_THROWS(ets::LuauLibraries(state, {}, definitions));
    }
    lua_close(state);
}

TEST_CASE(
    "Native instances outlive userdata even after a failed open",
    "[luau][library]"
) {
    struct Counts {
        int opened = 0;
        int cancelled = 0;
        int finalized = 0;
        int destroyed = 0;
    } counts;
    struct Native final : ets::LuauNativeLibrary {
        Counts& counts;
        explicit Native(Counts& value) : counts(value) {}
        ~Native() override { ++counts.destroyed; }
        void cancel() noexcept override { ++counts.cancelled; }
        void open(lua_State* state) override {
            ++counts.opened;
            lua_newtable(state);
            auto** owner = static_cast<Native**>(
                lua_newuserdatadtor(state, sizeof(Native*), [](void* data) {
                    auto* instance = *static_cast<Native**>(data);
                    ++instance->counts.finalized;
                })
            );
            *owner = this;
            lua_setfield(state, -2, "owned");
            if (counts.opened == 1) {
                throw std::runtime_error("open failure");
            }
        }
    };
    auto* state = luaL_newstate();
    luaL_openlibs(state);
    luaL_sandbox(state);
    {
        const std::vector<ets::LuauLibraryDefinition> definitions {
            {.name = "@lifetime", .create = [&](auto&) {
                 return std::make_unique<Native>(counts);
             }}
        };
        ets::LuauLibraries libraries(state, {}, definitions);
        CHECK_THROWS(libraries.require(state, "@lifetime"));
        CHECK(counts.cancelled == 1);
        CHECK_NOTHROW(libraries.require(state, "@lifetime"));
        CHECK(counts.opened == 2);
        libraries.close();
        CHECK(counts.destroyed == 0);
        lua_close(state);
        CHECK(counts.finalized == 2);
        CHECK(counts.destroyed == 0);
    }
    CHECK(counts.destroyed == 2);
}

TEST_CASE(
    "Native model and connection state remain VM local",
    "[luau][library][ai]"
) {
    auto definitions = std::vector<ets::LuauLibraryDefinition>(
        ets::default_luau_libraries().begin(),
        ets::default_luau_libraries().end()
    );
    definitions.push_back(
        {.name = "@probe",
         .source = R"(
        local ai = require('@internal/ai')
        local http = require('@internal/http')
        local json = require('@json')
        local model = ai.model('generation', 'planner')
        local connection = http.connection(model.connection)
        assert(type(connection) == 'userdata')
        assert(not pcall(http.close, {}))
        assert(not pcall(http.submit, {connection = {}, path = '/'}))
        assert(not pcall(json.encode, connection))
        assert(not pcall(function() return model.headers end))
        assert(not pcall(function() return model.apiKey end))
        assert(http.connection(model.connection) == connection)
        http.close(connection)
        assert(not pcall(http.connection, model.connection))
        return { id = model.id }
    )",
         .dependencies = {"@internal/ai", "@internal/http", "@json"}}
    );
    for (const auto* id : {"first", "second"}) {
        auto config = std::make_shared<ets::LuauHostConfig>();
        config->connections["test"].base_url = "http://127.0.0.1:9";
        config->generation["planner"] =
            {.id = id, .protocol = "chat-completions", .connection = "test"};
        Vm vm(definitions, {.config = config});
        vm.libraries->require(vm.state, "@probe");
        lua_getfield(vm.state, -1, "id");
        CHECK(std::string(lua_tostring(vm.state, -1)) == id);
    }
}

TEST_CASE(
    "Reflected numeric arguments reject negative unsigned values",
    "[luau][library]"
) {
    auto* state = luaL_newstate();
    lua_pushnumber(state, -1);
    CHECK_THROWS(ets::LuauScalarCodec<unsigned int>::read(state, 1));
    auto value = ets::detail::luau_value_for_type(
        state,
        1,
        ets::type_id<unsigned int>()
    );
    CHECK_FALSE(value);
    lua_close(state);
}

TEST_CASE(
    "Reflected container iterators reject expired borrows",
    "[luau][library]"
) {
    using namespace ets;
    Vm vm({});
    detail::install_luau_borrowed_object_metatable(vm.state);
    std::vector<int> values {4, 5};
    LuauBorrowScope scope;
    auto token = scope.begin();
    detail::push_luau_borrowed_ref(vm.state, make_ref(values), scope, token);
    lua_pushcfunction(vm.state, detail::luau_borrowed_iter, "iterate");
    lua_pushvalue(vm.state, 1);
    REQUIRE(lua_pcall(vm.state, 1, 1, 0) == LUA_OK);
    lua_pushvalue(vm.state, -1);
    REQUIRE(lua_pcall(vm.state, 0, 2, 0) == LUA_OK);
    CHECK(lua_tointeger(vm.state, -2) == 1);
    CHECK(lua_tointeger(vm.state, -1) == 4);
    lua_pop(vm.state, 2);
    scope.end(token);
    REQUIRE(lua_pcall(vm.state, 0, 2, 0) != LUA_OK);
    CHECK(
        std::string(lua_tostring(vm.state, -1)).find("expired ECS borrow") !=
        std::string::npos
    );
}
