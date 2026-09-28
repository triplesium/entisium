#pragma once

#include "base/result.hpp"
#include "scripting/runtime.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ets {

struct LuauExecutionModule {
    std::vector<LuauScriptModuleId> lanes;

    [[nodiscard]] std::size_t lane_count() const { return lanes.size(); }
};

struct LuauExecutionImportBinding {
    std::string specifier;
    std::shared_ptr<const LuauExecutionModule> module;
};

class LuauExecutionPool {
  private:
    struct Lane {
        std::unique_ptr<LuauRuntime> runtime;
    };

    std::shared_ptr<const LuauHostConfig> m_config;
    std::vector<Lane> m_lanes;
    std::size_t m_active_modules {};

  public:
    explicit LuauExecutionPool(
        std::size_t lane_count = 1,
        std::shared_ptr<const LuauHostConfig> config = {}
    );

    LuauExecutionPool(const LuauExecutionPool&) = delete;
    LuauExecutionPool& operator=(const LuauExecutionPool&) = delete;
    LuauExecutionPool(LuauExecutionPool&&) noexcept = default;
    LuauExecutionPool& operator=(LuauExecutionPool&&) noexcept = default;

    Status<LuauScriptError> set_lane_count(std::size_t lane_count);

    [[nodiscard]] std::size_t lane_count() const { return m_lanes.size(); }
    [[nodiscard]] std::size_t active_module_count() const {
        return m_active_modules;
    }

    Result<LuauRuntime&, LuauScriptError> runtime(std::size_t lane_index);
    Result<LuauRuntime&, LuauScriptError> current_runtime();

    Result<LuauExecutionModule, LuauScriptError> load_module(
        const LuauScriptModuleArtifact& artifact,
        std::span<const LuauExecutionImportBinding> imports = {}
    );
    Status<LuauScriptError> unload_module(const LuauExecutionModule& module);

    Status<LuauScriptError> call_module_function(
        const LuauExecutionModule& module,
        const std::string& name,
        std::span<const Ref> args = {}
    );
    Result<bool, LuauScriptError> call_module_condition(
        const LuauExecutionModule& module,
        const std::string& name,
        std::span<const Ref> args
    );
};

} // namespace ets
