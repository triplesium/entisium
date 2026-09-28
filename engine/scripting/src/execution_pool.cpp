#include "scripting/execution_pool.hpp"

#include "ecs/execution_lane.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ets {
namespace {

template<typename Load>
Result<LuauExecutionModule, LuauScriptError> load_across_lanes(
    LuauExecutionPool& pool,
    std::span<const LuauExecutionImportBinding> imports,
    Load load
) {
    for (const auto& import : imports) {
        if (import.module == nullptr) {
            return failure(
                LuauScriptError {
                    "Luau execution import '" + import.specifier +
                        "' has no module",
                }
            );
        }
        if (import.module->lane_count() != pool.lane_count()) {
            return failure(
                LuauScriptError {
                    "Luau execution import '" + import.specifier + "' has " +
                        std::to_string(import.module->lane_count()) +
                        " lanes but the pool has " +
                        std::to_string(pool.lane_count()),
                }
            );
        }
    }

    LuauExecutionModule result;
    result.lanes.reserve(pool.lane_count());
    for (std::size_t lane_index = 0; lane_index < pool.lane_count();
         ++lane_index) {
        auto runtime = pool.runtime(lane_index);
        if (!runtime) {
            return failure(std::move(runtime.error()));
        }
        std::vector<LuauScriptImportBinding> lane_imports;
        lane_imports.reserve(imports.size());
        for (const auto& import : imports) {
            lane_imports.push_back(
                LuauScriptImportBinding {
                    .specifier = import.specifier,
                    .module = import.module->lanes[lane_index],
                }
            );
        }
        auto module = load(*runtime, lane_imports);
        if (!module) {
            for (std::size_t loaded_lane = 0; loaded_lane < result.lanes.size();
                 ++loaded_lane) {
                auto loaded_runtime = pool.runtime(loaded_lane);
                if (loaded_runtime) {
                    loaded_runtime->unload_module(result.lanes[loaded_lane]);
                }
            }
            return failure(std::move(module.error()));
        }
        result.lanes.push_back(*module);
    }
    return result;
}

Result<std::size_t, LuauScriptError>
current_lane(const LuauExecutionPool& pool, const LuauExecutionModule& module) {
    const auto lane = current_system_execution_lane();
    if (!lane) {
        return failure(
            LuauScriptError {
                "Luau execution module accessed outside a system invocation",
            }
        );
    }
    if (lane->count != pool.lane_count() ||
        module.lane_count() != pool.lane_count()) {
        return failure(
            LuauScriptError {
                "Luau execution module lane count does not match the current "
                "system execution context",
            }
        );
    }
    if (lane->index >= module.lanes.size()) {
        return failure(
            LuauScriptError {"Current Luau execution lane is out of range"}
        );
    }
    return lane->index;
}

} // namespace

LuauExecutionPool::LuauExecutionPool(
    std::size_t lane_count,
    std::shared_ptr<const LuauHostConfig> config
) : m_config(std::move(config)), m_lanes(lane_count) {
    if (lane_count == 0) {
        throw std::invalid_argument(
            "Luau execution pool requires at least one lane"
        );
    }
}

Status<LuauScriptError>
LuauExecutionPool::set_lane_count(std::size_t lane_count) {
    if (lane_count == 0) {
        return failure(
            LuauScriptError {
                "Luau execution pool requires at least one lane",
            }
        );
    }
    if (lane_count == m_lanes.size()) {
        return {};
    }
    if (m_active_modules != 0) {
        return failure(
            LuauScriptError {
                "Cannot resize a Luau execution pool while modules are loaded",
            }
        );
    }
    auto lanes = std::vector<Lane>(lane_count);
    m_lanes.swap(lanes);
    return {};
}

Result<LuauRuntime&, LuauScriptError>
LuauExecutionPool::runtime(std::size_t lane_index) {
    if (lane_index >= m_lanes.size()) {
        return failure(
            LuauScriptError {
                "Luau execution lane " + std::to_string(lane_index) +
                    " is out of range for pool with " +
                    std::to_string(m_lanes.size()) + " lanes",
            }
        );
    }
    auto& lane = m_lanes[lane_index];
    if (!lane.runtime) {
        lane.runtime = std::make_unique<LuauRuntime>(m_config);
    }
    return *lane.runtime;
}

Result<LuauRuntime&, LuauScriptError> LuauExecutionPool::current_runtime() {
    const auto execution_lane = current_system_execution_lane();
    if (!execution_lane) {
        return failure(
            LuauScriptError {
                "Luau execution pool accessed outside a system invocation",
            }
        );
    }
    if (execution_lane->count != m_lanes.size()) {
        return failure(
            LuauScriptError {
                "System execution lane count " +
                    std::to_string(execution_lane->count) +
                    " does not match Luau execution pool lane count " +
                    std::to_string(m_lanes.size()),
            }
        );
    }
    return runtime(execution_lane->index);
}

Result<LuauExecutionModule, LuauScriptError> LuauExecutionPool::load_module(
    const LuauScriptModuleArtifact& artifact,
    std::span<const LuauExecutionImportBinding> imports
) {
    auto loaded = load_across_lanes(
        *this,
        imports,
        [&artifact](
            LuauRuntime& runtime,
            std::span<const LuauScriptImportBinding> lane_imports
        ) {
            return runtime.load_module(artifact, lane_imports);
        }
    );
    if (loaded) {
        ++m_active_modules;
    }
    return loaded;
}

Status<LuauScriptError>
LuauExecutionPool::unload_module(const LuauExecutionModule& module) {
    if (module.lane_count() != lane_count()) {
        return failure(
            LuauScriptError {
                "Cannot unload a Luau execution module with a mismatched lane "
                "count",
            }
        );
    }
    if (m_active_modules == 0) {
        return failure(
            LuauScriptError {"Luau execution pool has no loaded modules"}
        );
    }
    for (std::size_t lane_index = 0; lane_index < lane_count(); ++lane_index) {
        auto lane_runtime = runtime(lane_index);
        if (!lane_runtime) {
            return failure(std::move(lane_runtime.error()));
        }
        auto unloaded = lane_runtime->unload_module(module.lanes[lane_index]);
        if (!unloaded) {
            return failure(std::move(unloaded.error()));
        }
    }
    --m_active_modules;
    return {};
}

Status<LuauScriptError> LuauExecutionPool::call_module_function(
    const LuauExecutionModule& module,
    const std::string& name,
    std::span<const Ref> args
) {
    auto lane = current_lane(*this, module);
    if (!lane) {
        return failure(std::move(lane.error()));
    }
    auto lane_runtime = runtime(*lane);
    if (!lane_runtime) {
        return failure(std::move(lane_runtime.error()));
    }
    return lane_runtime->call_module_function(module.lanes[*lane], name, args);
}

Result<bool, LuauScriptError> LuauExecutionPool::call_module_condition(
    const LuauExecutionModule& module,
    const std::string& name,
    std::span<const Ref> args
) {
    auto lane = current_lane(*this, module);
    if (!lane) {
        return failure(std::move(lane.error()));
    }
    auto lane_runtime = runtime(*lane);
    if (!lane_runtime) {
        return failure(std::move(lane_runtime.error()));
    }
    return lane_runtime->call_module_condition(module.lanes[*lane], name, args);
}

} // namespace ets
