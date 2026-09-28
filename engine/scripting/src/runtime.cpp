#include "scripting/runtime.hpp"

#include "ecs/dynamic/state.hpp"
#include "ecs/dynamic/world.hpp"
#include "refl/annotations.hpp"
#include "refl/enum.hpp"
#include "refl/registry.hpp"
#include "refl/type.hpp"
#include "scripting/detail/binding.hpp"
#include "scripting/detail/json_value.hpp"
#include "scripting/detail/reflection_bridge.hpp"
#include "scripting/detail/state.hpp"
#include "scripting/library.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp> // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ets {
namespace {

using Json = nlohmann::json;

using detail::luau_json;
using detail::push_json;
int absolute_index(lua_State* state, int index) {
    return lua_absindex(state, index);
}

std::string required_string(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error(
            std::string("playtest field '") + field + "' must be a string"
        );
    }
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    std::string result(value, size);
    lua_pop(state, 1);
    return result;
}

std::string optional_string(
    lua_State* state,
    int table,
    const char* field,
    std::string fallback
) {
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return fallback;
    }
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error(
            std::string("playtest field '") + field + "' must be a string"
        );
    }
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    std::string result(value, size);
    lua_pop(state, 1);
    return result;
}

uint32
optional_uint(lua_State* state, int table, const char* field, uint32 fallback) {
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return fallback;
    }
    const auto value = lua_tonumber(state, -1);
    const bool valid =
        lua_isnumber(state, -1) && std::trunc(value) == value && value >= 1.0 &&
        value <= static_cast<double>(std::numeric_limits<uint32>::max());
    lua_pop(state, 1);
    if (!valid) {
        throw std::runtime_error(
            std::string("playtest ticks field '") + field +
            "' must be a positive 32-bit integer"
        );
    }
    return static_cast<uint32>(value);
}

bool optional_bool(
    lua_State* state,
    int table,
    const char* field,
    bool fallback
) {
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return fallback;
    }
    if (!lua_isboolean(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error(
            std::string("playtest ticks field '") + field +
            "' must be a boolean"
        );
    }
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
}

std::string schema_json(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error(
            std::string("playtest field '") + field +
            "' must be a JSON Schema table"
        );
    }
    auto result = luau_json(state, -1, field).dump();
    lua_pop(state, 1);
    return result;
}

int callback_ref(
    lua_State* state,
    int table,
    const char* field,
    bool required
) {
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1) && !required) {
        lua_pop(state, 1);
        return 0;
    }
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error(
            std::string("playtest field '") + field + "' must be a function"
        );
    }
    const int result = lua_ref(state, -1);
    lua_pop(state, 1);
    return result;
}

struct ParsedPlaytest {
    LuauPlaytestDeclaration declaration;
    int begin_step {0};
    int end_step {0};
    int observe {0};
};

ParsedPlaytest parse_playtest(lua_State* state, int index) {
    if (!lua_istable(state, index)) {
        throw std::runtime_error("app:add_playtest expects a table");
    }
    const int table = absolute_index(state, index);
    auto id = required_string(state, table, "id");
    auto label = optional_string(state, table, "label", id);
    auto description = optional_string(
        state,
        table,
        "description",
        "Project playtest interface " + id
    );
    uint32 decision_ticks = 1;
    uint32 minimum_ticks = 1;
    uint32 maximum_ticks = 1;
    bool override_ticks = false;
    lua_getfield(state, table, "ticks");
    if (!lua_isnil(state, -1)) {
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            throw std::runtime_error("playtest field 'ticks' must be a table");
        }
        const int ticks = absolute_index(state, -1);
        decision_ticks = optional_uint(state, ticks, "default", 1);
        minimum_ticks = optional_uint(state, ticks, "min", decision_ticks);
        maximum_ticks = optional_uint(state, ticks, "max", decision_ticks);
        override_ticks = optional_bool(state, ticks, "overridable", false);
    }
    lua_pop(state, 1);
    auto action_schema = schema_json(state, table, "action");
    std::string observation_schema =
        R"({"type":"object","additionalProperties":false})";
    lua_getfield(state, table, "observation");
    const bool has_observation = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (has_observation) {
        observation_schema = schema_json(state, table, "observation");
    }
    return ParsedPlaytest {
        .declaration =
            LuauPlaytestDeclaration {
                .id = std::move(id),
                .label = std::move(label),
                .description = std::move(description),
                .decision_ticks = decision_ticks,
                .minimum_ticks = minimum_ticks,
                .maximum_ticks = maximum_ticks,
                .allow_tick_override = override_ticks,
                .action_schema_json = std::move(action_schema),
                .observation_schema_json = std::move(observation_schema),
            },
        .begin_step = callback_ref(state, table, "begin_step", true),
        .end_step = callback_ref(state, table, "end_step", false),
        .observe = callback_ref(state, table, "observe", false),
    };
}

void install_system_config_metatables(lua_State* state);

std::vector<DynamicSystemParamDeclPtr> clone_param_declarations(
    const std::vector<DynamicSystemParamDeclPtr>& declarations
) {
    std::vector<DynamicSystemParamDeclPtr> result;
    result.reserve(declarations.size());
    for (const auto& declaration : declarations) {
        result.push_back(declaration->clone());
    }
    return result;
}

} // namespace

struct LuauRuntime::Impl {
    struct Module {
        struct PlaytestCallbacks {
            int begin_step {0};
            int end_step {0};
            int observe {0};
        };

        lua_State* thread {nullptr};
        int thread_ref {0};
        int exports_ref {0};
        std::unordered_map<std::string, int> functions;
        std::unordered_map<const void*, std::size_t> function_indices;
        std::vector<LuauFunctionDecl> function_declarations;
        std::unordered_map<std::string, LuauScriptModuleId> imports;
        std::unordered_set<std::string> script_namespace_roots;
        std::unordered_set<TypeId> script_types;
        std::vector<LuauPlaytestDeclaration> playtests;
        std::vector<PlaytestCallbacks> playtest_callbacks;
        bool script_namespaces_sealed {false};
    };

    lua_State* state {nullptr};
    std::unique_ptr<LuauLibraries> libraries;
    std::unordered_map<LuauScriptModuleId, Module> modules;
    std::unordered_map<std::string, int> native_modules;
    std::uint64_t next_module_id {1};
    LuauBorrowScope borrow_scope;
    lua_State* task {nullptr};
    int task_ref {0};
    bool task_started {false};
    uint32 interrupt_limit {0};
    uint32 interrupts_left {0};

    Result<int, std::string>
    native_module_ref(lua_State* thread, const std::string& specifier) {
        if (specifier.starts_with("@internal/")) {
            return failure(std::string("Private Luau library: ") + specifier);
        }
        if (const auto cached = native_modules.find(specifier);
            cached != native_modules.end()) {
            return cached->second;
        }

        if (libraries->contains(specifier)) {
            try {
                libraries->require(thread, specifier);
                const int reference = lua_ref(thread, -1);
                lua_pop(thread, 1);
                native_modules.emplace(specifier, reference);
                return reference;
            } catch (const std::exception& error) {
                return failure(std::string(error.what()));
            }
        }
        constexpr std::string_view prefix = "@entisium/";
        if (!specifier.starts_with(prefix) ||
            specifier.size() == prefix.size()) {
            return failure(
                "Invalid native Luau module specifier '" + specifier + "'"
            );
        }
        const std::string_view module_name =
            std::string_view {specifier}.substr(prefix.size());
        const int base_top = lua_gettop(thread);
        lua_newtable(thread);
        const int exports = lua_absindex(thread, -1);
        std::size_t export_count = 0;
        for (const TypeId id :
             Registry::instance()
                 .types_with_annotation<annotations::ScriptModule>()) {
            auto type = Registry::instance().try_get_type(id);
            if (!type || !is_luau_visible(*type)) {
                continue;
            }
            const auto annotation =
                type->annotation<annotations::ScriptModule>();
            if (!annotation || annotation->name != module_name) {
                continue;
            }

            const std::string name {type->local_name()};
            lua_getfield(thread, exports, name.c_str());
            const bool occupied = !lua_isnil(thread, -1);
            lua_pop(thread, 1);
            if (occupied) {
                lua_settop(thread, base_top);
                return failure(
                    "Native Luau module '" + specifier +
                    "' has duplicate export '" + name + "'"
                );
            }

            if (auto reflected_enum = Registry::instance().try_get_enum(id)) {
                lua_newtable(thread);
                for (const auto& [enumerator, underlying_value] :
                     reflected_enum->enumerators()) {
                    detail::push_luau_owned_value(
                        thread,
                        reflected_enum->make_val(underlying_value)
                    );
                    lua_setfield(thread, -2, enumerator.c_str());
                }
                lua_setreadonly(thread, -1, true);
            } else {
                detail::push_luau_type_token(thread, id);
            }
            lua_setfield(thread, exports, name.c_str());
            ++export_count;
        }
        if (export_count == 0) {
            lua_settop(thread, base_top);
            return failure(
                "Native Luau module '" + specifier + "' is not available"
            );
        }
        lua_setreadonly(thread, exports, true);
        const int reference = lua_ref(thread, exports);
        lua_pop(thread, 1);
        native_modules.emplace(specifier, reference);
        return reference;
    }

