#pragma once

#include "scripting/runtime.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace ets {

// Sources have canonical names. Resolution belongs to the embedding host.
using LuauSourceResolver =
    std::function<Result<LuauScriptSource, LuauScriptError>(
        std::string_view importer,
        std::string_view specifier
    )>;

// Owns module instances in one VM, with no World or Plugin installation.
class LuauModuleLoader {
  public:
    LuauModuleLoader(LuauRuntime& runtime, LuauSourceResolver resolver);
    Result<LuauScriptModuleId, LuauScriptError>
    load(std::string_view importer, std::string_view specifier);

  private:
    struct Module {
        LuauScriptModuleId id;
        std::shared_ptr<const LuauModuleMetadata> metadata;
    };
    LuauRuntime& m_runtime;
    LuauSourceResolver m_resolver;
    std::unordered_map<std::string, Module> m_modules;
    std::unordered_set<std::string> m_loading;
};
} // namespace ets
