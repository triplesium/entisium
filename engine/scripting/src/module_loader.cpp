#include "scripting/module_loader.hpp"

#include "scripting/detail/script_system_loader.hpp"

namespace ets {
LuauModuleLoader::LuauModuleLoader(
    LuauRuntime& runtime,
    LuauSourceResolver resolver
) : m_runtime(runtime), m_resolver(std::move(resolver)) {}

Result<LuauScriptModuleId, LuauScriptError>
LuauModuleLoader::load(std::string_view importer, std::string_view specifier) {
    auto source = m_resolver(importer, specifier);
    if (!source) {
        return failure(std::move(source.error()));
    }
    if (auto cached = m_modules.find(source->name); cached != m_modules.end()) {
        return cached->second.id;
    }
    if (!m_loading.insert(source->name).second) {
        return failure(
            LuauScriptError {"Circular Luau module dependency: " + source->name}
        );
    }
    struct LoadingGuard {
        std::unordered_set<std::string>& loading;
        std::string name;
        ~LoadingGuard() { loading.erase(name); }
    } guard {m_loading, source->name};
    auto imports = extract_luau_script_imports(*source);
    if (!imports) {
        return failure(std::move(imports.error()));
    }
    std::vector<LuauScriptImportBinding> bindings;
    std::unordered_map<std::string, std::shared_ptr<const LuauModuleMetadata>>
        metadata;
    for (const auto& path : *imports) {
        if (is_native_luau_module(path)) {
            continue;
        }
        auto dependency = load(source->name, path);
        if (!dependency) {
            return failure(std::move(dependency.error()));
        }
        bindings.push_back({path, *dependency});
        auto resolved = m_resolver(source->name, path);
        if (!resolved) {
            return failure(std::move(resolved.error()));
        }
        metadata.emplace(path, m_modules.at(resolved->name).metadata);
    }
    auto schema = compile_luau_module_metadata(*source, false, true);
    if (!schema) {
        return failure(std::move(schema.error()));
    }
    auto shared =
        std::make_shared<const LuauModuleMetadata>(std::move(*schema));
    auto artifact = compile_luau_script_module(
        *source,
        LuauCompileOptions {
            .snapshot_safe = false,
            .allow_return = true,
            .metadata = shared,
            .module_metadata_resolver = [&metadata](std::string_view path)
                -> Result<
                    std::shared_ptr<const LuauModuleMetadata>,
                    LuauScriptError> {
                const auto found = metadata.find(std::string(path));
                if (found == metadata.end()) {
                    return failure(
                        LuauScriptError {
                            "Unresolved module: " + std::string(path)
                        }
                    );
                }
                return found->second;
            },
        }
    );
    if (!artifact) {
        return failure(std::move(artifact.error()));
    }
    auto module = m_runtime.load_module(*artifact, bindings);
    if (!module) {
        return failure(std::move(module.error()));
    }
    if (source->runtime_types) {
        auto prepared =
            detail::prepare_luau_module(m_runtime, *module, shared->schema);
        if (!prepared) {
            (void)m_runtime.unload_module(*module);
            return failure(std::move(prepared.error()));
        }
    }
    m_modules.emplace(source->name, Module {*module, shared});
    return *module;
}
} // namespace ets