    static int require_module(lua_State* thread) {
        auto* impl =
            static_cast<Impl*>(lua_touserdata(thread, lua_upvalueindex(1)));
        const auto* importer_id = static_cast<const LuauScriptModuleId*>(
            lua_touserdata(thread, lua_upvalueindex(2))
        );
        std::size_t length = 0;
        const char* value = luaL_checklstring(thread, 1, &length);
        const std::string specifier {value, length};
        if (is_native_luau_module(specifier)) {
            auto native = impl->native_module_ref(thread, specifier);
            if (!native) {
                luaL_error(thread, "%s", native.error().c_str());
                return 0;
            }
            lua_getref(thread, *native);
            return 1;
        }
        if (importer_id == nullptr) {
            luaL_error(thread, "Luau importer module is not loaded");
            return 0;
        }
        const auto importer = impl->modules.find(*importer_id);
        if (importer == impl->modules.end()) {
            luaL_error(thread, "Luau importer module is not loaded");
            return 0;
        }
        const auto binding = importer->second.imports.find(specifier);
        if (binding == importer->second.imports.end()) {
            luaL_error(
                thread,
                "Luau module has no static import binding for '%s'",
                specifier.c_str()
            );
            return 0;
        }
        const auto dependency = impl->modules.find(binding->second);
        if (dependency == impl->modules.end() ||
            dependency->second.exports_ref == 0) {
            luaL_error(
                thread,
                "Required Luau module '%s' is not loaded",
                specifier.c_str()
            );
            return 0;
        }
        lua_getref(thread, dependency->second.exports_ref);
        return 1;
    }

    void install_imports(
        LuauScriptModuleId id,
        Module& module,
        std::span<const LuauScriptImportBinding> imports
    ) {
        for (const auto& import : imports) {
            module.imports.emplace(import.specifier, import.module);
        }
        lua_pushlightuserdata(module.thread, this);
        auto* importer_id = static_cast<LuauScriptModuleId*>(
            lua_newuserdata(module.thread, sizeof(LuauScriptModuleId))
        );
        *importer_id = id;
        lua_pushcclosure(module.thread, require_module, "require", 2);
        lua_setglobal(module.thread, "require");
    }

    explicit Impl(std::shared_ptr<const LuauHostConfig> config) :
        state(luaL_newstate()) {
        luaL_openlibs(state);
        detail::install_luau_borrowed_object_metatable(state);
        install_system_config_metatables(state);
        luaL_sandbox(state);
        libraries = std::make_unique<LuauLibraries>(
            state,
            LuauLibraryServices {.config = std::move(config)}
        );
    }

    ~Impl() {
        if (state) {
            libraries->close();
            lua_close(state);
        }
    }
};

namespace {

char c_script_namespace_marker;
char c_system_config_marker;
char c_system_config_metatable;
char c_system_chain_marker;
char c_state_condition_marker;

bool is_system_config(lua_State* state, int index) {
    if (!lua_istable(state, index)) {
        return false;
    }
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &c_system_config_marker);
    lua_rawget(state, index);
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
}

bool is_system_chain(lua_State* state, int index) {
    if (!lua_istable(state, index)) {
        return false;
    }
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &c_system_chain_marker);
    lua_rawget(state, index);
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
}

bool is_state_condition(lua_State* state, int index) {
    if (!lua_istable(state, index)) {
        return false;
    }
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &c_state_condition_marker);
    lua_rawget(state, index);
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
}

void push_system_config_metatable(lua_State* state) {
    lua_pushlightuserdata(state, &c_system_config_metatable);
    lua_rawget(state, LUA_REGISTRYINDEX);
}

int system_config_method(lua_State* state) {
    const int call_argument_count = lua_gettop(state);
    const int method = lua_tointeger(state, lua_upvalueindex(1));
    int config_index = 0;
    if (lua_isfunction(state, 1)) {
        lua_newtable(state);
        config_index = lua_absindex(state, -1);
        lua_pushlightuserdata(state, &c_system_config_marker);
        lua_pushboolean(state, 1);
        lua_rawset(state, config_index);
        lua_pushvalue(state, 1);
        lua_setfield(state, config_index, "run");
        push_system_config_metatable(state);
        lua_setmetatable(state, config_index);
    } else if (is_system_config(state, 1)) {
        lua_pushvalue(state, 1);
        config_index = lua_absindex(state, -1);
    } else {
        luaL_error(
            state,
            "system configuration methods require a function or descriptor"
        );
        return 0;
    }

    const int argument_count = call_argument_count - 1;
    if (argument_count == 0) {
        luaL_error(
            state,
            "system configuration method requires at least one argument"
        );
        return 0;
    }
    constexpr int run_if_method = 2;
    for (int index = 2; index <= argument_count + 1; ++index) {
        const bool valid =
            lua_isfunction(state, index) ||
            (method == run_if_method && is_state_condition(state, index));
        if (!valid) {
            luaL_error(
                state,
                method == run_if_method ?
                    "run_if arguments must be conditions" :
                    "before/after arguments must be functions"
            );
            return 0;
        }
    }

    if (method == run_if_method) {
        lua_getfield(state, config_index, "conditions");
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushvalue(state, -1);
            lua_setfield(state, config_index, "conditions");
        }
        const int conditions_index = lua_absindex(state, -1);
        auto condition_count =
            static_cast<int>(lua_objlen(state, conditions_index));
        for (int index = 2; index <= argument_count + 1; ++index) {
            lua_pushvalue(state, index);
            lua_rawseti(state, conditions_index, ++condition_count);
        }
        lua_pop(state, 1);
    } else {
        const char* field = method == 0 ? "before" : "after";
        lua_pushstring(state, field);
        lua_rawget(state, config_index);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushvalue(state, -1);
            lua_setfield(state, config_index, field);
        }
        const int targets_index = lua_absindex(state, -1);
        auto target_count = static_cast<int>(lua_objlen(state, targets_index));
        for (int index = 2; index <= argument_count + 1; ++index) {
            lua_pushvalue(state, index);
            lua_rawseti(state, targets_index, ++target_count);
        }
        lua_pop(state, 1);
    }
    lua_pushvalue(state, config_index);
    return 1;
}

void install_system_config_metatables(lua_State* state) {
    const int base_top = lua_gettop(state);
    lua_newtable(state);
    const int methods_index = lua_absindex(state, -1);
    const char* methods[] = {"before", "after", "run_if"};
    for (std::size_t index = 0; index < std::size(methods); ++index) {
        lua_pushinteger(state, static_cast<int>(index));
        lua_pushcclosure(state, system_config_method, methods[index], 1);
        lua_setfield(state, methods_index, methods[index]);
    }
    lua_setreadonly(state, methods_index, true);

    lua_newtable(state);
    lua_pushvalue(state, methods_index);
    lua_setfield(state, -2, "__index");
    lua_setreadonly(state, -1, true);
    lua_pushlightuserdata(state, &c_system_config_metatable);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1);

    lua_pushcfunction(state, system_config_method, "system_config_target");
    lua_newtable(state);
    lua_pushvalue(state, methods_index);
    lua_setfield(state, -2, "__index");
    lua_setreadonly(state, -1, true);
    lua_setmetatable(state, -2);
    lua_settop(state, base_top);
}

