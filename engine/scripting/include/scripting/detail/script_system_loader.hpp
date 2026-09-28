#pragma once

#include "base/result.hpp"
#include "ecs/fwd.hpp"
#include "scripting/error.hpp"
#include "scripting/execution_pool.hpp"
#include "scripting/module_decl.hpp"
#include "scripting/runtime.hpp"
#include "scripting/schema.hpp"

#include <memory>
#include <vector>

namespace ets {

class World;

namespace detail {

Status<LuauScriptError> prepare_luau_module(
    LuauRuntime& runtime,
    LuauScriptModuleId module,
    const LuauModuleSchema& schema
);

Status<LuauScriptError> prepare_luau_script_system_module(
    LuauRuntime& runtime,
    LuauScriptModuleId module,
    const LuauModuleSchema& schema,
    const LuauPluginDecl& plugin
);

Result<std::vector<SystemHandle>, LuauScriptError> install_luau_script_systems(
    World& world,
    LuauRuntime& runtime,
    LuauScriptModuleId module,
    const LuauModuleSchema& schema,
    const LuauPluginDecl& plugin
);

Result<std::vector<SystemHandle>, LuauScriptError>
install_luau_script_system_functions(
    World& world,
    LuauExecutionPool& execution_pool,
    std::shared_ptr<const LuauExecutionModule> module,
    std::string_view source_name,
    const std::vector<DynamicSystemDecl>& systems
);

} // namespace detail
} // namespace ets
