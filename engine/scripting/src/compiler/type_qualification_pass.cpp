#include "ecs/dynamic/system_decl.hpp"
#include "pass.hpp"
#include "refl/annotations.hpp"
#include "refl/registry.hpp"
#include "scripting/detail/reflection_bridge.hpp"
#include "scripting/library.hpp"
#include "source_name.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace ets::detail::luau_compiler {
namespace {

using Luau::AstExprCall;
using Luau::AstExprConstantString;
using Luau::AstExprGlobal;
using Luau::AstStatLocal;

std::string_view name_view(Luau::AstName name) {
    return name.value != nullptr ? std::string_view {name.value} :
                                   std::string_view {};
}

using LuauTypeBindings = std::unordered_map<std::string, std::string>;

struct ImportedTypeBinding {
    std::string qualified;
    bool script_type {false};
    bool prefix {false};
};

using ImportedTypeBindings =
    std::unordered_map<std::string, ImportedTypeBinding>;

template<typename Qualify>
void qualify_function_types(LuauFunctionDecl& function, Qualify&& qualify) {
    for (auto& param : function.params) {
        if (param->decl_type_id() == type_id<DynamicResourceParamDecl>()) {
            qualify(static_cast<DynamicResourceParamDecl&>(*param).type);
        } else if (param->decl_type_id() == type_id<DynamicQueryParamDecl>()) {
            auto& query = static_cast<DynamicQueryParamDecl&>(*param);
            for (auto& field : query.fields) {
                qualify(field.type);
            }
            const auto qualify_filter =
                [&](const auto& recurse,
                    DynamicQueryFilterDecl& filter) -> void {
                qualify(filter.type);
                for (auto& child : filter.filters) {
                    recurse(recurse, child);
                }
            };
            for (auto& filter : query.filters) {
                qualify_filter(qualify_filter, filter);
            }
        } else if (param->decl_type_id() == type_id<DynamicStateParamDecl>()) {
            qualify(static_cast<DynamicStateParamDecl&>(*param).type);
        } else if (
            param->decl_type_id() == type_id<DynamicNextStateParamDecl>()
        ) {
            qualify(static_cast<DynamicNextStateParamDecl&>(*param).type);
        } else if (
            param->decl_type_id() ==
            type_id<DynamicRemovedComponentsParamDecl>()
        ) {
            qualify(
                static_cast<DynamicRemovedComponentsParamDecl&>(*param).type
            );
        } else if (param->decl_type_id() == type_id<DynamicEventParamDecl>()) {
            qualify(static_cast<DynamicEventParamDecl&>(*param).type);
        }
    }
}

LuauTypeBindings script_type_bindings(
    const LuauModuleSchema& schema,
    std::span<const LuauStateDecl> states
) {
    LuauTypeBindings result;
    for (const auto& type : schema.types) {
        result.emplace(type.name, type.qualified_name);
    }
    for (const auto& enumeration : schema.enums) {
        result.emplace(enumeration.name, enumeration.qualified_name);
    }
    for (const auto& state : states) {
        result.emplace(state.name, state.qualified_name);
    }
    return result;
}

std::unordered_map<std::string, std::string>
collect_imports(const Luau::AstStatBlock& root) {
    std::unordered_map<std::string, std::string> result;
    for (const Luau::AstStat* statement : root.body) {
        const auto* local = statement->as<AstStatLocal>();
        if (local == nullptr) {
            continue;
        }
        const auto count = std::min(local->vars.size, local->values.size);
        for (std::size_t index = 0; index < count; ++index) {
            const auto* call = local->values.data[index]->as<AstExprCall>();
            const auto* callee =
                call != nullptr ? call->func->as<AstExprGlobal>() : nullptr;
            const auto* specifier =
                call != nullptr && call->args.size == 1 ?
                    call->args.data[0]->as<AstExprConstantString>() :
                    nullptr;
            if (callee == nullptr || name_view(callee->name) != "require" ||
                specifier == nullptr) {
                continue;
            }
            result.emplace(
                name_view(local->vars.data[index]->name),
                std::string {specifier->value.data, specifier->value.size}
            );
        }
    }
    return result;
}

ImportedTypeBindings imported_type_bindings(
    const LuauScriptSource& source,
    const Luau::AstStatBlock& root
) {
    const auto delimiter = source.name.find("://");
    const std::string source_prefix = delimiter == std::string::npos ?
                                          std::string {} :
                                          source.name.substr(0, delimiter + 3);
    const std::string source_path = delimiter == std::string::npos ?
                                        source.name :
                                        source.name.substr(delimiter + 3);
    ImportedTypeBindings result;
    for (const auto& [local_name, specifier] : collect_imports(root)) {
        if (is_luau_library(specifier)) {
            continue;
        }
        constexpr std::string_view native_prefix = "@entisium/";
        if (specifier.starts_with(native_prefix)) {
            const std::string_view module_name =
                std::string_view {specifier}.substr(native_prefix.size());
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
                result.emplace(
                    local_name + "::" + std::string(type->local_name()),
                    ImportedTypeBinding {.qualified = type->name()}
                );
            }
            continue;
        }
        auto dependency =
            (std::filesystem::path(source_path).parent_path() / specifier)
                .lexically_normal();
        if (dependency.extension().empty()) {
            dependency += ".luau";
        }
        result.emplace(
            local_name + "::",
            ImportedTypeBinding {
                .qualified = module_name_from_source(
                                 source_prefix + dependency.generic_string()
                             ) +
                             ".",
                .script_type = true,
                .prefix = true,
            }
        );
    }
    return result;
}