std::string luau_error(lua_State* state, std::string fallback) {
    const char* message = lua_tostring(state, -1);
    return message != nullptr ? std::string {message} : std::move(fallback);
}

int raise_message(lua_State* state, const std::string& message) {
    luaL_error(state, "%s", message.c_str());
    return 0;
}

bool is_script_namespace(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &c_script_namespace_marker);
    lua_rawget(state, index);
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return result;
}

void push_script_namespace(lua_State* state) {
    lua_newtable(state);
    lua_pushlightuserdata(state, &c_script_namespace_marker);
    lua_pushboolean(state, 1);
    lua_rawset(state, -3);
}

template<typename Module, typename PushValue>
Status<LuauScriptError>
set_script_global(Module& module, const Type& type, PushValue push_value) {
    if (module.script_namespaces_sealed) {
        return failure(
            LuauScriptError {"Luau script namespaces are already sealed"}
        );
    }

    auto* state = module.thread;
    const int base_top = lua_gettop(state);
    const auto name = luau_type_name(type);
    if (name.namespace_path.empty()) {
        lua_getglobal(state, std::string(name.local_name).c_str());
        if (!lua_isnil(state, -1)) {
            lua_settop(state, base_top);
            return failure(
                LuauScriptError {
                    "Script type path '" + luau_type_path(type) +
                        "' is already occupied",
                }
            );
        }
        lua_pop(state, 1);
        push_value(state);
        lua_setglobal(state, std::string(name.local_name).c_str());
        return {};
    }

    const auto& root = name.namespace_path.front();
    lua_getglobal(state, root.c_str());
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        push_script_namespace(state);
        lua_pushvalue(state, -1);
        lua_setglobal(state, root.c_str());
        module.script_namespace_roots.insert(root);
    } else if (!lua_istable(state, -1) || !is_script_namespace(state, -1)) {
        lua_settop(state, base_top);
        return failure(
            LuauScriptError {
                "Script namespace '" + root + "' is already occupied",
            }
        );
    }

    for (std::size_t index = 1; index < name.namespace_path.size(); ++index) {
        const auto& component = name.namespace_path[index];
        lua_getfield(state, -1, component.c_str());
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            push_script_namespace(state);
            lua_pushvalue(state, -1);
            lua_setfield(state, -3, component.c_str());
        } else if (!lua_istable(state, -1) || !is_script_namespace(state, -1)) {
            lua_settop(state, base_top);
            return failure(
                LuauScriptError {
                    "Script namespace component '" + component +
                        "' is already occupied",
                }
            );
        }
        lua_remove(state, -2);
    }

    lua_getfield(state, -1, std::string(name.local_name).c_str());
    if (!lua_isnil(state, -1)) {
        lua_settop(state, base_top);
        return failure(
            LuauScriptError {
                "Script type path '" + luau_type_path(type) +
                    "' is already occupied",
            }
        );
    }
    lua_pop(state, 1);
    push_value(state);
    lua_setfield(state, -2, std::string(name.local_name).c_str());
    lua_settop(state, base_top);
    return {};
}

void seal_script_namespace(lua_State* state, int index) {
    index = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_istable(state, -1) && is_script_namespace(state, -1)) {
            seal_script_namespace(state, -1);
        }
        lua_pop(state, 1);
    }
    lua_setreadonly(state, index, true);
}

int system_helper(lua_State* state) {
    if (!lua_isfunction(state, 2) && !is_system_config(state, 2)) {
        luaL_error(state, "system expects a function or configured system");
        return 0;
    }
    lua_pushvalue(state, 2);
    return 1;
}

int chain_helper(lua_State* state) {
    const int argument_count = lua_gettop(state);
    if (argument_count < 2) {
        luaL_error(state, "chain expects at least two system groups");
        return 0;
    }
    for (int index = 1; index <= argument_count; ++index) {
        if (!lua_isfunction(state, index) && !is_system_config(state, index) &&
            !is_system_chain(state, index)) {
            luaL_error(
                state,
                "chain arguments must be systems or nested chains"
            );
            return 0;
        }
    }

    lua_newtable(state);
    const int chain_index = lua_absindex(state, -1);
    lua_pushlightuserdata(state, &c_system_chain_marker);
    lua_pushboolean(state, 1);
    lua_rawset(state, chain_index);
    for (int index = 1; index <= argument_count; ++index) {
        lua_pushvalue(state, index);
        lua_rawseti(state, chain_index, index);
    }
    lua_setreadonly(state, chain_index, true);
    return 1;
}

std::string dynamic_schedule_key(ScheduleId schedule) {
    return "__ets_schedule_" + std::to_string(schedule);
}

int state_schedule_helper(lua_State* state) {
    const int kind = lua_tointeger(state, lua_upvalueindex(1));
    const int expected_arguments = kind == 2 ? 2 : 1;
    if (lua_gettop(state) != expected_arguments) {
        luaL_error(
            state,
            kind == 2 ? "OnTransition expects exited and entered state values" :
                        "OnEnter/OnExit expect one state value"
        );
        return 0;
    }

    auto first = detail::copy_luau_reflected_value(state, 1, "state schedule");
    if (!first) {
        return raise_message(state, first.error());
    }
    auto ops = resolve_dynamic_state(first->type_id());
    if (!ops) {
        return raise_message(state, ops.error().message);
    }

    ScheduleId schedule {};
    if (kind == 0) {
        schedule = ops->on_enter(first->ref());
    } else if (kind == 1) {
        schedule = ops->on_exit(first->ref());
    } else {
        auto second =
            detail::copy_luau_reflected_value(state, 2, "OnTransition");
        if (!second) {
            return raise_message(state, second.error());
        }
        if (second->type_id() != first->type_id()) {
            return raise_message(
                state,
                "OnTransition state values must have the same type"
            );
        }
        schedule = ops->on_transition(first->ref(), second->ref());
    }
    const auto key = dynamic_schedule_key(schedule);
    lua_pushlstring(state, key.data(), key.size());
    return 1;
}

int in_state_helper(lua_State* state) {
    if (lua_gettop(state) != 1) {
        luaL_error(state, "in_state expects one state value");
        return 0;
    }
    auto value = detail::copy_luau_reflected_value(state, 1, "in_state");
    if (!value) {
        return raise_message(state, value.error());
    }
    auto ops = resolve_dynamic_state(value->type_id());
    if (!ops) {
        return raise_message(state, ops.error().message);
    }

    lua_newtable(state);
    const int descriptor = lua_absindex(state, -1);
    lua_pushlightuserdata(state, &c_state_condition_marker);
    lua_pushboolean(state, 1);
    lua_rawset(state, descriptor);
    lua_pushvalue(state, 1);
    lua_setfield(state, descriptor, "value");
    lua_setreadonly(state, descriptor, true);
    return 1;
}

int query_descriptor_helper(lua_State* state) {
    luaL_checktype(state, 1, LUA_TUSERDATA);
    const char* kind = lua_tostring(state, lua_upvalueindex(1));
    lua_newtable(state);
    lua_pushstring(state, kind);
    lua_setfield(state, -2, "kind");
    lua_pushvalue(state, 1);
    lua_setfield(state, -2, "type");
    return 1;
}

int query_or_descriptor_helper(lua_State* state) {
    const int count = lua_gettop(state);
    if (count == 0) {
        luaL_error(state, "Or expects at least one query filter");
    }
    lua_newtable(state);
    const int descriptor = lua_absindex(state, -1);
    lua_pushstring(state, "or");
    lua_setfield(state, descriptor, "kind");
    for (int index = 1; index <= count; ++index) {
        luaL_checktype(state, index, LUA_TTABLE);
        lua_pushvalue(state, index);
        lua_rawseti(state, descriptor, index);
    }
    return 1;
}

