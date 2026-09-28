#pragma once

#include "base/result.hpp"
#include "scripting/error.hpp"
#include "scripting/module_decl.hpp"
#include "scripting/module_metadata.hpp"
#include "scripting/source.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ets {

using LuauModuleMetadataResolver = std::function<
    Result<std::shared_ptr<const LuauModuleMetadata>, LuauScriptError>(
        std::string_view specifier
    )>;

enum class LuauOptimizationPass : std::uint8_t {
    FlattenPropertyPaths,
    ElidePropertyAliases,
    ReuseQueryUserdata,
    ChunkQueryIteration,
    Count,
};

enum class LuauOptimizationPipeline : std::uint8_t {
    Default,
    None,
    Property,
    Query,
    All,
};

enum class LuauOptimizationDependencyKind : std::uint8_t {
    Required,
    BenefitsFrom,
};

class LuauOptimizationPasses {
  public:
    constexpr LuauOptimizationPasses() = default;

    [[nodiscard]] static constexpr LuauOptimizationPasses all() {
        return LuauOptimizationPasses {c_all};
    }

    [[nodiscard]] static constexpr LuauOptimizationPasses none() {
        return LuauOptimizationPasses {0};
    }

    [[nodiscard]] constexpr bool contains(LuauOptimizationPass pass) const {
        return (m_enabled & bit(pass)) != 0;
    }

    constexpr LuauOptimizationPasses&
    set(LuauOptimizationPass pass, bool enabled = true) {
        if (enabled) {
            m_enabled |= bit(pass);
        } else {
            m_enabled &= ~bit(pass);
        }
        return *this;
    }

    [[nodiscard]] constexpr std::uint64_t mask() const { return m_enabled; }

    [[nodiscard]] constexpr bool
    operator==(const LuauOptimizationPasses&) const = default;

  private:
    static constexpr std::uint64_t bit(LuauOptimizationPass pass) {
        return std::uint64_t {1} << static_cast<std::uint8_t>(pass);
    }

    static constexpr std::uint64_t c_all =
        (std::uint64_t {1} << static_cast<std::uint8_t>(
             LuauOptimizationPass::Count
         )) -
        1;

    explicit constexpr LuauOptimizationPasses(std::uint64_t enabled) :
        m_enabled(enabled) {}

    std::uint64_t m_enabled {c_all};
};

[[nodiscard]]
std::string_view luau_optimization_pass_name(LuauOptimizationPass pass);

struct LuauOptimizationPassDescriptor {
    LuauOptimizationPass pass {LuauOptimizationPass::Count};
    LuauOptimizationPasses required_passes = LuauOptimizationPasses::none();
    LuauOptimizationPasses benefits_from = LuauOptimizationPasses::none();
};

struct LuauOptimizationDiagnostic {
    LuauOptimizationPass pass {LuauOptimizationPass::Count};
    LuauOptimizationPass dependency {LuauOptimizationPass::Count};
    LuauOptimizationDependencyKind kind {
        LuauOptimizationDependencyKind::BenefitsFrom
    };
};

[[nodiscard]]
std::span<const LuauOptimizationPassDescriptor>
luau_optimization_pass_descriptors();

[[nodiscard]]
std::string_view
luau_optimization_pipeline_name(LuauOptimizationPipeline pipeline);

[[nodiscard]]
std::span<const LuauOptimizationPass>
luau_optimization_pipeline_order(LuauOptimizationPipeline pipeline);

[[nodiscard]]
LuauOptimizationPasses
luau_optimization_pipeline_passes(LuauOptimizationPipeline pipeline);

[[nodiscard]]
std::vector<LuauOptimizationDiagnostic>
diagnose_luau_optimization_passes(LuauOptimizationPasses passes);

struct LuauOptimizationPassReport {
    LuauOptimizationPass pass {LuauOptimizationPass::Count};
    bool enabled {false};
    std::size_t candidates {0};
    std::size_t applied {0};
};

struct LuauCompileOptions {
    // Project scripts default to snapshot-safe behavior. The opt-out exists
    // for low-level VM tests and tooling that never participates in rollback.
    bool snapshot_safe {true};
    // Tool modules may return a table or entry function instead of exports.
    bool allow_return {false};
    // Optimization passes are independently selectable for benchmarking and
    // diagnostics. Production compilation keeps every stable pass enabled.
    LuauOptimizationPasses optimization_passes = LuauOptimizationPasses::all();
    // Reuses the source-owned reflection metadata when an asset compilation
    // session has already analyzed this module.
    std::shared_ptr<const LuauModuleMetadata> metadata;
    // Resolves the reflection metadata for a relative require. Imported
    // system signatures are queried from the resolved module.
    LuauModuleMetadataResolver module_metadata_resolver;
};

struct LuauPropertyPathDecl {
    std::string atom_name;
    TypeId root_type;
    TypeId leaf_type;
    std::vector<std::string> properties;
};

struct LuauScriptModuleArtifact {
    std::shared_ptr<const LuauModuleMetadata> metadata;
    std::vector<LuauPluginDecl> plugins;
    std::vector<LuauFunctionDecl> functions;
    std::vector<LuauStateDecl> states;
    std::string bytecode;
    std::vector<TypeId> required_runtime_types;
    std::vector<LuauPropertyPathDecl> property_paths;
    std::vector<LuauOptimizationPassReport> optimization_report;
    std::vector<LuauOptimizationDiagnostic> optimization_diagnostics;

    [[nodiscard]] const LuauPluginDecl*
    find_plugin(std::string_view name) const {
        for (const auto& plugin : plugins) {
            if (plugin.name == name) {
                return &plugin;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::span<const LuauPluginDependency>
    plugin_dependencies(std::string_view plugin_name) const {
        const auto* selected = metadata->find_plugin(plugin_name);
        return selected != nullptr ?
                   std::span<const LuauPluginDependency> {
                       selected->dependencies,
                   } :
                   std::span<const LuauPluginDependency> {};
    }
};

bool is_native_luau_module(std::string_view specifier);

Result<std::vector<std::string>, LuauScriptError>
extract_luau_script_imports(const LuauScriptSource& source);

Result<LuauModuleMetadata, LuauScriptError> compile_luau_module_metadata(
    const LuauScriptSource& source,
    bool snapshot_safe = true,
    bool allow_return = false
);

Result<LuauScriptModuleArtifact, LuauScriptError> compile_luau_script_module(
    const LuauScriptSource& source,
    LuauCompileOptions options = {}
);

} // namespace ets