void qualify_script_type_ref(
    DynamicTypeRef& type,
    const LuauTypeBindings& script_types
) {
    if (const auto found = script_types.find(type.type_name);
        found != script_types.end()) {
        type.type_name = found->second;
    }
}

void qualify_imported_type_ref(
    DynamicTypeRef& type,
    const ImportedTypeBindings& imported_types
) {
    if (const auto exact = imported_types.find(type.type_name);
        exact != imported_types.end() && !exact->second.prefix) {
        type.type_name = exact->second.qualified;
        return;
    }
    for (const auto& [prefix, binding] : imported_types) {
        if (binding.prefix && type.type_name.starts_with(prefix)) {
            type.type_name =
                binding.qualified + type.type_name.substr(prefix.size());
            return;
        }
    }
}

void qualify_imported_field_type(
    LuauTypeRef& type,
    const ImportedTypeBindings& imported_types
) {
    if (const auto exact = imported_types.find(type.type_name);
        exact != imported_types.end() && !exact->second.prefix) {
        type.type_name = exact->second.qualified;
        type.script_type = exact->second.script_type;
        return;
    }
    for (const auto& [prefix, binding] : imported_types) {
        if (binding.prefix && type.type_name.starts_with(prefix)) {
            type.type_name =
                binding.qualified + type.type_name.substr(prefix.size());
            type.script_type = binding.script_type;
            return;
        }
    }
}

} // namespace

void TypeQualificationPass::run(
    const LuauScriptSource& source,
    const Luau::AstStatBlock& root,
    LuauModuleSchema& schema,
    std::vector<LuauFunctionDecl>& functions,
    std::span<const LuauStateDecl> states
) const {
    const auto script_types = script_type_bindings(schema, states);
    const auto imported_types = imported_type_bindings(source, root);
    for (auto& type : schema.types) {
        for (auto& field : type.fields) {
            qualify_imported_field_type(field.type, imported_types);
        }
    }
    for (auto& function : functions) {
        qualify_function_types(function, [&](DynamicTypeRef& type) {
            qualify_script_type_ref(type, script_types);
            qualify_imported_type_ref(type, imported_types);
        });
    }
}

void TypeQualificationPass::run(
    const LuauScriptSource& source,
    const Luau::AstStatBlock& root,
    LuauModuleSchema& schema,
    LuauPluginDecl& plugin
) const {
    run(source, root, schema, plugin.functions, plugin.states);
}

} // namespace ets::detail::luau_compiler