int field_helper(lua_State* state) {
    const int argument_count = lua_gettop(state);
    if (argument_count < 1 || argument_count > 2) {
        luaL_error(state, "field expects a type and an optional default value");
    }
    lua_newtable(state);
    return 1;
}

int optional_helper(lua_State* state) {
    if (lua_gettop(state) != 1) {
        luaL_error(state, "optional expects exactly one type");
    }
    lua_newtable(state);
    return 1;
}

int plugin_new_helper(lua_State* state) {
    if (lua_gettop(state) != 1 || !lua_istable(state, 1)) {
        luaL_error(state, "Plugin.new expects exactly one descriptor table");
        return 0;
    }
    lua_setreadonly(state, 1, true);
    lua_pushvalue(state, 1);
    return 1;
}

template<typename Module>
Result<const LuauFunctionDecl*, LuauScriptError> function_declaration(
    Module& module,
    lua_State* state,
    int index,
    std::string_view context
) {
    if (!lua_isfunction(state, index)) {
        return failure(
            LuauScriptError {std::string(context) + " must be a function"}
        );
    }
    const auto pointer_match =
        module.function_indices.find(lua_topointer(state, index));
    if (pointer_match != module.function_indices.end() &&
        pointer_match->second < module.function_declarations.size()) {
        return &module.function_declarations[pointer_match->second];
    }

    index = lua_absindex(state, index);
    for (std::size_t declaration_index = 0;
         declaration_index < module.function_declarations.size();
         ++declaration_index) {
        const auto& declaration =
            module.function_declarations[declaration_index];
        const auto stored = module.functions.find(declaration.name);
        if (stored == module.functions.end()) {
            continue;
        }
        lua_getref(state, stored->second);
        const bool matches = lua_rawequal(state, index, -1) != 0;
        lua_pop(state, 1);
        if (matches) {
            module.function_indices.emplace(
                lua_topointer(state, index),
                declaration_index
            );
            return &declaration;
        }
    }
    return failure(
        LuauScriptError {
            std::string(context) +
                " is not a compiled top-level or imported function " +
                "(compiled declarations: " +
                std::to_string(module.function_declarations.size()) +
                ", registered functions: " +
                std::to_string(module.functions.size()) + ")",
        }
    );
}

template<typename Module>
Result<DynamicSystemDecl, LuauScriptError> decode_system(
    Module& module,
    lua_State* state,
    int index,
    ScheduleId schedule
) {
    index = lua_absindex(state, index);
    int run_index = index;
    if (is_system_config(state, index)) {
        lua_getfield(state, index, "run");
        run_index = lua_absindex(state, -1);
    }
    auto function = function_declaration(module, state, run_index, "system");
    if (!function) {
        if (run_index != index) {
            lua_pop(state, 1);
        }
        return failure(std::move(function.error()));
    }
    auto params = clone_param_declarations((*function)->params);
    DynamicSystemDecl result {
        .name = (*function)->name,
        .params = std::move(params),
        .schedule = schedule,
    };
    if (run_index != index) {
        lua_pop(state, 1);
    }
    if (!is_system_config(state, index)) {
        return result;
    }

    const auto decode_targets =
        [&](const char* field,
            std::vector<std::string>& targets) -> Status<LuauScriptError> {
        lua_pushstring(state, field);
        lua_rawget(state, index);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            return {};
        }
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            return failure(
                LuauScriptError {"system configuration targets are invalid"}
            );
        }
        const auto count = static_cast<std::size_t>(lua_objlen(state, -1));
        for (std::size_t target = 0; target < count; ++target) {
            lua_rawgeti(state, -1, static_cast<int>(target + 1));
            auto declaration = function_declaration(module, state, -1, field);
            lua_pop(state, 1);
            if (!declaration) {
                lua_pop(state, 1);
                return failure(std::move(declaration.error()));
            }
            targets.push_back((*declaration)->name);
        }
        lua_pop(state, 1);
        return {};
    };
    auto targets = decode_targets("before", result.before);
    if (!targets) {
        return failure(std::move(targets.error()));
    }
    targets = decode_targets("after", result.after);
    if (!targets) {
        return failure(std::move(targets.error()));
    }

    lua_pushstring(state, "conditions");
    lua_rawget(state, index);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return result;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return failure(
            LuauScriptError {"system configuration conditions are invalid"}
        );
    }
    const auto condition_count =
        static_cast<std::size_t>(lua_objlen(state, -1));
    for (std::size_t condition_index = 0; condition_index < condition_count;
         ++condition_index) {
        lua_rawgeti(state, -1, static_cast<int>(condition_index + 1));
        if (is_state_condition(state, -1)) {
            lua_getfield(state, -1, "value");
            auto value = detail::copy_luau_reflected_value(
                state,
                -1,
                "in_state condition"
            );
            lua_pop(state, 1);
            if (!value) {
                lua_pop(state, 2);
                return failure(LuauScriptError {std::move(value.error())});
            }
            auto param = std::make_unique<DynamicStateParamDecl>();
            param->name = "state";
            param->type.type_id = value->type_id();
            DynamicConditionDecl condition {
                .kind = DynamicConditionDeclKind::InState,
                .name = "in_state",
                .state_value = std::move(*value),
            };
            condition.params.push_back(std::move(param));
            result.conditions.push_back(std::move(condition));
        } else {
            auto declaration =
                function_declaration(module, state, -1, "run_if condition");
            if (!declaration) {
                lua_pop(state, 2);
                return failure(std::move(declaration.error()));
            }
            auto condition_params =
                clone_param_declarations((*declaration)->params);
            result.conditions.push_back(
                DynamicConditionDecl {
                    .name = (*declaration)->name,
                    .params = std::move(condition_params),
                }
            );
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return result;
}

struct DecodedSystemGroup {
    std::vector<DynamicSystemDecl> systems;
    std::vector<std::size_t> first;
    std::vector<std::size_t> last;
};

template<typename Module>
Result<DecodedSystemGroup, LuauScriptError> decode_system_group(
    Module& module,
    lua_State* state,
    int index,
    ScheduleId schedule
) {
    index = lua_absindex(state, index);
    if (!is_system_chain(state, index)) {
        auto system = decode_system(module, state, index, schedule);
        if (!system) {
            return failure(std::move(system.error()));
        }
        DecodedSystemGroup result;
        result.systems.push_back(std::move(*system));
        result.first.push_back(0);
        result.last.push_back(0);
        return result;
    }
    const auto count = static_cast<std::size_t>(lua_objlen(state, index));
    if (count < 2) {
        return failure(LuauScriptError {"chain expects at least two groups"});
    }
    DecodedSystemGroup result;
    std::vector<std::size_t> previous;
    for (std::size_t child_index = 0; child_index < count; ++child_index) {
        lua_rawgeti(state, index, static_cast<int>(child_index + 1));
        auto child = decode_system_group(module, state, -1, schedule);
        lua_pop(state, 1);
        if (!child) {
            return failure(std::move(child.error()));
        }
        const auto offset = result.systems.size();
        std::vector<std::size_t> current_first;
        for (const auto child_first : child->first) {
            current_first.push_back(offset + child_first);
        }
        for (const auto former : previous) {
            for (const auto latter : current_first) {
                result.systems[former].before.push_back(
                    child->systems[latter - offset].name
                );
            }
        }
        if (child_index == 0) {
            result.first = current_first;
        }
        previous.clear();
        for (const auto child_last : child->last) {
            previous.push_back(offset + child_last);
        }
        for (auto& system : child->systems) {
            result.systems.push_back(std::move(system));
        }
    }
    result.last = std::move(previous);
    return result;
}

Result<ScheduleId, LuauScriptError>
decode_schedule(lua_State* state, int index) {
    if (lua_isnumber(state, index)) {
        const auto schedule = lua_tointeger(state, index);
        if (schedule < 0 || schedule > 18) {
            return failure(LuauScriptError {"invalid main schedule"});
        }
        return static_cast<ScheduleId>(schedule);
    }
    if (!lua_isstring(state, index)) {
        return failure(LuauScriptError {"schedule must be a schedule value"});
    }
    std::size_t size = 0;
    const char* text = lua_tolstring(state, index, &size);
    constexpr std::string_view prefix = "__ets_schedule_";
    const std::string_view value {text, size};
    if (!value.starts_with(prefix)) {
        return failure(LuauScriptError {"invalid dynamic schedule"});
    }
    ScheduleId schedule {};
    const auto digits = value.substr(prefix.size());
    const auto parsed =
        std::from_chars(digits.data(), digits.data() + digits.size(), schedule);
    if (parsed.ec != std::errc {} ||
        parsed.ptr != digits.data() + digits.size()) {
        return failure(LuauScriptError {"invalid dynamic schedule"});
    }
    return schedule;
}

template<typename Module>
Result<Optional<LuauPluginBuildOperation>, LuauScriptError>
decode_plugin_build_call(
    Module& module,
    lua_State* state,
    std::string_view method
) {
    const int count = lua_gettop(state);
    if (method == "init_state" || method == "insert_state") {
        if (count != 2) {
            return failure(LuauScriptError {"state methods expect one value"});
        }
        auto value = detail::copy_luau_reflected_value(state, 2, method);
        if (!value) {
            return failure(LuauScriptError {std::move(value.error())});
        }
        return LuauPluginBuildOperation {LuauStateBuildOperation {
            .initial = std::move(*value),
            .init_if_missing = method == "init_state",
        }};
    }
    if (method == "add_resource" || method == "insert_resource") {
        if (count != 2) {
            return failure(
                LuauScriptError {"resource methods expect one value"}
            );
        }
        auto value = detail::copy_luau_reflected_value(state, 2, method);
        if (!value) {
            return failure(LuauScriptError {std::move(value.error())});
        }
        return LuauPluginBuildOperation {LuauResourceBuildOperation {
            .value = std::move(*value),
            .init_if_missing = method == "add_resource",
        }};
    }
    if (method == "add_event") {
        if (count != 2) {
            return failure(LuauScriptError {"add_event expects one type"});
        }
        return LuauPluginBuildOperation {LuauEventBuildOperation {
            .type = detail::check_luau_type_token(state, 2, "add_event"),
        }};
    }
    if (method == "add_system" || method == "add_systems") {
        if (count < 3 || (method == "add_system" && count != 3)) {
            return failure(
                LuauScriptError {
                    "system methods expect a schedule and system groups",
                }
            );
        }
        auto schedule = decode_schedule(state, 2);
        if (!schedule) {
            return failure(std::move(schedule.error()));
        }
        LuauSystemsBuildOperation operation;
        for (int index = 3; index <= count; ++index) {
            auto group = decode_system_group(module, state, index, *schedule);
            if (!group) {
                return failure(std::move(group.error()));
            }
            for (auto& system : group->systems) {
                operation.systems.push_back(std::move(system));
            }
        }
        return LuauPluginBuildOperation {std::move(operation)};
    }
    if (method == "add_playtest") {
        if (count != 2 || !lua_istable(state, 2)) {
            return failure(
                LuauScriptError {"add_playtest expects one declaration table"}
            );
        }
        try {
            auto parsed = parse_playtest(state, 2);
            module.playtests.push_back(std::move(parsed.declaration));
            module.playtest_callbacks.push_back(
                typename Module::PlaytestCallbacks {
                    .begin_step = parsed.begin_step,
                    .end_step = parsed.end_step,
                    .observe = parsed.observe,
                }
            );
        } catch (const std::exception& error) {
            return failure(LuauScriptError {error.what()});
        }
        return Optional<LuauPluginBuildOperation> {};
    }
    return failure(
        LuauScriptError {
            "Unsupported Luau Plugin App method '" + std::string(method) + "'",
        }
    );
}

using PluginBuildMethodDispatch = std::function<
    Status<LuauScriptError>(lua_State* state, std::string_view method)>;

int plugin_build_method(lua_State* state) {
    auto* dispatch = static_cast<const PluginBuildMethodDispatch*>(
        lua_touserdata(state, lua_upvalueindex(1))
    );
    std::size_t method_size = 0;
    const char* method =
        lua_tolstring(state, lua_upvalueindex(2), &method_size);
    if (dispatch == nullptr || method == nullptr) {
        luaL_error(state, "Luau Plugin build context is invalid");
        return 0;
    }
    auto status = (*dispatch)(state, std::string_view {method, method_size});
    if (!status) {
        luaL_error(state, "%s", status.error().message.c_str());
        return 0;
    }
    return 0;
}

void push_plugin_build_context(
    lua_State* state,
    const PluginBuildMethodDispatch& dispatch
) {
    lua_newtable(state);
    constexpr std::string_view methods[] = {
        "init_state",
        "insert_state",
        "add_event",
        "add_resource",
        "insert_resource",
        "add_system",
        "add_systems",
        "add_playtest",
    };
    for (const auto method : methods) {
        lua_pushlightuserdata(
            state,
            const_cast<PluginBuildMethodDispatch*>(&dispatch)
        );
        lua_pushlstring(state, method.data(), method.size());
        lua_pushcclosure(state, plugin_build_method, method.data(), 2);
        lua_setfield(state, -2, std::string(method).c_str());
    }
    lua_setreadonly(state, -1, true);
}

void install_module_helpers(lua_State* state) {
    lua_pushcfunction(state, detail::luau_chunk_query, "__ets_chunk_query");
    lua_setglobal(state, "__ets_chunk_query");
    lua_pushcfunction(state, detail::luau_reusable_query, "__ets_reuse_query");
    lua_setglobal(state, "__ets_reuse_query");
    lua_pushcfunction(state, system_helper, "system");
    lua_setglobal(state, "system");
    lua_pushcfunction(state, chain_helper, "chain");
    lua_setglobal(state, "chain");
    const char* state_schedules[] = {"OnEnter", "OnExit", "OnTransition"};
    for (std::size_t index = 0; index < std::size(state_schedules); ++index) {
        lua_pushinteger(state, static_cast<int>(index));
        lua_pushcclosure(
            state,
            state_schedule_helper,
            state_schedules[index],
            1
        );
        lua_setglobal(state, state_schedules[index]);
    }
    lua_pushcfunction(state, in_state_helper, "in_state");
    lua_setglobal(state, "in_state");
    lua_pushcfunction(state, field_helper, "field");
    lua_setglobal(state, "field");
    lua_pushcfunction(state, optional_helper, "optional");
    lua_setglobal(state, "optional");
    lua_newtable(state);
    lua_pushcfunction(state, plugin_new_helper, "Plugin.new");
    lua_setfield(state, -2, "new");
    lua_setreadonly(state, -1, true);
    lua_setglobal(state, "Plugin");

    const char* query_descriptors[] = {
        "Read",
        "Write",
        "With",
        "Without",
        "Added",
        "Changed",
    };
    for (const char* name : query_descriptors) {
        std::string kind {name};
        kind[0] = static_cast<char>(std::tolower(kind[0]));
        lua_pushlstring(state, kind.data(), kind.size());
        lua_pushcclosure(state, query_descriptor_helper, name, 1);
        lua_setglobal(state, name);
    }
    lua_pushcfunction(state, query_or_descriptor_helper, "Or");
    lua_setglobal(state, "Or");
    lua_newtable(state);
    lua_pushstring(state, "entity");
    lua_setfield(state, -2, "kind");
    lua_setglobal(state, "Entity");

    lua_newtable(state);
    const char* schedules[] = {
        "First",       "PreStartUp",       "StartUp",      "PreUpdate",
        "Update",      "PostUpdate",       "Last",         "RenderPrepare",
        "RenderFirst", "RenderStart",      "RenderUpdate", "RenderEnd",
        "RenderLast",  "RunFixedMainLoop", "FixedFirst",   "FixedPreUpdate",
        "FixedUpdate", "FixedPostUpdate",  "FixedLast",
    };
    for (std::size_t index = 0; index < std::size(schedules); ++index) {
        lua_pushinteger(state, static_cast<int>(index));
        lua_setfield(state, -2, schedules[index]);
        lua_pushinteger(state, static_cast<int>(index));
        lua_setglobal(state, schedules[index]);
    }
    lua_setglobal(state, "MainSchedules");
}

} // namespace

LuauRuntime::LuauRuntime(std::shared_ptr<const LuauHostConfig> config) :
    m_impl(std::make_unique<Impl>(std::move(config))) {}
std::string LuauRuntime::library_diagnostic(
    std::string_view name,
    std::string_view key
) const {
    return m_impl->libraries->diagnostic(name, key);
}
LuauRuntime::~LuauRuntime() = default;
LuauRuntime::LuauRuntime(LuauRuntime&&) noexcept = default;
LuauRuntime& LuauRuntime::operator=(LuauRuntime&&) noexcept = default;

void LuauRuntime::enable_host_calls(uint32 interrupt_budget) {
    m_impl->libraries->enable_host_calls();
    m_impl->interrupt_limit = interrupt_budget;
    m_impl->interrupts_left = interrupt_budget;
    lua_pushlightuserdata(m_impl->state, m_impl.get());
    lua_setfield(m_impl->state, LUA_REGISTRYINDEX, "__ets_interrupt_runtime");
    lua_callbacks(m_impl->state)->interrupt = [](lua_State* state, int gc) {
        if (gc >= 0) {
            return;
        }
        lua_getfield(state, LUA_REGISTRYINDEX, "__ets_interrupt_runtime");
        auto* impl = static_cast<Impl*>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        if (impl->interrupts_left > 0) {
            --impl->interrupts_left;
        }
        if (impl->interrupts_left == 0) {
            luaL_error(state, "Luau execution budget exceeded");
        }
    };
}

Status<LuauScriptError>
LuauRuntime::start_task(LuauScriptModuleId module, std::string_view entry) {
    if (m_impl->task) {
        return failure(LuauScriptError {"A task is already running"});
    }
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Module not loaded"});
    }
    auto* root = m_impl->state;
    auto* task = lua_newthread(root);
    lua_getref(task, found->second.exports_ref);
    lua_getfield(task, -1, std::string(entry).c_str());
    lua_remove(task, -2);
    if (!lua_isfunction(task, -1)) {
        lua_pop(root, 1);
        return failure(LuauScriptError {"Task entry must be a function"});
    }
    m_impl->task_ref = lua_ref(root, -1);
    lua_pop(root, 1);
    m_impl->task = task;
    m_impl->task_started = false;
    return {};
}

Result<std::string, LuauScriptError>
LuauRuntime::resume_task(std::string_view reply_json) {
    auto* task = m_impl->task;
    if (!task) {
        return failure(LuauScriptError {"No task is running"});
    }
    auto finish = [&] {
        m_impl->libraries->cancel();
        lua_unref(m_impl->state, m_impl->task_ref);
        m_impl->task = nullptr;
    };
    try {
        int arguments = 0;
        if (m_impl->task_started) {
            lua_settop(task, 0);
            const auto reply = Json::parse(reply_json);
            push_json(task, reply, 0, true);
            arguments = 1;
        }
        m_impl->task_started = true;
        m_impl->interrupts_left = m_impl->interrupt_limit;
        const int status = lua_resume(task, nullptr, arguments);
        if (status == LUA_YIELD) {
            if (lua_gettop(task) != 1) {
                throw std::runtime_error("Host call must yield one request");
            }
            return luau_json(task, -1, "Host request").dump();
        }
        if (status != LUA_OK) {
            std::string message = luau_error(task, "Task failed");
            message += "\n";
            message += lua_debugtrace(task);
            finish();
            return failure(LuauScriptError {std::move(message)});
        }
        finish();
        return std::string {R"({"kind":"completed"})"};
    } catch (const std::exception& error) {
        finish();
        return failure(LuauScriptError {error.what()});
    }
}

Result<LuauScriptModuleId, LuauScriptError> LuauRuntime::load_module(
    const LuauScriptModuleArtifact& artifact,
    std::span<const LuauScriptImportBinding> imports
) {
    lua_State* root = m_impl->state;
    const int root_top = lua_gettop(root);
    lua_State* thread = lua_newthread(root);
    const int thread_ref = lua_ref(root, -1);
    lua_pop(root, 1);
    luaL_sandboxthread(thread);
    install_module_helpers(thread);

    const auto id = static_cast<LuauScriptModuleId>(m_impl->next_module_id++);
    auto [loaded_entry, inserted] = m_impl->modules.try_emplace(id);
    (void)inserted;
    auto& loaded = loaded_entry->second;
    loaded.thread = thread;
    loaded.thread_ref = thread_ref;
    m_impl->install_imports(id, loaded, imports);
    auto fail_loading = [&](
                            LuauScriptError error
                        ) -> Result<LuauScriptModuleId, LuauScriptError> {
        for (const auto& entry : loaded.functions) {
            lua_unref(root, entry.second);
        }
        if (loaded.exports_ref != 0) {
            lua_unref(root, loaded.exports_ref);
        }
        lua_unref(root, thread_ref);
        m_impl->modules.erase(id);
        lua_settop(root, root_top);
        return failure(std::move(error));
    };
    loaded.function_declarations.reserve(artifact.functions.size());
    for (const auto& function : artifact.functions) {
        auto params = clone_param_declarations(function.params);
        loaded.function_declarations.push_back(
            LuauFunctionDecl {
                .name = function.name,
                .params = std::move(params),
            }
        );
    }
    auto registered_paths =
        detail::register_luau_property_paths(thread, artifact.property_paths);
    if (!registered_paths) {
        return fail_loading(
            LuauScriptError {std::move(registered_paths.error())}
        );
    }
    for (TypeId required_type : artifact.required_runtime_types) {
        auto type = Registry::instance().try_get_type(required_type);
        if (!type) {
            return fail_loading(
                LuauScriptError {std::move(type.error().message)}
            );
        }
        auto reflected_enum = Registry::instance().try_get_enum(required_type);
        if (!reflected_enum) {
            return fail_loading(
                LuauScriptError {
                    "Required Luau state type '" + type->name() +
                    "' is not a reflected enum"
                }
            );
        }
        auto push_enum = [&](lua_State* state) {
            lua_newtable(state);
            for (const auto& [enumerator, underlying_value] :
                 reflected_enum->enumerators()) {
                detail::push_luau_owned_value(
                    state,
                    reflected_enum->make_val(underlying_value)
                );
                lua_setfield(state, -2, enumerator.c_str());
            }
            lua_setreadonly(state, -1, true);
        };
        Status<LuauScriptError> bound;
        if (type->has_structured_name()) {
            bound = set_script_global(loaded, *type, push_enum);
        } else {
            const auto& name = type->stripped_name();
            lua_getglobal(thread, name.c_str());
            if (!lua_isnil(thread, -1)) {
                lua_pop(thread, 1);
                bound = failure(
                    LuauScriptError {
                        "Script type name '" + name + "' is already occupied",
                    }
                );
            } else {
                lua_pop(thread, 1);
                push_enum(thread);
                lua_setglobal(thread, name.c_str());
            }
        }
        if (!bound) {
            return fail_loading(
                LuauScriptError {std::move(bound.error().message)}
            );
        }
        loaded.script_types.insert(required_type);
    }
    for (const auto& script_enum : artifact.metadata->schema.enums) {
        auto ensured = ensure_luau_enum_type(script_enum);
        if (!ensured) {
            return fail_loading(
                LuauScriptError {std::move(ensured.error().message)}
            );
        }
        lua_getglobal(thread, script_enum.name.c_str());
        if (!lua_isnil(thread, -1)) {
            lua_pop(thread, 1);
            return fail_loading(
                LuauScriptError {
                    "Script enum name '" + script_enum.name +
                    "' is already occupied"
                }
            );
        }
        lua_pop(thread, 1);
        lua_newtable(thread);
        for (const auto& value : script_enum.values) {
            auto enum_value = make_luau_enum_value(script_enum, value.name);
            if (!enum_value) {
                return fail_loading(
                    LuauScriptError {std::move(enum_value.error().message)}
                );
            }
            detail::push_luau_owned_value(thread, std::move(*enum_value));
            lua_setfield(thread, -2, value.name.c_str());
        }
        lua_setreadonly(thread, -1, true);
        lua_setglobal(thread, script_enum.name.c_str());
        loaded.script_types.insert(script_enum.type_id);
    }
    for (const auto& script_state : artifact.states) {
        auto ensured = ensure_luau_state_type(script_state);
        if (!ensured) {
            return fail_loading(
                LuauScriptError {std::move(ensured.error().message)}
            );
        }
    }

    if (luau_load(
            thread,
            artifact.metadata->schema.source_name.c_str(),
            artifact.bytecode.data(),
            artifact.bytecode.size(),
            0
        ) != 0) {
        std::string message = luau_error(thread, "Failed to load Luau module");
        return fail_loading(LuauScriptError {std::move(message)});
    }
    if (lua_pcall(thread, 0, 1, 0) != 0) {
        std::string message = luau_error(thread, "Failed to run Luau module");
        return fail_loading(LuauScriptError {std::move(message)});
    }
    if (lua_isfunction(thread, -1) && artifact.functions.empty()) {
        lua_newtable(thread);
        lua_pushvalue(thread, -2);
        lua_setfield(thread, -2, "run");
        lua_remove(thread, -2);
    }
    if (!lua_istable(thread, -1)) {
        return fail_loading(
            LuauScriptError {"Luau module must return a table"}
        );
    }

    const int module_index = lua_absindex(thread, -1);
    lua_pushvalue(thread, module_index);
    loaded.exports_ref = lua_ref(thread, -1);
    lua_pop(thread, 1);
    lua_getfield(thread, module_index, "__ets_functions");
    if (lua_isnil(thread, -1) && artifact.functions.empty()) {
        lua_settop(thread, 0);
        lua_settop(root, root_top);
        return id;
    }
    if (!lua_istable(thread, -1)) {
        return fail_loading(
            LuauScriptError {"Luau module functions field must be a table"}
        );
    }
    const int functions_index = lua_absindex(thread, -1);
    auto store_function = [&](const std::string& name,
                              int index) -> Status<LuauScriptError> {
        if (!lua_isfunction(thread, index)) {
            return failure(
                LuauScriptError {
                    "Luau module function '" + name + "' is not a function"
                }
            );
        }
        if (loaded.functions.contains(name)) {
            return {};
        }
        lua_pushvalue(thread, index);
        const int function_ref = lua_ref(thread, -1);
        lua_pop(thread, 1);
        loaded.functions.emplace(name, function_ref);
        return {};
    };

    for (std::size_t index = 0; index < artifact.functions.size(); ++index) {
        lua_rawgeti(thread, functions_index, static_cast<int>(index + 1));
        const auto& declaration = artifact.functions[index];
        auto stored = store_function(declaration.name, -1);
        if (stored) {
            loaded.function_indices.emplace(lua_topointer(thread, -1), index);
        }
        lua_pop(thread, 1);
        if (!stored) {
            return fail_loading(std::move(stored.error()));
        }
    }
    lua_settop(thread, 0);

    lua_settop(root, root_top);
    return id;
}

Status<LuauScriptError> LuauRuntime::unload_module(LuauScriptModuleId module) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    for (const auto& entry : found->second.functions) {
        lua_unref(m_impl->state, entry.second);
    }
    for (const auto& callbacks : found->second.playtest_callbacks) {
        lua_unref(m_impl->state, callbacks.begin_step);
        if (callbacks.end_step != 0) {
            lua_unref(m_impl->state, callbacks.end_step);
        }
        if (callbacks.observe != 0) {
            lua_unref(m_impl->state, callbacks.observe);
        }
    }
    if (found->second.exports_ref != 0) {
        lua_unref(m_impl->state, found->second.exports_ref);
    }
    lua_unref(m_impl->state, found->second.thread_ref);
    m_impl->modules.erase(found);
    return {};
}

Status<LuauScriptError> LuauRuntime::bind_module_type(
    LuauScriptModuleId module,
    const std::string& name,
    const Type& type
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    auto* thread = found->second.thread;
    detail::push_luau_type_token(thread, type.id());
    lua_setglobal(thread, name.c_str());
    return {};
}

Status<LuauScriptError> LuauRuntime::bind_module_exported_type(
    LuauScriptModuleId module,
    const std::string& name,
    const Type& type
) {
    auto bound = bind_module_type(module, name, type);
    if (!bound) {
        return bound;
    }
    const auto found = m_impl->modules.find(module);
    if (found->second.exports_ref == 0) {
        return {};
    }
    auto* thread = found->second.thread;
    lua_getref(thread, found->second.exports_ref);
    if (!lua_istable(thread, -1)) {
        lua_pop(thread, 1);
        return failure(LuauScriptError {"Luau module exports are not a table"});
    }
    lua_setreadonly(thread, -1, false);
    detail::push_luau_type_token(thread, type.id());
    lua_setfield(thread, -2, name.c_str());
    lua_setreadonly(thread, -1, true);
    lua_pop(thread, 1);
    return {};
}

Status<LuauScriptError> LuauRuntime::bind_module_script_type(
    LuauScriptModuleId module,
    const Type& type
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    if (found->second.script_types.contains(type.id())) {
        return {};
    }
    auto status = set_script_global(found->second, type, [&](lua_State* state) {
        detail::push_luau_type_token(state, type.id());
    });
    if (status) {
        found->second.script_types.insert(type.id());
    }
    return status;
}

Status<LuauScriptError> LuauRuntime::bind_module_enum(
    LuauScriptModuleId module,
    const std::string& name,
    const Enum& enm
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    auto* thread = found->second.thread;
    lua_newtable(thread);
    for (const auto& [enumerator, underlying_value] : enm.enumerators()) {
        detail::push_luau_owned_value(thread, enm.make_val(underlying_value));
        lua_setfield(thread, -2, enumerator.c_str());
    }
    lua_setreadonly(thread, -1, true);
    lua_setglobal(thread, name.c_str());
    return {};
}

Status<LuauScriptError> LuauRuntime::bind_module_script_enum(
    LuauScriptModuleId module,
    const Enum& enm
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    auto type = Registry::instance().try_get_type(enm.type_id());
    if (!type) {
        return failure(LuauScriptError {std::move(type.error().message)});
    }
    if (found->second.script_types.contains(type->id())) {
        return {};
    }
    auto status =
        set_script_global(found->second, *type, [&](lua_State* state) {
            lua_newtable(state);
            for (const auto& [enumerator, underlying_value] :
                 enm.enumerators()) {
                detail::push_luau_owned_value(
                    state,
                    enm.make_val(underlying_value)
                );
                lua_setfield(state, -2, enumerator.c_str());
            }
            lua_setreadonly(state, -1, true);
        });
    if (status) {
        found->second.script_types.insert(type->id());
    }
    return status;
}

Status<LuauScriptError>
LuauRuntime::seal_module_script_namespaces(LuauScriptModuleId module) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    auto& loaded = found->second;
    if (loaded.script_namespaces_sealed) {
        return {};
    }
    for (const auto& root : loaded.script_namespace_roots) {
        lua_getglobal(loaded.thread, root.c_str());
        if (lua_istable(loaded.thread, -1) &&
            is_script_namespace(loaded.thread, -1)) {
            seal_script_namespace(loaded.thread, -1);
        }
        lua_pop(loaded.thread, 1);
    }
    loaded.script_namespaces_sealed = true;
    return {};
}

Status<LuauScriptError> LuauRuntime::call_module_plugin_build(
    LuauScriptModuleId module,
    std::string_view plugin_name,
    const LuauPluginBuildDispatch& dispatch
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    if (plugin_name.empty()) {
        return failure(LuauScriptError {"Luau Plugin name is empty"});
    }

    lua_State* thread = found->second.thread;
    lua_getref(thread, found->second.exports_ref);
    lua_getfield(thread, -1, std::string(plugin_name).c_str());
    if (!lua_istable(thread, -1)) {
        lua_settop(thread, 0);
        return failure(
            LuauScriptError {
                "Luau module Plugin '" + std::string(plugin_name) +
                    "' is not a table",
            }
        );
    }
    lua_getfield(thread, -1, "build");
    if (!lua_isfunction(thread, -1)) {
        lua_settop(thread, 0);
        return failure(
            LuauScriptError {
                "Luau module Plugin '" + std::string(plugin_name) +
                    "' has no build function",
            }
        );
    }
    PluginBuildMethodDispatch method_dispatch = [&](lua_State* state,
                                                    std::string_view method) {
        auto operation = decode_plugin_build_call(found->second, state, method);
        if (!operation) {
            return Status<LuauScriptError> {
                failure(std::move(operation.error()))
            };
        }
        if (!*operation) {
            return Status<LuauScriptError> {};
        }
        return dispatch(std::move(**operation));
    };
    push_plugin_build_context(thread, method_dispatch);
    if (lua_pcall(thread, 1, 0, 0) != 0) {
        std::string message = luau_error(thread, "Failed to build Luau Plugin");
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    lua_settop(thread, 0);
    return {};
}

Status<LuauScriptError> LuauRuntime::call_module_function(
    LuauScriptModuleId module,
    const std::string& function_name
) {
    return call_module_function(module, function_name, {});
}

Status<LuauScriptError> LuauRuntime::call_module_function(
    LuauScriptModuleId module,
    const std::string& function_name,
    std::span<const Ref> args
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    const auto function = found->second.functions.find(function_name);
    if (function == found->second.functions.end()) {
        return failure(
            LuauScriptError {
                "Luau module function '" + function_name + "' not found",
            }
        );
    }

    lua_State* thread = found->second.thread;
    auto refreshed_paths = detail::refresh_luau_property_paths(thread);
    if (!refreshed_paths) {
        return failure(LuauScriptError {std::move(refreshed_paths.error())});
    }
    auto& scope = m_impl->borrow_scope;
    const auto token = scope.begin();
    lua_getref(thread, function->second);
    for (Ref arg : args) {
        detail::push_luau_borrowed_ref(thread, arg, scope, token);
    }
    if (lua_pcall(thread, static_cast<int>(args.size()), 0, 0) != 0) {
        std::string message = luau_error(thread, "Failed to call Luau system");
        scope.end(token);
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    scope.end(token);
    return {};
}

Result<bool, LuauScriptError> LuauRuntime::call_module_condition(
    LuauScriptModuleId module,
    const std::string& function_name,
    std::span<const Ref> args
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return failure(LuauScriptError {"Luau module not found"});
    }
    const auto function = found->second.functions.find(function_name);
    if (function == found->second.functions.end()) {
        return failure(
            LuauScriptError {
                "Luau module condition '" + function_name + "' not found"
            }
        );
    }

    lua_State* thread = found->second.thread;
    auto refreshed_paths = detail::refresh_luau_property_paths(thread);
    if (!refreshed_paths) {
        return failure(LuauScriptError {std::move(refreshed_paths.error())});
    }
    auto& scope = m_impl->borrow_scope;
    const auto token = scope.begin();
    lua_getref(thread, function->second);
    for (Ref arg : args) {
        detail::push_luau_borrowed_ref(thread, arg, scope, token);
    }
    if (lua_pcall(thread, static_cast<int>(args.size()), 1, 0) != 0) {
        std::string message =
            luau_error(thread, "Failed to call Luau condition");
        scope.end(token);
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    if (!lua_isboolean(thread, -1)) {
        scope.end(token);
        lua_settop(thread, 0);
        return failure(
            LuauScriptError {
                "Luau condition '" + function_name + "' must return a boolean"
            }
        );
    }
    const bool result = lua_toboolean(thread, -1) != 0;
    lua_pop(thread, 1);
    scope.end(token);
    return result;
}

std::span<const LuauPlaytestDeclaration>
LuauRuntime::module_playtests(LuauScriptModuleId module) const {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end()) {
        return {};
    }
    return found->second.playtests;
}

Status<LuauScriptError> LuauRuntime::begin_module_playtest_step(
    LuauScriptModuleId module,
    std::size_t playtest,
    World& world,
    std::string_view action_json
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end() ||
        playtest >= found->second.playtest_callbacks.size()) {
        return failure(LuauScriptError {"Luau playtest callback not found"});
    }
    Json action;
    try {
        action = Json::parse(action_json);
    } catch (const std::exception& error) {
        return failure(
            LuauScriptError {
                std::string("Invalid Luau playtest action: ") + error.what()
            }
        );
    }

    DynamicWorld context("playtest.begin_step");
    auto prepared = context.prepare(world);
    if (!prepared) {
        return failure(LuauScriptError {std::move(prepared.error().message)});
    }
    auto* thread = found->second.thread;
    auto& scope = m_impl->borrow_scope;
    const auto token = scope.begin();
    lua_settop(thread, 0);
    lua_getref(thread, found->second.playtest_callbacks[playtest].begin_step);
    detail::push_luau_borrowed_ref(thread, *prepared, scope, token);
    try {
        push_json(thread, action);
    } catch (const std::exception& error) {
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return failure(LuauScriptError {error.what()});
    }
    if (lua_pcall(thread, 2, 0, 0) != 0) {
        auto message = luau_error(thread, "Failed to call playtest begin_step");
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    scope.end(token);
    context.finish();
    lua_settop(thread, 0);
    return {};
}

Status<LuauScriptError> LuauRuntime::end_module_playtest_step(
    LuauScriptModuleId module,
    std::size_t playtest,
    World& world
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end() ||
        playtest >= found->second.playtest_callbacks.size()) {
        return failure(LuauScriptError {"Luau playtest callback not found"});
    }
    const auto callback = found->second.playtest_callbacks[playtest].end_step;
    if (callback == 0) {
        return {};
    }
    DynamicWorld context("playtest.end_step");
    auto prepared = context.prepare(world);
    if (!prepared) {
        return failure(LuauScriptError {std::move(prepared.error().message)});
    }
    auto* thread = found->second.thread;
    auto& scope = m_impl->borrow_scope;
    const auto token = scope.begin();
    lua_settop(thread, 0);
    lua_getref(thread, callback);
    detail::push_luau_borrowed_ref(thread, *prepared, scope, token);
    if (lua_pcall(thread, 1, 0, 0) != 0) {
        auto message = luau_error(thread, "Failed to call playtest end_step");
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    scope.end(token);
    context.finish();
    lua_settop(thread, 0);
    return {};
}

Result<std::string, LuauScriptError> LuauRuntime::observe_module_playtest(
    LuauScriptModuleId module,
    std::size_t playtest,
    World& world
) {
    const auto found = m_impl->modules.find(module);
    if (found == m_impl->modules.end() ||
        playtest >= found->second.playtest_callbacks.size()) {
        return failure(LuauScriptError {"Luau playtest callback not found"});
    }
    const auto callback = found->second.playtest_callbacks[playtest].observe;
    if (callback == 0) {
        return std::string("{}");
    }
    DynamicWorld context("playtest.observe");
    auto prepared = context.prepare(world);
    if (!prepared) {
        return failure(LuauScriptError {std::move(prepared.error().message)});
    }
    auto* thread = found->second.thread;
    auto& scope = m_impl->borrow_scope;
    const auto token = scope.begin();
    lua_settop(thread, 0);
    lua_getref(thread, callback);
    detail::push_luau_borrowed_ref(thread, *prepared, scope, token);
    if (lua_pcall(thread, 1, 1, 0) != 0) {
        auto message = luau_error(thread, "Failed to call playtest observe");
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return failure(LuauScriptError {std::move(message)});
    }
    try {
        auto result = luau_json(thread, -1, "observation").dump();
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return result;
    } catch (const std::exception& error) {
        scope.end(token);
        context.finish();
        lua_settop(thread, 0);
        return failure(LuauScriptError {error.what()});
    }
}

} // namespace ets
